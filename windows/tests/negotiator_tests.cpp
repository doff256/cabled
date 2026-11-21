#include "negotiator.h"

#include <gtest/gtest.h>

using namespace cabled::engine;

namespace {

// Roughly what an iPhone's rear main camera reports (420v formats).
PhoneCaps iphoneCaps() {
    return parseCaps({{"formats",
                       {{{"width", 1280}, {"height", 720}, {"minFps", 1}, {"maxFps", 60}},
                        {{"width", 1920}, {"height", 1080}, {"minFps", 1}, {"maxFps", 60}},
                        {{"width", 1920}, {"height", 1440}, {"minFps", 1}, {"maxFps", 30}},
                        {{"width", 3840}, {"height", 2160}, {"minFps", 1}, {"maxFps", 60}},
                        {{"width", 4032}, {"height", 3024}, {"minFps", 1}, {"maxFps", 30}}}},
                      {"codecs", {"h264", "hevc"}}});
}

Want app(uint32_t w, uint32_t h, double fps) { return Want{true, w, h, fps}; }

}  // namespace

TEST(Negotiator, NobodyWatchingTurnsVideoOff) {
    const Choice c = negotiate(iphoneCaps(), Want{false}, 300);
    EXPECT_FALSE(c.enabled);
    EXPECT_EQ(c.toConfigure(), nlohmann::json({{"enabled", false}}));
}

TEST(Negotiator, ExactFormatWhenThePhoneHasIt) {
    const Choice c = negotiate(iphoneCaps(), app(1920, 1080, 60), 300);
    EXPECT_TRUE(c.enabled);
    EXPECT_EQ(c.width, 1920u);
    EXPECT_EQ(c.height, 1080u);
    EXPECT_DOUBLE_EQ(c.fps, 60);
    EXPECT_EQ(c.codec, "h264");
    EXPECT_NEAR(c.bitrate / 1e6, 40, 2);
}

TEST(Negotiator, SmallestCoveringFormatWhenNoExactMatch) {
    // 1600x900 isn't a camera format: take 1920x1080 (covers it, same aspect), not 4K.
    const Choice c = negotiate(iphoneCaps(), app(1600, 900, 30), 300);
    EXPECT_EQ(c.width, 1920u);
    EXPECT_EQ(c.height, 1080u);
}

TEST(Negotiator, NeverDropsBelowTheAppsFrameRate) {
    // 1920x1440 only does 30; the app wants 4:3 at 60 -> no 4:3 format can, so fall back to the
    // largest format that does 60 rather than lowering the frame rate.
    const Choice c = negotiate(iphoneCaps(), app(1440, 1080, 60), 300);
    EXPECT_DOUBLE_EQ(c.fps, 60);
    EXPECT_EQ(c.width, 3840u);
}

TEST(Negotiator, HevcWhenH264DoesntFitTheLink) {
    // 4K60 H.264 wants ~107 Mbit/s; a 120 Mbit/s link has an 84 Mbit/s budget.
    const Choice c = negotiate(iphoneCaps(), app(3840, 2160, 60), 120);
    EXPECT_EQ(c.codec, "hevc");
    EXPECT_LE(c.bitrate, 84'000'000u);
    // The iPhone Air's measured 313 Mbit/s fits H.264 easily.
    EXPECT_EQ(negotiate(iphoneCaps(), app(3840, 2160, 60), 313).codec, "h264");
}

TEST(Negotiator, BitrateCappedByTheLinkEvenWithoutHevc) {
    PhoneCaps caps = iphoneCaps();
    caps.hevc = false;
    const Choice c = negotiate(caps, app(3840, 2160, 60), 50);
    EXPECT_EQ(c.codec, "h264");
    EXPECT_EQ(c.bitrate, 35'000'000u);
}

TEST(Negotiator, UnknownCapsFallsBackToTheR3Setup) {
    const Choice c = negotiate(PhoneCaps{}, app(1280, 720, 60), 300);
    EXPECT_EQ(c.width, 1920u);
    EXPECT_EQ(c.height, 1080u);
    EXPECT_DOUBLE_EQ(c.fps, 30);
}

TEST(Negotiator, BitrateTable) {
    EXPECT_NEAR(defaultBitrate(1920, 1080, 30) / 1e6, 25, 0.5);
    EXPECT_NEAR(defaultBitrate(1280, 720, 30) / 1e6, 15, 2);
    EXPECT_NEAR(defaultBitrate(3840, 2160, 30) / 1e6, 60, 7);
    EXPECT_NEAR(defaultBitrate(3840, 2160, 60) / 1e6, 100, 8);
}
