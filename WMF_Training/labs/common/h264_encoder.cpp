#include "h264_encoder.h"

#include <codecapi.h>
#include <strmif.h> // ICodecAPI
#include <wmcodecdsp.h> // CLSID_CMSH264EncoderMFT
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <vector>

#include "hr.h"
#include "mf_util.h"

namespace {

bool g_quiet = false; // EncoderConfig::quiet（只影響 log）

void SetU4(ICodecAPI* api, const GUID& key, UINT32 value, const char* name)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr) && !g_quiet) std::fprintf(stderr, "  [warn] CODECAPI %s = %u not supported (%s)\n", name, value, HrToString(hr).c_str());
}

void SetBool(ICodecAPI* api, const GUID& key, bool value, const char* name)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    HRESULT hr = api->SetValue(&key, &v);
    if (FAILED(hr) && !g_quiet) std::fprintf(stderr, "  [warn] CODECAPI %s not supported (%s)\n", name, HrToString(hr).c_str());
}

enum class PullResult { GotOutput, NeedMoreInput, StreamChanged };

// MFT_ENUM_ADAPTER_LUID（mftransform.h；舊版/mingw 標頭沒有，自己定義）
const GUID kMftEnumAdapterLuid = { 0x1d39518c, 0xe220, 0x4da8, { 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1 } };

bool LuidEqual(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// device manager 裡的 D3D11 device 在哪一張 GPU 上
bool GetDeviceManagerLuid(IMFDXGIDeviceManager* mgr, LUID* luid)
{
    HANDLE h = nullptr;
    if (FAILED(mgr->OpenDeviceHandle(&h))) return false;
    ComPtr<ID3D11Device> dev;
    HRESULT hr = mgr->GetVideoService(h, IID_PPV_ARGS(&dev));
    mgr->CloseDeviceHandle(h);
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc;
    if (FAILED(dev.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc))) return false;
    *luid = desc.AdapterLuid;
    return true;
}

// 硬體 MFT 屬於哪一張 GPU（屬性可能是 blob 或 UINT64，兩種都試）
bool GetActivateLuid(IMFActivate* act, LUID* luid)
{
    UINT32 size = 0;
    if (SUCCEEDED(act->GetBlob(kMftEnumAdapterLuid, (UINT8*)luid, sizeof(LUID), &size)) && size == sizeof(LUID)) return true;
    UINT64 v = 0;
    if (SUCCEEDED(act->GetUINT64(kMftEnumAdapterLuid, &v))) {
        luid->LowPart = (DWORD)(v & 0xFFFFFFFF);
        luid->HighPart = (LONG)(v >> 32);
        return true;
    }
    return false;
}

} // namespace

struct H264Encoder::Impl : std::enable_shared_from_this<H264Encoder::Impl> {
    EncoderConfig cfg;
    OutputFn onOutput;

    ComPtr<IMFTransform> mft;
    ComPtr<ICodecAPI> codecApi;
    ComPtr<IMFMediaEventGenerator> events;
    DWORD inId = 0, outId = 0;
    bool hardware = false, async = false, d3d = false;
    std::string name;

    // async 狀態
    std::mutex mu;
    std::condition_variable cv;
    int needInput = 0;          // 收到但還沒用掉的 METransformNeedInput 數
    bool drainComplete = false;
    bool eventLoopRunning = false;
    bool shuttingDown = false;
    HRESULT asyncError = S_OK;
    std::string lastError;

    void ArmEvent()
    {
        auto self = shared_from_this(); // callback 持有 Impl，避免 Impl 先被釋放
        auto* cb = new LambdaAsyncCallback([self](IMFAsyncResult* r) { self->OnEvent(r); });
        HRESULT hr = events->BeginGetEvent(cb, nullptr);
        cb->Release(); // MF 自己會 AddRef
        if (FAILED(hr)) {
            std::lock_guard<std::mutex> lk(mu);
            if (!shuttingDown) asyncError = hr;
            eventLoopRunning = false;
            cv.notify_all();
        }
    }

    // 處理一個 async MFT 事件（呼叫端必須持有 mu）
    void HandleEvent(IMFMediaEvent* ev)
    {
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        ev->GetType(&type);
        ev->GetStatus(&status);
        if (FAILED(status)) {
            asyncError = status;
            if (!cfg.quiet) std::fprintf(stderr, "  [event] type %lu carries failure status %s\n", (unsigned long)type, HrToString(status).c_str());
            return;
        }
        switch (type) {
        case METransformNeedInput:
            ++needInput;
            break;
        case METransformHaveOutput:
            try {
                while (PullOneOutput() == PullResult::StreamChanged) {}
            } catch (const HrError& e) {
                asyncError = e.hr();
                lastError = e.what();
                if (!cfg.quiet) std::fprintf(stderr, "%s\n", e.what());
            }
            break;
        case METransformDrainComplete:
            drainComplete = true;
            break;
        case MEError:
            asyncError = E_FAIL;
            break;
        default:
            break; // METransformMarker 等
        }
    }

    // blockingEvents 模式：在呼叫端 thread 取一個事件。noWait 時沒有事件就回傳 false。
    bool PumpEvent(bool noWait)
    {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events->GetEvent(noWait ? MF_EVENT_FLAG_NO_WAIT : 0, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) return false;
        if (FAILED(hr)) {
            asyncError = hr;
            return false;
        }
        HandleEvent(ev.Get());
        return true;
    }

    // blockingEvents 模式：等一個事件，最多 timeoutMs（避免壞掉的 encoder 讓程式永遠卡住）
    void WaitEvent(DWORD timeoutMs = 5000)
    {
        DWORD start = GetTickCount();
        while (!PumpEvent(true)) {
            if (FAILED(asyncError)) return;
            if (GetTickCount() - start > timeoutMs) {
                asyncError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                lastError = "timed out waiting for encoder event";
                return;
            }
            Sleep(1);
        }
    }

    // 在 MF 的 worker thread 上被呼叫
    void OnEvent(IMFAsyncResult* result)
    {
        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = events->EndGetEvent(result, &ev);
        std::unique_lock<std::mutex> lk(mu);
        if (FAILED(hr)) {
            if (!shuttingDown && hr != MF_E_SHUTDOWN) asyncError = hr;
            eventLoopRunning = false;
            cv.notify_all();
            return;
        }
        HandleEvent(ev.Get());
        cv.notify_all();
        if (shuttingDown || FAILED(asyncError)) {
            eventLoopRunning = false;
            return;
        }
        lk.unlock();
        ArmEvent(); // 一次 BeginGetEvent 只會收到一個事件，要重新登記
    }

    PullResult PullOneOutput()
    {
        MFT_OUTPUT_STREAM_INFO si{};
        CHECK_HR(mft->GetOutputStreamInfo(outId, &si));
        // 有些 MFT（大部分硬體 encoder）自己配置輸出 sample，有些要呼叫端配置
        const bool mftAllocates = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        ComPtr<IMFSample> own;
        if (!mftAllocates) {
            DWORD size = std::max<DWORD>(si.cbSize, cfg.width * cfg.height * 3 / 2);
            ComPtr<IMFMediaBuffer> buf;
            CHECK_HR(MFCreateMemoryBuffer(size, &buf)); // 輸出是 bitstream，本來就在 CPU，這不是 readback
            CHECK_HR(MFCreateSample(&own));
            CHECK_HR(own->AddBuffer(buf.Get()));
        }

        MFT_OUTPUT_DATA_BUFFER ob{};
        ob.dwStreamID = outId;
        ob.pSample = own.Get();
        DWORD status = 0;
        HRESULT hr = mft->ProcessOutput(0, 1, &ob, &status);
        if (ob.pEvents) ob.pEvents->Release(); // 容易漏掉的洩漏
        ComPtr<IMFSample> out;
        if (mftAllocates) out.Attach(ob.pSample); // MFT 給的 sample 由我們負責 Release
        else out = own;

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return PullResult::NeedMoreInput;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            CHECK_HR(mft->GetOutputAvailableType(outId, 0, &t));
            CHECK_HR(mft->SetOutputType(outId, t.Get(), 0));
            return PullResult::StreamChanged;
        }
        if (FAILED(hr)) {
            char what[160];
            std::snprintf(what, sizeof(what), "ProcessOutput (output stream flags=0x%lX cbSize=%lu, %s-allocated sample)",
                          (unsigned long)si.dwFlags, (unsigned long)si.cbSize, mftAllocates ? "MFT" : "caller");
            throw HrError(hr, what, __FILE__, __LINE__);
        }
        if (!out) return PullResult::GotOutput;

        EncodedFrame f;
        out->GetSampleTime(&f.sampleTime);
        f.keyframe = MFGetAttributeUINT32(out.Get(), MFSampleExtension_CleanPoint, FALSE) != 0;
        ComPtr<IMFMediaBuffer> buf;
        CHECK_HR(out->ConvertToContiguousBuffer(&buf));
        BYTE* p = nullptr;
        DWORD len = 0;
        CHECK_HR(buf->Lock(&p, nullptr, &len));
        f.data.assign(p, p + len);
        buf->Unlock();
        if (onOutput && !f.data.empty()) onOutput(std::move(f));
        return PullResult::GotOutput;
    }

    void ThrowIfAsyncFailed()
    {
        if (FAILED(asyncError))
            throw HrError(asyncError, lastError.empty() ? "async encoder event" : "async encoder event (see first error above)", __FILE__, __LINE__);
    }
};

H264Encoder::H264Encoder() = default;
H264Encoder::~H264Encoder() { Shutdown(); }

bool H264Encoder::IsHardware() const { return impl_ && impl_->hardware; }
bool H264Encoder::IsAsync() const { return impl_ && impl_->async; }
bool H264Encoder::UsesD3D() const { return impl_ && impl_->d3d; }
std::string H264Encoder::Name() const { return impl_ ? impl_->name : ""; }

void H264Encoder::Init(const EncoderConfig& cfg, IMFDXGIDeviceManager* deviceManager, OutputFn onOutput)
{
    Shutdown();
    impl_ = std::make_shared<Impl>();
    Impl& m = *impl_;
    m.cfg = cfg;
    m.onOutput = std::move(onOutput);
    g_quiet = cfg.quiet;

    // ---- 1. 找 encoder：硬體優先，沒有就退回微軟軟體 encoder ----
    if (cfg.preferHardware) {
        MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 };
        MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                               &in, &out, &acts, &count);
        if (SUCCEEDED(hr)) {
            // 混合顯卡（例如 Intel 內顯 + NVIDIA 獨顯）會列出多個硬體 encoder。
            // encoder 必須和我們的 D3D11 device 在同一張 GPU，GPU texture 才能直接餵進去。
            LUID devLuid{};
            bool haveDevLuid = deviceManager && GetDeviceManagerLuid(deviceManager, &devLuid);
            std::vector<UINT32> order;
            for (UINT32 i = 0; i < count; ++i) {
                LUID l{};
                if (haveDevLuid && GetActivateLuid(acts[i], &l) && LuidEqual(l, devLuid)) order.insert(order.begin(), i);
                else order.push_back(i);
            }
            if (cfg.hwIndex >= 0) { // 除錯：指定第幾個
                order.clear();
                if ((UINT32)cfg.hwIndex < count) order.push_back((UINT32)cfg.hwIndex);
            }
            if (count > 1 && !cfg.quiet) {
                std::printf("  Hardware H.264 encoders found: %u\n", count);
                for (UINT32 i = 0; i < count; ++i) {
                    WCHAR* n = nullptr;
                    UINT32 nl = 0;
                    acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &n, &nl);
                    LUID l{};
                    bool same = haveDevLuid && GetActivateLuid(acts[i], &l) && LuidEqual(l, devLuid);
                    std::printf("    [%u] %s%s\n", i, n ? Narrow(n).c_str() : "(unnamed)", same ? "  <- same GPU as our D3D11 device" : "");
                    CoTaskMemFree(n);
                }
            }
            for (UINT32 k = 0; k < order.size() && !m.mft; ++k) {
                const UINT32 i = order[k];
                WCHAR* fname = nullptr;
                UINT32 len = 0;
                acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &fname, &len);
                std::wstring wname = fname ? fname : L"(unnamed hardware MFT)";
                CoTaskMemFree(fname);
                HRESULT ahr = acts[i]->ActivateObject(IID_PPV_ARGS(&m.mft));
                if (SUCCEEDED(ahr)) {
                    m.hardware = true;
                    m.name = Narrow(wname);
                } else {
                    std::fprintf(stderr, "  [warn] activating %s failed: %s\n", Narrow(wname).c_str(), HrToString(ahr).c_str());
                }
            }
            for (UINT32 i = 0; i < count; ++i) acts[i]->Release(); // 陣列裡每一個都要 Release
            CoTaskMemFree(acts);                                   // 陣列本身用 CoTaskMemFree
        }
        if (!m.mft && cfg.hwIndex >= 0) throw std::runtime_error("requested hardware encoder index not available");
        if (!m.mft) std::printf("  No hardware H.264 encoder found -> falling back to software encoder\n");
    }
    if (!m.mft) {
        // 注意：CLSID_CMSH264EncoderMFT 是「軟體」encoder（勘誤 E1）
        CHECK_HR(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m.mft)));
        m.name = "Microsoft H264 Encoder MFT (software)";
    }

    // ---- 2. async MFT 要先解鎖（勘誤 E2：是 SetUINT32，不是 SetUnknown） ----
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m.mft->GetAttributes(&attrs))) {
        m.async = MFGetAttributeUINT32(attrs.Get(), MF_TRANSFORM_ASYNC, FALSE) != 0;
        if (m.async) CHECK_HR(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));
    }

    // 有些 MFT 的 stream ID 不是 0
    DWORD inId = 0, outId = 0;
    if (SUCCEEDED(m.mft->GetStreamIDs(1, &inId, 1, &outId))) {
        m.inId = inId;
        m.outId = outId;
    }

    // ---- 3. 綁 D3D11 device：GPU texture 直接進 encoder，沒有 CPU readback ----
    if (deviceManager && attrs && MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, FALSE)) {
        HRESULT hr = m.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager);
        m.d3d = SUCCEEDED(hr);
        if (!m.d3d && !cfg.quiet) std::fprintf(stderr, "  [warn] SET_D3D_MANAGER failed: %s\n", HrToString(hr).c_str());
    }

    // ---- 4. 編碼參數（部分屬性要在 SetOutputType 之前設才生效） ----
    // 用 IID_ICodecAPI 而不是 __uuidof：mingw 的標頭沒有替 ICodecAPI 宣告 uuid
    if (SUCCEEDED(m.mft->QueryInterface(IID_ICodecAPI, reinterpret_cast<void**>(m.codecApi.ReleaseAndGetAddressOf()))) &&
        cfg.applyCodecApi) {
        SetU4(m.codecApi.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, "RateControlMode=CBR");
        SetU4(m.codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, cfg.bitrate, "MeanBitRate");
        if (cfg.lowLatency) {
            SetBool(m.codecApi.Get(), CODECAPI_AVLowLatencyMode, true, "LowLatencyMode");
            SetU4(m.codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0, "BPictureCount");
        }
        if (cfg.gopSize) SetU4(m.codecApi.Get(), CODECAPI_AVEncMPVGOPSize, cfg.gopSize, "GOPSize");
    }

    // ---- 5. 輸出 type 先設（encoder 的可用輸入格式常常取決於輸出設定） ----
    ComPtr<IMFMediaType> outType;
    CHECK_HR(MFCreateMediaType(&outType));
    CHECK_HR(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CHECK_HR(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    CHECK_HR(outType->SetUINT32(MF_MT_AVG_BITRATE, cfg.bitrate));
    CHECK_HR(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height));
    CHECK_HR(MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1));
    CHECK_HR(MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    CHECK_HR(outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CHECK_HR(outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main));
    CHECK_HR(m.mft->SetOutputType(m.outId, outType.Get(), 0));

    // ---- 6. 輸入 type：從 encoder 列出的可用 type 中找 NV12，再補上尺寸/幀率 ----
    ComPtr<IMFMediaType> inType;
    for (DWORD k = 0;; ++k) {
        ComPtr<IMFMediaType> t;
        if (FAILED(m.mft->GetInputAvailableType(m.inId, k, &t))) break;
        GUID sub{};
        if (SUCCEEDED(t->GetGUID(MF_MT_SUBTYPE, &sub)) && sub == MFVideoFormat_NV12) {
            inType = t;
            break;
        }
    }
    if (!inType) {
        CHECK_HR(MFCreateMediaType(&inType));
        CHECK_HR(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
        CHECK_HR(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    }
    CHECK_HR(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height));
    CHECK_HR(MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, cfg.fps, 1));
    CHECK_HR(MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    CHECK_HR(inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    // 告訴 encoder 輸入的色彩空間（和 VideoScaler 的輸出設定一致：BT.709 studio）
    if (cfg.inputColorAttrs) {
        CHECK_HR(inType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
        CHECK_HR(inType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
    }
    CHECK_HR(m.mft->SetInputType(m.inId, inType.Get(), 0));

    // ---- 7. 開始串流 ----
    if (m.async) {
        CHECK_HR(m.mft.As(&m.events));
        m.eventLoopRunning = true;
        if (!cfg.blockingEvents) m.ArmEvent();
    }
    CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
}

void H264Encoder::Encode(IMFSample* sample)
{
    Impl& m = *impl_;
    if (m.async && m.cfg.blockingEvents) {
        std::lock_guard<std::mutex> lk(m.mu);
        while (m.needInput == 0 && SUCCEEDED(m.asyncError)) m.WaitEvent(); // 等到 NeedInput
        m.ThrowIfAsyncFailed();
        --m.needInput;
        CHECK_HR(m.mft->ProcessInput(m.inId, sample, 0));
        while (SUCCEEDED(m.asyncError) && m.PumpEvent(true)) {} // 順手把已經好的輸出拿走
        m.ThrowIfAsyncFailed();
        return;
    }
    if (m.async) {
        std::unique_lock<std::mutex> lk(m.mu);
        // async：一定要等到 METransformNeedInput 才能送（否則 MF_E_NOTACCEPTING）
        if (!m.cv.wait_for(lk, std::chrono::seconds(5),
                           [&] { return m.needInput > 0 || FAILED(m.asyncError) || !m.eventLoopRunning; }))
            throw std::runtime_error("timed out waiting for METransformNeedInput");
        m.ThrowIfAsyncFailed();
        if (m.needInput == 0) throw std::runtime_error("encoder event loop stopped");
        --m.needInput;
        CHECK_HR(m.mft->ProcessInput(m.inId, sample, 0));
        return;
    }
    // sync：送一張，然後把拿得到的輸出全部拿完
    HRESULT hr = m.mft->ProcessInput(m.inId, sample, 0);
    if (hr == MF_E_NOTACCEPTING) {
        while (m.PullOneOutput() != PullResult::NeedMoreInput) {}
        hr = m.mft->ProcessInput(m.inId, sample, 0);
    }
    CHECK_HR(hr);
    while (m.PullOneOutput() != PullResult::NeedMoreInput) {}
}

void H264Encoder::ForceKeyFrame()
{
    if (impl_ && impl_->codecApi)
        SetU4(impl_->codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1, "ForceKeyFrame");
}

void H264Encoder::Drain()
{
    if (!impl_ || !impl_->mft) return;
    Impl& m = *impl_;
    if (m.async && m.cfg.blockingEvents) {
        std::lock_guard<std::mutex> lk(m.mu);
        m.drainComplete = false;
        CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, m.inId));
        CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m.inId));
        while (!m.drainComplete && SUCCEEDED(m.asyncError)) m.WaitEvent();
        m.ThrowIfAsyncFailed();
        return;
    }
    if (m.async) {
        std::unique_lock<std::mutex> lk(m.mu);
        m.drainComplete = false;
        CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, m.inId));
        CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m.inId));
        bool ok = m.cv.wait_for(lk, std::chrono::seconds(10),
                                [&] { return m.drainComplete || FAILED(m.asyncError) || !m.eventLoopRunning; });
        if (!ok) std::fprintf(stderr, "  [warn] drain timed out\n");
        m.ThrowIfAsyncFailed();
        return;
    }
    CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, m.inId));
    CHECK_HR(m.mft->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, m.inId));
    while (m.PullOneOutput() != PullResult::NeedMoreInput) {}
}

void H264Encoder::Shutdown()
{
    if (!impl_) return;
    std::shared_ptr<Impl> m = std::move(impl_);
    {
        std::lock_guard<std::mutex> lk(m->mu);
        m->shuttingDown = true;
        m->onOutput = nullptr; // Shutdown 之後不再呼叫 callback
    }
    if (m->mft) {
        m->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (m->d3d) m->mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0); // 解除綁定
        ComPtr<IMFShutdown> sd;
        if (m->async && SUCCEEDED(m->mft.As(&sd))) sd->Shutdown(); // async MFT 必須支援 IMFShutdown
    }
    if (m->async && !m->cfg.blockingEvents) {
        std::unique_lock<std::mutex> lk(m->mu);
        m->cv.wait_for(lk, std::chrono::seconds(2), [&] { return !m->eventLoopRunning; });
    }
    // 若仍有 pending callback，它持有 shared_ptr<Impl>，Impl 會在 callback 結束後才釋放
}
