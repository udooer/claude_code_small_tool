// Lab 6 — Sender：擷取 -> 縮放/轉 NV12 -> H.264 編碼 -> TCP 送出
//
//   sender.exe [--host 127.0.0.1] [--port 5000] [--output 0] [--fps 60] [--res 1080|720|480]
//              [--bitrate 8000000] [--software] [--throttle-mbps N]
//   --throttle-mbps：模擬慢速網路（send thread 依頻寬 sleep），用來觀察背壓與丟幀策略
//   執行中按 + / - 切換解析度，ESC 結束。請先啟動 receiver.exe。
//
// 架構（兩條 thread）：
//   main thread : DDA Acquire -> VideoProcessorBlt(GPU) -> Encoder.Encode(GPU texture)
//   encoder 輸出 callback（async 時在 MF thread）: 加上 header，丟進 send queue
//   send thread : 從 queue 取出 -> SendAll()；失敗代表 receiver 斷線 -> 通知全部結束
//
// 整條路徑上唯一落到 CPU 的資料是 encoder 輸出的 bitstream。
#include <conio.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "blocking_queue.h"
#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "framing.h"
#include "h264_encoder.h"
#include "hr.h"
#include "mf_util.h"
#include "net.h"
#include "nv12_pool.h"
#include "timer.h"
#include "video_scaler.h"

namespace {

struct Resolution {
    const char* name;
    UINT w, h;
};
const Resolution kResolutions[] = { { "1080p", 1920, 1080 }, { "720p", 1280, 720 }, { "480p", 854, 480 } };
constexpr int kNumRes = 3;

// send queue 超過這個深度就在「編碼之前」丟掉原始畫面：
// 不能丟已編碼的 P-frame，否則後面所有畫面都參考錯誤直到下一個 IDR。
constexpr size_t kSendQueueSoftLimit = 3;

SOCKET Connect(const std::string& host, const std::string& port)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) throw std::runtime_error("getaddrinfo failed");
    SOCKET s = INVALID_SOCKET;
    for (int attempt = 0; attempt < 10 && s == INVALID_SOCKET; ++attempt) {
        s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
            closesocket(s);
            s = INVALID_SOCKET;
            std::printf("  connect to %s:%s failed, retrying (is receiver.exe running?)\n", host.c_str(), port.c_str());
            Sleep(1000);
        }
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) throw std::runtime_error("could not connect to receiver");
    SetNoDelay(s);
    return s;
}

} // namespace

static int Run(Args& args)
{
    args.DeclareFlags({ "--software" });
    const std::string host = args.Get("--host", "127.0.0.1");
    const std::string port = args.Get("--port", "5000");
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    const UINT fps = (UINT)args.GetInt("--fps", 60);
    const UINT bitrate = (UINT)args.GetInt("--bitrate", 8'000'000);
    bool preferHw = !args.Has("--software");
    const double throttleMbps = std::atof(args.Get("--throttle-mbps", "0").c_str());
    const long resArg = args.GetInt("--res", 720);
    int resIdx = resArg >= 1080 ? 0 : resArg >= 720 ? 1 : 2;

    WsaScope wsa;
    D3DContext d3d = CreateD3DForOutput(outputIndex);
    ComPtr<IMFDXGIDeviceManager> devMgr = CreateDxgiDeviceManager(d3d.device.Get());

    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        return 2;
    }
    ComPtr<ID3D11Texture2D> latest = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    std::printf("Connecting to receiver %s:%s ...\n", host.c_str(), port.c_str());
    SOCKET sock = Connect(host, port);
    std::printf("Connected (TCP_NODELAY on).\n");

    std::atomic<bool> stop{ false };
    BlockingQueue<std::vector<uint8_t>> sendQueue;
    std::atomic<int64_t> bytesSent{ 0 };
    std::atomic<int> framesSent{ 0 };

    // ---- send thread：網路 I/O 不要卡住擷取/編碼 ----
    std::thread sender([&] {
        std::vector<uint8_t> msg;
        while (!stop) {
            if (!sendQueue.Pop(msg, std::chrono::milliseconds(100))) continue;
            if (!SendAll(sock, msg.data(), (int)msg.size())) {
                std::printf("\nReceiver disconnected (%s). Stopping.\n", WsaErrorString(WSAGetLastError()).c_str());
                stop = true;
                break;
            }
            bytesSent += (int64_t)msg.size();
            ++framesSent;
            if (throttleMbps > 0) // 模擬頻寬：送 N bits 要花 N / (Mbps*1e6) 秒
                Sleep((DWORD)(msg.size() * 8 / (throttleMbps * 1000)));
        }
    });

    // ---- encoder 輸出：對應回擷取時間，組 header，丟進 queue ----
    std::mutex metaMu;
    std::map<int64_t, int64_t> captureQpcBySampleTime; // sampleTime(100ns) -> capture QPC
    uint32_t frameIndex = 0;
    UINT curW = 0, curH = 0;
    Stat capToEnc;

    auto onEncoded = [&](EncodedFrame&& f) {
        int64_t now = QpcNow();
        std::lock_guard<std::mutex> lk(metaMu);
        framing::FrameHeader h;
        h.payloadSize = (uint32_t)f.data.size();
        h.frameIndex = frameIndex++;
        h.flags = f.keyframe ? (uint32_t)framing::kKeyframe : 0u;
        h.width = (uint16_t)curW;
        h.height = (uint16_t)curH;
        auto it = captureQpcBySampleTime.find(f.sampleTime);
        h.captureQpc = it != captureQpcBySampleTime.end() ? it->second : now;
        if (it != captureQpcBySampleTime.end()) captureQpcBySampleTime.erase(it);
        h.encodeDoneQpc = now;
        capToEnc.Add(QpcToMs(h.encodeDoneQpc - h.captureQpc)); // 延遲 ①

        std::vector<uint8_t> msg(framing::kHeaderSize + f.data.size());
        framing::Serialize(h, msg.data());
        std::copy(f.data.begin(), f.data.end(), msg.begin() + framing::kHeaderSize);
        sendQueue.Push(std::move(msg));
    };

    VideoScaler scaler;
    Nv12SamplePool pool;
    H264Encoder encoder;

    // 解析度切換：重建 scaler 輸出、texture pool、encoder（新 encoder 第一張必然是帶新 SPS 的 IDR）
    auto configure = [&](int idx) {
        encoder.Shutdown(); // 舊 encoder 還沒吐出的畫面直接丟掉即可
        const Resolution& r = kResolutions[idx];
        {
            std::lock_guard<std::mutex> lk(metaMu);
            curW = r.w;
            curH = r.h;
            captureQpcBySampleTime.clear();
        }
        ScalerConfig sc;
        sc.srcWidth = capture.Width();
        sc.srcHeight = capture.Height();
        sc.srcFormat = capture.Format();
        sc.dstWidth = r.w;
        sc.dstHeight = r.h;
        sc.dstFormat = DXGI_FORMAT_NV12;
        scaler.Init(d3d.device.Get(), sc);
        pool.Init(d3d.device.Get(), r.w, r.h, 6);
        EncoderConfig ec;
        ec.width = r.w;
        ec.height = r.h;
        ec.fps = fps;
        ec.bitrate = bitrate;
        ec.gopSize = fps * 2; // 2 秒一個 IDR：receiver 掉幀後最多 2 秒恢復
        ec.lowLatency = true;
        ec.preferHardware = preferHw;
        encoder.Init(ec, devMgr.Get(), onEncoded);
        std::printf("\n[config] %s %ux%u, encoder: %s (hw=%s, d3d=%s)\n", r.name, r.w, r.h, encoder.Name().c_str(),
                    encoder.IsHardware() ? "yes" : "no", encoder.UsesD3D() ? "yes" : "no");
    };
    configure(resIdx);
    std::printf("Streaming. Keys: '+' higher resolution, '-' lower resolution, ESC quit.\n");

    const int64_t period = QpcFrequency() / fps;
    const int64_t startQpc = QpcNow();
    int64_t nextTick = startQpc;
    int64_t lastSampleTime = -1;
    int64_t latestCaptureQpc = 0;
    bool pending = false, force = true;
    int captured = 0, encoded = 0, skipped = 0;
    int64_t statsStart = QpcNow();

    while (!stop) {
        // ---- 鍵盤：切解析度 / 結束 ----
        while (_kbhit()) {
            int c = _getch();
            if (c == 27) stop = true;
            int want = c == '+' || c == '=' ? resIdx - 1 : c == '-' ? resIdx + 1 : resIdx;
            if (want != resIdx && want >= 0 && want < kNumRes) {
                resIdx = want;
                configure(resIdx);
                force = true; // 畫面靜止時也要馬上送一張新解析度的 IDR
            }
        }
        if (stop) break;

        // ---- 擷取：等到下一個 tick，期間有新畫面就更新 latest ----
        int64_t remain = nextTick - QpcNow();
        int64_t acquiredAt = 0;
        CaptureStatus cs = capture.Acquire(remain > 0 ? (UINT)QpcToMs(remain) : 0, latest.Get(), &acquiredAt);
        if (cs == CaptureStatus::NewFrame) {
            pending = true;
            latestCaptureQpc = acquiredAt;
            ++captured;
        } else if (cs == CaptureStatus::AccessLost) {
            if (FAILED(capture.Reinit())) Sleep(200); // 例如鎖屏中，稍後再試
            continue;
        }
        if (QpcNow() < nextTick) continue;
        nextTick = std::max(nextTick + period, QpcNow()); // 落後太多就不追
        if (!pending && !force) continue;                 // 畫面沒變就不編碼（省頻寬）

        // ---- 背壓：網路跟不上時，在編碼前丟掉原始畫面 ----
        if (sendQueue.Size() >= kSendQueueSoftLimit) {
            ++skipped;
            continue; // pending 保持 true，下個 tick 送最新的畫面
        }

        Nv12SamplePool::Slot* slot = pool.Acquire();
        if (!slot) { ++skipped; continue; }
        scaler.Process(latest.Get(), 0, slot->texture.Get()); // GPU：縮放 + BGRA->NV12
        ComPtr<IMFSample> sample = slot->sample;
        if (!encoder.UsesD3D()) // 只有軟體 fallback 才會讀回 CPU
            sample = MakeCpuNv12Sample(d3d.device.Get(), d3d.context.Get(), slot->texture.Get(), kResolutions[resIdx].w,
                                       kResolutions[resIdx].h);

        int64_t capQpc = pending ? latestCaptureQpc : QpcNow();
        int64_t sampleTime = std::max(QpcTo100ns(QpcNow() - startQpc), lastSampleTime + 1);
        lastSampleTime = sampleTime;
        CHECK_HR(sample->SetSampleTime(sampleTime));
        CHECK_HR(sample->SetSampleDuration(10'000'000 / fps));
        {
            std::lock_guard<std::mutex> lk(metaMu);
            captureQpcBySampleTime[sampleTime] = capQpc;
        }
        try {
            encoder.Encode(sample.Get());
        } catch (const std::exception& e) {
            // 硬體 encoder 在某些 driver 上無法運作（例如 E_UNEXPECTED）：改用軟體 encoder 繼續串流
            if (!encoder.IsHardware()) throw;
            std::fprintf(stderr, "\nHardware encoder failed: %s\n-> switching to the software encoder.\n", e.what());
            preferHw = false;
            configure(resIdx);
            force = true;
            continue;
        }
        ++encoded;
        pending = force = false;

        // ---- 每秒統計 ----
        if (QpcToMs(QpcNow() - statsStart) >= 1000) {
            double sec = QpcToMs(QpcNow() - statsStart) / 1000;
            std::lock_guard<std::mutex> lk(metaMu);
            std::printf("[%s] captured %4.1f fps | encoded %4.1f fps | skipped %d | queue %zu | %.2f Mbps | "
                        "capture->encoded avg %.1f ms max %.1f ms\n",
                        kResolutions[resIdx].name, captured / sec, encoded / sec, skipped, sendQueue.Size(),
                        bytesSent.exchange(0) * 8 / sec / 1e6, capToEnc.Avg(), capToEnc.max);
            capToEnc.Reset();
            captured = encoded = skipped = 0;
            statsStart = QpcNow();
        }
    }

    // ---- 收尾：先停 encoder（不再產生輸出），再關 socket 喚醒 send thread ----
    stop = true;
    encoder.Shutdown();
    sendQueue.Close();
    shutdown(sock, SD_BOTH);
    closesocket(sock);
    sender.join();
    std::printf("Sender stopped cleanly. Frames sent: %d\n", framesSent.load());
    return 0;
}

int main(int argc, char** argv)
{
    SetupConsole();
    EnablePerMonitorDpiAwareness();
    try {
        MfScope mf;
        Args args(argc, argv);
        return Run(args);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
