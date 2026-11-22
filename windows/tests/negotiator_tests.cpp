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

namespace {

PhoneCaps tripleCaps() {
    const nlohmann::json wide = {{{"width", 1920}, {"height", 1080}, {"maxFps", 60}},
                                 {{"width", 3840}, {"height", 2160}, {"maxFps", 60}}};
    const nlohmann::json front = {{{"width", 1920}, {"height", 1080}, {"maxFps", 30}}};
    return parseCaps({{"codecs", {"h264"}},
                      {"defaultCamera", "triple"},
                      {"cameras",
                       {{{"id", "triple"}, {"type", "triple"}, {"virtual", true}, {"minZoom", 1}, {"maxZoom", 30},
                         {"zoomScale", 0.5}, {"switchOver", {2, 6}}, {"formats", wide}},
                        {{"id", "front"}, {"type", "front"}, {"position", "front"}, {"formats", front}}}}});
}

}  // namespace

TEST(Negotiator, UsesTheChosenCamerasFormats) {
    const PhoneCaps caps = tripleCaps();
    Want w = app(1920, 1080, 60);
    EXPECT_EQ(negotiate(caps, w, 300).camera, "triple");  // default
    w.camera = "front";
    const Choice c = negotiate(caps, w, 300);
    EXPECT_EQ(c.camera, "front");
    EXPECT_DOUBLE_EQ(c.fps, 30);  // the front camera tops out at 30
    w.camera = "nonexistent";
    EXPECT_EQ(negotiate(caps, w, 300).camera, "triple");
    EXPECT_EQ(c.toConfigure(1.5)["camera"], "front");
    EXPECT_EQ(c.toConfigure(1.5)["zoom"], 1.5);
}

TEST(Negotiator, CameraLabelsAndZoomPresets) {
    const PhoneCaps caps = tripleCaps();
    const CameraInfo* triple = caps.find("triple");
    ASSERT_TRUE(triple);
    EXPECT_EQ(triple->label(), "Auto");
    EXPECT_DOUBLE_EQ(triple->minDisplay(), 0.5);
    EXPECT_DOUBLE_EQ(triple->maxDisplay(), 15);
    EXPECT_EQ(triple->presets(), (std::vector<double>{0.5, 1, 2, 3}));
    EXPECT_EQ(caps.find("front")->label(), "Front");
    // TrueDepth front cameras are "virtual" too; they must still read "Front".
    const PhoneCaps trueDepth = parseCaps(
        {{"cameras", {{{"id", "td"}, {"type", "front"}, {"position", "front"}, {"virtual", true}}}}});
    EXPECT_EQ(trueDepth.find("td")->label(), "Front");
    EXPECT_EQ(caps.find("front")->presets(), (std::vector<double>{1}));
}

TEST(Negotiator, FramingAsksForTwiceTheOutput) {
    Want w = app(1920, 1080, 30);
    EXPECT_EQ(negotiate(iphoneCaps(), w, 300).width, 1920u);
    w.framing = true;
    const Choice c = negotiate(iphoneCaps(), w, 300);
    EXPECT_EQ(c.width, 3840u);
    EXPECT_EQ(c.height, 2160u);
    // 4K output + framing: nothing covers 7680x4320, so the largest format at that rate.
    w = app(3840, 2160, 30);
    w.framing = true;
    EXPECT_EQ(negotiate(iphoneCaps(), w, 300).width, 4032u);
}

TEST(Negotiator, PinnedCaptureOverridesAutomatic) {
    Want w = app(1920, 1080, 30);
    w.framing = true;  // would ask for 4K...
    w.pinWidth = 1280;
    w.pinHeight = 720;
    w.pinFps = 60;
    const Choice c = negotiate(iphoneCaps(), w, 300);
    EXPECT_EQ(c.width, 1280u);  // ...but the user pinned 720p60
    EXPECT_DOUBLE_EQ(c.fps, 60);
}

TEST(Negotiator, HotPhoneStepsDown) {
    Want w = app(1920, 1080, 60);
    w.framing = true;  // 4K60 normally
    EXPECT_EQ(negotiate(iphoneCaps(), w, 300).width, 3840u);
    w.thermal = "serious";
    Choice c = negotiate(iphoneCaps(), w, 300);
    EXPECT_EQ(c.width, 1920u);
    EXPECT_DOUBLE_EQ(c.fps, 30);
    w.thermal = "critical";
    c = negotiate(iphoneCaps(), w, 300);
    EXPECT_EQ(c.width, 1280u);
    EXPECT_DOUBLE_EQ(c.fps, 30);
}

TEST(Negotiator, BitrateTable) {
    EXPECT_NEAR(defaultBitrate(1920, 1080, 30) / 1e6, 25, 0.5);
    EXPECT_NEAR(defaultBitrate(1280, 720, 30) / 1e6, 15, 2);
    EXPECT_NEAR(defaultBitrate(3840, 2160, 30) / 1e6, 60, 7);
    EXPECT_NEAR(defaultBitrate(3840, 2160, 60) / 1e6, 100, 8);
}

// R13: a phone hovering at the edge mustn't flip the capture back and forth.
TEST(Negotiator, ThermalGovernorStepsDownAtOnceAndUpSlowly) {
    ThermalGovernor g(60);
    EXPECT_EQ(g.update("fair", 0), "fair");
    EXPECT_EQ(g.update("serious", 1), "serious");     // hotter: immediately
    EXPECT_EQ(g.update("fair", 2), "serious");        // cooler: wait
    EXPECT_EQ(g.update("serious", 30), "serious");    // back up: the clock restarts
    EXPECT_EQ(g.update("fair", 31), "serious");
    EXPECT_EQ(g.update("fair", 90), "serious");       // 59 s cool
    EXPECT_EQ(g.update("nominal", 92), "nominal");    // 61 s cool: step back up
    EXPECT_EQ(g.update("critical", 93), "critical");
    EXPECT_EQ(thermalLevel("critical"), 3);
    EXPECT_EQ(thermalLevel(""), 0);
}
