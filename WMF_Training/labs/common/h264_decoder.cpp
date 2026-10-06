#include "h264_decoder.h"

#include <codecapi.h>
#include <wmcodecdsp.h> // CLSID_CMSH264DecoderMFT

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "hr.h"

void H264Decoder::Init(IMFDXGIDeviceManager* deviceManager, bool lowLatency)
{
    Shutdown();
    // 不用 MFTEnumEx(HARDWARE)：微軟的 decoder 註冊成軟體 MFT，很多機器上 HARDWARE 列舉結果是 0 個（勘誤 E7）
    CHECK_HR(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mft_)));

    ComPtr<IMFAttributes> attrs;
    CHECK_HR(mft_->GetAttributes(&attrs));
    if (MFGetAttributeUINT32(attrs.Get(), MF_TRANSFORM_ASYNC, FALSE))
        throw std::runtime_error("unexpected async decoder"); // MS decoder 是 sync

    if (lowLatency) // 不要為了 reorder 而 hold 住畫面
        WARN_HR(attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE));

    if (deviceManager && MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE)) {
        HRESULT hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager);
        d3d_ = SUCCEEDED(hr);
        if (!d3d_) std::fprintf(stderr, "  [warn] decoder SET_D3D_MANAGER failed (%s) -> software decode\n", HrToString(hr).c_str());
    }

    ComPtr<IMFMediaType> inType;
    CHECK_HR(MFCreateMediaType(&inType));
    CHECK_HR(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    CHECK_HR(mft_->SetInputType(0, inType.Get(), 0));

    // 先設一個 NV12 輸出；真正的尺寸要等解析到 SPS 後的 STREAM_CHANGE 才知道。
    // 有些版本在還沒看到資料前不提供輸出 type，那就等 STREAM_CHANGE 再設。
    try {
        OnStreamChange();
    } catch (const std::exception&) {
        outputTypeSet_ = false;
    }
    streamChanges_ = 1; // 之後真正的 STREAM_CHANGE 會從 2 開始，StreamChanges() 會扣掉這一次

    CHECK_HR(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    CHECK_HR(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

void H264Decoder::OnStreamChange()
{
    ++streamChanges_;
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> t;
        if (FAILED(mft_->GetOutputAvailableType(0, i, &t))) break;
        GUID sub{};
        t->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            chosen = t;
            break;
        }
    }
    if (!chosen) throw std::runtime_error("decoder offers no NV12 output type");
    CHECK_HR(mft_->SetOutputType(0, chosen.Get(), 0));
    outputTypeSet_ = true;

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(chosen.Get(), MF_MT_FRAME_SIZE, &w, &h);
    codedW_ = w;
    codedH_ = h;
    dispW_ = w;
    dispH_ = h;
    MFVideoArea area{};
    if (SUCCEEDED(chosen->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), nullptr))) {
        dispW_ = (UINT)area.Area.cx;
        dispH_ = (UINT)area.Area.cy;
    }
    if (streamChanges_ > 1) // 第 1 次是 Init 時的預設 type（尺寸只是佔位值），不印
        std::printf("  [decoder] output type: NV12 coded %ux%u, display %ux%u\n", codedW_, codedH_, dispW_, dispH_);
}

void H264Decoder::Decode(const uint8_t* data, size_t size, int64_t sampleTime, const FrameFn& onFrame)
{
    ComPtr<IMFMediaBuffer> buf;
    CHECK_HR(MFCreateMemoryBuffer((DWORD)size, &buf));
    BYTE* p = nullptr;
    CHECK_HR(buf->Lock(&p, nullptr, nullptr));
    std::memcpy(p, data, size);
    buf->Unlock();
    CHECK_HR(buf->SetCurrentLength((DWORD)size));

    ComPtr<IMFSample> sample;
    CHECK_HR(MFCreateSample(&sample));
    CHECK_HR(sample->AddBuffer(buf.Get()));
    CHECK_HR(sample->SetSampleTime(sampleTime));

    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { // 輸出還沒拿完
        while (PullOneOutput(onFrame)) {}
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    CHECK_HR(hr);
    while (PullOneOutput(onFrame)) {}
}

void H264Decoder::Drain(const FrameFn& onFrame)
{
    if (!mft_) return;
    CHECK_HR(mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
    CHECK_HR(mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0)); // 不 drain 會少最後幾張
    while (PullOneOutput(onFrame)) {}
}

bool H264Decoder::PullOneOutput(const FrameFn& onFrame)
{
    MFT_OUTPUT_STREAM_INFO si{};
    CHECK_HR(mft_->GetOutputStreamInfo(0, &si));
    const bool mftAllocates = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

    ComPtr<IMFSample> own;
    if (!mftAllocates) { // 軟體模式：自己配置 CPU buffer
        ComPtr<IMFMediaBuffer> b;
        CHECK_HR(MFCreateMemoryBuffer(si.cbSize ? si.cbSize : 4096 * 2304 * 3 / 2, &b));
        CHECK_HR(MFCreateSample(&own));
        CHECK_HR(own->AddBuffer(b.Get()));
    }

    MFT_OUTPUT_DATA_BUFFER ob{};
    ob.dwStreamID = 0;
    ob.pSample = own.Get();
    DWORD status = 0;
    HRESULT hr = mft_->ProcessOutput(0, 1, &ob, &status);
    if (ob.pEvents) ob.pEvents->Release();
    ComPtr<IMFSample> out;
    if (mftAllocates) out.Attach(ob.pSample); // 一定要 Release，否則 decoder 的 surface pool 會被用光而卡死
    else out = own;

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) { // 看到 SPS / 解析度改變：重新協商
        OnStreamChange();
        return true;
    }
    CHECK_HR(hr);
    if (!out) return true;

    DecodedFrame f;
    f.codedWidth = codedW_;
    f.codedHeight = codedH_;
    f.displayWidth = dispW_;
    f.displayHeight = dispH_;
    out->GetSampleTime(&f.sampleTime);

    ComPtr<IMFMediaBuffer> buf;
    CHECK_HR(out->GetBufferByIndex(0, &buf));
    ComPtr<IMFDXGIBuffer> dxgiBuf;
    if (SUCCEEDED(buf.As(&dxgiBuf))) {
        ComPtr<ID3D11Texture2D> tex;
        CHECK_HR(dxgiBuf->GetResource(IID_PPV_ARGS(&tex)));
        CHECK_HR(dxgiBuf->GetSubresourceIndex(&f.subresource)); // 不要假設永遠是 0
        f.texture = tex.Get();
        if (onFrame) onFrame(f);
        return true;
    }

    // 軟體模式：Lock2D 才拿得到正確的 pitch
    ComPtr<IMF2DBuffer> b2d;
    if (SUCCEEDED(buf.As(&b2d))) {
        BYTE* p = nullptr;
        LONG pitch = 0;
        CHECK_HR(b2d->Lock2D(&p, &pitch));
        f.cpuData = p;
        f.cpuPitch = (UINT)pitch;
        if (onFrame) onFrame(f);
        b2d->Unlock2D();
    } else {
        BYTE* p = nullptr;
        CHECK_HR(buf->Lock(&p, nullptr, nullptr));
        f.cpuData = p;
        f.cpuPitch = codedW_;
        if (onFrame) onFrame(f);
        buf->Unlock();
    }
    return true;
}

void H264Decoder::Shutdown()
{
    if (!mft_) return;
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    if (d3d_) mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    mft_.Reset();
    d3d_ = false;
}
