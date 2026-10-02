// Lab 6 — Receiver：TCP 收 -> H.264 解碼(GPU) -> VideoProcessorBlt -> 視窗顯示
//
//   receiver.exe [--port 5000] [--width 1280] [--height 720] [--novsync] [--software]
//   先啟動 receiver，再啟動 sender。在視窗按 ESC 結束。
//
// 架構：
//   recv thread : RecvAll(固定長度 header) -> 檢查 magic/長度 -> RecvAll(payload) -> queue
//   main thread : 視窗訊息 + 解碼 + 顯示 + 延遲統計
//
// 延遲（同一台機器 QPC 可直接相減；跨機器時 ②③ 無意義，需要另外對時）：
//   ① 擷取 -> 編碼完成      = header.encodeDoneQpc - header.captureQpc
//   ② 編碼完成 -> 解碼完成  = decodeDone - header.encodeDoneQpc（含 TCP 傳輸與排隊）
//   ③ 端到端                = Present 回來 - header.captureQpc
#include <atomic>
#include <cstdio>
#include <map>
#include <thread>
#include <vector>

#include "blocking_queue.h"
#include "cli.h"
#include "d3d_util.h"
#include "framing.h"
#include "h264_decoder.h"
#include "hr.h"
#include "mf_util.h"
#include "net.h"
#include "render_window.h"
#include "timer.h"

namespace {

struct Packet {
    framing::FrameHeader header;
    std::vector<uint8_t> payload;
};

// 解碼跟不上時（queue 太長），丟到下一個 keyframe 為止：不能只丟 P-frame
constexpr size_t kMaxBacklog = 30;

SOCKET ListenAndAccept(const std::string& port, bool* isLoopback)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (getaddrinfo(nullptr, port.c_str(), &hints, &res) != 0) throw std::runtime_error("getaddrinfo failed");
    SOCKET ls = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (bind(ls, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR || listen(ls, 1) == SOCKET_ERROR) {
        freeaddrinfo(res);
        closesocket(ls);
        throw std::runtime_error("bind/listen failed on port " + port);
    }
    freeaddrinfo(res);
    std::printf("Waiting for sender on port %s ...\n", port.c_str());
    sockaddr_in peer{};
    int len = sizeof(peer);
    SOCKET s = accept(ls, (sockaddr*)&peer, &len);
    closesocket(ls);
    if (s == INVALID_SOCKET) throw std::runtime_error("accept failed");
    char ip[64] = {};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    std::printf("Sender connected from %s\n", ip);
    *isLoopback = (peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    SetNoDelay(s);
    return s;
}

} // namespace

static int Run(Args& args)
{
    args.DeclareFlags({ "--novsync", "--software" });
    const std::string port = args.Get("--port", "5000");
    const UINT winW = (UINT)args.GetInt("--width", 1280);
    const UINT winH = (UINT)args.GetInt("--height", 720);
    const bool vsync = !args.Has("--novsync");

    WsaScope wsa;
    bool loopback = true;
    SOCKET sock = ListenAndAccept(port, &loopback);
    if (!loopback)
        std::printf("NOTE: sender is on another machine. QPC clocks are not comparable, so latency (2) and (3)\n"
                    "      are meaningless here; only (1) is valid. See the walkthrough for how to measure.\n");

    D3DContext d3d = CreateD3DDefault();
    ComPtr<IMFDXGIDeviceManager> devMgr = CreateDxgiDeviceManager(d3d.device.Get());
    H264Decoder decoder;
    decoder.Init(args.Has("--software") ? nullptr : devMgr.Get(), /*lowLatency*/ true);
    std::printf("Decoder D3D11/DXVA: %s\n", decoder.UsesD3D() ? "yes" : "NO (software)");

    RenderWindow window;
    window.Create(L"WMF Lab 6 receiver (ESC to quit)", winW, winH, d3d.device.Get());

    // ---- recv thread ----
    BlockingQueue<Packet> queue;
    std::atomic<bool> disconnected{ false };
    std::atomic<int64_t> bytesRecv{ 0 };
    std::thread receiver([&] {
        uint8_t hb[framing::kHeaderSize];
        for (;;) {
            Packet p;
            if (!RecvAll(sock, hb, sizeof(hb))) break;
            if (!framing::Deserialize(hb, p.header)) { // magic 錯 / 長度超過上限：資料錯位或惡意輸入
                std::fprintf(stderr, "Invalid frame header, closing connection\n");
                break;
            }
            p.payload.resize(p.header.payloadSize);
            if (!RecvAll(sock, p.payload.data(), (int)p.payload.size())) break;
            bytesRecv += (int64_t)(sizeof(hb) + p.payload.size());
            queue.Push(std::move(p));
        }
        disconnected = true;
        queue.Close();
    });

    std::map<int64_t, framing::FrameHeader> inflight; // decoder sampleTime -> header
    Stat lat1, lat2, lat3;
    int shown = 0, dropped = 0;
    bool dropUntilKey = false;
    UINT lastW = 0, lastH = 0;
    int64_t statsStart = QpcNow();

    auto onFrame = [&](const DecodedFrame& f) {
        int64_t decodeDone = QpcNow();
        auto it = inflight.find(f.sampleTime);
        window.Present(f, vsync); // GPU：NV12 texture -> back buffer
        int64_t presented = QpcNow();
        if (it != inflight.end()) {
            const framing::FrameHeader& h = it->second;
            lat1.Add(QpcToMs(h.encodeDoneQpc - h.captureQpc));
            lat2.Add(QpcToMs(decodeDone - h.encodeDoneQpc));
            lat3.Add(QpcToMs(presented - h.captureQpc));
            inflight.erase(inflight.begin(), std::next(it));
        }
        ++shown;
    };

    while (window.PumpMessages()) {
        Packet p;
        if (!queue.Pop(p, std::chrono::milliseconds(5))) {
            if (disconnected) break;
            continue;
        }
        const bool key = (p.header.flags & framing::kKeyframe) != 0;
        if (queue.Size() > kMaxBacklog) dropUntilKey = true;
        if (dropUntilKey && !key) {
            ++dropped;
            continue;
        }
        dropUntilKey = false;
        if (p.header.width != lastW || p.header.height != lastH) {
            std::printf("\n[stream] %ux%u%s\n", p.header.width, p.header.height, key ? " (keyframe)" : "");
            lastW = p.header.width;
            lastH = p.header.height;
        }
        int64_t ts = (int64_t)p.header.frameIndex * 10000; // 只要遞增即可
        inflight[ts] = p.header;
        decoder.Decode(p.payload.data(), p.payload.size(), ts, onFrame);

        if (QpcToMs(QpcNow() - statsStart) >= 1000) {
            double sec = QpcToMs(QpcNow() - statsStart) / 1000;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "%ux%u %4.1f fps %.2f Mbps | (1)cap->enc %.1f/%.1f | (2)enc->dec %.1f/%.1f | (3)e2e %.1f/%.1f ms "
                          "(avg/max) | dropped %d",
                          lastW, lastH, shown / sec, bytesRecv.exchange(0) * 8 / sec / 1e6, lat1.Avg(), lat1.max,
                          lat2.Avg(), lat2.max, lat3.Avg(), lat3.max, dropped);
            std::printf("%s\n", line);
            window.SetTitle(line);
            lat1.Reset();
            lat2.Reset();
            lat3.Reset();
            shown = dropped = 0;
            statsStart = QpcNow();
        }
    }

    std::printf(disconnected ? "Sender disconnected.\n" : "Window closed.\n");
    shutdown(sock, SD_BOTH); // 讓阻塞中的 recv 返回，recv thread 才能結束
    closesocket(sock);
    queue.Close();
    receiver.join();
    decoder.Shutdown();
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
