#include "probe_session.h"

#include "product.h"
#include "protocol.h"

#include <algorithm>
#include <cstdio>

namespace cabled::probe {
namespace {

using proto::Type;

class Session {
public:
    Session(net::Socket& s, const Options& o, Report& r) : s_(s), o_(o), r_(r) {}

    void log(const std::string& line) const {
        if (o_.log) o_.log(line);
    }

    // Handles messages that can arrive at any time. False means the session is over.
    bool handleSideMessage(std::string* error) {
        switch (msg.type()) {
            case Type::Log:
                log("[phone] " + std::string(msg.text()));
                return true;
            case Type::Status:
                if (auto j = proto::parseJson(msg)) {
                    r_.peerStatus = *j;
                    log("[phone status] " + j->dump());
                }
                return true;
            case Type::Error:
                *error = "phone reported an error: " + std::string(msg.text());
                return false;
            case Type::Bye:
                *error = "phone ended the session: " + std::string(msg.text());
                return false;
            default:
                log("ignoring unexpected " + proto::typeName(msg.rawType));
                return true;
        }
    }

    bool waitFor(Type want, std::string* error) {
        while (true) {
            if (!proto::readMessage(s_, msg, error)) return false;
            if (msg.type() == want) return true;
            if (!handleSideMessage(error)) return false;
        }
    }

    net::Socket& socket() { return s_; }
    proto::Message msg;

private:
    net::Socket& s_;
    const Options& o_;
    Report& r_;
};

double toMbps(uint64_t bytes, double seconds) {
    return seconds > 0 ? static_cast<double>(bytes) * 8.0 / seconds / 1e6 : 0.0;
}

DirectionResult runDown(Session& ss, const Options& o) {
    DirectionResult d;
    d.ran = true;
    net::Socket& s = ss.socket();
    const nlohmann::json request = {{"direction", "down"}, {"seconds", o.seconds}, {"chunk", o.chunk}};
    if (!proto::writeJson(s, Type::ProbeRequest, request)) {
        d.error = "down: failed to send PROBE_REQUEST";
        return d;
    }
    s.setRecvTimeout(static_cast<uint32_t>((o.seconds + 10.0) * 1000.0));

    uint64_t firstNs = 0;
    uint64_t lastNs = 0;
    uint64_t firstChunkBytes = 0;
    uint64_t expectedSeq = 0;
    std::string err;
    while (true) {
        if (!proto::readMessage(s, ss.msg, &err)) {
            d.error = "down: " + err;
            return d;
        }
        if (ss.msg.type() == Type::ProbeData) {
            const uint64_t now = proto::nowNs();
            const uint64_t wireBytes = ss.msg.payload.size() + proto::kHeaderSize;
            if (!firstNs) {
                firstNs = now;
                firstChunkBytes = wireBytes;
            }
            lastNs = now;
            d.bytes += wireBytes;
            ++d.chunks;
            uint64_t seq = 0;
            if (!proto::verifyProbeChunk(ss.msg.payload.data(), ss.msg.payload.size(), &seq)) {
                ++d.corruptChunks;
            } else {
                if (seq != expectedSeq) ++d.outOfOrderChunks;
                expectedSeq = seq + 1;
            }
            continue;
        }
        if (ss.msg.type() == Type::ProbeEnd) {
            lastNs = proto::nowNs();
            if (auto j = proto::parseJson(ss.msg)) {
                d.peerMbps = toMbps(j->value("bytes", uint64_t{0}), j->value("seconds", 0.0));
            }
            break;
        }
        if (!ss.handleSideMessage(&err)) {
            d.error = "down: " + err;
            return d;
        }
    }
    s.setRecvTimeout(5000);

    // Time runs from the first chunk's arrival, so that chunk's bytes don't count.
    d.seconds = firstNs ? static_cast<double>(lastNs - firstNs) / 1e9 : 0.0;
    d.mbps = toMbps(d.bytes - firstChunkBytes, d.seconds);
    const nlohmann::json result = {{"direction", "down"},       {"bytes", d.bytes},
                                   {"seconds", d.seconds},      {"mbps", d.mbps},
                                   {"chunks", d.chunks},        {"corruptChunks", d.corruptChunks},
                                   {"outOfOrderChunks", d.outOfOrderChunks}};
    proto::writeJson(s, Type::ProbeResult, result);

    if (d.chunks == 0) {
        d.error = "down: the phone sent no data";
    } else if (d.corruptChunks || d.outOfOrderChunks) {
        d.error = "down: " + std::to_string(d.corruptChunks) + " corrupt and " + std::to_string(d.outOfOrderChunks) +
                  " out-of-order chunks";
    } else {
        d.ok = true;
    }
    return d;
}

DirectionResult runUp(Session& ss, const Options& o) {
    DirectionResult d;
    d.ran = true;
    net::Socket& s = ss.socket();
    const nlohmann::json request = {{"direction", "up"}, {"seconds", o.seconds}, {"chunk", o.chunk}};
    if (!proto::writeJson(s, Type::ProbeRequest, request)) {
        d.error = "up: failed to send PROBE_REQUEST";
        return d;
    }

    std::vector<uint8_t> buffer(proto::kHeaderSize + o.chunk);
    proto::encodeHeader(buffer.data(), Type::ProbeData, 0, o.chunk);
    proto::fillProbePattern(buffer.data() + proto::kHeaderSize, o.chunk);

    const uint64_t startNs = proto::nowNs();
    const uint64_t endNs = startNs + static_cast<uint64_t>(o.seconds * 1e9);
    uint64_t seq = 0;
    while (proto::nowNs() < endNs) {
        proto::writeProbeSequence(buffer.data() + proto::kHeaderSize, seq++);
        if (!s.sendAll(buffer.data(), buffer.size())) {
            d.error = "up: send failed: " + net::lastErrorString();
            return d;
        }
        d.bytes += buffer.size();
        ++d.chunks;
    }
    d.seconds = static_cast<double>(proto::nowNs() - startNs) / 1e9;
    const double localMbps = toMbps(d.bytes, d.seconds);

    const nlohmann::json end = {{"direction", "up"}, {"bytes", d.bytes}, {"seconds", d.seconds}, {"chunks", d.chunks}};
    if (!proto::writeJson(s, Type::ProbeEnd, end)) {
        d.error = "up: failed to send PROBE_END";
        return d;
    }

    s.setRecvTimeout(15000);
    std::string err;
    if (!ss.waitFor(Type::ProbeResult, &err)) {
        d.error = "up: no PROBE_RESULT from phone: " + err;
        return d;
    }
    s.setRecvTimeout(5000);
    auto j = proto::parseJson(ss.msg);
    if (!j) {
        d.error = "up: PROBE_RESULT wasn't JSON";
        return d;
    }
    d.peerMbps = j->value("mbps", 0.0);
    // The receiver's number is the honest one; our send rate includes socket buffering.
    d.mbps = d.peerMbps > 0 ? d.peerMbps : localMbps;
    const uint64_t peerBytes = j->value("bytes", uint64_t{0});
    if (peerBytes != d.bytes) {
        d.error = "up: phone received " + std::to_string(peerBytes) + " of " + std::to_string(d.bytes) + " bytes";
        return d;
    }
    d.ok = true;
    return d;
}

Report fail(Report& r, std::string error) {
    r.ok = false;
    r.error = std::move(error);
    return r;
}

}  // namespace

nlohmann::json DirectionResult::toJson() const {
    return {{"ran", ran},
            {"ok", ok},
            {"bytes", bytes},
            {"chunks", chunks},
            {"seconds", seconds},
            {"mbps", mbps},
            {"peerMbps", peerMbps},
            {"corruptChunks", corruptChunks},
            {"outOfOrderChunks", outOfOrderChunks},
            {"error", error}};
}

nlohmann::json Report::toJson() const {
    return {{"ok", ok},
            {"error", error},
            {"peerHello", peerHello},
            {"peerStatus", peerStatus},
            {"rtt",
             {{"count", rttMs.size()},
              {"minMs", rttMinMs},
              {"medianMs", rttMedianMs},
              {"p95Ms", rttP95Ms},
              {"maxMs", rttMaxMs}}},
            {"clockOffsetNs", clockOffsetNs},
            {"down", down.toJson()},
            {"up", up.toJson()}};
}

Report run(net::Socket& s, const Options& o) {
    Report r;
    Session ss(s, o, r);
    s.setNoDelay(true);
    s.setBufferSizes(4 << 20);
    s.setRecvTimeout(5000);

    // 1. Handshake
    const nlohmann::json hello = {
        {"protocol", proto::versionString()}, {"role", "pc"}, {"app", "cabled-probe"}, {"appVersion", CABLED_VERSION}};
    if (!proto::writeJson(s, Type::Hello, hello)) return fail(r, "failed to send HELLO");
    std::string err;
    if (!ss.waitFor(Type::Hello, &err)) return fail(r, "no HELLO from the phone: " + err);
    auto peer = proto::parseJson(ss.msg);
    if (!peer) return fail(r, "the phone's HELLO wasn't JSON");
    r.peerHello = *peer;
    const std::string peerVersion = peer->value("protocol", "");
    if (!proto::isCompatible(peerVersion)) {
        return fail(r, "incompatible protocol: the phone speaks " + peerVersion + ", this tool speaks " +
                           proto::versionString() + ". Update whichever side is older.");
    }
    ss.log("Phone: " + peer->dump());

    // 2. Round trips
    uint64_t bestRtt = UINT64_MAX;
    for (int i = 0; i < o.pings; ++i) {
        const uint64_t t0 = proto::nowNs();
        const auto ping = proto::encodePing(t0);
        if (!proto::writeMessage(s, Type::Ping, 0, ping.data(), ping.size())) return fail(r, "failed to send PING");
        if (!ss.waitFor(Type::Pong, &err)) return fail(r, "no PONG: " + err);
        const uint64_t t1 = proto::nowNs();
        auto pong = proto::decodePong(ss.msg);
        if (!pong || pong->pcTimeNs != t0) return fail(r, "malformed PONG");
        const uint64_t rtt = t1 - t0;
        r.rttMs.push_back(static_cast<double>(rtt) / 1e6);
        if (rtt < bestRtt) {
            bestRtt = rtt;
            r.clockOffsetNs = static_cast<int64_t>(pong->phoneTimeNs) - static_cast<int64_t>(t0 + rtt / 2);
        }
    }
    if (!r.rttMs.empty()) {
        std::vector<double> sorted = r.rttMs;
        std::sort(sorted.begin(), sorted.end());
        r.rttMinMs = sorted.front();
        r.rttMaxMs = sorted.back();
        r.rttMedianMs = sorted[sorted.size() / 2];
        r.rttP95Ms = sorted[std::min(sorted.size() - 1, (sorted.size() * 95) / 100)];
    }

    // 3. Throughput
    if (o.down) r.down = runDown(ss, o);
    if (o.up && (!o.down || r.down.ok)) r.up = runUp(ss, o);

    proto::writeJson(s, Type::Bye, {{"reason", "probe finished"}});

    r.ok = (!o.down || r.down.ok) && (!o.up || r.up.ok);
    if (!r.ok) r.error = !r.down.error.empty() ? r.down.error : r.up.error;
    return r;
}

std::vector<std::string> bitrateVerdict(double linkMbps) {
    struct Mode {
        const char* name;
        double h264;
        double hevc;
    };
    static constexpr Mode kModes[] = {
        {"720p30", 15, 10}, {"1080p30", 25, 18}, {"1080p60", 40, 28}, {"4K30", 60, 45}, {"4K60", 100, 70}};
    const double budget = linkMbps * 0.7;
    std::vector<std::string> lines;
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Video budget (70%% of %.0f Mbit/s): %.0f Mbit/s", linkMbps, budget);
    lines.emplace_back(buf);
    for (const Mode& m : kModes) {
        std::snprintf(buf, sizeof(buf), "  %-8s H.264 @%3.0f: %-4s  HEVC @%3.0f: %s", m.name, m.h264,
                      m.h264 <= budget ? "fits" : "no", m.hevc, m.hevc <= budget ? "fits" : "no");
        lines.emplace_back(buf);
    }
    return lines;
}

}  // namespace cabled::probe
