#include "engine.h"

#include "decoder.h"
#include "frame_convert.h"
#include "product.h"
#include "protocol.h"
#include "status_card.h"
#include "usbmux.h"
#include "vcam_output.h"
#include "video.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <windows.h>

#include <algorithm>
#include <chrono>

namespace cabled::engine {

const char* stateName(EngineState s) {
    switch (s) {
        case EngineState::Stopped: return "stopped";
        case EngineState::NoService: return "no-service";
        case EngineState::NoDevice: return "no-device";
        case EngineState::WaitingForCompanion: return "waiting-for-companion";
        case EngineState::Connecting: return "connecting";
        case EngineState::Incompatible: return "incompatible";
        case EngineState::Streaming: return "streaming";
        case EngineState::Paused: return "paused";
        case EngineState::Error: return "error";
    }
    return "?";
}

nlohmann::json EngineStatus::toJson() const {
    return {{"state", stateName(state)},
            {"headline", headline},
            {"detail", detail},
            {"transport", transport},
            {"device", {{"model", deviceModel}, {"system", deviceSystem}, {"id", deviceId}, {"companion", companionVersion}}},
            {"video", {{"width", width}, {"height", height}, {"fps", fps}, {"mbps", mbps}, {"decodeMs", decodeMs},
                       {"latencyMs", latencyMs}, {"hardwareDecode", hardwareDecode}}},
            {"counters", {{"framesDecoded", framesDecoded}, {"keyframeRequests", keyframeRequests},
                          {"reconnects", reconnects}}},
            {"vcam", {{"state", vcam}, {"inUse", vcamInUse}}},
            {"phone", {{"thermal", thermal}, {"battery", battery}}}};
}

namespace {

using proto::Type;

struct Packet {
    std::vector<uint8_t> annexB;
    uint32_t seq = 0;
    uint64_t captureNs = 0;
    bool flushFirst = false;
};

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

struct PauseText {
    const char* headline;
    const char* detail;
};

PauseText pauseText(const std::string& reason) {
    if (reason == "background") return {"Cabled is in the background on your iPhone", "Open it again to resume."};
    if (reason == "camera-in-use") return {"Another iPhone app is using the camera", "Close it to resume."};
    if (reason == "system-pressure") return {"Your iPhone is too hot", "The camera paused to cool down."};
    if (reason == "call") return {"Paused for a call on your iPhone", "Video resumes after the call."};
    return {"Paused on your iPhone", "Open Cabled on your iPhone to resume."};
}

constexpr size_t kMaxQueuedPackets = 12;

}  // namespace

struct Engine::Impl {
    EngineConfig cfg;
    Engine* owner = nullptr;
    std::atomic<bool> running{false};
    std::thread connectionThread;
    std::thread housekeepingThread;
    std::mutex sleepMutex;
    std::condition_variable sleepCv;

    // Status
    mutable std::mutex statusMutex;
    EngineStatus st;
    std::atomic<EngineState> state{EngineState::Stopped};

    // Session (valid while sessionActive)
    std::mutex ioMutex;  // guards `socket` and serialises writes
    net::Socket* socket = nullptr;
    std::atomic<bool> sessionActive{false};
    std::atomic<uint64_t> lastMessageNs{0};
    std::atomic<bool> waitingForKeyframe{true};
    std::atomic<int64_t> clockOffsetNs{0};
    uint64_t bestRtt = UINT64_MAX;  // connection thread only

    // Decode queue
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<Packet> queue;
    bool decodeStop = false;

    // Virtual camera (decode + housekeeping threads)
    std::mutex vcamMutex;
    VcamOutput vcam;
    StatusCard card;
    std::string cardKey;
    uint64_t lastCardNs = 0;
    std::atomic<uint64_t> lastLivePublishNs{0};

    // Preview
    mutable std::mutex previewMutex;
    std::vector<uint8_t> preview;
    uint32_t previewWidth = 0, previewHeight = 0;
    uint64_t previewSeq = 0;
    FrameConverter previewConverter;  // decode thread only
    std::vector<uint8_t> previewScratch;

    // Counters
    std::atomic<uint64_t> framesDecoded{0}, bytesReceived{0}, keyframeRequests{0}, reconnects{0};
    std::atomic<bool> hardwareActive{false};
    std::atomic<uint32_t> frameWidth{0}, frameHeight{0};
    std::mutex emaMutex;
    double decodeMsEma = 0, latencyMsEma = 0;

    void log(const std::string& line) {
        if (cfg.log) cfg.log(line);
    }

    void setState(EngineState s, const std::string& headline, const std::string& detail) {
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            if (st.state == s && st.headline == headline && st.detail == detail) return;
            st.state = s;
            st.headline = headline;
            st.detail = detail;
        }
        state = s;
        log(std::string("State: ") + stateName(s) + " - " + headline + (detail.empty() ? "" : " (" + detail + ")"));
    }

    // Interruptible sleep. False when the engine is stopping.
    bool sleepFor(double seconds) {
        std::unique_lock<std::mutex> lock(sleepMutex);
        sleepCv.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return !running.load(); });
        return running.load();
    }

    bool send(Type t, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(ioMutex);
        return socket && proto::writeMessage(*socket, t, 0, data, size);
    }
    bool sendJson(Type t, const nlohmann::json& j) {
        const std::string text = j.dump();
        return send(t, text.data(), text.size());
    }
    void requestKeyframe(const std::string& why) {
        ++keyframeRequests;
        log("Requesting keyframe: " + why);
        send(Type::KeyframeRequest, nullptr, 0);
    }

    std::optional<net::Socket> connect();
    void connectionLoop();
    void runSession(net::Socket& s);
    void decodeLoop(Decoder& decoder);
    void onFrame(const AVFrame* f, const Packet& p, uint64_t decodeStartNs);
    void housekeepingLoop();
    void publishStatusCard();
};

// ------------------------------------------------------------------ connecting

std::optional<net::Socket> Engine::Impl::connect() {
    if (!cfg.tcpTarget.empty()) {
        const size_t colon = cfg.tcpTarget.rfind(':');
        std::string err;
        auto s = colon == std::string::npos
                     ? std::nullopt
                     : net::Socket::connectTcp(cfg.tcpTarget.substr(0, colon),
                                               uint16_t(std::stoi(cfg.tcpTarget.substr(colon + 1))), &err);
        if (!s) {
            setState(EngineState::WaitingForCompanion, "Waiting for the phone",
                     "Nothing is answering at " + cfg.tcpTarget + " yet.");
            return std::nullopt;
        }
        std::lock_guard<std::mutex> lock(statusMutex);
        st.transport = "tcp";
        st.deviceId = cfg.tcpTarget;
        return s;
    }

    usbmux::Client client;
    std::string err;
    auto devices = client.listDevices(&err);
    if (!devices) {
        setState(EngineState::NoService, "Apple Mobile Device Service isn't running",
                 "Install \"Apple Devices\" from the Microsoft Store (or iTunes from apple.com), then plug in the "
                 "iPhone.");
        return std::nullopt;
    }
    const usbmux::Device* device = nullptr;
    for (const auto& d : *devices) {
        if (d.isUsb() && (cfg.udid.empty() || d.serial == cfg.udid)) {  // Wi-Fi devices ignored (F16)
            device = &d;
            break;
        }
    }
    if (!device) {
        setState(EngineState::NoDevice, "Connect your iPhone", "Plug it in with a USB cable and unlock it.");
        return std::nullopt;
    }
    auto r = client.connect(device->id, cfg.devicePort);
    if (!r.socket) {
        if (r.code == usbmux::kResultConnectionRefused) {
            setState(EngineState::WaitingForCompanion, "Open Cabled on your iPhone",
                     "Keep it on screen while you use the camera. If it won't open, refresh it in SideStore.");
        } else {
            setState(EngineState::WaitingForCompanion, "Can't reach your iPhone yet",
                     "If it asks \"Trust This Computer?\", tap Trust. (" + r.error + ")");
        }
        return std::nullopt;
    }
    std::lock_guard<std::mutex> lock(statusMutex);
    st.transport = "usb";
    st.deviceId = device->serial;
    return std::move(r.socket);
}

void Engine::Impl::connectionLoop() {
    while (running) {
        auto s = connect();
        if (!s) {
            sleepFor(cfg.retrySeconds);
            continue;
        }
        runSession(*s);
        if (!running) break;
        ++reconnects;
        sleepFor(cfg.retrySeconds);
    }
}

// ------------------------------------------------------------------ session

void Engine::Impl::runSession(net::Socket& s) {
    setState(EngineState::Connecting, "Connecting to your iPhone", "");
    s.setNoDelay(true);
    s.setBufferSizes(4 << 20);
    s.setRecvTimeout(5000);

    // Registered at once so stop() can abort even a handshake; unregistered on every exit path.
    {
        std::lock_guard<std::mutex> lock(ioMutex);
        socket = &s;
    }
    struct Unregister {
        Impl* self;
        ~Unregister() {
            std::lock_guard<std::mutex> lock(self->ioMutex);
            self->socket = nullptr;
        }
    } unregister{this};

    // Handshake.
    const nlohmann::json hello = {
        {"protocol", proto::versionString()}, {"role", "pc"}, {"app", "cabled-engine"}, {"appVersion", CABLED_VERSION}};
    if (!sendJson(Type::Hello, hello)) return;
    proto::Message m;
    std::string err;
    do {
        if (!proto::readMessage(s, m, &err)) {
            log("No HELLO from the phone: " + err);
            return;
        }
    } while (m.type() != Type::Hello);
    const auto peer = proto::parseJson(m).value_or(nlohmann::json::object());
    const std::string peerVersion = peer.value("protocol", "");
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        st.deviceModel = peer.value("model", "");
        st.deviceSystem = peer.value("system", "");
        st.companionVersion = peer.value("appVersion", "");
    }
    if (!proto::isCompatible(peerVersion)) {
        setState(EngineState::Incompatible, "Update the Cabled app on your iPhone",
                 "It speaks protocol " + peerVersion + "; this PC needs " + proto::versionString() + ".");
        sleepFor(5);
        return;
    }

    // Clock sync burst.
    bestRtt = UINT64_MAX;
    for (int i = 0; i < 8; ++i) {
        const uint64_t t0 = proto::nowNs();
        const auto ping = proto::encodePing(t0);
        if (!send(Type::Ping, ping.data(), ping.size())) return;
        do {
            if (!proto::readMessage(s, m, &err)) return;
        } while (m.type() != Type::Pong);
        if (auto pong = proto::decodePong(m)) {
            const uint64_t rtt = proto::nowNs() - pong->pcTimeNs;
            if (rtt < bestRtt) {
                bestRtt = rtt;
                clockOffsetNs = int64_t(pong->phoneTimeNs) - int64_t(pong->pcTimeNs + rtt / 2);
            }
        }
    }

    Decoder decoder;
    if (!decoder.open(cfg.hardwareDecode, &err)) {
        setState(EngineState::Error, "Video decoder failed to start", err);
        return;
    }

    // From here the housekeeping thread may ping and abort; decode thread may write.
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.clear();
        decodeStop = false;
    }
    waitingForKeyframe = true;
    lastMessageNs = proto::nowNs();
    sessionActive = true;
    std::thread decodeThread([&] { decodeLoop(decoder); });

    sendJson(Type::Configure, {{"enabled", true},
                               {"codec", "h264"},
                               {"width", cfg.width},
                               {"height", cfg.height},
                               {"fps", cfg.fps},
                               {"bitrate", cfg.bitrate},
                               {"keyframeInterval", 2.0}});
    s.setRecvTimeout(0);  // the housekeeping watchdog handles silence

    std::string endReason = "connection closed";
    uint32_t lastSeq = 0;
    while (running) {
        if (!proto::readMessage(s, m, &err)) {
            endReason = err;
            break;
        }
        lastMessageNs = proto::nowNs();
        bool end = false;
        switch (m.type()) {
            case Type::Video: {
                auto v = video::parsePayload(m.payload.data(), m.payload.size());
                if (!v || v->header.codec != video::Codec::H264) break;
                bytesReceived += m.payload.size();
                const uint32_t seq = v->header.frameSeq;
                const bool keyframe = (v->header.flags & video::kKeyframe) != 0;
                if (lastSeq != 0 && seq != lastSeq + 1 && !waitingForKeyframe) {
                    waitingForKeyframe = true;
                    requestKeyframe("gap in frame sequence");
                }
                lastSeq = seq;
                if (waitingForKeyframe && !keyframe) break;  // never decode from the middle of a GOP
                Packet p;
                p.flushFirst = waitingForKeyframe.exchange(false);
                if (!video::avccToAnnexB(v->data, v->dataSize, p.annexB)) {
                    waitingForKeyframe = true;
                    requestKeyframe("malformed AVCC data");
                    break;
                }
                p.seq = seq;
                p.captureNs = uint64_t(int64_t(v->header.ptsNs) - clockOffsetNs.load());
                bool overflow = false;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    if (queue.size() >= kMaxQueuedPackets) {
                        queue.clear();  // decoder can't keep up: skip to fresh video
                        overflow = true;
                    } else {
                        queue.push_back(std::move(p));
                    }
                }
                if (overflow) {
                    waitingForKeyframe = true;
                    requestKeyframe("decoder fell behind");
                } else {
                    queueCv.notify_one();
                }
                break;
            }
            case Type::Configured: {
                const auto j = proto::parseJson(m).value_or(nlohmann::json::object());
                if (!j.value("ok", false)) {
                    setState(EngineState::Error, "Your iPhone refused the video settings", j.value("error", j.dump()));
                    endReason = "CONFIGURE refused";
                    end = true;
                } else {
                    std::lock_guard<std::mutex> lock(statusMutex);
                    st.width = j.value("width", 0u);
                    st.height = j.value("height", 0u);
                }
                break;
            }
            case Type::Status: {
                const auto j = proto::parseJson(m).value_or(nlohmann::json::object());
                {
                    std::lock_guard<std::mutex> lock(statusMutex);
                    if (j.contains("thermal")) st.thermal = j.value("thermal", "");
                    if (j.contains("battery") && j["battery"].is_number()) st.battery = j["battery"].get<double>();
                }
                const std::string session = j.value("session", "");
                if (session == "interrupted") {
                    const PauseText t = pauseText(j.value("reason", ""));
                    setState(EngineState::Paused, t.headline, t.detail);
                } else if (session == "running" && state == EngineState::Paused) {
                    setState(EngineState::Connecting, "Resuming", "");  // Streaming again on the next frame
                }
                break;
            }
            case Type::Pong:
                if (auto pong = proto::decodePong(m)) {
                    const uint64_t rtt = proto::nowNs() - pong->pcTimeNs;
                    // Follow drift: accept anything close to the best round trip seen.
                    if (rtt <= bestRtt + 300'000) {
                        bestRtt = std::min(bestRtt, rtt);
                        clockOffsetNs = int64_t(pong->phoneTimeNs) - int64_t(pong->pcTimeNs + rtt / 2);
                    }
                }
                break;
            case Type::Log:
                log("[phone] " + std::string(m.text()));
                break;
            case Type::Bye:
                endReason = "phone said goodbye: " + std::string(m.text());
                end = true;
                break;
            case Type::Error:
                endReason = "phone error: " + std::string(m.text());
                end = true;
                break;
            default:
                break;
        }
        if (end) break;
    }

    sessionActive = false;  // stop() already said BYE if we're shutting down
    {
        std::lock_guard<std::mutex> lock(ioMutex);
        socket = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        decodeStop = true;
        queue.clear();
    }
    queueCv.notify_all();
    decodeThread.join();
    log("Session ended: " + endReason);
    if (running && state != EngineState::Error) setState(EngineState::Connecting, "Reconnecting", endReason);
}

// ------------------------------------------------------------------ decoding

void Engine::Impl::decodeLoop(Decoder& decoder) {
    std::string err;
    while (true) {
        Packet p;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCv.wait(lock, [&] { return decodeStop || !queue.empty(); });
            if (decodeStop) return;
            p = std::move(queue.front());
            queue.pop_front();
        }
        if (p.flushFirst) decoder.flush();
        const uint64_t t0 = proto::nowNs();
        const bool ok =
            decoder.decode(p.annexB.data(), p.annexB.size(), p.seq, [&](const AVFrame* f) { onFrame(f, p, t0); }, &err);
        hardwareActive = decoder.hardwareActive();
        if (!ok) {
            waitingForKeyframe = true;
            requestKeyframe("decode error: " + err);
        }
    }
}

void Engine::Impl::onFrame(const AVFrame* f, const Packet& p, uint64_t decodeStartNs) {
    const uint64_t done = proto::nowNs();
    ++framesDecoded;
    frameWidth = uint32_t(f->width);
    frameHeight = uint32_t(f->height);
    {
        std::lock_guard<std::mutex> lock(emaMutex);
        const double decodeMs = double(done - decodeStartNs) / 1e6;
        const double latencyMs = done > p.captureNs ? double(done - p.captureNs) / 1e6 : 0.0;
        decodeMsEma = decodeMsEma == 0 ? decodeMs : decodeMsEma * 0.9 + decodeMs * 0.1;
        latencyMsEma = latencyMsEma == 0 ? latencyMs : latencyMsEma * 0.9 + latencyMs * 0.1;
    }
    if (state != EngineState::Streaming) setState(EngineState::Streaming, "Streaming", "");

    if (cfg.frameSink) cfg.frameSink(f, p.seq);

    if (cfg.virtualCamera) {
        std::lock_guard<std::mutex> lock(vcamMutex);
        if (vcam.publish(f, nsToQpc(p.captureNs)) == VcamOutput::State::Streaming) lastLivePublishNs = done;
    }

    if (owner->previewEnabled_ && cfg.previewMaxWidth > 0) {
        const uint32_t pw = std::min<uint32_t>(cfg.previewMaxWidth, uint32_t(f->width)) & ~1u;
        const uint32_t ph = uint32_t(uint64_t(pw) * uint32_t(f->height) / std::max(1, f->width)) & ~1u;
        previewScratch.resize(size_t(pw) * ph * 4);
        const auto view = vcam::packedView(vcam::kFourccRGB32, pw, ph, previewScratch.data());
        if (pw && ph && previewConverter.convert(f, view)) {
            for (size_t i = 3; i < previewScratch.size(); i += 4) previewScratch[i] = 255;  // BGR0 -> opaque BGRA
            std::lock_guard<std::mutex> lock(previewMutex);
            preview.swap(previewScratch);
            previewWidth = pw;
            previewHeight = ph;
            ++previewSeq;
        }
    }
}

// ------------------------------------------------------------------ housekeeping

void Engine::Impl::publishStatusCard() {
    const uint64_t now = proto::nowNs();
    const bool live = state == EngineState::Streaming &&
                      int64_t(now) - int64_t(lastLivePublishNs.load()) < 1'000'000'000ll;
    if (live) return;
    if (now - lastCardNs < 200'000'000ull) return;  // ~5 fps is plenty; the source repeats frames between

    std::string headline, detail;
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        headline = st.headline;
        detail = st.detail;
    }
    if (state == EngineState::Streaming) {  // connected, but frames stopped arriving
        headline = "Waiting for video";
        detail = "Your iPhone is connected but no frames are arriving.";
    }

    std::lock_guard<std::mutex> lock(vcamMutex);
    uint32_t w = 0, h = 0;
    if (!vcam.activeSize(w, h)) return;  // no app is using the camera
    const std::string key = headline + "\n" + detail + "\n" + std::to_string(w) + "x" + std::to_string(h);
    if (key != cardKey) {
        card = renderStatusCard(w, h, widen(headline), widen(detail));
        cardKey = key;
    }
    vcam.publishBgra(card.bgra.data(), card.width, card.height, card.width * 4, nsToQpc(now));
    lastCardNs = now;
}

void Engine::Impl::housekeepingLoop() {
    uint64_t lastPingNs = 0;
    uint64_t lastStatsNs = proto::nowNs();
    uint64_t lastFrames = framesDecoded, lastBytes = bytesReceived;
    while (sleepFor(0.1)) {
        const uint64_t now = proto::nowNs();

        if (sessionActive) {
            if (now - lastPingNs >= 1'000'000'000ull) {
                const auto ping = proto::encodePing(now);
                send(Type::Ping, ping.data(), ping.size());
                lastPingNs = now;
            }
            // Signed: the receive thread may have stamped a message after `now` was read.
            const double silentSeconds = double(int64_t(now) - int64_t(lastMessageNs.load())) / 1e9;
            if (silentSeconds > cfg.watchdogSeconds) {
                log("No messages from the phone for " + std::to_string(int(silentSeconds)) + " s; reconnecting");
                std::lock_guard<std::mutex> lock(ioMutex);
                if (socket) socket->abort();
                lastMessageNs = now;  // don't fire again while the session winds down
            }
        }

        if (cfg.virtualCamera) publishStatusCard();

        if (now - lastStatsNs >= 1'000'000'000ull) {
            const double seconds = double(now - lastStatsNs) / 1e9;
            const uint64_t frames = framesDecoded, bytes = bytesReceived;
            std::string vcamText;
            bool vcamInUse = false;
            if (cfg.virtualCamera) {
                std::lock_guard<std::mutex> lock(vcamMutex);
                vcamText = vcam.describe();
                vcamInUse = vcam.state() == VcamOutput::State::Streaming;
            }
            std::lock_guard<std::mutex> lock(statusMutex);
            st.fps = double(frames - lastFrames) / seconds;
            st.mbps = double(bytes - lastBytes) * 8.0 / seconds / 1e6;
            st.framesDecoded = frames;
            st.keyframeRequests = keyframeRequests;
            st.reconnects = reconnects;
            st.hardwareDecode = hardwareActive;
            if (frameWidth) {
                st.width = frameWidth;
                st.height = frameHeight;
            }
            {
                std::lock_guard<std::mutex> emaLock(emaMutex);
                st.decodeMs = decodeMsEma;
                st.latencyMs = latencyMsEma;
            }
            st.vcam = cfg.virtualCamera ? vcamText : "off";
            st.vcamInUse = vcamInUse;
            lastFrames = frames;
            lastBytes = bytes;
            lastStatsNs = now;
        }
    }
}

// ------------------------------------------------------------------ public

Engine::Engine(EngineConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(config);
    impl_->owner = this;
}

Engine::~Engine() { stop(); }

void Engine::start() {
    if (impl_->running.exchange(true)) return;
    impl_->setState(EngineState::Connecting, "Starting", "");
    impl_->connectionThread = std::thread([this] { impl_->connectionLoop(); });
    impl_->housekeepingThread = std::thread([this] { impl_->housekeepingLoop(); });
}

void Engine::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->sleepCv.notify_all();
    {
        // Wake the receive loop; it sends BYE first if it still can.
        std::lock_guard<std::mutex> lock(impl_->ioMutex);
        if (impl_->socket) {
            proto::writeJson(*impl_->socket, Type::Configure, {{"enabled", false}});
            proto::writeJson(*impl_->socket, Type::Bye, {{"reason", "Cabled closed on the PC"}});
            impl_->socket->abort();
        }
    }
    if (impl_->connectionThread.joinable()) impl_->connectionThread.join();
    if (impl_->housekeepingThread.joinable()) impl_->housekeepingThread.join();
    impl_->setState(EngineState::Stopped, "Stopped", "");
}

EngineStatus Engine::status() const {
    std::lock_guard<std::mutex> lock(impl_->statusMutex);
    return impl_->st;
}

bool Engine::copyPreview(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height, uint64_t& seq) const {
    std::lock_guard<std::mutex> lock(impl_->previewMutex);
    if (impl_->previewSeq == 0 || impl_->previewSeq == seq) return false;
    bgra = impl_->preview;
    width = impl_->previewWidth;
    height = impl_->previewHeight;
    seq = impl_->previewSeq;
    return true;
}

}  // namespace cabled::engine
