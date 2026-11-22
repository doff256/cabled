#include "image_adjust.h"

#include <gtest/gtest.h>

#include <vector>

using namespace cabled::engine;

namespace {

struct Nv12 {
    uint32_t w, h;
    std::vector<uint8_t> y, uv;
    Nv12(uint32_t width, uint32_t height, uint8_t luma = 126, uint8_t u = 128, uint8_t v = 128)
        : w(width), h(height), y(size_t(width) * height, luma), uv(size_t(width) * height / 2) {
        for (size_t i = 0; i < uv.size(); i += 2) uv[i] = u, uv[i + 1] = v;
    }
    cabled::vcam::FrameView view() {
        cabled::vcam::FrameView f;
        f.fourcc = cabled::vcam::kFourccNV12;
        f.width = w;
        f.height = h;
        f.plane0 = y.data();
        f.pitch0 = w;
        f.plane1 = uv.data();
        f.pitch1 = w;
        return f;
    }
};

}  // namespace

TEST(ImageAdjust, NeutralLeavesThePictureAlone) {
    Nv12 a(16, 8, 100, 90, 170);
    const auto before = a.y;
    ImageAdjuster adj;
    adj.apply(a.view(), ImageAdjust{});
    EXPECT_EQ(a.y, before);
    EXPECT_TRUE(ImageAdjust{}.neutral());
}

TEST(ImageAdjust, BrightnessAndContrastMoveLuma) {
    Nv12 a(16, 8, 126);  // mid-grey in limited range ((126-16)/219 ~ 0.5)
    ImageAdjuster adj;
    ImageAdjust brighter;
    brighter.brightness = 0.5;
    adj.apply(a.view(), brighter);
    EXPECT_NEAR(a.y[0], 126 + 0.125 * 219, 1);

    Nv12 dark(16, 8, 60), light(16, 8, 190);
    ImageAdjust contrast;
    contrast.contrast = 1.5;
    adj.apply(dark.view(), contrast);
    adj.apply(light.view(), contrast);
    EXPECT_LT(dark.y[0], 60);   // darks darker
    EXPECT_GT(light.y[0], 190); // lights lighter
    ImageAdjust extreme;
    extreme.brightness = 1;
    extreme.contrast = 2;
    adj.apply(light.view(), extreme);
    EXPECT_EQ(light.y[0], 235);  // stays in limited range
}

TEST(ImageAdjust, ZeroSaturationIsGrey) {
    Nv12 a(16, 8, 120, 60, 200);
    ImageAdjust bw;
    bw.saturation = 0;
    ImageAdjuster adj;
    adj.apply(a.view(), bw);
    EXPECT_EQ(a.uv[0], 128);
    EXPECT_EQ(a.uv[1], 128);
}

TEST(ImageAdjust, MirrorFlipsLumaAndKeepsChromaPairsTogether) {
    Nv12 a(8, 2);
    for (uint32_t x = 0; x < 8; ++x) a.y[x] = uint8_t(20 + x);
    a.uv[0] = 50, a.uv[1] = 60;  // leftmost pair
    a.uv[6] = 70, a.uv[7] = 80;  // rightmost pair
    ImageAdjust m;
    m.mirror = true;
    ImageAdjuster adj;
    adj.apply(a.view(), m);
    EXPECT_EQ(a.y[0], 27);
    EXPECT_EQ(a.y[7], 20);
    EXPECT_EQ(a.uv[0], 70);  // U stays before V
    EXPECT_EQ(a.uv[1], 80);
    EXPECT_EQ(a.uv[6], 50);
    EXPECT_EQ(a.uv[7], 60);
}

TEST(ImageAdjust, SharpeningIncreasesEdgeContrastOnly) {
    Nv12 a(16, 8, 100);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 8; x < 16; ++x) a.y[y * 16 + x] = 160;  // vertical edge at x = 8
    ImageAdjust s;
    s.sharpness = 1;
    ImageAdjuster adj;
    adj.apply(a.view(), s);
    EXPECT_LT(a.y[4 * 16 + 7], 100);  // dark side of the edge darker
    EXPECT_GT(a.y[4 * 16 + 8], 160);  // light side lighter
    EXPECT_EQ(a.y[4 * 16 + 2], 100);  // flat areas untouched
    EXPECT_EQ(a.y[4 * 16 + 13], 160);
}

TEST(ImageAdjust, Rgb32MirrorAndSaturation) {
    std::vector<uint8_t> px = {10, 20, 200, 0, 50, 60, 70, 0};  // two BGRX pixels
    cabled::vcam::FrameView f;
    f.fourcc = cabled::vcam::kFourccRGB32;
    f.width = 2;
    f.height = 1;
    f.plane0 = px.data();
    f.pitch0 = 8;
    ImageAdjust a;
    a.mirror = true;
    a.saturation = 0;
    ImageAdjuster adj;
    adj.apply(f, a);
    EXPECT_EQ(px[0], px[1]);  // grey
    EXPECT_EQ(px[1], px[2]);
    EXPECT_NEAR(px[0], 0.0722 * 50 + 0.7152 * 60 + 0.2126 * 70, 1);  // the right pixel came left
}
