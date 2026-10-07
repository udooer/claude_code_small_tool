# Lab 1 複習筆記：桌面擷取（DXGI Desktop Duplication）

> 這份筆記整理了實際跑 Lab 1 時提出的問題與解答，方便日後複習或帶學員時參考。
> 程式碼位置以 `WMF_Training/labs/` 為根目錄。

**實測環境**：ASUS 筆電，Intel UHD Graphics 620（driver 27.20.100.8681）＋ NVIDIA GeForce MX250，單螢幕 1920×1080。

---

## 目錄

1. [列出 GPU 與螢幕：`PrintOutputs()`](#1-列出-gpu-與螢幕printoutputs)
2. [`D3DContext` 結構：各欄位的關係](#2-d3dcontext-結構各欄位的關係)
3. [device 與 context 的差別](#3-device-與-context-的差別)
4. [為什麼 `Map` 會讓 CPU 等 GPU](#4-為什麼-map-會讓-cpu-等-gpu)
5. [`GetImmediateContext()` 和結構裡的 context 是否等價](#5-getimmediatecontext-和結構裡的-context-是否等價)
6. [為什麼第 2 次 Acquire 才拿到畫面](#6-為什麼第-2-次-acquire-才拿到畫面)
7. [自我檢查題](#7-自我檢查題)

---

## 1. 列出 GPU 與螢幕：`PrintOutputs()`

**位置**：`common/d3d_util.cpp` → `PrintOutputs()`；執行 `dda_capture.exe --list`

**一句話**：用兩層迴圈列出「每張 GPU（adapter）」以及「接在它上面的每個螢幕（output）」。

### 程式結構

```cpp
CreateDXGIFactory1(...)                              // DXGI 的入口
for (a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)   // 每張 GPU
    adapter->GetDesc1(&ad);                          // 名稱、VRAM、LUID
    for (o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)  // 每個螢幕
        output->GetDesc(&od);                        // DeviceName、DesktopCoordinates
        printf("--output %u", global++);             // 跨所有 GPU 的螢幕總編號
```

| 名詞 | 意義 |
|---|---|
| Factory（`IDXGIFactory1`） | DXGI 的入口，所有列舉都從它開始 |
| Adapter（`IDXGIAdapter1`） | 一張 GPU（實體或軟體）。用 `...1` 版本才拿得到 LUID |
| Output（`IDXGIOutput`） | 一個接在這張 GPU 上的螢幕 |
| `DXGI_ERROR_NOT_FOUND` | 列舉結束的標準訊號，不是錯誤 |
| `DesktopCoordinates` | 螢幕在虛擬桌面上的位置與大小。主螢幕左上角是 `(0,0)`；副螢幕在左邊時座標為負數 |
| `global` | 跨所有 GPU 連續編號，使用者只需要說 `--output N`，不用知道螢幕接在哪張 GPU 上 |

### 實測輸出與解讀

```
Adapter 0: Intel(R) UHD Graphics 620
  --output 0 : \\.\DISPLAY1  1920x1080 at (0,0)
Adapter 1: NVIDIA GeForce MX250
Adapter 2: Microsoft Basic Render Driver
```

- **Intel UHD 620 有 output**：筆電螢幕實際接在內顯上。
- **NVIDIA MX250 沒有 output**：這是混合顯卡（NVIDIA Optimus）的典型配置。NVIDIA 只負責運算，算好的畫面複製給 Intel，再由 Intel 輸出到螢幕。
- **Microsoft Basic Render Driver**：Windows 內建的軟體 GPU（WARP），用 CPU 模擬 D3D。沒有螢幕，也沒有硬體編解碼，Lab 用不到。

### 為什麼重要：DDA 的硬規則（勘誤 M2）

> **D3D11 device 必須建在「擁有這個螢幕的 GPU」上**，否則 `DuplicateOutput` 會回傳 `DXGI_ERROR_UNSUPPORTED (0x887A0004)`。

原訓練文件的寫法是「用預設 GPU 建 device，再找它的螢幕」。在這台筆電上，如果 NVIDIA 控制台把程式指定給 MX250，就會失敗。`CreateD3DForOutput()` 用同樣的雙層迴圈先找到螢幕，再在**它所屬的 adapter** 上建 device：

```cpp
D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, ...);
//                ↑ 指定 adapter 時，driver type 必須是 UNKNOWN
```

---

## 2. `D3DContext` 結構：各欄位的關係

**位置**：`common/d3d_util.h`

```cpp
struct D3DContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;     // 只有 CreateD3DForOutput 會填
    DXGI_OUTPUT_DESC outputDesc{};
    std::wstring adapterName;
};
```

### 關係圖

```
                 adapter（一張 GPU）  ── adapterName（只用來印 log）
                  │                 │
     「螢幕接在它上面」               「在它上面建立」
                  ▼                 ▼
     output（一個螢幕）            device（使用這張 GPU 的身分）
     outputDesc（螢幕資訊快照）          │ 每個 device 有一個
                  │                     ▼
                  │                context（下指令給 GPU）
                  │                     │
                  └── output->DuplicateOutput(device) ──┘
                      ↑ 兩者必須屬於同一張 adapter
```

### 各欄位

| 欄位 | 是什麼 | 用在哪裡 |
|---|---|---|
| `adapter` | 一張 GPU，所有東西的根 | 取 LUID：Lab 4 挑選和 device 在同一張 GPU 上的硬體 encoder |
| `adapterName` | GPU 名稱（寬字元） | 印 log |
| `device` | 「建立」資源 | `CreateTexture2D`、`DuplicateOutput`、Video Processor、分享給 encoder/decoder 的 device manager |
| `context` | 「使用」資源、下指令 | `CopyResource`、`Map`、`VideoProcessorBlt` |
| `output` | 一個螢幕。用 `IDXGIOutput1`，因為 **`DuplicateOutput` 只在 `IDXGIOutput1` 上** | DDA 擷取 |
| `outputDesc` | 螢幕資訊的**快照**（不會自動更新） | 印資訊；Lab 3 色塊視窗要蓋滿螢幕 |

### 兩種建立方式

| 函式 | 會不會填 `output` | 用在 |
|---|---|---|
| `CreateD3DForOutput(n)` | 會 | 需要擷取螢幕：Lab 1~4、Lab 6 sender |
| `CreateD3DDefault()` | 不會（`nullptr`） | 只需要 GPU：Lab 5、Lab 6 receiver、codec_loopback |

### 為什麼包成一個結構

1. **從結構上保證「同一張 GPU」**，避免呼叫端自己配錯。
2. **一次傳遞整套環境**，後面的函式需要的都是這一組。
3. **生命週期由 RAII 管理**：每個欄位都是 `ComPtr`，離開 scope 時自動 Release；複製結構時自動 AddRef。要注意 `MfScope` 必須比它先宣告、最後解構。

---

## 3. device 與 context 的差別

**一句話**：device 是「倉庫管理員」，負責建立資源；context 是「施工隊長」，負責下指令使用資源。

| | device（`ID3D11Device`） | context（`ID3D11DeviceContext`） |
|---|---|---|
| 做什麼 | 建立：texture、view、query、video processor | 使用：Copy、Map、Draw、`VideoProcessorBlt` |
| 有沒有狀態 | 幾乎沒有，每次呼叫獨立 | 有：綁了哪個 render target、shader、viewport… |
| 多執行緒 | 可以同時呼叫 | immediate context **不是**執行緒安全的 |
| 呼叫回傳時 | 建立完成 | **GPU 還沒做**，只是排進佇列 |

### 為什麼拆成兩個

- **建立資源是無狀態的**，兩個執行緒同時要 texture，各給一張即可，容易做成執行緒安全。
- **下指令是有狀態的**：GPU 管線是一台狀態機，A 執行緒剛設好 render target，B 執行緒就改掉了，A 的 Draw 會畫錯地方。
- **歷史教訓**：D3D9 只有一個 device 包辦兩件事，多執行緒只能整個 device 加一把大鎖。D3D11 拆開之後，建立資源不會被下指令的鎖擋住；需要時還能用 deferred context 讓多個執行緒各自錄指令。

### 在我們的 Lab 裡的影響

**(1) 執行緒安全（勘誤 M1）**：Lab 4 的 Intel encoder 在自己的執行緒，透過 device manager 使用**同一個** device 的 immediate context。沒有保護的話會隨機 crash 或畫面錯亂，而且很難重現。解法：

```cpp
// common/d3d_util.cpp  EnableMultithreadProtection()
mt->SetMultithreadProtected(TRUE);   // runtime 自動替每個 context 呼叫加鎖
```

只要 device 要分享給 MF 的 encoder 或 decoder，就一定要開。

**(2) 非同步執行**：context 的指令只是寫進 command buffer，GPU 之後才執行。

```
CPU:  [Copy 0.01ms][Blt 0.02ms][Map ......等待......][存 BMP]
GPU:          [── Copy 0.3ms ──][──── Blt 0.5ms ────]
```

| 後果 | 對應 Lab |
|---|---|
| 用 QPC 包住 `VideoProcessorBlt` 只量到「排進佇列」的時間，要用 query 等 GPU 做完 | Lab 2 計時 |
| `Map` 會讓 CPU 停下來等 GPU | Lab 1、3、5 存 BMP；Lab 6 禁止每幀 Map |
| **同一個 context 內的順序有保證**：Copy 一定在 Blt 之前做完，不需要自己同步 | Lab 2、3 |
| 交給其他元件（async encoder）的資源，要等對方用完才能重用 | Lab 4 的 `Nv12SamplePool`（勘誤 M9） |

---

## 4. 為什麼 `Map` 會讓 CPU 等 GPU

**一句話**：`Map(READ)` 承諾回傳一個 CPU 能**立刻讀到正確資料**的指標，但寫入資料的 GPU 指令可能根本還沒送出，所以它只能等。

### Map 內部的步驟

```cpp
// common/d3d_util.cpp  ReadbackBgra()
ctx->CopyResource(staging, tex);              // ① 只是排進佇列，還沒發生
ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);  // ② 這裡開始等
```

| 步驟 | 做什麼 |
|---|---|
| (a) 追蹤資源 | driver 用 **fence 值**記錄「最後寫入 staging 的指令」，發現它還沒完成 |
| (b) 隱性 Flush | Copy 還在 CPU 端的 command buffer，不送出 GPU 就永遠不會做，所以強迫提早送出（批次變小，成本變高） |
| (c) 等待 | CPU 執行緒在 fence 上**睡眠**，直到 GPU 做完佇列中**排在前面的所有工作**和這個 Copy |
| (d) 快取一致性 | 確保 GPU 寫的資料對 CPU 可見。內顯（UHD 620）共用系統記憶體；**獨顯還要經 PCIe 傳回**（1080p BGRA 約 8 MB） |
| (e) 回傳 | 把記憶體對應到 process，回傳 `pData` 和 `RowPitch`（有對齊 padding，所以 ≠ width×4） |

### 為什麼不能放在每幀的路徑上

```
正常管線（CPU、GPU 同時忙）：
CPU:  [準備1][準備2][準備3][準備4]
GPU:         [做1  ][做2  ][做3  ]

每幀都 Map（兩邊輪流閒置）：
CPU:  [準備1][等......][讀1][準備2][等......][讀2]
GPU:         [做1    ]              [做2    ]
```

- 延遲變高：每幀都要清空整條 GPU 佇列，包括 encoder 排在前面的工作。
- 吞吐量下降：CPU 和 GPU 各只有一半時間在工作。
- 典型症狀：**CPU、GPU 使用率都不高，程式卻很慢。**

### 真的必須每幀讀回時

多張 staging 輪流使用，搭配 `D3D11_MAP_FLAG_DO_NOT_WAIT`，以延遲幾幀為代價換取不阻塞：

```cpp
HRESULT hr = ctx->Map(staging[(n - 2) % 3].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
if (hr == DXGI_ERROR_WAS_STILL_DRAWING) { /* 還沒好，下次再拿 */ }
```

> 進階題：`common/nv12_pool.h` 的 `MakeCpuNv12Sample()`（軟體 encoder fallback）目前是「Copy 完立刻 Map」的阻塞寫法，可以改成上面的 ring buffer。

### 不同 Map 的等待行為

| 情境 | 會不會等 |
|---|---|
| `STAGING` + `MAP_READ` | **會**：要等 GPU 寫完 |
| `STAGING` + `MAP_WRITE` | 可能會：要等 GPU 讀完 |
| `DYNAMIC` + `MAP_WRITE_DISCARD` | 通常不會：driver 給一塊新記憶體（renaming） |
| 加上 `D3D11_MAP_FLAG_DO_NOT_WAIT` | **不會**：還沒好就回傳 `DXGI_ERROR_WAS_STILL_DRAWING` |
| `DEFAULT` usage（例如 DDA 的 texture） | 不能 Map，所以要先 Copy 到 staging |

---

## 5. `GetImmediateContext()` 和結構裡的 context 是否等價

**位置**：`common/desktop_capture.cpp` → `DesktopCapture::Init()`

```cpp
device->GetImmediateContext(&context_);
```

**一句話**：**完全等價，是同一個物件。** 每個 device 只有一個 immediate context，`GetImmediateContext()` 不管呼叫幾次，都回傳同一個物件（並 AddRef）。

```cpp
ComPtr<ID3D11DeviceContext> ctx2;
d3d.device->GetImmediateContext(&ctx2);
assert(ctx2.Get() == d3d.context.Get());   // 成立
```

### 那為什麼只收 device、自己取 context

| 理由 | 說明 |
|---|---|
| **不會配錯** | 如果介面收 context，呼叫端可能傳進「別的 device 的 context」（Lab 6 同一個 process 有多個 device）→ D3D 報錯或 crash。只收 device 就從設計上排除了這個可能 |
| **一定是 immediate context** | 不會誤收 deferred context：deferred context 只會錄指令不執行，而且不能 `Map(READ)`，擷取到的畫面會永遠是空的 |
| **介面精簡** | 能從已有參數推導出來的就不要再要求呼叫端提供，同一份資料只有一個來源 |
| **方便重建** | `Reinit()` 只需要 device 和 output |

### 推論

`DesktopCapture` 的 `context_`、`VideoScaler` 的 `videoContext_`（從 immediate context `QueryInterface` 成 `ID3D11VideoContext`）、`RenderWindow` 的 `context_`、`d3d.context`，**全部指向同一個 GPU 指令佇列**。

- 好處：同一個執行緒裡的「Copy → Blt」順序由 GPU 保證。
- 代價：大家共用一個佇列，device 分享給 encoder 時必須開 multithread protection。

`D3DContext` 結構裡仍然存一份 context，是為了讓**呼叫端**方便直接使用，例如 `SaveBgraTextureAsBmp`、`WaitForGpu`。

---

## 6. 為什麼第 2 次 Acquire 才拿到畫面

**實測輸出**：

```
Duplication: 1920x1080
  attempt 1: no new desktop image (timeout or pointer-only update), retrying
Got a frame on attempt 2
Saved capture.bmp
```

### DDA 的機制：有變化才給畫面

DDA 不是固定間隔拍照，而是**DWM 有合成新的桌面（Present）時才給新 frame**：

```
AcquireNextFrame(500)
 ├─ 上次之後桌面有 Present 過      → 立刻回傳 frame
 └─ 沒有                          → 最多等 500ms
      ├─ 期間有 Present           → 立刻回傳
      └─ 500ms 都沒變化           → DXGI_ERROR_WAIT_TIMEOUT
```

### 第 1 次失敗的兩種可能

| | 情境 A：真的 timeout | 情境 B：拿到「空」frame |
|---|---|---|
| 回傳值 | `DXGI_ERROR_WAIT_TIMEOUT` | `S_OK`，但 `LastPresentTime == 0` |
| 等了多久 | 約 500ms | 幾乎立刻 |
| 原因 | 桌面完全沒有變化 | 只有滑鼠移動（游標是硬體疊加層，不經 DWM 合成），或剛 `DuplicateOutput` 後 driver 給的初始 frame（`AccumulatedFrames=0`，內容常常是全黑） |
| 要不要 `ReleaseFrame` | **不要**，根本沒拿到 frame | **要**（程式用 RAII guard 自動處理） |
| 程式回傳 | `CaptureStatus::NoChange` | `CaptureStatus::PointerOnly` |

**勘誤 M3 的重點**：`AcquireNextFrame` 成功**不代表**有新畫面，一定要檢查 `LastPresentTime`。不檢查的話，最常見的結果是存出一張**全黑**的 `capture.bmp`。

### 第 2 次為什麼就成功

很可能是**程式自己觸發的**：

```
attempt 1 失敗 → printf 印出訊息 → 終端機視窗內容改變
→ DWM 重新合成桌面並 Present → attempt 2 立刻拿到新 frame
```

其他常見觸發來源：終端機游標閃爍、工作列時鐘、滑鼠 hover 造成按鈕高亮。現代桌面很少真的完全靜止。

### 程式改進（commit `d0db1b5`）

原本兩種情況印成同一句話，無法判斷。現在分開回報：

```
# 情境 A
  attempt 1: DXGI_ERROR_WAIT_TIMEOUT after 500 ms -> the desktop did not change at all
# 情境 B
  attempt 1: frame returned in 0.3 ms but LastPresentTime=0 (AccumulatedFrames=0, mouse update=no) -> no new desktop image yet, skipped
```

判斷方式：第 1 次等了約 500ms → 情境 A；幾乎馬上回傳且 `AccumulatedFrames=0` → 情境 B（driver 初始空 frame）；`mouse update=yes` → 剛好在動滑鼠。

### 和後面 Lab 的關聯

| Lab | 影響 |
|---|---|
| Lab 1 | 必須重試，只接受 `LastPresentTime != 0` 的 frame |
| Lab 4 | 要剛好 150 張（30fps × 5 秒），桌面靜止時沒有新 frame → timeout 時**重送上一張**（輸出中的 `repeated` 數字） |
| Lab 6 | 反過來利用：畫面沒變就不編碼、不傳送，省頻寬。桌面靜止時 `captured fps` 接近 0 是正常的 |

---

## 7. 自我檢查題

答得出來就代表真的懂了，不是背出來的。

1. 為什麼 NVIDIA MX250 底下沒有 output？那遊戲畫面是怎麼顯示出來的？
2. 如果用 NVIDIA 建 device 去擷取筆電螢幕，會得到什麼錯誤？為什麼？
3. `DesktopCoordinates` 什麼時候會出現負數？
4. 為什麼要用 `IDXGIOutput1` 而不是 `IDXGIOutput`？
5. Lab 5 為什麼不需要 `output`？
6. device 和 context 各自負責什麼？為什麼 D3D11 要把它們拆開？
7. Lab 4 的 encoder 在另一個執行緒使用同一個 device，沒有開 multithread protection 會怎樣？
8. 用 QPC 量 `VideoProcessorBlt` 得到 0.02ms，為什麼這個數字可能是錯的？
9. `Map(READ)` 內部做了哪些事？為什麼會等多久跟「你自己的 Copy 有多快」關係不大？
10. 每幀都 Map，為什麼會出現「CPU、GPU 使用率都不高，程式卻很慢」？
11. 真的需要每幀讀回時，要怎麼避免 stall？
12. `GetImmediateContext()` 拿到的和 `D3D11CreateDevice` 回傳的 context 是同一個嗎？為什麼類別只收 device 參數？
13. 如果有人把 `DesktopCapture::Init` 改成接收 context 參數，最可能出什麼 bug？
14. `AcquireNextFrame` 回傳 `S_OK`，就一定有新畫面嗎？
15. `WAIT_TIMEOUT` 和 `LastPresentTime == 0` 的處理有什麼不同？哪一個需要 `ReleaseFrame`？
16. 為什麼 Lab 1 的重試通常第 2 次就成功？
17. 桌面完全靜止 5 秒，Lab 4 要怎麼錄出 150 張？

<details>
<summary>參考答案（先自己想過再展開）</summary>

1. 混合顯卡（Optimus）：螢幕接在 Intel 上；NVIDIA 算好的畫面會複製到 Intel 的記憶體，再由 Intel 輸出。
2. `DXGI_ERROR_UNSUPPORTED (0x887A0004)`：DDA 要求 device 和螢幕屬於同一張 adapter。
3. 副螢幕放在主螢幕的左邊或上面時。
4. `DuplicateOutput` 只定義在 `IDXGIOutput1` 上。
5. 解碼不需要擷取螢幕，只需要一個能使用 GPU 的 device。
6. device 建立資源，無狀態、執行緒安全；context 下指令，帶管線狀態、不是執行緒安全的、非同步執行。拆開是為了讓建立資源不被下指令的鎖拖累（D3D9 單一 device 的教訓）。
7. 兩個執行緒同時用 immediate context，指令和狀態交錯：隨機 crash、畫面錯亂、`DEVICE_REMOVED`，而且很難重現。
8. context 呼叫是非同步的，只量到排進佇列的時間；要用 `D3D11_QUERY_EVENT` 等 GPU 做完。
9. 追蹤 fence、隱性 Flush、在 fence 上睡眠等待、快取同步（獨顯還有 PCIe 傳輸）、對應記憶體。要等 GPU 佇列中**排在前面的所有工作**都做完，例如 encoder 的工作。
10. CPU 和 GPU 從「同時工作」變成「輪流等對方」，管線出現大量空泡。
11. 多張 staging 輪流使用 + `D3D11_MAP_FLAG_DO_NOT_WAIT`，接受晚幾幀拿到資料。
12. 是同一個（每個 device 只有一個 immediate context）。只收 device 可以避免配錯 device、避免誤收 deferred context，介面也比較精簡。
13. 傳進別的 device 的 context（D3D 報錯或 crash），或傳進 deferred context（指令只被錄下不執行、Map 失敗）。
14. 不一定，要檢查 `LastPresentTime`；為 0 時只有滑鼠更新，或是初始的空 frame。
15. timeout 時沒拿到 frame，**不能** `ReleaseFrame`；`LastPresentTime == 0` 時有拿到 frame，**必須** `ReleaseFrame`。
16. 桌面很少完全靜止，例如程式自己印出的訊息、游標閃爍，都會觸發 DWM 重新合成。
17. timeout 時重送上一張畫面（所以要有自己擁有的 stable texture），時間戳照樣遞增。

</details>
