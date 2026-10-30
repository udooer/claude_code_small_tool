// Lab 4 — H.264 硬體編碼
//
//   dda_encode.exe [--output N] [--width 1280] [--height 720] [--fps 30] [--seconds 5]
//                  [--bitrate 8000000] [--gop 60] [--out output.h264] [--software] [--timer-1ms] [--sync-gpu] [--no-low-latency] [--bframes N]
//                  [--frame-log frames.csv]   每幀的量測值寫成 CSV（給 encoder_bench.ps1 分析）
//
// 學習重點：
//   1. MFTEnumEx 列舉硬體 encoder，沒有就 fallback 到軟體，印出實際用的是哪一個
//   2. MFT 資料流：協商 MediaType -> START_OF_STREAM -> 送/取 -> DRAIN
//   3. 時間戳：100ns 單位，i * 10,000,000 / fps（先乘後除，不累積誤差）
//   4. 恆定幀率：DDA 畫面沒變時不給 frame，要重送上一張，才會剛好 fps*seconds 張
//   5. 送進 async encoder 的 texture 要用 pool，不能每幀覆寫同一張
#include <windows.h>
#include <mmsystem.h> // timeBeginPeriod

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "cli.h"
#include "d3d_util.h"
#include "desktop_capture.h"
#include "h264_encoder.h"
#include "hr.h"
#include "mf_util.h"
#include "nv12_pool.h"
#include "timer.h"
#include "video_scaler.h"

static int Run(const Args& args)
{
    const UINT outputIndex = (UINT)args.GetInt("--output", 0);
    EncoderConfig ec;
    ec.width = (UINT)args.GetInt("--width", 1280) & ~1u;
    ec.height = (UINT)args.GetInt("--height", 720) & ~1u;
    ec.fps = (UINT)args.GetInt("--fps", 30);
    ec.bitrate = (UINT)args.GetInt("--bitrate", 8'000'000);
    ec.gopSize = (UINT)args.GetInt("--gop", ec.fps * 2);
    ec.lowLatency = !args.Has("--no-low-latency"); // 關掉時 encoder 可以累積多張、也可能用 B-frame（延遲變大）
    ec.bFrames = args.GetInt("--bframes", -1);    // 單獨控制 B-frame 數量（-1 = 跟著 low latency 的設定）
    ec.preferHardware = !args.Has("--software");
    const UINT seconds = (UINT)args.GetInt("--seconds", 5);
    const UINT totalFrames = ec.fps * seconds;
    const std::string outPath = args.Get("--out", "output.h264");

    // 除錯用：把 Windows 計時器解析度從預設的 15.6 ms 調成 1 ms。
    // 如果 Encode() 的時間因此大幅下降，代表某處（例如 driver 內部）用 Sleep/計時器在等待。
    struct TimerPeriod {
        bool on;
        explicit TimerPeriod(bool enable) : on(enable && timeBeginPeriod(1) == TIMERR_NOERROR) {}
        ~TimerPeriod() { if (on) timeEndPeriod(1); }
    } timerPeriod(args.Has("--timer-1ms"));
    if (timerPeriod.on) std::printf("Timer resolution: 1 ms (timeBeginPeriod)\n");
    // 除錯用：送進 encoder 前先等 GPU 把縮放做完。用來分辨 ProcessInput 的等待是不是在等我們的 GPU 工作。
    const bool syncGpu = args.Has("--sync-gpu");

    D3DContext d3d = CreateD3DForOutput(outputIndex);
    ComPtr<IMFDXGIDeviceManager> devMgr = CreateDxgiDeviceManager(d3d.device.Get());

    DesktopCapture capture;
    HRESULT hr = capture.Init(d3d.device.Get(), d3d.output.Get());
    if (FAILED(hr)) {
        std::fprintf(stderr, "DuplicateOutput failed: %s\n", HrToString(hr).c_str());
        return 2;
    }
    // 「最近一張」桌面畫面。DDA 沒給新畫面時就重送它（恆定幀率）。
    ComPtr<ID3D11Texture2D> latest = CreateTexture(d3d.device.Get(), capture.Width(), capture.Height(), capture.Format(),
                                                   D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);

    ScalerConfig sc;
    sc.srcWidth = capture.Width();
    sc.srcHeight = capture.Height();
    sc.srcFormat = capture.Format();
    sc.dstWidth = ec.width;
    sc.dstHeight = ec.height;
    sc.dstFormat = DXGI_FORMAT_NV12; // 縮放 + BGRA->NV12 一次 Blt
    VideoScaler scaler;
    scaler.Init(d3d.device.Get(), sc);

    Nv12SamplePool pool;
    pool.Init(d3d.device.Get(), ec.width, ec.height, 8);

    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "Cannot open %s\n", outPath.c_str());
        return 3;
    }
    std::mutex outMu; // async encoder 會在 MF 的 thread 上呼叫 callback
    std::atomic<int> framesOut{ 0 }, keyframes{ 0 };
    std::atomic<int64_t> bytesOut{ 0 };

    H264Encoder encoder;
    auto onOutput = [&](EncodedFrame&& f) {
        std::lock_guard<std::mutex> lk(outMu);
        std::fwrite(f.data.data(), 1, f.data.size(), out); // Annex B 直接串接就是合法的 .h264 檔
        framesOut++;
        keyframes += f.keyframe;
        bytesOut += (int64_t)f.data.size();
    };
    auto initEncoder = [&] {
        encoder.Init(ec, devMgr.Get(), onOutput);
        std::printf("Encoder : %s\n          hardware=%s async=%s d3d11-input=%s\n", encoder.Name().c_str(),
                    encoder.IsHardware() ? "yes" : "NO (software fallback)", encoder.IsAsync() ? "yes" : "no",
                    encoder.UsesD3D() ? "yes" : "no");
        std::printf("          low-latency=%s, B-frames=%s\n", ec.lowLatency ? "yes" : "no (--no-low-latency)",
                    ec.bFrames >= 0 ? std::to_string(ec.bFrames).c_str() : (ec.lowLatency ? "0" : "encoder default"));
        if (!encoder.UsesD3D())
            std::printf("          NOTE: encoder can't read GPU textures -> each frame is read back to CPU (slow path)\n");
    };
    initEncoder();
    std::printf("Encoding %u frames (%u s @ %u fps) at %ux%u, %u bps -> %s\n", totalFrames, seconds, ec.fps, ec.width,
                ec.height, ec.bitrate, outPath.c_str());

    // 先拿到第一張畫面
    CaptureStatus s = CaptureStatus::NoChange;
    for (int i = 0; i < 20 && s != CaptureStatus::NewFrame; ++i) s = capture.Acquire(500, latest.Get());
    if (s != CaptureStatus::NewFrame) {
        std::fprintf(stderr, "No desktop frame captured\n");
        return 4;
    }

    const int64_t period = QpcFrequency() / ec.fps;
    int64_t start = QpcNow();
    int newFrames = 0, repeatedFrames = 0; // 第 0 幀在迴圈裡會以 gotNew 計入，這裡不能先算
    Stat submitMs, gpuWaitMs, lateMs; // lateMs：實際送進 encoder 的時刻比預定時刻晚了多久（跟不上時會一直變大）
    std::vector<double> submitEndMs;   // 每幀 Encode() 回來的時刻（從 start 起算）
    std::vector<char> newFlags;        // 每幀是不是新的桌面畫面
    double wallMs = 0;

    // 硬體 encoder 在某些機器/driver 上會在第一次 ProcessOutput 就失敗（例如舊版 Intel driver 回 E_UNEXPECTED）。
    // 還沒產生任何輸出前失敗的話，自動改用軟體 encoder 從頭再錄一次，而不是直接結束。
    for (int attempt = 0;; ++attempt) {
    try {
    for (UINT i = 0; i < totalFrames; ++i) {
        // ---- 等到這一幀的時間點；期間有新畫面就更新 latest ----
        const int64_t due = start + (int64_t)i * period;
        bool gotNew = (i == 0);
        for (;;) {
            int64_t remain = due - QpcNow();
            UINT timeout = remain > 0 ? (UINT)QpcToMs(remain) : 0;
            CaptureStatus cs = capture.Acquire(timeout, latest.Get());
            if (cs == CaptureStatus::NewFrame) gotNew = true;
            if (cs == CaptureStatus::AccessLost) {
                std::fprintf(stderr, "  ACCESS_LOST at frame %u, re-creating duplication\n", i);
                capture.Reinit(); // 失敗（例如鎖屏中）就繼續重送最後一張，下一輪再試
            }
            if (QpcNow() >= due) break;
        }
        gotNew ? ++newFrames : ++repeatedFrames;
        newFlags.push_back(gotNew ? 1 : 0);

        // ---- 縮放 + 轉 NV12 到 pool 中一張 encoder 沒在用的 texture ----
        Nv12SamplePool::Slot* slot = pool.Acquire();
        if (!slot) throw std::runtime_error("all NV12 textures are still held by the encoder");
        scaler.Process(latest.Get(), 0, slot->texture.Get());
        if (syncGpu) gpuWaitMs.Add(WaitForGpu(d3d.device.Get(), d3d.context.Get()));

        ComPtr<IMFSample> sample = slot->sample;
        if (!encoder.UsesD3D())
            sample = MakeCpuNv12Sample(d3d.device.Get(), d3d.context.Get(), slot->texture.Get(), ec.width, ec.height);

        // ---- 時間戳：100ns 單位，先乘後除 ----
        CHECK_HR(sample->SetSampleTime(MFllMulDiv(i, 10'000'000, ec.fps, 0)));
        CHECK_HR(sample->SetSampleDuration(MFllMulDiv(1, 10'000'000, ec.fps, 0)));

        int64_t t0 = QpcNow();
        lateMs.Add(QpcToMs(t0 - due));
        encoder.Encode(sample.Get());
        const int64_t t1 = QpcNow();
        submitMs.Add(QpcToMs(t1 - t0));
        submitEndMs.push_back(QpcToMs(t1 - start));
        if ((i + 1) % ec.fps == 0) std::printf("  %u / %u frames submitted, %d encoded\n", i + 1, totalFrames, framesOut.load());
    }

    encoder.Drain(); // 不 drain 會少最後幾張
    wallMs = QpcToMs(QpcNow() - start);
    break;
    } catch (const std::exception& e) {
        if (attempt > 0 || !encoder.IsHardware() || framesOut.load() > 0) throw;
        std::fprintf(stderr, "\nHardware encoder failed before producing any output:\n  %s\n", e.what());
        std::fprintf(stderr, "-> Falling back to the software encoder and starting over. (Use --software to skip this.)\n\n");
        ec.preferHardware = false;
        initEncoder();
        start = QpcNow();
        newFrames = 0;
        repeatedFrames = 0;
        submitMs.Reset();
        gpuWaitMs.Reset();
        lateMs.Reset();
        submitEndMs.clear();
        newFlags.clear();
    }
    }
    // 以下都必須在 Shutdown 之前取：Shutdown 會釋放 encoder 內部狀態
    const EncoderTiming et = encoder.Timing();
    const std::string encName = encoder.Name();
    const bool encHw = encoder.IsHardware(), encAsync = encoder.IsAsync(), encD3d = encoder.UsesD3D();
    encoder.Shutdown();
    std::fclose(out);

    const double actualBps = bytesOut * 8.0 / seconds;
    std::printf("\nDone.\n");
    std::printf("  frames in   : %u  (new desktop images %d, repeated %d)\n", totalFrames, newFrames, repeatedFrames);
    std::printf("  frames out  : %d  (keyframes %d)%s\n", framesOut.load(), keyframes.load(),
                framesOut.load() == (int)totalFrames ? "" : "   <-- should equal frames in!");
    // 跟得上的話，實際花的時間 ≈ 影片長度。明顯比較久代表每幀的處理時間超過 1/fps。
    std::printf("  wall time   : %.2f s for %u s of video (%.0f frames/s actually processed)%s\n", wallMs / 1000,
                seconds, totalFrames * 1000.0 / wallMs, wallMs > seconds * 1000.0 * 1.05 ? "   <-- could not keep up" : "");
    std::printf("  file size   : %lld bytes, average %.2f Mbps (target %.2f Mbps)\n", (long long)bytesOut.load(),
                actualBps / 1e6, ec.bitrate / 1e6);
    std::printf("  Encode() call time: avg %.2f ms, max %.2f ms\n", submitMs.Avg(), submitMs.max);
    if (gpuWaitMs.count)
        std::printf("  GPU wait before Encode (--sync-gpu): avg %.2f ms, max %.2f ms\n", gpuWaitMs.Avg(), gpuWaitMs.max);
    auto line = [](const char* name, const Stat& s) {
        if (s.count) std::printf("    %-24s avg %6.2f ms, max %6.2f ms  (n=%d)\n", name, s.Avg(), s.max, s.count);
    };
    std::printf("  Encoder timing breakdown:\n");
    line("wait for NeedInput", et.waitInput);
    line("ProcessInput", et.processInput);
    line("input -> next NeedInput", et.inputToNeedInput);
    line("input -> output (latency)", et.inputToOutput);
    // ---- 每幀明細（--frame-log）----
    const std::string frameLog = args.Get("--frame-log", "");
    if (!frameLog.empty()) {
        if (FILE* f = std::fopen(frameLog.c_str(), "w")) {
            // lat_ms / output_at_ms 依「輸出順序」排列；沒有 B-frame 時就等於幀的順序
            std::fprintf(f, "frame,new,late_ms,encode_ms,wait_ms,process_input_ms,latency_ms,submit_end_ms,output_at_ms\n");
            auto at = [](const std::vector<double>& v, size_t i) { return i < v.size() ? v[i] : -1.0; };
            for (size_t i = 0; i < (size_t)totalFrames; ++i)
                std::fprintf(f, "%zu,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", i, i < newFlags.size() ? newFlags[i] : -1,
                             at(lateMs.values, i), at(submitMs.values, i), at(et.waitInput.values, i),
                             at(et.processInput.values, i), at(et.inputToOutput.values, i), at(submitEndMs, i),
                             at(et.outputAtMs, i));
            std::fclose(f);
            std::printf("  frame log   : %s\n", frameLog.c_str());
        } else {
            std::fprintf(stderr, "Cannot write %s\n", frameLog.c_str());
        }
    }

    // ---- 機器可讀的一行摘要（encoder_bench.ps1 解析這一行）----
    std::string r = "RESULT";
    auto kv = [&](const char* k, const std::string& v) { r += std::string(" ") + k + "=" + v; };
    auto num = [](double v) { char b[32]; std::snprintf(b, sizeof(b), "%.3f", v); return std::string(b); };
    auto stat = [&](const char* k, const Stat& st) {
        if (!st.count) return;
        const std::string p = k;
        kv((p + "_avg").c_str(), num(st.Avg()));
        kv((p + "_min").c_str(), num(st.min));
        kv((p + "_p50").c_str(), num(st.Percentile(50)));
        kv((p + "_p95").c_str(), num(st.Percentile(95)));
        kv((p + "_max").c_str(), num(st.max));
    };
    kv("hw", encHw ? "1" : "0");
    kv("async", encAsync ? "1" : "0");
    kv("d3d", encD3d ? "1" : "0");
    kv("ll", ec.lowLatency ? "1" : "0");
    kv("bframes", std::to_string(ec.bFrames));
    kv("sync_gpu", syncGpu ? "1" : "0");
    kv("timer1ms", timerPeriod.on ? "1" : "0");
    kv("width", std::to_string(ec.width));
    kv("height", std::to_string(ec.height));
    kv("fps", std::to_string(ec.fps));
    kv("seconds", std::to_string(seconds));
    kv("frames_in", std::to_string(totalFrames));
    kv("frames_out", std::to_string(framesOut.load()));
    kv("keyframes", std::to_string(keyframes.load()));
    kv("new", std::to_string(newFrames));
    kv("repeated", std::to_string(repeatedFrames));
    kv("wall_ms", num(wallMs));
    kv("bytes", std::to_string((long long)bytesOut.load()));
    kv("mbps", num(actualBps / 1e6));
    stat("encode", submitMs);
    stat("late", lateMs);
    stat("gpuwait", gpuWaitMs);
    stat("wait", et.waitInput);
    stat("pi", et.processInput);
    stat("i2n", et.inputToNeedInput);
    stat("lat", et.inputToOutput);
    std::printf("%s encoder=\"%s\"\n", r.c_str(), encName.c_str());

    std::printf("\nVerify with:\n  ffprobe -v error -count_frames -select_streams v:0 -show_entries "
                "stream=codec_name,profile,width,height,nb_read_frames,has_b_frames -of default=nw=1 %s\n"
                "  ffplay -framerate %u %s     (raw .h264 has no timestamps; ffmpeg assumes 25 fps otherwise)\n",
                outPath.c_str(), ec.fps, outPath.c_str());
    return 0;
}

int main(int argc, char** argv)
{
    SetupConsole();
    EnablePerMonitorDpiAwareness();
    try {
        MfScope mf;
        return Run(Args(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ERROR: %s\n", e.what());
        return 1;
    }
}
