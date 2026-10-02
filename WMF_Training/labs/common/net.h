// net.h — Winsock 小工具（Lab 6）
#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <stdexcept>
#include <string>

class WsaScope {
public:
    WsaScope()
    {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~WsaScope() { WSACleanup(); }
};

// recv 不保證一次收滿：一定要迴圈。這是 Lab 6 最常見的 bug（本機測試幾乎不會出現，跨機器才爆）。
inline bool RecvAll(SOCKET s, void* buf, int len)
{
    char* p = static_cast<char*>(buf);
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false; // 0 = 對方正常關閉；<0 = 錯誤（例如 WSAECONNRESET）
        p += n;
        len -= n;
    }
    return true;
}

// send 在阻塞模式下通常會送完，但規範上仍可能只送一部分
inline bool SendAll(SOCKET s, const void* buf, int len)
{
    const char* p = static_cast<const char*>(buf);
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n == SOCKET_ERROR) return false;
        p += n;
        len -= n;
    }
    return true;
}

// 關掉 Nagle：小封包不要等著合併，否則延遲會多 40~200ms（勘誤 M11）
inline void SetNoDelay(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

inline std::string WsaErrorString(int err)
{
    return "WSA error " + std::to_string(err);
}
