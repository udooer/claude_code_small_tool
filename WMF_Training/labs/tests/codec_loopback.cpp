// codec_loopback.cpp — 不需要螢幕擷取的 encoder -> decoder 自我測試（Windows）
//
//   codec_loopback.exe [--frames 60] [--width 640] [--height 360] [--hw] [--hw-sysmem] [--out loopback.h264]
//
// 產生會動的 NV12 測試畫面 -> H264Encoder -> 寫檔 -> H264Decoder（軟體）-> 檢查：
//   預設        ：軟體 encoder，畫面在 CPU 記憶體
//   --hw        ：硬體 encoder，畫面上傳到 GPU NV12 texture + D3D11 device manager（和 Lab 4/6 相同的路徑）
//   --hw-sysmem ：硬體 encoder 但直接餵 CPU 記憶體（不給 D3D device）。有些硬體 encoder（例如 Intel QSV）
//                 在這種組態下會在 ProcessOutput 失敗，用來對照「為什麼要給 device manager」
//   1. 解出來的張數 == 送進去的張數（encoder 與 decoder 都有 drain）
//   2. 第一張畫面的色塊顏色正確（BT.709 studio 一路對齊）
// 用途：在 VM / CI / 沒有螢幕的機器上確認 MF 編解碼環境與封裝類別正常。
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "cli.h"
#include "d3d_util.h"
#include "h264_decoder.h"
#include "h264_encoder.h"
#include "hr.h"
#include "mf_util.h"
#include "nv12_pool.h"
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
    args.DeclareFlags({ "--hw", "--hw-sysmem" });
    const bool hwSysmem = args.Has("--hw-sysmem");
    const bool useGpu = args.Has("--hw") && !hwSysmem;
    const UINT frames = (UINT)args.GetInt("--frames", 60);
    EncoderConfig ec;
    ec.width = (UINT)args.GetInt("--width", 640) & ~1u;
    ec.height = (UINT)args.GetInt("--height", 360) & ~1u;
    ec.fps = 30;
    ec.bitrate = 2'000'000;
    ec.gopSize = 30;
    ec.preferHardware = args.Has("--hw") || hwSysmem;
    const std::string outPath = args.Get("--out", "loopback.h264");

    std::vector<uint8_t> bitstream;
    int encodedFrames = 0;
    // --hw：和 Lab 4 一樣，建立 D3D11 device + device manager，畫面放在 GPU texture
    D3DContext d3d;
    ComPtr<IMFDXGIDeviceManager> devMgr;
    Nv12SamplePool pool;
    ComPtr<ID3D11Texture2D> upload;
    if (useGpu) {
        d3d = CreateD3DDefault();
        devMgr = CreateDxgiDeviceManager(d3d.device.Get());
        pool.Init(d3d.device.Get(), ec.width, ec.height, 8);
        upload = CreateTexture(d3d.device.Get(), ec.width, ec.height, DXGI_FORMAT_NV12, 0, D3D11_USAGE_STAGING,
                               D3D11_CPU_ACCESS_WRITE);
        std::printf("D3D11 device on: %s\n", Narrow(d3d.adapterName).c_str());
    }

    H264Encoder enc;
    enc.Init(ec, devMgr.Get(), [&](EncodedFrame&& f) {
        bitstream.insert(bitstream.end(), f.data.begin(), f.data.end());
        ++encodedFrames;
    });
    std::printf("Encoder: %s (hw=%s async=%s d3d11-input=%s)\n", enc.Name().c_str(), enc.IsHardware() ? "yes" : "no",
                enc.IsAsync() ? "yes" : "no", enc.UsesD3D() ? "yes" : "no");
    if (useGpu && !enc.UsesD3D())
        std::printf("  NOTE: encoder rejected our D3D11 device (different GPU?) -> falling back to CPU frames\n");

    const DWORD size = ec.width * ec.height * 3 / 2;
    std::vector<uint8_t> nv12(size);
    for (UINT i = 0; i < frames; ++i) {
        MakeFrame(nv12, ec.width, ec.height, i);
        ComPtr<IMFSample> s;
        if (useGpu && enc.UsesD3D()) {
            // CPU 測試畫面 -> staging texture -> pool 中的 NV12 texture（實際 Lab 中這一步是 VideoProcessorBlt）
            D3D11_MAPPED_SUBRESOURCE m;
            CHECK_HR(d3d.context->Map(upload.Get(), 0, D3D11_MAP_WRITE, 0, &m));
            uint8_t* dstY = (uint8_t*)m.pData;
            uint8_t* dstUV = dstY + (size_t)m.RowPitch * ec.height;
            for (UINT y = 0; y < ec.height; ++y) std::memcpy(dstY + (size_t)y * m.RowPitch, nv12.data() + (size_t)y * ec.width, ec.width);
            for (UINT y = 0; y < ec.height / 2; ++y)
                std::memcpy(dstUV + (size_t)y * m.RowPitch, nv12.data() + (size_t)ec.width * ec.height + (size_t)y * ec.width, ec.width);
            d3d.context->Unmap(upload.Get(), 0);
            Nv12SamplePool::Slot* slot = pool.Acquire();
            if (!slot) throw std::runtime_error("all NV12 textures are still held by the encoder");
            d3d.context->CopyResource(slot->texture.Get(), upload.Get());
            s = slot->sample;
        } else {
            ComPtr<IMFMediaBuffer> buf;
            CHECK_HR(MFCreateMemoryBuffer(size, &buf));
            BYTE* p = nullptr;
            CHECK_HR(buf->Lock(&p, nullptr, nullptr));
            std::memcpy(p, nv12.data(), size);
            buf->Unlock();
            CHECK_HR(buf->SetCurrentLength(size));
            CHECK_HR(MFCreateSample(&s));
            CHECK_HR(s->AddBuffer(buf.Get()));
        }
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
