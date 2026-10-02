// annexb.h — 最簡單的 H.264 Annex B 切割（平台無關，Lab 5 / Lab 6 使用）
//
// Annex B：每個 NAL 前面有 start code 00 00 01 或 00 00 00 01。
// NAL header 第一個 byte 的低 5 bits 是 nal_unit_type。
//
// 重點觀念：NAL 數量 != 畫面數。
//   SPS(7)/PPS(8)/SEI(6)/AUD(9) 都不是畫面；一張畫面也可能切成多個 slice。
//   「一張畫面的所有 NAL」叫做 Access Unit (AU)，送給解碼器時以 AU 為單位最自然。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

enum NalType : int {
    kNalSlice = 1,
    kNalIdr = 5,
    kNalSei = 6,
    kNalSps = 7,
    kNalPps = 8,
    kNalAud = 9,
};

struct NalUnit {
    size_t startCodePos; // start code 起點（含 00 00 (00) 01）
    size_t payloadPos;   // NAL header byte 的位置
    size_t end;          // 下一個 start code 起點（不含）
    int type;
};

struct AccessUnit {
    size_t begin; // 第一個 NAL 的 start code 起點
    size_t end;
    int nalCount;
    bool keyframe; // 含 IDR slice
};

inline std::vector<NalUnit> SplitNalUnits(const uint8_t* buf, size_t len)
{
    std::vector<NalUnit> nals;
    size_t i = 0;
    while (i + 3 <= len) {
        if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1) {
            size_t sc = (i > 0 && buf[i - 1] == 0) ? i - 1 : i; // 4-byte start code
            if (!nals.empty()) nals.back().end = sc;
            size_t payload = i + 3;
            if (payload < len)
                nals.push_back({ sc, payload, len, buf[payload] & 0x1F });
            i = payload;
        } else {
            ++i;
        }
    }
    // 去掉每個 NAL 尾巴的 0（trailing_zero_8bits / 下一個 4-byte start code 的前導 0 已經處理）
    for (auto& n : nals)
        while (n.end > n.payloadPos + 1 && buf[n.end - 1] == 0) --n.end;
    return nals;
}

inline bool IsVcl(int type) { return type == kNalSlice || type == kNalIdr; }

// slice header 第一個欄位是 first_mb_in_slice (ue(v))。值為 0 時編碼成單一個 '1' bit，
// 所以「header 後第一個 byte 的最高位元 = 1」代表這是一張新畫面的第一個 slice。
inline bool IsFirstSliceOfPicture(const uint8_t* buf, const NalUnit& n)
{
    return n.payloadPos + 1 < n.end && (buf[n.payloadPos + 1] & 0x80) != 0;
}

inline std::vector<AccessUnit> GroupAccessUnits(const uint8_t* buf, size_t len)
{
    std::vector<AccessUnit> aus;
    std::vector<NalUnit> nals = SplitNalUnits(buf, len);
    bool curHasVcl = false;
    for (const NalUnit& n : nals) {
        bool startsNew;
        if (aus.empty()) {
            startsNew = true;
        } else if (IsVcl(n.type)) {
            startsNew = curHasVcl && IsFirstSliceOfPicture(buf, n);
        } else {
            // AUD/SPS/PPS/SEI 出現在 VCL 之後 => 下一張畫面開始了
            startsNew = curHasVcl && (n.type == kNalAud || n.type == kNalSps || n.type == kNalPps ||
                                      n.type == kNalSei || (n.type >= 14 && n.type <= 18));
        }
        if (startsNew) {
            aus.push_back({ n.startCodePos, n.end, 0, false });
            curHasVcl = false;
        }
        AccessUnit& au = aus.back();
        au.end = n.end;
        au.nalCount++;
        if (n.type == kNalIdr) au.keyframe = true;
        if (IsVcl(n.type)) curHasVcl = true;
    }
    return aus;
}
