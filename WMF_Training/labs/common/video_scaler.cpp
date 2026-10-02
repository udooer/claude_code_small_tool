#include "video_scaler.h"

#include <algorithm>
#include <cstdio>

#include "hr.h"

namespace {

bool IsYuv(DXGI_FORMAT f) { return f == DXGI_FORMAT_NV12 || f == DXGI_FORMAT_P010 || f == DXGI_FORMAT_YUY2; }

DXGI_COLOR_SPACE_TYPE YuvColorSpace(YuvMatrix m, YuvRange r)
{
    if (m == YuvMatrix::BT709)
        return r == YuvRange::Studio ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709;
    return r == YuvRange::Studio ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601;
}

// 舊版 API（ID3D11VideoContext1 不存在時的 fallback）
D3D11_VIDEO_PROCESSOR_COLOR_SPACE LegacyColorSpace(bool yuv, YuvMatrix m, YuvRange r)
{
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
    cs.Usage = 0;     // playback
    cs.RGB_Range = 0; // RGB full range 0-255
    if (yuv) {
        cs.YCbCr_Matrix = (m == YuvMatrix::BT709) ? 1 : 0;
        cs.Nominal_Range = (r == YuvRange::Studio) ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235
                                                   : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    }
    return cs;
}

} // namespace

RECT ComputeDestRect(UINT srcW, UINT srcH, UINT dstW, UINT dstH, ScaleMode mode)
{
    if (mode == ScaleMode::Stretch) return { 0, 0, (LONG)dstW, (LONG)dstH };
    double s = std::min((double)dstW / srcW, (double)dstH / srcH);
    LONG w = std::min((LONG)dstW, (LONG)(srcW * s + 0.5)) & ~1L;
    LONG h = std::min((LONG)dstH, (LONG)(srcH * s + 0.5)) & ~1L;
    LONG x = (((LONG)dstW - w) / 2) & ~1L;
    LONG y = (((LONG)dstH - h) / 2) & ~1L;
    return { x, y, x + w, y + h };
}

void VideoScaler::Init(ID3D11Device* device, const ScalerConfig& cfg)
{
    cfg_ = cfg;
    ClearViewCache();
    vp_.Reset();
    enum_.Reset();

    CHECK_HR(device->QueryInterface(IID_PPV_ARGS(&videoDevice_)));
    ComPtr<ID3D11DeviceContext> ctx;
    device->GetImmediateContext(&ctx);
    CHECK_HR(ctx.As(&videoContext_));

    // ContentDesc 只是「提示」，讓驅動挑合適的 processor；真正的縮放由 rect 決定
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = cfg.srcWidth;
    cd.InputHeight = cfg.srcHeight;
    cd.OutputWidth = cfg.dstWidth;
    cd.OutputHeight = cfg.dstHeight;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    CHECK_HR(videoDevice_->CreateVideoProcessorEnumerator(&cd, &enum_));

    UINT inFlags = 0, outFlags = 0;
    CHECK_HR(enum_->CheckVideoProcessorFormat(cfg.srcFormat, &inFlags));
    CHECK_HR(enum_->CheckVideoProcessorFormat(cfg.dstFormat, &outFlags));
    if (!(inFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
        throw std::runtime_error("Video processor does not support the input format on this GPU");
    if (!(outFlags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::runtime_error("Video processor does not support the output format on this GPU");

    CHECK_HR(videoDevice_->CreateVideoProcessor(enum_.Get(), 0, &vp_));

    destRect_ = ComputeDestRect(cfg.srcWidth, cfg.srcHeight, cfg.dstWidth, cfg.dstHeight, cfg.mode);
    RECT full = { 0, 0, (LONG)cfg.dstWidth, (LONG)cfg.dstHeight };
    RECT src = { 0, 0, (LONG)cfg.srcWidth, (LONG)cfg.srcHeight };

    videoContext_->VideoProcessorSetStreamFrameFormat(vp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, &src);
    videoContext_->VideoProcessorSetStreamDestRect(vp_.Get(), 0, TRUE, &destRect_);
    videoContext_->VideoProcessorSetOutputTargetRect(vp_.Get(), TRUE, &full);
    // 關掉驅動的自動畫質增強（降噪、銳化、色彩增強），否則 Lab 3 色塊量測會被影響
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(vp_.Get(), 0, FALSE);

    // letterbox 黑邊：沒設的話黑邊區域內容是未定義的（可能是殘影）
    D3D11_VIDEO_COLOR black{};
    black.RGBA.R = 0; black.RGBA.G = 0; black.RGBA.B = 0; black.RGBA.A = 1;
    videoContext_->VideoProcessorSetOutputBackgroundColor(vp_.Get(), FALSE, &black);

    ApplyColorSpaces();
}

void VideoScaler::ApplyColorSpaces()
{
    const bool srcYuv = IsYuv(cfg_.srcFormat), dstYuv = IsYuv(cfg_.dstFormat);
    ComPtr<ID3D11VideoContext1> vc1; // ...ColorSpace1 系列在 ID3D11VideoContext1（勘誤 E4）
    if (SUCCEEDED(videoContext_.As(&vc1))) {
        vc1->VideoProcessorSetStreamColorSpace1(vp_.Get(), 0,
            srcYuv ? YuvColorSpace(cfg_.yuvMatrix, cfg_.yuvRange) : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vc1->VideoProcessorSetOutputColorSpace1(vp_.Get(),
            dstYuv ? YuvColorSpace(cfg_.yuvMatrix, cfg_.yuvRange) : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in = LegacyColorSpace(srcYuv, cfg_.yuvMatrix, cfg_.yuvRange);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out = LegacyColorSpace(dstYuv, cfg_.yuvMatrix, cfg_.yuvRange);
        videoContext_->VideoProcessorSetStreamColorSpace(vp_.Get(), 0, &in);
        videoContext_->VideoProcessorSetOutputColorSpace(vp_.Get(), &out);
    }
}

void VideoScaler::Process(ID3D11Texture2D* src, UINT arraySlice, ID3D11Texture2D* dst, const RECT* srcRect)
{
    auto& inView = inputViews_[{ src, arraySlice }];
    if (!inView) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d{};
        d.FourCC = 0; // 用 texture 本身的格式
        d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = 0;
        d.Texture2D.ArraySlice = arraySlice;
        CHECK_HR(videoDevice_->CreateVideoProcessorInputView(src, enum_.Get(), &d, &inView));
    }
    auto& outView = outputViews_[dst];
    if (!outView) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC d{};
        d.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D; // 目標 texture 需要 BIND_RENDER_TARGET
        CHECK_HR(videoDevice_->CreateVideoProcessorOutputView(dst, enum_.Get(), &d, &outView));
    }
    if (srcRect) videoContext_->VideoProcessorSetStreamSourceRect(vp_.Get(), 0, TRUE, srcRect);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();
    CHECK_HR(videoContext_->VideoProcessorBlt(vp_.Get(), outView.Get(), 0, 1, &stream));
}
