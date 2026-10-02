// Lab 5 — H.264 硬體解碼
//
//   h264_decode.exe output.h264 [--outdir .] [--max 0] [--no-save] [--play] [--fps 30] [--software]
//
// 學習重點：
//   1. 自己切 Annex B：start code -> NAL -> access unit（NAL 數 != 畫面數）
//   2. MF_E_TRANSFORM_STREAM_CHANGE：看到 SPS 後重新協商輸出 type
//   3. 輸出是 texture array + subresource index；1080p 實際高度 1088，要依 display aperture 裁切
//   4. 用完輸出 sample 立刻 Release；最後 drain，張數要和編碼張數「相等」
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

#include "annexb.h"
#include "bmp.h"
#include "cli.h"
#include "d3d_util.h"
#include "h264_decoder.h"
#include "hr.h"
#include "mf_util.h"
#include "render_window.h"
#include "timer.h"

static std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return data;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

static int Run(Args& args)
{
    args.DeclareFlags({ "--no-save", "--play", "--software" });
    const std::string inPath = args.Positional(0, "output.h264");
    const std::string outDir = args.Get("--outdir", ".");
    const int maxSave = (int)args.GetInt("--max", 0); // 0 = 全部存
    const bool save = !args.Has("--no-save");
    const bool play = args.Has("--play");
    const UINT fps = (UINT)args.GetInt("--fps", 30);

    std::vector<uint8_t> file = ReadFile(inPath);
    if (file.empty()) {
        std::fprintf(stderr, "Cannot read %s\n", inPath.c_str());
        return 2;
    }

    // ---- 1. 切 NAL / access unit，先印出統計，讓學員看到「NAL 數 != 畫面數」 ----
    std::vector<NalUnit> nals = SplitNalUnits(file.data(), file.size());
    std::vector<AccessUnit> aus = GroupAccessUnits(file.data(), file.size());
    std::map<int, int> byType;
    for (const auto& n : nals) byType[n.type]++;
    int keyAus = 0;
    for (const auto& a : aus) keyAus += a.keyframe;
    std::printf("%s: %zu bytes, %zu NAL units, %zu access units (pictures), %d keyframes\n", inPath.c_str(),
                file.size(), nals.size(), aus.size(), keyAus);
    std::printf("  NAL types:");
    for (auto& kv : byType) std::printf(" [type %d] x%d", kv.first, kv.second);
    std::printf("   (1=slice 5=IDR 6=SEI 7=SPS 8=PPS 9=AUD)\n");

    // ---- 2. decoder ----
    // --software 且不播放時完全不需要 GPU（純 CPU 解碼 + 存檔）
    const bool software = args.Has("--software");
    D3DContext d3d;
    ComPtr<IMFDXGIDeviceManager> devMgr;
    if (!software || play) {
        d3d = CreateD3DDefault();
        devMgr = CreateDxgiDeviceManager(d3d.device.Get());
    }
    H264Decoder decoder;
    // 播放檔案不需要低延遲；low latency 模式遇到有 B-frame 的檔案（例如 ffmpeg 產生的）可能輸出順序不對
    decoder.Init(software ? nullptr : devMgr.Get(), /*lowLatency*/ false);
    std::printf("Decoder: Microsoft H264 Decoder MFT, D3D11/DXVA=%s (adapter: %s)\n", decoder.UsesD3D() ? "yes" : "NO (software)",
                d3d.device ? Narrow(d3d.adapterName).c_str() : "none");

    std::unique_ptr<RenderWindow> window;
    if (play) {
        window.reset(new RenderWindow());
        window->Create(L"h264_decode --play (ESC to quit)", 1280, 720, d3d.device.Get());
    }

    int decoded = 0;
    bool quit = false;
    const int64_t framePeriod = QpcFrequency() / fps;
    int64_t nextPresent = 0;

    auto onFrame = [&](const DecodedFrame& f) {
        ++decoded;
        if (decoded == 1 && f.codedHeight != f.displayHeight)
            std::printf("  note: coded height %u > display height %u -> cropping the %u alignment rows\n", f.codedHeight,
                        f.displayHeight, f.codedHeight - f.displayHeight);
        if (save && (maxSave == 0 || decoded <= maxSave)) {
            char name[64];
            std::snprintf(name, sizeof(name), "frame_%04d.bmp", decoded);
            std::string path = outDir + "/" + name;
            if (f.texture) {
                // decoder texture -> staging -> CPU（存檔用的唯一一次 readback）
                Nv12Image img = ReadbackNv12(d3d.device.Get(), d3d.context.Get(), f.texture, f.subresource);
                SaveNv12AsBmp(img, f.displayWidth, f.displayHeight, YuvMatrix::BT709, YuvRange::Studio, path);
            } else {
                std::vector<uint8_t> bgra((size_t)f.displayWidth * f.displayHeight * 4);
                Nv12ToBgra(f.cpuData, f.cpuData + (size_t)f.cpuPitch * f.codedHeight, f.cpuPitch, f.displayWidth,
                           f.displayHeight, YuvMatrix::BT709, YuvRange::Studio, bgra.data());
                SaveBmp32(path, bgra.data(), f.displayWidth, f.displayHeight);
            }
        }
        if (window) {
            if (!window->PumpMessages()) quit = true;
            while (QpcNow() < nextPresent) Sleep(1); // 依 fps 播放
            window->Present(f, /*vsync*/ true);
            nextPresent = (nextPresent ? nextPresent : QpcNow()) + framePeriod;
        }
    };

    // ---- 3. 一個 access unit 一個 sample；raw .h264 沒有時間戳，自己給遞增的 ----
    int64_t t0 = QpcNow();
    for (size_t i = 0; i < aus.size() && !quit; ++i) {
        const AccessUnit& au = aus[i];
        decoder.Decode(file.data() + au.begin, au.end - au.begin, MFllMulDiv((LONGLONG)i, 10'000'000, fps, 0), onFrame);
    }
    if (!quit) decoder.Drain(onFrame);
    double ms = QpcToMs(QpcNow() - t0);

    std::printf("\nDecoded %d frames from %zu access units%s\n", decoded, aus.size(),
                decoded == (int)aus.size() ? "" : "   <-- should be equal (missing drain?)");
    std::printf("  stream changes: %d (first one = SPS seen)\n", decoder.StreamChanges());
    if (!play) std::printf("  total time %.1f ms (%.2f ms/frame incl. BMP saving)\n", ms, decoded ? ms / decoded : 0.0);
    if (save) std::printf("  saved frame_0001.bmp ... in %s\n", outDir.c_str());
    return decoded == (int)aus.size() ? 0 : 5;
}

int main(int argc, char** argv)
{
    SetupConsole();
    EnablePerMonitorDpiAwareness();
    try {
        MfScope mf;
        Args args(argc, argv);
        return Run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
