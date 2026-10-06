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
