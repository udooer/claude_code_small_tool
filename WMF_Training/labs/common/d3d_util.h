// d3d_util.h — D3D11 device 建立、texture 建立、GPU→CPU readback（只在存 BMP 時使用）
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "yuv.h"

using Microsoft::WRL::ComPtr;

struct D3DContext {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output; // 只有 CreateD3DForOutput 會填
    DXGI_OUTPUT_DESC outputDesc{};
    std::wstring adapterName;
};

// 列出所有 adapter 上的所有 output（全域編號 0,1,2...）
void PrintOutputs();

// 找到第 outputIndex 個螢幕，並在「擁有這個螢幕的 adapter」上建立 device。
// 這樣在混合顯卡筆電上才不會 DuplicateOutput 失敗（mentor 指南勘誤 M2）。
D3DContext CreateD3DForOutput(UINT outputIndex);

// 不需要螢幕的情境（解碼端）：用預設 adapter。
D3DContext CreateD3DDefault();

// MFT 會在自己的 thread 使用同一個 device，必須開多執行緒保護（勘誤 M1）
void EnableMultithreadProtection(ID3D11Device* device);

// 給 encoder / decoder 共用 device 用
ComPtr<IMFDXGIDeviceManager> CreateDxgiDeviceManager(ID3D11Device* device);

ComPtr<ID3D11Texture2D> CreateTexture(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                      UINT bindFlags, D3D11_USAGE usage = D3D11_USAGE_DEFAULT, UINT cpuAccess = 0);

// ---------- Readback（除錯/存檔用，絕對不要放在串流的每幀路徑上） ----------

// BGRA texture -> 緊密排列的 BGRA bytes。注意 Map 回來的 RowPitch 不等於 width*4。
std::vector<uint8_t> ReadbackBgra(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                                  UINT* outWidth, UINT* outHeight);

bool SaveBgraTextureAsBmp(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const std::string& path);

// NV12 的 CPU 複本
struct Nv12Image {
    std::vector<uint8_t> data; // pitch * codedHeight * 3/2
    UINT pitch = 0;
    UINT codedWidth = 0, codedHeight = 0; // texture 實際尺寸（例如 1920x1088）
    const uint8_t* YPlane() const { return data.data(); }
    const uint8_t* UVPlane() const { return data.data() + (size_t)pitch * codedHeight; } // 用 codedHeight！
};

// 支援 texture array（解碼器輸出）：subresource 指定是哪一張。
Nv12Image ReadbackNv12(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, UINT subresource);

// Lab 3 的 SaveNV12AsBmp：手刻 YUV->RGB（yuv.h），只轉左上角 width x height（display 區域）
bool SaveNv12AsBmp(const Nv12Image& img, UINT width, UINT height, YuvMatrix m, YuvRange r, const std::string& path);

// 等 GPU 把目前已提交的指令做完（量測用）。回傳等待的毫秒數。
double WaitForGpu(ID3D11Device* device, ID3D11DeviceContext* ctx);
