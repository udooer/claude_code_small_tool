# Windows Media Foundation 新人訓練 — Mentor 指南

> 對象：帶新人做《[WMF 新人訓練文件](WMF_Training_Guide.md)》Lab 1~6 的 mentor。
> 用途：(1) 讓 mentor 自己吃透每題「真正在考什麼」；(2) 提供參考解答骨架；(3) 提供可以直接拿來 review 學員作業的 checklist、症狀診斷表、口試追問與評分標準。
>
> 本文件**不要**直接發給學員，裡面有參考解答與追問的預期答案。
>
> **可執行的參考實作**在 [`labs/`](labs/README.md)；逐題帶讀與「故意弄壞」實驗見 [`labs/WALKTHROUGH.md`](labs/WALKTHROUGH.md)。

---

## 目錄

0. [怎麼使用這份文件](#0-怎麼使用這份文件)
1. [先讀：原訓練文件的勘誤與補充](#1-先讀原訓練文件的勘誤與補充)
2. [貫穿全部 Lab 的 5 個核心觀念](#2-貫穿全部-lab-的-5-個核心觀念)
3. [通用 Review Checklist 與 grep 速查](#3-通用-review-checklist-與-grep-速查)
4. [Lab 1｜桌面擷取存成圖片](#lab-1桌面擷取存成圖片)
5. [Lab 2｜GPU 縮放](#lab-2gpu-縮放)
6. [Lab 3｜BGRA → NV12 驗證](#lab-3bgra--nv12-驗證)
7. [Lab 4｜H264 硬體編碼](#lab-4h264-硬體編碼)
8. [Lab 5｜H264 硬體解碼](#lab-5h264-硬體解碼)
9. [Lab 6｜端到端即時串流](#lab-6端到端即時串流)
10. [附錄 A：症狀 → 原因 快速診斷表](#附錄-a症狀--原因-快速診斷表)
11. [附錄 B：評分表範本](#附錄-b評分表範本)

---

## 0. 怎麼使用這份文件

每個 Lab 都用同一個結構寫：

| 區塊 | 用途 |
|---|---|
| **真正在考什麼** | 一句話抓住核心。review 時先問自己：「學員有沒有理解這一點？」能跑起來不等於理解。 |
| **Mentor 背景知識** | 你自己要懂的原理，學員問你時能答。 |
| **參考解答骨架** | 關鍵程式碼，不是完整專案。用來對照學員的寫法。 |
| **Mentor 驗收步驟** | 你實際要怎麼跑、看哪些輸出。 |
| **Code Review Checklist** | 🔴 必須（不過就退件）／🟡 應該（要求修正但不擋）／🟢 加分。 |
| **症狀診斷表** | 看到學員的輸出長怎樣，就知道 bug 大概在哪。 |
| **口試追問** | 附預期答案。用來確認學員不是複製貼上。 |
| **評分** | 建議配分。 |

**建議的 review 流程（每題約 30~45 分鐘）**：

1. 先自己跑一次學員的程式（照 README），確認驗收標準。
2. 跑附錄的 grep 速查，快速找紅旗。
3. 照 checklist 看 code。
4. 挑 2~3 題口試追問，當面或用文字問。
5. 填評分表，回饋時**先講對的觀念，再講 bug**。

---

## 1. 先讀：原訓練文件的勘誤與補充

原文件整體方向正確，但有幾處**錯誤或會讓學員卡關的地方**。你要先知道，否則學員照文件寫卡住時，你會以為是學員的錯。建議擇期修正原文件，或在開訓時口頭補充。

### 1.1 錯誤（會造成編譯失敗、執行失敗或觀念錯誤）

| # | 位置 | 原文寫法 | 問題 | 正確作法 |
|---|---|---|---|---|
| E1 | §5.1 | `CLSID_CMSH264EncoderMFT` 在支援的硬體上會自動走 Quick Sync / NVENC / VCE | **錯。** `CLSID_CMSH264EncoderMFT` 是 Microsoft 的**軟體** encoder。硬體 encoder 是各家顯卡廠商另外註冊的 MFT（例如 "Intel® Quick Sync Video H.264 Encoder MFT"、"NVIDIA H.264 Encoder MFT"、"AMDh264Encoder"），要用 `MFTEnumEx` 加 `MFT_ENUM_FLAG_HARDWARE` 列舉才拿得到。 | 先用 `MFTEnumEx(HARDWARE)` 拿硬體 MFT；若 `count == 0`，再 fallback 到 `CLSID_CMSH264EncoderMFT`（同步、軟體）。`MFT_ENUM_HARDWARE_URL_Attribute` 只有硬體 MFT 才有，可以用來判斷。 |
| E2 | §5.3 | `encoderAttrs->SetUnknown(MF_TRANSFORM_ASYNC_UNLOCK, nullptr);` | **錯。** 這個 attribute 是 `UINT32`。用 `SetUnknown` 不會解鎖，之後呼叫其他方法會拿到 `MF_E_TRANSFORM_ASYNC_LOCKED`。 | `encoderAttrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);`，而且只有在 `MF_TRANSFORM_ASYNC == TRUE` 時才需要。 |
| E3 | §5.3 | async MFT「建立後要先呼叫 `MFT_MESSAGE_COMMAND_FLUSH` 之類…的流程」 | **錯。** FLUSH 是丟棄內部資料用的，不是啟動流程。 | Async MFT 的正確順序：unlock → `SET_D3D_MANAGER` → `SetOutputType` → `SetInputType` → 取 `IMFMediaEventGenerator` → `MFT_MESSAGE_NOTIFY_BEGIN_STREAMING` → `MFT_MESSAGE_NOTIFY_START_OF_STREAM` → 收到 `METransformNeedInput` 才能 `ProcessInput`、收到 `METransformHaveOutput` 才能 `ProcessOutput`。 |
| E4 | §4.3 | `videoContext->VideoProcessorSetStreamColorSpace1(...)`，`videoContext` 型別是 `ID3D11VideoContext` | **編譯失敗。** `...ColorSpace1` 系列在 `ID3D11VideoContext1`（`d3d11_1.h`）。 | `ComPtr<ID3D11VideoContext1> vc1; videoContext.As(&vc1);` 再呼叫。或用舊版 `VideoProcessorSetStreamColorSpace` 搭配 `D3D11_VIDEO_PROCESSOR_COLOR_SPACE` 結構。 |
| E5 | §5.5 | 即時強制 I-frame 用 `MFT_MESSAGE_COMMAND_MARK_IN_STREAM` | **錯。** MARK_IN_STREAM 是在資料流中插標記，跟 keyframe 無關。 | 只用 `ICodecAPI::SetValue(&CODECAPI_AVEncVideoForceKeyFrame, ...)`（值為 1），在送下一張 input 前設定。 |
| E6 | §6.5 | 「Decoder MFT 也是 async 為主」 | **錯。** Microsoft H264 Decoder（`CLSID_CMSH264DecoderMFT`）是**同步** MFT，設了 D3D manager 後內部走 DXVA 硬體解碼。 | 直接用同步 `ProcessInput`/`ProcessOutput` 迴圈即可。 |
| E7 | §6.2 | 用 `MFTEnumEx(DECODER, MFT_ENUM_FLAG_HARDWARE, ...)` 列舉解碼器並直接取 `[0]` | 微軟的 H264 decoder 是註冊成軟體 MFT（硬體加速是透過 DXVA），很多機器上 `HARDWARE` flag 列舉結果是 **0 個** → `activateArray[0]` 直接 crash。 | 直接 `CoCreateInstance(CLSID_CMSH264DecoderMFT)`，檢查 `MF_SA_D3D11_AWARE` attribute 為 TRUE 後再設 D3D manager。 |
| E8 | §6.4 | 只處理 `hr == S_OK` | 漏掉 **`MF_E_TRANSFORM_STREAM_CHANGE`**：MS decoder 解析到第一個 SPS 後**一定**會先回這個，必須重新 `GetOutputAvailableType` → `SetOutputType(NV12)` 才能繼續。沒處理就永遠拿不到畫面。 | 見 Lab 5 參考解答。 |
| E9 | §6.4 | `outputBuffer.pSample` 用完沒 Release | **記憶體洩漏**（而且洩漏的是 decoder surface pool 的 sample，跑一陣子 decoder 會因為沒 surface 可用而卡住）。同理 `outputBuffer.pEvents` 也要 Release。 | 用完立刻 `outputBuffer.pSample->Release()`，或用 `ComPtr<IMFSample>` 的 `Attach`。 |
| E10 | §3.2 | 「CPU (`StretchRect` 等)」 | 小錯。`StretchRect` 是 D3D9 的 GPU API。 | 改成「CPU（自己寫迴圈 / GDI `StretchBlt`）」。 |

### 1.2 遺漏（學員一定會踩到，但原文件沒講）

| # | 主題 | 說明 |
|---|---|---|
| M1 | **D3D11 device 建立 flag** | 要給 MF 用的 device 應加上 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`；並且**一定要**開多執行緒保護：`ComPtr<ID3D10Multithread> mt; device.As(&mt); mt->SetMultithreadProtected(TRUE);`。MFT 會在自己的 thread 用同一個 device，沒開保護會隨機 crash / 畫面損毀，很難查。 |
| M2 | **混合顯卡筆電（Optimus）** | §2.2 用預設 adapter 建 device，再 `GetAdapter()->EnumOutputs(0)`。在 Intel 內顯 + NVIDIA 獨顯的筆電上，預設 adapter 可能是獨顯，而螢幕接在內顯 → `EnumOutputs` 回 `DXGI_ERROR_NOT_FOUND`，或 `DuplicateOutput` 回 `DXGI_ERROR_UNSUPPORTED`。正確作法是用 `IDXGIFactory1::EnumAdapters1` 找到「擁有該 output 的 adapter」，再用該 adapter + `D3D_DRIVER_TYPE_UNKNOWN` 建 device。 |
| M3 | **`frameInfo.LastPresentTime == 0`** | `AcquireNextFrame` 成功不代表桌面畫面有更新，可能只是滑鼠移動。只看 `LastPresentTime != 0` 才代表有新的畫面內容。 |
| M4 | **Map 的 RowPitch** | `Map` 回來的 `RowPitch` 通常 **≠ width × 4**（有對齊 padding）。逐行拷貝時沒用 RowPitch → 圖片歪斜。這是 Lab 1/3/5 最常見的 bug。 |
| M5 | **GPU 是非同步的** | `VideoProcessorBlt` / `CopyResource` 呼叫回來時 GPU 還沒做完，只是把指令排進 queue。用 `QueryPerformanceCounter` 包住呼叫，量到的是「提交時間」不是「執行時間」（Lab 2 量測）。 |
| M6 | **MS decoder 輸出高度 1088** | 1920×1080 的 H264 實際編碼高度是 1088（16 對齊），decoder 輸出 texture 會是 1088 高，有效區域在 `MF_MT_MINIMUM_DISPLAY_APERTURE`。學員直接存整張 → 圖片底部多 8 行綠/灰色垃圾。 |
| M7 | **Raw `.h264` 沒有時間軸** | Annex B 原始 bitstream 沒有容器，時間戳不會被寫進檔案。ffplay/ffprobe 只能依 SPS 中的 VUI timing（若有）或**預設 25fps** 猜時長。所以 Lab 4 的「時間軸正確」不能用播放時長驗證，應該用 frame 數驗證（見 Lab 4）。 |
| M8 | **B-frame** | Main / High profile 可能產生 B-frame → 輸出順序 ≠ 輸入順序，decoder 也要多 buffer 幾張才能輸出 → 延遲增加。低延遲串流應設 `CODECAPI_AVEncMPVDefaultBPictureCount = 0`（或用 Baseline / Constrained Baseline），decoder 端也設 `CODECAPI_AVLowLatencyMode = TRUE`。 |
| M9 | **硬體 encoder 輸入 texture 生命週期** | Async encoder 收到 sample 後不會立刻用完。學員如果每一幀都 Blt 到「同一張」NV12 texture 再送進 encoder，下一張的 Blt 可能覆蓋 encoder 還沒讀的資料 → 畫面跳格、重複、撕裂。需要 texture pool（ring buffer，至少 3~4 張），或用 sample 的 `IMFTrackedSample` / 等待 encoder 釋放。 |
| M10 | **`MFCreateDXGISurfaceBuffer` 後設長度** | 有些 encoder 會檢查 buffer 的 current length，為 0 時直接拒收或輸出空。保險作法：`ComPtr<IMF2DBuffer> b2d; buf.As(&b2d); DWORD len; b2d->GetContiguousLength(&len); buf->SetCurrentLength(len);` |
| M11 | **TCP_NODELAY**（Lab 6） | 沒關 Nagle，小封包會被延遲合併，再遇上接收端 delayed ACK，常見多 40~200ms 延遲。低延遲串流一定要 `setsockopt(TCP_NODELAY)`。 |
| M12 | **Lab 5 frame 數「正負幾張都合理」** | 原文件說 encoder 可能會丟棄/合併 frame。在這個 Lab 的設定下（MF encoder、固定輸入），**正確實作應該完全相等**。少了幾張幾乎都是 encoder 或 decoder **沒有 drain**。建議驗收時要求相等，不等就追問原因。 |
| M13 | **Async encoder 的 STREAM_CHANGE**（實機踩到） | Intel Quick Sync 第一次 `ProcessOutput` 會回 `MF_E_TRANSFORM_STREAM_CHANGE`。重設 output type 之後，**必須等下一個 `METransformHaveOutput` 事件**才能再呼叫 `ProcessOutput`；立刻重呼叫會得到 `0x8000FFFF E_UNEXPECTED`，而且錯誤訊息完全看不出原因。Sync MFT（軟體 encoder、MS decoder）則可以立刻重呼叫。學員的 Lab 4 在 Intel 機器上出現 `E_UNEXPECTED` 時，第一個檢查這裡。 |

---

## 2. 貫穿全部 Lab 的 5 個核心觀念

Review 每一題時，都可以用這 5 個觀念去檢視學員。學員能用自己的話講出這 5 點，才算真的學會。

### 觀念 1：資料留在 GPU（GPU Residency）

```
CPU 能碰的只有：  (a) 編碼後的 bitstream    (b) 除錯用的一次性存檔
其他全部是：      ID3D11Texture2D → ID3D11Texture2D → IMFSample(DXGI buffer)
```

- 一張 1080p BGRA 是 8MB，60fps 就是 480MB/s。GPU→CPU readback 不只是頻寬問題，還會**讓 CPU 等 GPU 把 queue 跑完**（pipeline stall），延遲直接爆。
- **隱形的 readback**：程式碼裡沒有 `Map`，但 encoder 沒設 D3D manager，或拿到的是軟體 encoder，MF 會在內部偷偷 readback。所以「grep 不到 Map」≠「沒有 readback」。

### 觀念 2：資源生命週期（誰擁有、活多久）

| 資源 | 活到什麼時候 |
|---|---|
| DDA 的 desktop texture | 到 `ReleaseFrame` 為止，要保留就 `CopyResource` |
| 送進 async encoder 的 input texture | 到 encoder 用完為止（不是 `ProcessInput` 回來時） |
| Decoder 輸出 sample | 你 `Release` 為止；不 Release 就佔住 decoder 的 surface pool |
| `MFTEnumEx` 的 activate 陣列 | 每個都要 Release，陣列本身 `CoTaskMemFree` |

### 觀念 3：格式協商（MediaType 是合約）

- MFT 不是「丟資料就會動」，要先跟它談好輸入/輸出格式。談不攏就是 `MF_E_INVALIDMEDIATYPE`，忘了談就是 `MF_E_TRANSFORM_TYPE_NOT_SET`。
- 格式會**中途改變**：decoder 看到 SPS 後回 `MF_E_TRANSFORM_STREAM_CHANGE`，要重新談。

### 觀念 4：管線與延遲（Pipeline / Buffering）

- Encoder/decoder 內部都有 buffer。`NEED_MORE_INPUT` 不是錯誤，是「我還在攢資料」。
- 送完最後一張要 **drain**，否則最後幾張永遠出不來。
- 延遲的來源：lookahead、B-frame、內部 queue、網路 buffering (Nagle)、vsync。低延遲就是一個一個把這些 buffer 拿掉。

### 觀念 5：色彩空間是兩端的合約

- RGB→YUV 的「係數（601/709）」與「range（full/studio）」只是一組約定，編碼端與解碼/顯示端必須一致。
- 灰階（R=G=B）時 U=V=128，**係數錯了灰階看不出來**，只有飽和色會偏。range 錯了則整體對比會變。這就是 Lab 3 要用純色色塊測試的原因。

---

## 3. 通用 Review Checklist 與 grep 速查

### 3.1 每一題都要看的項目

| 等級 | 項目 | 怎麼看 |
|---|---|---|
| 🔴 | 所有 COM 物件用 `ComPtr`（或等價 RAII），沒有裸指標忘記 Release | grep `->Release()`、`**`、`IMFActivate**` |
| 🔴 | 每個回傳 `HRESULT` 的呼叫都有檢查 | 找沒接回傳值的 `->Create...`、`->Set...`、`->ProcessInput` |
| 🔴 | 失敗時印出 **HRESULT 數值 + 是哪個呼叫失敗**，不是只印 "failed" | 要有 `0x%08X` 之類的輸出 |
| 🔴 | 硬體不支援時印出原因並正常結束，不 crash | 檢查 `count == 0`、`activateArray[0]` 前有沒有判斷 |
| 🔴 | `MFStartup`/`MFShutdown`、`CoInitializeEx`/`CoUninitialize` 成對 | 而且 `MFShutdown` 時 ComPtr 都已經釋放（注意 ComPtr 在 `main` 的 scope 結尾才解構，可能晚於 `MFShutdown`） |
| 🟡 | Debug build 開 D3D debug layer（`D3D11_CREATE_DEVICE_DEBUG`），結束時沒有 live object 警告 | 請學員貼 Output 視窗 |
| 🟡 | 有 README：怎麼編、參數、選了什麼策略 | |
| 🟢 | 有簡單的 `CHECK_HR(expr)` macro 統一錯誤處理 | |

> 小陷阱：`MFShutdown()` 寫在 `main` 最後，但 `ComPtr` 是 `main` 的區域變數 → 解構發生在 `MFShutdown` **之後**。通常不會炸，但屬於不嚴謹。好的寫法是把工作放進一個函式或 `{}` scope，scope 結束後再 `MFShutdown`。

### 3.2 grep 速查（找 GPU→CPU 搬運與常見紅旗）

在學員專案根目錄執行（PowerShell 可用 `Select-String`）：

```bash
# GPU→CPU 搬運的嫌疑點：每一個都要能說明「為什麼這裡可以」
grep -nE "Map\(|D3D11_USAGE_STAGING|D3D11_CPU_ACCESS_READ|MFCreateMemoryBuffer|->Lock\(|Lock2D|GetDC\(" -r .

# 生命週期 / 洩漏嫌疑
grep -nE "pSample|pEvents|CoTaskMemFree|activateArray|->Release\(\)" -r .

# 每幀建立資源（應該 pool 化）
grep -nE "CreateTexture2D|CreateVideoProcessor(Input|Output)View|MFCreateSample" -r .

# 錯誤處理覆蓋
grep -nE "ACCESS_LOST|WAIT_TIMEOUT|NEED_MORE_INPUT|STREAM_CHANGE|ACCESSDENIED" -r .
```

判讀原則：

- `Map` / `STAGING` 出現在**存 BMP 的函式**裡 → OK（Lab 1~5 允許）。
- 出現在**每幀的主迴圈**裡（Lab 6 sender/receiver 路徑）→ 🔴 退件。
- `MFCreateMemoryBuffer` 用在 **encoder 輸出** → OK（bitstream 本來就在 CPU）；用在 **encoder 輸入** → 🔴。
- `CreateTexture2D` 在每幀迴圈裡 → 🟡（效能差，Lab 6 應改 pool）。

---

## Lab 1｜桌面擷取存成圖片

> 實機問答與複習題：[labs/lab1_capture/NOTES.md](labs/lab1_capture/NOTES.md)

### 真正在考什麼

1. **DDA 的生命週期契約**：Acquire → 用 → (Copy) → Release，以及哪些錯誤是「正常」、哪些要重建。
2. **GPU texture 怎麼安全地讀回 CPU**：staging texture + `Map` + **RowPitch**。

### Mentor 背景知識

- DDA 的 texture 是 DWM 的 surface，只借你用到 `ReleaseFrame`。它的 `Usage` 是 DEFAULT、沒有 CPU access，**不能直接 Map**，一定要 `CopyResource` 到 `D3D11_USAGE_STAGING` + `D3D11_CPU_ACCESS_READ` 的 texture。
- 第一次 `AcquireNextFrame` 通常會拿到完整桌面；但如果拿到 `LastPresentTime == 0`（只有滑鼠更新），texture 內容不保證是新的。穩健的寫法是重試到 `LastPresentTime != 0`。
- BMP 格式重點：`BITMAPFILEHEADER` + `BITMAPINFOHEADER`；32bpp 每行剛好 4 的倍數不用 padding；**預設是 bottom-up**（`biHeight > 0` 時第一行存的是最底下那行）。用 `biHeight = -height` 可以 top-down 直接寫。
- 鎖定畫面（Win+L）時處於 secure desktop，一般權限的程式 `DuplicateOutput` 會回 **`E_ACCESSDENIED`**；如果是擷取途中才鎖定，`AcquireNextFrame` 會回 **`DXGI_ERROR_ACCESS_LOST`**，重建時才會拿到 `E_ACCESSDENIED`。
  - 實際測法：讓程式先 `Sleep(5000)` 再開始建立，啟動後馬上按 Win+L。或用工作排程器延遲執行。

### 參考解答骨架

```cpp
// 1) 找到 output 與它所屬的 adapter（處理多螢幕/混合顯卡）
ComPtr<IDXGIFactory1> factory;
CHECK_HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));

ComPtr<IDXGIAdapter1> adapter;
ComPtr<IDXGIOutput>   output;
UINT globalIndex = 0;
for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
        if (globalIndex++ == requestedIndex) goto found;
        output.Reset();
    }
    adapter.Reset();
}
fprintf(stderr, "Output index %u not found\n", requestedIndex);
return 1;
found:

// 2) 在「擁有該 output 的 adapter」上建 device（adapter 非 null 時 driver type 必須是 UNKNOWN）
CHECK_HR(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
    nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));

ComPtr<IDXGIOutput1> output1;
CHECK_HR(output.As(&output1));
HRESULT hr = output1->DuplicateOutput(device.Get(), &dup);
if (FAILED(hr)) {
    // E_ACCESSDENIED: secure desktop (鎖屏/UAC)
    // DXGI_ERROR_UNSUPPORTED: 混合顯卡 adapter 不對、或遠端桌面 session
    // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: 同時使用 DDA 的程式太多
    fprintf(stderr, "DuplicateOutput failed: 0x%08X\n", hr);
    return 1;
}

// 3) 取一張「真的有畫面更新」的 frame
ComPtr<ID3D11Texture2D> staging;
for (int retry = 0; retry < kMaxRetry; ++retry) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> res;
    hr = dup->AcquireNextFrame(500, &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;          // 正常：畫面沒變
    if (FAILED(hr)) { /* ACCESS_LOST 等：印出並結束或重建 */ return 1; }

    if (info.LastPresentTime.QuadPart != 0) {             // 有新畫面
        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);
        D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        CHECK_HR(device->CreateTexture2D(&d, nullptr, &staging));
        context->CopyResource(staging.Get(), tex.Get());  // 必須在 ReleaseFrame 之前
    }
    dup->ReleaseFrame();                                   // 每次成功 Acquire 都要配一次
    if (staging) break;
}

// 4) Map + 逐行拷貝（用 RowPitch！）
D3D11_MAPPED_SUBRESOURCE m;
CHECK_HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)); // 這裡 CPU 會等 GPU 做完 Copy
std::vector<uint8_t> pixels(width * height * 4);
for (UINT y = 0; y < height; ++y)
    memcpy(&pixels[y * width * 4], (uint8_t*)m.pData + y * m.RowPitch, width * 4);
context->Unmap(staging.Get(), 0);
SaveBmp32("capture.bmp", pixels.data(), width, height); // biHeight = -height (top-down)
```

### Mentor 驗收步驟

1. 單螢幕跑一次，打開 `capture.bmp`，檢查：顏色、文字清晰、沒有歪斜、沒有上下顛倒。
2. 若有 Windows 縮放 150%：圖片尺寸應該是**實體解析度**（例如 3840×2160），不是 2560×1440。
3. 延遲 5 秒啟動 + Win+L，確認印出 `E_ACCESSDENIED`（或 ACCESS_LOST）而不是 crash。
4. 多螢幕：`--output 1` 抓第二螢幕（加分）。
5. 一個很快的壓力測試：迴圈連抓 100 次（請學員臨時改），確認不會第 2 次就失敗（檢查 ReleaseFrame 有沒有配對）。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | `ReleaseFrame` 與每次**成功**的 `AcquireNextFrame` 配對（timeout 時不要呼叫 ReleaseFrame——呼叫了會回 `DXGI_ERROR_INVALID_CALL`，無害但代表不懂契約） |
| 🔴 | `CopyResource` 在 `ReleaseFrame` **之前** |
| 🔴 | Map 時使用 `RowPitch` |
| 🔴 | `WAIT_TIMEOUT` 有重試上限並印訊息 |
| 🔴 | `DuplicateOutput` 失敗時印 HRESULT，不 crash |
| 🟡 | 檢查 `LastPresentTime != 0` |
| 🟡 | 解析度取自 texture desc 或 `DXGI_OUTPUT_DESC`，**不是** `GetSystemMetrics`（DPI 虛擬化會給錯的值） |
| 🟢 | 用 `EnumAdapters1` 正確處理多 adapter |
| 🟢 | 處理螢幕旋轉（`DXGI_OUTDUPL_DESC::Rotation`，旋轉的螢幕拿到的 texture 是未旋轉的） |

### 症狀診斷表

| 學員的輸出 | 原因 |
|---|---|
| 圖片斜斜的、像被剪切 | 沒用 `RowPitch`，用了 `width*4` |
| 上下顛倒 | BMP 是 bottom-up，`biHeight` 正值卻從第一行開始寫 |
| 紅藍互換（藍色天空變橘色） | 把 BGRA 當 RGBA 寫，或 BMP 寫入時自己換了通道（BMP 本來就是 BGR(A) 順序，DDA 的 BGRA 可直接寫） |
| 全黑 | 拿到 `LastPresentTime == 0` 的 frame、或 ReleaseFrame 後才 Copy、或 Map 一個 DEFAULT texture（會失敗而學員沒檢查 hr） |
| 圖片尺寸比螢幕小（例如 2560×1440 而實際 4K） | 用 `GetSystemMetrics` 而程式不是 DPI aware |
| 第二次執行 / 第二次 Acquire 失敗 | 沒有 `ReleaseFrame` |
| 筆電上 `DuplicateOutput` 回 `0x887A0004` (UNSUPPORTED) | 混合顯卡，device 建在錯的 adapter |

### 口試追問

1. **Q：為什麼一定要 `CopyResource` 到另一張 texture，不能直接 Map DDA 給的 texture？**
   A：兩個原因。(1) DDA texture 是 DEFAULT usage、沒有 CPU access 權限，Map 會失敗；(2) 它只借到 `ReleaseFrame`，之後 DWM 會覆寫。
2. **Q：`WAIT_TIMEOUT` 跟 `ACCESS_LOST` 處理方式有什麼不同？為什麼？**
   A：TIMEOUT 是「畫面沒變」，正常，繼續等；ACCESS_LOST 代表 duplication 物件已失效（模式切換、secure desktop、GPU reset），必須整個 Release 後重建 duplication（甚至 device）。
3. **Q：你的 `Map` 呼叫花多久？為什麼可能比 `CopyResource` 久很多？**
   A：`CopyResource` 只是排進 GPU queue，`Map` 會讓 CPU **等 GPU 把 Copy 做完**才返回。這就是後面 Lab 一直強調「不要在熱路徑 Map」的原因。
4. **Q：timeout 設 500ms，如果改成 0 會怎樣？**
   A：變成忙碌輪詢，CPU 空轉；DDA 本身就是「有變化才推」的模型，用合理的 timeout 讓 thread 睡著。

### 評分（Lab 1，100 分）

| 項目 | 分數 |
|---|---|
| 圖片正確（顏色/方向/無歪斜） | 30 |
| Acquire/Release 生命週期正確 | 25 |
| 錯誤處理（TIMEOUT 重試、鎖屏不 crash、印 HRESULT） | 20 |
| COM/RAII 正確 | 15 |
| 加分：多螢幕、DPI、旋轉 | 10 |

---

## Lab 2｜GPU 縮放

### 真正在考什麼

1. 會用 `ID3D11VideoProcessor` 這套 API（Enumerator → Processor → InputView/OutputView → Blt）。
2. **長寬比 / 座標矩形的思考**（source rect / dest rect / 背景色）。
3. **正確量測 GPU 時間**（不被非同步騙）。

### Mentor 背景知識

- **ContentDesc 只是「提示」**：`CreateVideoProcessorEnumerator` 的 `InputWidth/Height`、`OutputWidth/Height` 用來讓驅動選最適合的 processor。真正的縮放由 `SetStreamSourceRect` / `SetStreamDestRect` / `SetOutputTargetRect` 決定。
- **輸出 texture 要求**：`D3D11_BIND_RENDER_TARGET`。格式是否支援應先用 `ID3D11VideoProcessorEnumerator::CheckVideoProcessorFormat` 檢查（`D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT/OUTPUT`）。
- **Letterbox 的三個 rect**：
  - Stream source rect：從來源取哪一塊（全圖）。
  - Stream dest rect：貼到輸出的哪一塊（等比縮放後置中的那塊）。
  - Output 背景：dest rect 以外的區域，用 `VideoProcessorSetOutputBackgroundColor` 填黑。**沒設背景色時，黑邊區域內容是未定義的**（可能是前一張殘影）。
- **Letterbox 計算**（3840×2160 → 1280×720 剛好同比例；測試時應該用**不同比例**，例如 1280×1024 或 1024×1024 才看得出策略）：

  ```cpp
  float s = std::min((float)dstW / srcW, (float)dstH / srcH);
  LONG w = (LONG)(srcW * s) & ~1, h = (LONG)(srcH * s) & ~1;  // 取偶數，後面 NV12 需要
  LONG x = (dstW - w) / 2 & ~1, y = (dstH - h) / 2 & ~1;
  RECT dst = { x, y, x + w, y + h };
  ```
- **GPU 計時的正確方式**（二選一）：
  - 簡易：`Blt` 後用 `ID3D11Query`（`D3D11_QUERY_EVENT`）`End()`，然後 `while (context->GetData(query, ...) == S_FALSE)` 等到完成再取 QPC。量到的是「提交 + GPU 執行」。
  - 精確：`D3D11_QUERY_TIMESTAMP_DISJOINT` + 兩個 `D3D11_QUERY_TIMESTAMP`，量純 GPU 時間。
  - 參考量級：獨顯/近年內顯做 4K→720p 的 VideoProcessorBlt 大約 **0.3~2 ms**。學員報 **0.01~0.05 ms** → 幾乎一定是只量到提交時間。報 **> 20 ms** → 可能包含了 Map/存檔，或每次都重建 processor。

### 參考解答骨架

```cpp
D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
cd.InputWidth = srcW;  cd.InputHeight = srcH;
cd.OutputWidth = dstW; cd.OutputHeight = dstH;
cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
CHECK_HR(videoDevice->CreateVideoProcessorEnumerator(&cd, &vpEnum));

UINT flags = 0;
CHECK_HR(vpEnum->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &flags));
if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) { /* 印出並結束 */ }

CHECK_HR(videoDevice->CreateVideoProcessor(vpEnum.Get(), 0, &vp));

// 輸出 texture（BGRA, 目標尺寸）
D3D11_TEXTURE2D_DESC od{};
od.Width = dstW; od.Height = dstH; od.MipLevels = 1; od.ArraySize = 1;
od.Format = DXGI_FORMAT_B8G8R8A8_UNORM; od.SampleDesc.Count = 1;
od.Usage = D3D11_USAGE_DEFAULT; od.BindFlags = D3D11_BIND_RENDER_TARGET;
CHECK_HR(device->CreateTexture2D(&od, nullptr, &outTex));

// views（略，同訓練文件 3.4）

// 黑邊
D3D11_VIDEO_COLOR black{}; black.RGBA = { 0, 0, 0, 1 };
videoContext->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &black);
videoContext->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcRect);
videoContext->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &letterboxRect);
videoContext->VideoProcessorSetOutputTargetRect(vp.Get(), TRUE, &fullOutputRect);

// 計時：提交 + 等 GPU 完成
LARGE_INTEGER t0, t1, freq; QueryPerformanceFrequency(&freq);
QueryPerformanceCounter(&t0);
CHECK_HR(videoContext->VideoProcessorBlt(vp.Get(), outView.Get(), 0, 1, &stream));
context->End(eventQuery.Get());
while (context->GetData(eventQuery.Get(), nullptr, 0, 0) == S_FALSE) { /* spin */ }
QueryPerformanceCounter(&t1);
printf("Scale: %.3f ms\n", (t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart);
```

### Mentor 驗收步驟

1. `--width 1280 --height 720`：輸出尺寸正確、文字可讀。
2. **刻意用不同比例** `--width 1024 --height 1024`：確認 letterbox 黑邊乾淨（或拉伸），且跟 README 寫的策略一致。
3. 看計時數字是否落在合理範圍（見上），問學員量的是什麼。
4. 確認從 Acquire 到 Blt 完成之間沒有 Map（只有存檔前一次）。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | 縮放路徑中沒有 Map / CPU 迴圈（只有存檔時一次） |
| 🔴 | 長寬比策略有實作且在 README/註解說明 |
| 🔴 | 輸出 texture 有 `BIND_RENDER_TARGET` |
| 🟡 | 有設背景色（letterbox 情況） |
| 🟡 | 計時有等 GPU 完成（Query / 或至少學員能說明自己量到的只是提交時間） |
| 🟡 | 用 `CheckVideoProcessorFormat` 檢查格式支援 |
| 🟢 | 用 Timestamp query 量純 GPU 時間 |
| 🟢 | 寬高強制偶數（為 Lab 3 NV12 鋪路） |

### 症狀診斷表

| 學員的輸出 | 原因 |
|---|---|
| 黑邊區域有殘影 / 雜訊 | 沒設 `OutputBackgroundColor`，或 `OutputTargetRect` 沒涵蓋全畫面 |
| 輸出只有左上角一塊有畫面 | dest rect 設成 source 的尺寸、或沒呼叫 `SetStreamDestRect` |
| 畫面被裁掉 | source rect 設錯（例如用了目標尺寸） |
| `CreateVideoProcessorInputView` 回 `E_INVALIDARG` | 直接拿 DDA 的 texture、或 input texture 來自別的 device；也可能 `FourCC` / `ViewDimension` 沒填 |
| 計時 0.01ms | 只量到提交 |
| 文字非常糊 | 縮放倍率太大且驅動用 bilinear（正常現象，可討論）；或先縮小再放大兩次 |

### 口試追問

1. **Q：ContentDesc 裡的寬高跟 SetStreamDestRect 的寬高，哪個才真正決定縮放結果？**
   A：rect 決定；ContentDesc 是讓驅動挑選 processor 能力的提示。
2. **Q：你量到 0.05ms，為什麼這個數字可能是錯的？**
   A：D3D11 呼叫是非同步，只量到把指令排進 queue；要用 query 等 GPU 做完。
3. **Q：如果解析度要隨網路頻寬動態改變，哪些物件要重建、哪些可以重用？**
   A：output texture + output view 要換（或預先 pool 多種尺寸）；processor 原則上可重用（只改 rect），但若尺寸差距大，嚴謹作法是重建 enumerator/processor。input view 隨來源 texture 綁定。
4. **Q：為什麼建議寬高取偶數？**
   A：下一步要轉 NV12（4:2:0），色度是寬高各半，奇數尺寸無法對齊；encoder 也要求偶數。

### 評分（Lab 2，100 分）

| 項目 | 分數 |
|---|---|
| 縮放結果正確 | 30 |
| 全程 GPU（無中途 readback） | 25 |
| 長寬比策略實作 + 說明 | 20 |
| 計時正確性與理解 | 15 |
| 程式品質 | 10 |

---

## Lab 3｜BGRA → NV12 驗證

### 真正在考什麼

1. **NV12 記憶體佈局**：Y plane 全解析度 + UV plane 半解析度且 U/V 交錯，兩個 plane 在 Map 之後**接在同一塊記憶體**。
2. **色彩空間合約**：係數（601/709）× range（full/studio）共 4 種組合，學員要知道 GPU 那端用哪一種、自己手刻的反轉換用哪一種，**兩者必須一致**。
3. 寫出「設錯會怎樣」的那 3~5 句話——這是確認理解的關鍵。

### Mentor 背景知識

#### NV12 在 D3D11 Map 後的佈局

```
pData ──► ┌──────────── RowPitch ────────────┐
          │ Y Y Y Y Y Y Y Y ... (width bytes) │  ← 第 0 行
          │ ...                               │  共 Height 行
          ├───────────────────────────────────┤ ← pData + RowPitch * Height
          │ U V U V U V ... (width bytes)     │  ← UV 第 0 行（對應 Y 的第 0、1 行）
          │ ...                               │  共 Height/2 行
          └───────────────────────────────────┘
```

- UV plane 起點 = `pData + RowPitch * Height`（Height 是 texture 的高）。兩個 plane 的 pitch 相同。
- 像素 (x, y) 的取值：
  - `Y = yPlane[y * pitch + x]`
  - `U = uvPlane[(y/2) * pitch + (x/2)*2 + 0]`
  - `V = uvPlane[(y/2) * pitch + (x/2)*2 + 1]`
- 最常見錯誤：UV 偏移用 `width * height`（應用 pitch）、U/V 順序顛倒、用 `x` 而非 `(x/2)*2` 取 UV。

#### 4 組反轉換公式（YUV → RGB，8-bit）

Studio（limited）range，先令 `y = Y - 16`、`u = U - 128`、`v = V - 128`：

| | R | G | B |
|---|---|---|---|
| **BT.709 limited** | `1.164y + 1.793v` | `1.164y − 0.213u − 0.533v` | `1.164y + 2.112u` |
| **BT.601 limited** | `1.164y + 1.596v` | `1.164y − 0.392u − 0.813v` | `1.164y + 2.017u` |

Full range，令 `y = Y`、`u = U - 128`、`v = V - 128`：

| | R | G | B |
|---|---|---|---|
| **BT.709 full** | `y + 1.575v` | `y − 0.187u − 0.468v` | `y + 1.856u` |
| **BT.601 full** | `y + 1.402v` | `y − 0.344u − 0.714v` | `y + 1.772u` |

結果都要 clamp 到 [0, 255]。沒 clamp → 飽和色出現奇怪的反色點（溢位繞回）。

> 原文件 §4.3 的設定是 input `RGB_FULL_G22_NONE_P709`、output `YCBCR_STUDIO_G22_LEFT_P709` → 學員應該用 **BT.709 limited** 公式。

#### 標準色塊對照表（BT.709 limited，用來檢查 GPU 端輸出）

請學員在 `SaveNV12AsBmp` 裡額外印出色塊中心點的 Y/U/V 原始值，跟下表比對（±2 都算正常）：

| 顏色 (RGB) | Y | U (Cb) | V (Cr) |
|---|---|---|---|
| 黑 (0,0,0) | 16 | 128 | 128 |
| 白 (255,255,255) | 235 | 128 | 128 |
| 紅 (255,0,0) | 63 | 102 | 240 |
| 綠 (0,255,0) | 173 | 42 | 26 |
| 藍 (0,0,255) | 32 | 240 | 118 |

BT.601 limited 的紅會是 Y=82、U=90、V=240；綠 Y=145、U=54、V=34；藍 Y=41、U=240、V=110。

**用途**：如果學員印出紅色 Y≈82 → GPU 端實際用的是 601（例如 ColorSpace 沒設成功，D3D11 預設通常是 BT.601）；如果白色 Y=255、黑色 Y=0 → GPU 端輸出 full range。這張表讓你**不用看圖就能判斷 GPU 那端到底輸出什麼**。

#### 「設錯會怎樣」—— 學員報告的標準答案

學員那 3~5 句話至少要涵蓋下列 3 點中的 2 點：

1. **range 錯**：
   - studio 資料當 full 解 → 黑變深灰（16）、白變淺灰（235），整體**對比降低、霧霧的**（washed out）。
   - full 資料當 studio 解 → 暗部被壓成全黑、亮部爆白，**對比過高、細節消失**（crushed blacks / clipped whites）。
2. **係數錯（601 ↔ 709）**：灰階**完全不受影響**（因為 U=V=128 時係數不起作用），只有**飽和色偏移**，而且幅度不大（約 15~40 個數值），肉眼往往只覺得「有點怪」：
   - 709 編、601 解：純紅 → (233,0,2) 變暗；純綠 → (20,255,9) 偏黃綠。
   - 601 編、709 解：純紅 → (255,24,0) 偏橘；純綠 → (0,216,0) 變暗。
   所以必須用純色色塊**量測**，不能靠肉眼。
3. **U/V 對調** → 紅藍互換（紅色變藍紫、天空變橘）；取 UV 的索引錯 → 色彩錯位、條紋。

> 判斷學員有沒有真的懂：問「為什麼你的測試圖要用純紅/純綠，用黑白文件測不行嗎？」——答得出「灰階 U=V=128，係數錯了看不出來」的才是真懂。

### 參考解答骨架

```cpp
// GPU 端：輸出 NV12，並明確設定 color space（注意 ID3D11VideoContext1）
ComPtr<ID3D11VideoContext1> vc1;
CHECK_HR(videoContext.As(&vc1));
vc1->VideoProcessorSetStreamColorSpace1(vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
vc1->VideoProcessorSetOutputColorSpace1(vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);

// CPU 端（除錯用）：NV12 staging → BGRA
void SaveNV12AsBmp(ID3D11DeviceContext* ctx, ID3D11Texture2D* nv12Staging, UINT w, UINT h, const char* path)
{
    D3D11_MAPPED_SUBRESOURCE m;
    CHECK_HR(ctx->Map(nv12Staging, 0, D3D11_MAP_READ, 0, &m));
    const uint8_t* yP  = (const uint8_t*)m.pData;
    const uint8_t* uvP = yP + m.RowPitch * h;   // h = texture 高度

    std::vector<uint8_t> bgra(w * h * 4);
    for (UINT y = 0; y < h; ++y) {
        for (UINT x = 0; x < w; ++x) {
            int Y = yP[y * m.RowPitch + x];
            const uint8_t* uv = uvP + (y / 2) * m.RowPitch + (x / 2) * 2;
            float c = 1.164f * (Y - 16), d = uv[0] - 128.f, e = uv[1] - 128.f;  // d=U, e=V
            uint8_t* p = &bgra[(y * w + x) * 4];
            p[2] = Clamp(c + 1.793f * e);                  // R
            p[1] = Clamp(c - 0.213f * d - 0.533f * e);     // G
            p[0] = Clamp(c + 2.112f * d);                  // B
            p[3] = 255;
        }
    }
    ctx->Unmap(nv12Staging, 0);
    SaveBmp32(path, bgra.data(), w, h);
}
```

### Mentor 驗收步驟

1. 把 Lab 2 的 BGRA 輸出跟 Lab 3 的 NV12→BMP 放在一起比對（可用 Beyond Compare 的圖片比對、或兩張開在同一個看圖軟體快速切換）。
2. 用色塊測試圖（建議你**自己準備一張統一的測試圖**給所有學員：紅/綠/藍/白/黑/50% 灰 + 一段黑底白字 + 一段彩色文字），檢查色塊。
3. 要求學員印出色塊中心的 Y/U/V 值，對照上表。
4. 讀學員的「設錯會怎樣」那段文字，對照上面的標準答案。
5. 加分檢查：請學員**故意**把公式換成 full range 或 601，截圖給你看，描述的現象要跟他寫的一致。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | UV plane 偏移用 `RowPitch * Height` |
| 🔴 | U 在前 V 在後 |
| 🔴 | 公式與 GPU 端設定的 color space 一致，且文件有寫明 |
| 🔴 | 有 clamp |
| 🔴 | 沒有偷用第二個 Video Processor 轉回 RGB（題目禁止） |
| 🟡 | 明確設定了 stream / output color space（沒設 = 依賴驅動預設，不可靠） |
| 🟡 | 寬高為偶數 |
| 🟢 | 有印出 YUV 原始值來交叉驗證 |
| 🟢 | 色度做雙線性內插而非最近鄰（題目沒要求） |

### 症狀診斷表

| 學員的輸出 | 原因 |
|---|---|
| 整體偏綠 | UV 全部讀到 0（偏移錯、讀到 Y plane 以外的空白），或把 U/V 當成無號直接用沒減 128 |
| 偏粉紅/紫 | UV 讀到過大的值、或 U/V 偏移錯位 |
| 紅藍互換 | U/V 對調 |
| 霧霧的、黑不夠黑 | GPU 輸出 studio，學員用 full 公式 |
| 對比過高、暗部全黑 | GPU 輸出 full，學員用 studio 公式 |
| 灰階正確但紅色偏橘或綠色怪怪的 | 601/709 不一致（常見：GPU 沒設成功 → 預設 601，學員用 709） |
| 下半部畫面重複 / 彩色雜訊 | 把 UV plane 當成 Y 解，或 h 用錯 |
| 斜紋 | 沒用 RowPitch |
| 飽和色邊緣有亮點/黑點 | 沒 clamp，溢位 |
| 只有左半邊有顏色 | UV 用 `x/2` 而不是 `(x/2)*2` 取 byte 位置 |

### 口試追問

1. **Q：1920×1080 的 NV12 一張佔多少 bytes？跟 BGRA 比呢？**
   A：1920×1080×1.5 ≈ 3.1MB；BGRA 是 ×4 ≈ 8.3MB。NV12 是 BGRA 的 3/8。
2. **Q：為什麼測試圖要用純色色塊，不能只用一般桌面截圖？**
   A：灰階 U=V=128，係數錯看不出；純色才能暴露係數與 range 錯誤；而且有已知理論值可比對。
3. **Q：如果 encoder 端用 709 studio，播放器卻用 601 解，會看到什麼？那如果整個系統都錯成 601，你看得出來嗎？**
   A：飽和色色相偏移、灰階正常。若兩端都一致錯，自己系統看起來沒事——這就是「自己解沒事、別人的播放器顏色不對」的典型 bug，所以還要在 bitstream 的 VUI 寫對 `matrix_coefficients` / `video_full_range_flag`（透過 MediaType 的 `MF_MT_YUV_MATRIX`、`MF_MT_VIDEO_NOMINAL_RANGE`）。
4. **Q：為什麼 Lab 3 的 CPU 轉換不能用在產品裡？**
   A：要 readback（stall + 頻寬）、CPU 逐像素運算慢；產品路徑在 GPU 上一次 Blt 做完。

### 評分（Lab 3，100 分）

| 項目 | 分數 |
|---|---|
| NV12 佈局解析正確 | 25 |
| 公式與 GPU 端一致、色塊驗證通過 | 25 |
| 「設錯會怎樣」文字說明（理解程度） | 25 |
| 測試方法（色塊、印 YUV 值） | 15 |
| 程式品質 | 10 |

---

## Lab 4｜H264 硬體編碼

### 真正在考什麼

1. **MFT 列舉與硬體/軟體判斷**（並有 fallback 而不是 crash）。
2. **MFT 的資料流協定**：MediaType 協商 → 串流開始通知 → 餵資料 / 取資料 → drain → 結束。async 的話還有事件迴圈。
3. **時間戳**的正確計算。
4. **恆定幀率的思考**：DDA 只在畫面變化時給 frame，但 encoder 要 30fps。

### Mentor 背景知識

#### 硬體 vs 軟體 encoder

| | 硬體 encoder MFT（廠商提供） | `CLSID_CMSH264EncoderMFT`（微軟軟體） |
|---|---|---|
| 怎麼拿 | `MFTEnumEx(..., MFT_ENUM_FLAG_HARDWARE \| SORTANDFILTER, ...)` | `CoCreateInstance` 或 `MFTEnumEx(MFT_ENUM_FLAG_SYNCMFT)` |
| 同步/非同步 | **Async**（幾乎都是） | Sync |
| D3D11 input | 支援（設 D3D manager） | 會 readback 到 CPU 做 |
| 名稱 | `MFT_FRIENDLY_NAME_Attribute`，例如 "NVIDIA H.264 Encoder MFT" | "H264 Encoder MFT" |

學員必須印出 `MFT_FRIENDLY_NAME_Attribute`。如果印出的是微軟的軟體 encoder 卻宣稱「硬體」，就是沒搞懂（E1）。

#### Async MFT 的事件迴圈（重點流程）

```
SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE)
ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, mgr)
SetOutputType → SetInputType
ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING)
ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM)
loop:
    GetEvent(0, &ev)                       // 阻塞等待
    METransformNeedInput  → 有下一張就 ProcessInput；沒了就 NOTIFY_END_OF_STREAM + COMMAND_DRAIN
    METransformHaveOutput → ProcessOutput → 寫檔
    METransformDrainComplete → 結束
ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING)
ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0)   // 解除綁定再 Release
```

關鍵規則：
- **只有收到 `METransformNeedInput` 才能 `ProcessInput`**，否則回 `MF_E_NOTACCEPTING`。
- **只有收到 `METransformHaveOutput` 才能 `ProcessOutput`**。
- 一個 NeedInput 事件只能送一張。
- 學員可以用簡單的同步 `GetEvent(0, ...)`（阻塞），不一定要實作 `IMFAsyncCallback` + `BeginGetEvent`。兩者都可接受。

#### Sync MFT（軟體 fallback）的迴圈

```
for each frame:
    hr = ProcessInput(...)
    if hr == MF_E_NOTACCEPTING: 先把 output 拉乾淨再重送
    while (ProcessOutput(...) == S_OK) 寫檔
    // ProcessOutput 回 MF_E_TRANSFORM_NEED_MORE_INPUT 就跳出，送下一張
NOTIFY_END_OF_STREAM → COMMAND_DRAIN
while (ProcessOutput(...) == S_OK) 寫檔     // 直到 NEED_MORE_INPUT
```

輸出 sample 由誰配置：看 `GetOutputStreamInfo` 的 `dwFlags`，有 `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES`（或 `CAN_PROVIDE_SAMPLES`）時 `pSample` 設 nullptr 讓 MFT 給；否則自己用 `cbSize` 配置。硬體 encoder 通常自己配置。學員直接照訓練文件 §5.6 寫死自己配置，在部分硬體上會出錯。

#### 時間戳

- 100ns 單位。30fps → duration = 10,000,000 / 30 = **333,333**（有餘數）。
- 正確：`time = MFllMulDiv(i, 10'000'000, 30, 0)` 或 `i * 10'000'000LL / 30`（先乘後除，不累積誤差）。
- 次佳：`time += 333333` 累加，150 張誤差只有 50 個單位，可接受，但要問學員有沒有意識到。
- 錯誤：用 `int`（32-bit）存 → 約 214 秒後溢位（5 秒不會出事，但 Lab 6 跑 10 分鐘會）。
- 錯誤：沒設 → `MF_E_NO_SAMPLE_TIMESTAMP`。

#### 恆定幀率 vs DDA

這是本題**最有鑑別度**的點，原文件沒明講：
- 如果桌面靜止，DDA 一直 `WAIT_TIMEOUT`，學員「每拿到一張就編一張」→ 5 秒可能只有 20 張。
- 好的作法：用固定 33ms 的節拍跑迴圈，`AcquireNextFrame` timeout 設小，**timeout 時重送上一張畫面**（所以需要 Lab 1 那張自己擁有的 stable texture），時間戳照節拍遞增。
- 也可接受：用實際擷取時間當時間戳（可變幀率），但 raw .h264 沒容器存不了時間，播放會「快轉」。

#### Raw H264 檔驗證（勘誤 M7）

- 編出來的 Annex B 檔本身沒時間戳。ffprobe 顯示的 duration 依 VUI timing 或預設 25fps 估算。150 張 @25fps → 顯示 6 秒，**不代表學員錯**。
- 正確驗證：
  ```bash
  ffprobe -v error -count_frames -select_streams v:0 \
    -show_entries stream=codec_name,profile,width,height,nb_read_frames,has_b_frames \
    -of default=nw=1 output.h264
  ```
  檢查 `codec_name=h264`、寬高、`nb_read_frames` ≈ 150、`has_b_frames`（Lab 6 低延遲時應為 0）。
- 想看正確時長：`ffplay -framerate 30 output.h264` 或 `ffmpeg -r 30 -i output.h264 -c copy out.mp4`。
- 每個 frame 的類型：`ffprobe -show_frames -select_streams v -show_entries frame=pict_type -of csv output.h264`，可看 I/P/B 分布與 GOP。

#### Bitrate 挑戰的期望值

- 檔案大小 ≈ bitrate × 秒數 / 8 → 2Mbps × 5s ≈ **1.25MB**、8Mbps × 5s ≈ **5MB**。
- 若錄的是靜止桌面，encoder 用不到那麼多 bit，兩者可能都很小且差距不大（硬體 CBR 通常不會塞 filler）。請學員錄**有動態的內容**（播影片、拖動視窗）再比較。
- 兩檔大小一樣 → bitrate 根本沒設進去（常見：在 `SetOutputType` 之後才設 CODECAPI，或只設了 `MF_MT_AVG_BITRATE` 但 rate control mode 不對）。

#### 其他會踩的點

- `MF_MT_FRAME_SIZE` 輸入輸出必須一致，寬高偶數。
- 先 `SetOutputType` 再 `SetInputType`；部分 CODECAPI 屬性（例如 rate control mode）要在設 MediaType **之前**設定才生效。
- `MFCreateDXGISurfaceBuffer` 後設 current length（M10）。
- 送進 encoder 的 NV12 texture 要用 pool（M9）。5 秒的 Lab 不一定會看出問題，但會在播放時看到偶發的畫面倒退或重複。
- 第一個輸出 sample 通常帶 SPS/PPS + IDR。可以檢查輸出檔前幾個 bytes 是 `00 00 00 01 67`（SPS，NAL type 7）。

### 參考解答骨架

```cpp
// 列舉：硬體優先，fallback 軟體
ComPtr<IMFTransform> encoder;
bool isHardware = false;
{
    MFT_REGISTER_TYPE_INFO in{ MFMediaType_Video, MFVideoFormat_NV12 };
    MFT_REGISTER_TYPE_INFO out{ MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** acts = nullptr; UINT32 n = 0;
    CHECK_HR(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in, &out, &acts, &n));
    if (n > 0) {
        WCHAR* name = nullptr; UINT32 len = 0;
        acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len);
        wprintf(L"Using HW encoder: %s\n", name ? name : L"(unknown)");
        CoTaskMemFree(name);
        CHECK_HR(acts[0]->ActivateObject(IID_PPV_ARGS(&encoder)));
        isHardware = true;
    }
    for (UINT32 i = 0; i < n; ++i) acts[i]->Release();
    CoTaskMemFree(acts);
}
if (!encoder) {
    wprintf(L"No HW encoder, falling back to Microsoft software H264 encoder\n");
    CHECK_HR(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&encoder)));
}

// async unlock
ComPtr<IMFAttributes> attrs;
CHECK_HR(encoder->GetAttributes(&attrs));
UINT32 isAsync = MFGetAttributeUINT32(attrs.Get(), MF_TRANSFORM_ASYNC, FALSE);
if (isAsync) CHECK_HR(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE));

// D3D manager（硬體才有意義；device 需 VIDEO_SUPPORT + multithread protected）
if (isHardware)
    CHECK_HR(encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)devMgr.Get()));

// CODECAPI（在 SetOutputType 之前）
ComPtr<ICodecAPI> api;
if (SUCCEEDED(encoder.As(&api))) {
    VARIANT v; VariantInit(&v);
    v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_CBR;
    api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
    v.vt = VT_UI4; v.ulVal = bitrate;
    api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
}

// MediaType（同訓練文件 §5.4，output 先）
// ...

// 時間戳
const LONGLONG dur = 10'000'000LL / 30;
for (UINT i = 0; i < 150; ++i) {
    sample->SetSampleTime(MFllMulDiv(i, 10'000'000, 30, 0));
    sample->SetSampleDuration(dur);
    // ...
}
```

Async 事件迴圈與 drain 的骨架見上面「Async MFT 的事件迴圈」。

### Mentor 驗收步驟

1. 看 console 印出的 encoder 名稱，確認「硬體/軟體」判斷正確。
2. `ffprobe -count_frames ...`（見上）：codec、尺寸、frame 數。
3. `ffplay -framerate 30 output.h264`：內容正確、無花屏、無跳格倒退。
4. 在沒有硬體 encoder 的環境（或 VM、或請學員臨時把 HARDWARE flag 改掉）跑：要能 fallback 或清楚報錯。
5. Bitrate 挑戰：看兩個檔案大小與上面期望值比較。
6. 靜止桌面錄 5 秒：frame 數是否仍接近 150（有沒有處理恆定幀率）。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | `MFTEnumEx` 結果 `count == 0` 有處理 |
| 🔴 | activate 陣列全部 Release + `CoTaskMemFree` |
| 🔴 | 正確判斷 sync/async，async 有 unlock（`SetUINT32`）且依事件驅動 |
| 🔴 | 有 drain，且 drain 後把剩下的 output 全部拿完 |
| 🔴 | 時間戳 100ns 單位、64-bit、不累積大誤差 |
| 🔴 | 輸入是 DXGI surface buffer（硬體時），不是把 NV12 Map 到 CPU 再 `MFCreateMemoryBuffer` |
| 🟡 | 輸出 sample 依 `MFT_OUTPUT_STREAM_PROVIDES_SAMPLES` 決定由誰配置 |
| 🟡 | 輸出時釋放 `pEvents`、MFT 配置的 `pSample` |
| 🟡 | 處理 `MF_E_TRANSFORM_STREAM_CHANGE`（encoder 很少發生，但有寫代表懂） |
| 🟡 | NV12 input texture 有 pool / 不會被覆寫 |
| 🟡 | 恆定幀率（timeout 時重送上一張） |
| 🟢 | 結束時送 `NOTIFY_END_STREAMING`、解除 D3D manager |
| 🟢 | `--bitrate` 參數生效並有數據佐證 |

### 症狀診斷表

| 現象 | 原因 |
|---|---|
| `ActivateObject` 前 crash | `count == 0` 沒處理 |
| 任何呼叫都回 `0xC00D6D77` (`MF_E_TRANSFORM_ASYNC_LOCKED`) | 沒 unlock，或用了 `SetUnknown`（勘誤 E2） |
| `ProcessInput` 回 `MF_E_NOTACCEPTING` | async 沒等 NeedInput 就送；或 sync 沒先把 output 拉完 |
| `SetInputType` 回 `MF_E_INVALIDMEDIATYPE` | 先設了 input 才設 output；或寬高/frame rate 跟 output 不一致；或寬高奇數 |
| 檔案很小只有幾 KB / 0 bytes | 沒 drain、ProcessOutput 結果沒寫檔、或 buffer 沒取 current length |
| 少了最後幾張 | 沒 drain |
| 只有 20~40 張 | DDA 靜止畫面沒補幀 |
| 播放畫面偶爾倒退/重複 | NV12 texture 沒 pool，被下一張覆蓋；或 B-frame 而時間戳亂設 |
| ffprobe 時長 6 秒 | 正常（raw H264 預設 25fps），不是 bug |
| 2Mbps 與 8Mbps 檔案一樣大 | CODECAPI 沒生效（設定時機、或錄的是靜止畫面） |
| 顏色偏 | Lab 3 的 color space 問題延續，或 encoder VUI 沒標 709 而播放器用 601 |
| 結束時卡住 | async 迴圈在 drain 後還在等 NeedInput；或 `GetEvent` 阻塞而沒有事件 |

### 口試追問

1. **Q：你怎麼知道用的是硬體 encoder？`CLSID_CMSH264EncoderMFT` 是硬體還是軟體？**
   A：看 enum 時的 HARDWARE flag 結果 / `MFT_ENUM_HARDWARE_URL_Attribute` / friendly name。`CLSID_CMSH264EncoderMFT` 是軟體。
2. **Q：`MF_E_TRANSFORM_NEED_MORE_INPUT` 代表什麼？為什麼 encoder 不能每送一張就吐一張？**
   A：內部還在 buffer（lookahead、B-frame 需要後面的 frame 當參考、rate control）。不是錯誤。
3. **Q：Drain 是什麼？不 drain 會怎樣？**
   A：告訴 MFT「沒有新輸入了，把 buffer 裡的全部吐出來」。不 drain 會少最後幾張。
4. **Q：桌面完全靜止 5 秒，你的檔案有幾張 frame？為什麼？**
   A：考恆定幀率思考。好答案：「150 張，timeout 時我重送上一張」；可接受：「只有幾張，因為 DDA 沒變化不給 frame，若要固定幀率要補幀」。
5. **Q：同一張 NV12 texture 每幀重複使用送進 async encoder，會有什麼風險？**
   A：encoder 非同步讀取，可能下一次 Blt 已經覆蓋了它還沒讀完的資料；要用 pool。
6. **Q：為什麼 ffprobe 說是 6 秒？**
   A：raw H264 沒容器、沒時間戳，ffmpeg 預設 25fps 估算。
7. **Q：GOP 設很長跟很短，對串流各有什麼影響？**
   A：長 → 壓縮率高但丟包後要很久才恢復、新加入者要等很久；短 → 恢復快但 I-frame 大、bitrate 尖峰多。即時串流常用「長 GOP + 需要時強制 IDR」。

### 評分（Lab 4，100 分）

| 項目 | 分數 |
|---|---|
| 檔案可播放、frame 數與尺寸正確 | 25 |
| 列舉 + 硬體判斷 + fallback | 15 |
| MFT 資料流（sync/async、NEED_MORE_INPUT、drain）正確 | 25 |
| 時間戳正確 | 10 |
| GPU 路徑（DXGI buffer、D3D manager、texture pool） | 15 |
| Bitrate 挑戰 / 恆定幀率（加分項） | 10 |

---

## Lab 5｜H264 硬體解碼

### 真正在考什麼

1. **Annex B bitstream 結構**：start code、NAL type，以及「NAL ≠ frame」。
2. **Decoder 的協商流程**：特別是 `MF_E_TRANSFORM_STREAM_CHANGE`。
3. **Decoder 輸出 texture 的使用**：texture array + subresource index、對齊後的高度、釋放 sample。

### Mentor 背景知識

#### NAL 結構速查

每個 NAL 以 `00 00 01` 或 `00 00 00 01` 開頭，接著一個 header byte：`nal_unit_type = byte & 0x1F`。

| type | 名稱 | 是否是畫面 |
|---|---|---|
| 1 | Non-IDR slice（P/B） | 是（一張畫面可能有多個 slice） |
| 5 | IDR slice | 是 |
| 6 | SEI | 否 |
| 7 | SPS | 否 |
| 8 | PPS | 否 |
| 9 | AUD（Access Unit Delimiter） | 否，分隔用 |

- **NAL 數量 ≠ 畫面數**：SPS/PPS/SEI/AUD 不是畫面；一張畫面可以有多個 slice NAL（硬體 encoder 有時會用多 slice）。
- **Access Unit（AU）= 一張畫面的所有 NAL**。理想上一個 MF sample 送一個 AU。
- 判斷新 AU 開始：遇到 AUD/SPS/PPS/SEI，或遇到 slice（type 1/5）且 slice header 的 `first_mb_in_slice == 0`（實作上：header 後第一個 byte 的最高位元為 1）。
- 學員**每個 NAL 送一個 sample** 也可以——MS decoder 能吃——不扣分，但要問他知不知道 AU 的概念（Lab 6 的 length-prefix 要以 frame 為單位）。

#### `MF_E_TRANSFORM_STREAM_CHANGE`

MS decoder 一開始不知道真正的解析度/格式，看到 SPS 之後：
1. `ProcessOutput` 回 `MF_E_TRANSFORM_STREAM_CHANGE`（`0xC00D6D61`）。
2. 你要：`GetOutputAvailableType(0, i, &t)` 迴圈找到 subtype == NV12 的那個 → `SetOutputType(0, t, 0)`。
3. 讀新 type 的 `MF_MT_FRAME_SIZE`（可能是 1920×1088）與 `MF_MT_MINIMUM_DISPLAY_APERTURE`（實際 1920×1080）。
4. 再呼叫 `ProcessOutput`。

不處理這個 → 學員永遠拿不到畫面，然後以為 decoder 壞了。這是 Lab 5 最常見的卡關點，**學員卡在這裡時可以直接提示**。

#### 輸出 texture

- 設了 D3D manager 後，decoder 自己配置 output sample（`MFT_OUTPUT_STREAM_PROVIDES_SAMPLES`），`pSample` 傳 nullptr。
- 拿到的 texture 是 **texture array**（`ArraySize` > 1，`BindFlags` 含 `D3D11_BIND_DECODER`），用 `IMFDXGIBuffer::GetSubresourceIndex` 拿是哪一張。
- 不能直接 Map。存 BMP 要：`CopySubresourceRegion(staging, 0, 0,0,0, decodedTex, subIndex, &box)` 到一張單張 NV12 staging texture，box 只取 display aperture 範圍。
- 存完 BMP、**立刻 Release sample**，否則 decoder 的 surface pool（通常 10~20 張）用完就會卡死不再輸出。

#### Drain 與 frame 數

- 檔案讀完後要送 `MFT_MESSAGE_COMMAND_DRAIN`，然後一直 `ProcessOutput` 到 `NEED_MORE_INPUT`。不 drain 會少幾張（因為 decoder 為了 reorder 會 hold 住幾張）。
- 正確實作的輸出張數應**等於** Lab 4 的編碼張數（`ffprobe -count_frames` 的數字）。這是很好的客觀驗收點（勘誤 M12）。

#### Decoder 輸入時間戳

Raw .h264 沒時間戳，學員要自己給遞增的假時間戳（例如 i × 333333）。沒設時 MS decoder 通常還是會解，但輸出時間戳無意義；有些情況會影響輸出順序判斷。

### 參考解答骨架

```cpp
// 建立 decoder（不靠 HARDWARE enum，見勘誤 E7）
ComPtr<IMFTransform> dec;
CHECK_HR(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dec)));

ComPtr<IMFAttributes> da; CHECK_HR(dec->GetAttributes(&da));
if (MFGetAttributeUINT32(da.Get(), MF_SA_D3D11_AWARE, FALSE)) {
    CHECK_HR(dec->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)devMgr.Get()));
    printf("D3D11 (DXVA) decoding enabled\n");
} else {
    printf("Decoder not D3D11 aware -> software decode\n");
}
// 可選：低延遲
da->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);

// input type: H264；output type: 先設一個 NV12，等 STREAM_CHANGE 再重設
// ...
dec->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
dec->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

auto DrainOutputs = [&]() -> HRESULT {
    for (;;) {
        MFT_OUTPUT_DATA_BUFFER ob{}; DWORD st = 0;
        HRESULT hr = dec->ProcessOutput(0, 1, &ob, &st);
        if (ob.pEvents) ob.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            ComPtr<IMFMediaType> t;
            for (DWORD i = 0; SUCCEEDED(dec->GetOutputAvailableType(0, i, &t)); ++i, t.Reset()) {
                GUID sub; t->GetGUID(MF_MT_SUBTYPE, &sub);
                if (sub == MFVideoFormat_NV12) { CHECK_HR(dec->SetOutputType(0, t.Get(), 0)); break; }
            }
            ReadFrameSizeAndAperture();     // 例如 1920x1088, aperture 1920x1080
            continue;
        }
        if (FAILED(hr)) return hr;

        ComPtr<IMFSample> s; s.Attach(ob.pSample);   // 確保 Release
        ComPtr<IMFMediaBuffer> b;   CHECK_HR(s->GetBufferByIndex(0, &b));
        ComPtr<IMFDXGIBuffer> db;   CHECK_HR(b.As(&db));
        ComPtr<ID3D11Texture2D> tex; CHECK_HR(db->GetResource(IID_PPV_ARGS(&tex)));
        UINT sub = 0;               CHECK_HR(db->GetSubresourceIndex(&sub));

        D3D11_BOX box{ 0, 0, 0, apertureW, apertureH, 1 };
        context->CopySubresourceRegion(stagingNV12.Get(), 0, 0, 0, 0, tex.Get(), sub, &box);
        SaveNV12AsBmp(context.Get(), stagingNV12.Get(), apertureW, apertureH, NextFileName());
    }
};

for (auto& au : SplitAnnexB(fileBytes)) {         // 自寫 start code 切割
    ComPtr<IMFSample> in = MakeMemorySample(au, ts); ts += 333333;
    HRESULT hr = dec->ProcessInput(0, in.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) { CHECK_HR(DrainOutputs()); hr = dec->ProcessInput(0, in.Get(), 0); }
    CHECK_HR(hr);
    CHECK_HR(DrainOutputs());
}
dec->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
dec->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
CHECK_HR(DrainOutputs());
```

> 注意：staging NV12 texture 的高度若取 1080，而來源是 1088 的 array，`CopySubresourceRegion` 要用 box 限制範圍；或 staging 建成 1088 再在 `SaveNV12AsBmp` 只取前 1080 行（但 UV plane 偏移要用 1088 計算！這是另一個容易錯的地方）。

### Mentor 驗收步驟

1. 跑 `h264_decode.exe output.h264`，看產生張數 = Lab 4 `ffprobe -count_frames` 的數字。
2. 開 `frame_0001.bmp` 與 Lab 3 存的畫面比對。
3. 檢查**最底部 8 行**有無綠/灰條（1080p 時）。
4. 檢查 console 有沒有印出是否走 D3D11/DXVA。
5. 用工作管理員看解碼 150 張過程記憶體是否穩定（sample 有沒有 Release）。
6. 拿一個別人（ffmpeg）編的 h264 檔餵給它，確認不是只能吃自己 Lab 4 的檔：
   ```bash
   ffmpeg -f lavfi -i testsrc2=size=1280x720:rate=30 -t 5 -c:v libx264 -bsf:v h264_mp4toannexb test.h264
   ```
   （testsrc2 也很適合檢查色彩與畫面正確性）
7. 加分：視窗即時播放，看是否流暢。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | 處理 `MF_E_TRANSFORM_STREAM_CHANGE` |
| 🔴 | 使用 `GetSubresourceIndex`，不假設 index 0 |
| 🔴 | output sample 有 Release（`pSample`、`pEvents`） |
| 🔴 | 結尾 drain，張數與編碼張數一致 |
| 🔴 | start code 3-byte 與 4-byte 都處理 |
| 🟡 | 依 display aperture 裁掉對齊的多餘行 |
| 🟡 | `MF_SA_D3D11_AWARE` 檢查後才設 D3D manager，並印出是否硬體加速 |
| 🟡 | 輸入 sample 有遞增時間戳 |
| 🟢 | 以 AU（整張畫面）為單位送 sample |
| 🟢 | 可解第三方產生的 h264 |
| 🟢 | 視窗播放用 VideoProcessorBlt（NV12→BGRA）到 swap chain，全程 GPU |

### 症狀診斷表

| 現象 | 原因 |
|---|---|
| 一直 `NEED_MORE_INPUT`，永遠沒畫面 | 沒處理 `STREAM_CHANGE`（通常學員看到這個 HRESULT 就當錯誤跳出了，或以為只是另一種 NEED_MORE_INPUT） |
| 解了 10~20 張後卡住不再輸出 | output sample 沒 Release，surface pool 耗盡 |
| 畫面底部 8 行綠色 | 1088 對齊沒裁 |
| 底部 1/3 顏色怪 / 整張顏色錯位 | staging 高度 1080 但 UV 偏移應該算 1088（或反過來） |
| 第一張是灰色/花屏 | 第一個送進去的不是從 SPS/PPS/IDR 開始；或 NAL 切割吃掉了 header byte |
| 張數比預期少 3~5 張 | 沒 drain |
| 張數遠多於預期（例如 450） | 把每個 NAL 都當一張畫面計數（自己編號的邏輯錯，不是 decoder 輸出） |
| 每張畫面相同 | 一直讀同一個 subresource index |
| `0xC00D36B4` (`MF_E_INVALIDMEDIATYPE`) on SetInputType | input type 少了 major type / subtype，或順序錯 |

### 口試追問

1. **Q：`output.h264` 有多少個 NAL？為什麼比 frame 數多？**
   A：多了 SPS/PPS/SEI（以及可能的 AUD、多 slice）。
2. **Q：`MF_E_TRANSFORM_STREAM_CHANGE` 是什麼時候、為什麼發生？**
   A：decoder 解析到 SPS 才知道真正的解析度/格式，要求你重新協商 output type。中途解析度改變（Lab 6 切換解析度）也會再發生。
3. **Q：為什麼 decoder 給你的是 texture array？**
   A：DXVA decoder 有 reference frame pool（DPB），在同一個 array 裡循環使用；每次輸出只是告訴你是第幾張。
4. **Q：如果你拿到 sample 不 Release 會怎樣？**
   A：那張 surface 被你佔住，decoder 沒有空的 surface 可寫，最終卡死。
5. **Q：1080p 為什麼會變 1088？**
   A：H264 以 16×16 macroblock 編碼，1080 不是 16 的倍數，編碼高度補到 1088，用 SPS 的 cropping 資訊標示有效區。
6. **Q：H264 bitstream 裡有 AVCC 和 Annex B，你的 Lab 6 的 length-prefix 跟 AVCC 有什麼相同與不同？**
   A：同樣是用長度而不是 start code 來切；AVCC 是**每個 NAL** 一個長度前綴，Lab 6 是**每張畫面（AU）**一個前綴、裡面仍是 Annex B。

### 評分（Lab 5，100 分）

| 項目 | 分數 |
|---|---|
| 解碼結果正確（畫面、張數相等） | 25 |
| NAL 切割正確 | 15 |
| STREAM_CHANGE、drain 等 MFT 流程 | 25 |
| 輸出 texture 正確使用（subresource、aperture、Release） | 25 |
| 加分：視窗播放 / 第三方檔案 | 10 |

---

## Lab 6｜端到端即時串流

### 真正在考什麼

1. **「全程 GPU」的系統性落實**（題目明說的核心考點）。
2. **TCP framing**：byte stream 沒有訊息邊界、`send`/`recv` 可能只做一部分。
3. **多執行緒 pipeline 與背壓（backpressure）**：網路慢時誰等誰、queue 會不會無限長大。
4. **延遲量測的方法論**：量的是什麼、時鐘是否可比。
5. **動態重設定**：解析度切換時 encoder/decoder/render 該怎麼處理。
6. **穩定性**：10 分鐘不洩漏、斷線乾淨結束。

### Mentor 背景知識

#### 架構參考

```
Sender                                              Receiver
┌────────────── capture/encode thread ──────────┐   ┌──── recv thread ────┐   ┌──── decode/render thread ───────┐
│ Acquire → Copy(pool) → Blt(NV12, pool)        │   │ recvAll(4B len)     │   │ pop → ProcessInput              │
│ → DXGI sample → Encoder → bitstream(CPU) ─────┼─► │ recvAll(len bytes)  │─► │ → ProcessOutput (GPU texture)   │
└───────────────────────────────────────────────┘   │ push queue          │   │ → Blt(NV12→BGRA backbuffer)     │
                  │ queue (bounded)                 └─────────────────────┘   │ → Present                       │
            ┌──── send thread ──────┐                                         └─────────────────────────────────┘
            │ pop → sendAll(hdr+data)│ ── TCP (TCP_NODELAY) ──►
            └────────────────────────┘
```

唯一合法的 CPU 資料：encoder 輸出的 bitstream、網路 buffer、decoder 的輸入 bitstream。

#### TCP framing 的三個經典 bug

1. **`recv` 不保證收滿**：`recv(sock, buf, 4, 0)` 可能只回 2。一定要有 `recvAll` 迴圈。這是本題最常見的 bug，**在 127.0.0.1 上幾乎不會發生**，跨機器 / 大 frame 才會出現，所以學員常常「本機測沒事」。
2. **`send` 不保證送完**：阻塞模式下通常會送完，但規範上仍要迴圈處理回傳值。
3. **Endianness**：長度欄位要約定 byte order（`htonl`/`ntohl`），兩端都是 x86 時不會出錯，但沒處理代表沒想到。

加分：header 裡除了長度，還放 frame 序號、capture timestamp、是否 keyframe、寬高。這讓延遲量測與解析度切換都更好做。

**安全性**：receiver 讀到長度後應檢查上限（例如 > 10MB 視為錯誤），否則壞資料 / 惡意輸入會導致巨大配置。

```cpp
bool RecvAll(SOCKET s, void* buf, int len) {
    char* p = (char*)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0) return false;   // 0 = 對端正常關閉, <0 = 錯誤
        p += n; len -= n;
    }
    return true;
}
```

#### Nagle（勘誤 M11）

```cpp
BOOL on = TRUE;
setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
```

沒設的症狀：本機延遲還好，但 P-frame 很小時延遲忽高忽低，跨機器明顯多 40~200ms。

#### Backpressure / 丟幀策略

網路比 encoder 慢時，send queue 會一直長 → 延遲無限增加、記憶體一直漲（10 分鐘測試會抓到）。

- 不能隨便丟 encoded frame：丟掉一個 P-frame，後面所有 P-frame 都會參考錯誤直到下一個 IDR → 花屏。
- 合理策略（擇一，學員能說明即可）：
  - **queue 滿了就丟「還沒編碼的原始畫面」**（在 encoder 之前丟，不影響參考鏈）。
  - queue 滿了就清空 queue，並對 encoder 強制下一張 IDR（`CODECAPI_AVEncVideoForceKeyFrame`）。
  - 依 queue 長度動態降 bitrate / 解析度。
- 學員**完全沒有 bounded queue** → 🟡 扣分並追問；如果 queue 是無上限且 10 分鐘測試記憶體上升 → 🔴。

#### 延遲量測方法論

| 量測項 | 正確的量法 | 常見錯誤 |
|---|---|---|
| ① 擷取 → 編碼完成 | 起點：`AcquireNextFrame` 回傳時的 QPC（或 `frameInfo.LastPresentTime`，它也是 QPC 單位）；終點：該 frame 的 output sample 從 `ProcessOutput` 出來時。需要把起點 QPC 跟 frame 對應起來（用 sample time 或序號當 key）。 | 用「這次 ProcessInput 到這次 ProcessOutput 的時間」——但 encoder 有延遲，這次出來的可能是前一張。 |
| ② 編碼完成 → 解碼完成 | 跨 process：把 sender 的 QPC 值放進 frame header，receiver 解碼完成時取 QPC 相減。**同一台機器 QPC 全系統一致，可直接相減。** | 跨機器時直接相減（兩台時鐘不同步）。 |
| ③ 端到端 | 起點同①；終點：`Present` 回傳後（嚴格說還要加上到 vsync 掃出的時間）。 | 終點用 decode 完成時間。 |

- **跨機器**：兩台的 QPC/系統時鐘不可直接比較。可接受的作法：(a) 只在本機量 ①②③ 並說明；(b) 用 round-trip（receiver 回傳 ack，sender 算 RTT/2 估計）；(c) 最簡單也最直觀的方法——在 sender 畫面上顯示一個毫秒計時器，用手機同時拍 sender 螢幕與 receiver 視窗，兩個數字相減。學員有想到時鐘問題就給分。
- Vsync：如果 receiver 的 `Present(1, 0)`，最多會多等一個 refresh（60Hz = 16.7ms）。用 `Present(0, 0)` 或 flip model + `DXGI_PRESENT_ALLOW_TEARING` 可降低，但可能撕裂。這是報告裡「怎麼降延遲」的好話題。

#### 經驗值參考（本機 127.0.0.1、1080p、硬體編解碼、低延遲設定）

> 以下是幫你判斷學員數字「合不合理」的量級，不是標準答案，依 GPU 與驅動差異很大。

| 量測 | 合理範圍 | 太高時優先懷疑 |
|---|---|---|
| ① 擷取 → 編碼完成 | 約 3~15 ms | 沒開 low latency / 有 B-frame / encoder 輸入前 Map 到 CPU / 軟體 encoder（軟體 1080p 可能 20~50ms+） |
| ② 編碼完成 → 解碼完成（本機） | 約 2~15 ms | Nagle、decoder 沒設 low latency 而 hold 住多張、queue 堆積 |
| ③ 端到端 | 約 15~50 ms | vsync 等待、render thread 跟 decode 同步方式、queue 堆積 |

數字**隨時間持續上升** → queue 無上限在堆積（backpressure 問題），這是很重要的觀察。

#### 解析度動態切換

正確流程（學員能做到其中一種就可以）：

1. **Sender**：
   - VP output texture pool 換尺寸（或預先為三種尺寸都建好）。
   - Encoder：最簡單可靠的作法是**重建 encoder**（drain 舊的 → 釋放 → 建新的）；進階作法是 `MFT_MESSAGE_COMMAND_FLUSH` 後重設 media type（不是所有硬體 encoder 都支援）。
   - 新尺寸的第一張必然是 IDR，帶新的 SPS/PPS。
2. **Receiver**：
   - Decoder 看到新 SPS 會回 `MF_E_TRANSFORM_STREAM_CHANGE` → 重新協商 output type（Lab 5 已經寫過）。
   - Render 端要依新尺寸調整（VP 的 input 尺寸變了、可能要重建 input view；視窗大小不變，VP 自動縮放到視窗）。
3. 常見 bug：sender 換了尺寸但 encoder 沒換 → `ProcessInput` 失敗或畫面錯亂；receiver 沒處理 STREAM_CHANGE → 切換後黑屏。

#### 斷線處理

- Receiver 關掉 → sender 的 `send` 回 `SOCKET_ERROR`（`WSAECONNRESET` / `WSAECONNABORTED`）→ send thread 設 stop flag → 其他 thread 檢查 flag 結束 → 依序釋放（先停 capture、drain/丟棄 encoder、`closesocket`、`MFShutdown`）。
- Sender 關掉 → receiver 的 `recv` 回 0 → 同理結束。
- 阻塞在 `recv` 的 thread 怎麼喚醒：從另一個 thread `closesocket`（或 `shutdown`）會讓阻塞的 `recv` 返回錯誤。
- 🔴 紅旗：斷線後 sender 卡住（thread 在等一個永遠不會來的 queue item）、或狂印錯誤不停止、或 crash。

### Mentor 驗收步驟

1. **本機互連**：先開 receiver 再開 sender，畫面出現且順暢；拖動視窗、播影片看即時性。
2. **看延遲數字**：是否三個都有、是否合理、**是否隨時間上升**。
3. **按 `+`/`-` 切解析度**連續切 10 次，不黑屏、不 crash、不需重啟。
4. **Ctrl+C / 關掉 receiver**：sender 5 秒內乾淨結束；反之亦然。
5. **10 分鐘測試**：開工作管理員「詳細資料」頁，觀察兩個 process 的「記憶體（專用工作集）」與 **「GPU 專用記憶體」**（工作管理員可加這一欄）。前 1 分鐘會上升到穩定，之後應持平。持續上升 → 洩漏（常見來源：pSample、每幀 CreateTexture2D、無上限 queue）。
6. **確認 GPU 路徑**：
   - 跑 §3.2 的 grep，sender/receiver 的每幀路徑中不應有 `Map` / `STAGING` / `MFCreateMemoryBuffer`（encoder 輸出除外）。
   - Console 應印出 encoder 是否硬體、decoder 是否 D3D11 aware。**軟體 encoder 即使程式碼全是 DXGI buffer，內部仍會 readback**，報告中要說明。
   - 進階：用 PIX / GPUView 看有沒有 copy queue 上的 readback，或工作管理員 GPU 頁面看 "Copy" 引擎使用率是否異常高。
7. **跨機器**（加分）：在同一個區網兩台跑一次，看延遲變化（Nagle、`recvAll` 的 bug 會在這裡現形）。

### Code Review Checklist

| 等級 | 項目 |
|---|---|
| 🔴 | 每幀路徑上沒有非必要 GPU→CPU（含：encoder 有設 D3D manager、input 是 DXGI buffer、render 用 VP Blt 而非 Map 後畫 GDI） |
| 🔴 | `recvAll` / `sendAll` 迴圈 |
| 🔴 | length prefix 正確（固定長度、有約定 byte order、receiver 檢查上限） |
| 🔴 | 網路 I/O 在獨立 thread |
| 🔴 | 斷線時乾淨結束，不卡死不 crash |
| 🔴 | 10 分鐘記憶體不持續上升 |
| 🔴 | 三個延遲數字都有，且學員能說明量的起點終點 |
| 🟡 | `TCP_NODELAY` |
| 🟡 | bounded queue + 合理的丟幀策略（不在 encoder 之後隨便丟 P-frame） |
| 🟡 | Encoder low latency、B-frame = 0；decoder low latency |
| 🟡 | Texture pool（capture copy、NV12）不是每幀 CreateTexture2D |
| 🟡 | 解析度切換正確處理 encoder 重建 / decoder STREAM_CHANGE |
| 🟡 | 跨 thread 使用 device 有 multithread protection（M1），或 immediate context 只在一個 thread 使用 |
| 🟢 | Header 帶 timestamp / 序號 / keyframe flag |
| 🟢 | Receiver 能在連線中途加入（sender 收到新連線時強制 IDR） |
| 🟢 | 跨機器實測並討論時鐘同步 |

### 症狀診斷表

| 現象 | 原因 |
|---|---|
| 本機正常，跨機器花屏或卡死 | `recv` 沒迴圈收滿；長度欄位 byte order |
| 延遲數字隨時間一直變大 | queue 無上限堆積（網路/decoder 慢於 encoder） |
| 延遲固定多 40~200ms 且忽高忽低 | 沒設 `TCP_NODELAY` |
| 延遲固定多 1~3 張畫面的時間 | B-frame / encoder lookahead / decoder 沒開 low latency |
| 開始幾秒後 receiver 花屏、之後一直花 | 丟了 encoded frame（P-frame）沒強制 IDR；或 recv 錯位後沒有重新同步 |
| 切解析度後黑屏 | receiver 沒處理 STREAM_CHANGE；或 sender 沒重建 encoder |
| 切解析度後畫面被拉伸/只剩一角 | VP 的 rect 沒更新 |
| 關掉 receiver 後 sender 卡住 | send thread 結束了但 capture thread 卡在 push 滿的 queue；或 encoder async 迴圈在等事件 |
| 隨機 crash，多在 encoder 內部 | device 沒開 multithread protection |
| 10 分鐘記憶體一路上升 | decoder `pSample` 沒 Release、`pEvents` 沒 Release、每幀 `CreateTexture2D`/`MFCreateSample` 沒釋放、queue 無上限 |
| GPU 使用率不高但 CPU 很高 | 軟體 encoder fallback、或某處 Map + CPU 處理 |
| 跑約 3.5 分鐘後時間戳異常 | 時間戳用了 32-bit |

### 報告評閱基準

學員報告要回答三件事，以下是「好的答案」長什麼樣：

**(1) 三個延遲數字**
- ✅ 有數字、有標明硬體/軟體、有說明量測的起點終點、最好有平均與最大值（或 P95）。
- ❌ 只有一次的數字、不說明怎麼量的、數字明顯不合理（例如端到端 < 編碼時間）卻沒察覺。

**(2) 哪個環節最慢，為什麼**
- ✅ 能用數據指出，並給出合理機制解釋，例如：
  - 「端到端比 ①+② 多約 10ms，主要是 Present 等 vsync」
  - 「② 很高是因為 decoder 為了 reorder hold 住了幾張，開 low latency 後下降」
  - 「軟體 encoder 是瓶頸」
- ❌ 只說「編碼比較慢」沒有數據或原因。

**(3) 下一步降延遲會調什麼，為什麼**
好答案（任何 2~3 個並說得出理由即可）：
- 關 B-frame / 開 low latency mode（encoder & decoder）。
- `TCP_NODELAY`；或改 UDP（但要處理丟包與 FEC/重傳）。
- 降 queue 深度、改成「永遠只送最新的一張」。
- Present 不等 vsync（allow tearing）或改善 render 時機。
- 縮短 GOP 不會降延遲（這是陷阱，如果學員這樣說要追問——GOP 影響的是恢復速度與 bitrate 尖峰）。
- 降解析度 / bitrate 降低編碼與傳輸時間。
- 使用 slice-based / sub-frame 輸出（進階）。

### 口試追問

1. **Q：TCP 是可靠的，為什麼還需要 length prefix？**
   A：可靠 ≠ 有訊息邊界。TCP 是 byte stream，兩次 send 可能被一次 recv 收到、一次 send 也可能分多次 recv。
2. **Q：你的 receiver 在本機跑沒問題，換成跨機器可能會出什麼問題？**
   A：`recv` 收不滿（若沒 recvAll）、Nagle、頻寬不足導致 queue 堆積、時鐘無法比較。
3. **Q：網路突然變慢，你的 sender 會怎樣？記憶體會怎樣？延遲會怎樣？**
   A：看 queue 設計。好答案：bounded queue + 在 encoder 前丟原始畫面 / 強制 IDR。
4. **Q：為什麼不能直接丟掉 send queue 裡的某幾個 encoded frame？**
   A：P-frame 參考前面的 frame，丟了之後所有後續 frame 都會錯直到下一個 IDR。
5. **Q：整條路徑上，資料在哪幾個點落到 CPU 記憶體？每一個都是必要的嗎？**
   A：只有 encoder 輸出 bitstream → socket → decoder 輸入 bitstream。其他都在 GPU。軟體 encoder fallback 時 encoder 內部會 readback（不是學員程式碼造成，但要知道）。
6. **Q：切換解析度時，encoder 和 decoder 各發生了什麼事？**
   A：encoder 重建或重設 type，第一張是帶新 SPS 的 IDR；decoder 看到新 SPS 回 STREAM_CHANGE，重新協商。
7. **Q：兩台機器的延遲你怎麼量？**
   A：時鐘不同步不能直接相減；用 RTT、或拍照比對螢幕計時器、或 NTP/PTP 同步並說明誤差。
8. **Q：Receiver 被關掉時，sender 的每個 thread 是怎麼知道要結束的？**
   A：考 thread 協調設計：send 失敗 → stop flag / event → 每個阻塞點（queue pop、GetEvent、AcquireNextFrame）都要有逾時或可被喚醒。

### 評分（Lab 6，100 分）

| 項目 | 分數 |
|---|---|
| 可即時運作（畫面、順暢、ESC 結束） | 15 |
| **全程 GPU（核心考點）** | 25 |
| TCP framing 正確（recvAll、prefix、上限檢查、NODELAY） | 15 |
| Thread 架構、backpressure、斷線處理 | 15 |
| 延遲量測正確性 | 10 |
| 解析度動態切換 | 10 |
| 報告品質（數據 + 分析 + 改善方向） | 10 |

---

## 附錄 A：症狀 → 原因 快速診斷表

學員來問「為什麼會這樣」時，先查這張表。

### 畫面外觀

| 症狀 | 最可能原因 | 相關 Lab |
|---|---|---|
| 圖片歪斜（剪切） | 沒用 `RowPitch` | 1, 3, 5 |
| 上下顛倒 | BMP bottom-up | 1 |
| 紅藍互換 | BGRA/RGBA 搞混；或 U/V 對調 | 1, 3 |
| 整體偏綠 | UV 讀到 0 / 偏移錯 | 3, 5 |
| 霧霧的、黑不夠黑 | studio 資料當 full 解 | 3, 5, 6 |
| 對比過高、暗部死黑 | full 資料當 studio 解 | 3, 5, 6 |
| 灰階對、飽和色偏 | 601 / 709 不一致 | 3, 5, 6 |
| 底部 8 行綠條 | 1088 對齊沒裁 | 5, 6 |
| 黑邊有殘影 | VP 沒設背景色 | 2 |
| 偶發畫面倒退、重複 | NV12 texture 沒 pool 被覆寫 | 4, 6 |
| 持續花屏直到某一刻恢復 | 丟包 / 丟了 P-frame，等到下一個 IDR | 6 |

### HRESULT

| HRESULT | 名稱 | 原因 |
|---|---|---|
| `0x887A0027` | `DXGI_ERROR_WAIT_TIMEOUT` | 畫面沒變，正常 |
| `0x887A0026` | `DXGI_ERROR_ACCESS_LOST` | 重建 duplication |
| `0x80070005` | `E_ACCESSDENIED` | secure desktop（鎖屏/UAC） |
| `0x887A0004` | `DXGI_ERROR_UNSUPPORTED` | 混合顯卡 adapter 錯誤、遠端桌面 session |
| `0x887A0022` | `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE` | 太多程式同時使用 DDA |
| `0x887A0001` | `DXGI_ERROR_INVALID_CALL` | 例如 timeout 後仍呼叫 `ReleaseFrame`、或上一張還沒 Release 又 Acquire |
| `0xC00D6D72` | `MF_E_TRANSFORM_NEED_MORE_INPUT` | 正常，再送資料 |
| `0xC00D6D61` | `MF_E_TRANSFORM_STREAM_CHANGE` | 重新協商 output type |
| `0xC00D6D60` | `MF_E_TRANSFORM_TYPE_NOT_SET` | 沒設 MediaType |
| `0xC00D6D77` | `MF_E_TRANSFORM_ASYNC_LOCKED` | async MFT 沒 unlock |
| `0xC00D36B4` | `MF_E_INVALIDMEDIATYPE` | MediaType 不被接受 |
| `0xC00D36B3` | `MF_E_INVALIDSTREAMNUMBER` | 傳錯 stream ID（有些 MFT 的 stream ID 不是 0，用 `GetStreamIDs` 查） |
| `0xC00D36B5` | `MF_E_NOTACCEPTING` | 輸出還沒拿、或 async 沒等 NeedInput |
| （查表） | `MF_E_NO_SAMPLE_TIMESTAMP` | 沒 `SetSampleTime` |

> HRESULT 數值建議你開訓前在自己機器上用 `FormatMessage` 或 Visual Studio 的「錯誤查詢」工具核對一次；學員回報錯誤時請他們一律附上 16 進位數值。

---

## 附錄 B：評分表範本

複製這張表給每位學員、每題一份：

```
學員：__________   Lab：__   Review 日期：__________   Mentor：__________

[驗收]
  □ 照 README 能編譯（Debug / Release）
  □ 驗收標準 1：________________________   結果：________
  □ 驗收標準 2：________________________   結果：________
  □ 額外挑戰：__________________________   結果：________

[Code Review]
  🔴 未通過項目（需修正後重交）：
    -
  🟡 應改善項目：
    -
  🟢 加分亮點：
    -

[口試]
  Q1：______________________  回答：□ 完整 □ 部分 □ 不清楚
  Q2：______________________  回答：□ 完整 □ 部分 □ 不清楚
  Q3：______________________  回答：□ 完整 □ 部分 □ 不清楚

[分數]  ____ / 100
  通過標準建議：≥ 70 分且無 🔴 項目

[給學員的回饋]（先寫做得好的觀念，再寫要修正的）
```

### 整體判斷學員「真的懂了」的訊號

- 能不看程式碼，畫出 Lab 6 的資料流，並標出每個環節資料在 GPU 還是 CPU。
- 被問到「為什麼」時，回答的是**機制**（非同步、buffer、生命週期、合約），而不是「文件這樣寫」。
- 遇到 bug 時，第一步是印出 HRESULT 並查它的意義，而不是改東改西碰運氣。
- 能主動指出自己實作的限制（例如「我沒做 backpressure，網路慢時延遲會一直上升」）——這比「什麼都做了但說不清楚」更有價值。
