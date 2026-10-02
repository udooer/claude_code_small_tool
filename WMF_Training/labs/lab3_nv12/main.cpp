// Lab 3 — 色彩格式轉換（BGRA -> NV12）驗證
//
//   dda_nv12.exe [--output N] [--width 1280] [--height 720] [--patches]
//                [--vp-matrix 709|601] [--vp-range studio|full]          GPU 端 (VideoProcessor) 的設定
//                [--decode-matrix 709|601] [--decode-range studio|full]  CPU 端手刻反轉換用的假設
//
// 產出：
//   capture_bgra.bmp     GPU 直接輸出 BGRA（對照組，等於 Lab 2）
//   capture_nv12.bmp     GPU 輸出 NV12 -> 手刻 YUV->RGB（實驗組）
//
// --patches：在螢幕上蓋一個純色色塊視窗再擷取，印出每個色塊的 Y/U/V 實測值與理論值。
// 故意讓 --decode-* 和 --vp-* 不一致，就能親眼看到「設錯會怎樣」。
#include <cstdio>
#include <cstdlib>
#include <memory>

#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "hr.h"
#include "mf_util.h"
#include "pattern_window.h"
#include "video_scaler.h"

static YuvMatrix ParseMatrix(const std::string& s) { return s == "601" ? YuvMatrix::BT601 : YuvMatrix::BT709; }
static YuvRange ParseRange(const std::string& s) { return s == "full" ? YuvRange::Full : YuvRange::Studio; }

static int Run(const Args& args)
{
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    const UINT dstW = (UINT)args.GetInt("--width", 1280) & ~1u; // NV12 4:2:0 需要偶數寬高
    const UINT dstH = (UINT)args.GetInt("--height", 720) & ~1u;
    const YuvMatrix vpMatrix = ParseMatrix(args.Get("--vp-matrix", "709"));
    const YuvRange vpRange = ParseRange(args.Get("--vp-range", "studio"));
    const YuvMatrix decMatrix = ParseMatrix(args.Get("--decode-matrix", args.Get("--vp-matrix", "709")));
    const YuvRange decRange = ParseRange(args.Get("--decode-range", args.Get("--vp-range", "studio")));
    const bool patches = args.Has("--patches");

    std::printf("GPU  (VideoProcessor) writes NV12 as : %s %s\n", ToString(vpMatrix), ToString(vpRange));
    std::printf("CPU  (SaveNV12AsBmp) assumes         : %s %s%s\n", ToString(decMatrix), ToString(decRange),
                (vpMatrix != decMatrix || vpRange != decRange) ? "   <-- MISMATCH (on purpose?)" : "");

    D3DContext d3d = CreateD3DForOutput(outputIndex);
    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        return 2;
    }

    ComPtr<ID3D11Texture2D> frame = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11Texture2D> bgraOut = CreateTexture(d3d.device.Get(), dstW, dstH, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET);
    ComPtr<ID3D11Texture2D> nv12Out = CreateTexture(d3d.device.Get(), dstW, dstH, DXGI_FORMAT_NV12, D3D11_BIND_RENDER_TARGET);

    ScalerConfig cfg;
    cfg.srcWidth = capture.Width();
    cfg.srcHeight = capture.Height();
    cfg.srcFormat = capture.Format();
    cfg.dstWidth = dstW;
    cfg.dstHeight = dstH;
    cfg.yuvMatrix = vpMatrix;
    cfg.yuvRange = vpRange;
    VideoScaler toBgra, toNv12;
    cfg.dstFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    toBgra.Init(d3d.device.Get(), cfg);
    cfg.dstFormat = DXGI_FORMAT_NV12;
    toNv12.Init(d3d.device.Get(), cfg);

    std::unique_ptr<PatternWindow> pattern;
    if (patches) {
        pattern.reset(new PatternWindow(d3d.outputDesc.DesktopCoordinates));
        pattern->PumpFor(800); // 讓 DWM 把色塊合成到桌面
    }

    CaptureStatus s = CaptureStatus::NoChange;
    for (int i = 0; i < 20 && s != CaptureStatus::NewFrame; ++i) {
        s = capture.Acquire(500, frame.Get());
        if (s == CaptureStatus::AccessLost && FAILED(capture.Reinit())) break;
        if (pattern) pattern->PumpFor(10);
    }
    if (s != CaptureStatus::NewFrame) {
        std::fprintf(stderr, "No desktop frame captured\n");
        return 4;
    }

    // 縮放 + 色彩轉換：一次 VideoProcessorBlt
    toBgra.Process(frame.Get(), 0, bgraOut.Get());
    toNv12.Process(frame.Get(), 0, nv12Out.Get());

    SaveBgraTextureAsBmp(d3d.device.Get(), d3d.context.Get(), bgraOut.Get(), "capture_bgra.bmp");
    Nv12Image img = ReadbackNv12(d3d.device.Get(), d3d.context.Get(), nv12Out.Get(), 0);
    std::printf("NV12 readback: %ux%u, RowPitch=%u (width=%u)  -> UV plane offset = RowPitch*Height = %u bytes\n",
                img.codedWidth, img.codedHeight, img.pitch, dstW, img.pitch * img.codedHeight);
    SaveNv12AsBmp(img, dstW, dstH, decMatrix, decRange, "capture_nv12.bmp");
    std::printf("Saved capture_bgra.bmp (GPU BGRA) and capture_nv12.bmp (GPU NV12 + hand-written YUV->RGB)\n");

    if (!pattern) return 0;

    // 色塊量測：在 NV12 輸出中找到每個色塊中心（考慮 letterbox 的位置）
    RECT dr = toNv12.DestRect();
    int failures = 0;
    std::printf("\n%-6s | %-15s | %-15s | %-16s | %s\n", "patch", "YUV measured", "YUV expected", "RGB after decode", "result");
    std::printf("-------+-----------------+-----------------+------------------+-------\n");
    for (size_t i = 0; i < TestPatches().size(); ++i) {
        const Patch& p = TestPatches()[i];
        POINT c = pattern->PatchCenter(i);
        UINT x = (UINT)(dr.left + (LONG)((int64_t)c.x * (dr.right - dr.left) / (LONG)capture.Width()));
        UINT y = (UINT)(dr.top + (LONG)((int64_t)c.y * (dr.bottom - dr.top) / (LONG)capture.Height()));
        Yuv got = SampleNv12(img.YPlane(), img.UVPlane(), img.pitch, x, y);
        Yuv exp = RgbToYuv(p.color, vpMatrix, vpRange);
        Rgb back = YuvToRgb(got, decMatrix, decRange);
        bool yuvOk = std::abs(got.y - exp.y) <= 4 && std::abs(got.u - exp.u) <= 4 && std::abs(got.v - exp.v) <= 4;
        bool rgbOk = std::abs(back.r - p.color.r) <= 6 && std::abs(back.g - p.color.g) <= 6 && std::abs(back.b - p.color.b) <= 6;
        failures += !(yuvOk && rgbOk);
        std::printf("%-6s | %3d %3d %3d     | %3d %3d %3d     | %3d %3d %3d      | %s%s\n", p.name, got.y, got.u, got.v,
                    exp.y, exp.u, exp.v, back.r, back.g, back.b, yuvOk ? "" : "GPU-YUV-OFF ", rgbOk ? "OK" : "COLOR-OFF");
    }
    std::printf("\n%s\n", failures == 0 ? "All patches within tolerance."
                                         : "Some patches are off. GPU-YUV-OFF: the driver did not produce the requested "
                                           "color space. COLOR-OFF: CPU decode assumption differs from GPU output.");
    return failures == 0 ? 0 : 5;
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
