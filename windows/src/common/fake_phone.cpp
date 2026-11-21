#include "fake_phone.h"

#include "product.h"
#include "protocol.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace cabled::fakephone {
namespace {

using proto::Type;

bool sendDown(net::Socket& c, double seconds, size_t chunk, double throttleMbps) {
    std::vector<uint8_t> buffer(proto::kHeaderSize + chunk);
    proto::encodeHeader(buffer.data(), Type::ProbeData, 0, chunk);
    proto::fillProbePattern(buffer.data() + proto::kHeaderSize, chunk);

    const uint64_t startNs = proto::nowNs();
    const uint64_t endNs = startNs + static_cast<uint64_t>(seconds * 1e9);
    uint64_t seq = 0;
    uint64_t bytes = 0;
    while (true) {
        const uint64_t now = proto::nowNs();
        if (now >= endNs) break;
        if (throttleMbps > 0) {
            const double allowed = static_cast<double>(now - startNs) / 1e9 * throttleMbps * 1e6 / 8.0;
            if (static_cast<double>(bytes) > allowed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
        }
        proto::writeProbeSequence(buffer.data() + proto::kHeaderSize, seq++);
        if (!c.sendAll(buffer.data(), buffer.size())) return false;
        bytes += buffer.size();
    }
    const double elapsed = static_cast<double>(proto::nowNs() - startNs) / 1e9;
    return proto::writeJson(c, Type::ProbeEnd,
                            {{"direction", "down"}, {"bytes", bytes}, {"seconds", elapsed}, {"chunks", seq}});
}

}  // namespace

void serveClient(net::Socket c, const Options& o) {
    auto log = [&](const std::string& line) {
        if (o.log) o.log(line);
    };
    c.setNoDelay(true);
    c.setBufferSizes(4 << 20);

    const nlohmann::json hello = {
        {"protocol", o.protocolVersion.empty() ? proto::versionString() : o.protocolVersion},
        {"role", "phone"},
        {"app", CABLED_PRODUCT_NAME},
        {"appVersion", CABLED_VERSION},
        {"build", "fake"},
        {"model", o.model},
        {"system", "FakeOS 1.0"},
        {"signingExpiry", nullptr},
        {"bind", "loopback"}};
    if (!proto::writeJson(c, Type::Hello, hello)) return;
    proto::writeJson(c, Type::Status, {{"thermal", "nominal"}, {"battery", 0.8}, {"charging", true}});
    proto::writeText(c, Type::Log, "fake phone ready");

    bool upActive = false;
    uint64_t upBytes = 0;
    uint64_t upFirstNs = 0;

    proto::Message m;
    std::string err;
    while (proto::readMessage(c, m, &err)) {
        switch (m.type()) {
            case Type::Hello:
                log("PC hello: " + std::string(m.text()));
                break;
            case Type::Ping:
                if (auto t = proto::decodePing(m)) {
                    const auto pong = proto::encodePong({*t, proto::nowNs()});
                    if (!proto::writeMessage(c, Type::Pong, 0, pong.data(), pong.size())) return;
                }
                break;
            case Type::ProbeRequest: {
                auto j = proto::parseJson(m);
                if (!j) break;
                const std::string direction = j->value("direction", "");
                const double seconds = std::clamp(j->value("seconds", 5.0), 0.1, 60.0);
                const size_t chunk = std::clamp<size_t>(j->value("chunk", size_t{65536}), proto::kMinProbeChunk,
                                                        size_t{4} << 20);
                if (direction == "down") {
                    log("sending for " + std::to_string(seconds) + " s");
                    if (!sendDown(c, seconds, chunk, o.throttleMbps)) return;
                } else if (direction == "up") {
                    upActive = true;
                    upBytes = 0;
                    upFirstNs = 0;
                }
                break;
            }
            case Type::ProbeData:
                if (upActive) {
                    if (!upFirstNs) upFirstNs = proto::nowNs();
                    upBytes += m.payload.size() + proto::kHeaderSize;
                }
                break;
            case Type::ProbeEnd:
                if (upActive) {
                    upActive = false;
                    const double secs = upFirstNs ? static_cast<double>(proto::nowNs() - upFirstNs) / 1e9 : 0.0;
                    const double mbps = secs > 0 ? static_cast<double>(upBytes) * 8.0 / secs / 1e6 : 0.0;
                    proto::writeJson(c, Type::ProbeResult,
                                     {{"direction", "up"}, {"bytes", upBytes}, {"seconds", secs}, {"mbps", mbps}});
                }
                break;
            case Type::ProbeResult:
                log("PC measured: " + std::string(m.text()));
                break;
            case Type::Bye:
                log("PC said bye");
                return;
            default:
                break;
        }
    }
}

}  // namespace cabled::fakephone
