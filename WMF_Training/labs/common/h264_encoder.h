// h264_encoder.h — H.264 Encoder MFT 封裝（Lab 4 / Lab 6）
//
// 同時支援兩種 MFT：
//   * 硬體 encoder（顯卡廠商提供，幾乎都是 async MFT）：事件驅動，
//     收到 METransformNeedInput 才能 ProcessInput，收到 METransformHaveOutput 才能 ProcessOutput。
//   * 微軟軟體 encoder CLSID_CMSH264EncoderMFT（sync MFT）：自己輪詢 ProcessInput/ProcessOutput。
// 對外介面一致：Encode(sample) 送一張、輸出透過 callback 交出；Drain() 把剩下的全部吐出。
#pragma once

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

struct EncoderConfig {
    UINT width = 1280, height = 720; // 必須是偶數
    UINT fps = 30;
    UINT bitrate = 8'000'000;
    UINT gopSize = 0;       // 0 = 讓 encoder 決定；即時串流建議 fps*2 左右
    bool lowLatency = true; // CODECAPI_AVLowLatencyMode + 不用 B-frame
    bool preferHardware = true;
};

struct EncodedFrame {
    std::vector<uint8_t> data; // Annex B（帶 start code）
    int64_t sampleTime = 0;    // 100ns，等於輸入 sample 的時間
    bool keyframe = false;
};

class H264Encoder {
public:
    using OutputFn = std::function<void(EncodedFrame&&)>;

    H264Encoder();
    ~H264Encoder();
    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // deviceManager：硬體 encoder 用來直接吃 GPU texture；nullptr = 不綁 GPU
    // onOutput：async 時會在 MF 的 worker thread 被呼叫，請自行注意執行緒安全
    void Init(const EncoderConfig& cfg, IMFDXGIDeviceManager* deviceManager, OutputFn onOutput);

    // 送一張 NV12 sample（必須已設好 SampleTime / SampleDuration）。
    // async 時會阻塞到 encoder 要求輸入為止。
    void Encode(IMFSample* sample);

    // 下一張輸出強制為 IDR（例如 receiver 剛連上、或丟包需要重新同步）
    void ForceKeyFrame();

    // END_OF_STREAM + DRAIN，阻塞到所有輸出都交給 callback
    void Drain();

    // 釋放 MFT。之後不會再呼叫 callback。
    void Shutdown();

    bool IsHardware() const;
    bool IsAsync() const;
    bool UsesD3D() const; // false 時 MF 會把 GPU texture 讀回 CPU 給 encoder（隱形 readback）
    std::string Name() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};
