# Lab 3 複習筆記：色彩格式轉換（BGRA → NV12）

> 這份筆記整理了做 Lab 3 時提出的問題與解答，方便日後複習或帶學員時參考。
> 程式碼位置以 `WMF_Training/labs/` 為根目錄。
> 前面的筆記：[Lab 1](../lab1_capture/NOTES.md)（device/context、Map stall）、[Lab 2](../lab2_scale/NOTES.md)（texture/view、`VideoScaler`、偶數對齊）。

**實測環境**：Intel UHD Graphics 620（driver 27.20.100.8681），輸出 1280×720。`--patches` 全部通過（見第 7 節）。

---

## 目錄

1. [Lab 3 在考什麼、程式流程](#1-lab-3-在考什麼程式流程)
2. [RGB 與 YUV：概念、名稱、數值範圍](#2-rgb-與-yuv概念名稱數值範圍)
3. [色度降採樣與記憶體排列](#3-色度降採樣與記憶體排列)
4. [轉換的數學](#4-轉換的數學)
5. [GPU 端：告訴 video processor 用哪個色彩空間](#5-gpu-端告訴-video-processor-用哪個色彩空間)
6. [`--vp-*` 與 `--decode-*`：為什麼分 GPU 和 CPU 兩端](#6---vp--與---decode-為什麼分-gpu-和-cpu-兩端)
7. [`--patches`：顯示什麼、作用、實測結果](#7---patches顯示什麼作用實測結果)
8. [故意弄錯：設錯會怎樣](#8-故意弄錯設錯會怎樣)
9. [`ReadbackBgra` 與 `ReadbackNv12`](#9-readbackbgra-與-readbacknv12)
10. [為什麼 BGRA 逐行去掉 pitch，NV12 卻整塊複製](#10-為什麼-bgra-逐行去掉-pitchnv12-卻整塊複製)
11. [`Nv12ToBgra` 程式解析](#11-nv12tobgra-程式解析)
12. [存檔一定要轉 RGB 嗎？JPEG、PNG、H.264、MP4 的色彩空間](#12-存檔一定要轉-rgb-嗎jpegpngh264mp4-的色彩空間)
13. [檢視學員作業與常見錯誤速查](#13-檢視學員作業與常見錯誤速查)
14. [自我檢查題](#14-自我檢查題)

---

## 1. Lab 3 在考什麼、程式流程

**一句話**：讓 GPU 把 BGRA 轉成 NV12，再用自己手寫的公式轉回 RGB，並用純色色塊**量測**，證明兩端對色彩空間的約定一致。Lab 4 的 encoder 吃的就是這裡產生的 NV12。

真正在考的三件事：

1. **NV12 的記憶體排列**：Y 平面、UV 平面、pitch、UV 的起點
2. **色彩空間是一份約定**：係數（601 / 709）× 範圍（studio / full），兩端必須一致
3. **說得出「設錯會怎樣」**，而且用量測證明，不是相信 API 呼叫成功

### 為什麼要轉成 NV12

| | BGRA | NV12 |
|---|---|---|
| 每像素 | 4 bytes | **1.5 bytes** |
| 1080p 一張 | 8.3 MB | 3.1 MB |
| 誰要用 | 螢幕、DDA | **幾乎所有硬體 H.264 encoder 的輸入** |

### 程式流程（`lab3_nv12/main.cpp`）

```
               ┌─ toBgra（輸出 BGRA）─▶ bgraOut ─▶ capture_bgra.bmp               ← 對照組（等於 Lab 2）
DDA ─▶ frame ──┤
               └─ toNv12（輸出 NV12）─▶ nv12Out ─▶ Readback ─▶ 手寫 YUV→RGB ─▶ capture_nv12.bmp   ← 實驗組
                                                       │
                                             --patches：量測色塊的 Y/U/V
```

同一張 `frame`、兩個 scaler，只有輸出格式不同。兩張 BMP 應該看起來一樣。

---

## 2. RGB 與 YUV：概念、名稱、數值範圍

**一句話**：同一個顏色的兩套座標系。RGB 記錄紅綠藍三種光的強度；YUV 記錄亮度（Y）＋偏藍多少（U）＋偏紅多少（V）。亮度和顏色分開後，就能**只降低顏色的解析度**。

### 為什麼需要 YUV

| | RGB | YUV |
|---|---|---|
| 來源 | 螢幕的物理結構（紅綠藍發光點） | 人眼特性與電視歷史 |
| 擅長 | 顯示、繪圖、合成 | 壓縮、傳輸、影片 |
| 誰在用 | 螢幕、GPU 繪圖、Windows 桌面、DDA | H.264/HEVC/AV1、JPEG、相機 |

1. **人眼對亮度比對顏色敏感得多**：顏色可以用較低的解析度儲存。RGB 三個通道都混著亮度，做不到；YUV 把亮度獨立出來才行。
2. **黑白電視相容**：Y 就是原本的黑白訊號，U、V 是額外的色彩資訊。

| 顏色 | RGB | YUV（709 studio） | 觀察 |
|---|---|---|---|
| 黑 | 0,0,0 | 16,128,128 | |
| 白 | 255,255,255 | 235,128,128 | |
| 灰 | 128,128,128 | 126,128,128 | **灰階的 U、V 永遠是 128** |
| 紅 | 255,0,0 | 63,102,240 | V 高：偏紅 |
| 藍 | 0,0,255 | 32,240,118 | U 高：偏藍；Y 低：藍色看起來暗 |

### 名稱

| 名稱 | 意義 |
|---|---|
| YUV | 口語統稱 |
| **YCbCr** | 數位版的正式名稱。**Cb = U（藍色差），Cr = V（紅色差）** |
| Y'（有一撇） | luma：從 gamma 編碼後的 R'G'B' 算出，不是物理亮度 |
| RGBA | 通道名稱，不一定是記憶體順序 |
| **BGRA** | 記憶體順序是 B、G、R、A。Windows 桌面和 DDA 給的就是這個 |

### RGB / RGBA 的細節

- **Alpha 不是顏色**，是混合比例。YUV 沒有 alpha，**BGRA → NV12 時 alpha 被丟掉**。
- **Gamma**：RGB 數值經過 gamma 編碼，128 的物理亮度約只有 255 的 22%。`B8G8R8A8_UNORM` 原樣讀寫；`..._UNORM_SRGB` 在 shader 讀取時轉成線性。YUV 是從 gamma 編碼後的值計算的，所以轉換時直接對數值做矩陣運算。
- **記憶體**：`B8G8R8A8_UNORM` 每像素 4 bytes，順序 `B G R A`；x86 little-endian 用 `uint32_t` 讀是 `0xAARRGGBB`。

```
pData ──► ┌─────────── RowPitch（≥ width × 4）───────────┐
          │ BGRA BGRA ... BGRA (width 個像素)    [padding]│  ← 第 0 行
          │ ...                                           │  共 height 行
          └───────────────────────────────────────────────┘
像素 (x, y) = pData + y × RowPitch + x × 4
```

### YUV 的三個分量與範圍

| 分量 | 意義 | 數學範圍 |
|---|---|---|
| Y | R、G、B 的加權平均 | 0–1 |
| Cb (U) | 藍色差 | −0.5–+0.5 |
| Cr (V) | 紅色差 | −0.5–+0.5 |

沒有綠色差，因為 Y、B、R 已知就能反推 G。

**幾何直覺**：RGB 是立方體，黑到白的對角線是灰階軸。YUV 就是把立方體旋轉，讓灰階軸變成 Y 軸；U、V 量測顏色離灰階軸多遠、往哪個方向。

| 範圍 | Y | U、V | 黑 | 白 | 用途 |
|---|---|---|---|---|---|
| **Studio（limited、TV）** | 16–235 | 16–240 | 16 | 235 | **影片標準**，H.264 預設 |
| **Full（PC、JPEG）** | 0–255 | 0–255 | 0 | 255 | JPEG、部分螢幕錄影 |

Studio range 上下保留的空間，原本是為了類比訊號的過衝。

---

## 3. 色度降採樣與記憶體排列

### J:a:b 表示法

在 **J 像素寬、2 行高**的區塊中：`a` = 第一行的色度樣本數，`b` = 第二行**新增**的色度樣本數。

```
          4:4:4                  4:2:2                  4:2:0
  第1行 ● ● ● ●             ● ○ ● ○                ● ○ ● ○
  第2行 ● ● ● ●             ● ○ ● ○                ○ ○ ○ ○
  一組 UV 涵蓋  1×1                 2×1                    2×2
```

| 格式 | 每像素 | 1920×1080 | 相對 BGRA |
|---|---|---|---|
| BGRA | 4 | 8,294,400 bytes | 100% |
| YUV 4:4:4 | 3 | 6,220,800 bytes | 75% |
| YUV 4:2:2 | 2 | 4,147,200 bytes | 50% |
| **YUV 4:2:0（NV12）** | **1.5** | **3,110,400 bytes** | **37.5%** |

**代價**：彩色細字和細線會糊掉（螢幕串流產品有時因此用 4:4:4）。另外有**取樣位置（chroma siting）**的問題：那組 UV 代表 2×2 的哪個位置？left（H.264 慣例）、center（JPEG）、top-left（HEVC HDR）。兩端認定不同時，顏色邊緣偏移半個像素。

### 三種排列方式（以 4×2 的圖為例）

```
Planar（I420）：     Y Y Y Y / Y Y Y Y      U U       V V       ← 三個平面（YV12 是 V 在前）
Semi-planar（NV12）：Y Y Y Y / Y Y Y Y      U V U V             ← Y 一塊，UV 交錯一塊（NV21 是 VU）
Packed（YUY2）：     Y0 U0 Y1 V0  Y2 U1 Y3 V1 / ...              ← 全部交錯，4:2:2
```

**硬體偏好 NV12**：只有兩個平面；UV 交錯，讀一組色度只要讀一個位置；剛好能用 `R8` 和 `R8G8` 兩種 texture 格式來看。

### NV12 在 D3D11 裡

一張 `DXGI_FORMAT_NV12` texture **同時包含兩個平面**：

```
pData ──► ┌──────────── RowPitch ──────────────┐
          │ Y Y Y Y ... (width bytes)    [pad] │  ← Y 第 0 行
          │ ...                                │  共 codedHeight 行
          ├────────────────────────────────────┤ ← pData + RowPitch × codedHeight
          │ U V U V ... (width bytes)    [pad] │  ← UV 第 0 行（對應 Y 的第 0、1 行）
          │ ...                                │  共 codedHeight / 2 行
          └────────────────────────────────────┘

Y = pData[y × pitch + x]
U = uvPlane[(y/2) × pitch + (x/2)×2 + 0]
V = uvPlane[(y/2) × pitch + (x/2)×2 + 1]
```

- UV 平面每行也是 `width` bytes（width/2 組，每組 2 bytes），兩個平面 pitch 相同。
- **`codedHeight` 可能大於顯示高度**：H.264 以 16 為單位，1080 會變 1088（Lab 5）。
- shader 使用時，對同一張 texture 建兩個 SRV：`R8_UNORM` 看到 Y，`R8G8_UNORM` 看到 UV（R = U，G = V）。
- **P010**（10-bit HDR）：排列同 NV12，每個值 16 bits，10-bit 數值放在高位，大小是 NV12 的兩倍。

| 格式 | 取樣 | 排列 | 每像素 | 常見用途 |
|---|---|---|---|---|
| `B8G8R8A8` | RGB | packed | 4 | 桌面、DDA、swap chain |
| **NV12** | 4:2:0 | semi-planar | 1.5 | **Windows 硬體編解碼** |
| I420 | 4:2:0 | planar | 1.5 | 軟體 codec、ffmpeg `yuv420p` |
| NV21 | 4:2:0 | semi-planar（VU） | 1.5 | Android 相機 |
| YUY2 | 4:2:2 | packed | 2 | USB webcam |
| P010 | 4:2:0 10-bit | semi-planar | 3 | HDR |

ffmpeg 交叉比對時要加 `-pix_fmt nv12`，因為預設的 `yuv420p` 是 I420：資料量相同，只是排列不同。

---

## 4. 轉換的數學

**位置**：`common/yuv.h`

### RGB → YUV 的步驟

```
R'G'B'（0–255）
   │ ① 正規化到 0–1
   │ ② 矩陣（Kr、Kb）             ← 係數：601 或 709
   │ ③ 縮放和偏移                  ← 範圍：studio 或 full
   │ ④ 四捨五入、clamp 到 0–255
   │ ⑤ 色度降採樣（2×2 → 1 組）    ← 4:2:0 與取樣位置
   ▼
NV12
```

### ② 從定義推導，不要背係數

| 標準 | Kr | Kb | Kg | 用途 |
|---|---|---|---|---|
| BT.601 | 0.299 | 0.114 | 0.587 | SD |
| **BT.709** | **0.2126** | **0.0722** | **0.7152** | **HD（我們用這個）** |

```
E'y = Kr·R + Kg·G + Kb·B
Cb  = (B − E'y) / (2·(1 − Kb))      ← 除以這個，讓純藍時剛好是 0.5
Cr  = (R − E'y) / (2·(1 − Kr))
```

綠色權重最大（709 是 71.5%），因為人眼對綠光最敏感；所以純藍的 Y 只有 32。

### ③ 範圍映射

```
Studio：Y = 16 + 219·E'y     U = 128 + 224·Cb     V = 128 + 224·Cr
Full：  Y = 255·E'y          U = 128 + 255·Cb     V = 128 + 255·Cr
```

### 例子：純紅用 709 studio

```
E'y = 0.2126                         → Y = 16 + 219 × 0.2126      = 62.56 → 63
Cb  = −0.2126 / 1.8556 = −0.1146     → U = 128 + 224 × (−0.1146) = 102.3 → 102
Cr  =  0.7874 / 1.5748 =  0.5        → V = 128 + 224 × 0.5       = 240
```

### 合併成矩陣：709 studio，輸入 0–255

```
⎡Y⎤   ⎡ 0.1826   0.6142   0.0620⎤ ⎡R⎤   ⎡ 16⎤
⎢U⎥ = ⎢−0.1006  −0.3386   0.4392⎥ ⎢G⎥ + ⎢128⎥
⎣V⎦   ⎣ 0.4392  −0.3989  −0.0403⎦ ⎣B⎦   ⎣128⎦
```

U、V 列的係數和為 0，所以灰階（R = G = B）的 U、V 一定是 128。

### 整數定點寫法

逐像素浮點運算太慢，實務上把係數放大 256 倍。經典的 **BT.601 studio**：

```c
Y = (( 66*R + 129*G +  25*B + 128) >> 8) + 16;
U = ((-38*R -  74*G + 112*B + 128) >> 8) + 128;
V = ((112*R -  94*G -  18*B + 128) >> 8) + 128;
```

很多網路範例用這組數字，**直接拿來處理 HD 影片，就會得到係數錯誤的偏色**。

### YUV → RGB（`MakeYuvToRgbCoeffs`、`YuvToRgb`）

```
① 色度升採樣  ② 去掉偏移和縮放  ③ 反矩陣  ④ clamp（不能省）

709 studio 展開：
R = 1.164·(Y−16)                  + 1.793·(V−128)
G = 1.164·(Y−16) − 0.213·(U−128) − 0.533·(V−128)
B = 1.164·(Y−16) + 2.112·(U−128)
```

1.164 = 255/219。`yuv.h` 用 Kr、Kb 在執行時計算這些係數，`portable_tests` 驗證展開結果。

**`Clamp8` 不能省**：濾波和降採樣可能讓結果略超出 0–255，沒 clamp 的話 −3 會繞回 253，飽和色邊緣出現亮點或黑點。

### 轉換是有損的

| 損失來源 | 說明 |
|---|---|
| **色度降採樣** | 最主要。2×2 的顏色被平均，無法復原 |
| 量化 | 四捨五入。studio 只有 220 個亮度等級 |
| 色域不對稱 | YUV 的數值空間比 RGB 立方體大，有些組合轉回會超出範圍，只能 clamp |

### 由誰轉換

| 位置 | Lab 裡 |
|---|---|
| **GPU video processor**（最快） | Lab 2–5 的 `VideoScaler` |
| GPU shader | 沒用到 |
| CPU（驗證用；正式產品用 SIMD 如 libyuv） | Lab 3 的 `SaveNv12AsBmp` |
| encoder / decoder 內部 | 沒用到，自己轉才知道用了哪種色彩空間 |

### 色彩空間不只是矩陣

| 參數 | 選項 | 設錯的後果 | 這門課 |
|---|---|---|---|
| **矩陣** | 601 / 709 / 2020 | 飽和色偏，灰階正常 | 709 |
| **範圍** | studio / full | 霧霧的，或黑白截斷 | studio |
| 取樣 | 4:4:4 / 4:2:2 / 4:2:0 | 格式不符 | 4:2:0 |
| 取樣位置 | left / center / top-left | 顏色邊緣偏半個像素 | left |
| 原色 | 709 / 2020 / P3 | 整體色調偏（色域不同） | 709 |
| 傳輸函數 | gamma 2.2 / PQ / HLG | 亮度曲線錯，HDR 灰灰的 | gamma 2.2 |

**矩陣和原色是兩回事**：矩陣只是 RGB↔YUV 的座標轉換；原色定義「R、G、B 到底是哪一種紅綠藍」。

---

## 5. GPU 端：告訴 video processor 用哪個色彩空間

**位置**：`common/video_scaler.cpp` → `ApplyColorSpaces()`

```cpp
vc1->VideoProcessorSetStreamColorSpace1(vp_, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);    // 輸入 BGRA
vc1->VideoProcessorSetOutputColorSpace1(vp_, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);  // 輸出 NV12
```

```
DXGI_COLOR_SPACE_ YCBCR _ STUDIO _ G22 _ LEFT _ P709
                    │        │       │      │      └─ 原色與矩陣：BT.709
                    │        │       │      └──────── 色度取樣位置：left
                    │        │       └─────────────── 傳輸函數：gamma 2.2
                    │        └─────────────────────── 範圍：studio
                    └──────────────────────────────── 模型：YCbCr
```

- `...ColorSpace1` 在 **`ID3D11VideoContext1`** 上（勘誤 E4），所以先 `As(&vc1)`；失敗時退回舊 API（`D3D11_VIDEO_PROCESSOR_COLOR_SPACE` 的位元欄位）。
- 這些 setter **回傳 `void`**。driver 忽略設定時**不會有任何錯誤**，所以一定要量測。

---

## 6. `--vp-*` 與 `--decode-*`：為什麼分 GPU 和 CPU 兩端

**一句話**：`vp` 是「寫的人」用的格式，`dec` 是「讀的人」以為的格式。真實系統裡兩端通常是不同的程式、甚至不同的電腦。它們**可以**不同，但不同就是 bug。

| 變數 | 全名 | 誰 | 做什麼 |
|---|---|---|---|
| `vpMatrix`、`vpRange` | Video Processor | GPU | **寫入** NV12 用的色彩空間 |
| `decMatrix`、`decRange` | Decode | CPU（`SaveNv12AsBmp`） | **還原**時假設的色彩空間 |

```
BGRA ──▶ [GPU：用 vp 轉換] ──▶ NV12 的位元組 ──▶ [CPU：用 dec 還原] ──▶ RGB
                                    ↑
                     只是一堆數字，不會記錄自己是用哪種色彩空間寫的
```

輸出用字也有區別：GPU 是 **writes**，CPU 是 **assumes**。

| | 寫入端 | 讀取端 |
|---|---|---|
| Lab 3 | GPU video processor | 同一程式的 CPU 函式 |
| Lab 4 → 5 | encoder 端 | decoder + render window |
| Lab 6 | sender | receiver（另一台電腦） |
| 實際產品 | 你的串流程式 | 對方的播放器 |

Lab 3 在同一個程式裡**模擬**未來分處兩地的兩端。兩端唯一的溝通管道是 bitstream 裡的色彩空間標示（Lab 4 的 `MF_MT_YUV_MATRIX` 和 VUI）。

**真實世界的 bug**：寫入端沒有標示色彩空間，播放器只能猜（常見規則：≥ 720p 當 709，否則當 601）。所以同一個程式只是改輸出成 640×480，就可能被當成 601 而偏色。

### `MISMATCH` 提示

```cpp
(vpMatrix != decMatrix || vpRange != decRange) ? "   <-- MISMATCH (on purpose?)" : ""
```

矩陣或範圍任一不同就加提示：做實驗時提醒接下來的 `COLOR-OFF` 是預期的；不小心打錯參數時讓你發現。

### 預設值串接

```cpp
const YuvMatrix decMatrix = ParseMatrix(args.Get("--decode-matrix", args.Get("--vp-matrix", "709")));
```

查找順序：`--decode-matrix` → `--vp-matrix` → `"709"`。

| 指令 | vp | dec | MISMATCH |
|---|---|---|---|
| （無） | 709 | 709 | 否 |
| `--vp-matrix 601` | 601 | **601**（自動跟上） | 否 |
| `--decode-matrix 601` | 709 | 601 | **是** |
| `--vp-matrix 601 --decode-matrix 709` | 601 | 709 | **是** |

不刻意指定 `--decode-*` 時 CPU 自動和 GPU 一致，避免「只想測 601」時無意間製造 mismatch。

**特殊用途**：driver 忽略 ColorSpace（要求 709 卻輸出 601）時，用 `--vp-matrix 709 --decode-matrix 601`，RGB 變 OK 就證明 driver 實際用的是 601。「要求的格式」和「實際的格式」不一定相同。

---

## 7. `--patches`：顯示什麼、作用、實測結果

**一句話**：在螢幕上蓋一層**已知顏色**的測試畫面，擷取後在每個色塊中心量測 GPU 實際寫出的 Y/U/V，和公式算出的理論值比對。把「看起來好像對」變成「用數字證明對」。

### 為什麼需要

桌面畫面沒有標準答案，而**係數錯誤只讓飽和色偏一點、灰階完全不變**，肉眼幾乎發現不了。

### 測試畫面（`common/pattern_window.cpp`）

```
┌────────┬────────┬────────┬────────┬────────┬────────┐
│  紅    │  綠    │  藍    │  白    │  黑    │  灰    │
│   ●    │   ●    │   ●    │   ●    │   ●    │   ●    │  ← 量測點：每條正中央
└────────┴────────┴────────┴────────┴────────┴────────┘
```

| 設定 | 作用 |
|---|---|
| `WS_EX_TOPMOST` | 永遠在最上層 |
| `WS_POPUP` | 沒有標題列和邊框 |
| `WS_EX_TOOLWINDOW` | 不出現在工作列 |
| `PumpFor(800)` | 讓 `WM_PAINT` 執行、DWM 合成，DDA 才擷取得到 |

| 色塊 | 偵測 | 原因 |
|---|---|---|
| **紅、綠、藍** | 係數錯誤 | 只有飽和色受係數影響 |
| **白、黑** | 範圍錯誤 | 範圍的兩端（studio 235/16，full 255/0） |
| **灰** | 中間值、色度中性 | U、V 必須剛好 128 |

只取**中心**，避開邊緣的縮放濾波和色度降採樣混色。

### 量測點座標換算

```cpp
RECT dr = toNv12.DestRect();     // Lab 2 的 content rect，考慮 letterbox
UINT x = dr.left + c.x * (dr.right - dr.left) / capture.Width();
UINT y = dr.top  + c.y * (dr.bottom - dr.top) / capture.Height();
```

沒有這一步，有黑邊時量測點會落在黑邊上。

### 表格的每一欄

| 欄位 | 內容 | 用哪組設定 |
|---|---|---|
| **YUV measured** | GPU 實際寫出的值（`SampleNv12`） | 無，量測結果 |
| **YUV expected** | 原始 RGB 用公式算出的理論值（`RgbToYuv`） | `--vp-*` |
| **RGB after decode** | measured 用 CPU 公式還原（`YuvToRgb`） | `--decode-*` |

| `result` | 問題出在哪 |
|---|---|
| `OK` | 沒有問題 |
| `GPU-YUV-OFF` | **GPU 端**：driver 沒照要求的色彩空間輸出 |
| `COLOR-OFF` | **CPU 端的假設**和 GPU 實際輸出不一致 |

這兩個檢查讓你**分辨是哪一端錯了**。容許值 YUV ±4（driver 捨入、濾波差異）、RGB ±6（再加上量化損失）。全部通過回傳 0，失敗回傳 5，可放進自動化腳本。

### 使用注意

不要動滑鼠（游標可能蓋住量測點）；通知視窗可能蓋住色塊；**Windows 開啟 HDR 時**桌面格式是 FP16、SDR 內容被重新映射，量到的值會不同，測試時請關閉 HDR。

### 實測結果

```
GPU  (VideoProcessor) writes NV12 as : BT.709 studio(16-235)
CPU  (SaveNV12AsBmp) assumes         : BT.709 studio(16-235)
NV12 readback: 1280x720, RowPitch=1280 (width=1280)  -> UV plane offset = RowPitch*Height = 921600 bytes

patch  | YUV measured    | YUV expected    | RGB after decode | result
-------+-----------------+-----------------+------------------+-------
red    |  63 102 240     |  63 102 240     | 255   1   0      | OK
green  | 173  42  26     | 173  42  26     |   0 255   1      | OK
blue   |  32 240 118     |  32 240 118     |   1   0 255      | OK
white  | 235 128 128     | 235 128 128     | 255 255 255      | OK
black  |  16 128 128     |  16 128 128     |   0   0   0      | OK
gray   | 126 128 128     | 126 128 128     | 128 128 128      | OK
```

| 觀察 | 代表什麼 |
|---|---|
| measured 和 expected 完全一致 | **Intel driver 有照要求輸出 709 studio**，沒有 `GPU-YUV-OFF` |
| 黑 16、白 235 | 確實是 studio range |
| 灰階 U、V 都是 128 | 沒有顏色 |
| 紅色 Y = 63（不是 81~82） | 用的是 709 係數，不是 601 |

**為什麼紅色還原是 `255, 1, 0`**：量化損失。Y 的真實值 62.56 被存成 63，還原時：

```
E'y = (63 − 16) × 255/219    =  54.73
Cb  = (102 − 128) × 255/224  = −29.60
Cr  = (240 − 128) × 255/224  = 127.50
R = 54.73 + 1.5748 × 127.50                   = 255.5 → 255
G = 54.73 + 0.1873 × 29.60 − 0.4681 × 127.50  =   0.59 → 1   ← 這個 1
B = 54.73 − 1.8556 × 29.60                    =  −0.20 → 0
```

就算兩端設定完全正確，RGB → YUV → RGB 也不會完全還原，這就是 RGB 容許值 ±6 的原因。

**這次沒測到的**：`RowPitch = 1280` 剛好等於寬度，pitch 用錯也看不出來。要用 `--width 1366 --height 768` 讓 driver 產生 padding。不過 pitch 的處理已經由 `portable_tests` 的 `TestNv12Layout` 驗證過（第 11 節）。

---

## 8. 故意弄錯：設錯會怎樣

> 下表是用 `yuv.h` 公式算出的**理論值**，實機尚未執行。

```powershell
.\dda_nv12.exe --patches --decode-range full
.\dda_nv12.exe --patches --decode-matrix 601
.\dda_nv12.exe --patches --vp-matrix 601 --decode-matrix 709
```

| 實驗 | YUV measured | 黑 | 白 | 紅 | 綠 | 看起來 |
|---|---|---|---|---|---|---|
| 正確（709 studio） | 同第 7 節 | 0,0,0 | 255,255,255 | 255,0,0 | 0,255,0 | 正常 |
| `--decode-range full` | **不變** | **16,16,16** | **235,235,235** | **239,15,15** | **12,237,13** | 霧霧的、黑不夠黑、白不夠白 |
| `--decode-matrix 601` | **不變** | 0,0,0 | 255,255,255 | **233,0,2** | **20,255,9** | 灰階正確，飽和色稍偏 |
| GPU 601、CPU 709 | **改變**：紅 ≈ 81~82, 90, 240 | | | **255,24,0** | **0,216,0** | 紅偏橘、綠偏暗 |

前兩個實驗只改 CPU 的假設，**YUV measured 完全不變**；RGB 錯就代表問題在讀取端。

### 三個結論（學員報告「設錯會怎樣」的標準答案）

1. **範圍錯了，一眼看得出來**，整張對比跑掉。
2. **係數錯了，灰階完全看不出來**（灰色 Cb = Cr = 0，不受係數影響），飽和色只偏 15–40。**一定要用純色色塊量測**。
3. **兩端錯得一致時，自己的系統看起來完全正常**，但別人的播放器會偏色。所以色彩空間必須寫進 bitstream（Lab 4 的 `MF_MT_YUV_MATRIX`、`MF_MT_VIDEO_NOMINAL_RANGE`）。

### 改壞 `yuv.h`

| 改法 | 會看到 | 原因 |
|---|---|---|
| `uv[0]` 和 `uv[1]` 對調 | 紅藍互換 | U 是藍色差、V 是紅色差 |
| `(x / 2) * 2` 改成 `x` | 右半邊顏色跑掉 | 讀到錯的 UV 組，U、V 錯位 |
| 刪掉 `Clamp8` 的 min/max | 飽和色邊緣有亮點黑點 | 溢位繞回 |
| `UVPlane()` 改成 `width * height` | pitch ≠ width 時偏綠或條紋 | UV 起點錯，讀到偏低的值 |

**為什麼錯的 UV 會偏綠**：U、V 遠低於 128 時，Cb、Cr 是很大的負值，R、B 被大幅扣減，G 因為 `− gu·cb − gv·cr` 反而增加。

---

## 9. `ReadbackBgra` 與 `ReadbackNv12`

**位置**：`common/d3d_util.cpp`

**一句話**：都是把 GPU texture 讀回 CPU：複製到 staging → Map → 複製出來 → Unmap。差別在 NV12 有兩個平面，而且可能來自 decoder 的 texture array。

```
tex（DEFAULT，CPU 不能讀）
   │ ① Copy（GPU 內部，非同步）
   ▼
staging（STAGING + CPU_ACCESS_READ，bind = 0）
   │ ② Map：CPU 等 GPU 做完 ①（Lab 1 筆記第 4 節的 stall）
   │ ③ 複製到 std::vector
   │ ④ Unmap：之後 pData 不能再用
   ▼
CPU 記憶體
```

**為什麼不能直接 Map 原本的 texture**：DEFAULT texture 放在 GPU 最快的記憶體，排列（tiling）是為 GPU 最佳化的，CPU 讀不了。staging 是兩邊都看得懂的中轉站。

### `ReadbackNv12` 的關鍵：`CopySubresourceRegion`

```cpp
ctx->CopySubresourceRegion(staging.Get(), 0,     // 目的：第 0 個 subresource
                           0, 0, 0,              // 放在 (0,0,0)
                           tex, subresource,     // 來源：第 subresource 個
                           nullptr);             // 整個 subresource
```

```
decoder 的 tex（ArraySize = 10）            staging（ArraySize = 1）
│ slice 0 │
│ slice 1 │
│ slice 2 │ ── 只複製 subresource 2 ──▶ │ slice 0 │
│  ...    │
```

- `CopyResource` 要求兩邊**結構完全相同**（包括 array 大小），不能把 10 張的 array 複製到 1 張。
- subresource index = `MipSlice + ArraySlice × MipLevels`，我們只有一層 mipmap，所以就是 slice 編號。
- 程式註解說「帶 BIND_DECODER 不能直接 Map」，對但不完整：**任何 DEFAULT texture 都不能 Map**。用 `CopySubresourceRegion` 的真正原因是**從 array 中挑一張**。

### `× 3 / 2` 與 `codedHeight`

```
Y 平面 Height 行 + UV 平面 Height/2 行，每行 RowPitch → RowPitch × Height × 3/2
```

D3D11 Map NV12 時，UV 平面緊接在 Y 平面之後，pitch 相同，所以能一次整塊複製。`codedWidth` / `codedHeight` 記錄 **texture 實際大小**（decoder 是 1920×1088），UV 起點必須用它計算。

### 對照

| | `ReadbackBgra` | `ReadbackNv12` |
|---|---|---|
| 支援來源 | 單張 texture | 單張或 **array 的某一張** |
| 複製指令 | `CopyResource` | `CopySubresourceRegion` |
| 格式檢查 | **沒有**（假設 4 bytes） | 有 |
| 輸出排列 | 緊密排列 | 保留 pitch 與兩個平面的原始位置 |
| 用在 | Lab 1、2 存 BMP；Lab 3 對照組 | Lab 3 量測；Lab 4 軟體 fallback；Lab 5 除錯 |

### 除錯用途可以接受、正式產品要改的地方

| 問題 | 正式產品的作法 |
|---|---|
| 每次新建 staging | 建一次，重複使用 |
| Map 會 stall | 多張 staging 輪流 + `D3D11_MAP_FLAG_DO_NOT_WAIT` |
| `ReadbackBgra` 不檢查格式：開 HDR 時桌面可能是 FP16（8 bytes） | 檢查 `d.Format` |
| `ReadbackBgra` 的 `CopyResource` 不支援 array；它回傳 `void`，失敗只會在 debug layer 留訊息 | 改用 `CopySubresourceRegion` |
| Map 之後才配置 vector，`bad_alloc` 時不會 Unmap | 先配置再 Map |

---

## 10. 為什麼 BGRA 逐行去掉 pitch，NV12 卻整塊複製

**一句話**：NV12 **也有**處理 RowPitch，只是把「去掉 padding」**延後**給使用者。

```cpp
img.pitch = m.RowPitch;                                   // ① 記下 pitch
size_t bytes = (size_t)m.RowPitch * d.Height * 3 / 2;     // ② 用 pitch 計算大小
img.data.assign(pData, pData + bytes);                    // ③ 連同 padding 整塊複製
```

之後所有讀取都用 `img.pitch`：`UVPlane()` 是 `pitch * codedHeight`，`SampleNv12` 和 `Nv12ToBgra` 用 `y * pitch`。

```
ReadbackBgra：Map（有 pad）──逐行複製──▶ 緊密排列 ──▶ 使用者不用知道 pitch
ReadbackNv12：Map（有 pad）──整塊複製──▶ 原樣 + pitch ──▶ 使用者一定要用 pitch
```

| | 讀回時去掉（BGRA） | 保留（NV12） |
|---|---|---|
| 讀回時 | 逐行 memcpy | 一次整塊 |
| 使用時 | `y × width` | `y × pitch`，**每個使用者都要記得** |
| 適合 | 交給不認識 pitch 的東西（BMP） | 保持和 GPU 相同的排列 |

兩種都正確，關鍵是**誰負責處理 pitch 要明確**。最容易出錯的是：讀回端保留了 padding，使用端卻以為是緊密排列。

### NV12 選擇保留的原因

1. **兩個平面的相對位置**：保留原樣，UV 就在 `pitch × codedHeight`，和 GPU 一模一樣，不會和 decoder 的 1088 搞混。
2. **Lab 3 要測的就是 pitch 處理**：讀回時就去掉的話，`SampleNv12`、`Nv12ToBgra` 永遠不會遇到 pitch ≠ width。
3. BGRA 的去向（BMP）本來就要求緊密排列。

### NV12 也要緊密排列時（例如存 raw `.nv12` 檔）

```cpp
for (UINT y = 0; y < h; ++y)                                    // Y：h 行
    memcpy(&packed[(size_t)y * w], src + (size_t)y * m.RowPitch, w);
const uint8_t* srcUV = src + (size_t)m.RowPitch * d.Height;     // 來源 UV：texture 的高度和 pitch
uint8_t* dstUV = packed.data() + (size_t)w * h;                 // 目的 UV：w × h
for (UINT y = 0; y < h / 2; ++y)
    memcpy(dstUV + (size_t)y * w, srcUV + (size_t)y * m.RowPitch, w);
```

來源和目的的 UV 起點**用不同公式**（`RowPitch × 1088` 對 `w × 1080`），混用就是偏綠的主因。

---

## 11. `Nv12ToBgra` 程式解析

**位置**：`common/yuv.h` → `Nv12ToBgra`

**一句話**：每個像素讀自己的 Y，再讀所屬 2×2 區塊共用的 UV，套用反轉換，寫出 B、G、R、A。輸入保留 pitch，輸出緊密排列。

### 介面設計

```cpp
inline void Nv12ToBgra(const uint8_t* yPlane, const uint8_t* uvPlane, size_t pitch,
                       uint32_t width, uint32_t height, YuvMatrix m, YuvRange range, uint8_t* bgraOut);
```

傳指標和 pitch、不傳 `Nv12Image`：

1. **和 D3D 無關**，能在 Linux 上用 `portable_tests` 測試。
2. **任何來源都能用**：`SaveNv12AsBmp`（D3D 讀回）、`lab5_decode/main.cpp:107`（decoder 的 CPU 記憶體）、`portable_tests`（手工資料）。
3. **UV 位置由呼叫端算**：函式只知道顯示的 `height`（1080），不知道 texture 的 `codedHeight`（1088）。

### 逐行

```cpp
const YuvToRgbCoeffs k = MakeYuvToRgbCoeffs(m, range);          // 迴圈外算一次
for (uint32_t y = 0; y < height; ++y) {                         // height = 顯示高度
    const uint8_t* yRow  = yPlane  + y * pitch;                 // 輸入：用 pitch
    const uint8_t* uvRow = uvPlane + (y / 2) * pitch;           // 兩行 Y 共用一行 UV
    uint8_t* out = bgraOut + (size_t)y * width * 4;             // 輸出：緊密排列
    for (uint32_t x = 0; x < width; ++x) {
        const uint8_t* uv = uvRow + (x / 2) * 2;                // 一組 UV 涵蓋 2×2
        Rgb c = YuvToRgb(yRow[x], uv[0], uv[1], k);             // U 在前、V 在後
        out[x * 4 + 0] = c.b;  out[x * 4 + 1] = c.g;            // BGRA 順序
        out[x * 4 + 2] = c.r;  out[x * 4 + 3] = 255;            // alpha 補不透明
    }
}
```

| `y` | `y / 2` | | `x` | `(x/2) × 2` | 讀到 |
|---|---|---|---|---|---|
| 0、1 | UV 第 0 行 | | 0、1 | 0 | `U0 V0` |
| 2、3 | UV 第 1 行 | | 2、3 | 2 | `U1 V1` |
| 4、5 | UV 第 2 行 | | 4、5 | 4 | `U2 V2` |

- `x / 2` = 第幾組 UV；`× 2` = 每組 2 bytes。寫成 `uvRow + x` 會 U、V 錯位，越右邊錯越遠。
- **紅藍互換的兩個可能原因**：U、V 讀反，或 RGB 寫反。兩處都要檢查。
- 迴圈只跑顯示高度，decoder 的 1088 中最後 8 行會被跳過。

### 色度升採樣：最近鄰

2×2 的四個像素**直接用同一組 UV**。GPU video processor 則用雙線性內插並考慮取樣位置，所以 `capture_nv12.bmp` 和 `capture_bgra.bmp` 在**顏色邊緣**可能有細微差異，這不是色彩空間錯誤。`--patches` 只量中心就是為了避開這點。

### 效能

逐像素浮點運算加函式呼叫，以清楚為優先，存一張圖足夠。每幀轉換時的最佳化：UV 部分每組只算一次（四個像素共用）、整數定點、查表、SIMD（libyuv）、**交給 GPU**（Lab 5 的作法）。

### 單元測試已驗證 pitch 處理（`tests/portable_tests.cpp` → `TestNv12Layout`）

```cpp
const uint32_t w = 4, h = 2, codedH = 4, pitch = 8;   // pitch > width，codedHeight > height
std::vector<uint8_t> buf(..., 0xCD);                  // 讀到 padding 就會出錯
uint8_t* uvP = buf.data() + pitch * codedH;           // UV 偏移用 codedHeight
// 左邊 2 像素紅、右邊 2 像素藍
EXPECT(bgra[2] > 250 && bgra[0] < 5);                 // (0,0) 紅
EXPECT(bgra[3 * 4 + 0] > 250 && bgra[3 * 4 + 2] < 5); // (3,0) 藍
EXPECT(bgra[(1 * w + 1) * 4 + 2] > 250);              // (1,1) 紅：和 (0,0) 共用 UV
```

| 測試設計 | 偵測 |
|---|---|
| `pitch = 8`，`w = 4` | 用 `width` 代替 `pitch` |
| `codedH = 4`，`h = 2` | UV 起點用顯示高度 |
| 左紅右藍 | `(x/2)*2` 寫錯 |
| 檢查 (1,1) | `y / 2` 錯誤 |
| 同時檢查 R 和 B | U、V 讀反或 RGB 寫反 |

實機的 `RowPitch` 剛好等於寬度，但這個測試在 Windows 上已經通過，所以 pitch 和 codedHeight 的處理已經被證明。

---

## 12. 存檔一定要轉 RGB 嗎？JPEG、PNG、H.264、MP4 的色彩空間

**一句話**：看格式。BMP、PNG 只能存 RGB；JPEG 和影片存的就是 YUV。**除錯時最好直接存 NV12 原始資料，不要轉換**。不過螢幕是 RGB，只要「看」，最後一定會在某處轉成 RGB。

| 格式 | 存的是 | 要先轉 RGB？ |
|---|---|---|
| BMP | RGB / BGRA | **要**（Lab 1、2 直接存 BGRA；Lab 3 的 NV12 要轉） |
| PNG | RGB / RGBA | **要** |
| JPEG | YCbCr | 不用（一般 API 收 RGB，內部自己轉） |
| WebP、HEIF、AVIF | YUV 4:2:0 | 不用 |
| H.264 / .mp4 | YUV 4:2:0 | 不用（Lab 4 直接把 NV12 交給 encoder） |
| raw `.yuv` / `.nv12` | 原始位元組 | 不用 |

### Lab 3 為什麼轉 RGB 存 BMP

1. 讓任何看圖軟體都能打開。
2. **驗證手寫的還原公式**：這次轉換本身就是測試對象。

### 除錯：直接存 raw NV12

轉成 RGB 再看，看起來不對時**分不出**是 GPU 的 NV12 錯還是你的公式錯。存 raw 檔再用可信的工具開，就能分開：

```
NV12 ─┬─▶ 你的公式 ─▶ BMP       ← 測試「你的轉換」
      └─▶ raw .nv12 ─▶ ffplay   ← 測試「GPU 的資料」
```

raw 檔要**緊密排列**（第 10 節的寫法）。沒有檔頭，格式和尺寸要自己指定：

```powershell
ffplay -f rawvideo -pix_fmt nv12 -video_size 1280x720 capture.nv12
```

`-video_size` 打錯會整個斜掉。YUView（免費開源）可以逐像素看 Y/U/V 並切換色彩空間。

### JPEG 一定是 BT.601 full range 嗎？

**實務上幾乎是，但不是 JPEG 核心規格規定的。**

| 層級 | 規定 |
|---|---|
| JPEG 核心（ITU-T T.81） | 只管怎麼壓縮通道，**不管代表什麼顏色** |
| **JFIF**（一般 `.jpg`） | YCbCr 用 **BT.601、full range** |
| Adobe APP14 | 可宣告 RGB（不轉換）或 CMYK / YCCK |

**色度降採樣不固定**：4:4:4、4:2:2、4:2:0 都可以。

**陷阱**：把 Lab 3 的 709 studio NV12 直接當 raw YCbCr 交給 JPEG encoder，存出來會霧霧的且偏色（等於 vp 和 dec 不一致）。

### JPEG 壓縮率比 PNG 高，只是因為 YUV 嗎？

**不是。YUV 4:2:0 只貢獻約 2 倍，主因是「有損」。**

```
原始 RGB 6.2 MB ──4:2:0 ÷2──▶ 3.1 MB ──DCT + 量化 ÷5~15──▶ 熵編碼 ──▶ 0.2~0.6 MB（共 10~30 倍）
```

| 步驟 | 做什麼 | 有損？ |
|---|---|---|
| YCbCr + 4:2:0 | 色度降解析度 | 有損 |
| **DCT** | 8×8 區塊從像素轉成頻率成分 | 無損 |
| **量化** | 頻率成分除以一個數再四捨五入，**高頻大多變 0** | **有損，最主要的壓縮來源** |
| 之字形 + 遊程 + Huffman | 壓縮大量的 0 | 無損 |

YUV 的間接作用：亮度和色度可用**不同的量化表**，色度量化得更粗。

**PNG 無損**，只能用「預測 + DEFLATE」：

| | JPEG | PNG |
|---|---|---|
| 照片 | **小很多** | 很大（雜訊難預測） |
| **截圖、文字、UI** | 文字周圍有雜點，不一定比較小 | **常比 JPEG 小**（大片純色好預測） |

桌面畫面比較像截圖，這也是螢幕串流要特別處理文字清晰度的原因。

### H.264 和 MP4 都固定 709 studio 嗎？

**都不是。** H.264 是 codec（信的內容），MP4 是容器（信封），可以裝 H.264、HEVC、AV1、AAC 等。Lab 4 的 `.h264` 沒有容器，所以沒有時間戳（勘誤 M7）。

H.264 的色彩空間寫在 SPS 的 **VUI**，而且**全部是選填**：

| VUI 欄位 | 常見值 |
|---|---|
| `matrix_coefficients` | 1 = 709；5、6 = 601；9 = 2020 |
| `video_full_range_flag` | 0 = studio；1 = full |
| `colour_primaries` | 1 = 709；9 = 2020 |
| `transfer_characteristics` | 1 = 709（SDR）；16 = PQ；18 = HLG |

沒寫就是「未指定」，播放器只能猜。

| 內容 | 常見色彩空間（慣例，不是規定） |
|---|---|
| **HD** | **709 studio** |
| SD | 601 studio |
| 4K HDR | BT.2020 + PQ / HLG，studio |
| 部分手機、螢幕錄影 | **full range**（ffmpeg 的 `yuvj420p`） |

H.264 也不只 8-bit 4:2:0：High 10、High 4:2:2、High 4:4:4 profile 支援更多。MP4 的 `colr` box 也能標示色彩空間，和 bitstream 不一致時，不同播放器可能採用不同的那個。

**對 Lab 4 的意義**：所以 encoder 要明確設 `MF_MT_YUV_MATRIX` 和 `MF_MT_VIDEO_NOMINAL_RANGE`。但**設了不代表一定寫進 VUI**，要量測：

```powershell
ffprobe -v error -select_streams v:0 -show_entries stream=color_range,color_space,color_primaries,color_transfer -of default=nw=1 output.h264
```

`color_range=tv`、`color_space=bt709` 代表有寫入（`tv` = studio，`pc` = full）；`unknown` 代表沒寫，播放器只能猜。這台機器的結果尚未確認。

---

## 13. 檢視學員作業與常見錯誤速查

### 檢視重點

| 項目 | 錯誤的症狀 |
|---|---|
| 說得出 GPU 端的色彩空間 | 只說「轉成 NV12」，說不出 709/601、studio/full |
| CPU 還原公式和 GPU 一致 | 霧霧的或偏色 |
| UV 起點用 `pitch × codedHeight` | 偏綠、條紋（要用奇怪寬度才測得到） |
| UV 取樣 `(x/2)·2`，U 在前 | 錯位、紅藍互換 |
| 有 clamp | 飽和色邊緣雜點 |
| **用色塊量測，不只看圖** | 係數錯了看不出來 |
| 說得出「設錯會怎樣」三個結論 | 只是跑通 |
| 知道要標示進 bitstream | 自己正常、別人偏色 |
| `ColorSpace1` 有 QI 到 `ID3D11VideoContext1` | 編譯不過（E4） |

**口試題**：「你的 NV12 轉回來和原圖一模一樣，能證明轉換正確嗎？」**不能**。可能兩端錯得一致，或係數錯了但畫面以灰階為主。要用純色色塊和理論值比對。

### 症狀速查

| 症狀 | 最可能的原因 |
|---|---|
| 霧霧的，黑不夠黑、白不夠白 | studio 被當成 full |
| 黑色死黑、暗部消失、白色過曝 | full 被當成 studio |
| 灰階正常，飽和色稍偏 | 601 / 709 混用 |
| **紅藍互換** | U、V 顛倒（NV12 / NV21），或 RGBA / BGRA 搞混 |
| 整張偏綠 | UV 起點錯（`width × height`） |
| 斜向條紋、歪斜 | 用 `width` 當行距 |
| 上下錯位、底部雜訊 | UV 起點用 1080 而非 1088 |
| 彩色細字糊掉 | 4:2:0 的正常現象 |
| 飽和色邊緣亮點黑點 | 沒有 clamp |
| ffmpeg `format negotiation failed` | 給 `yuv420p`，encoder 要 `nv12` |

---

## 14. 自我檢查題

1. 為什麼硬體 encoder 要 NV12，而不是直接吃 BGRA？1080p 一張各是多大？
2. YUV 存在的兩個理由是什麼？
3. Cb、Cr 分別對應 U 還是 V？為什麼沒有綠色差？
4. 灰色的 U、V 為什麼一定是 128？
5. Studio range 和 full range 的黑、白各是多少？H.264 預設是哪一個？
6. 4:2:0 的 `4`、`2`、`0` 各是什麼意思？一組 UV 涵蓋幾個像素？
7. NV12、I420、NV21 的差別是什麼？為什麼硬體偏好 NV12？
8. D3D11 Map NV12 後，UV 平面的起點在哪裡？為什麼要用 `codedHeight`？
9. 用 Kr、Kb 推導純紅在 709 studio 下的 Y、U、V。
10. 經典的 `(66*R + 129*G + 25*B + 128) >> 8` 是哪一種色彩空間？拿來處理 HD 影片會怎樣？
11. 為什麼 YUV → RGB 一定要 clamp？
12. 就算兩端設定完全正確，RGB → YUV → RGB 為什麼也不會完全還原？
13. 矩陣和原色有什麼不同？
14. `DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709` 的每一段代表什麼？
15. 為什麼設定 `ColorSpace1` 之後還要量測？
16. `--vp-*` 和 `--decode-*` 各代表誰？在 Lab 6 中分別對應什麼？
17. 只指定 `--vp-matrix 601` 時，decode 端用什麼？為什麼這樣設計？
18. `--patches` 為什麼選紅、綠、藍、白、黑、灰這六個顏色？
19. `GPU-YUV-OFF` 和 `COLOR-OFF` 分別代表哪一端出錯？
20. 實測紅色還原成 `255, 1, 0`，是 bug 嗎？
21. `--decode-range full` 時，YUV measured 會改變嗎？RGB 會變成什麼樣子？
22. 係數設錯，為什麼灰階完全看不出來？
23. 兩端錯得一致時有什麼問題？要怎麼避免？
24. 為什麼 `ReadbackNv12` 用 `CopySubresourceRegion` 而不是 `CopyResource`？
25. `ReadbackBgra` 逐行去掉 padding，`ReadbackNv12` 卻整塊複製，哪一個是對的？
26. 把 NV12 存成 raw 檔時，來源和目的的 UV 起點各用什麼公式？
27. `Nv12ToBgra` 為什麼傳指標和 pitch，而不是傳 `Nv12Image`？
28. `(x / 2) * 2` 寫成 `x` 會怎樣？
29. 看到紅藍互換，要檢查哪兩個地方？
30. 實機 `RowPitch` 等於寬度，要怎麼確認 pitch 處理是正確的？
31. 存檔一定要轉成 RGB 嗎？除錯時為什麼建議存 raw NV12？
32. JPEG 的壓縮率比 PNG 高，主要原因是 YUV 嗎？
33. H.264 一定是 709 studio 嗎？要怎麼確認 Lab 4 的輸出有標示色彩空間？

<details>
<summary>參考答案（先自己想過再展開）</summary>

1. NV12 是 YUV 4:2:0，每像素 1.5 bytes，幾乎所有硬體 encoder 都以它為輸入；BGRA 每像素 4 bytes。1080p：BGRA 8.3 MB，NV12 3.1 MB。
2. 人眼對亮度比顏色敏感，亮度和顏色分開後可以只降低顏色的解析度；黑白電視相容（Y 就是黑白訊號）。
3. Cb = U（藍色差），Cr = V（紅色差）。Y、B、R 已知就能反推 G，存了是重複資訊。
4. 灰色 R = G = B，Cb = (B − Y)/… = 0、Cr = 0，存成 8-bit 時加上中心值 128。
5. Studio：黑 16、白 235；full：黑 0、白 255。H.264 慣例是 studio。
6. 4 像素寬、2 行高的區塊：第一行 2 個色度樣本，第二行新增 0 個。一組 UV 涵蓋 2×2 = 4 個像素。
7. I420 是三個平面（Y、U、V）；NV12 是 Y 加 UV 交錯；NV21 是 Y 加 VU 交錯。NV12 只有兩個平面，UV 交錯讀一組只要一個位置，且能用 R8 / R8G8 格式來看。
8. `pData + RowPitch × codedHeight`。decoder 的 texture 可能是 1088 高，用顯示的 1080 會讀到 Y 平面的最後幾行。
9. E'y = 0.2126 → Y = 16 + 219 × 0.2126 ≈ 63；Cb = −0.2126 / 1.8556 ≈ −0.1146 → U ≈ 102；Cr = 0.7874 / 1.5748 = 0.5 → V = 240。
10. BT.601 studio。處理 HD（709）影片時飽和色會偏，灰階正常。
11. 濾波、降採樣、色域不對稱會讓結果略超出 0–255，沒 clamp 會溢位繞回（−3 變 253），飽和色邊緣出現亮點黑點。
12. 色度降採樣（主要）、量化四捨五入、YUV 數值空間比 RGB 大（超出的只能 clamp）。
13. 矩陣是 RGB 與 YUV 之間的座標轉換；原色定義 R、G、B 是哪一種紅綠藍（色域）。
14. YCbCr 模型、studio 範圍、gamma 2.2、色度取樣位置 left、BT.709 原色與矩陣。
15. setter 回傳 `void`，driver 忽略設定時不會報錯，只能量測。
16. vp = GPU 寫入 NV12 用的格式；decode = CPU 還原時的假設。Lab 6 中分別是 sender 和 receiver。
17. 也是 601。預設值串接（`--decode-matrix` → `--vp-matrix` → `709`），不刻意指定時兩端自動一致，避免無意間製造 mismatch。
18. 紅綠藍偵測係數錯誤（只有飽和色受係數影響）；白黑偵測範圍錯誤（範圍的兩端）；灰檢查中間值與色度是否中性（U、V = 128）。
19. `GPU-YUV-OFF`：GPU 端沒照要求的色彩空間輸出；`COLOR-OFF`：CPU 的假設和 GPU 實際輸出不一致。
20. 不是。Y 的真實值 62.56 存成 63，還原時 G 算出 0.59 → 1。量化損失，在容許值內。
21. 不會改變（GPU 沒動）。RGB 變得霧霧的：黑約 16,16,16，白約 235,235,235，紅約 239,15,15。
22. 灰色的 Cb = Cr = 0，係數只乘在色度上，所以完全不受影響。
23. 自己的系統看起來正常，別人的播放器會偏色。要把色彩空間標示進 bitstream（VUI），並和實際轉換一致。
24. decoder 輸出是 texture array；`CopyResource` 要求兩邊結構完全相同，`CopySubresourceRegion` 可以只挑一個 subresource 複製到單張 staging。
25. 兩個都對。BGRA 在讀回時去掉 padding，使用者不用管 pitch；NV12 保留原始排列並記錄 pitch，由使用者處理。關鍵是誰負責要明確。
26. 來源：`RowPitch × texture 高度`（例如 1088）；目的：`width × height`（例如 1920 × 1080）。
27. 和 D3D 無關、可在 Linux 單元測試；任何來源的 NV12 都能用；UV 起點需要 `codedHeight`，函式本身只知道顯示高度，所以由呼叫端計算。
28. 讀到錯的 UV 組，U、V 錯位，越右邊錯越遠，右半邊顏色完全跑掉。
29. U、V 是否讀反（`uv[0]`、`uv[1]`），以及 B、G、R 寫入順序是否反了。
30. 用 `--width 1366` 之類的寬度讓 driver 產生 padding 再跑 `--patches`；另外 `portable_tests` 的 `TestNv12Layout` 已經用 pitch > width、codedHeight > height 的資料驗證過。
31. 不一定：BMP、PNG 要；JPEG、影片存的就是 YUV。存 raw NV12 再用 ffplay 等可信工具開啟，可以把「GPU 資料錯」和「自己的公式錯」分開。
32. 不是。4:2:0 只貢獻約 2 倍；主因是 DCT 後的量化（有損），把高頻成分大多變成 0。PNG 是無損的，截圖類畫面反而常比 JPEG 小。
33. 不是，VUI 的色彩欄位是選填的。用 `ffprobe -show_entries stream=color_range,color_space,...`，看到 `tv`、`bt709` 才代表 encoder 有寫入；`unknown` 代表沒寫。

</details>
