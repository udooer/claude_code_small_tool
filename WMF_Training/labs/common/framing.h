// framing.h — Lab 6 的 TCP 訊框格式（平台無關）
//
// TCP 是 byte stream，沒有訊息邊界：一次 send 可能被拆成多次 recv，多次 send 也可能被一次 recv 收到。
// 所以每張編碼後的畫面前面加一個「固定長度」的 header，裡面有 payload 長度。
// Receiver 先收滿 kHeaderSize bytes，解析出長度，再收滿 payloadSize bytes，才算一張畫面。
//
// 除了長度，我們順便放了延遲量測需要的時間戳（同一台機器的 QPC 可以直接相減）。
// 所有整數都用 big-endian（network byte order），兩端 CPU 架構不同也不會錯。
#pragma once

#include <cstddef>
#include <cstdint>

namespace framing {

constexpr uint32_t kMagic = 0x574D4636;           // "WMF6"，用來偵測收錯位置
constexpr uint32_t kMaxPayload = 16 * 1024 * 1024; // 防止壞資料導致巨大配置
constexpr size_t kHeaderSize = 4 + 4 + 4 + 4 + 2 + 2 + 8 + 8;

enum Flags : uint32_t { kKeyframe = 1u << 0 };

struct FrameHeader {
    uint32_t magic = kMagic;
    uint32_t payloadSize = 0;
    uint32_t frameIndex = 0;
    uint32_t flags = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    int64_t captureQpc = 0;    // Sender: AcquireNextFrame 拿到畫面的時間
    int64_t encodeDoneQpc = 0; // Sender: encoder 吐出這張畫面的時間
};

inline void PutBE(uint8_t*& p, uint64_t v, int bytes)
{
    for (int i = bytes - 1; i >= 0; --i) *p++ = (uint8_t)(v >> (8 * i));
}
inline uint64_t GetBE(const uint8_t*& p, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | *p++;
    return v;
}

inline void Serialize(const FrameHeader& h, uint8_t out[kHeaderSize])
{
    uint8_t* p = out;
    PutBE(p, h.magic, 4);
    PutBE(p, h.payloadSize, 4);
    PutBE(p, h.frameIndex, 4);
    PutBE(p, h.flags, 4);
    PutBE(p, h.width, 2);
    PutBE(p, h.height, 2);
    PutBE(p, (uint64_t)h.captureQpc, 8);
    PutBE(p, (uint64_t)h.encodeDoneQpc, 8);
}

// 回傳 false 代表資料不合法（magic 錯或長度超過上限），呼叫端應該斷線。
inline bool Deserialize(const uint8_t in[kHeaderSize], FrameHeader& h)
{
    const uint8_t* p = in;
    h.magic = (uint32_t)GetBE(p, 4);
    h.payloadSize = (uint32_t)GetBE(p, 4);
    h.frameIndex = (uint32_t)GetBE(p, 4);
    h.flags = (uint32_t)GetBE(p, 4);
    h.width = (uint16_t)GetBE(p, 2);
    h.height = (uint16_t)GetBE(p, 2);
    h.captureQpc = (int64_t)GetBE(p, 8);
    h.encodeDoneQpc = (int64_t)GetBE(p, 8);
    return h.magic == kMagic && h.payloadSize > 0 && h.payloadSize <= kMaxPayload;
}

} // namespace framing
