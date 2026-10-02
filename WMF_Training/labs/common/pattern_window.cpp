#include "pattern_window.h"

#include "hr.h"

const std::vector<Patch>& TestPatches()
{
    static const std::vector<Patch> patches = {
        { "red", { 255, 0, 0 } },     { "green", { 0, 255, 0 } }, { "blue", { 0, 0, 255 } },
        { "white", { 255, 255, 255 } }, { "black", { 0, 0, 0 } }, { "gray", { 128, 128, 128 } },
    };
    return patches;
}

LRESULT CALLBACK PatternWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        const auto& p = TestPatches();
        for (size_t i = 0; i < p.size(); ++i) {
            RECT r{ (LONG)(client.right * i / p.size()), 0, (LONG)(client.right * (i + 1) / p.size()), client.bottom };
            HBRUSH b = CreateSolidBrush(RGB(p[i].color.r, p[i].color.g, p[i].color.b));
            FillRect(dc, &r, b);
            DeleteObject(b);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

PatternWindow::PatternWindow(const RECT& r)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"WmfLabPatternWindow";
    RegisterClassExW(&wc);
    width_ = r.right - r.left;
    height_ = r.bottom - r.top;
    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"patches", WS_POPUP | WS_VISIBLE,
                            r.left, r.top, width_, height_, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd_) CHECK_HR(HRESULT_FROM_WIN32(GetLastError()));
    SetForegroundWindow(hwnd_);
}

PatternWindow::~PatternWindow()
{
    if (hwnd_) DestroyWindow(hwnd_);
}

void PatternWindow::PumpFor(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    while (GetTickCount() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
}

POINT PatternWindow::PatchCenter(size_t i) const
{
    size_t n = TestPatches().size();
    return { (LONG)(width_ * (2 * i + 1) / (2 * n)), height_ / 2 };
}
