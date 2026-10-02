#include "render_window.h"

#include <cstring>

#include "d3d_util.h"
#include "hr.h"

namespace {
const wchar_t* kClassName = L"WmfLabRenderWindow";
}

LRESULT CALLBACK RenderWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    auto* self = reinterpret_cast<RenderWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && self) self->closed_ = true;
        return 0;
    case WM_CHAR:
        if (self) self->lastChar_ = (int)wp;
        return 0;
    case WM_CLOSE:
        if (self) self->closed_ = true;
        return 0; // 由擁有者決定何時 DestroyWindow
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RenderWindow::Create(const wchar_t* title, UINT clientWidth, UINT clientHeight, ID3D11Device* device)
{
    device_ = device;
    device->GetImmediateContext(&context_);
    width_ = clientWidth;
    height_ = clientHeight;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc); // 重複註冊會失敗，無妨

    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX); // 固定大小，省去 ResizeBuffers
    RECT r{ 0, 0, (LONG)clientWidth, (LONG)clientHeight };
    AdjustWindowRect(&r, style, FALSE);
    hwnd_ = CreateWindowExW(0, kClassName, title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) CHECK_HR(HRESULT_FROM_WIN32(GetLastError()));
    ShowWindow(hwnd_, SW_SHOW);

    ComPtr<IDXGIDevice> dxgiDevice;
    CHECK_HR(device_.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    CHECK_HR(dxgiDevice->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    CHECK_HR(adapter->GetParent(IID_PPV_ARGS(&factory)));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = clientWidth;
    sd.Height = clientHeight;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    CHECK_HR(factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &swapChain_));
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
}

RenderWindow::~RenderWindow()
{
    swapChain_.Reset();
    if (hwnd_) DestroyWindow(hwnd_);
}

bool RenderWindow::PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

int RenderWindow::TakeLastChar()
{
    int c = lastChar_;
    lastChar_ = 0;
    return c;
}

void RenderWindow::SetTitle(const std::string& utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    SetWindowTextW(hwnd_, w.c_str());
}

ID3D11Texture2D* RenderWindow::UploadCpuFrame(const DecodedFrame& f)
{
    // 軟體解碼時畫面在 CPU：CPU -> staging -> GPU。這是 fallback 路徑，正常應走 D3D 解碼。
    D3D11_TEXTURE2D_DESC d{};
    if (uploadTex_) uploadTex_->GetDesc(&d);
    if (!uploadTex_ || d.Width != f.codedWidth || d.Height != f.codedHeight) {
        uploadTex_ = CreateTexture(device_.Get(), f.codedWidth, f.codedHeight, DXGI_FORMAT_NV12, 0);
        uploadStaging_ = CreateTexture(device_.Get(), f.codedWidth, f.codedHeight, DXGI_FORMAT_NV12, 0,
                                       D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_WRITE);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK_HR(context_->Map(uploadStaging_.Get(), 0, D3D11_MAP_WRITE, 0, &m));
    const uint8_t* srcY = f.cpuData;
    const uint8_t* srcUV = f.cpuData + (size_t)f.cpuPitch * f.codedHeight;
    uint8_t* dstY = (uint8_t*)m.pData;
    uint8_t* dstUV = dstY + (size_t)m.RowPitch * f.codedHeight;
    for (UINT y = 0; y < f.codedHeight; ++y) std::memcpy(dstY + (size_t)y * m.RowPitch, srcY + (size_t)y * f.cpuPitch, f.codedWidth);
    for (UINT y = 0; y < f.codedHeight / 2; ++y) std::memcpy(dstUV + (size_t)y * m.RowPitch, srcUV + (size_t)y * f.cpuPitch, f.codedWidth);
    context_->Unmap(uploadStaging_.Get(), 0);
    context_->CopyResource(uploadTex_.Get(), uploadStaging_.Get());
    return uploadTex_.Get();
}

void RenderWindow::Present(const DecodedFrame& f, bool vsync)
{
    ID3D11Texture2D* src = f.texture;
    UINT slice = f.subresource;
    if (!src) {
        src = UploadCpuFrame(f);
        slice = 0;
    }

    // 來源尺寸改變（第一次、或 Lab 6 切換解析度）時重建 video processor
    if (!scaler_.IsInitialized() || scalerSrcW_ != f.displayWidth || scalerSrcH_ != f.displayHeight) {
        ScalerConfig c;
        c.srcWidth = f.displayWidth; // 用 display 尺寸：1088 多出來的 8 行不會被顯示
        c.srcHeight = f.displayHeight;
        c.srcFormat = DXGI_FORMAT_NV12;
        c.dstWidth = width_;
        c.dstHeight = height_;
        c.dstFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        c.mode = ScaleMode::Letterbox;
        scaler_.Init(device_.Get(), c); // 色彩空間：BT.709 studio，與 sender 端一致
        scalerSrcW_ = f.displayWidth;
        scalerSrcH_ = f.displayHeight;
    }

    // flip model 在 D3D11 下 buffer 0 永遠代表「目前的 back buffer」
    ComPtr<ID3D11Texture2D> backBuffer;
    CHECK_HR(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)));
    scaler_.Process(src, slice, backBuffer.Get());
    HRESULT hr = swapChain_->Present(vsync ? 1 : 0, 0);
    if (hr != DXGI_STATUS_OCCLUDED) CHECK_HR(hr);
}
