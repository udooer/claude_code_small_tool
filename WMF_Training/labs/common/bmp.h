// bmp.h — 32bpp BMP 寫檔（平台無關）
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// pixels: top-down、緊密排列（stride = width*4）、BGRA 順序。
// BMP 本身就是 B,G,R,(A) 的順序，所以 DXGI 的 B8G8R8A8 可以直接寫，不用換通道。
// biHeight 用負值 = top-down，第一行就是畫面最上面那行（正值的話要倒著寫）。
inline bool SaveBmp32(const std::string& path, const uint8_t* pixels, uint32_t width, uint32_t height)
{
    const uint32_t imageSize = width * height * 4;
    const uint32_t headerSize = 14 + 40;
    std::vector<uint8_t> h(headerSize, 0);
    auto put16 = [&](size_t off, uint16_t v) { h[off] = v & 0xFF; h[off + 1] = v >> 8; };
    auto put32 = [&](size_t off, uint32_t v) { for (int i = 0; i < 4; ++i) h[off + i] = (v >> (8 * i)) & 0xFF; };

    // BITMAPFILEHEADER
    h[0] = 'B'; h[1] = 'M';
    put32(2, headerSize + imageSize);
    put32(10, headerSize);
    // BITMAPINFOHEADER
    put32(14, 40);
    put32(18, width);
    put32(22, (uint32_t)(-(int32_t)height)); // top-down
    put16(26, 1);                            // planes
    put16(28, 32);                           // bpp
    put32(30, 0);                            // BI_RGB
    put32(34, imageSize);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size() &&
              std::fwrite(pixels, 1, imageSize, f) == imageSize;
    std::fclose(f);
    return ok;
}
