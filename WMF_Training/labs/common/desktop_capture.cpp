#include "desktop_capture.h"

#include <cstdio>

#include "hr.h"
#include "timer.h"

HRESULT DesktopCapture::Init(ID3D11Device* device, IDXGIOutput1* output)
{
    dup_.Reset(); // 同一個 output 不能同時有兩個 duplication，先放掉舊的
    device_ = device;
    output_ = output;
    device->GetImmediateContext(&context_);

    HRESULT hr = output->DuplicateOutput(device, &dup_);
    if (FAILED(hr)) return hr;

    dup_->GetDesc(&desc_);
    if (desc_.Rotation != DXGI_MODE_ROTATION_IDENTITY && desc_.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
        std::fprintf(stderr, "  [warn] output is rotated; captured image is NOT rotated (not handled in this lab)\n");
    return S_OK;
}

CaptureStatus DesktopCapture::Acquire(UINT timeoutMs, ID3D11Texture2D* dest, int64_t* acquireQpc)
{
    if (!dup_) return CaptureStatus::AccessLost;

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = dup_->AcquireNextFrame(timeoutMs, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return CaptureStatus::NoChange; // 正常：畫面沒變，不需要 ReleaseFrame
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        dup_.Reset();
        return CaptureStatus::AccessLost;
    }
    CHECK_HR(hr);
    int64_t t = QpcNow();

    // 從這裡開始，不管發生什麼都要 ReleaseFrame
    struct ReleaseGuard {
        IDXGIOutputDuplication* d;
        ~ReleaseGuard() { d->ReleaseFrame(); }
    } guard{ dup_.Get() };

    // LastPresentTime == 0：只有滑鼠位置/形狀更新，桌面影像沒變（勘誤 M3）
    if (info.LastPresentTime.QuadPart == 0) return CaptureStatus::NoChange;

    ComPtr<ID3D11Texture2D> desktopTex;
    CHECK_HR(resource.As(&desktopTex));
    context_->CopyResource(dest, desktopTex.Get()); // 必須在 ReleaseFrame 之前
    if (acquireQpc) *acquireQpc = t;
    return CaptureStatus::NewFrame;
}
