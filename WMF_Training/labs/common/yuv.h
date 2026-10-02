// yuv.h — RGB <-> YUV 換算（手刻，平台無關，可在 Linux 上做單元測試）
//
// 這是 Lab 3 的核心：用「定義」推導公式，而不是背係數表。
//
//   Kr / Kb 由標準決定：BT.601 = (0.299, 0.114)，BT.709 = (0.2126, 0.0722)
//   E'y = Kr*R + (1-Kr-Kb)*G + Kb*B                 (R,G,B 正規化到 0..1)
//   Cb  = (B - E'y) / (2*(1-Kb))                     (-0.5..0.5)
//   Cr  = (R - E'y) / (2*(1-Kr))
//
//   Studio range: Y = 16 + 219*E'y,  U = 128 + 224*Cb,  V = 128 + 224*Cr
//   Full   range: Y = 255*E'y,       U = 128 + 255*Cb,  V = 128 + 255*Cr
//
// 把 BT.709 studio 代進去展開，就會得到 mentor 指南裡那張表：
//   R = 1.164(Y-16) + 1.793(V-128)
//   G = 1.164(Y-16) - 0.213(U-128) - 0.533(V-128)
//   B = 1.164(Y-16) + 2.112(U-128)
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

enum class YuvMatrix { BT601, BT709 };
enum class YuvRange { Studio, Full };

struct Yuv { uint8_t y, u, v; };
struct Rgb { uint8_t r, g, b; };

inline const char* ToString(YuvMatrix m) { return m == YuvMatrix::BT709 ? "BT.709" : "BT.601"; }
inline const char* ToString(YuvRange r) { return r == YuvRange::Studio ? "studio(16-235)" : "full(0-255)"; }

namespace yuv_detail {
inline void Coeffs(YuvMatrix m, double& kr, double& kb)
{
    if (m == YuvMatrix::BT709) { kr = 0.2126; kb = 0.0722; }
    else                       { kr = 0.299;  kb = 0.114;  }
}
inline uint8_t Clamp8(double v)
{
    // 沒有 clamp 的話，飽和色會溢位繞回，出現奇怪的反色點
    long r = std::lround(v);
    return (uint8_t)std::min(255L, std::max(0L, r));
}
} // namespace yuv_detail

inline Yuv RgbToYuv(Rgb c, YuvMatrix m, YuvRange range)
{
    double kr, kb;
    yuv_detail::Coeffs(m, kr, kb);
    double r = c.r / 255.0, g = c.g / 255.0, b = c.b / 255.0;
    double ey = kr * r + (1 - kr - kb) * g + kb * b;
    double cb = (b - ey) / (2 * (1 - kb));
    double cr = (r - ey) / (2 * (1 - kr));
    if (range == YuvRange::Studio)
        return { yuv_detail::Clamp8(16 + 219 * ey), yuv_detail::Clamp8(128 + 224 * cb), yuv_detail::Clamp8(128 + 224 * cr) };
    return { yuv_detail::Clamp8(255 * ey), yuv_detail::Clamp8(128 + 255 * cb), yuv_detail::Clamp8(128 + 255 * cr) };
}

// 預先算好的反轉換係數（避免每個像素都重算）
struct YuvToRgbCoeffs {
    double yOffset, yScale, cScale; // ey = (Y - yOffset) * yScale; cb = (U-128) * cScale
    double rv, gu, gv, bu;          // R = ey + rv*cr; G = ey - gu*cb - gv*cr; B = ey + bu*cb
};

inline YuvToRgbCoeffs MakeYuvToRgbCoeffs(YuvMatrix m, YuvRange range)
{
    double kr, kb;
    yuv_detail::Coeffs(m, kr, kb);
    double kg = 1 - kr - kb;
    YuvToRgbCoeffs k{};
    if (range == YuvRange::Studio) { k.yOffset = 16; k.yScale = 255.0 / 219; k.cScale = 255.0 / 224; }
    else                           { k.yOffset = 0;  k.yScale = 1.0;         k.cScale = 1.0; }
    k.rv = 2 * (1 - kr);
    k.bu = 2 * (1 - kb);
    k.gu = kb * k.bu / kg;
    k.gv = kr * k.rv / kg;
    return k;
}

inline Rgb YuvToRgb(int Y, int U, int V, const YuvToRgbCoeffs& k)
{
    double ey = (Y - k.yOffset) * k.yScale;
    double cb = (U - 128) * k.cScale;
    double cr = (V - 128) * k.cScale;
    return { yuv_detail::Clamp8(ey + k.rv * cr),
             yuv_detail::Clamp8(ey - k.gu * cb - k.gv * cr),
             yuv_detail::Clamp8(ey + k.bu * cb) };
}

inline Rgb YuvToRgb(Yuv p, YuvMatrix m, YuvRange range)
{
    return YuvToRgb(p.y, p.u, p.v, MakeYuvToRgbCoeffs(m, range));
}

// NV12 記憶體佈局：
//   yPlane : height 行，每行 pitch bytes，前 width bytes 是 Y
//   uvPlane: height/2 行，每行 pitch bytes，U V U V ... 交錯（U 在前）
// 注意：uvPlane 的位置是 yPlane + pitch * codedHeight，由呼叫端算好傳進來，
//       因為解碼器輸出的 codedHeight（例如 1088）可能大於要顯示的 height（1080）。
inline void Nv12ToBgra(const uint8_t* yPlane, const uint8_t* uvPlane, size_t pitch,
                       uint32_t width, uint32_t height,
                       YuvMatrix m, YuvRange range, uint8_t* bgraOut /* width*height*4 */)
{
    const YuvToRgbCoeffs k = MakeYuvToRgbCoeffs(m, range);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + y * pitch;
        const uint8_t* uvRow = uvPlane + (y / 2) * pitch;
        uint8_t* out = bgraOut + (size_t)y * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t* uv = uvRow + (x / 2) * 2; // 一組 UV 涵蓋 2x2 個像素
            Rgb c = YuvToRgb(yRow[x], uv[0], uv[1], k);
            out[x * 4 + 0] = c.b;
            out[x * 4 + 1] = c.g;
            out[x * 4 + 2] = c.r;
            out[x * 4 + 3] = 255;
        }
    }
}

// 讀出 NV12 中某個像素的 Y/U/V（用來和理論值比對）
inline Yuv SampleNv12(const uint8_t* yPlane, const uint8_t* uvPlane, size_t pitch, uint32_t x, uint32_t y)
{
    const uint8_t* uv = uvPlane + (y / 2) * pitch + (x / 2) * 2;
    return { yPlane[y * pitch + x], uv[0], uv[1] };
}
