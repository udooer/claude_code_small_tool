// codec_loopback.cpp — 不需要螢幕擷取的 encoder -> decoder 自我測試（Windows）
//
//   codec_loopback.exe [--frames 60] [--width 640] [--height 360] [--hw] [--out loopback.h264]
//
// 產生會動的 NV12 測試畫面（CPU 記憶體）-> H264Encoder -> 寫檔 -> H264Decoder（軟體）-> 檢查：
//   1. 解出來的張數 == 送進去的張數（encoder 與 decoder 都有 drain）
//   2. 第一張畫面的色塊顏色正確（BT.709 studio 一路對齊）
// 用途：在 VM / CI / 沒有螢幕的機器上確認 MF 編解碼環境與封裝類別正常。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cli.h"
#include "h264_decoder.h"
#include "h264_encoder.h"
#include "hr.h"
#include "mf_util.h"
#include "yuv.h"

static const Rgb kBars[] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };

// 四條直色塊 + 一個會移動的白色方塊
static void MakeFrame(std::vector<uint8_t>& nv12, UINT w, UINT h, UINT index)
{
    uint8_t* yP = nv12.data();
    uint8_t* uvP = nv12.data() + (size_t)w * h;
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) {
            Rgb c = kBars[x * 4 / w];
            UINT bx = (index * 8) % (w - 32);
            if (x >= bx && x < bx + 32 && y >= h / 2 && y < h / 2 + 32) c = { 0, 0, 0 };
            Yuv p = RgbToYuv(c, YuvMatrix::BT709, YuvRange::Studio);
            yP[(size_t)y * w + x] = p.y;
            if ((x % 2) == 0 && (y % 2) == 0) {
                uvP[(size_t)(y / 2) * w + x] = p.u;
                uvP[(size_t)(y / 2) * w + x + 1] = p.v;
            }
        }
}

static int Run(Args& args)
{
    args.DeclareFlags({ "--hw" });
    const UINT frames = (UINT)args.GetInt("--frames", 60);
    EncoderConfig ec;
    ec.width = (UINT)args.GetInt("--width", 640) & ~1u;
    ec.height = (UINT)args.GetInt("--height", 360) & ~1u;
    ec.fps = 30;
    ec.bitrate = 2'000'000;
    ec.gopSize = 30;
    ec.preferHardware = args.Has("--hw");
    const std::string outPath = args.Get("--out", "loopback.h264");

    std::vector<uint8_t> bitstream;
    int encodedFrames = 0;
    H264Encoder enc;
    enc.Init(ec, nullptr, [&](EncodedFrame&& f) {
        bitstream.insert(bitstream.end(), f.data.begin(), f.data.end());
        ++encodedFrames;
    });
    std::printf("Encoder: %s (hw=%s async=%s)\n", enc.Name().c_str(), enc.IsHardware() ? "yes" : "no", enc.IsAsync() ? "yes" : "no");

    const DWORD size = ec.width * ec.height * 3 / 2;
    std::vector<uint8_t> nv12(size);
    for (UINT i = 0; i < frames; ++i) {
        MakeFrame(nv12, ec.width, ec.height, i);
        ComPtr<IMFMediaBuffer> buf;
        CHECK_HR(MFCreateMemoryBuffer(size, &buf));
        BYTE* p = nullptr;
        CHECK_HR(buf->Lock(&p, nullptr, nullptr));
        std::memcpy(p, nv12.data(), size);
        buf->Unlock();
        CHECK_HR(buf->SetCurrentLength(size));
        ComPtr<IMFSample> s;
        CHECK_HR(MFCreateSample(&s));
        CHECK_HR(s->AddBuffer(buf.Get()));
        CHECK_HR(s->SetSampleTime(MFllMulDiv(i, 10'000'000, ec.fps, 0)));
        CHECK_HR(s->SetSampleDuration(MFllMulDiv(1, 10'000'000, ec.fps, 0)));
        enc.Encode(s.Get());
    }
    enc.Drain();
    enc.Shutdown();
    std::printf("Encoded %d / %u frames, %zu bytes\n", encodedFrames, frames, bitstream.size());
    if (FILE* f = std::fopen(outPath.c_str(), "wb")) {
        std::fwrite(bitstream.data(), 1, bitstream.size(), f);
        std::fclose(f);
    }

    int decodedFrames = 0, colorErrors = 0;
    H264Decoder dec;
    dec.Init(nullptr, false);
    auto onFrame = [&](const DecodedFrame& f) {
        if (decodedFrames++ != 0) return;
        const uint8_t* uv = f.cpuData + (size_t)f.cpuPitch * f.codedHeight;
        for (int b = 0; b < 4; ++b) {
            UINT x = (UINT)(f.displayWidth * (2 * b + 1) / 8) & ~1u, y = f.displayHeight / 4;
            Yuv got{ f.cpuData[(size_t)y * f.cpuPitch + x], uv[(size_t)(y / 2) * f.cpuPitch + x], uv[(size_t)(y / 2) * f.cpuPitch + x + 1] };
            Rgb c = YuvToRgb(got, YuvMatrix::BT709, YuvRange::Studio);
            bool ok = std::abs(c.r - kBars[b].r) <= 8 && std::abs(c.g - kBars[b].g) <= 8 && std::abs(c.b - kBars[b].b) <= 8;
            colorErrors += !ok;
            std::printf("  bar %d: YUV %3d %3d %3d -> RGB %3d %3d %3d  expected %3d %3d %3d  %s\n", b, got.y, got.u, got.v,
                        c.r, c.g, c.b, kBars[b].r, kBars[b].g, kBars[b].b, ok ? "OK" : "OFF");
        }
    };
    // 整個 bitstream 一次送也可以：decoder 自己會切 NAL。這裡刻意分塊送，模擬網路收到的片段。
    const size_t chunk = 4096;
    for (size_t off = 0; off < bitstream.size(); off += chunk)
        dec.Decode(bitstream.data() + off, std::min(chunk, bitstream.size() - off), (int64_t)off, onFrame);
    dec.Drain(onFrame);
    std::printf("Decoded %d / %u frames\n", decodedFrames, frames);

    bool pass = encodedFrames == (int)frames && decodedFrames == (int)frames && colorErrors == 0;
    std::printf("%s\n", pass ? "LOOPBACK PASS" : "LOOPBACK FAIL");
    return pass ? 0 : 5;
}

int main(int argc, char** argv)
{
    SetupConsole();
    try {
        MfScope mf;
        Args args(argc, argv);
        return Run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
