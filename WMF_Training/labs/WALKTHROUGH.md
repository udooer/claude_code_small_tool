# Lab 帶讀：一步一步走過 Lab 1~6

這份文件帶你親手跑過每一題的參考實作。每一題分成四段：

1. **跑起來**：指令，以及你應該看到什麼。
2. **程式導讀**：只看關鍵的那幾行，每一行對應一個核心觀念。
3. **動手弄壞它**：故意改錯一個地方，親眼看到症狀。之後在學員作業裡看到同樣的症狀，就能直接認出原因。
4. **對照 mentor 指南**：這一題的參考實作對應指南裡的哪些 checklist 項目。

建議一次走一題。每題大約 20~40 分鐘。

> 下面的輸出都是**示意**，實際數字會因你的 GPU、驅動、解析度而不同。
> 建置方式見 [README.md](README.md)。以下指令假設你在 `build\Release\` 目錄。

---

## 開始之前：先跑兩個不需要螢幕的測試

```bat
portable_tests.exe
codec_loopback.exe
codec_loopback.exe --hw
```

- `portable_tests`：檢查 YUV 公式、NAL 切割、TCP 訊框、BMP 寫檔。結尾印出 `ALL PASSED` 才對。
- `codec_loopback`：產生測試畫面 → 編碼 → 解碼，檢查張數與顏色，用來確認這台機器的 Media Foundation 環境沒問題。加上 `--hw` 會改用硬體 encoder。
  - `--hw` 走的是和 Lab 4/6 相同的路徑：D3D11 device + device manager + GPU NV12 texture。
  - **實機紀錄（Intel Quick Sync）**：硬體 encoder 如果只餵 CPU 記憶體、不給 D3D device（`--hw-sysmem`），Intel QSV 會在 `ProcessOutput` 回 `0x8000FFFF E_UNEXPECTED`。這是很好的教材：硬體 encoder 是為「GPU texture 進、bitstream 出」設計的，**device manager 不是可有可無的效能選項**。
  - 混合顯卡筆電會列出多個硬體 encoder（例如 Intel + NVIDIA）。程式會優先挑和 D3D11 device **同一張 GPU** 的那一個，並標示 `<- same GPU as our D3D11 device`。
  - 印出 `REGDB_E_CLASSNOTREG` → 這是 Windows N 版，請先安裝 Media Feature Pack。
  - 預期輸出（示意）：
    ```
    Encoder: Microsoft H264 Encoder MFT (software) (hw=no async=no)
    Encoded 60 / 60 frames, 98765 bytes
      bar 0: YUV  63 102 240 -> RGB 255   0   0  expected 255   0   0  OK
      ...
    Decoded 60 / 60 frames
    LOOPBACK PASS
    ```
  - 注意紅色色塊的 YUV 是 `63 102 240`。這組數字在 Lab 3 會再出現。

---

## Lab 1｜桌面擷取存成圖片

### 跑起來

```bat
dda_capture.exe --list
dda_capture.exe
dda_capture.exe --output 1          :: 多螢幕
dda_capture.exe --delay 5000        :: 執行後 5 秒內按 Win+L
```

正常情況（示意）：
```
Adapter : NVIDIA GeForce RTX 3060
Output  : \\.\DISPLAY1 (2560x1440)
Duplication: 2560x1440
Got a frame on attempt 1
Saved capture.bmp
```
鎖定畫面時（示意）：
```
DuplicateOutput failed: 0x80070005 E_ACCESSDENIED (secure desktop: lock screen / UAC?) - Access is denied.
  -> The secure desktop (lock screen / UAC prompt) is active; normal processes can't capture it.
```
程式不會 crash，而是印出清楚的原因後結束（exit code 2）。這正是 Lab 1 的驗收標準。

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [d3d_util.cpp `CreateD3DForOutput`](common/d3d_util.cpp) | 用 `EnumAdapters1` 找出「擁有這個螢幕的 adapter」，在那個 adapter 上建立 device。這樣在混合顯卡筆電上才不會出現 `DXGI_ERROR_UNSUPPORTED`。 |
| [d3d_util.cpp `CreateDeviceOn`](common/d3d_util.cpp) | `VIDEO_SUPPORT` flag 和多執行緒保護：後面的 Lab 要讓 MFT 共用這個 device，所以從第一題就先設好。 |
| [desktop_capture.cpp:31](common/desktop_capture.cpp#L31) | `WAIT_TIMEOUT` 回傳 `NoChange`，**而且不呼叫 ReleaseFrame**，因為這次並沒有成功 Acquire。 |
| [desktop_capture.cpp:39-42](common/desktop_capture.cpp#L39-L42) | 用 RAII guard 確保每一次成功的 Acquire 都配一次 `ReleaseFrame`，就算中途丟例外也一樣。 |
| [desktop_capture.cpp:46](common/desktop_capture.cpp#L46) | `LastPresentTime == 0` 表示只有滑鼠在動，桌面影像沒有更新。 |
| [desktop_capture.cpp:50](common/desktop_capture.cpp#L50) | `CopyResource` 一定要在 `ReleaseFrame`（guard 解構）**之前**。 |
| [d3d_util.cpp:156](common/d3d_util.cpp#L156) | 讀回 CPU：先複製到 STAGING texture 再 Map，並且**逐行用 RowPitch** 複製。 |
| [bmp.h](common/bmp.h) | `biHeight` 用負值代表 top-down。BMP 的 BGRA 順序剛好等於 DXGI 的 B8G8R8A8，不需要換通道。 |

### 動手弄壞它

| 改法 | 你會看到 | 學到什麼 |
|---|---|---|
| 刪掉 `ReleaseGuard`，然後跑 Lab 4（它會連續 Acquire 很多次） | 第 2 次 Acquire 就回 `DXGI_ERROR_INVALID_CALL` | 每次成功的 Acquire 都要 Release |
| 把 `CopyResource` 移到 `ReleaseFrame` 之後 | 有時正常、有時畫面殘缺或是舊的畫面 | DDA texture 只借到 ReleaseFrame 為止。這種「有時候會壞」的 bug 最難抓 |
| `ReadbackBgra` 裡把 `m.RowPitch` 改成 `d.Width * 4`，並用 Lab 2 `--width 1366` 輸出 | 圖片斜斜的 | **重點：在 1920、2560 這類寬度下，RowPitch 常常剛好等於 width×4，bug 會被藏起來。** review 學員作業時，一定要用「奇怪的寬度」測一次 |
| 把 `biHeight` 改成正值 | 圖片上下顛倒 | BMP 預設是 bottom-up |
| 不呼叫 `EnablePerMonitorDpiAwareness`，改用 `GetSystemMetrics` 取螢幕尺寸，然後在 150% 縮放下執行 | 得到 1707×960 這種虛擬化過的尺寸 | DDA 給的是實體像素 |

### 對照 mentor 指南

Lab 1 checklist 的 🔴 項目全部示範在 `desktop_capture.cpp` 與 `lab1_capture/main.cpp`；🟢 加分項（多 adapter、旋轉偵測）也有實作。

---

## Lab 2｜GPU 縮放

### 跑起來

```bat
dda_scale.exe --width 1280 --height 720
dda_scale.exe --width 1024 --height 1024                 :: 比例不同 → 看 letterbox
dda_scale.exe --width 1024 --height 1024 --mode stretch  :: 對照：拉伸
```

輸出（示意）：
```
Scale 2560x1440 -> 1024x1024, mode=letterbox, content rect=(0,224)-(1024,800)
  run 0: submit 0.412 ms | submit + GPU done 1.873 ms  (first run includes view creation)
  run 1: submit 0.021 ms | submit + GPU done 0.402 ms
  run 2: submit 0.019 ms | submit + GPU done 0.388 ms
  ...
Note: 'submit' only measures queuing the command. The real cost is 'submit + GPU done'.
```

**看懂這段計時**：`submit` 只有 0.02 ms，因為 `VideoProcessorBlt` 只是把指令排進 GPU queue；真正的成本要等 GPU 做完才知道（約 0.4 ms）。學員如果回報「縮放只要 0.02 ms」，就是被非同步騙了。第一次比較慢，是因為包含了建立 view 的成本。

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [video_scaler.cpp `Init`](common/video_scaler.cpp) | ContentDesc 只是給驅動的「提示」；真正決定縮放結果的是 source rect / dest rect / target rect。 |
| [video_scaler.cpp:35 `ComputeDestRect`](common/video_scaler.cpp#L35) | 等比縮放後置中，並且對齊偶數（NV12 需要）。 |
| [video_scaler.cpp:87](common/video_scaler.cpp#L87) | 關掉驅動的自動畫質增強，否則 Lab 3 的色塊量測會被影響。 |
| [video_scaler.cpp:92](common/video_scaler.cpp#L92) | letterbox 的黑邊：沒設背景色的話，黑邊區域內容是未定義的。 |
| [video_scaler.cpp `Process`](common/video_scaler.cpp) | input/output view 用 texture 指標當 key 快取起來，不要每幀重建。 |
| [d3d_util.cpp `WaitForGpu`](common/d3d_util.cpp) | `D3D11_QUERY_EVENT`：插一個標記，等 GPU 執行到這個標記。 |

### 動手弄壞它

| 改法 | 你會看到 |
|---|---|
| 刪掉 `VideoProcessorSetOutputBackgroundColor`，用 `--width 1024 --height 1024` 跑 | 黑邊可能出現殘影或雜訊（依驅動而定，有些驅動會自己清成黑色） |
| 輸出 texture 不加 `D3D11_BIND_RENDER_TARGET` | `CreateVideoProcessorOutputView` 回 `E_INVALIDARG` |
| 把 `ComputeDestRect` 改成直接回傳 `{0,0,srcW,srcH}` | 只看到左上角一塊，或整張被裁掉 |

### 對照 mentor 指南

Lab 2 的計時口試題（「你量到 0.05 ms，為什麼可能是錯的？」）可以直接用這個程式的輸出當教材。

---

## Lab 3｜BGRA → NV12 驗證

這是**最值得花時間**的一題。整條 pipeline 的顏色正確性都靠這裡建立的觀念。

### 跑起來：先做對的版本

```bat
dda_nv12.exe --patches
```

程式會在螢幕上蓋一個全螢幕色塊視窗（約 1 秒），擷取後印出（示意）：
```
GPU  (VideoProcessor) writes NV12 as : BT.709 studio(16-235)
CPU  (SaveNV12AsBmp) assumes         : BT.709 studio(16-235)
NV12 readback: 1280x720, RowPitch=1280 (width=1280)  -> UV plane offset = RowPitch*Height = 921600 bytes

patch  | YUV measured    | YUV expected    | RGB after decode | result
-------+-----------------+-----------------+------------------+-------
red    |  63 102 240     |  63 102 240     | 255   0   0      | OK
green  | 173  42  26     | 173  42  26     |   0 255   0      | OK
blue   |  32 240 118     |  32 240 118     |   0   0 255      | OK
white  | 235 128 128     | 235 128 128     | 255 255 255      | OK
black  |  16 128 128     |  16 128 128     |   0   0   0      | OK
gray   | 126 128 128     | 126 128 128     | 128 128 128      | OK

All patches within tolerance.
```

同時會產生 `capture_bgra.bmp`（GPU 直接輸出的 BGRA，作為對照組）和 `capture_nv12.bmp`（GPU 輸出 NV12，再用手刻公式轉回來），兩張應該看起來一樣。

**如果出現 `GPU-YUV-OFF`**：代表驅動沒有照我們要求的色彩空間輸出。例如紅色量到 `82 90 240`，就是 GPU 實際用了 BT.601。這是很真實的狀況：有些驅動會忽略 ColorSpace 設定。這也說明了為什麼要量測，而不是相信 API 呼叫。

### 跑起來：故意弄錯（這是重點）

GPU 端保持 BT.709 studio，只改 CPU 端「以為」的格式：

```bat
dda_nv12.exe --patches --decode-range full
dda_nv12.exe --patches --decode-matrix 601
```

你會看到的結果（下表數字是用 [yuv.h](common/yuv.h) 的公式算出來的理論值，`portable_tests` 會驗證它們）：

| 實驗 | 黑 | 白 | 紅 | 綠 | 肉眼看起來 |
|---|---|---|---|---|---|
| 正確 (709 studio) | 0,0,0 | 255,255,255 | 255,0,0 | 0,255,0 | 正常 |
| `--decode-range full` | **16,16,16** | **235,235,235** | **239,15,15** | **12,237,13** | 整張霧霧的、黑不夠黑、白不夠白、顏色不夠飽和 |
| `--decode-matrix 601` | 0,0,0 | 255,255,255 | **233,0,2** | **20,255,9** | 灰階完全正確；飽和色有細微偏差（綠色偏黃綠） |

再反過來，讓 GPU 輸出 601、CPU 用 709 去解：

```bat
dda_nv12.exe --patches --vp-matrix 601 --decode-matrix 709
```

紅會變成 **255,24,0**（偏橘），綠變成 **0,216,0**（偏暗）。

**從這些實驗要帶走的三件事**（也就是學員報告裡「設錯會怎樣」的標準答案）：

1. **range 錯了**，整張畫面的對比都會跑掉，一眼就看得出來。
2. **係數錯了，灰階完全看不出來**，只有飽和色會偏，而且偏差不大（15~40 個數值），肉眼只會覺得「好像怪怪的」。所以一定要用純色色塊**量測**。
3. 兩端錯成一致時，自己的系統看起來完全正常，但別人的播放器會偏色。所以色彩空間是一份「合約」，必須寫進 bitstream（見 [h264_encoder.cpp](common/h264_encoder.cpp) 的 `MF_MT_YUV_MATRIX` / `MF_MT_VIDEO_NOMINAL_RANGE`）。

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [yuv.h 開頭註解](common/yuv.h) | 用 Kr/Kb 的**定義**推導公式，不是背係數。mentor 指南表格裡的 1.164 / 1.793 / 0.213 / 0.533 / 2.112 都是展開後的結果（`portable_tests` 有驗證）。 |
| [yuv.h `Nv12ToBgra`](common/yuv.h) | UV 用 `(y/2)*pitch + (x/2)*2` 取值：一組 UV 涵蓋 2×2 個像素，U 在前、V 在後。 |
| [d3d_util.h `Nv12Image::UVPlane`](common/d3d_util.h) | UV plane 的位置是 `pitch * codedHeight`，不是 `width * height`。 |
| [video_scaler.cpp:100](common/video_scaler.cpp#L100) | `...ColorSpace1` 在 `ID3D11VideoContext1` 上（原文件勘誤 E4），舊驅動則 fallback 到舊 API。 |

### 動手弄壞它

| 改法（在 `yuv.h`） | 你會看到 |
|---|---|
| `uv[0]` 和 `uv[1]` 對調 | 紅藍互換 |
| `(x / 2) * 2` 改成 `x` | 色彩錯位，右半邊的顏色跑掉 |
| 刪掉 `Clamp8` 裡的 min/max | 飽和色邊緣出現亮點、黑點（溢位） |
| `Nv12Image::UVPlane` 改成 `width * height` | 當 pitch ≠ width 時整張偏綠或出現條紋 |

### 對照 mentor 指南

指南 Lab 3 的「標準色塊對照表」和「設錯會怎樣」兩節，就是這個程式 `--patches` 印出來的東西。review 學員作業時，請他們也印出色塊的 YUV 值。

---

## Lab 4｜H.264 硬體編碼

### 跑起來

```bat
dda_encode.exe
```

輸出（示意）：
```
Encoder : NVIDIA H.264 Encoder MFT
          hardware=yes async=yes d3d11-input=yes
Encoding 150 frames (5 s @ 30 fps) at 1280x720, 8000000 bps -> output.h264
  30 / 150 frames submitted, 28 encoded
  ...
Done.
  frames in   : 150  (new desktop images 41, repeated 109)
  frames out  : 150  (keyframes 3)
  file size   : 812345 bytes, average 1.30 Mbps (target 8.00 Mbps)
  Encode() call time: avg 0.35 ms, max 2.10 ms
```

**看懂這段輸出**：

- `hardware=yes async=yes`：硬體 encoder 幾乎都是 async MFT。加上 `--software` 可以對照軟體 encoder（sync，`d3d11-input=no`，每幀都會讀回 CPU）。
- `new desktop images 41, repeated 109`：桌面大部分時間是靜止的，DDA 只給了 41 張新畫面，其餘 109 張是**重送上一張**，這樣才湊得出恆定的 30fps × 5 秒 = 150 張。這是本題最有鑑別度的觀念。
- `frames out == frames in`：有 drain 才會相等。
- `average 1.30 Mbps` 比 target 低很多：因為靜止畫面很好壓縮，硬體 CBR 不會塞 filler。想驗證 bitrate 設定確實生效，請在錄的時候播放影片。
- `keyframes 3`：GOP = fps × 2 = 60，150 張會有第 0、60、120 張三個 IDR。

驗證檔案：
```bat
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=codec_name,profile,width,height,nb_read_frames,has_b_frames -of default=nw=1 output.h264
ffplay -framerate 30 output.h264
```
預期 `nb_read_frames=150`、`has_b_frames=0`。如果不加 `-framerate 30` 直接 ffplay，會以 25fps 播放 6 秒，這是 raw .h264 沒有時間戳造成的，不是 bug。

Bitrate 實驗（錄製時播放一段影片）：
```bat
dda_encode.exe --bitrate 2000000 --out out_2m.h264
dda_encode.exe --bitrate 8000000 --out out_8m.h264
```
預期檔案大小分別約 1.25 MB 與 5 MB（bitrate × 秒數 / 8）。

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [h264_encoder.cpp `Init` 第 1 段](common/h264_encoder.cpp) | `MFTEnumEx(HARDWARE)`，印出 friendly name；拿不到就 fallback 到 `CLSID_CMSH264EncoderMFT`（**軟體**，勘誤 E1）。activate 陣列每一個都要 Release，陣列本身用 `CoTaskMemFree`。 |
| [h264_encoder.cpp:234](common/h264_encoder.cpp#L234) | `SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)`（勘誤 E2）。 |
| [h264_encoder.cpp:246](common/h264_encoder.cpp#L246) | `SET_D3D_MANAGER`：讓 GPU texture 直接進 encoder。 |
| [h264_encoder.cpp 第 4 段](common/h264_encoder.cpp) | CODECAPI 要在 `SetOutputType` **之前**設定：CBR、低延遲、B-frame = 0、GOP。 |
| [h264_encoder.cpp:274 / :299](common/h264_encoder.cpp#L274) | 先 output type、再 input type；input type 從 `GetInputAvailableType` 裡找 NV12。 |
| [h264_encoder.cpp `OnEvent`](common/h264_encoder.cpp) | async 事件迴圈：`NeedInput` → 計數，`HaveOutput` → `ProcessOutput`，`DrainComplete` → 結束。每處理完一個事件要重新 `BeginGetEvent`。 |
| [h264_encoder.cpp:316](common/h264_encoder.cpp#L316) | `Encode()` 要等到有 `NeedInput` 才能送（否則回 `MF_E_NOTACCEPTING`）。 |
| [h264_encoder.cpp:129](common/h264_encoder.cpp#L129) | 輸出 sample 由誰配置：看 `PROVIDES_SAMPLES` flag。`pEvents` 也要 Release。 |
| [nv12_pool.h](common/nv12_pool.h) | texture pool：encoder 還持有 sample（refcount > 1）時就不重用。 |
| [lab4 main.cpp:132](lab4_encode/main.cpp#L132) | 時間戳：`MFllMulDiv(i, 10'000'000, fps, 0)`，先乘後除。 |
| [lab4 main.cpp 主迴圈](lab4_encode/main.cpp) | 恆定幀率：等到每一幀的時間點，期間有新畫面就更新 `latest`，沒有就重送。 |

### 動手弄壞它

| 改法 | 你會看到 |
|---|---|
| 刪掉 `encoder.Drain()` | `frames out` 少幾張（硬體 encoder 通常少 1~5 張） |
| 把 `SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)` 換成原文件的 `SetUnknown(..., nullptr)` | `SetOutputType` 回 `MF_E_TRANSFORM_ASYNC_LOCKED`（0xC00D6D77） |
| pool 大小從 8 改成 1，並刪掉 refcount 檢查（永遠回傳同一張） | 播放時偶爾畫面跳回前一張，或出現撕裂 |
| 主迴圈改成「只有 NewFrame 才編碼」 | 靜止桌面 5 秒只有幾十張；ffplay 播放時像在快轉 |
| 先 `SetInputType` 再 `SetOutputType` | 很多 encoder 會失敗，常見的是 `MF_E_TRANSFORM_TYPE_NOT_SET` 或 `MF_E_INVALIDMEDIATYPE`（依 encoder 而異） |

### 對照 mentor 指南

指南 Lab 4「症狀診斷表」裡的每一行，幾乎都能用上面的改法重現出來。

---

## Lab 5｜H.264 硬體解碼

### 跑起來

```bat
h264_decode.exe output.h264 --max 5
h264_decode.exe output.h264 --no-save --play
```

輸出（示意）：
```
output.h264: 812345 bytes, 153 NAL units, 150 access units (pictures), 3 keyframes
  NAL types: [type 1] x147 [type 5] x3 [type 7] x3 [type 8] x3 (1=slice 5=IDR 6=SEI 7=SPS 8=PPS 9=AUD)
Decoder: Microsoft H264 Decoder MFT, D3D11/DXVA=yes (adapter: NVIDIA GeForce RTX 3060)
  [decoder] output type: NV12 coded 1280x720, display 1280x720

Decoded 150 frames from 150 access units
  stream changes: 1 (first one = SPS seen)
```

**看懂這段輸出**：

- **153 個 NAL，但只有 150 張畫面**：多出來的是 SPS/PPS。NAL 數 ≠ 畫面數。
- `stream changes: 1`：decoder 看到 SPS 後回 `MF_E_TRANSFORM_STREAM_CHANGE`，重新協商了一次。
- `Decoded 150 frames from 150 access units`：兩邊**相等**。不相等就是沒 drain。

用 1080p 再跑一次，看 1088 的處理：
```bat
dda_encode.exe --width 1920 --height 1080 --out out1080.h264
h264_decode.exe out1080.h264 --max 3
```
會多印出：
```
  [decoder] output type: NV12 coded 1920x1088, display 1920x1080
  note: coded height 1088 > display height 1080 -> cropping the 8 alignment rows
```

用第三方產生的檔案測試（包含 B-frame）：
```bat
ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p" -c:v libx264 -bsf:v h264_mp4toannexb -f h264 test.h264
h264_decode.exe test.h264 --max 3
```

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [annexb.h `SplitNalUnits`](common/annexb.h) | 3-byte 與 4-byte start code 都要處理。 |
| [annexb.h `GroupAccessUnits`](common/annexb.h) | 什麼時候算新的一張畫面：AUD/SPS/PPS/SEI 出現在 slice 之後，或 `first_mb_in_slice == 0`。 |
| [h264_decoder.cpp `Init`](common/h264_decoder.cpp) | 用 `CoCreateInstance(CLSID_CMSH264DecoderMFT)`，不用 HARDWARE enum（勘誤 E7）。它是 sync MFT（勘誤 E6）。 |
| [h264_decoder.cpp:141](common/h264_decoder.cpp#L141) | `STREAM_CHANGE` → 從 `GetOutputAvailableType` 找 NV12 → `SetOutputType`。 |
| [h264_decoder.cpp:76](common/h264_decoder.cpp#L76) | `MF_MT_MINIMUM_DISPLAY_APERTURE`：1080 vs 1088。 |
| [h264_decoder.cpp:137](common/h264_decoder.cpp#L137) | `out.Attach(ob.pSample)`：decoder 給的 sample 由我們負責 Release。 |
| [h264_decoder.cpp:161](common/h264_decoder.cpp#L161) | `GetSubresourceIndex`：輸出是 texture array。 |
| [d3d_util.cpp `ReadbackNv12`](common/d3d_util.cpp) | `CopySubresourceRegion(..., subresource, ...)` 把 array 中的那一張拷到 staging texture。 |

### 動手弄壞它

| 改法 | 你會看到 |
|---|---|
| `STREAM_CHANGE` 時直接 `return false`（當成 NEED_MORE_INPUT） | `Decoded 0 frames`。這是學員最常卡關的地方 |
| `out.Attach(ob.pSample)` 改成不 Attach（洩漏） | D3D 模式下解到十幾張後就不再輸出 |
| 刪掉 `decoder.Drain()` | 少幾張 |
| `GetSubresourceIndex` 改成固定 0 | 每張存出來的圖都一樣，或順序錯亂 |
| `SaveNv12AsBmp` 傳 `codedHeight` 而不是 `displayHeight` | 1080p 的圖底部多 8 行綠條 |

---

## Lab 6｜端到端即時串流

### 跑起來

開兩個 console：
```bat
:: console 1
receiver.exe

:: console 2
sender.exe --res 720
```

Sender 輸出（示意）：
```
[config] 720p 1280x720, encoder: NVIDIA H.264 Encoder MFT (hw=yes, d3d=yes)
Streaming. Keys: '+' higher resolution, '-' lower resolution, ESC quit.
[720p] captured 58.9 fps | encoded 59.0 fps | skipped 0 | queue 0 | 6.41 Mbps | capture->encoded avg 4.2 ms max 7.9 ms
```
Receiver 輸出（同時也顯示在視窗標題）：
```
1280x720 59.0 fps 6.40 Mbps | (1)cap->enc 4.2/7.9 | (2)enc->dec 2.8/5.1 | (3)e2e 21.5/34.0 ms (avg/max) | dropped 0
```

拖動一個視窗或播放影片，fps 才會上來。桌面靜止時 sender 不會編碼，以節省頻寬。

### 看懂三個延遲數字

```
capture ──(1)──> encoded ──(2)──> decoded ──> presented
   └──────────────────(3)──────────────────────┘
```

- **(1) 擷取 → 編碼完成**：sender 在 encoder 的輸出 callback 裡，用 sampleTime 對回擷取時的 QPC 算出來，並寫進 header。
- **(2) 編碼完成 → 解碼完成**：包含 send queue、TCP、recv、decode。
- **(3) 端到端**：(3) 減掉 (1)+(2)，大部分是 `Present` 等 vsync（60Hz 最多 16.7 ms）。用 `receiver --novsync` 對照，(3) 應該明顯下降。

同一台機器的 QPC 是全系統一致的，所以可以跨 process 相減。跨機器時 receiver 會印出提示：(2)(3) 無意義。

### 實驗

| 實驗 | 指令 / 操作 | 觀察 |
|---|---|---|
| 解析度切換 | 在 sender console 按 `+` / `-` | receiver 印 `[stream] 1920x1080 (keyframe)`；不會黑屏、不需要重啟 |
| vsync 的影響 | `receiver --novsync` | (3) 通常會下降（少了等 vsync 的時間，60Hz 最多 16.7 ms） |
| 軟體 encoder | `sender --software` | (1) 明顯上升；sender 顯示 `d3d=no`（每幀都讀回 CPU） |
| **慢速網路 / 背壓** | `sender --res 1080 --throttle-mbps 3`，然後播放影片 | sender 的 `skipped` 開始增加，`queue` 維持在 3 左右**不會無限長大**，延遲也不會一直累積。這就是「在編碼前丟原始畫面」的背壓策略 |
| 斷線 | 關掉 receiver 視窗 | sender 印 `Receiver disconnected ... Stopping.` 並乾淨結束 |
| 斷線（反向） | 在 sender 按 ESC | receiver 印 `Sender disconnected.` 並結束 |
| 10 分鐘測試 | 工作管理員 → 詳細資料，加上「GPU 專用記憶體」欄位 | 兩個 process 的記憶體在第 1 分鐘後應該持平 |
| 跨機器 | `receiver`（機器 B）；`sender --host <B 的 IP>`（機器 A） | 防火牆要開 TCP 5000。(1) 仍然有效 |

### 程式導讀

| 位置 | 觀念 |
|---|---|
| [framing.h](common/framing.h) | 固定 36 bytes 的 header（magic + 長度 + 時間戳），big-endian。`Deserialize` 會檢查 magic 和長度上限。 |
| [net.h `RecvAll` / `SendAll`](common/net.h) | recv 不保證一次收滿，**一定要迴圈**。這個 bug 本機測試幾乎不會出現，跨機器才會爆。 |
| [net.h `SetNoDelay`](common/net.h) | 關掉 Nagle。 |
| [sender.cpp:47 `kSendQueueSoftLimit`](lab6_stream/sender.cpp#L47) | 背壓：queue 太長時在**編碼前**丟原始畫面，不丟已編碼的 P-frame。 |
| [sender.cpp `configure`](lab6_stream/sender.cpp) | 切換解析度 = 重建 scaler 輸出 + pool + encoder；新 encoder 的第一張必然是帶新 SPS 的 IDR。 |
| [sender.cpp send thread](lab6_stream/sender.cpp) | 網路 I/O 在獨立 thread；`SendAll` 失敗 → `stop` → 主迴圈結束 → 依序收尾。 |
| [receiver.cpp:39 `kMaxBacklog`](lab6_stream/receiver.cpp#L39) | 解碼跟不上時丟到下一個 keyframe 為止（sender 的 GOP = 2 秒）。 |
| [receiver.cpp `onFrame`](lab6_stream/receiver.cpp) | decoder texture → `RenderWindow::Present` → VideoProcessorBlt 到 back buffer，全程 GPU。 |
| [receiver.cpp 收尾](lab6_stream/receiver.cpp) | `shutdown(sock)` 讓阻塞中的 `recv` 返回，recv thread 才能 join。 |

### 「全程 GPU」檢查（Lab 6 的核心考點）

在 sender 的每幀路徑上，只有 [nv12_pool.h `MakeCpuNv12Sample`](common/nv12_pool.h) 會 readback，而且只在 `encoder.UsesD3D() == false`（軟體 fallback）時才會呼叫。用 mentor 指南 §3.2 的 grep 指令掃一遍這個專案，你會看到：

- `Map(`：只出現在 `d3d_util.cpp`（存 BMP）和 `render_window.cpp`（軟體解碼 fallback 的上傳）。
- `MFCreateMemoryBuffer`：出現在 encoder **輸出**（bitstream，本來就在 CPU）、decoder **輸入**（從網路收到的 bitstream），以及軟體 fallback。

這就是你 review 學員作業時，應該期待看到的分布。

### 動手弄壞它

| 改法 | 你會看到 |
|---|---|
| 註解掉兩端的 `SetNoDelay` | 本機差異不大；跨機器時 (2) 多出幾十 ms 且忽高忽低 |
| `RecvAll` 改成只呼叫一次 `recv` | 本機通常沒事；跨機器或 1080p 大 IDR 時，header 錯位 → `Invalid frame header` |
| 刪掉背壓判斷，搭配 `--throttle-mbps 3` | sender 的 queue 一直長大、延遲一直增加、記憶體一直上升 |
| 背壓改成「丟 queue 裡的 encoded frame」 | receiver 畫面花掉，直到下一個 IDR（最多 2 秒）才恢復 |
| 切換解析度時不重建 encoder（只改 scaler） | `ProcessInput` 失敗，或畫面錯亂 |

---

## 用參考實作 review 學員作業

1. **先跑學員的程式，再跑參考實作**，用同樣的參數比對輸出：張數、檔案大小、延遲數字的量級。
2. 學員的輸出有異狀時，對照本文件各題「動手弄壞它」的表格，大多數症狀都能在這裡找到對應的錯誤。
3. 學員的寫法和參考實作不同**不代表錯**。例如用 `IMFAsyncCallback` 自己實作事件迴圈、用 `IMFTrackedSample` 取代 refcount 檢查、用 Sink Writer，這些都是合理的選擇。要檢查的是 mentor 指南 checklist 上的**觀念**有沒有做到。
4. 口試時可以請學員「弄壞」自己的程式並預測症狀，這比問定義更能看出他是否真的理解。
