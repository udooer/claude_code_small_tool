// portable_tests.cpp — 不需要 Windows / GPU 就能跑的單元測試
// 測試 yuv.h（Lab 3 公式）、annexb.h（Lab 5 NAL 切割）、framing.h（Lab 6 訊框）、bmp.h
//
//   g++ -std=c++17 -I../common portable_tests.cpp -o portable_tests && ./portable_tests
#include "annexb.h"
#include "bmp.h"
#include "framing.h"
#include "yuv.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failures = 0;
#define EXPECT(cond)                                                              \
    do {                                                                          \
        if (!(cond)) {                                                            \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++g_failures;                                                         \
        }                                                                         \
    } while (0)

static bool Near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

static void TestYuvReferenceTable()
{
    std::printf("[yuv] reference table (mentor guide Lab 3)\n");
    struct Case { Rgb rgb; YuvMatrix m; Yuv expect; } cases[] = {
        { {0, 0, 0},       YuvMatrix::BT709, {16, 128, 128} },
        { {255, 255, 255}, YuvMatrix::BT709, {235, 128, 128} },
        { {255, 0, 0},     YuvMatrix::BT709, {63, 102, 240} },
        { {0, 255, 0},     YuvMatrix::BT709, {173, 42, 26} },
        { {0, 0, 255},     YuvMatrix::BT709, {32, 240, 118} },
        { {255, 0, 0},     YuvMatrix::BT601, {82, 90, 240} },
        { {0, 255, 0},     YuvMatrix::BT601, {145, 54, 34} },
        { {0, 0, 255},     YuvMatrix::BT601, {41, 240, 110} },
    };
    for (const auto& c : cases) {
        Yuv y = RgbToYuv(c.rgb, c.m, YuvRange::Studio);
        EXPECT(Near(y.y, c.expect.y, 1) && Near(y.u, c.expect.u, 1) && Near(y.v, c.expect.v, 1));
    }
}

static void TestYuvRoundTrip()
{
    std::printf("[yuv] round trip, all 4 matrix/range combos\n");
    const YuvMatrix ms[] = { YuvMatrix::BT601, YuvMatrix::BT709 };
    const YuvRange rs[] = { YuvRange::Studio, YuvRange::Full };
    for (auto m : ms)
        for (auto r : rs)
            for (int R = 0; R < 256; R += 15)
                for (int G = 0; G < 256; G += 15)
                    for (int B = 0; B < 256; B += 15) {
                        Rgb in{ (uint8_t)R, (uint8_t)G, (uint8_t)B };
                        Rgb out = YuvToRgb(RgbToYuv(in, m, r), m, r);
                        // 8-bit 量化誤差；studio range 只有 219 階，誤差稍大
                        EXPECT(Near(out.r, R, 3) && Near(out.g, G, 3) && Near(out.b, B, 3));
                    }
}

static void TestExplicitCoefficients()
{
    std::printf("[yuv] derived coefficients == textbook BT.709 studio formula\n");
    YuvToRgbCoeffs k = MakeYuvToRgbCoeffs(YuvMatrix::BT709, YuvRange::Studio);
    EXPECT(std::fabs(k.yScale - 1.164) < 0.001);
    EXPECT(std::fabs(k.rv * k.cScale - 1.793) < 0.001);
    EXPECT(std::fabs(k.gu * k.cScale - 0.213) < 0.001);
    EXPECT(std::fabs(k.gv * k.cScale - 0.533) < 0.001);
    EXPECT(std::fabs(k.bu * k.cScale - 2.112) < 0.001);
}

static void TestMismatchSymptoms()
{
    std::printf("[yuv] wrong range/matrix produce the documented symptoms\n");
    // studio 資料當 full 解：黑變灰、白變不夠白（對比降低）
    Yuv black = RgbToYuv({0, 0, 0}, YuvMatrix::BT709, YuvRange::Studio);
    Yuv white = RgbToYuv({255, 255, 255}, YuvMatrix::BT709, YuvRange::Studio);
    Rgb b2 = YuvToRgb(black, YuvMatrix::BT709, YuvRange::Full);
    Rgb w2 = YuvToRgb(white, YuvMatrix::BT709, YuvRange::Full);
    EXPECT(b2.r == 16 && w2.r == 235);
    // 係數錯：灰階不受影響
    Yuv gray = RgbToYuv({128, 128, 128}, YuvMatrix::BT709, YuvRange::Studio);
    Rgb g2 = YuvToRgb(gray, YuvMatrix::BT601, YuvRange::Studio);
    EXPECT(Near(g2.r, 128, 1) && Near(g2.g, 128, 1) && Near(g2.b, 128, 1));
    // 係數錯：飽和色會偏。差異不大（約 15~40 個數值），肉眼只覺得「有點怪」，所以要用量測的。
    //   709 編 / 601 解：純綠 -> (20,255,9) 偏黃綠；純紅 -> (233,0,2) 變暗
    //   601 編 / 709 解：純紅 -> (255,24,0) 偏橘；純綠 -> (0,216,0) 變暗
    Rgb gr2 = YuvToRgb(RgbToYuv({0, 255, 0}, YuvMatrix::BT709, YuvRange::Studio), YuvMatrix::BT601, YuvRange::Studio);
    EXPECT(gr2.r >= 15);
    Rgb rd2 = YuvToRgb(RgbToYuv({255, 0, 0}, YuvMatrix::BT601, YuvRange::Studio), YuvMatrix::BT709, YuvRange::Studio);
    EXPECT(rd2.g >= 15);
}

static void TestNv12Layout()
{
    std::printf("[yuv] NV12 layout with pitch > width and codedHeight > height\n");
    const uint32_t w = 4, h = 2, codedH = 4, pitch = 8;
    std::vector<uint8_t> buf(pitch * codedH + pitch * codedH / 2, 0xCD);
    uint8_t* yP = buf.data();
    uint8_t* uvP = buf.data() + pitch * codedH; // UV 偏移要用 codedHeight！
    Yuv red = RgbToYuv({255, 0, 0}, YuvMatrix::BT709, YuvRange::Studio);
    Yuv blue = RgbToYuv({0, 0, 255}, YuvMatrix::BT709, YuvRange::Studio);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) yP[y * pitch + x] = x < 2 ? red.y : blue.y;
    uvP[0] = red.u; uvP[1] = red.v; uvP[2] = blue.u; uvP[3] = blue.v;
    std::vector<uint8_t> bgra(w * h * 4);
    Nv12ToBgra(yP, uvP, pitch, w, h, YuvMatrix::BT709, YuvRange::Studio, bgra.data());
    EXPECT(bgra[2] > 250 && bgra[0] < 5);                   // (0,0) red: R high, B low
    EXPECT(bgra[3 * 4 + 0] > 250 && bgra[3 * 4 + 2] < 5);   // (3,0) blue
    EXPECT(bgra[(1 * w + 1) * 4 + 2] > 250);                // (1,1) red (shares UV)
}

static void TestAnnexB()
{
    std::printf("[annexb] NAL split and access unit grouping\n");
    // AUD | SPS | PPS | IDR(first_mb=0) | AUD | P slice (first_mb=0) | P slice (first_mb!=0) | SEI | P(first_mb=0)
    std::vector<uint8_t> s = {
        0, 0, 0, 1, 0x09, 0xF0,                  // AUD
        0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1F,      // SPS
        0, 0, 1,    0x68, 0xCE, 0x38,            // PPS (3-byte start code)
        0, 0, 0, 1, 0x65, 0x88, 0x84,            // IDR, first_mb_in_slice=0
        0, 0, 0, 1, 0x09, 0xF0,                  // AUD -> new AU
        0, 0, 0, 1, 0x41, 0x9A, 0x01,            // P, first_mb=0
        0, 0, 1,    0x41, 0x40, 0x02,            // P, second slice of same picture
        0, 0, 0, 1, 0x06, 0x05, 0x01,            // SEI -> new AU
        0, 0, 0, 1, 0x41, 0x9A, 0x03, 0x00, 0x00 // P, with trailing zeros
    };
    auto nals = SplitNalUnits(s.data(), s.size());
    EXPECT(nals.size() == 9);
    EXPECT(nals[0].type == kNalAud && nals[1].type == kNalSps && nals[2].type == kNalPps && nals[3].type == kNalIdr);
    EXPECT(nals[2].startCodePos == 14 && nals[2].payloadPos == 17);
    EXPECT(nals[8].end == s.size() - 2); // 尾巴的 0 被去掉
    auto aus = GroupAccessUnits(s.data(), s.size());
    EXPECT(aus.size() == 3);
    if (aus.size() == 3) {
        EXPECT(aus[0].keyframe && aus[0].nalCount == 4 && aus[0].begin == 0);
        EXPECT(!aus[1].keyframe && aus[1].nalCount == 3);
        EXPECT(aus[2].nalCount == 2);
    }
}

static void TestFraming()
{
    std::printf("[framing] header round trip and validation\n");
    framing::FrameHeader h;
    h.payloadSize = 123456;
    h.frameIndex = 42;
    h.flags = framing::kKeyframe;
    h.width = 1920;
    h.height = 1080;
    h.captureQpc = 0x0123456789ABCDEFLL;
    h.encodeDoneQpc = -5;
    uint8_t buf[framing::kHeaderSize];
    framing::Serialize(h, buf);
    EXPECT(buf[0] == 'W' && buf[1] == 'M' && buf[2] == 'F' && buf[3] == '6');
    EXPECT(buf[4] == 0x00 && buf[5] == 0x01 && buf[6] == 0xE2 && buf[7] == 0x40); // big-endian 123456
    framing::FrameHeader r;
    EXPECT(framing::Deserialize(buf, r));
    EXPECT(r.payloadSize == h.payloadSize && r.frameIndex == 42 && r.flags == 1 && r.width == 1920 &&
           r.height == 1080 && r.captureQpc == h.captureQpc && r.encodeDoneQpc == -5);
    buf[0] = 'X';
    EXPECT(!framing::Deserialize(buf, r));
    h.payloadSize = framing::kMaxPayload + 1;
    framing::Serialize(h, buf);
    EXPECT(!framing::Deserialize(buf, r));
}

static void TestBmp()
{
    std::printf("[bmp] header fields\n");
    std::vector<uint8_t> px(3 * 2 * 4, 0x7F);
    const char* path = "portable_test.bmp";
    EXPECT(SaveBmp32(path, px.data(), 3, 2));
    FILE* f = std::fopen(path, "rb");
    std::vector<uint8_t> d(200);
    size_t n = f ? std::fread(d.data(), 1, d.size(), f) : 0;
    if (f) std::fclose(f);
    std::remove(path);
    EXPECT(n == 54 + 24);
    EXPECT(d[0] == 'B' && d[1] == 'M' && d[18] == 3 && d[28] == 32);
    int32_t height = (int32_t)(d[22] | d[23] << 8 | d[24] << 16 | (uint32_t)d[25] << 24);
    EXPECT(height == -2);
}

int main()
{
    TestYuvReferenceTable();
    TestYuvRoundTrip();
    TestExplicitCoefficients();
    TestMismatchSymptoms();
    TestNv12Layout();
    TestAnnexB();
    TestFraming();
    TestBmp();
    std::printf(g_failures ? "\n%d FAILURE(S)\n" : "\nALL PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
