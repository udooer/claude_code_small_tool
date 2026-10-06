// nv12_pool.h — 送進 encoder 的 NV12 texture pool（Lab 4 / Lab 6）
//
// 為什麼需要 pool（勘誤 M9）：async encoder 收到 sample 之後不會馬上讀完。
// 如果每一幀都 Blt 到「同一張」texture，下一幀可能覆蓋 encoder 還沒讀的資料 -> 畫面跳格/重複。
// 作法：準備多張 texture，每張預先包好一個 IMFSample；encoder 還持有 sample 時（refcount > 1）就不重用。
// （更正式的作法是 IMFTrackedSample / MFCreateVideoSampleAllocatorEx，會在 sample 被釋放時通知你；
//  這裡用 refcount 是為了讓機制一目了然。若遇到只持有 buffer 不持有 sample 的驅動，改用 tracked sample。）
#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

#include "d3d_util.h"
#include "hr.h"
#include "mf_util.h"

using Microsoft::WRL::ComPtr;

class Nv12SamplePool {
public:
    struct Slot {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<IMFSample> sample; // 底層直接指向 texture（MFCreateDXGISurfaceBuffer），沒有 CPU 複製
    };

    void Init(ID3D11Device* device, UINT width, UINT height, UINT count, UINT bindFlags = D3D11_BIND_RENDER_TARGET)
    {
        slots_.clear();
        next_ = 0;
        for (UINT i = 0; i < count; ++i) {
            Slot s;
            s.texture = CreateTexture(device, width, height, DXGI_FORMAT_NV12, bindFlags);
            ComPtr<IMFMediaBuffer> buf;
            CHECK_HR(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), s.texture.Get(), 0, FALSE, &buf));
            // 有些 encoder 會檢查 current length，為 0 時拒收（勘誤 M10）
            ComPtr<IMF2DBuffer> b2d;
            DWORD len = 0;
            if (SUCCEEDED(buf.As(&b2d)) && SUCCEEDED(b2d->GetContiguousLength(&len))) CHECK_HR(buf->SetCurrentLength(len));
            CHECK_HR(MFCreateSample(&s.sample));
            CHECK_HR(s.sample->AddBuffer(buf.Get()));
            slots_.push_back(s);
        }
    }

    // 找一個 encoder 已經不再使用的 slot；全部都在用時最多等 timeoutMs
    Slot* Acquire(DWORD timeoutMs = 200)
    {
        DWORD start = GetTickCount();
        for (;;) {
            for (size_t k = 0; k < slots_.size(); ++k) {
                Slot& s = slots_[(next_ + k) % slots_.size()];
                if (PeekRefCount(s.sample.Get()) == 1) { // 只剩我們自己持有
                    next_ = (next_ + k + 1) % slots_.size();
                    return &s;
                }
            }
            if (GetTickCount() - start > timeoutMs) return nullptr;
            Sleep(1);
        }
    }

private:
    std::vector<Slot> slots_;
    size_t next_ = 0;
};

// 軟體 encoder fallback：它不吃 GPU texture，只好讀回 CPU 做成記憶體 sample。
// 這就是「軟體 fallback 時無法做到全程 GPU」的那一次 readback，刻意寫得很明顯。
inline ComPtr<IMFSample> MakeCpuNv12Sample(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12,
                                           UINT width, UINT height)
{
    Nv12Image img = ReadbackNv12(device, ctx, nv12, 0);
    const DWORD size = width * height * 3 / 2;
    ComPtr<IMFMediaBuffer> buf;
    CHECK_HR(MFCreateMemoryBuffer(size, &buf));
    BYTE* p = nullptr;
    CHECK_HR(buf->Lock(&p, nullptr, nullptr));
    for (UINT y = 0; y < height; ++y) memcpy(p + (size_t)y * width, img.YPlane() + (size_t)y * img.pitch, width);
    for (UINT y = 0; y < height / 2; ++y)
        memcpy(p + (size_t)width * height + (size_t)y * width, img.UVPlane() + (size_t)y * img.pitch, width);
    buf->Unlock();
    CHECK_HR(buf->SetCurrentLength(size));
    ComPtr<IMFSample> s;
    CHECK_HR(MFCreateSample(&s));
    CHECK_HR(s->AddBuffer(buf.Get()));
    return s;
}
