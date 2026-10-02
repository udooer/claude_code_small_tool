// render_window.h — Win32 視窗 + flip-model swap chain，把解碼後的 NV12 畫面顯示出來（Lab 5 --play / Lab 6 receiver）
//
// 顯示路徑全程 GPU：decoder 的 NV12 texture --VideoProcessorBlt(縮放 + NV12->BGRA)--> swap chain back buffer --> Present
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

#include "h264_decoder.h"
#include "video_scaler.h"

using Microsoft::WRL::ComPtr;

class RenderWindow {
public:
    void Create(const wchar_t* title, UINT clientWidth, UINT clientHeight, ID3D11Device* device);
    ~RenderWindow();

    // 處理視窗訊息；視窗被關掉或按 ESC 時回傳 false
    bool PumpMessages();

    // 最近一次按下的字元（'+', '-' 等），沒有則回傳 0
    int TakeLastChar();

    void Present(const DecodedFrame& frame, bool vsync);
    void SetTitle(const std::string& utf8);
    HWND Hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    ID3D11Texture2D* UploadCpuFrame(const DecodedFrame& f);

    HWND hwnd_ = nullptr;
    bool closed_ = false;
    int lastChar_ = 0;
    UINT width_ = 0, height_ = 0;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    VideoScaler scaler_;
    UINT scalerSrcW_ = 0, scalerSrcH_ = 0;
    ComPtr<ID3D11Texture2D> uploadTex_, uploadStaging_; // 只有軟體解碼 fallback 才會用到
};
