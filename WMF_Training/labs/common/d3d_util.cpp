#include "d3d_util.h"

#include <d3d10.h> // ID3D10Multithread

#include <cstdio>
#include <cstring>

#include "bmp.h"
#include "hr.h"
#include "mf_util.h"
#include "timer.h"

namespace {

constexpr UINT kDeviceFlags =
    D3D11_CREATE_DEVICE_BGRA_SUPPORT |  // 跟 DXGI/D2D 互通
    D3D11_CREATE_DEVICE_VIDEO_SUPPORT;  // Media Foundation / Video Processor 需要

ComPtr<ID3D11Device> CreateDeviceOn(IDXGIAdapter* adapter, ComPtr<ID3D11DeviceContext>& ctx)
{
    UINT flags = kDeviceFlags;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG; // 結束時會在 Output 視窗報告沒釋放的物件
#endif
    static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    ComPtr<ID3D11Device> device;
    // 指定 adapter 時 driver type 必須是 UNKNOWN
    D3D_DRIVER_TYPE type = adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device, nullptr, &ctx);
#if defined(_DEBUG)
    if (FAILED(hr)) { // 沒裝 Graphics Tools 時 debug layer 會失敗，退回不帶 debug
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter, type, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device, nullptr, &ctx);
    }
#endif
    CHECK_HR(hr);
    EnableMultithreadProtection(device.Get());
    return device;
}

} // namespace

void EnableMultithreadProtection(ID3D11Device* device)
{
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt))))
        mt->SetMultithreadProtected(TRUE);
}

void PrintOutputs()
{
    ComPtr<IDXGIFactory1> factory;
    CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    UINT global = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        DXGI_ADAPTER_DESC1 ad;
        adapter->GetDesc1(&ad);
        std::printf("Adapter %u: %s\n", a, Narrow(ad.Description).c_str());
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o, output.Reset()) {
            DXGI_OUTPUT_DESC od;
            output->GetDesc(&od);
            const RECT& r = od.DesktopCoordinates;
            std::printf("  --output %u : %s  %ldx%ld at (%ld,%ld)\n", global++, Narrow(od.DeviceName).c_str(),
                        r.right - r.left, r.bottom - r.top, r.left, r.top);
        }
    }
}

D3DContext CreateD3DForOutput(UINT outputIndex)
{
    ComPtr<IDXGIFactory1> factory;
    CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

    D3DContext d;
    UINT global = 0;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a, adapter.Reset()) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o, output.Reset()) {
            if (global++ != outputIndex) continue;
            CHECK_HR(output.As(&d.output));
            output->GetDesc(&d.outputDesc);
            d.adapter = adapter;
            DXGI_ADAPTER_DESC1 ad;
            adapter->GetDesc1(&ad);
            d.adapterName = ad.Description;
            d.device = CreateDeviceOn(adapter.Get(), d.context);
            return d;
        }
    }
    std::fprintf(stderr, "Output index %u not found. Available outputs:\n", outputIndex);
    PrintOutputs();
    throw std::runtime_error("output not found");
}

D3DContext CreateD3DDefault()
{
    D3DContext d;
    d.device = CreateDeviceOn(nullptr, d.context);
    ComPtr<IDXGIDevice> dxgiDevice;
    CHECK_HR(d.device.As(&dxgiDevice));
    ComPtr<IDXGIAdapter> adapter;
    CHECK_HR(dxgiDevice->GetAdapter(&adapter));
    CHECK_HR(adapter.As(&d.adapter));
    DXGI_ADAPTER_DESC1 ad;
    d.adapter->GetDesc1(&ad);
    d.adapterName = ad.Description;
    return d;
}

ComPtr<IMFDXGIDeviceManager> CreateDxgiDeviceManager(ID3D11Device* device)
{
    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> mgr;
    CHECK_HR(MFCreateDXGIDeviceManager(&token, &mgr));
    CHECK_HR(mgr->ResetDevice(device, token));
    return mgr;
}

ComPtr<ID3D11Texture2D> CreateTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                      UINT bindFlags, D3D11_USAGE usage, UINT cpuAccess)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = usage;
    d.BindFlags = bindFlags;
    d.CPUAccessFlags = cpuAccess;
    ComPtr<ID3D11Texture2D> tex;
    CHECK_HR(device->CreateTexture2D(&d, nullptr, &tex));
    return tex;
}

std::vector<uint8_t> ReadbackBgra(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                                  UINT* outWidth, UINT* outHeight)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    // DEFAULT usage 的 texture 不能 Map，要先複製到 STAGING + CPU_ACCESS_READ
    ComPtr<ID3D11Texture2D> staging =
        CreateTexture(device, d.Width, d.Height, d.Format, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ctx->CopyResource(staging.Get(), tex);

    D3D11_MAPPED_SUBRESOURCE m;
    CHECK_HR(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)); // CPU 在這裡等 GPU 做完 Copy
    std::vector<uint8_t> pixels((size_t)d.Width * d.Height * 4);
    for (UINT y = 0; y < d.Height; ++y) // 逐行複製，因為 RowPitch >= Width*4（有對齊 padding）
        std::memcpy(&pixels[(size_t)y * d.Width * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, d.Width * 4);
    ctx->Unmap(staging.Get(), 0);
    if (outWidth) *outWidth = d.Width;
    if (outHeight) *outHeight = d.Height;
    return pixels;
}

bool SaveBgraTextureAsBmp(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path)
{
    UINT w = 0, h = 0;
    std::vector<uint8_t> px = ReadbackBgra(device, ctx, tex, &w, &h);
    return SaveBmp32(path, px.data(), w, h);
}

Nv12Image ReadbackNv12(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT subresource)
{
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_NV12) throw std::runtime_error("ReadbackNv12: texture is not NV12");

    // 解碼器的輸出是 texture array 且帶 BIND_DECODER，不能直接 Map；
    // 用 CopySubresourceRegion 把「第 subresource 張」拷到單張 staging texture
    ComPtr<ID3D11Texture2D> staging =
        CreateTexture(device, d.Width, d.Height, DXGI_FORMAT_NV12, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex, subresource, nullptr);

    D3D11_MAPPED_SUBRESOURCE m;
    CHECK_HR(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m));
    Nv12Image img;
    img.pitch = m.RowPitch;
    img.codedWidth = d.Width;
    img.codedHeight = d.Height;
    // D3D11 NV12 Map 後：Y plane 在前（Height 行），UV plane 緊接在 pData + RowPitch*Height
    size_t bytes = (size_t)m.RowPitch * d.Height * 3 / 2;
    img.data.assign((const uint8_t*)m.pData, (const uint8_t*)m.pData + bytes);
    ctx->Unmap(staging.Get(), 0);
    return img;
}

bool SaveNv12AsBmp(const Nv12Image& img, UINT width, UINT height, YuvMatrix m, YuvRange r, const std::string& path)
{
    std::vector<uint8_t> bgra((size_t)width * height * 4);
    Nv12ToBgra(img.YPlane(), img.UVPlane(), img.pitch, width, height, m, r, bgra.data());
    return SaveBmp32(path, bgra.data(), width, height);
}

double WaitForGpu(ID3D11Device* device, ID3D11DeviceContext* ctx)
{
    D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> q;
    CHECK_HR(device->CreateQuery(&qd, &q));
    int64_t t0 = QpcNow();
    ctx->End(q.Get());
    BOOL done = FALSE;
    while (ctx->GetData(q.Get(), &done, sizeof(done), 0) == S_FALSE) YieldProcessor();
    return QpcToMs(QpcNow() - t0);
}
