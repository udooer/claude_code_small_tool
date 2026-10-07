# WMF 訓練 Lab 參考實作

《[WMF 新人訓練文件](../WMF_Training_Guide.md)》Lab 1~6 的參考解答。每一題都可以獨立編譯、執行。
帶讀教學請看 **[WALKTHROUGH.md](WALKTHROUGH.md)**。

> 這是 mentor 用的參考實作。不要直接發給學員，否則他們就沒有東西好練習了。

## 目錄結構

```
labs/
├── CMakeLists.txt
├── WALKTHROUGH.md          ← 逐題帶你走一遍（從這裡開始）
├── common/                 ← 所有 Lab 共用的封裝，每個檔案對應一個核心觀念
│   ├── hr.h                  CHECK_HR、HRESULT 名稱對照
│   ├── d3d_util.*            D3D11 device（找對 adapter）、readback（RowPitch）、NV12 存 BMP
│   ├── desktop_capture.*     DDA：Acquire → Copy → ReleaseFrame
│   ├── video_scaler.*        ID3D11VideoProcessor：縮放 + 色彩轉換 + letterbox
│   ├── yuv.h                 手刻 RGB↔YUV（BT.601/709 × studio/full）
│   ├── h264_encoder.*        Encoder MFT：硬體 async / 軟體 sync 兩種都支援
│   ├── h264_decoder.*        Decoder MFT：STREAM_CHANGE、texture array、1088 裁切
│   ├── nv12_pool.h           送進 encoder 的 texture pool
│   ├── annexb.h              Annex B NAL / access unit 切割
│   ├── framing.h, net.h      Lab 6 的 TCP 訊框（長度前綴）、recvAll / sendAll
│   ├── render_window.*       視窗 + swap chain，GPU 顯示 NV12
│   └── pattern_window.*      Lab 3 色塊測試畫面
├── lab1_capture/   → dda_capture.exe
├── lab2_scale/     → dda_scale.exe
├── lab3_nv12/      → dda_nv12.exe
├── lab4_encode/    → dda_encode.exe
├── lab5_decode/    → h264_decode.exe
├── lab6_stream/    → sender.exe / receiver.exe
└── tests/
    ├── portable_tests.cpp    不需要 Windows 的單元測試（YUV 公式、NAL 切割、訊框、BMP）
    └── codec_loopback.cpp    不需要螢幕擷取的 encoder→decoder 自我測試
```

## 建置

需求：Windows 10/11、Visual Studio 2019 以上（含「使用 C++ 的桌面開發」）、CMake 3.16 以上。

```bat
cd WMF_Training\labs
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
:: 執行檔在 build\Release\
```

Visual Studio 也可以直接「開啟資料夾」，它會自己讀 CMakeLists.txt。

或直接執行 **`build.bat`**（在「x64 Native Tools Command Prompt for VS 2022」裡執行，會自動完成上面兩步）。

**Debug build** 會開啟 D3D debug layer，程式結束時 Output 視窗會列出沒有釋放的 D3D 物件，很適合用來檢查洩漏。需要先到「選用功能」安裝 Graphics Tools，沒裝的話程式會自動改用不帶 debug layer 的 device。

**Windows N / KN 版**：這兩個版本預設沒有 Media Foundation 編解碼器，執行時會出現 `REGDB_E_CLASSNOTREG`。請先安裝 Media Feature Pack。

### 在 Linux 上檢查是否能編譯

```bash
# 單元測試（原生執行）
cmake -B build-linux && cmake --build build-linux && ./build-linux/portable_tests

# 交叉編譯出 Windows exe（需要 mingw-w64），產生的 exe 是靜態連結，可以直接拷到 Windows 執行
cmake -B build-mingw -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw
```

## 各程式一覽

| Lab | 執行檔 | 常用參數 | 產出 |
|---|---|---|---|
| 1 | `dda_capture` | `--list` `--output N` `--delay 5000` | `capture.bmp` |
| 2 | `dda_scale` | `--width 1280 --height 720 --mode letterbox\|stretch` | `capture_scaled.bmp`、GPU 計時 |
| 3 | `dda_nv12` | `--patches` `--vp-matrix/--vp-range` `--decode-matrix/--decode-range` | `capture_bgra.bmp`、`capture_nv12.bmp`、色塊量測表 |
| 4 | `dda_encode` | `--seconds 5 --fps 30 --bitrate 8000000 --software` | `output.h264` |
| 5 | `h264_decode` | `output.h264 --max 10 --play --software` | `frame_0001.bmp`…、視窗播放 |
| 6 | `receiver` / `sender` | `receiver --novsync`、`sender --host IP --res 720` | 即時串流 + 三個延遲數字 |
| - | `codec_loopback` | `--hw` | 編解碼自我測試 |

## 驗證狀態

這些程式是在 Linux 上開發的，無法使用 GPU 與桌面擷取。目前做過的驗證如下：

| 項目 | 方式 | 結果 |
|---|---|---|
| 全部 9 個執行檔能編譯（7 個 Lab + 2 個測試） | mingw-w64 13 交叉編譯，`-Wall -Wextra` | ✅ 0 warning |
| `portable_tests` | Linux 原生 + Wine 執行 Windows 版 | ✅ 全部通過 |
| Lab 5 NAL/AU 切割 + MF decoder 流程（STREAM_CHANGE、drain、1088 裁切、BT.709 反轉換） | Wine 9 + `--software`，輸入 ffmpeg 產生的 720p 與 1080p（含 B-frame）檔案 | ✅ 張數相等；和 ffmpeg 解出的畫面相比 PSNR 48.9 dB |
| MSVC 建置、`portable_tests`、`codec_loopback`（軟體 encoder） | Windows 實機（ASUS，Intel GPU） | ✅ 通過 |
| `codec_loopback --hw` / `--probe`（Intel Quick Sync, UHD 620, driver 27.20.100.8681） | Windows 實機 | ✅ 修正 async STREAM_CHANGE 後全部 66 種組態通過 |
| Lab 1~4、Lab 6、`--play`、D3D11/DXVA 路徑 | 需要真實的 Windows + GPU | ⚠️ 尚未在實機執行 |

第一次在 Windows 實機跑的時候，建議照 WALKTHROUGH 的順序從 Lab 1 開始。遇到問題請記下 console 印出的 HRESULT（例如 `0x887A0004 DXGI_ERROR_UNSUPPORTED`），再對照 mentor 指南的附錄 A。

