// The engine end to end against the fake phone: CONFIGURE -> H.264 over TCP -> FFmpeg decode.
// Uses windows/testdata/pattern_1080p30.h264, whose frames carry their frame number in the stamp
// band, so we can check every decoded picture arrived, in order, intact.

#include "fake_phone.h"
#include "frame_convert.h"
#include "net.h"
#include "session.h"
#include "test_pattern.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace cabled;

namespace {

const std::string kClip = std::string(CABLED_TESTDATA_DIR) + "/pattern_1080p30.h264";

struct Collected {
    engine::SessionStats stats;
    std::vector<uint32_t> stampFrames;  // frame number read back from each decoded picture
    uint32_t width = 0, height = 0;
};

Collected run(const fakephone::Options& phone, bool hardware, double seconds) {
    net::WinsockSession winsock;
    Collected c;
    std::string err;
    auto listener = net::Socket::listenTcp("127.0.0.1", 0, &err);
    EXPECT_TRUE(listener) << err;
    if (!listener) return c;
    const uint16_t port = listener->localPort();
    std::thread phoneThread([&] {
        auto client = listener->accept(nullptr);
        if (client) fakephone::serveClient(std::move(*client), phone);
    });

    auto socket = net::Socket::connectTcp("127.0.0.1", port, &err);
    EXPECT_TRUE(socket) << err;
    if (socket) {
        engine::SessionOptions o;
        o.hardwareDecode = hardware;
        o.seconds = seconds;
        engine::FrameConverter converter;
        std::vector<uint8_t> nv12;
        c.stats = engine::runSession(
            *socket, o,
            [&](const AVFrame* frame, const engine::FrameInfo&) {
                c.width = uint32_t(frame->width);
                c.height = uint32_t(frame->height);
                nv12.resize(vcam::packedFrameBytes(vcam::kFourccNV12, c.width, c.height));
                const auto view = vcam::packedView(vcam::kFourccNV12, c.width, c.height, nv12.data());
                if (converter.convert(frame, view)) c.stampFrames.push_back(vcam::readStamp(view).frame);
            },
            nullptr, nullptr);
    }
    socket.reset();
    phoneThread.join();
    return c;
}

void expectConsecutive(const std::vector<uint32_t>& frames, uint32_t clipLength) {
    ASSERT_FALSE(frames.empty());
    for (size_t i = 1; i < frames.size(); ++i) {
        const uint32_t expected = frames[i - 1] == clipLength ? 1 : frames[i - 1] + 1;  // the clip loops
        ASSERT_EQ(frames[i], expected) << "at decoded frame " << i;
    }
}

}  // namespace

TEST(Engine, DecodesFakePhoneStreamInOrder_Software) {
    std::string err;
    fakephone::Options phone;
    phone.clip = fakephone::loadClip(kClip, 1920, 1080, 30, &err);
    ASSERT_TRUE(phone.clip) << err;

    const Collected c = run(phone, /*hardware=*/false, 2.0);
    EXPECT_TRUE(c.stats.error.empty()) << c.stats.error;
    EXPECT_TRUE(c.stats.configured.value("ok", false));
    EXPECT_EQ(c.width, 1920u);
    EXPECT_EQ(c.height, 1080u);
    EXPECT_GE(c.stats.framesDecoded, 45u);  // ~60 expected in 2 s at 30 fps
    EXPECT_EQ(c.stats.decodeErrors, 0u);
    EXPECT_EQ(c.stats.sequenceGaps, 0u);
    EXPECT_EQ(c.stampFrames.front(), 1u);  // starts on the clip's first (key)frame
    expectConsecutive(c.stampFrames, uint32_t(phone.clip->units.size()));
}

TEST(Engine, DecodesWithHardwareWhenAvailable) {
    std::string err;
    fakephone::Options phone;
    phone.clip = fakephone::loadClip(kClip, 1920, 1080, 30, &err);
    ASSERT_TRUE(phone.clip) << err;

    const Collected c = run(phone, /*hardware=*/true, 1.5);
    EXPECT_TRUE(c.stats.error.empty()) << c.stats.error;
    EXPECT_GE(c.stats.framesDecoded, 30u);
    EXPECT_EQ(c.stats.decodeErrors, 0u);
    expectConsecutive(c.stampFrames, uint32_t(phone.clip->units.size()));
    // CI machines have no GPU decoder; then FFmpeg falls back to software and this still passes.
    std::printf("hardware decode active: %s\n", c.stats.hardwareDecode ? "yes" : "no");
}

// A frame lost in transit (F9): the engine must notice the sequence gap, ask for a keyframe, skip
// the broken GOP and resume cleanly, never showing a corrupted picture.
TEST(Engine, RecoversFromLostFramesWithKeyframeRequest) {
    std::string err;
    fakephone::Options phone;
    phone.clip = fakephone::loadClip(kClip, 1920, 1080, 30, &err);
    ASSERT_TRUE(phone.clip) << err;
    phone.dropEveryFrames = 20;

    const Collected c = run(phone, /*hardware=*/false, 3.0);
    EXPECT_TRUE(c.stats.error.empty()) << c.stats.error;
    EXPECT_GE(c.stats.sequenceGaps, 2u);
    EXPECT_GE(c.stats.keyframeRequests, 2u);
    EXPECT_EQ(c.stats.decodeErrors, 0u);
    EXPECT_GT(c.stats.framesDroppedWaitingForKeyframe, 0u);
    EXPECT_GE(c.stats.framesDecoded, 30u);
    // Every picture we did show is intact: its stamp decodes to a real clip frame number.
    for (uint32_t f : c.stampFrames) {
        EXPECT_GE(f, 1u);
        EXPECT_LE(f, uint32_t(phone.clip->units.size()));
    }
}

TEST(Engine, ReportsRefusedConfigure) {
    fakephone::Options phone;  // no clip: the fake phone answers CONFIGURED ok=false
    const Collected c = run(phone, false, 2.0);
    EXPECT_NE(c.stats.error.find("refused CONFIGURE"), std::string::npos) << c.stats.error;
}
