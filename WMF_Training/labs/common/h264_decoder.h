// h264_decoder.h — Microsoft H.264 Decoder MFT 封裝（Lab 5 / Lab 6）
//
// 重點：
//   * CLSID_CMSH264DecoderMFT 是 sync MFT；設了 D3D manager 後內部走 DXVA 硬體解碼（勘誤 E6/E7）
//   * 解析到 SPS 後 ProcessOutput 會回 MF_E_TRANSFORM_STREAM_CHANGE，必須重新設定輸出 type（勘誤 E8）
//   * D3D 模式下輸出是 texture array + subresource index；用完 sample 一定要 Release（勘誤 E9）
//   * 1080p 的實際編碼高度是 1088，有效區域看 MF_MT_MINIMUM_DISPLAY_APERTURE（勘誤 M6）
#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

using Microsoft::WRL::ComPtr;

struct DecodedFrame {
    // D3D 模式：texture（通常是 array）+ 第幾張
    ID3D11Texture2D* texture = nullptr;
    UINT subresource = 0;
    // 軟體模式：CPU 上的 NV12（Y plane 後接 UV plane，UV 偏移 = cpuPitch * codedHeight）
    const uint8_t* cpuData = nullptr;
    UINT cpuPitch = 0;

    UINT codedWidth = 0, codedHeight = 0;     // 例如 1920x1088
    UINT displayWidth = 0, displayHeight = 0; // 例如 1920x1080
    int64_t sampleTime = 0;
};

class H264Decoder {
public:
    using FrameFn = std::function<void(const DecodedFrame&)>;

    // deviceManager 為 nullptr 時走軟體解碼、輸出在 CPU 記憶體
    void Init(IMFDXGIDeviceManager* deviceManager, bool lowLatency);
    bool UsesD3D() const { return d3d_; }

    // data：一個或多個完整的 NAL（Annex B，帶 start code），最好是一整個 access unit
    void Decode(const uint8_t* data, size_t size, int64_t sampleTime, const FrameFn& onFrame);
    void Drain(const FrameFn& onFrame);
    void Shutdown();
    ~H264Decoder() { Shutdown(); }

    // MF_E_TRANSFORM_STREAM_CHANGE 發生的次數（第一次是看到 SPS，之後是解析度改變）
    int StreamChanges() const { return streamChanges_; }

private:
    bool PullOneOutput(const FrameFn& onFrame); // false = 需要更多輸入
    void OnStreamChange();

    ComPtr<IMFTransform> mft_;
    bool d3d_ = false;
    bool outputTypeSet_ = false;
    UINT codedW_ = 0, codedH_ = 0, dispW_ = 0, dispH_ = 0;
    int streamChanges_ = 0;
};
