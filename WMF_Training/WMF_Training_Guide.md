# Windows Media Foundation 新人訓練文件

適用對象：剛加入、需要負責螢幕擷取 / 編碼 / 解碼相關功能開發的 RD。
先備知識：C++、基本 COM 概念（`IUnknown`、`ComPtr`、`Release`）、Direct3D11 基礎（Device / Context / Texture2D）。

---

## 目錄

1. [Media Foundation 架構總覽](#1-media-foundation-架構總覽)
2. [螢幕擷取：Desktop Duplication API](#2-螢幕擷取desktop-duplication-api)
3. [畫面縮放](#3-畫面縮放)
4. [畫面格式轉換](#4-畫面格式轉換)
5. [H264 Encode](#5-h264-encode)
6. [H264 Decode](#6-h264-decode)
7. [End-to-End Pipeline 範例](#7-end-to-end-pipeline-範例)
8. [除錯工具與常見錯誤碼](#8-除錯工具與常見錯誤碼)
9. [實作練習（Lab）](#9-實作練習lab)
10. [延伸閱讀](#10-延伸閱讀)

---

## 1. Media Foundation 架構總覽

### 1.1 為什麼要學 Media Foundation（MF）

Windows 上做「畫面擷取 → 處理 → 編碼 → 傳輸 → 解碼 → 顯示」這條 pipeline（例如遠端桌面、雲端遊戲、視訊會議），核心會用到三塊：

- **DXGI Desktop Duplication API**：擷取桌面畫面（GPU texture）。
- **Direct3D11 Video Processor / Media Foundation Transform (MFT)**：縮放、色彩格式轉換。
- **Media Foundation H264 Encoder/Decoder MFT**：硬體加速編解碼（走 Intel Quick Sync / NVENC / AMD VCE，視顯卡而定）。

這三塊都圍繞著同一組資料容器打轉，所以先熟悉 MF 的基本物件模型，後面每一節都是重複套用同一套概念。

### 1.2 核心物件

| 物件 | 說明 |
|---|---|
| `IMFMediaType` | 描述一份資料的格式（video subtype 如 `MFVideoFormat_NV12`、解析度、frame rate…）。輸入輸出的協商都靠比對 MediaType 完成。 |
| `IMFSample` | 一個「時間單位」的資料容器，帶 timestamp / duration，內部包含 1 個或多個 `IMFMediaBuffer`。 |
| `IMFMediaBuffer` / `IMF2DBuffer2` | 真正裝 bytes 的 buffer，可以是系統記憶體，也可以包一個 D3D11 Texture（`IMFDXGIBuffer`）。 |
| `IMFTransform` (MFT) | 所有「處理器」的統一介面：Video Processor、H264 Encoder、H264 Decoder 全部都實作這個介面（`ProcessInput` / `ProcessOutput`）。 |
| `IMFDXGIDeviceManager` | 讓 MFT 跟你共用同一個 D3D11 Device，資料全程留在 GPU，不需要 CPU readback。**這是效能的關鍵，務必在每個階段都設好。** |

### 1.3 初始化 / 收尾（每支程式都要做）

```cpp
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

// 程式進入點
HRESULT hr = MFStartup(MF_VERSION);

// ... 所有 MF 相關工作 ...

MFShutdown();
```

`MFStartup`/`MFShutdown` 必須成對呼叫，且要在建立任何 MFT / Session 之前完成。

### 1.4 兩種使用姿勢

- **手動兜 MFT**：自己呼叫 `ProcessInput`/`ProcessOutput`，適合像我們這種「擷取→處理→編碼」的自訂 pipeline，掌控力最高，也是本文件的主軸。
- **Topology + Media Session**：MF 內建的高階播放/轉檔框架（`IMFTopology`、`IMFMediaSession`），適合單純播檔案或轉檔，本文件不深入，有興趣可參考第 10 節連結。

---

## 2. 螢幕擷取：Desktop Duplication API

### 2.1 為什麼不用 GDI `BitBlt`

- `BitBlt` 走 CPU，且需要輪詢畫面是否變化，延遲高、耗 CPU。
- **DXGI Desktop Duplication API (DDA)**（`IDXGIOutputDuplication`）是作業系統合成器（DWM）主動推送「有變化的畫面」，資料直接是 GPU 上的 `ID3D11Texture2D`，且能拿到 dirty rect / move rect，非常適合做串流。

> 注意：DDA 抓到的是「桌面合成後」的畫面，全螢幕獨佔（exclusive fullscreen）的遊戲可能抓不到或需要特殊處理；如果要抓特定視窗內容，另有 Windows.Graphics.Capture (WinRT) 可用，本文件聚焦 DDA。

### 2.2 建立流程

```cpp
using Microsoft::WRL::ComPtr;

ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
D3D_FEATURE_LEVEL featureLevel;

D3D11CreateDevice(
    nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
    D3D11_CREATE_DEVICE_BGRA_SUPPORT, // 之後給 D3D11VideoProcessor 用
    nullptr, 0, D3D11_SDK_VERSION,
    &device, &featureLevel, &context);

ComPtr<IDXGIDevice> dxgiDevice;
device.As(&dxgiDevice);

ComPtr<IDXGIAdapter> adapter;
dxgiDevice->GetAdapter(&adapter);

ComPtr<IDXGIOutput> output;
adapter->EnumOutputs(0, &output); // 0 = 主螢幕，多螢幕要列舉

ComPtr<IDXGIOutput1> output1;
output.As(&output1);

ComPtr<IDXGIOutputDuplication> duplication;
HRESULT hr = output1->DuplicateOutput(device.Get(), &duplication);
```

**重要限制**：同一個 Output 同時只能有**一個** `IDXGIOutputDuplication` 存在（同一個 session 內）。如果程式重複建立而沒釋放舊的，會拿到 `E_ACCESSDENIED` / `DXGI_ERROR_UNSUPPORTED`。UAC 提權視窗、鎖定畫面、切換使用者時也可能導致擷取被拒。

### 2.3 抓一張畫面

```cpp
DXGI_OUTDUPL_FRAME_INFO frameInfo;
ComPtr<IDXGIResource> desktopResource;

HRESULT hr = duplication->AcquireNextFrame(
    /*TimeoutInMilliseconds*/ 500,
    &frameInfo,
    &desktopResource);

if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
    // 畫面沒變化，這不是錯誤，正常情況，continue polling
    return;
}
if (hr == DXGI_ERROR_ACCESS_LOST) {
    // 解析度變更 / 顯卡切換 / session 切換，需要整個重建 duplication
    duplication.Reset();
    // 重新走 2.2 的建立流程
    return;
}

ComPtr<ID3D11Texture2D> desktopTexture;
desktopResource.As(&desktopTexture); // 格式通常是 DXGI_FORMAT_B8G8R8A8_UNORM

// 用完務必釋放，否則下一次 AcquireNextFrame 會失敗
duplication->ReleaseFrame();
```

`AcquireNextFrame` 拿到的 texture 生命週期只到下一次 `ReleaseFrame`（或下一次 `AcquireNextFrame`）為止，**如果要保留這張畫面供後續非同步處理（例如丟到編碼 queue），必須自己 `CopyResource` 到另一張 texture**，不能直接存指標帶著跑。

```cpp
// 建立自己的長生命週期 texture（每個 pipeline 通常會建一個 pool 重複使用）
D3D11_TEXTURE2D_DESC desc;
desktopTexture->GetDesc(&desc);
desc.Usage = D3D11_USAGE_DEFAULT;
desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
desc.MiscFlags = 0;

ComPtr<ID3D11Texture2D> stableTexture;
device->CreateTexture2D(&desc, nullptr, &stableTexture);
context->CopyResource(stableTexture.Get(), desktopTexture.Get());
```

### 2.4 滑鼠游標

DDA 預設**不會**把游標畫進畫面裡（除非游標本身是硬體合成的一部分，行為依驅動而異）。`frameInfo.PointerPosition` / `frameInfo.PointerShapeBufferSize` 給你游標形狀跟位置，若需求上要求「畫面含游標」，需要自己用 `GetFramePointerShape` 拿到游標 bitmap，再疊繪到你的 texture 上（AlphaBlend 或 MaskedColor 兩種游標型態要分別處理）。這部分坑較多，建議先確認產品需求是否真的需要，很多串流場景是「本地端自己畫游標」，遠端只傳游標座標即可。

### 2.5 常見錯誤整理

| 情境 | 處理方式 |
|---|---|
| `DXGI_ERROR_ACCESS_LOST` | 顯示模式改變、GPU 重置、UAC 視窗、鎖屏。重建整個 duplication interface。 |
| `DXGI_ERROR_WAIT_TIMEOUT` | 正常，代表畫面沒變化，繼續下一輪。 |
| 多螢幕 | 每個 `IDXGIOutput` 要各自建立一份 duplication；不同螢幕可能在不同 GPU adapter 上，要小心 texture 跨 device 使用的限制。 |
| 全螢幕遊戲黑屏 | 部分獨佔全螢幕應用會擋 DDA，需要提示使用者切視窗模式，或改用 Windows.Graphics.Capture。 |

---

## 3. 畫面縮放

### 3.1 為什麼要縮放

- 擷取到的畫面通常是螢幕原始解析度（例如 3840x2160），但傳輸頻寬 / 目標解析度可能只需要 1920x1080 甚至更低。
- 編碼器對輸入解析度也有要求（通常需要偶數，某些硬體編碼器要求 16 對齊）。

### 3.2 方案比較

| 方案 | 說明 | 適用情境 |
|---|---|---|
| `ID3D11VideoProcessor` (`VideoProcessorBlt`) | D3D11 原生介面，GPU 硬體縮放+色彩轉換一次搞定，效能最好 | **推薦**，本文件主線 |
| Media Foundation "Video Processor MFT" (CLSID `CLSID_VideoProcessorMFT`) | 把上面那組包成 `IMFTransform`，方便跟其他 MFT 串接 | 如果 pipeline 其他部分都是 MFT 風格，統一介面比較好維護 |
| Pixel Shader 自寫 | 完全客製化（例如要做特殊濾鏡） | 有特殊畫質/效能需求時才自己寫 |
| CPU (`StretchRect` 等) | 效能差，資料要來回搬 CPU/GPU | 不建議，除非沒有 GPU 資源 |

本節示範 **`ID3D11VideoProcessor`**，因為縮放跟格式轉換（第 4 節）通常是同一次 Blt 完成，效能最佳。

### 3.3 建立 Video Processor

```cpp
ComPtr<ID3D11VideoDevice> videoDevice;
device.As(&videoDevice);

ComPtr<ID3D11VideoContext> videoContext;
context.As(&videoContext);

D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc = {};
contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
contentDesc.InputWidth  = srcWidth;
contentDesc.InputHeight = srcHeight;
contentDesc.OutputWidth  = dstWidth;
contentDesc.OutputHeight = dstHeight;

ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
videoDevice->CreateVideoProcessorEnumerator(&contentDesc, &vpEnum);

ComPtr<ID3D11VideoProcessor> videoProcessor;
videoDevice->CreateVideoProcessor(vpEnum.Get(), /*RateConversionIndex*/ 0, &videoProcessor);
```

### 3.4 建立輸入/輸出 View 並執行縮放

```cpp
// 輸入：包住桌面擷取到的 BGRA texture
D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputViewDesc = {};
inputViewDesc.FourCC = 0; // 用 texture 原本的 format
inputViewDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
inputViewDesc.Texture2D.MipSlice = 0;
inputViewDesc.Texture2D.ArraySlice = 0;

ComPtr<ID3D11VideoProcessorInputView> inputView;
videoDevice->CreateVideoProcessorInputView(
    stableTexture.Get(), vpEnum.Get(), &inputViewDesc, &inputView);

// 輸出：目標尺寸、目標格式的 texture（例如縮放後仍是 BGRA，或直接轉 NV12，見第 4 節）
D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputViewDesc = {};
outputViewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;

ComPtr<ID3D11VideoProcessorOutputView> outputView;
videoDevice->CreateVideoProcessorOutputView(
    outputTexture.Get(), vpEnum.Get(), &outputViewDesc, &outputView);

// 設定來源/目的矩形（可用來做裁切或維持長寬比）
RECT srcRect = { 0, 0, (LONG)srcWidth, (LONG)srcHeight };
RECT dstRect = { 0, 0, (LONG)dstWidth, (LONG)dstHeight };
videoContext->VideoProcessorSetStreamSourceRect(videoProcessor.Get(), 0, TRUE, &srcRect);
videoContext->VideoProcessorSetStreamDestRect(videoProcessor.Get(), 0, TRUE, &dstRect);

D3D11_VIDEO_PROCESSOR_STREAM stream = {};
stream.Enable = TRUE;
stream.pInputSurface = inputView.Get();

videoContext->VideoProcessorBlt(videoProcessor.Get(), outputView.Get(), 0, 1, &stream);
```

### 3.5 效能提醒

- 全程都是 GPU texture，**不要**中間插入 `Map`/`CopyResource` 到 CPU 記憶體再算，那會把 GPU pipeline 打斷、拖慢整條 pipeline。
- `VideoProcessorBlt` 支援同時做 deinterlace / 色彩轉換 / 縮放 / HDR tone mapping，是「一次 Blt 打包多工」的概念，第 4 節的格式轉換基本上就是接著這裡多設一個 output format 而已。
- 如果縮放比例是動態變化的（例如依網路頻寬自動降解析度），建議 output texture 用 pool 重複利用，避免每次都重新 `CreateTexture2D`。

---

## 4. 畫面格式轉換

### 4.1 為什麼要轉格式

- Desktop Duplication 給的是 **BGRA (B8G8R8A8_UNORM)**，這是給顯示 / 合成器用的格式。
- H264 硬體編碼器（第 5 節）幾乎全部要求輸入是 **NV12**（YUV 4:2:0，Y plane + interleaved UV plane），少數支援 P010（10-bit HDR）。
- 所以擷取到畫面之後，一定要做一次 **BGRA → NV12** 的色彩空間轉換，順便如第 3 節做縮放。

### 4.2 色彩空間需要注意的坑

| 議題 | 說明 |
|---|---|
| RGB → YUV 係數 | 主要有 BT.601（SD內容）跟 BT.709（HD/UHD 內容），係數不同，選錯會讓顏色偏色。桌面串流一般用 BT.709。 |
| Full range vs Studio range | RGB 通常是 full range (0-255)；YUV 傳統視訊是 studio/limited range (16-235)，某些播放器對這個很敏感，畫面會偏灰或黑位不對，記得跟解碼端的假設保持一致。 |
| Chroma siting | 4:2:0 的 UV 取樣位置（co-sited vs interstitial）在極端銳利畫面（如文字）邊緣會有肉眼可見差異。 |

這些參數在 `ID3D11VideoContext` 上是透過 `VideoProcessorSetOutputColorSpace` / `VideoProcessorSetStreamColorSpace` 設定，或者在 MediaType 裡用 `MF_MT_VIDEO_NOMINAL_RANGE`、`MF_MT_YUV_MATRIX` attribute 描述，兩端（encode 設定與 decode 端假設）務必一致，否則會出現「編出來自己解沒事，別人的播放器顏色不對」的典型 bug。

### 4.3 承接第 3 節：讓 VideoProcessorBlt 直接輸出 NV12

延續 3.4 的程式碼，只要把 `outputTexture` 建成 NV12 格式即可，縮放跟色彩轉換一次做完：

```cpp
D3D11_TEXTURE2D_DESC nv12Desc = {};
nv12Desc.Width  = dstWidth;
nv12Desc.Height = dstHeight;
nv12Desc.Format = DXGI_FORMAT_NV12;
nv12Desc.MipLevels = 1;
nv12Desc.ArraySize = 1;
nv12Desc.SampleDesc.Count = 1;
nv12Desc.Usage = D3D11_USAGE_DEFAULT;
nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET; // Video Processor 輸出需要這個 bind flag

ComPtr<ID3D11Texture2D> nv12Texture;
device->CreateTexture2D(&nv12Desc, nullptr, &nv12Texture);

// 設定色彩空間（BT.709, studio range，符合大部分編碼器預期）
DXGI_COLOR_SPACE_TYPE inputColorSpace  = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
DXGI_COLOR_SPACE_TYPE outputColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
videoContext->VideoProcessorSetStreamColorSpace1(videoProcessor.Get(), 0, inputColorSpace);
videoContext->VideoProcessorSetOutputColorSpace1(videoProcessor.Get(), outputColorSpace);

// 之後照 3.4 建 output view + VideoProcessorBlt，outputView 綁的就是 nv12Texture
```

### 4.4 包裝成 IMFSample 交給編碼器

轉完格式後，要把這張 `ID3D11Texture2D` 包成 `IMFSample`，才能餵給 H264 Encoder MFT（第 5 節）：

```cpp
ComPtr<IMFMediaBuffer> mfBuffer;
MFCreateDXGISurfaceBuffer(
    __uuidof(ID3D11Texture2D), nv12Texture.Get(),
    /*subresourceIndex*/ 0, /*fTopDown*/ FALSE, &mfBuffer);

ComPtr<IMFSample> sample;
MFCreateSample(&sample);
sample->AddBuffer(mfBuffer.Get());
sample->SetSampleTime(timestamp100ns);
sample->SetSampleDuration(frameDuration100ns);
```

`MFCreateDXGISurfaceBuffer` 是關鍵函式：它讓 `IMFSample` 底層直接指向 GPU texture，不需要 CPU readback，這也是為什麼第 5 節的 Encoder MFT 一定要設定 `IMFDXGIDeviceManager`——沒設的話，MF 會強迫把 GPU texture 讀回 CPU 再處理，效能會掉非常多。

---

## 5. H264 Encode

### 5.1 硬體編碼器（H264 Encoder MFT）

Windows 內建的 H264 Encoder MFT（CLSID `CLSID_CMSH264EncoderMFT`）在支援的硬體上會自動走 Intel Quick Sync / NVIDIA NVENC / AMD VCE，不支援的機器會 fallback 到軟體編碼（`MFT_ENUM_HARDWARE_URL` attribute 可以拿來判斷是不是硬體）。

### 5.2 列舉並建立 Encoder MFT

```cpp
MFT_REGISTER_TYPE_INFO inputType  = { MFMediaType_Video, MFVideoFormat_NV12 };
MFT_REGISTER_TYPE_INFO outputType = { MFMediaType_Video, MFVideoFormat_H264 };

IMFActivate** activateArray = nullptr;
UINT32 count = 0;
MFTEnumEx(
    MFT_CATEGORY_VIDEO_ENCODER,
    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
    &inputType, &outputType,
    &activateArray, &count);

ComPtr<IMFTransform> encoder;
activateArray[0]->ActivateObject(IID_PPV_ARGS(&encoder));
// 用完 for 迴圈 Release 每個 activateArray[i]，再 CoTaskMemFree(activateArray)
```

### 5.3 綁定 D3D11 Device（讓 GPU texture 直接進 Encoder）

```cpp
UINT resetToken;
ComPtr<IMFDXGIDeviceManager> deviceManager;
MFCreateDXGIDeviceManager(&resetToken, &deviceManager);
deviceManager->ResetDevice(device.Get(), resetToken);

ComPtr<IMFAttributes> encoderAttrs;
encoder->GetAttributes(&encoderAttrs);
encoderAttrs->SetUnknown(MF_TRANSFORM_ASYNC_UNLOCK, nullptr); // 若是 async MFT 需要先 unlock
encoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager.Get());
```

大部分硬體編碼 MFT 是 **Async MFT**：建立後要先呼叫 `MFT_MESSAGE_COMMAND_FLUSH` 之類事件驅動的流程，並實作 `IMFAsyncCallback` 監聽 `METransformNeedInput` / `METransformHaveOutput` 事件。也有部分（尤其較舊的軟體 encoder）是同步的，可以直接 `ProcessInput`/`ProcessOutput` 輪詢。建議先用 `MF_TRANSFORM_ASYNC` attribute 判斷。

### 5.4 設定輸入/輸出 MediaType

```cpp
// 輸出（先設定輸出，因為編碼器的輸入格式常常依輸出參數決定可用選項）
ComPtr<IMFMediaType> outType;
MFCreateMediaType(&outType);
outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
outType->SetUINT32(MF_MT_AVG_BITRATE, 8'000'000); // 8 Mbps
MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, dstWidth, dstHeight);
MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, 60, 1);
MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
encoder->SetOutputType(0, outType.Get(), 0);

// 輸入
ComPtr<IMFMediaType> inType;
MFCreateMediaType(&inType);
inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, dstWidth, dstHeight);
MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 60, 1);
encoder->SetInputType(0, inType.Get(), 0);
```

### 5.5 即時串流常用參數（低延遲）

即時串流（如遠端桌面/雲遊戲）跟一般轉檔追求的目標不同，重點是「低延遲、可預期的 bitrate」而不是壓縮率極限：

| 參數 | Codec API attribute | 說明 |
|---|---|---|
| Low latency 模式 | `CODECAPI_AVLowLatencyMode = TRUE` | 關掉多張 lookahead，減少內部緩衝延遲 |
| Rate control | `CODECAPI_AVEncCommonRateControlMode` | 串流建議 `CBR`，畫質優先可用 `Quality VBR` |
| GOP / I-frame 間隔 | `CODECAPI_AVEncMPVGOPSize` | 越短越容易做 seek/錯誤恢復，但壓縮率會下降 |
| 即時強制 I-frame | `MFT_MESSAGE_COMMAND_MARK_IN_STREAM` / `CODECAPI_AVEncVideoForceKeyFrame` | 網路 lost frame 需要重新同步時強制送一張 I-frame |
| Max bitrate | `CODECAPI_AVEncCommonMaxBitRate` | 避免尖峰畫面（如快速滑動）bitrate 爆衝 |

```cpp
ComPtr<ICodecAPI> codecApi;
encoder.As(&codecApi);

VARIANT v;
V_VT(&v) = VT_BOOL; V_BOOL(&v) = VARIANT_TRUE;
codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);

V_VT(&v) = VT_UI4; V_UI4(&v) = eAVEncCommonRateControlMode_CBR;
codecApi->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
```

### 5.6 送資料 / 拿結果（同步輪詢版簡化示意）

```cpp
// 送一張畫面進去
encoder->ProcessInput(0, sample.Get(), 0);

// 詢問輸出（實務上常見 MF_E_TRANSFORM_NEED_MORE_INPUT，代表要再送一張才夠）
MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
DWORD status = 0;

MFT_OUTPUT_STREAM_INFO streamInfo;
encoder->GetOutputStreamInfo(0, &streamInfo);

ComPtr<IMFSample> outSample;
MFCreateSample(&outSample);
ComPtr<IMFMediaBuffer> outBuffer;
MFCreateMemoryBuffer(streamInfo.cbSize, &outBuffer);
outSample->AddBuffer(outBuffer.Get());
outputBuffer.pSample = outSample.Get();

HRESULT hr = encoder->ProcessOutput(0, 1, &outputBuffer, &status);
if (hr == S_OK) {
    // outSample 裡就是編碼好的 H264 bitstream（Annex B 格式，帶 start code）
    // 送去網路傳輸 / 存檔
}
```

實務上（尤其 async MFT）建議直接查官方 `MFTEnumEx` sample 或用 **Sink Writer**（`IMFSinkWriter`）包一層，會處理掉很多 async 事件迴圈的細節，除非有特殊延遲要求才自己手刻 async loop。

---

## 6. H264 Decode

### 6.1 硬體解碼器（H264 Decoder MFT）

對應 CLSID `CLSID_CMSH264DecoderMFT`，一樣走 DXVA2/D3D11 硬體解碼路徑，輸出可以直接是 D3D11 texture（NV12），不需要 CPU readback，非常適合接著直接 render 到畫面上。

### 6.2 建立與設定（跟 Encoder 對稱）

```cpp
MFT_REGISTER_TYPE_INFO decInput  = { MFMediaType_Video, MFVideoFormat_H264 };
MFT_REGISTER_TYPE_INFO decOutput = { MFMediaType_Video, MFVideoFormat_NV12 };

IMFActivate** decActivate = nullptr;
UINT32 decCount = 0;
MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
    MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
    &decInput, &decOutput, &decActivate, &decCount);

ComPtr<IMFTransform> decoder;
decActivate[0]->ActivateObject(IID_PPV_ARGS(&decoder));

// 同樣要綁 D3D11 device manager，讓輸出直接落在 GPU texture
decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)deviceManager.Get());
```

### 6.3 輸入格式：SPS/PPS 與 NAL 處理

H264 bitstream 常見兩種封裝：

- **Annex B**（帶 `00 00 00 01` start code）：MF Decoder MFT 預期吃這種格式。
- **AVCC**（length-prefixed，MP4/RTP 常見）：如果上游用 RTP/WebRTC 傳輸，通常是 AVCC 或裸 NAL，**餵給 MF Decoder 前務必轉成 Annex B**（把 length prefix 換成 start code）。

SPS（Sequence Parameter Set）/ PPS（Picture Parameter Set）通常放在第一個 keyframe 前，或透過 `MF_MT_MPEG_SEQUENCE_HEADER` attribute 塞進輸入 MediaType：

```cpp
ComPtr<IMFMediaType> decInType;
MFCreateMediaType(&decInType);
decInType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
decInType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
MFSetAttributeSize(decInType.Get(), MF_MT_FRAME_SIZE, width, height);
// 若拿得到 SPS/PPS bytes，可用 MF_MT_MPEG_SEQUENCE_HEADER 預先塞入，
// 否則讓第一個 IDR frame 自帶 SPS/PPS 也可以，解碼器會自動解析。
decoder->SetInputType(0, decInType.Get(), 0);
```

### 6.4 送資料 / 拿畫面

跟 Encoder 對稱，只是方向相反：

```cpp
decoder->ProcessInput(0, encodedSample.Get(), 0); // encodedSample 裡是收到的 H264 NAL bytes

MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
outputBuffer.pSample = nullptr; // 硬體解碼器通常自己配置輸出 texture，不用你先建
DWORD status = 0;
HRESULT hr = decoder->ProcessOutput(0, 1, &outputBuffer, &status);

if (hr == S_OK) {
    ComPtr<IMFMediaBuffer> buf;
    outputBuffer.pSample->GetBufferByIndex(0, &buf);
    ComPtr<IMFDXGIBuffer> dxgiBuf;
    buf.As(&dxgiBuf);

    ComPtr<ID3D11Texture2D> decodedTexture;
    dxgiBuf->GetResource(IID_PPV_ARGS(&decodedTexture));
    UINT subresourceIndex;
    dxgiBuf->GetSubresourceIndex(&subresourceIndex);
    // decodedTexture + subresourceIndex 就是解碼結果，可直接拿去 render 或做後續 VideoProcessorBlt 轉回 RGB 顯示
}
```

拿到的 `decodedTexture` 常常是 **texture array**（硬體解碼器內部有個 surface pool 循環使用），要用 `subresourceIndex` 指定是 array 裡的哪一張，不要假設永遠是同一張 texture 指標。

### 6.5 Decoder 端注意事項

- 遇到丟包/花屏時，通常靠上游網路協議（如 RTP/WebRTC 的 NACK/PLI）要求對端重送 keyframe，Decoder 本身沒有錯誤修復能力，錯誤畫面會一直延續到下一張 IDR。
- 解碼輸出若要顯示，記得跟第 4 節提到的色彩空間（BT.709/601、range）要跟編碼端假設一致，否則顏色會偏。
- Decoder MFT 也是 async 為主，一樣需要處理 `METransformNeedInput`/`METransformHaveOutput` 事件，或用 `IMFSourceReader` 包一層簡化。

---

## 7. End-to-End Pipeline 範例

把前面各節串起來，一個典型的「本機串流傳輸」pipeline 長這樣：

```
[Desktop Duplication]                     [對端/播放端]
   AcquireNextFrame                        ProcessOutput (Decoder)
   (BGRA D3D11 Texture)                     ↓ NV12 D3D11 Texture
        ↓                                   VideoProcessorBlt (NV12→RGB, 若需顯示轉回螢幕格式)
   VideoProcessorBlt                        ↓
   (縮放 + BGRA→NV12)                       Render to swapchain
        ↓
   MFCreateDXGISurfaceBuffer → IMFSample
        ↓
   H264 Encoder MFT.ProcessInput/Output
        ↓
   H264 NAL bytes ──── 網路傳輸 (RTP/自訂協議) ────→ ProcessInput (Decoder)
```

**設計原則**：整條路徑上，畫面資料應該全程留在 GPU（`ID3D11Texture2D`），只有最終的「編碼後 bitstream」才會落回 CPU 記憶體用於網路傳輸；解碼端收到 bitstream 後立刻進硬體解碼器，輸出又是 GPU texture，直到真的要顯示才走 render。任何一個環節如果多了一次不必要的 GPU↔CPU 搬運（`Map`/`CopyResource` 到 staging texture），都會是效能瓶頸的常見來源，Code Review 時要特別留意。

---

## 8. 除錯工具與常見錯誤碼

### 8.1 工具

| 工具 | 用途 |
|---|---|
| **GraphStudioNext** | 視覺化拼 DirectShow/MF graph，快速驗證某個 MFT 支援哪些格式 |
| **dxcap** (Windows SDK) | 針對 DXGI/D3D 做 capture 除錯 |
| **PIX for Windows** | GPU frame 除錯，看 texture 內容、GPU timeline，抓效能瓶頸的第一選擇 |
| **MFTrace** (舊版 SDK 工具) | 追蹤 MF pipeline 的方法呼叫順序 |
| Media Foundation ETW (`Microsoft-Windows-Media-Foundation-Platform`) | 用 `WPR`/`xperf` 抓 MF 內部事件 |
| `dxdiag` | 快速確認顯卡支援的硬體編解碼能力 |

### 8.2 常見 HRESULT

| HRESULT | 常見原因 |
|---|---|
| `MF_E_TRANSFORM_TYPE_NOT_SET` | 呼叫 `ProcessInput`/`ProcessOutput` 前沒設好 Input/Output MediaType |
| `MF_E_TRANSFORM_NEED_MORE_INPUT` | Encoder/Decoder 內部緩衝還不夠，需要再送資料才有輸出（不是錯誤，是正常流程訊號） |
| `MF_E_INVALIDMEDIATYPE` | 設定的 MediaType 該 MFT 不支援，先用 `GetInputAvailableType`/`GetOutputAvailableType` 列舉支援清單 |
| `DXGI_ERROR_ACCESS_LOST` | 見第 2.5 節，Desktop Duplication 需要重建 |
| `E_ACCESSDENIED`（建立 duplication 時） | 前一個 duplication 沒釋放、或處於安全桌面（UAC/鎖屏） |
| `MF_E_NO_SAMPLE_TIMESTAMP` | Encoder 要求輸入 Sample 一定要設 `SetSampleTime`，忘了設 |

---

## 9. 實作練習（Lab）

原則：**每題都要能獨立編譯執行、獨立驗收**，不要求一次到位串完整條 pipeline。後面題目允許直接沿用前一題的程式碼基底繼續加功能。除非特別註明，皆使用 C++ / Win32，Debug 與 Release 都要能跑（硬體編碼/解碼在某些筆電上可能因驅動而不支援，遇到這種情況要能明確印出錯誤原因，而不是直接閃退）。

---

### Lab 1｜桌面擷取存成圖片

**題目**：寫一支 CLI 小程式 `dda_capture.exe`，用 Desktop Duplication API 抓取指定螢幕（用參數指定 Output index，預設 0）的一張畫面，存成 `capture.bmp`。

**規格**：
- 需正確處理 `DXGI_ERROR_WAIT_TIMEOUT`（重試，最多重試 N 次後放棄並印出訊息）。
- 需正確處理 `AcquireNextFrame` 拿到的 texture 生命週期，`ReleaseFrame` 呼叫時機要正確（不能提早釋放造成資料損毀，也不能忘記釋放造成下次 Acquire 失敗）。
- 程式結束前要正常 `Release` 所有 COM 物件（用 `ComPtr`/RAII，不要手動 `Release()` 裸指標）。

**驗收標準**：
- 執行後產生的 `capture.bmp` 用小畫家打開，畫面內容與當下螢幕一致（顏色、比例都對）。
- 刻意把螢幕切到鎖定畫面（Win+L）後執行，程式要能印出清楚的錯誤訊息（例如 `E_ACCESSDENIED`），而不是 crash。
- 額外挑戰：支援多螢幕環境下，指定第 2 個螢幕也能正確抓取。

---

### Lab 2｜GPU 縮放

**題目**：延伸 Lab 1，改用 `ID3D11VideoProcessor` 把擷取到的畫面縮放到指定解析度（用參數指定，例如 `dda_capture.exe --width 1280 --height 720`），輸出仍存成 BGRA 的 `capture_scaled.bmp`。

**規格**：
- 縮放過程中資料不能落回 CPU 記憶體（即擷取到縮放完成這段，只能有 D3D11 Texture 之間的操作，最後存檔前才能 `Map` 到 CPU 一次）。
- 需正確處理來源長寬比與目標長寬比不同的情況：至少要選擇一種策略實作（等比縮放後上下/左右補黑邊 letterbox，或直接忽略比例強制拉伸），並在程式的 README 或註解說明選了哪種策略。

**驗收標準**：
- 螢幕解析度 3840x2160 縮放到 1280x720，輸出圖片實際尺寸為 1280x720，畫面中文字清晰可辨識、無明顯鋸齒或模糊到誇張的程度。
- 用碼表或簡易 `QueryPerformanceCounter` 量測「拿到 texture → 縮放完成」耗時，寫在程式輸出裡（讓自己養成量測效能的習慣）。

---

### Lab 3｜色彩格式轉換（BGRA → NV12）驗證

**題目**：延伸 Lab 2，把 Video Processor 的輸出格式從 BGRA 改成 NV12，並自己寫一個 `SaveNV12AsBmp()` 函式（自己手刻 YUV→RGB 換算公式，不能呼叫另一個 Video Processor 幫你轉回去），把 NV12 結果轉存成 BMP 驗證。

**規格**：
- 需要處理 NV12 的記憶體佈局：Y plane 完整解析度，UV plane 是寬高各半、U/V 交錯（`ID3D11Texture2D` 的 NV12 格式在 `Map` 出來後，Y 跟 UV 是同一個 texture 兩個 plane，要用 `subresource` / row pitch 正確取值）。
- 需選定並在文件中寫明使用的係數（BT.601 或 BT.709）與 range（full 或 studio），自行手刻转换公式。

> **注意**：這裡要求「手刻」CPU 版的 YUV→RGB 轉換，純粹是教學目的——逼你自己算一次公式，理解係數/range 設錯會發生什麼事——只用來一次性驗證單張畫面的正確性，不是正式的效能路徑，所以不需要考慮 SIMD 或多執行緒優化。正式串流 pipeline 的色彩轉換仍然是靠第 3、4 節教的 `VideoProcessorBlt`（GPU 硬體轉換），跟這裡的除錯用 CPU 程式碼是兩條完全不同的路徑，兩者效能互不影響，不要把 Lab 3 的寫法搬進正式產品程式碼。

**驗收標準**：
- 存出來的 BMP 跟 Lab 2 的 BGRA 版本，肉眼比對顏色一致（沒有偏綠、偏紫、或對比異常）。
- 準備一張刻意設計的測試畫面（例如純紅、純綠、純藍、純白色塊的圖，用小畫家畫好後全螢幕顯示再擷取），驗證轉換後色塊仍接近正確的顏色，允許因 8-bit 量化有些許誤差，但不應該肉眼可辨的偏色。
- 文件需要寫一段（3-5 句）說明「如果 range 或係數設錯，畫面會變成什麼樣子」，證明你理解這個轉換背後的原理，不是複製貼上程式碼。

---

### Lab 4｜H264 硬體編碼

**題目**：延伸 Lab 3，接上 H264 Encoder MFT，把連續擷取 5 秒（假設 30fps，共約 150 張）的 NV12 畫面編碼成一支 `output.h264` 檔案。

**規格**：
- 需要用 `MFTEnumEx` 列舉出編碼器，並印出「這次用的是硬體編碼器還是軟體 fallback」（用 `MFT_ENUM_FLAG_HARDWARE` 篩選並印出實際拿到的 Activate 名稱）。
- 需正確設定輸入/輸出 `IMFMediaType`，並處理 `MF_E_TRANSFORM_NEED_MORE_INPUT` 的迴圈邏輯（送一張、可能拿不到輸出、再送下一張，直到全部送完後要 `Drain`）。
- 需要正確設定每個 Sample 的 timestamp / duration（依 30fps 換算成 100ns 單位），輸出的 h264 檔案時間軸才會正確。

**驗收標準**：
- 用 `ffplay output.h264` 或 VLC 打開可以正常播放，播放時長與畫面內容跟預期（約 5 秒的螢幕錄影）一致。
- 用 `ffprobe output.h264` 檢查，`codec_name` 為 `h264`，解析度與 Lab 2 設定的目標解析度相符。
- 額外挑戰：實作 `--bitrate` 參數，分別編碼 2Mbps 與 8Mbps 兩個版本，用檔案大小佐證 bitrate 設定確實生效（貼出兩個檔案的大小對照）。

---

### Lab 5｜H264 硬體解碼

**題目**：獨立寫一支 `h264_decode.exe`，讀取 Lab 4 產生的 `output.h264`，用 H264 Decoder MFT 解碼，把每一張解碼出來的畫面存成 `frame_0001.bmp`, `frame_0002.bmp`, ... 依序編號。

**規格**：
- 需要自己寫最簡單的 Annex B NAL 切割邏輯（用 `00 00 00 01` / `00 00 01` start code 切出一個個 NAL unit，餵給 decoder），不需要用完整的 demuxer 函式庫。
- 解碼輸出的 `ID3D11Texture2D` 記得用 `subresourceIndex` 正確取用（第 6.4 節有範例），並轉存成 BMP 前一樣要處理 NV12→RGB。

**驗收標準**：
- 存出來的第一張 `frame_0001.bmp` 跟 Lab 4 編碼前的原始畫面（Lab 3 存的那張）肉眼比對內容一致（構圖、文字都能辨識，允許因編碼壓縮產生的輕微失真）。
- 產生的圖片張數應接近預期的 frame 數（約 150 張，正負幾張都合理，因為編碼器可能會丟棄/合併極少數 frame）。
- 額外挑戰：把畫面 render 到一個 Win32 視窗上即時播放（而不是存成一堆 BMP），驗證播放起來流暢、沒有明顯撕裂或卡頓。

---

### Lab 6｜端到端即時串流（整合＋效能量測）

**題目**：把 Lab 1~5 串成兩支程式（Sender / Receiver），透過 **TCP socket** 傳輸 H264 bitstream：Sender 端畫面擷取 → 縮放 → 格式轉換 → H264 編碼 → 用 TCP 送出；Receiver 端從 TCP 收資料 → H264 解碼 → render 到視窗，即時跑，直到使用者按 ESC 結束。兩支程式可以先用 `127.0.0.1` 本機互連測試，能力足夠的話再實測跨機器 (同一區網) 傳輸。

**規格**：
- TCP 是連續的 byte stream，沒有訊息邊界，**每次送一張編碼後的畫面前，必須先送一個固定長度的長度前綴（例如 4 bytes，代表接下來這個 NAL/frame 的 byte 數）**，Receiver 端才知道要讀多少 bytes 才算收完一張畫面，這是本題刻意要練習的重點（跟 6.3 節提到的 AVCC length-prefix 概念一致）。
- Socket 建議用**阻塞模式 + 獨立的收送執行緒**（Sender 一個 thread 專職 `send()`，Receiver 一個 thread 專職 `recv()`），不要把網路 I/O 卡在畫面擷取或 render 的主迴圈裡，避免網路延遲直接卡住擷取/顯示。
- 全程量測並即時印出（或畫在畫面角落）以下三個延遲數字：
  1. 擷取到編碼完成的時間（ms）
  2. 編碼完成到解碼完成的時間（ms，此區間已包含 TCP 傳輸耗時）
  3. 端到端總延遲（擷取那一刻到畫面顯示出來那一刻，ms）
- 需要能設定並即時切換輸出解析度（例如按 `+`/`-` 鍵切換 1080p / 720p / 480p），驗證縮放參數可以動態調整而不需要重啟程式。
- 程式要能連續跑 10 分鐘不 crash、不明顯記憶體洩漏（用工作管理員或 PIX 簡單確認記憶體用量是否持續上升），且 TCP 連線斷開（例如 Receiver 端被關掉）時 Sender 要能偵測到並乾淨結束，不要一直卡住或狂噴例外。

**驗收標準**：
- 提交一份簡短報告（3~5 段文字即可），列出：
  - 三個延遲數字的量測結果（在你自己的機器上，標明是硬體編碼/解碼還是軟體 fallback）。
  - 你觀察到 pipeline 中哪個環節耗時最長，猜測原因。
  - 如果要進一步降低延遲，你會先嘗試調整哪個參數或環節，為什麼。
- Code Review 時會特別檢查：整條路徑上有沒有「非必要的 GPU→CPU→GPU」搬運（例如某個環節偷懶直接 Map 到 CPU 處理），這是本題的核心考點，不是能不能跑起來而已。

---

### 提交方式與 Review 重點

- 每題請各自建立獨立的資料夾/專案，附上簡短 README 說明如何編譯執行、用到哪些參數。
- Code Review 時 mentor 會特別檢查：COM 物件釋放是否正確（有無用 `ComPtr` 或等價 RAII）、錯誤處理是否覆蓋 `ACCESS_LOST`/`WAIT_TIMEOUT` 等已知情境、GPU 資料是否全程留在 GPU（未做不必要的 CPU readback）、以及色彩空間/range 設定是否前後一致。
- 卡關超過半天沒有進展，直接找 mentor 討論，不需要自己硬摳——這些 API 的坑很多是「文件沒寫清楚」等級的坑，卡住很正常。

---

## 10. 延伸閱讀

- Microsoft Docs – [Media Foundation](https://learn.microsoft.com/windows/win32/medfound/microsoft-media-foundation-sdk)
- Microsoft Docs – [Desktop Duplication API](https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api)
- Microsoft Docs – [H.264 Video Encoder](https://learn.microsoft.com/windows/win32/medfound/h-264-video-encoder)
- Microsoft Docs – [H.264 Video Decoder](https://learn.microsoft.com/windows/win32/medfound/h-264-video-decoder)
- Microsoft Docs – [Video Processor MFT / ID3D11VideoProcessor](https://learn.microsoft.com/windows/win32/api/d3d11/nn-d3d11-id3d11videoprocessor)
- Microsoft Docs – [ICodecAPI / CODECAPI 屬性列表](https://learn.microsoft.com/windows/win32/medfound/codecapi-properties)
- GitHub – Microsoft `Windows-classic-samples` repo 中的 `Media Foundation` 範例（DesktopDuplication、EncodingWithMFT 等現成 sample project，可直接抓來跑）

