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

TEST(Framing, ZoomsInOnAFaceWithHeadroom) {
    Framer f;
    double t = 0;
    // A face 10% of the frame wide, a bit right of centre, upper third.
    const Face face{7, 0.55, 0.30, 0.10, 0.18};
    const CropRect r = settle(f, {face}, 3840, 2160, t);
    // Default zoom 2x: half the 3840 px width.
    EXPECT_NEAR(r.w, 1920, 2);
    EXPECT_NEAR(r.w / r.h, 16.0 / 9.0, 0.01);
    // Face centred horizontally.
    EXPECT_NEAR(r.x + r.w / 2, (face.x + face.w / 2) * 3840, 40);
    // Top of the face ~18% down the crop.
    EXPECT_NEAR((face.y * 2160 - r.y) / r.h, 0.18, 0.03);
}

// Owner feedback on R8: the zoom is what the slider says, not something the framer changes.
TEST(Framing, KeepsTheZoomTheUserSetWhateverTheFaceSize) {
    Framer a, b;
    double ta = 0, tb = 0;
    const CropRect nearby = settle(a, {{1, 0.40, 0.20, 0.20, 0.35}}, 3840, 2160, ta);
    const CropRect far = settle(b, {{1, 0.45, 0.30, 0.05, 0.09}}, 3840, 2160, tb);
    EXPECT_NEAR(nearby.w, 1920, 2);
    EXPECT_NEAR(far.w, 1920, 2);
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
    EXPECT_LT(biggestStep, 300);  // px per frame: a glide, not a cut (the whole move is ~2300 px)
}

TEST(Framing, SliderGoesFromWholeViewToClose) {
    FramingSettings wide, close;
    wide.setTightness(0);
    close.setTightness(1);
    Framer a, b;
    double ta = 0, tb = 0;
    const std::vector<Face> face = {{1, 0.45, 0.30, 0.08, 0.15}};
    const CropRect rw = settle(a, face, 3840, 2160, ta, 6.0, wide);
    const CropRect rc = settle(b, face, 3840, 2160, tb, 6.0, close);
    EXPECT_NEAR(rw.w, 3840, 2);  // the whole view
    EXPECT_NEAR(rc.w, 1280, 2);  // 3x
    EXPECT_DOUBLE_EQ(close.tightness(), 1.0);
}

TEST(Framing, SliderWorksWithNobodyInShot) {
    FramingSettings s;
    s.setTightness(1);
    Framer f;
    double t = 0;
    EXPECT_NEAR(settle(f, {}, 3840, 2160, t, 6.0, s).w, 1280, 2);
}

TEST(Framing, GroupFramingFitsEveryone) {
    FramingSettings s;
    s.group = true;
    Framer f;
    double t = 0;
    const std::vector<Face> two = {{1, 0.15, 0.30, 0.08, 0.15}, {2, 0.70, 0.32, 0.08, 0.15}};
    const CropRect r = settle(f, two, 3840, 2160, t, 6.0, s);
    for (const Face& face : two) {  // both faces fully inside the crop
        EXPECT_LE(r.x, face.x * 3840);
        EXPECT_GE(r.x + r.w, (face.x + face.w) * 3840);
    }
    // Single-person framing would follow just one of them.
    Framer single;
    double t2 = 0;
    const CropRect one = settle(single, two, 3840, 2160, t2);
    EXPECT_LT(one.w, r.w);
}

TEST(Framing, StaysPutWhenTheFaceIsLost) {
    Framer f;
    double t = 0;
    const CropRect framed = settle(f, {{1, 0.70, 0.30, 0.10, 0.18}}, 3840, 2160, t);
    const CropRect later = settle(f, {}, 3840, 2160, t, 8.0);
    EXPECT_NEAR(later.w, framed.w, 2);
    EXPECT_NEAR(later.x, framed.x, 2);
}

TEST(Framing, QualityGuardLimitsTheZoom) {
    FramingSettings s;
    s.setTightness(1);  // 3x asked for
    Framer f;
    double t = 0;
    // 1080p capture for 1080p output: at most 1.5x (upscaled) before it gets too soft.
    const CropRect r = settle(f, {{1, 0.50, 0.30, 0.10, 0.18}}, 1920, 1080, t, 6.0, s);
    EXPECT_NEAR(r.w, 1280, 2);
    EXPECT_NEAR(reachableZoom(1920, 1080, 1920, 1080, s), 1.5, 1e-9);
    EXPECT_NEAR(reachableZoom(3840, 2160, 1920, 1080, s), 3.0, 1e-9);
}

TEST(Framing, ResolutionChangeKeepsTheFraming) {
    Framer f;
    double t = 0;
    const CropRect before = settle(f, {{1, 0.70, 0.30, 0.10, 0.18}}, 3840, 2160, t);
    // Capture drops to 1080p: the very next frame is still looking at the same spot.
    const CropRect after = f.update({{1, 0.70, 0.30, 0.10, 0.18}}, 1920, 1080, 1920, 1080, t += 1.0 / 30, {});
    EXPECT_NEAR(after.x + after.w / 2, (before.x + before.w / 2) / 2, 30);
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
