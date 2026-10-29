# Lab 2 複習筆記：GPU 縮放（ID3D11VideoProcessor）

> 這份筆記整理了做 Lab 2 時提出的問題與解答，方便日後複習或帶學員時參考。
> 程式碼位置以 `WMF_Training/labs/` 為根目錄。
> Lab 1 的筆記（device/context、Map stall、非同步）見 [../lab1_capture/NOTES.md](../lab1_capture/NOTES.md)，本篇會直接引用。

**注意**：文中的預期數值（content rect 等）以 **1920×1080** 的來源推算，尚未在實機上逐一驗證。螢幕解析度不同時，用第 4 節的公式自己代入。

---

## 目錄

1. [Texture、View 的關係，以及 SRV / RTV 是什麼](#1-textureview-的關係以及-srv--rtv-是什麼)
2. [Bind flags 的意義與用法](#2-bind-flags-的意義與用法)
3. [`VideoScaler::Init` 程式解析](#3-videoscalerinit-程式解析)
4. [Content rect 的計算：為什麼 1366×768 是右邊 2px 黑邊、奇數尺寸為什麼被修正](#4-content-rect-的計算)
5. [`VideoScaler::Process` 程式解析](#5-videoscalerprocess-程式解析)
6. [`WaitForGpu`：為什麼要等、怎麼等](#6-waitforgpu為什麼要等怎麼等)
7. [`EnablePerMonitorDpiAwareness`：DPI 設定](#7-enablepermonitordpiawarenessdpi-設定)
8. [如何測試 Lab 2](#8-如何測試-lab-2)
9. [自我檢查題](#9-自我檢查題)

---

## 1. Texture、View 的關係，以及 SRV / RTV 是什麼

**一句話**：Texture 是顯示記憶體裡的一塊原始資料，只有大小和格式。View 是「為某個用途、看其中哪一部分、用什麼格式解讀」的描述。**GPU 管線的各個階段只認 view，不直接碰 texture。**

### 三層結構

```
ID3D11Resource                     ← 「GPU 上的一塊記憶體」
 ├─ ID3D11Buffer
 └─ ID3D11Texture2D   ◀── Lab 裡幾乎都是這個

ID3D11View                         ← 「怎麼使用」某個 resource
 ├─ ID3D11ShaderResourceView   (SRV)  給 shader 讀
 ├─ ID3D11RenderTargetView     (RTV)  給管線寫（畫上去）
 ├─ ID3D11DepthStencilView     (DSV)  深度緩衝
 ├─ ID3D11UnorderedAccessView  (UAV)  compute shader 任意讀寫
 └─ Video 專用
     ├─ ID3D11VideoDecoderOutputView     解碼器寫入
     ├─ ID3D11VideoProcessorInputView    video processor 讀取
     └─ ID3D11VideoProcessorOutputView   video processor 寫入
```

| 概念 | 比喻 |
|---|---|
| Texture | 倉庫，裡面堆著貨（像素） |
| Bind flag | 倉庫蓋好時申請的用途許可 |
| View | 某個部門的通行證：能進哪一區、用什麼方式解讀貨物 |
| 管線的各個階段 | 員工，**只認通行證，不認倉庫** |

### 為什麼要多一層 view

一張 texture 可以有很多種「看法」，view 負責指定：

| View 可以指定 | 例子 |
|---|---|
| 哪一個 subresource | Lab 5 的 decoder 輸出是 **texture array**，view 指定 `ArraySlice` 只看其中一張 |
| 用什麼格式解讀 | 同一張 NV12：用 `R8_UNORM` 看是 Y 平面，用 `R8G8_UNORM` 看是 UV 平面 |
| 用途 | 同一張 texture 可以同時有 SRV（讀）和 RTV（寫） |

另外，view 建立時 runtime 會一次做完驗證，之後每幀使用都不必再檢查。

### 各種 view

| View | 用途 | 需要的 bind flag | 綁定方式 |
|---|---|---|---|
| **SRV**（Shader Resource View） | shader **讀取或取樣**，也就是「貼圖」 | `SHADER_RESOURCE` | `PSSetShaderResources` |
| **RTV**（Render Target View） | 繪圖輸出**寫入**，也就是「畫布」 | `RENDER_TARGET` | `OMSetRenderTargets`、`ClearRenderTargetView` |
| DSV | 3D 深度緩衝 | `DEPTH_STENCIL` | 影像處理用不到 |
| UAV | compute shader 任意位置讀寫 | `UNORDERED_ACCESS` | GPGPU、自己寫色彩轉換 |
| VP **Input** View | video processor 讀取（影片版的 SRV） | 一般 DEFAULT texture，常見是 `SHADER_RESOURCE` | `D3D11_VIDEO_PROCESSOR_STREAM::pInputSurface` |
| VP **Output** View | video processor 寫入（影片版的 RTV） | **`RENDER_TARGET`**（必要） | `VideoProcessorBlt` 的參數 |
| Decoder Output View | DXVA 解碼器寫入 | `DECODER` | MFT 內部處理 |

「RT」指的是 **render target**，也就是被畫的目標。RTV 是指向它的 view，`RENDER_TARGET` bind flag 則是許可。

### 什麼操作需要 view

| 操作 | 需要 view？ |
|---|---|
| `CopyResource`、`CopySubresourceRegion`、`UpdateSubresource` | ✘，直接操作 resource |
| `Map` / `Unmap` | ✘，看的是 Usage（STAGING / DYNAMIC） |
| shader 取樣 / 繪圖輸出 / compute 寫入 | ✔ SRV / RTV / UAV |
| `VideoProcessorBlt` | ✔ Input View + Output View |

### 各 Lab 的 texture 和 view

```
Lab 1/2  DDA 桌面 ──Copy──▶ frame ─[InputView]─▶ VideoProcessor ─[OutputView]─▶ scaled (BGRA)
                    （不需要 view）                                                 │
                                                       CopyResource ─▶ staging ─Map─▶ CPU
                                                       （bind=0，沒有 view）

Lab 3/4  frame ─[InputView]─▶ VideoProcessor ─[OutputView]─▶ pool 的 NV12 texture ─▶ IMFSample ─▶ Encoder

Lab 5    Decoder ─[DecoderOutputView]─▶ texture array[slice] ─[InputView(slice)]─▶ VideoProcessor
                                             ─[OutputView]─▶ swap chain back buffer ─Present─▶ 螢幕
```

Lab 5 的 `common/render_window.cpp:157-158`：`swapChain_->GetBuffer(0, ...)` 拿到的 back buffer 本身也是一張 `ID3D11Texture2D`，video processor 寫進去後再 `Present`。

### 生命週期

**View 會持有 texture 的參考（AddRef）**，只要 view 還在，texture 就不會被釋放。所以快取 view 等於讓 texture 一直活著（見第 5 節）。

---

## 2. Bind flags 的意義與用法

**一句話**：建立 texture 時宣告「將來會被管線的哪些階段使用」。**建立後不能改**，而且每個 view 都要有對應的 bind flag 才建得出來。

### 為什麼要事先宣告

- **記憶體排列**：要被取樣和要被寫入的資料，最佳的 tiling 方式可能不同。
- **壓縮**：很多 GPU 會對 render target 做無損壓縮。
- **驗證**：建立 view 時就檢查用途合不合法，錯了直接回 `E_INVALIDARG`。

### 常見 bind flags

| Flag | 對應 | 在 Lab 裡 |
|---|---|---|
| `SHADER_RESOURCE` | SRV、VP input | 縮放的**來源** |
| `RENDER_TARGET` | RTV、**VP output** | 縮放和轉 NV12 的**目的地** |
| `UNORDERED_ACCESS` | UAV | 沒用到 |
| `DEPTH_STENCIL` | DSV | 沒用到 |
| `DECODER` | decoder output | Lab 5 的 decoder texture array |
| `VIDEO_ENCODER` | 硬體 encoder 讀取 | 一般不用手動設 |
| `0` | 沒有任何 view | **STAGING 必須是 0** |

### 回到 Lab 2 的程式（`lab2_scale/main.cpp:36-40`）

```cpp
ComPtr<ID3D11Texture2D> frame = CreateTexture(..., capture.Format(),
                                              D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
// Video processor 的輸出 texture 必須有 BIND_RENDER_TARGET
ComPtr<ID3D11Texture2D> scaled = CreateTexture(..., DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET);
```

- **`frame`**：DDA 用 `CopyResource` 複製進來（不需要 flag），再當作 VP 的輸入。
  - `SHADER_RESOURCE`：很多 driver 的 VP 內部用 shader 取樣來源。
  - `RENDER_TARGET`：Lab 2 本身用不到，是**保守寫法**：之後要畫游標或 `Clear` 時需要；DDA 自己的桌面 texture 也常帶這兩個 flag，設成一樣可以減少相容性問題。拿掉在多數機器上應該還能跑，但沒有在每家 driver 驗證過。
- **`scaled`**：`VideoProcessorBlt` 的寫入目標。`CreateVideoProcessorOutputView` 的文件明確要求 `RENDER_TARGET`，所以是**必要的**。之後只被 Copy 到 staging，不需要 `SHADER_RESOURCE`。
- Lab 3/4 的 `Nv12SamplePool`（`common/nv12_pool.h:31`）預設也是 `RENDER_TARGET`，理由相同。

### 和 Usage 的搭配規則

| Usage | GPU 讀 | GPU 寫 | CPU 存取 | 可用的 bind flags |
|---|---|---|---|---|
| `DEFAULT` | ✔ | ✔ | ✘（只能 `UpdateSubresource`） | 全部 |
| `IMMUTABLE` | ✔ | ✘ | ✘ | 只能 `SHADER_RESOURCE` |
| `DYNAMIC` | ✔ | ✘ | CPU 寫 | `SHADER_RESOURCE`，**不能** `RENDER_TARGET` |
| `STAGING` | 只能 Copy | 只能 Copy | CPU 讀寫（`Map`） | **必須是 0** |

```
DDA 桌面 ──Copy──▶ frame (DEFAULT, SRV|RT) ──Blt──▶ scaled (DEFAULT, RT) ──Copy──▶ staging (STAGING, 0) ──Map──▶ CPU
```

### 格式也會影響

不是每種格式都支援每種用途，例如 NV12 能不能當 render target 要看硬體：

```cpp
UINT support = 0;
device->CheckFormatSupport(DXGI_FORMAT_NV12, &support);
bool canRT = support & D3D11_FORMAT_SUPPORT_RENDER_TARGET;
```

### 常見錯誤

| 症狀 | 原因 |
|---|---|
| `CreateVideoProcessorOutputView` 回 `E_INVALIDARG` | 輸出 texture 沒有 `RENDER_TARGET` |
| `CreateTexture2D` 回 `E_INVALIDARG` | STAGING 設了 bind flags，或 DYNAMIC 加了 `RENDER_TARGET` |
| `Map` 回 `E_INVALIDARG` | 直接 Map DEFAULT texture（不是 bind flag 的問題，要先 Copy 到 STAGING） |
| 什麼 flag 都加 | 能跑，但可能讓 driver 選到較差的排列或關閉壓縮 |

開發時用 `D3D11_CREATE_DEVICE_DEBUG` 建 device，錯誤組合會在 debug output 說明缺了什麼。

---

## 3. `VideoScaler::Init` 程式解析

**位置**：`common/video_scaler.cpp` → `VideoScaler::Init`

**一句話**：取得影片專用介面 → 用 enumerator 確認 GPU 支援 → 建立 processor，並設定「從哪裡讀、畫到哪裡、其他地方填什麼」。只在初始化時做一次，每幀的 `Process()` 只負責 Blt。

```
ID3D11Device ──QI──▶ ID3D11VideoDevice ──▶ Enumerator（能力查詢）──▶ VideoProcessor
ID3D11DeviceContext ──QI──▶ ID3D11VideoContext ──▶ 設定 processor 狀態、執行 Blt
```

這和 Lab 1 的 device/context 分工一樣：VideoDevice 負責建立，VideoContext 負責設定與執行。

### 逐段說明

| 段落 | 程式 | 重點 |
|---|---|---|
| ① 清狀態 | `ClearViewCache(); vp_.Reset(); enum_.Reset();` | `Init` 可以重複呼叫。**view 和 enumerator 綁定**，換 enumerator 前一定要清快取 |
| ② 影片介面 | `device->QueryInterface(&videoDevice_)`；`GetImmediateContext` 後 `ctx.As(&videoContext_)` | device 必須用 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT` 建立（`common/d3d_util.cpp:17`），否則 QI 回 `E_NOINTERFACE`。`GetImmediateContext` 見 Lab 1 筆記第 5 節 |
| ③ ContentDesc + Enumerator | 輸入/輸出尺寸、progressive、`OPTIMAL_SPEED` | ContentDesc 只是**提示**，讓 driver 選硬體路徑；真正的縮放由 rect 決定。Enumerator 是**能力查詢**物件，本身不做處理 |
| ④ 格式檢查 | `CheckVideoProcessorFormat` 檢查 `SUPPORT_INPUT` / `SUPPORT_OUTPUT` | 支援是**分方向的**。先檢查，錯誤訊息才會明確；否則到建 view 或 Blt 時只會得到 `E_INVALIDARG` |
| ⑤ 建立 processor | `CreateVideoProcessor(enum_, 0, &vp_)` | `0` 是 rate conversion caps 的索引（deinterlace、幀率轉換的能力組），我們用第 0 組 |
| ⑥ 三個 rect | source / dest / target | 見下方 |
| ⑦ 關自動處理 | `VideoProcessorSetStreamAutoProcessingMode(..., FALSE)` | driver 預設可能做降噪、銳化、色彩增強，會讓 Lab 3 的色塊量測不準，不同 GPU 結果也不一致 |
| ⑧ 背景色 | `VideoProcessorSetOutputBackgroundColor(vp_, FALSE, &black)` | 見下方 |
| ⑨ 色彩空間 | `ApplyColorSpaces()` | BGRA→NV12 時決定矩陣和範圍（Lab 3 的重點）。`ColorSpace1` 在 `ID3D11VideoContext1` 上（勘誤 E4） |

### 三個 rect

| Rect | 屬於 | 座標系 | 意義 |
|---|---|---|---|
| **Source rect** | stream | 輸入 texture | **讀取**輸入的哪一塊（設成部分區域就是裁切） |
| **Dest rect** | stream | 輸出 texture | 讀到的內容**放到**輸出的哪裡（大小不同就會縮放） |
| **Target rect** | output | 輸出 texture | 這次 Blt **允許寫入**的範圍，範圍外完全不動 |

```
輸入 1920×1080                          輸出 1024×1024
┌──────────────────────┐                ┌────────────────┐ ─┐
│                      │                │ 背景色（黑）   │  │
│     source rect      │   縮放 0.533   ├────────────────┤ y=224
│   (0,0)-(1920,1080)  │ ─────────────▶ │   dest rect    │  │ target rect
│                      │                │(0,224)-(1024,800)│ │ (0,0)-(1024,1024)
│                      │                ├────────────────┤ y=800
└──────────────────────┘                │ 背景色（黑）   │  │
                                        └────────────────┘ ─┘
```

- 縮放倍率 = dest rect 大小 ÷ source rect 大小。
- target rect 內、dest rect 外的區域填背景色，就是黑邊。
- 參數中的 `0` 是 **stream index**：VP 是合成器，可以同時疊好幾個 stream（子母畫面、字幕），各自有 source/dest rect。target rect 和背景色屬於輸出，沒有 stream index。
- `TRUE` 代表啟用這個 rect；`FALSE` 時 driver 用預設值（通常是整張）。

### 背景色

| 細節 | 說明 |
|---|---|
| 第二個參數 `FALSE` | 用 RGBA 表示；`TRUE` 用 YCbCr。`D3D11_VIDEO_COLOR` 是 union |
| 數值範圍 | **浮點數 0.0–1.0**，不是 0–255。`A = 1` 是不透明 |
| 輸出是 NV12 時 | driver 依輸出色彩空間轉換：studio range 的黑是 Y=16、UV=128 |
| 不設的話 | 規格不保證內容。有些 driver 清成黑色，有些會留下殘影 |

### 檢視重點

| 重點 | 說明 |
|---|---|
| **setter 都回傳 `void`** | `VideoProcessorSet...` 參數錯了不會報錯，要到 `VideoProcessorBlt` 才會失敗或得到錯誤畫面。用 debug layer 看訊息 |
| **設定是持久狀態** | 設一次一直保留，所以放在 `Init`。反過來說，每幀改的狀態下一幀不會自動還原 |
| 重建 enumerator 沒清快取 | 用到綁在舊 enumerator 的 view |
| 沒檢查格式支援 | 換 GPU 失敗時看不出原因 |
| 沒關 auto processing | Lab 2 看不出來，Lab 3 的數值驗證才出錯 |

---

## 4. Content rect 的計算

**位置**：`common/video_scaler.cpp:35` → `ComputeDestRect`；`lab2_scale/main.cpp:23-24`

**一句話**：所有尺寸和座標都**強制對齊偶數**，原因是 Lab 3/4 要輸出的 NV12 格式。

### 公式

```cpp
double s = std::min((double)dstW / srcW, (double)dstH / srcH);      // 等比倍率，取較小的才放得下
LONG w = std::min((LONG)dstW, (LONG)(srcW * s + 0.5)) & ~1L;        // 四捨五入，再對齊偶數
LONG h = std::min((LONG)dstH, (LONG)(srcH * s + 0.5)) & ~1L;
LONG x = (((LONG)dstW - w) / 2) & ~1L;                              // 置中，再對齊偶數
LONG y = (((LONG)dstH - h) / 2) & ~1L;
return { x, y, x + w, y + h };
```

### 例子：`--width 1366 --height 768` → 右邊 2px 黑邊

| 步驟 | 計算 | 結果 |
|---|---|---|
| 寬度倍率 | 1366 / 1920 | 0.711458 |
| 高度倍率 | 768 / 1080 | **0.711111**（較小，高度貼滿） |
| w | 1920 × 0.711111 = 1365.33 → 四捨五入 1365 → 對齊偶數 | **1364** |
| h | 1080 × 0.711111 = 768 | 768 |
| x | (1366 − 1364) / 2 = 1 → 對齊偶數 | **0** |
| 結果 | | `(0,0)-(1364,768)` |

```
欄位：   0                                          1363 │1364 1365
        ├────────────── 畫面內容 1364 px ─────────────────┤ 黑   黑 │
```

- 1366×768 比 16:9 稍寬（1.7786 對 1.7778），所以高度貼滿、寬度不足。
- **為什麼不是左右各 1px**：置中的 `x = 1` 被對齊成 0，多出的 2px 全到右邊。
- 就算不對齊偶數，w = 1365、x = 0（整數除法），也是右邊 1px，因為 1px 無法平分。
- **為什麼不拉伸到 1366**：letterbox 嚴格保持長寬比。理想寬度 1365.33，拉到 1366 只變形 0.05%，看不出來。有些產品會設容許值（黑邊小於幾 px 就拉伸），學員這樣做只要能說出理由也合理。

### 例子：`--width 1279 --height 721` → 修正成 1278×720

```cpp
const UINT dstW = (UINT)args.GetInt("--width", 1280) & ~1u; // 偶數：為 Lab 3 的 NV12 鋪路
```

`~1u` 是「除了最低位以外全部是 1」的遮罩，AND 之後最低位被清成 0，也就是**無條件捨去到偶數**：

```
1279 = 0b100_1111_1111  & ~1 → 0b100_1111_1110 = 1278
 721 = 0b10_1101_0001   & ~1 → 0b10_1101_0000  =  720
```

往下捨去，保證結果不會比使用者要求的大。

### 根本原因：NV12 的色度是 2×2 共用

```
Y 平面（每像素一個）          UV 平面（每 2×2 一組）
┌──┬──┬──┬──┐
│Y0│Y1│Y2│Y3│                ┌─────┬─────┐
├──┼──┼──┼──┤      →         │ UV0 │ UV1 │
│Y4│Y5│Y6│Y7│                └─────┴─────┘
└──┴──┴──┴──┘           UV0 管 Y0,Y1,Y4,Y5
```

**(a) 尺寸必須是偶數**：寬 1279 時色度要 639.5 欄，半個樣本無法表示。

| 層級 | 限制 |
|---|---|
| D3D11 | `DXGI_FORMAT_NV12` texture 的寬高必須是 2 的倍數 |
| H.264 | 4:2:0 的裁切單位是 2 個亮度像素，奇數尺寸無法編碼 |
| Media Foundation | 奇數尺寸的 NV12 type，encoder 通常拒收 |

**(b) 座標也必須是偶數**：內容從奇數欄開始時，一組色度會同時管到黑邊和畫面，只能取平均，邊緣出現一條顏色不對的線（顏色滲漏）。所以 `x`、`y`、`w`、`h` 四個都要對齊。

Lab 2 自己輸出 BGRA 可以用奇數，但同一份 `VideoScaler` 在 Lab 3/4 輸出 NV12，所以一開始就統一。

### 改進空間

程式修正奇數時**不會印警告**，只有第一行輸出的 `-> 1278x720` 看得出來。好的實作應該告知使用者。

---

## 5. `VideoScaler::Process` 程式解析

**位置**：`common/video_scaler.cpp` → `VideoScaler::Process`

**一句話**：從快取取得（或第一次建立）輸入和輸出 view → 必要時更新 source rect → `VideoProcessorBlt` 排進 GPU 佇列。

### View 快取

```cpp
// common/video_scaler.h:66-67
std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;

auto& inView = inputViews_[{ src, arraySlice }];
if (!inView) { ... CreateVideoProcessorInputView(src, enum_.Get(), &d, &inView); }
```

| 情況 | `map[key]` 的行為 | `if (!inView)` |
|---|---|---|
| 第一次 | **自動插入**空的 `ComPtr`，回傳參考 | 成立 → 建立 view，直接寫進 map |
| 之後 | 回傳已存在的 view | 不成立 → 直接使用 |

- **`auto&` 一定要有 `&`**。寫成 `auto` 會複製一份，建立的 view 不會存回快取，每幀都重建。
- `std::map` 插入時舊元素位址不變，拿著參考是安全的。
- **key 包含 `arraySlice`**：Lab 5 的 texture array，同一張 texture 的不同 slice 要不同的 view。只用指標當 key，會一直讀到同一個 slice。
- **raw pointer 當 key 是安全的**：view 持有 texture 的參考，texture 不會被釋放，位址也不會被重複使用。
- **代價**：快取讓 texture 一直活著。來源 texture 會一直換的情況（例如 decoder STREAM_CHANGE 後重新配置），要 `ClearViewCache()`，否則記憶體會增加。

### 輸入 / 輸出 view 的描述

| 欄位 | 意義 |
|---|---|
| `FourCC = 0` | 使用 texture 本身的格式 |
| `ViewDimension = TEXTURE2D` | 目前唯一選項 |
| `MipSlice = 0` | 影片 texture 只有一層 mipmap |
| `ArraySlice` | texture array 的第幾張；Lab 5 傳 decoder 給的 subresource index |
| 第二個參數 `enum_` | view 和 enumerator 綁定 |

輸出 view 的 key 只有指標（我們的輸出都不是 array），且 `dst` 必須帶 `RENDER_TARGET`。各 Lab 的輸出：Lab 2 是 `scaled`（1 個 view），Lab 4/6 是 pool 的 8 張，Lab 5 是 swap chain back buffer。

### 可選的 source rect

```cpp
if (srcRect) videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, srcRect);
```

這是**持久狀態**：這幀傳了、下一幀傳 `nullptr`，舊的 rect 會留著。目前所有呼叫端都傳 `nullptr`，所以沒問題。要做裁切功能時，比較安全的寫法：

```cpp
RECT full = { 0, 0, (LONG)cfg_.srcWidth, (LONG)cfg_.srcHeight };
videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, srcRect ? srcRect : &full);
```

### Blt

```cpp
D3D11_VIDEO_PROCESSOR_STREAM stream{};     // {} 讓不用的欄位都是 0，很重要
stream.Enable = TRUE;
stream.pInputSurface = inView.Get();
CHECK_HR(videoContext_->VideoProcessorBlt(vp_.Get(), outView.Get(), 0, 1, &stream));
//                                        processor  寫到哪裡  OutputFrame  StreamCount  stream 陣列
```

| `STREAM` 欄位 | 意義 |
|---|---|
| `Enable`、`pInputSurface` | 這個 stream 參與合成、輸入畫面 |
| `OutputIndex`、`InputFrameOrField` | 幀率轉換、交錯掃描用 |
| `PastFrames`、`FutureFrames`… | 進階 deinterlace 參考前後幀 |
| `pInputSurfaceRight` | 3D 立體的右眼 |

- **`VideoProcessorBlt` 回傳 HRESULT**，`Init` 的 setter 錯誤通常在這裡才顯現。
- **它是非同步的**：`S_OK` 只代表排進佇列，不代表縮放完成。

### 流程

```
第 1 幀：  查快取 → 沒有 → CreateInputView / CreateOutputView → Blt   ← 比較慢（run 0）
第 2 幀起：查快取 → 有 → 直接用 → Blt                              ← 只剩 Blt
```

### 檢視重點

| 寫法 | 問題 |
|---|---|
| 每幀建立 view | 浪費時間配置 driver 物件 |
| `auto inView = ...` 沒有 `&` | 看起來有快取，實際上每幀重建 |
| key 只用指標 | Lab 5 一直讀到同一個 slice |
| 來源一直換但從不清快取 | 舊 texture 被卡住，記憶體增加 |
| `STREAM` 沒用 `{}` 初始化 | 隨機值可能被當成 deinterlace 參考幀 |
| 把 `S_OK` 當成縮放完成 | 計時和同步都會錯 |

---

## 6. `WaitForGpu`：為什麼要等、怎麼等

**位置**：`common/d3d_util.cpp:203` → `WaitForGpu`；`lab2_scale/main.cpp`

**一句話**：讓 CPU 停住，直到 GPU 把之前排進佇列的指令全部做完。原理是在指令流裡插一個標記，然後一直問 GPU 執行到了沒。

### 為什麼量測前要先等

`capture.Acquire()` 裡的 `CopyResource` 返回時很可能還沒做完。GPU 依序執行，如果不先清空佇列就計時：

```
佇列：  [Copy 桌面→frame] [Blt 縮放]  [標記]

不先等：  t0 ├── Copy（不該算）──┼── Blt ──┤ t2     → run 0 被灌水
先等：    Copy 已完成   t0 ├── Blt ──┤ t2          → 只量到縮放
```

Lab 2 用了兩次：

```cpp
WaitForGpu(...);                                  // (A) 量測前：清空佇列，讓起點乾淨
for (int run = 0; run < 5; ++run) {
    int64_t t0 = QpcNow();
    scaler.Process(frame.Get(), 0, scaled.Get()); // 只是排進佇列
    int64_t t1 = QpcNow();                        // t1 - t0 = submit
    WaitForGpu(...);                              // (B) 等 Blt 做完，讓終點正確
    int64_t t2 = QpcNow();                        // t2 - t0 = submit + GPU done
}
```

少了 (B)，量到 0.02ms 會以為縮放幾乎不花時間；少了 (A)，run 0 包含 Copy 的時間。

### 實作

```cpp
D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
device->CreateQuery(&qd, &q);                                         // ① 事件查詢
ctx->End(q.Get());                                                    // ② 插入標記
while (ctx->GetData(q.Get(), &done, sizeof(done), 0) == S_FALSE)      // ③ 一直問
    YieldProcessor();
```

| 步驟 | 說明 |
|---|---|
| ① `D3D11_QUERY_EVENT` | 只回答「這個標記之前的指令都做完了嗎」 |
| ② `End` | 名稱有點誤導：意思是「在指令流的這個位置放標記」。標記也是排進佇列，依序執行，**標記完成代表前面的都完成了** |
| ③ `GetData` | `S_FALSE` = 還沒，`S_OK` = 完成 |
| 第四個參數 `0` | **關鍵**：代表沒做完就順便 flush。傳 `DONOTFLUSH` 的話，標記可能一直留在 CPU 端的緩衝區、從未送到 GPU，迴圈永遠不會結束 |
| `YieldProcessor()` | CPU 的 `pause` 指令。仍是忙碌等待，會占滿一個核心 |

### 其他「等 GPU」方式

| 方式 | 會等嗎 | 說明 |
|---|---|---|
| `ctx->Flush()` | ✘ | 只送出指令，**不等做完**（常見誤解） |
| `D3D11_QUERY_EVENT` | ✔ | 我們的作法 |
| `Map` STAGING | ✔ | 隱含等待（Lab 1 筆記第 4 節） |
| `ID3D11Fence`（D3D11.3） | ✔ | 可以用 event 睡著等，適合正式產品 |
| `D3D11_QUERY_TIMESTAMP` | 量測用 | 量**純 GPU 執行時間**，比較精確 |

Lab 2 的 `submit + GPU done` 是「提交 + 傳遞 + GPU 執行」的總時間，比純 GPU 時間稍長。說明非同步夠用；精確分析要用 timestamp query 或 GPUView、PIX。

### 不能放在每幀路徑上

```
不等待：  CPU: [幀1][幀2][幀3][幀4]
          GPU:    [幀1][幀2][幀3][幀4]    ← 重疊，產能高
每幀都等：CPU: [幀1]......[幀2]......
          GPU:    [幀1]      [幀2]        ← 輪流閒置，產能減半
```

只用在量測和除錯。Lab 4/6 的每幀迴圈都沒有呼叫它。

---

## 7. `EnablePerMonitorDpiAwareness`：DPI 設定

**位置**：`common/mf_util.h` → `EnablePerMonitorDpiAwareness()`；每個 Lab 的 `main()` 開頭都會呼叫

**一句話**：告訴 Windows「我自己處理每個螢幕的 DPI 縮放，不要幫我縮放，也不要對我謊報座標，全部給我實體像素」。

### 不宣告會怎樣：DPI 虛擬化

以 1920×1080、縮放 125% 為例：

| | 沒宣告（DPI Unaware） | 宣告 Per-Monitor V2 |
|---|---|---|
| `GetMonitorInfo` / `DesktopCoordinates` 回報的大小 | 可能是 **1536×864** | **1920×1080** |
| DDA 拿到的 texture 大小 | **1920×1080**（一定是實體像素） | 1920×1080 |
| 開出來的視窗 | 被系統拉伸，模糊 | 清楚 |
| 滑鼠座標、`GetClientRect` | 縮放過的座標 | 實體座標 |

**DDA 的 texture 一定是實體像素，但沒宣告時 API 回報的座標可能是縮放過的**，兩邊對不起來：`--list` 的解析度和實際 frame 大小不同、游標位置偏移、Lab 5 的視窗被拉伸兩次而模糊。另外 `IDXGIOutput5::DuplicateOutput1` 的文件要求呼叫的 process 必須是 per-monitor DPI aware。

### 程式

```cpp
inline void EnablePerMonitorDpiAwareness()
{
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
        if (auto fn = (Fn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
            fn((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
}
```

| 寫法 | 原因 |
|---|---|
| `GetModuleHandle`（不是 `LoadLibrary`） | user32.dll 一定已經載入 |
| **動態查找** `GetProcAddress` | 這個 API 是 Win10 1703 才有。直接呼叫的話，舊系統上程式**整個無法啟動**；舊版 mingw 或 SDK 也可能沒有宣告 |
| `(void*)` 中轉 | 避免 `FARPROC` 轉型的編譯警告 |
| `(HANDLE)-4` | SDK 裡 `DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` 的定義值，寫數字以免依賴新標頭 |

| 值 | 模式 |
|---|---|
| -1 | Unaware：一律當 96 DPI，系統拉伸 |
| -2 | System Aware：只認開機時主螢幕的 DPI |
| -3 | Per-Monitor V1：標題列等非客戶區不會自動縮放 |
| **-4** | **Per-Monitor V2**：最完整，還會收到 `WM_DPICHANGED` |
| -5 | Unaware GDI Scaled |

### 注意事項

1. **必須在建立任何視窗之前呼叫**，整個 process 只能設一次。
2. 回傳值被忽略：已經用 manifest 設過時會失敗（`ERROR_ACCESS_DENIED`），但結果一樣是 DPI aware。
3. 正式產品通常寫在 application manifest：`<dpiAwareness>PerMonitorV2</dpiAwareness>`。Lab 用程式碼，是為了 mingw 和 MSVC 共用。
4. 驗證：工作管理員 →「詳細資料」→ 欄位標題右鍵 → 勾選「DPI 感知」。或把呼叫註解掉，在 125%/150% 縮放下比較 `--list` 和 frame 大小。

---

## 8. 如何測試 Lab 2

### (1) 基本執行

```powershell
.\dda_scale.exe
```

**先檢查第一行的來源尺寸**：應該是螢幕的實體解析度，不是縮放後的數字（第 7 節）。

### (2) 幾何測試：和手算的 content rect 比對

| 指令 | 測什麼 | 預期（來源 1920×1080） |
|---|---|---|
| `--width 1280 --height 720` | 同比例，沒有黑邊 | `(0,0)-(1280,720)` |
| `--width 1024 --height 1024` | 上下黑邊 | `(0,224)-(1024,800)` |
| `--width 720 --height 1280` | 直式，上下大黑邊 | `(0,438)-(720,842)` |
| `--width 1024 --height 1024 --mode stretch` | 拉伸 | `(0,0)-(1024,1024)` |
| `--width 3840 --height 2160` | 放大 | `(0,0)-(3840,2160)` |
| `--width 1366 --height 768` | 幾乎 16:9 | `(0,0)-(1364,768)`，右邊 2px |
| `--width 1279 --height 721` | 奇數 | 修正成 1278×720 |

```powershell
.\dda_scale.exe --width 1024 --height 1024 --out sq_letterbox.bmp
.\dda_scale.exe --width 1024 --height 1024 --mode stretch --out sq_stretch.bmp
.\dda_scale.exe --width 720 --height 1280 --out portrait.bmp
.\dda_scale.exe --width 3840 --height 2160 --out upscale.bmp
.\dda_scale.exe --width 1366 --height 768 --out odd_ratio.bmp
```

### (3) 目視檢查 BMP（小畫家）

| 項目 | 怎麼看 | 錯誤的樣子 |
|---|---|---|
| 長寬比 | 擷取有圓形的畫面 | letterbox 下圓變橢圓 |
| 黑邊 | 色彩選擇工具點黑邊 | 不是 `(0,0,0)`、有雜訊或殘影 |
| 黑邊位置 | 左下角座標，移到交界處 | 交界不在 content rect 的 top/bottom |
| 顏色 | 和原桌面比較 | 偏色（BGRA→BGRA 不應該有色差） |
| 清晰度 | 看小字 | 只看到某一角、明顯鋸齒 |
| stretch 對照 | 兩張並排 | stretch 被壓扁、letterbox 有黑邊不變形 |

`odd_ratio.bmp`：游標移到最右邊，x = 1364、1365 應該是純黑，1363 是畫面內容。

### (4) 計時

```
run 0: submit 0.412 ms | submit + GPU done 1.873 ms   ← 包含建立 view
run 1: submit 0.021 ms | submit + GPU done 0.402 ms   ← submit 只是排進佇列
```

確認：`submit` 遠小於 `GPU done`（非同步）；run 0 比較慢（view 快取）；輸出越大 GPU 時間越長（比較 3840×2160 和 1280×720）。

### (5) 其他情境

| 情境 | 預期 |
|---|---|
| 多螢幕 `--output 1`（先用 `dda_capture --list` 看編號） | 擷取第二個螢幕 |
| 執行後馬上 `Win+L` | 回 `No desktop frame captured` 或 ACCESS_LOST 後重試，**不應該 crash** |
| 顯示縮放改成 150% | 來源尺寸不變，仍是實體解析度 |

### (6) 動手弄壞它

| 改法 | 位置 | 預期 |
|---|---|---|
| `scaled` 的 `RENDER_TARGET` 改成 `SHADER_RESOURCE` | `lab2_scale/main.cpp:40` | `CreateVideoProcessorOutputView` 回 `E_INVALIDARG` |
| 刪掉 `VideoProcessorSetOutputBackgroundColor` | `common/video_scaler.cpp` | `1024x1024` 黑邊可能有殘影（依 driver） |
| `ComputeDestRect` 直接回傳 `{0,0,srcW,srcH}` | `common/video_scaler.cpp:35` | 只看到左上角或被裁掉 |
| 拿掉迴圈裡的 `WaitForGpu` | `lab2_scale/main.cpp` | `GPU done` 和 `submit` 幾乎一樣 |

### (7) 檢視學員的 Lab 2

| 要求 | 怎麼驗證 |
|---|---|
| 印出 content rect | 和第 (2) 項的表格比對 |
| 支援 letterbox 和 stretch | `1024x1024` 兩種模式 |
| 黑邊純黑 | 色彩選擇工具 |
| 計時有等 GPU | 有 query 或其他同步 |
| view 有快取 | `Create...View` 不在每幀迴圈裡 |
| 輸出尺寸是偶數 | 用奇數尺寸測試 |

---

## 9. 自我檢查題

1. Texture 和 view 的差別是什麼？為什麼 GPU 管線不直接使用 texture？
2. SRV 和 RTV 各代表什麼？video processor 的 input / output view 分別對應哪一個？
3. 哪些操作完全不需要 view？
4. Bind flag 可以在 texture 建立後修改嗎？driver 為什麼需要事先知道？
5. 為什麼 `scaled` 一定要有 `D3D11_BIND_RENDER_TARGET`？為什麼 `frame` 的 `RENDER_TARGET` 算是保守寫法？
6. STAGING texture 的 bind flags 為什麼必須是 0？DYNAMIC 可以當 render target 嗎？
7. `QueryInterface` 取得 `ID3D11VideoDevice` 回傳 `E_NOINTERFACE`，最可能是什麼原因？
8. `D3D11_VIDEO_PROCESSOR_CONTENT_DESC` 會決定縮放結果嗎？真正決定縮放的是什麼？
9. Source rect、dest rect、target rect 各管什麼？黑邊是怎麼來的？
10. 為什麼要關掉 `AutoProcessingMode`？不關的話哪個 Lab 會出問題？
11. `VideoProcessorSet...` 系列設定錯誤時，會在哪裡發現？
12. 為什麼 `Init` 一開始要 `ClearViewCache()`？
13. 1920×1080 縮放到 1366×768，為什麼黑邊在右邊 2px，而不是左右各 1px？
14. `x & ~1u` 做了什麼？為什麼 NV12 的尺寸和座標都要是偶數？
15. view 快取寫成 `auto inView = inputViews_[...]`（沒有 `&`）會怎樣？
16. 為什麼 input view 快取的 key 要包含 `arraySlice`？
17. 用 raw pointer 當快取的 key，為什麼在這裡是安全的？代價是什麼？
18. `Process()` 某幀傳了 `srcRect`，下一幀傳 `nullptr`，會用哪個 source rect？
19. `VideoProcessorBlt` 回傳 `S_OK`，代表縮放已經完成了嗎？
20. 量測縮放時間前，為什麼要先呼叫一次 `WaitForGpu`？
21. `ctx->End(query)` 的意思是什麼？`GetData` 的 flags 傳 `DONOTFLUSH` 會怎樣？
22. `Flush()` 和 `WaitForGpu` 有什麼不同？
23. 你量到縮放只要 0.02 ms，為什麼可能是錯的？
24. 不宣告 DPI awareness 時，在 125% 縮放的螢幕上會出現什麼不一致？
25. 為什麼 `SetProcessDpiAwarenessContext` 要用 `GetProcAddress` 動態呼叫？

<details>
<summary>參考答案（先自己想過再展開）</summary>

1. Texture 是原始資料（大小、格式、Usage、bind flags）；view 描述用途、範圍（subresource）、解讀格式。view 建立時一次驗證完，之後每幀不必再檢查；同一張 texture 也能有多種看法。
2. SRV = 給 shader 讀（貼圖）；RTV = 繪圖輸出寫入（畫布）。VP input view 像 SRV，VP output view 像 RTV。
3. `CopyResource`、`CopySubresourceRegion`、`UpdateSubresource`、`Map`/`Unmap`。
4. 不行。driver 依用途決定記憶體排列（tiling）、是否壓縮，runtime 也用它驗證 view。
5. `CreateVideoProcessorOutputView` 要求輸出有 `RENDER_TARGET`。`frame` 只當 VP 輸入，Lab 2 不需要 RT；保留是為了之後畫游標或 Clear，以及和 DDA texture 的 flag 一致，減少相容性問題。
6. STAGING 只能被 Copy 和 Map，不能綁到管線任何階段。DYNAMIC 不能當 render target（GPU 不能寫）。
7. 建立 device 時沒有帶 `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`。
8. 不會，它只是讓 driver 選硬體路徑的提示。真正決定的是 source rect 和 dest rect 的大小比例。
9. source = 讀輸入的哪塊；dest = 放到輸出的哪裡；target = 允許寫入的範圍。target 內、dest 外的區域填背景色，就是黑邊。
10. driver 可能自動降噪、銳化、色彩增強。Lab 3 的色塊數值量測會偏離公式，不同 GPU 的結果也不一致。
11. setter 回傳 `void`，要到 `VideoProcessorBlt` 才會失敗，或得到錯誤畫面；用 debug layer 看訊息。
12. view 和 enumerator 綁定，重建 enumerator 後舊 view 不能用。
13. 高度倍率較小，寬度只有 1365.33 → 1365 → 對齊偶數 1364。置中的 x = 1 也被對齊成 0，所以 2px 都在右邊。
14. 清掉最低位，無條件捨去到偶數。NV12 每 2×2 像素共用一組 UV：奇數尺寸會需要半個色度樣本；奇數座標會讓一組 UV 同時涵蓋黑邊和畫面，造成顏色滲漏。D3D11 的 NV12 texture、H.264 4:2:0 也都要求偶數。
15. 複製出一份 `ComPtr`，建立的 view 不會存回 map，每幀都重建，等於沒有快取。
16. Lab 5 的 decoder 輸出是 texture array，同一張 texture 的不同 slice 需要不同的 view；只用指標會一直讀同一個 slice。
17. view 持有 texture 的參考，texture 不會被釋放，位址不會被重複使用。代價是 texture 一直活著；來源會一直換時要清快取，否則記憶體增加。
18. 還是上一幀的 `srcRect`，因為它是 processor 的持久狀態。
19. 不是。只代表指令成功排進佇列，GPU 之後才執行。
20. Acquire 裡的 `CopyResource` 可能還在佇列中，GPU 依序執行，不先清空的話 Copy 的時間會被算進縮放時間。
21. 在指令流中插入標記，GPU 執行到時 query 變成完成。傳 `DONOTFLUSH` 的話，標記可能一直留在 CPU 端緩衝區、從未送到 GPU，迴圈永遠等不到。
22. `Flush` 只把指令送給 GPU，不等做完；`WaitForGpu` 等到標記之前的指令都完成。
23. 只量到提交時間。`VideoProcessorBlt` 是非同步的，要等 GPU 完成（event query 或 timestamp query）才是真正的成本。
24. API 回報的螢幕大小是縮放後的（例如 1536×864），DDA 的 texture 是實體像素（1920×1080）；視窗被系統拉伸而模糊，游標座標偏移。
25. 這個 API 從 Win10 1703 才有，直接呼叫會讓舊系統上的程式無法啟動；舊版 SDK / mingw 也可能沒有宣告。

</details>
