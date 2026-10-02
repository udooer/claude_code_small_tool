// timer.h — QueryPerformanceCounter 小工具
#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>

inline int64_t QpcNow()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline int64_t QpcFrequency()
{
    static const int64_t f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x.QuadPart; }();
    return f;
}

inline double QpcToMs(int64_t ticks) { return ticks * 1000.0 / QpcFrequency(); }

// Media Foundation 的時間單位是 100ns
inline int64_t QpcTo100ns(int64_t ticks) { return (int64_t)((double)ticks * 1e7 / QpcFrequency()); }

// 簡單統計：平均 / 最大
struct Stat {
    double sum = 0, max = 0;
    int count = 0;
    void Add(double v) { sum += v; max = std::max(max, v); ++count; }
    double Avg() const { return count ? sum / count : 0; }
    void Reset() { *this = Stat(); }
};
