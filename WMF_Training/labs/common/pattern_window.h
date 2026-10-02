// pattern_window.h — Lab 3 用的全螢幕純色色塊測試畫面
//
// 為什麼要純色？灰階 (R=G=B) 轉成 YUV 後 U=V=128，係數 (601/709) 設錯完全看不出來；
// 只有飽和色才會暴露係數錯誤，range 錯誤則看黑/白。
#pragma once

#include <windows.h>

#include <vector>

#include "yuv.h"

struct Patch {
    const char* name;
    Rgb color;
};

const std::vector<Patch>& TestPatches();

// 在指定的螢幕區域（桌面座標）蓋一個 topmost 全螢幕視窗，畫出直條色塊。
class PatternWindow {
public:
    explicit PatternWindow(const RECT& desktopRect);
    ~PatternWindow();
    // 處理訊息一段時間，讓 DWM 把畫面合成出來
    void PumpFor(DWORD ms);

    // 第 i 個色塊中心點（相對於螢幕左上角，也就是 DDA 畫面座標）
    POINT PatchCenter(size_t i) const;

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    HWND hwnd_ = nullptr;
    LONG width_ = 0, height_ = 0;
};
