// The engine end to end against the fake phone over TCP: decode correctness, recovery, reconnects,
// pauses and the watchdog. Uses windows/testdata/pattern_1080p30.h264, whose frames carry their
// frame number in the pixels, so we can check every decoded picture arrived in order and intact.

#include "engine.h"
#include "fake_phone.h"
#include "frame_convert.h"
#include "net.h"
#include "status_card.h"
#include "test_pattern.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace cabled;
using namespace std::chrono_literals;

namespace {

const std::string kClip = std::string(CABLED_TESTDATA_DIR) + "/pattern_1080p30.h264";

// A fake phone that keeps accepting connections (one at a time) until destroyed.
class FakePhoneServer {
public:
    explicit FakePhoneServer(fakephone::Options options) : options_(std::move(options)) {
        std::string err;
        auto l = net::Socket::listenTcp("127.0.0.1", 0, &err);
        EXPECT_TRUE(l) << err;
        if (!l) return;
        listener_ = std::move(*l);
        port_ = listener_.localPort();
        thread_ = std::thread([this] {
            while (!stop_) {
                auto client = listener_.accept(nullptr);
                if (!client) return;
                ++connections_;
                fakephone::serveClient(std::move(*client), options_);
            }
        });
    }
    ~FakePhoneServer() {
        stop_ = true;
        listener_.abort();
        if (thread_.joinable()) thread_.join();
    }
    std::string target() const { return "127.0.0.1:" + std::to_string(port_); }
    int connections() const { return connections_; }

private:
    fakephone::Options options_;
    net::WinsockSession winsock_;
    net::Socket listener_;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> connections_{0};
};

fakephone::Options phoneWithClip() {
    fakephone::Options o;
    std::string err;
    o.clip = fakephone::loadClip(kClip, 1920, 1080, 30, &err);
    EXPECT_TRUE(o.clip) << err;
    return o;
}

struct Recorder {
    std::mutex mutex;
    std::vector<uint32_t> stampFrames;
    std::set<engine::EngineState> statesSeen;
    engine::FrameConverter converter;
    std::vector<uint8_t> nv12;

    void onFrame(const AVFrame* f) {
        const uint32_t w = uint32_t(f->width), h = uint32_t(f->height);
        std::lock_guard<std::mutex> lock(mutex);
        nv12.resize(vcam::packedFrameBytes(vcam::kFourccNV12, w, h));
        const auto view = vcam::packedView(vcam::kFourccNV12, w, h, nv12.data());
        if (converter.convert(f, view)) stampFrames.push_back(vcam::readStamp(view).frame);
    }
    size_t frames() {
        std::lock_guard<std::mutex> lock(mutex);
        return stampFrames.size();
    }
};

engine::EngineConfig configFor(const FakePhoneServer& phone, Recorder& rec, bool hardware = false) {
    engine::EngineConfig c;
    c.tcpTarget = phone.target();
    c.hardwareDecode = hardware;
    c.virtualCamera = false;  // tests run without Frame Server
    c.previewMaxWidth = 320;
    c.retrySeconds = 0.2;
    c.frameSink = [&rec](const AVFrame* f, uint32_t) { rec.onFrame(f); };
    return c;
}

// Runs the engine for `seconds`, sampling its state every 50 ms.
void runFor(engine::Engine& e, Recorder& rec, double seconds) {
    e.start();
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        rec.statesSeen.insert(e.status().state);
        std::this_thread::sleep_for(50ms);
    }
    e.stop();
}

void expectConsecutive(const std::vector<uint32_t>& frames, uint32_t clipLength) {
    ASSERT_FALSE(frames.empty());
    for (size_t i = 1; i < frames.size(); ++i) {
        const uint32_t expected = frames[i - 1] == clipLength ? 1 : frames[i - 1] + 1;  // the clip loops
        ASSERT_EQ(frames[i], expected) << "at decoded frame " << i;
    }
}

}  // namespace

TEST(Engine, DecodesFakePhoneStreamInOrder) {
    auto phoneOptions = phoneWithClip();
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    runFor(e, rec, 2.5);

    const auto s = e.status();
    EXPECT_GE(rec.frames(), 50u);  // ~70 expected
    EXPECT_EQ(rec.stampFrames.front(), 1u);  // started on the clip's first (key)frame
    expectConsecutive(rec.stampFrames, uint32_t(phoneOptions.clip->units.size()));
    EXPECT_TRUE(rec.statesSeen.count(engine::EngineState::Streaming));
    EXPECT_EQ(s.state, engine::EngineState::Stopped);
    EXPECT_EQ(s.deviceModel, "FakePhone1,1");
    EXPECT_EQ(s.keyframeRequests, 0u);

    // Preview frames were produced at the requested width.
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    uint64_t seq = 0;
    ASSERT_TRUE(e.copyPreview(bgra, w, h, seq));
    EXPECT_EQ(w, 320u);
    EXPECT_EQ(h, 180u);
    EXPECT_EQ(bgra[3], 255);  // opaque
}

TEST(Engine, HardwareDecodeWhenAvailable) {
    auto phoneOptions = phoneWithClip();
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec, /*hardware=*/true));
    runFor(e, rec, 2.0);
    EXPECT_GE(rec.frames(), 35u);
    expectConsecutive(rec.stampFrames, uint32_t(phoneOptions.clip->units.size()));
    // CI has no GPU decoder; FFmpeg then falls back to software and this still passes.
    std::printf("hardware decode active: %s\n", e.status().hardwareDecode ? "yes" : "no");
}

// F9: lost frames -> keyframe request -> skip the broken GOP -> clean resume.
TEST(Engine, RecoversFromLostFrames) {
    auto phoneOptions = phoneWithClip();
    phoneOptions.dropEveryFrames = 20;
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    runFor(e, rec, 3.0);
    EXPECT_GE(e.status().keyframeRequests, 2u);
    EXPECT_GE(rec.frames(), 30u);
    std::lock_guard<std::mutex> lock(rec.mutex);
    for (uint32_t f : rec.stampFrames) {  // every picture shown is intact
        EXPECT_GE(f, 1u);
        EXPECT_LE(f, uint32_t(phoneOptions.clip->units.size()));
    }
}

// F1: the connection drops; the engine reconnects by itself and video continues.
TEST(Engine, ReconnectsAfterDisconnect) {
    auto phoneOptions = phoneWithClip();
    phoneOptions.disconnectAfterSeconds = 1.0;
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    e.start();
    std::this_thread::sleep_for(1500ms);
    const size_t before = rec.frames();
    std::this_thread::sleep_for(1300ms);  // reconnects at ~1.2 s, then disconnects again at ~2.2 s
    const size_t after = rec.frames();
    const auto s = e.status();
    e.stop();
    EXPECT_GE(phone.connections(), 2);
    EXPECT_GE(s.reconnects, 1u);
    EXPECT_GT(after, before + 10) << "video should continue after reconnecting";
}

// F5: the phone reports its camera is interrupted; the engine shows Paused, then resumes.
TEST(Engine, PausesAndResumes) {
    auto phoneOptions = phoneWithClip();
    phoneOptions.pauseAfterSeconds = 0.8;
    phoneOptions.pauseForSeconds = 1.0;
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    e.start();
    bool sawPaused = false;
    std::string pausedHeadline;
    const auto end = std::chrono::steady_clock::now() + 3500ms;
    while (std::chrono::steady_clock::now() < end) {
        const auto s = e.status();
        if (s.state == engine::EngineState::Paused) {
            sawPaused = true;
            pausedHeadline = s.headline;
        }
        std::this_thread::sleep_for(50ms);
    }
    const auto final = e.status();
    e.stop();
    EXPECT_TRUE(sawPaused);
    EXPECT_NE(pausedHeadline.find("background"), std::string::npos) << pausedHeadline;
    EXPECT_EQ(final.state, engine::EngineState::Streaming) << "should have resumed";
    EXPECT_EQ(final.reconnects, 0u) << "a pause must not drop the connection";
}

// A hung phone (socket open, nothing arriving): the watchdog drops it and reconnects.
TEST(Engine, WatchdogDropsSilentPhone) {
    auto phoneOptions = phoneWithClip();
    phoneOptions.freezeAfterSeconds = 0.8;
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    auto config = configFor(phone, rec);
    config.watchdogSeconds = 1.0;
    engine::Engine e(config);
    e.start();
    std::this_thread::sleep_for(3500ms);
    const auto s = e.status();
    e.stop();
    EXPECT_GE(s.reconnects, 1u);
    EXPECT_GE(phone.connections(), 2);
}

TEST(Engine, ReportsRefusedConfigure) {
    FakePhoneServer phone(fakephone::Options{});  // no clip: CONFIGURED ok=false
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    e.start();
    std::this_thread::sleep_for(1500ms);  // handshake + 0.5 s link probe + CONFIGURE
    const auto s = e.status();
    e.stop();
    EXPECT_EQ(s.state, engine::EngineState::Error);
    EXPECT_NE(s.headline.find("refused"), std::string::npos) << s.headline;
}

TEST(Engine, WaitsWhileNobodyAnswers) {
    net::WinsockSession winsock;
    uint16_t port = 0;
    {
        auto l = net::Socket::listenTcp("127.0.0.1", 0, nullptr);
        ASSERT_TRUE(l);
        port = l->localPort();
    }
    engine::EngineConfig c;
    c.tcpTarget = "127.0.0.1:" + std::to_string(port);
    c.virtualCamera = false;
    c.retrySeconds = 0.1;
    engine::Engine e(c);
    e.start();
    // Windows takes ~2 s to report "refused" for a localhost port nobody listens on.
    std::this_thread::sleep_for(3000ms);
    EXPECT_EQ(e.status().state, engine::EngineState::WaitingForCompanion);
    e.stop();
    EXPECT_EQ(e.status().state, engine::EngineState::Stopped);
}

// R5: CAPS + link probe feed the first CONFIGURE.
TEST(Engine, NegotiatesFromCapsAndProbe) {
    auto phoneOptions = phoneWithClip();
    std::mutex m;
    std::vector<nlohmann::json> configures;
    phoneOptions.onConfigure = [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m);
        configures.push_back(j);
    };
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    runFor(e, rec, 2.0);
    const auto s = e.status();
    std::lock_guard<std::mutex> lock(m);
    ASSERT_FALSE(configures.empty());
    EXPECT_TRUE(configures[0].value("enabled", false));
    // Auto-framing is on by default (R-42): 1080p output -> ask for 4K so the crop stays sharp.
    EXPECT_EQ(configures[0].value("width", 0), 3840);
    EXPECT_EQ(configures[0].value("height", 0), 2160);
    EXPECT_EQ(configures[0].value("codec", ""), "h264");  // loopback link is far faster than needed
    EXPECT_GT(s.linkMbps, 100.0);
    EXPECT_NE(s.videoConfig.find("3840x2160"), std::string::npos) << s.videoConfig;
    EXPECT_GE(rec.frames(), 20u);
}

// R5: nobody watching (no camera app, no preview) -> ask the phone to stop video.
TEST(Engine, TurnsVideoOffWhenNobodyWatches) {
    auto phoneOptions = phoneWithClip();
    std::mutex m;
    std::vector<nlohmann::json> configures;
    phoneOptions.onConfigure = [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m);
        configures.push_back(j);
    };
    FakePhoneServer phone(phoneOptions);
    engine::EngineConfig c;
    c.tcpTarget = phone.target();
    c.virtualCamera = false;
    c.previewMaxWidth = 0;  // no preview, no sink
    engine::Engine e(c);
    e.start();
    std::this_thread::sleep_for(1500ms);
    const auto s = e.status();
    e.stop();
    std::lock_guard<std::mutex> lock(m);
    ASSERT_FALSE(configures.empty());
    EXPECT_FALSE(configures[0].value("enabled", true));
    EXPECT_EQ(s.state, engine::EngineState::Idle);
    EXPECT_EQ(e.status().framesDecoded, 0u);
}

// R6: camera list in the status; setZoom converts display units to the phone's factor; setCamera
// reconfigures at once.
TEST(Engine, CamerasAndZoom) {
    auto phoneOptions = phoneWithClip();
    std::mutex m;
    std::vector<nlohmann::json> configures, controls;
    phoneOptions.onConfigure = [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m);
        configures.push_back(j);
    };
    phoneOptions.onControlSet = [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m);
        controls.push_back(j);
    };
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    e.start();
    std::this_thread::sleep_for(1500ms);

    auto s = e.status();
    ASSERT_EQ(s.cameras.size(), 2u);
    EXPECT_EQ(s.cameras[0]["label"], "Auto");
    EXPECT_EQ(s.camera, "fake-triple");

    e.setZoom(3.0, true);  // 3x display on a 0.5-scale camera = factor 6
    std::this_thread::sleep_for(300ms);
    s = e.status();
    EXPECT_DOUBLE_EQ(s.zoom, 3.0);  // echoed back through CONTROL_STATE

    e.setCamera("fake-front");
    std::this_thread::sleep_for(500ms);
    e.stop();
    std::lock_guard<std::mutex> lock(m);
    ASSERT_FALSE(controls.empty());
    EXPECT_DOUBLE_EQ(controls.back().value("zoom", 0.0), 6.0);
    EXPECT_GT(controls.back().value("ramp", 0.0), 0.0);
    // (The very last CONFIGURE is the {enabled:false} sent when the engine stops.)
    bool switched = false;
    for (const auto& c : configures) switched |= c.value("camera", "") == "fake-front";
    EXPECT_TRUE(switched);
}

// R7: faces in VIDEO metadata -> the framer crops in; turning framing off goes back to the full view.
TEST(Engine, AutoFramingCropsToTheFace) {
    auto phoneOptions = phoneWithClip();
    phoneOptions.videoMeta = R"({"faces":[{"id":1,"x":0.45,"y":0.25,"w":0.06,"h":0.11}]})";
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    auto config = configFor(phone, rec);
    config.previewMaxWidth = 1920;  // preview = the crop at full resolution
    engine::Engine e(config);
    e.start();
    std::this_thread::sleep_for(3500ms);  // let the springs settle
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    uint64_t seq = 0;
    ASSERT_TRUE(e.copyPreview(bgra, w, h, seq));
    EXPECT_EQ(e.status().faces, 1);
    // A 1080p source can't be cropped without upscaling (maxUpscale 1), so the crop stays at the
    // full 1920 width: framing correctly refuses to zoom a 1080p source.
    EXPECT_EQ(w, 1920u);

    e.setFraming(false);
    std::this_thread::sleep_for(500ms);
    EXPECT_FALSE(e.status().framing);
    e.stop();
}

// R8: camera controls are remembered and re-applied after a camera switch; taps become full-frame
// focus points.
TEST(Engine, ControlsSurviveCameraSwitchAndTapMapsToFrame) {
    auto phoneOptions = phoneWithClip();
    std::mutex m;
    std::vector<nlohmann::json> controls;
    phoneOptions.onControlSet = [&](const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m);
        controls.push_back(j);
    };
    FakePhoneServer phone(phoneOptions);
    Recorder rec;
    engine::Engine e(configFor(phone, rec));
    e.start();
    std::this_thread::sleep_for(1500ms);
    e.setControls({{"exposure", {{"mode", "auto"}, {"bias", 0.7}}}});
    e.setCamera("fake-front");
    std::this_thread::sleep_for(500ms);
    e.tapToFocus(0.5, 0.5);
    std::this_thread::sleep_for(300ms);
    e.stop();

    std::lock_guard<std::mutex> lock(m);
    int exposureSets = 0;
    for (const auto& c : controls) {
        if (c.contains("exposure") && c["exposure"].value("bias", 0.0) == 0.7) ++exposureSets;
    }
    EXPECT_GE(exposureSets, 2) << "sent once by setControls, again after the camera switch";
    ASSERT_FALSE(controls.empty());
    const auto& tap = controls.back();
    ASSERT_TRUE(tap.contains("focus"));
    EXPECT_NEAR(tap["focus"]["point"][0].get<double>(), 0.5, 0.01);  // no faces: crop = whole frame
    EXPECT_NEAR(tap["focus"]["point"][1].get<double>(), 0.5, 0.01);
}

TEST(StatusCard, RendersReadableText) {
    const auto card = engine::renderStatusCard(1280, 720, L"Open Cabled on your iPhone", L"Keep it on screen.");
    ASSERT_EQ(card.bgra.size(), size_t(1280) * 720 * 4);
    size_t bright = 0;
    for (size_t i = 0; i < card.bgra.size(); i += 4) {
        if (card.bgra[i + 1] > 200) ++bright;  // near-white text pixels
        ASSERT_EQ(card.bgra[i + 3], 255);
    }
    EXPECT_GT(bright, 1000u);
}

TEST(StatusCard, LongHeadlineFitsSmallFrames) {
    const uint32_t w = 640, h = 360;
    const auto card = engine::renderStatusCard(w, h, L"Cabled is in the background on your iPhone", L"Open it again.");
    // Nothing bright in the outer 5% on either side: the headline shrank to fit.
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x : {0u, 10u, 20u, w - 21, w - 11, w - 1}) {
            ASSERT_LT(card.bgra[(size_t(y) * w + x) * 4 + 1], 150) << "text reaches x=" << x << " y=" << y;
        }
    }
}
