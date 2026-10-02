// hr.h — HRESULT 錯誤處理
//
// 訓練用程式碼選擇用例外：CHECK_HR 失敗就丟 HrError，main() 統一 catch 並印出
// 「哪一行、哪個呼叫、HRESULT 數值與名稱」。學員回報錯誤時一律附上這一行。
#pragma once

#include <windows.h>
#include <dxgi.h>
#include <mferror.h>

#include <cstdio>
#include <stdexcept>
#include <string>

inline const char* HrName(HRESULT hr)
{
    switch (hr) {
    case S_OK: return "S_OK";
    case E_FAIL: return "E_FAIL";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOTIMPL: return "E_NOTIMPL";
    case E_NOINTERFACE: return "E_NOINTERFACE";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case REGDB_E_CLASSNOTREG: return "REGDB_E_CLASSNOTREG (codec not installed: Windows N/KN edition without Media Feature Pack?)";
    case E_ACCESSDENIED: return "E_ACCESSDENIED (secure desktop: lock screen / UAC?)";
    case DXGI_ERROR_WAIT_TIMEOUT: return "DXGI_ERROR_WAIT_TIMEOUT";
    case DXGI_ERROR_ACCESS_LOST: return "DXGI_ERROR_ACCESS_LOST";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED (hybrid GPU wrong adapter / RDP session?)";
    case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
    case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
    case DXGI_ERROR_NOT_FOUND: return "DXGI_ERROR_NOT_FOUND";
    case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
    case MF_E_TRANSFORM_NEED_MORE_INPUT: return "MF_E_TRANSFORM_NEED_MORE_INPUT";
    case MF_E_TRANSFORM_STREAM_CHANGE: return "MF_E_TRANSFORM_STREAM_CHANGE";
    case MF_E_TRANSFORM_TYPE_NOT_SET: return "MF_E_TRANSFORM_TYPE_NOT_SET";
    case MF_E_TRANSFORM_ASYNC_LOCKED: return "MF_E_TRANSFORM_ASYNC_LOCKED";
    case MF_E_INVALIDMEDIATYPE: return "MF_E_INVALIDMEDIATYPE";
    case MF_E_INVALIDSTREAMNUMBER: return "MF_E_INVALIDSTREAMNUMBER";
    case MF_E_NOTACCEPTING: return "MF_E_NOTACCEPTING";
    case MF_E_NO_SAMPLE_TIMESTAMP: return "MF_E_NO_SAMPLE_TIMESTAMP";
    case MF_E_NO_MORE_TYPES: return "MF_E_NO_MORE_TYPES";
    case MF_E_SHUTDOWN: return "MF_E_SHUTDOWN";
    case MF_E_UNSUPPORTED_D3D_TYPE: return "MF_E_UNSUPPORTED_D3D_TYPE";
    default: return "";
    }
}

inline std::string HrToString(HRESULT hr)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "0x%08lX %s", (unsigned long)hr, HrName(hr));
    std::string s = buf;
    char* msg = nullptr;
    if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)hr, 0, (LPSTR)&msg, 0, nullptr) && msg) {
        std::string m = msg;
        LocalFree(msg);
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r')) m.pop_back();
        s += " - " + m;
    }
    return s;
}

class HrError : public std::runtime_error {
public:
    HrError(HRESULT hr, const char* expr, const char* file, int line)
        : std::runtime_error(Format(hr, expr, file, line)), hr_(hr) {}
    HRESULT hr() const { return hr_; }

private:
    static std::string Format(HRESULT hr, const char* expr, const char* file, int line)
    {
        const char* base = file;
        for (const char* p = file; *p; ++p)
            if (*p == '/' || *p == '\\') base = p + 1;
        return std::string(expr) + "\n    failed at " + base + ":" + std::to_string(line) + "\n    hr = " + HrToString(hr);
    }
    HRESULT hr_;
};

#define CHECK_HR(expr)                                                  \
    do {                                                                \
        HRESULT hr_check__ = (expr);                                    \
        if (FAILED(hr_check__)) throw HrError(hr_check__, #expr, __FILE__, __LINE__); \
    } while (0)

// 不想讓程式中斷、只想記錄的呼叫（例如可選的 CODECAPI 屬性）
#define WARN_HR(expr)                                                                   \
    do {                                                                                \
        HRESULT hr_warn__ = (expr);                                                     \
        if (FAILED(hr_warn__))                                                          \
            std::fprintf(stderr, "  [warn] %s -> %s\n", #expr, HrToString(hr_warn__).c_str()); \
    } while (0)
