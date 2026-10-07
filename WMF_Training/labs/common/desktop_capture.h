// desktop_capture.h — DXGI Desktop Duplication 封裝（Lab 1 起使用）
//
// 生命週期契約：
//   AcquireNextFrame 成功 -> 拿到的 texture 只借到 ReleaseFrame 為止
//   -> 要保留就在 ReleaseFrame 之前 CopyResource 到自己的 texture
//   -> 每次「成功」的 Acquire 都必須配一次 ReleaseFrame（timeout 時不用）
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

using Microsoft::WRL::ComPtr;

enum class CaptureStatus {
    NewFrame,    // 有新的桌面畫面，已複製到 dest
    NoChange,    // DXGI_ERROR_WAIT_TIMEOUT：timeout 時間內桌面完全沒有變化
    PointerOnly, // Acquire 成功，但 LastPresentTime == 0：只有滑鼠更新，或剛建立 duplication 時的「空」frame
    AccessLost,  // 模式切換 / 鎖屏 / UAC：duplication 已失效，需要 Reinit()
};

class DesktopCapture {
public:
    // 回傳 HRESULT 而不是丟例外：E_ACCESSDENIED（鎖屏中）是要讓呼叫端決定怎麼處理的情況
    HRESULT Init(ID3D11Device* device, IDXGIOutput1* output);
    HRESULT Reinit() { return Init(device_.Get(), output_.Get()); }
    void Reset() { dup_.Reset(); }
    bool IsValid() const { return dup_ != nullptr; }

    // dest：自己擁有的 BGRA texture，尺寸 = Width() x Height()
    // acquireQpc：拿到畫面的時間（QPC），延遲量測的起點
    CaptureStatus Acquire(UINT timeoutMs, ID3D11Texture2D* dest, int64_t* acquireQpc = nullptr);

    // 最近一次成功 Acquire 的 frame 資訊（除錯 / 教學用）
    const DXGI_OUTDUPL_FRAME_INFO& LastFrameInfo() const { return lastInfo_; }

    UINT Width() const { return desc_.ModeDesc.Width; }
    UINT Height() const { return desc_.ModeDesc.Height; }
    DXGI_FORMAT Format() const { return desc_.ModeDesc.Format; }

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutput1> output_;
    ComPtr<IDXGIOutputDuplication> dup_;
    DXGI_OUTDUPL_DESC desc_{};
    DXGI_OUTDUPL_FRAME_INFO lastInfo_{};
};
