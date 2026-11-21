#include "session.h"

#include "decoder.h"
#include "product.h"
#include "protocol.h"
#include "video.h"

#include <chrono>

namespace cabled::engine {
namespace {

using proto::Type;

struct ClockSync {
    uint64_t bestRtt = UINT64_MAX;
    int64_t offsetNs = 0;  // phone - pc
    uint64_t lastPingNs = 0;

    void onPong(const proto::Pong& pong, uint64_t receivedNs) {
        const uint64_t rtt = receivedNs - pong.pcTimeNs;
        // Prefer the fastest round trip, but let the estimate age so drift is followed.
        if (rtt <= bestRtt + 200'000) {
            bestRtt = rtt;
            offsetNs = int64_t(pong.phoneTimeNs) - int64_t(pong.pcTimeNs + rtt / 2);
        }
    }
};

}  // namespace

SessionStats runSession(net::Socket& s, const SessionOptions& o, const FrameSink& sink, const std::atomic<bool>* stop,
                        const std::function<void(const SessionStats&)>& onSecond) {
    SessionStats st;
    auto log = [&](const std::string& line) {
        if (o.log) o.log(line);
    };
    auto fail = [&](std::string why) {
        st.error = std::move(why);
        return st;
    };

    s.setNoDelay(true);
    s.setBufferSizes(4 << 20);
    s.setRecvTimeout(5000);  // the phone sends 30+ messages a second while streaming

    // --- Handshake
    const nlohmann::json hello = {
        {"protocol", proto::versionString()}, {"role", "pc"}, {"app", "cabled-engine"}, {"appVersion", CABLED_VERSION}};
    if (!proto::writeJson(s, Type::Hello, hello)) return fail("failed to send HELLO");
    proto::Message m;
    std::string err;
    while (true) {
        if (!proto::readMessage(s, m, &err)) return fail("no HELLO from the phone: " + err);
        if (m.type() == Type::Hello) break;
    }
    st.peerHello = proto::parseJson(m).value_or(nlohmann::json::object());
    const std::string peerVersion = st.peerHello.value("protocol", "");
    if (!proto::isCompatible(peerVersion)) {
        return fail("incompatible protocol: phone " + peerVersion + ", PC " + proto::versionString());
    }
    log("Phone: " + st.peerHello.dump());

    // --- Clock sync: a burst of pings before video starts.
    ClockSync clock;
    for (int i = 0; i < 10; ++i) {
        const uint64_t t0 = proto::nowNs();
        const auto ping = proto::encodePing(t0);
        if (!proto::writeMessage(s, Type::Ping, 0, ping.data(), ping.size())) return fail("failed to send PING");
        while (true) {
            if (!proto::readMessage(s, m, &err)) return fail("no PONG: " + err);
            if (m.type() == Type::Pong) break;
            if (m.type() == Type::Log) log("[phone] " + std::string(m.text()));
        }
        if (auto pong = proto::decodePong(m)) clock.onPong(*pong, proto::nowNs());
    }
    clock.lastPingNs = proto::nowNs();
    st.clockOffsetNs = clock.offsetNs;
    st.rttMs = double(clock.bestRtt) / 1e6;
    log("Clock offset " + std::to_string(double(clock.offsetNs) / 1e6) + " ms, best RTT " +
        std::to_string(st.rttMs) + " ms");

    // --- Decoder
    Decoder decoder;
    if (!decoder.open(o.hardwareDecode, &err)) return fail("decoder: " + err);

    // --- Configure
    const nlohmann::json configure = {{"enabled", true},  {"codec", "h264"},     {"width", o.width},
                                      {"height", o.height}, {"fps", o.fps},      {"bitrate", o.bitrate},
                                      {"keyframeInterval", 2.0}};
    if (!proto::writeJson(s, Type::Configure, configure)) return fail("failed to send CONFIGURE");

    // --- Receive loop
    const auto start = std::chrono::steady_clock::now();
    auto windowStart = start;
    uint32_t lastSeq = 0;
    bool waitingForKeyframe = true;  // never decode from the middle of a GOP
    std::vector<uint8_t> annexB;

    auto requestKeyframe = [&](const char* why) {
        if (!waitingForKeyframe) log(std::string("Requesting keyframe: ") + why);
        waitingForKeyframe = true;
        ++st.keyframeRequests;
        proto::writeMessage(s, Type::KeyframeRequest, 0, nullptr, 0);
        decoder.flush();
    };

    while (!(stop && stop->load())) {
        const auto now = std::chrono::steady_clock::now();
        if (o.seconds > 0 && std::chrono::duration<double>(now - start).count() >= o.seconds) break;

        // About once a second: refresh the clock and report.
        if (now - windowStart >= std::chrono::seconds(1)) {
            const uint64_t t0 = proto::nowNs();
            const auto ping = proto::encodePing(t0);
            proto::writeMessage(s, Type::Ping, 0, ping.data(), ping.size());
            st.windowSeconds = std::chrono::duration<double>(now - windowStart).count();
            st.hardwareDecode = decoder.hardwareActive();
            st.clockOffsetNs = clock.offsetNs;
            if (onSecond) onSecond(st);
            st.windowFrames = st.windowBytes = 0;
            st.windowDecodeMs.clear();
            st.windowLatencyMs.clear();
            windowStart = now;
        }

        if (!proto::readMessage(s, m, &err)) return fail("phone connection: " + err);
        switch (m.type()) {
            case Type::Video: {
                auto v = video::parsePayload(m.payload.data(), m.payload.size());
                if (!v || v->header.codec != video::Codec::H264) break;
                ++st.videoMessages;
                st.videoBytes += m.payload.size();
                st.windowBytes += m.payload.size();
                const uint32_t seq = v->header.frameSeq;
                const bool keyframe = (v->header.flags & video::kKeyframe) != 0;
                if (lastSeq != 0 && seq != lastSeq + 1) {
                    ++st.sequenceGaps;
                    requestKeyframe("gap in frame sequence");
                }
                lastSeq = seq;
                if (waitingForKeyframe && !keyframe) {
                    ++st.framesDroppedWaitingForKeyframe;
                    break;
                }
                waitingForKeyframe = false;
                if (!video::avccToAnnexB(v->data, v->dataSize, annexB)) {
                    requestKeyframe("malformed AVCC data");
                    break;
                }
                const uint64_t captureNs = uint64_t(int64_t(v->header.ptsNs) - clock.offsetNs);
                const uint64_t t0 = proto::nowNs();
                const bool ok = decoder.decode(
                    annexB.data(), annexB.size(), seq,
                    [&](const AVFrame* frame) {
                        const uint64_t done = proto::nowNs();
                        ++st.framesDecoded;
                        ++st.windowFrames;
                        st.windowDecodeMs.push_back(double(done - t0) / 1e6);
                        if (done > captureNs) st.windowLatencyMs.push_back(double(done - captureNs) / 1e6);
                        sink(frame, FrameInfo{seq, captureNs, keyframe});
                    },
                    &err);
                if (!ok) {
                    ++st.decodeErrors;
                    requestKeyframe(err.c_str());
                }
                break;
            }
            case Type::Configured: {
                st.configured = proto::parseJson(m).value_or(nlohmann::json::object());
                log("Configured: " + st.configured.dump());
                if (!st.configured.value("ok", false)) return fail("phone refused CONFIGURE: " + st.configured.dump());
                break;
            }
            case Type::Pong:
                if (auto pong = proto::decodePong(m)) clock.onPong(*pong, proto::nowNs());
                break;
            case Type::Log:
                log("[phone] " + std::string(m.text()));
                break;
            case Type::Status:
                log("[phone status] " + std::string(m.text()));
                break;
            case Type::Error:
                return fail("phone error: " + std::string(m.text()));
            case Type::Bye:
                return fail("phone ended the session: " + std::string(m.text()));
            default:
                break;
        }
    }

    proto::writeJson(s, Type::Configure, {{"enabled", false}});
    proto::writeJson(s, Type::Bye, {{"reason", "engine stopped"}});
    st.hardwareDecode = decoder.hardwareActive();
    return st;
}

}  // namespace cabled::engine
