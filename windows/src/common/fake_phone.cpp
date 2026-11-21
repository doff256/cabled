#include "fake_phone.h"

#include "product.h"
#include "protocol.h"
#include "video.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace cabled::fakephone {
namespace {

using proto::Type;

// The PC connection, shared by the read loop and the video thread.
class Link {
public:
    explicit Link(net::Socket& s) : s_(s) {}
    bool message(Type t, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(m_);
        return proto::writeMessage(s_, t, 0, data, size);
    }
    bool json(Type t, const nlohmann::json& j) {
        std::lock_guard<std::mutex> lock(m_);
        return proto::writeJson(s_, t, j);
    }
    bool text(Type t, std::string_view s) {
        std::lock_guard<std::mutex> lock(m_);
        return proto::writeText(s_, t, s);
    }
    // For the probe burst: holds the lock for the whole burst.
    std::mutex& mutex() { return m_; }
    net::Socket& socket() { return s_; }

private:
    net::Socket& s_;
    std::mutex m_;
};

bool sendDown(Link& link, double seconds, size_t chunk, double throttleMbps) {
    std::lock_guard<std::mutex> lock(link.mutex());
    net::Socket& c = link.socket();
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

// Streams the clip in a loop at the configured rate until stopped.
class VideoSender {
public:
    VideoSender(Link& link, std::shared_ptr<const Clip> clip, double fps, uint32_t dropEvery,
                std::function<void(const std::string&)> log)
        : link_(link), clip_(std::move(clip)), fps_(fps), dropEvery_(dropEvery), log_(std::move(log)) {}
    ~VideoSender() { stop(); }

    void start() {
        stop();
        stop_ = false;
        thread_ = std::thread([this] { run(); });
    }
    void stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }
    void requestKeyframe() { keyframeRequested_ = true; }

private:
    void run() {
        const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / fps_));
        auto next = std::chrono::steady_clock::now();
        size_t index = 0;
        uint32_t seq = 0;
        bool discontinuity = false;
        while (!stop_) {
            if (keyframeRequested_.exchange(false)) {
                // Jump to the next keyframe, as a real encoder would emit one on request.
                for (size_t step = 0; step < clip_->units.size(); ++step) {
                    if (clip_->isKeyframe[index]) break;
                    index = (index + 1) % clip_->units.size();
                }
                discontinuity = true;
            }
            const auto& unit = clip_->units[index];
            video::Header h;
            h.ptsNs = proto::nowNs();
            h.frameSeq = ++seq;
            h.codec = video::Codec::H264;
            h.flags = static_cast<uint8_t>((clip_->isKeyframe[index] ? video::kKeyframe : 0) |
                                           (discontinuity ? video::kDiscontinuity : 0));
            discontinuity = false;
            const bool lost = dropEvery_ > 0 && h.frameSeq % dropEvery_ == 0;  // simulated loss in transit
            if (!lost) {
                const auto payload = video::buildPayload(h, nullptr, 0, unit.data(), unit.size());
                if (!link_.message(Type::Video, payload.data(), payload.size())) return;
            }
            index = (index + 1) % clip_->units.size();

            next += interval;
            const auto now = std::chrono::steady_clock::now();
            if (next < now) next = now;  // fell behind: don't burst to catch up
            std::this_thread::sleep_until(next);
        }
    }

    Link& link_;
    std::shared_ptr<const Clip> clip_;
    double fps_;
    uint32_t dropEvery_;
    std::function<void(const std::string&)> log_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> keyframeRequested_{false};
};

}  // namespace

std::shared_ptr<const Clip> loadClip(const std::string& path, uint32_t width, uint32_t height, double fps,
                                     std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "can't open " + path;
        return nullptr;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto clip = std::make_shared<Clip>();
    clip->width = width;
    clip->height = height;
    clip->fps = fps;
    for (const auto& unit : video::splitAccessUnits(bytes.data(), bytes.size())) {
        clip->isKeyframe.push_back(video::containsIdr(video::splitAnnexB(unit.data(), unit.size())));
        clip->units.push_back(video::annexBToAvcc(unit.data(), unit.size()));
    }
    if (clip->units.empty() || std::find(clip->isKeyframe.begin(), clip->isKeyframe.end(), true) ==
                                   clip->isKeyframe.end()) {
        if (error) *error = path + " has no decodable H.264 access units with an IDR frame";
        return nullptr;
    }
    // Start on a keyframe.
    while (!clip->isKeyframe.front()) {
        std::rotate(clip->units.begin(), clip->units.begin() + 1, clip->units.end());
        std::rotate(clip->isKeyframe.begin(), clip->isKeyframe.begin() + 1, clip->isKeyframe.end());
    }
    return clip;
}

void serveClient(net::Socket c, const Options& o) {
    auto log = [&](const std::string& line) {
        if (o.log) o.log(line);
    };
    c.setNoDelay(true);
    c.setBufferSizes(4 << 20);
    Link link(c);

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
    if (!link.json(Type::Hello, hello)) return;
    link.json(Type::Status, {{"thermal", "nominal"}, {"battery", 0.8}, {"charging", true}});
    link.text(Type::Log, "fake phone ready");

    std::unique_ptr<VideoSender> sender;
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
                    if (!link.message(Type::Pong, pong.data(), pong.size())) return;
                }
                break;
            case Type::Configure: {
                auto j = proto::parseJson(m).value_or(nlohmann::json::object());
                const bool enabled = j.value("enabled", true);
                if (sender) sender->stop();
                if (!enabled) {
                    link.json(Type::Configured, {{"ok", true}, {"enabled", false}});
                    log("video stopped");
                    break;
                }
                if (!o.clip) {
                    link.json(Type::Configured, {{"ok", false}, {"error", "fake phone has no clip loaded"}});
                    break;
                }
                // Like a real phone: report what was actually applied (the clip's own size).
                const double fps = j.value("fps", o.clip->fps);
                link.json(Type::Configured, {{"ok", true},
                                             {"enabled", true},
                                             {"codec", "h264"},
                                             {"width", o.clip->width},
                                             {"height", o.clip->height},
                                             {"fps", fps},
                                             {"bitrate", j.value("bitrate", 0)}});
                sender = std::make_unique<VideoSender>(link, o.clip, fps, o.dropEveryFrames, o.log);
                sender->start();
                log("video started at " + std::to_string(fps) + " fps");
                break;
            }
            case Type::KeyframeRequest:
                if (sender) sender->requestKeyframe();
                log("keyframe requested");
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
                    if (!sendDown(link, seconds, chunk, o.throttleMbps)) return;
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
                    link.json(Type::ProbeResult,
                              {{"direction", "up"}, {"bytes", upBytes}, {"seconds", secs}, {"mbps", mbps}});
                }
                break;
            case Type::ProbeResult:
                log("PC measured: " + std::string(m.text()));
                break;
            case Type::Bye:
                log("PC said bye");
                return;  // sender's destructor stops the video thread
            default:
                break;
        }
    }
}

}  // namespace cabled::fakephone
