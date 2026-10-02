// video_scaler.h — ID3D11VideoProcessor 封裝：縮放 + 色彩格式轉換一次 Blt 完成（Lab 2 起使用）
//
//   Enumerator(內容描述) -> Processor -> InputView(來源 texture) / OutputView(目標 texture) -> VideoProcessorBlt
//
// 全程只有 GPU texture 之間的操作，沒有任何 CPU readback。
#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include <map>
#include <utility>

#include "yuv.h"

using Microsoft::WRL::ComPtr;

enum class ScaleMode {
    Letterbox, // 等比縮放，多出來的區域補黑邊
    Stretch,   // 忽略比例，直接拉滿
};

struct ScalerConfig {
    UINT srcWidth = 0, srcHeight = 0;
    DXGI_FORMAT srcFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT dstWidth = 0, dstHeight = 0;
    DXGI_FORMAT dstFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    ScaleMode mode = ScaleMode::Letterbox;
    // YUV 那一側（輸入或輸出是 NV12 時）使用的色彩空間。
    // 預設 BT.709 studio：桌面/HD 內容的慣例，也是大部分 encoder / player 的預期。
    YuvMatrix yuvMatrix = YuvMatrix::BT709;
    YuvRange yuvRange = YuvRange::Studio;
};

class VideoScaler {
public:
    void Init(ID3D11Device* device, const ScalerConfig& cfg);
    bool IsInitialized() const { return vp_ != nullptr; }
    const ScalerConfig& Config() const { return cfg_; }

    // src 可以是 texture array（解碼器輸出），arraySlice 指定第幾張。
    // srcRect 為 nullptr 時用整張；解碼器輸出 1920x1088 時傳 {0,0,1920,1080} 把對齊的多餘行裁掉。
    void Process(ID3D11Texture2D* src, UINT arraySlice, ID3D11Texture2D* dst, const RECT* srcRect = nullptr);

    // 縮放後內容在輸出 texture 中的位置（letterbox 時不是整張）
    RECT DestRect() const { return destRect_; }

    // 解析度切換時，舊的 view 會指向不再使用的 texture，清掉快取
    void ClearViewCache()
    {
        inputViews_.clear();
        outputViews_.clear();
    }

private:
    void ApplyColorSpaces();

    ScalerConfig cfg_;
    ComPtr<ID3D11VideoDevice> videoDevice_;
    ComPtr<ID3D11VideoContext> videoContext_;
    ComPtr<ID3D11VideoProcessorEnumerator> enum_;
    ComPtr<ID3D11VideoProcessor> vp_;
    RECT destRect_{};
    // view 建立有成本，用 (texture, slice) 當 key 快取
    std::map<std::pair<ID3D11Texture2D*, UINT>, ComPtr<ID3D11VideoProcessorInputView>> inputViews_;
    std::map<ID3D11Texture2D*, ComPtr<ID3D11VideoProcessorOutputView>> outputViews_;
};

// 依策略算出目標矩形（偶數對齊，NV12 需要）
RECT ComputeDestRect(UINT srcW, UINT srcH, UINT dstW, UINT dstH, ScaleMode mode);
