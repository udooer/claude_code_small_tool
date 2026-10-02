// Lab 2 — GPU 縮放
//
//   dda_scale.exe [--output N] [--width 1280] [--height 720] [--mode letterbox|stretch] [--out capture_scaled.bmp]
//
// 學習重點：
//   1. ID3D11VideoProcessor：Enumerator -> Processor -> InputView/OutputView -> Blt
//   2. 長寬比策略：source rect / dest rect / 背景色
//   3. 正確量測 GPU 時間：D3D11 呼叫是非同步的，只量呼叫本身會被騙
#include <cstdio>
#include <cstring>

#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "hr.h"
#include "mf_util.h"
#include "timer.h"
#include "video_scaler.h"

static int Run(const Args& args)
{
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    const UINT dstW = (UINT)args.GetInt("--width", 1280) & ~1u; // 偶數：為 Lab 3 的 NV12 鋪路
    const UINT dstH = (UINT)args.GetInt("--height", 720) & ~1u;
    const ScaleMode mode = args.Get("--mode", "letterbox") == "stretch" ? ScaleMode::Stretch : ScaleMode::Letterbox;
    const std::string outPath = args.Get("--out", "capture_scaled.bmp");

    D3DContext d3d = CreateD3DForOutput(outputIndex);
    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        return 2;
    }

    ComPtr<ID3D11Texture2D> frame = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    // Video processor 的輸出 texture 必須有 BIND_RENDER_TARGET
    ComPtr<ID3D11Texture2D> scaled = CreateTexture(d3d.device.Get(), dstW, dstH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                   D3D11_BIND_RENDER_TARGET);

    ScalerConfig cfg;
    cfg.srcWidth = capture.Width();
    cfg.srcHeight = capture.Height();
    cfg.srcFormat = capture.Format();
    cfg.dstWidth = dstW;
    cfg.dstHeight = dstH;
    cfg.dstFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    cfg.mode = mode;
    VideoScaler scaler;
    scaler.Init(d3d.device.Get(), cfg);
    RECT dr = scaler.DestRect();
    std::printf("Scale %ux%u -> %ux%u, mode=%s, content rect=(%ld,%ld)-(%ld,%ld)\n", cfg.srcWidth, cfg.srcHeight, dstW,
                dstH, mode == ScaleMode::Letterbox ? "letterbox" : "stretch", dr.left, dr.top, dr.right, dr.bottom);

    CaptureStatus s = CaptureStatus::NoChange;
    for (int i = 0; i < 20 && s != CaptureStatus::NewFrame; ++i) {
        s = capture.Acquire(500, frame.Get());
        if (s == CaptureStatus::AccessLost && FAILED(capture.Reinit())) break;
    }
    if (s != CaptureStatus::NewFrame) {
        std::fprintf(stderr, "No desktop frame captured\n");
        return 4;
    }
    WaitForGpu(d3d.device.Get(), d3d.context.Get()); // 先讓 capture 的 Copy 做完，別算進縮放時間

    // 量測：同一個 Blt 跑幾次，分別看「CPU 提交時間」與「GPU 完成時間」
    for (int run = 0; run < 5; ++run) {
        int64_t t0 = QpcNow();
        scaler.Process(frame.Get(), 0, scaled.Get());
        int64_t t1 = QpcNow();
        WaitForGpu(d3d.device.Get(), d3d.context.Get());
        int64_t t2 = QpcNow();
        std::printf("  run %d: submit %.3f ms | submit + GPU done %.3f ms%s\n", run, QpcToMs(t1 - t0), QpcToMs(t2 - t0),
                    run == 0 ? "  (first run includes view creation)" : "");
    }
    std::printf("Note: 'submit' only measures queuing the command. The real cost is 'submit + GPU done'.\n");

    // 從擷取到縮放完成都在 GPU；只有存檔這一步 Map 到 CPU 一次
    if (!SaveBgraTextureAsBmp(d3d.device.Get(), d3d.context.Get(), scaled.Get(), outPath)) return 3;
    std::printf("Saved %s\n", outPath.c_str());
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
