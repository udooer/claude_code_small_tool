// Lab 1 — 桌面擷取存成圖片
//
//   dda_capture.exe [--output N] [--retries N] [--delay ms] [--out capture.bmp] [--list]
//
// 學習重點：
//   1. 在「擁有該螢幕的 adapter」上建立 D3D11 device，DuplicateOutput
//   2. AcquireNextFrame -> CopyResource -> ReleaseFrame 的生命週期
//   3. WAIT_TIMEOUT 是正常的；ACCESS_LOST / E_ACCESSDENIED 要能清楚報錯而不是 crash
//   4. GPU texture 讀回 CPU：staging texture + Map + RowPitch
#include <cstdio>

#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "hr.h"
#include "mf_util.h"
#include "timer.h"

static int Run(const Args& args)
{
    if (args.Has("--list")) {
        PrintOutputs();
        return 0;
    }
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    const int maxRetries = (int)args.GetInt("--retries", 20);
    const DWORD delayMs = (DWORD)args.GetInt("--delay", 0);
    const std::string outPath = args.Get("--out", "capture.bmp");

    if (delayMs) { // 測試鎖定畫面：啟動後馬上按 Win+L
        std::printf("Waiting %lu ms before capture (press Win+L now to test the lock screen)...\n", delayMs);
        Sleep(delayMs);
    }

    D3DContext d3d = CreateD3DForOutput(outputIndex);
    const RECT& dr = d3d.outputDesc.DesktopCoordinates;
    std::printf("Adapter : %s\nOutput  : %s (%ldx%ld)\n", Narrow(d3d.adapterName).c_str(),
                Narrow(d3d.outputDesc.DeviceName).c_str(), dr.right - dr.left, dr.bottom - dr.top);

    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        if (hr == E_ACCESSDENIED)
            std::fprintf(stderr, "  -> The secure desktop (lock screen / UAC prompt) is active; normal processes can't capture it.\n");
        if (hr == DXGI_ERROR_UNSUPPORTED)
            std::fprintf(stderr, "  -> On hybrid-GPU laptops make sure the device is on the adapter that owns this output.\n");
        return 2;
    }
    std::printf("Duplication: %ux%u\n", capture.Width(), capture.Height());

    // 自己擁有的 texture：DDA 的 texture 只借到 ReleaseFrame，要保留必須複製出來
    ComPtr<ID3D11Texture2D> frame = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    const int64_t start = QpcNow();
    for (int attempt = 1; attempt <= maxRetries; ++attempt) {
        int64_t t0 = QpcNow();
        CaptureStatus s = capture.Acquire(500, frame.Get());
        double waited = QpcToMs(QpcNow() - t0);
        const DXGI_OUTDUPL_FRAME_INFO& fi = capture.LastFrameInfo();
        if (s == CaptureStatus::NewFrame) {
            std::printf("Got a frame on attempt %d after %.0f ms (Acquire returned in %.1f ms, AccumulatedFrames=%u)\n",
                        attempt, QpcToMs(QpcNow() - start), waited, fi.AccumulatedFrames);
            if (!SaveBgraTextureAsBmp(d3d.device.Get(), d3d.context.Get(), frame.Get(), outPath)) {
                std::fprintf(stderr, "Failed to write %s\n", outPath.c_str());
                return 3;
            }
            std::printf("Saved %s\n", outPath.c_str());
            return 0;
        }
        if (s == CaptureStatus::AccessLost) {
            std::fprintf(stderr, "DXGI_ERROR_ACCESS_LOST (mode change / lock screen / UAC). Re-creating duplication...\n");
            hr = capture.Reinit();
            if (FAILED(hr)) {
                std::fprintf(stderr, "Re-create failed: %s\n", HrToString(hr).c_str());
                return 2;
            }
            continue;
        }
        if (s == CaptureStatus::NoChange)
            std::printf("  attempt %d: DXGI_ERROR_WAIT_TIMEOUT after %.0f ms -> the desktop did not change at all\n", attempt, waited);
        else // PointerOnly
            std::printf("  attempt %d: frame returned in %.1f ms but LastPresentTime=0 (AccumulatedFrames=%u, "
                        "mouse update=%s) -> no new desktop image yet, skipped\n",
                        attempt, waited, fi.AccumulatedFrames, fi.LastMouseUpdateTime.QuadPart ? "yes" : "no");
    }
    std::fprintf(stderr, "Gave up after %d attempts without a new desktop frame. Move a window and try again.\n", maxRetries);
    return 4;
}

int main(int argc, char** argv)
{
    SetupConsole();
    EnablePerMonitorDpiAwareness();
    try {
        MfScope mf; // Lab 1 不需要 MF，但統一 COM 初始化
        return Run(Args(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
