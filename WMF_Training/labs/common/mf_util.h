// mf_util.h — COM / Media Foundation 初始化 RAII 與小工具
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <functional>
#include <string>

#include "hr.h"

using Microsoft::WRL::ComPtr;

// CoInitializeEx + MFStartup，解構時反向收尾。
// 用法：在 main 裡「最先」宣告，讓它比所有 ComPtr 都晚解構
//       （C++ 區域變數以宣告的相反順序解構）。
class MfScope {
public:
    MfScope()
    {
        CHECK_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        CHECK_HR(MFStartup(MF_VERSION));
    }
    ~MfScope()
    {
        MFShutdown();
        CoUninitialize();
    }
    MfScope(const MfScope&) = delete;
    MfScope& operator=(const MfScope&) = delete;
};

// 讓 DDA / 視窗座標以實體像素計算（否則在 150% 縮放下 GetSystemMetrics 等 API 會回傳虛擬化的值）
inline void EnablePerMonitorDpiAwareness()
{
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        if (auto fn = (Fn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
}

inline std::string Narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// 讓 console 能正確顯示 UTF-8 中文
inline void SetupConsole() { SetConsoleOutputCP(CP_UTF8); }

// 用 lambda 實作 IMFAsyncCallback（給 async MFT 的 BeginGetEvent 用）
class LambdaAsyncCallback : public IMFAsyncCallback {
public:
    explicit LambdaAsyncCallback(std::function<void(IMFAsyncResult*)> fn) : fn_(std::move(fn)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAsyncCallback)) {
            *ppv = static_cast<IMFAsyncCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* result) override
    {
        fn_(result);
        return S_OK;
    }

private:
    virtual ~LambdaAsyncCallback() = default;
    LONG ref_ = 1;
    std::function<void(IMFAsyncResult*)> fn_;
};

// 物件目前的參考計數（AddRef/Release 的回傳值）。
// 只用在 texture pool 判斷「encoder 是否還拿著這個 sample」。
inline ULONG PeekRefCount(IUnknown* p)
{
    p->AddRef();
    return p->Release();
}
