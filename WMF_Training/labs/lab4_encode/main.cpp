// Lab 4 — H.264 硬體編碼
//
//   dda_encode.exe [--output N] [--width 1280] [--height 720] [--fps 30] [--seconds 5]
//                  [--bitrate 8000000] [--gop 60] [--out output.h264] [--software]
//
// 學習重點：
//   1. MFTEnumEx 列舉硬體 encoder，沒有就 fallback 到軟體，印出實際用的是哪一個
//   2. MFT 資料流：協商 MediaType -> START_OF_STREAM -> 送/取 -> DRAIN
//   3. 時間戳：100ns 單位，i * 10,000,000 / fps（先乘後除，不累積誤差）
//   4. 恆定幀率：DDA 畫面沒變時不給 frame，要重送上一張，才會剛好 fps*seconds 張
//   5. 送進 async encoder 的 texture 要用 pool，不能每幀覆寫同一張
#include <atomic>
#include <cstdio>
#include <mutex>

#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "h264_encoder.h"
#include "hr.h"
#include "mf_util.h"
#include "nv12_pool.h"
#include "timer.h"
#include "video_scaler.h"

static int Run(const Args& args)
{
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    EncoderConfig ec;
    ec.width = (UINT)args.GetInt("--width", 1280) & ~1u;
    ec.height = (UINT)args.GetInt("--height", 720) & ~1u;
    ec.fps = (UINT)args.GetInt("--fps", 30);
    ec.bitrate = (UINT)args.GetInt("--bitrate", 8'000'000);
    ec.gopSize = (UINT)args.GetInt("--gop", ec.fps * 2);
    ec.lowLatency = true;
    ec.preferHardware = !args.Has("--software");
    const UINT seconds = (UINT)args.GetInt("--seconds", 5);
    const UINT totalFrames = ec.fps * seconds;
    const std::string outPath = args.Get("--out", "output.h264");

    D3DContext d3d = CreateD3DForOutput(outputIndex);
    ComPtr<IMFDXGIDeviceManager> devMgr = CreateDxgiDeviceManager(d3d.device.Get());

    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        return 2;
    }
    // 「最近一張」桌面畫面。DDA 沒給新畫面時就重送它（恆定幀率）。
    ComPtr<ID3D11Texture2D> latest = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    ScalerConfig sc;
    sc.srcWidth = capture.Width();
    sc.srcHeight = capture.Height();
    sc.srcFormat = capture.Format();
    sc.dstWidth = ec.width;
    sc.dstHeight = ec.height;
    sc.dstFormat = DXGI_FORMAT_NV12; // 縮放 + BGRA->NV12 一次 Blt
    VideoScaler scaler;
    scaler.Init(d3d.device.Get(), sc);

    Nv12SamplePool pool;
    pool.Init(d3d.device.Get(), ec.width, ec.height, 8);

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "Cannot open %s\n", outPath.c_str());
        return 3;
    }
    std::mutex outMu; // async encoder 會在 MF 的 thread 上呼叫 callback
    std::atomic<int> framesOut{ 0 }, keyframes{ 0 };
    std::atomic<int64_t> bytesOut{ 0 };

    H264Encoder encoder;
    encoder.Init(ec, devMgr.Get(), [&](EncodedFrame&& f) {
        std::lock_guard<std::mutex> lk(outMu);
        std::fwrite(f.data.data(), 1, f.data.size(), out); // Annex B 直接串接就是合法的 .h264 檔
        framesOut++;
        keyframes += f.keyframe;
        bytesOut += (int64_t)f.data.size();
    });
    std::printf("Encoder : %s\n          hardware=%s async=%s d3d11-input=%s\n", encoder.Name().c_str(),
                encoder.IsHardware() ? "yes" : "NO (software fallback)", encoder.IsAsync() ? "yes" : "no",
                encoder.UsesD3D() ? "yes" : "no");
    if (!encoder.UsesD3D())
        std::printf("          NOTE: encoder can't read GPU textures -> each frame is read back to CPU (slow path)\n");
    std::printf("Encoding %u frames (%u s @ %u fps) at %ux%u, %u bps -> %s\n", totalFrames, seconds, ec.fps, ec.width,
                ec.height, ec.bitrate, outPath.c_str());

    // 先拿到第一張畫面
    CaptureStatus s = CaptureStatus::NoChange;
    for (int i = 0; i < 20 && s != CaptureStatus::NewFrame; ++i) s = capture.Acquire(500, latest.Get());
    if (s != CaptureStatus::NewFrame) {
        std::fprintf(stderr, "No desktop frame captured\n");
        return 4;
    }

    const int64_t period = QpcFrequency() / ec.fps;
    const int64_t start = QpcNow();
    int newFrames = 1, repeatedFrames = 0;
    Stat submitMs;

    for (UINT i = 0; i < totalFrames; ++i) {
        // ---- 等到這一幀的時間點；期間有新畫面就更新 latest ----
        const int64_t due = start + (int64_t)i * period;
        bool gotNew = (i == 0);
        for (;;) {
            int64_t remain = due - QpcNow();
            UINT timeout = remain > 0 ? (UINT)QpcToMs(remain) : 0;
            CaptureStatus cs = capture.Acquire(timeout, latest.Get());
            if (cs == CaptureStatus::NewFrame) gotNew = true;
            if (cs == CaptureStatus::AccessLost) {
                std::fprintf(stderr, "  ACCESS_LOST at frame %u, re-creating duplication\n", i);
                capture.Reinit(); // 失敗（例如鎖屏中）就繼續重送最後一張，下一輪再試
            }
            if (QpcNow() >= due) break;
        }
        gotNew ? ++newFrames : ++repeatedFrames;

        // ---- 縮放 + 轉 NV12 到 pool 中一張 encoder 沒在用的 texture ----
        Nv12SamplePool::Slot* slot = pool.Acquire();
        if (!slot) throw std::runtime_error("all NV12 textures are still held by the encoder");
        scaler.Process(latest.Get(), 0, slot->texture.Get());

        ComPtr<IMFSample> sample = slot->sample;
        if (!encoder.UsesD3D())
            sample = MakeCpuNv12Sample(d3d.device.Get(), d3d.context.Get(), slot->texture.Get(), ec.width, ec.height);

        // ---- 時間戳：100ns 單位，先乘後除 ----
        CHECK_HR(sample->SetSampleTime(MFllMulDiv(i, 10'000'000, ec.fps, 0)));
        CHECK_HR(sample->SetSampleDuration(MFllMulDiv(1, 10'000'000, ec.fps, 0)));

        int64_t t0 = QpcNow();
        encoder.Encode(sample.Get());
        submitMs.Add(QpcToMs(QpcNow() - t0));
        if ((i + 1) % ec.fps == 0) std::printf("  %u / %u frames submitted, %d encoded\n", i + 1, totalFrames, framesOut.load());
    }

    encoder.Drain(); // 不 drain 會少最後幾張
    encoder.Shutdown();
    std::fclose(out);

    const double actualBps = bytesOut * 8.0 / seconds;
    std::printf("\nDone.\n");
    std::printf("  frames in   : %u  (new desktop images %d, repeated %d)\n", totalFrames, newFrames, repeatedFrames);
    std::printf("  frames out  : %d  (keyframes %d)%s\n", framesOut.load(), keyframes.load(),
                framesOut.load() == (int)totalFrames ? "" : "   <-- should equal frames in!");
    std::printf("  file size   : %lld bytes, average %.2f Mbps (target %.2f Mbps)\n", (long long)bytesOut.load(),
                actualBps / 1e6, ec.bitrate / 1e6);
    std::printf("  Encode() call time: avg %.2f ms, max %.2f ms\n", submitMs.Avg(), submitMs.max);
    std::printf("\nVerify with:\n  ffprobe -v error -count_frames -select_streams v:0 -show_entries "
                "stream=codec_name,profile,width,height,nb_read_frames,has_b_frames -of default=nw=1 %s\n"
                "  ffplay -framerate %u %s     (raw .h264 has no timestamps; ffmpeg assumes 25 fps otherwise)\n",
                outPath.c_str(), ec.fps, outPath.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    SetupConsole();
    EnablePerMonitorDpiAwareness();
    try {
        MfScope mf;
        return Run(Args(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
