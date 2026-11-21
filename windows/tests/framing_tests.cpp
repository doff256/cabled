#include "framing.h"

#include <gtest/gtest.h>

using namespace cabled::engine;

namespace {

// Runs the framer for `seconds` at 30 fps with the same faces every frame.
// Critically damped springs approach their target as (1 + wt)e^-wt, so give them time to settle.
CropRect settle(Framer& f, const std::vector<Face>& faces, uint32_t srcW, uint32_t srcH, double& t,
                double seconds = 6.0, FramingSettings s = {}) {
    CropRect r;
    for (int i = 0; i < int(seconds * 30); ++i) {
        t += 1.0 / 30;
        r = f.update(faces, srcW, srcH, 1920, 1080, t, s);
    }
    return r;
}

}  // namespace

TEST(Framing, FillCropForLandscapeAndPortraitSources) {
    const CropRect same = fillCrop(3840, 2160, 1920, 1080);
    EXPECT_DOUBLE_EQ(same.w, 3840);
    EXPECT_DOUBLE_EQ(same.h, 2160);
    const CropRect portrait = fillCrop(2160, 3840, 1920, 1080);  // phone held upright
    EXPECT_DOUBLE_EQ(portrait.w, 2160);
    EXPECT_DOUBLE_EQ(portrait.h, 1215);
    EXPECT_DOUBLE_EQ(portrait.y, (3840 - 1215) / 2.0);
    const CropRect fourThree = fillCrop(4032, 3024, 1920, 1080);
    EXPECT_DOUBLE_EQ(fourThree.h, 2268);
}

TEST(Framing, DisabledGivesTheFullView) {
    Framer f;
    FramingSettings s;
    s.enabled = false;
    const CropRect r = f.update({{1, 0.4, 0.3, 0.1, 0.1}}, 3840, 2160, 1920, 1080, 1.0, s);
    EXPECT_DOUBLE_EQ(r.w, 3840);
}

TEST(Framing, ZoomsInOnAFaceWithHeadroomAndWithoutUpscaling) {
    Framer f;
    double t = 0;
    // A face 10% of the frame wide, a bit right of centre, upper third.
    const Face face{7, 0.55, 0.30, 0.10, 0.18};
    const CropRect r = settle(f, {face}, 3840, 2160, t);
    // Crop width = 3.2 x face width = 1229 px, but never below the 1920 px output (no upscaling).
    EXPECT_NEAR(r.w, 1920, 2);
    EXPECT_NEAR(r.w / r.h, 16.0 / 9.0, 0.01);
    // Face centred horizontally.
    EXPECT_NEAR(r.x + r.w / 2, (face.x + face.w / 2) * 3840, 40);
    // Top of the face ~18% down the crop.
    EXPECT_NEAR((face.y * 2160 - r.y) / r.h, 0.18, 0.03);
}

TEST(Framing, BigFaceGetsAWiderShot) {
    Framer f;
    double t = 0;
    const CropRect r = settle(f, {{1, 0.40, 0.20, 0.20, 0.35}}, 3840, 2160, t);
    EXPECT_NEAR(r.w, 0.20 * 3840 * 3.2, 25);
}

TEST(Framing, IgnoresSmallMovesButFollowsBigOnes) {
    Framer f;
    double t = 0;
    const CropRect a = settle(f, {{1, 0.50, 0.30, 0.10, 0.18}}, 3840, 2160, t);
    const CropRect b = settle(f, {{1, 0.51, 0.30, 0.10, 0.18}}, 3840, 2160, t);  // 38 px: within the dead zone
    EXPECT_NEAR(b.x, a.x, 1);
    const CropRect c = settle(f, {{1, 0.30, 0.30, 0.10, 0.18}}, 3840, 2160, t);  // walked left
    EXPECT_LT(c.x, a.x - 500);
}

TEST(Framing, MovesSmoothlyNotInJumps) {
    Framer f;
    double t = 0;
    settle(f, {{1, 0.70, 0.30, 0.10, 0.18}}, 3840, 2160, t);
    double previous = f.update({{1, 0.70, 0.30, 0.10, 0.18}}, 3840, 2160, 1920, 1080, t += 1.0 / 30, {}).x;
    double biggestStep = 0;
    for (int i = 0; i < 90; ++i) {  // subject jumps to the far left
        const double x = f.update({{1, 0.10, 0.30, 0.10, 0.18}}, 3840, 2160, 1920, 1080, t += 1.0 / 30, {}).x;
        biggestStep = std::max(biggestStep, std::abs(x - previous));
        previous = x;
    }
    EXPECT_LT(biggestStep, 150);  // px per frame: a glide, not a cut
}

TEST(Framing, HoldsThenReturnsToFullViewWhenTheFaceIsLost) {
    Framer f;
    double t = 0;
    const CropRect framed = settle(f, {{1, 0.50, 0.30, 0.10, 0.18}}, 3840, 2160, t);
    const CropRect held = settle(f, {}, 3840, 2160, t, 1.0);  // within holdSeconds
    EXPECT_NEAR(held.w, framed.w, 5);
    const CropRect back = settle(f, {}, 3840, 2160, t, 8.0);
    EXPECT_NEAR(back.w, 3840, 10);
}

TEST(Framing, MaxZoomAllowsTighterCrops) {
    Framer f;
    double t = 0;
    FramingSettings s;
    s.maxUpscale = 2.0;
    const CropRect r = settle(f, {{1, 0.50, 0.30, 0.10, 0.18}}, 3840, 2160, t, 6.0, s);
    EXPECT_NEAR(r.w, 0.10 * 3840 * 3.2, 3);  // 1229 px, upscaled to 1920
}

TEST(Framing, StaysInsideTheFrame) {
    Framer f;
    double t = 0;
    const CropRect r = settle(f, {{1, 0.95, 0.0, 0.05, 0.10}}, 3840, 2160, t);  // face at the top-right edge
    EXPECT_GE(r.x, 0);
    EXPECT_GE(r.y, 0);
    EXPECT_LE(r.x + r.w, 3840 + 1e-6);
    EXPECT_LE(r.y + r.h, 2160 + 1e-6);
}
