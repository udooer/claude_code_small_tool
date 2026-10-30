// timer.h — QueryPerformanceCounter 小工具
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

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

// 簡單統計：平均 / 最小 / 最大 / 百分位數（保留每個樣本，量測用）
struct Stat {
    double sum = 0, min = 0, max = 0;
    int count = 0;
    std::vector<double> values;
    void Add(double v)
    {
        if (count == 0 || v < min) min = v;
        sum += v;
        max = std::max(max, v);
        ++count;
        values.push_back(v);
    }
    double Avg() const { return count ? sum / count : 0; }
    // p = 0..100；最近排名法
    double Percentile(double p) const
    {
        if (values.empty()) return 0;
        std::vector<double> v = values;
        std::sort(v.begin(), v.end());
        size_t i = (size_t)std::lround(p / 100.0 * (double)(v.size() - 1));
        return v[std::min(i, v.size() - 1)];
    }
    void Reset() { *this = Stat(); }
};
