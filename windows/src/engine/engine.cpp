#include "engine.h"

#include "decoder.h"
#include "frame_convert.h"
#include "framing.h"
#include "negotiator.h"
#include "product.h"
#include "protocol.h"
#include "status_card.h"
#include "usbmux.h"
#include "vcam_output.h"
#include "video.h"

#include <cstdio>

extern "C" {
#include <libavutil/frame.h>
}

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

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
        case EngineState::Idle: return "idle";
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
            {"link", {{"mbps", linkMbps}, {"config", videoConfig}}},
            {"camera", {{"current", camera}, {"zoom", zoom}, {"lens", lens}, {"list", cameras}}},
            {"framing", [&] {
                 nlohmann::json f = framingInfo;
                 f["enabled"] = framing;
                 f["faces"] = faces;
                 return f;
             }()},
            {"controls", controls},
            {"capture", capture},
            {"phone", {{"thermal", thermal}, {"battery", battery}}}};
}

namespace {

using proto::Type;

struct Packet {
    std::vector<uint8_t> annexB;
    uint32_t seq = 0;
    uint64_t captureNs = 0;
    bool flushFirst = false;
    bool hevc = false;
    std::string meta;  // VIDEO metadata (JSON): faces from the phone
};

// {"faces":[{"id":1,"x":..,"y":..,"w":..,"h":..}]}; coordinates normalized to the sent frame.
std::vector<Face> parseFaces(const std::string& meta) {
    std::vector<Face> faces;
    if (meta.empty()) return faces;
    const auto j = nlohmann::json::parse(meta, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return faces;
    for (const auto& f : j.value("faces", nlohmann::json::array())) {
        Face face;
        face.id = f.value("id", -1);
        face.x = f.value("x", 0.0);
        face.y = f.value("y", 0.0);
        face.w = f.value("w", 0.0);
        face.h = f.value("h", 0.0);
        if (face.w > 0 && face.h > 0) faces.push_back(face);
    }
    return faces;
}

// Debug overlay: an unfilled rectangle in a BGRA image.
void drawBox(std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, double x0, double y0, double x1, double y1,
             uint8_t r, uint8_t g, uint8_t b) {
    const int ix0 = std::clamp(int(x0), 0, int(w) - 1), ix1 = std::clamp(int(x1), 0, int(w) - 1);
    const int iy0 = std::clamp(int(y0), 0, int(h) - 1), iy1 = std::clamp(int(y1), 0, int(h) - 1);
    auto put = [&](int x, int y) {
        uint8_t* p = &bgra[(size_t(y) * w + size_t(x)) * 4];
        p[0] = b;
        p[1] = g;
        p[2] = r;
    };
    for (int t = 0; t < 2; ++t) {
        for (int x = ix0; x <= ix1; ++x) {
            put(x, std::min(iy0 + t, int(h) - 1));
            put(x, std::max(iy1 - t, 0));
        }
        for (int y = iy0; y <= iy1; ++y) {
            put(std::min(ix0 + t, int(w) - 1), y);
            put(std::max(ix1 - t, 0), y);
        }
    }
}

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

    // Negotiation (R5): connection thread writes caps/link, housekeeping renegotiates.
    std::mutex choiceMutex;
    PhoneCaps caps;
    double linkMbps = 0;
    Choice current;
    Choice pending;
    uint64_t pendingSinceNs = 0;
    bool configured = false;
    std::atomic<bool> renegotiateNow{false};  // user picked a camera: skip the debounce

    // Auto-framing (R7): decode thread only, except the on/off flag and the knobs (prefsMutex).
    std::atomic<bool> framingEnabled{true};
    Framer framer;
    FramingSettings framingSettings;
    FramingSettings framingKnobs;  // prefsMutex: tightness, speed, group as set by the user

    // R8 (prefsMutex): remembered camera controls, pinned capture.
    nlohmann::json rememberedControls = nlohmann::json::object();
    uint32_t pinWidth = 0, pinHeight = 0;
    double pinFps = 0;
    // Last crop, for tap-to-focus (decode thread writes, API reads).
    std::mutex cropMutex;
    CropRect lastCrop;
    double lastBaseX = 0, lastBaseY = 0, lastTargetW = 0, lastOutAspect = 16.0 / 9.0;
    uint32_t lastSrcW = 0, lastSrcH = 0;

    // Overview thumbnail (whole camera view)
    mutable std::mutex overviewMutex;
    std::vector<uint8_t> overview, overviewScratch;
    uint32_t overviewWidth = 0, overviewHeight = 0;
    uint64_t overviewSeq = 0;
    FrameConverter overviewConverter;

    // User preferences (R6), separate lock so currentWant() can be called under choiceMutex.
    std::mutex prefsMutex;
    std::string wantedCamera;
    std::map<std::string, double> zoomFactors;  // camera id -> device zoom factor

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

    // Messages that may arrive at any time, also during the handshake.
    void absorb(const proto::Message& m) {
        switch (m.type()) {
            case Type::Caps: {
                const PhoneCaps parsed = parseCaps(proto::parseJson(m).value_or(nlohmann::json::object()));
                nlohmann::json list = nlohmann::json::array();
                for (const auto& c : parsed.cameras) {
                    list.push_back({{"id", c.id},
                                    {"label", c.label()},
                                    {"name", c.name},
                                    {"type", c.type},
                                    {"minZoom", c.minDisplay()},
                                    {"maxZoom", c.maxDisplay()},
                                    {"presets", c.presets()}});
                }
                {
                    std::lock_guard<std::mutex> lock(choiceMutex);
                    caps = parsed;
                }
                {
                    std::lock_guard<std::mutex> lock(statusMutex);
                    st.cameras = list;
                }
                log("Phone offers " + std::to_string(parsed.cameras.size()) + " cameras, " +
                    std::to_string(parsed.formats.size()) + " formats on the default" +
                    (parsed.hevc ? ", HEVC" : ""));
                break;
            }
            case Type::ControlState: {
                const auto j = proto::parseJson(m).value_or(nlohmann::json::object());
                const std::string id = j.value("camera", "");
                const double factor = j.value("zoom", 0.0);
                double scale = 1;
                {
                    std::lock_guard<std::mutex> lock(choiceMutex);
                    if (const CameraInfo* c = caps.find(id)) scale = c->zoomScale;
                }
                if (factor > 0) {
                    std::lock_guard<std::mutex> lock(prefsMutex);
                    zoomFactors[id] = factor;  // restored on the next CONFIGURE
                }
                std::lock_guard<std::mutex> lock(statusMutex);
                if (!id.empty()) st.camera = id;
                if (factor > 0) st.zoom = std::round(factor * scale * 100) / 100;
                st.lens = j.value("lens", st.lens);
                if (j.contains("thermal")) st.thermal = j.value("thermal", st.thermal);
                for (const char* key : {"focus", "exposure", "whiteBalance"}) {  // R8 camera state
                    if (j.contains(key)) st.controls[key] = j[key];
                }
                break;
            }
            case Type::Log:
                log("[phone] " + std::string(m.text()));
                break;
            default:
                break;
        }
    }

    Want currentWant() {
        Want w;
        if (cfg.virtualCamera) {
            std::lock_guard<std::mutex> lock(vcamMutex);
            if (vcam.activeFormat(w.width, w.height, w.fps)) {
                w.consumer = true;
                return withCamera(w);
            }
        }
        // No app has the camera: only the preview (or a test sink) wants video, at the default.
        w.consumer = (owner->previewEnabled_ && cfg.previewMaxWidth > 0) || bool(cfg.frameSink);
        w.width = cfg.width;
        w.height = cfg.height;
        w.fps = cfg.fps;
        return withCamera(w);
    }

    Want withCamera(Want w) {
        w.framing = framingEnabled;
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            w.thermal = st.thermal;  // F7: hot phone -> lighter job
        }
        std::lock_guard<std::mutex> lock(prefsMutex);
        // A zoomed shot wants extra pixels to crop from, following or not (framing off still zooms).
        w.framing = framingEnabled || framingKnobs.zoom > 1.05;
        w.camera = wantedCamera;
        w.pinWidth = pinWidth;
        w.pinHeight = pinHeight;
        w.pinFps = pinFps;
        return w;
    }

    double zoomFor(const std::string& camera) {
        std::lock_guard<std::mutex> lock(prefsMutex);
        auto it = zoomFactors.find(camera);
        return it == zoomFactors.end() ? 0.0 : it->second;
    }

    // Sends CONFIGURE for `c` and updates status. Caller holds no locks.
    void applyChoice(const Choice& c) {
        {
            std::lock_guard<std::mutex> lock(choiceMutex);
            current = c;
            pending = c;
            configured = true;
        }
        sendJson(Type::Configure, c.toConfigure(zoomFor(c.camera)));
        if (c.enabled) {  // focus / exposure / white balance survive restarts and camera switches
            nlohmann::json controlsCopy;
            {
                std::lock_guard<std::mutex> lock(prefsMutex);
                controlsCopy = rememberedControls;
            }
            if (!controlsCopy.empty()) sendJson(Type::ControlSet, controlsCopy);
        }
        log("Video: " + c.describe() + (c.camera.empty() ? "" : " on camera " + c.camera));
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            st.videoConfig = c.describe();
            if (!c.camera.empty()) st.camera = c.camera;
        }
        if (!c.enabled) {
            setState(EngineState::Idle, "Connected", "Video starts when an app opens Cabled Camera.");
        } else if (state == EngineState::Idle) {
            setState(EngineState::Connecting, "Starting video", "");
        }
    }

    void renegotiate(uint64_t now) {
        if (!sessionActive) return;
        const Want want = currentWant();
        Choice next, cur;
        bool ready = false;
        {
            std::lock_guard<std::mutex> lock(choiceMutex);
            if (!configured) return;
            next = negotiate(caps, want, linkMbps);
            cur = current;
            if (next == cur) {
                pending = cur;
                return;
            }
            const bool userAction = renegotiateNow.exchange(false);
            if (!(next == pending) && !userAction) {  // a new wish: let it settle (apps open/close in bursts)
                pending = next;
                pendingSinceNs = now;
                return;
            }
            ready = userAction || now - pendingSinceNs >= 700'000'000ull;
        }
        if (ready) applyChoice(next);
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
        // After an error, leave its message up long enough to read before retrying.
        sleepFor(state == EngineState::Error ? 5.0 : cfg.retrySeconds);
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
    {
        std::lock_guard<std::mutex> lock(choiceMutex);
        caps = PhoneCaps{};
        configured = false;
        linkMbps = 0;
    }
    do {
        if (!proto::readMessage(s, m, &err)) {
            log("No HELLO from the phone: " + err);
            return;
        }
        if (m.type() != Type::Hello) absorb(m);
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
            if (m.type() != Type::Pong) absorb(m);
        } while (m.type() != Type::Pong);
        if (auto pong = proto::decodePong(m)) {
            const uint64_t rtt = proto::nowNs() - pong->pcTimeNs;
            if (rtt < bestRtt) {
                bestRtt = rtt;
                clockOffsetNs = int64_t(pong->phoneTimeNs) - int64_t(pong->pcTimeNs + rtt / 2);
            }
        }
    }

    // Link probe: half a second of phone -> PC data, so negotiation knows the budget (R5).
    {
        const double probeSeconds = 0.5;
        const nlohmann::json request = {{"direction", "down"}, {"seconds", probeSeconds}, {"chunk", 65536}};
        if (!sendJson(Type::ProbeRequest, request)) return;
        uint64_t bytes = 0, firstNs = 0, lastNs = 0;
        while (true) {
            if (!proto::readMessage(s, m, &err)) return;
            if (m.type() == Type::ProbeData) {
                const uint64_t now = proto::nowNs();
                if (!firstNs) firstNs = now;
                else bytes += m.payload.size() + proto::kHeaderSize;  // first chunk only starts the clock
                lastNs = now;
            } else if (m.type() == Type::ProbeEnd) {
                break;
            } else {
                absorb(m);
            }
        }
        const double seconds = firstNs && lastNs > firstNs ? double(lastNs - firstNs) / 1e9 : 0;
        const double mbps = seconds > 0 ? double(bytes) * 8 / seconds / 1e6 : 0;
        sendJson(Type::ProbeResult, {{"direction", "down"}, {"bytes", bytes}, {"seconds", seconds}, {"mbps", mbps}});
        {
            std::lock_guard<std::mutex> lock(choiceMutex);
            linkMbps = mbps;
        }
        {
            std::lock_guard<std::mutex> lock(statusMutex);
            st.linkMbps = mbps;
        }
        log("Link: " + std::to_string(int(mbps)) + " Mbit/s phone -> PC");
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

    {
        Choice first;
        {
            std::lock_guard<std::mutex> lock(choiceMutex);
            first = negotiate(caps, currentWant(), linkMbps);
        }
        applyChoice(first);
    }
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
                if (!v || (v->header.codec != video::Codec::H264 && v->header.codec != video::Codec::HEVC)) break;
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
                p.hevc = v->header.codec == video::Codec::HEVC;
                if (v->metaSize) p.meta.assign(reinterpret_cast<const char*>(v->meta), v->metaSize);
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
            case Type::Caps:
            case Type::ControlState:
                absorb(m);
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
        if (p.hevc != decoder.isHevc()) {  // negotiation switched codecs
            if (!decoder.open(cfg.hardwareDecode, &err, p.hevc)) {
                log("Decoder for " + std::string(p.hevc ? "HEVC" : "H.264") + " failed: " + err);
                continue;
            }
            log(std::string("Decoder switched to ") + (p.hevc ? "HEVC" : "H.264"));
        } else if (p.flushFirst) {
            decoder.flush();
        }
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
    if (state != EngineState::Streaming && state != EngineState::Idle) setState(EngineState::Streaming, "Streaming", "");

    if (cfg.frameSink) cfg.frameSink(f, p.seq);

    // Auto-framing: one crop for both the camera and the preview, at the app's output shape
    // (or the default shape when only the preview is watching). Also aspect-fills portrait video.
    uint32_t outW = cfg.width, outH = cfg.height;
    double outFps = 0;
    if (cfg.virtualCamera) {
        std::lock_guard<std::mutex> lock(vcamMutex);
        vcam.activeFormat(outW, outH, outFps);
    }
    const std::vector<Face> faces = parseFaces(p.meta);
    {
        std::lock_guard<std::mutex> lock(prefsMutex);
        framingSettings = framingKnobs;
    }
    framingSettings.enabled = framingEnabled;
    const CropRect crop = framer.update(faces, uint32_t(f->width), uint32_t(f->height), outW, outH,
                                        double(p.captureNs) / 1e9, framingSettings);
    {
        std::lock_guard<std::mutex> lock(cropMutex);
        lastCrop = crop;
        lastBaseX = framer.baseX();
        lastBaseY = framer.baseY();
        lastTargetW = framer.targetWidth();
        lastOutAspect = outH ? double(outW) / outH : 16.0 / 9.0;
        lastSrcW = uint32_t(f->width);
        lastSrcH = uint32_t(f->height);
    }
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        st.faces = int(faces.size());
        st.framingInfo = {{"tightness", framingSettings.tightness()},
                          {"speed", framingSettings.speed},
                          {"group", framingSettings.group},
                          {"zoom", framingSettings.zoom},
                          // Closest the quality guard allows at this capture size (UI shows the limit).
                          {"reachableZoom", reachableZoom(uint32_t(f->width), uint32_t(f->height), outW, outH,
                                                          framingSettings)},
                          {"sourceWidth", f->width},
                          {"sourceHeight", f->height},
                          {"crop",
                           {{"x", crop.x / f->width},
                            {"y", crop.y / f->height},
                            {"w", crop.w / f->width},
                            {"h", crop.h / f->height}}}};
    }

    // Overview: the whole camera view, small, for the box-in-box display (R8).
    if (owner->previewEnabled_ && cfg.previewMaxWidth > 0) {
        const uint32_t ow = 320, oh = uint32_t(320.0 * f->height / std::max(1, f->width)) & ~1u;
        overviewScratch.resize(size_t(ow) * oh * 4);
        if (oh && overviewConverter.convert(f, vcam::packedView(vcam::kFourccRGB32, ow, oh, overviewScratch.data()))) {
            for (size_t i = 3; i < overviewScratch.size(); i += 4) overviewScratch[i] = 255;
            std::lock_guard<std::mutex> lock(overviewMutex);
            overview.swap(overviewScratch);
            overviewWidth = ow;
            overviewHeight = oh;
            ++overviewSeq;
        }
    }

    if (cfg.virtualCamera) {
        std::lock_guard<std::mutex> lock(vcamMutex);
        if (vcam.publish(f, nsToQpc(p.captureNs), &crop) == VcamOutput::State::Streaming) lastLivePublishNs = done;
    }

    if (owner->previewEnabled_ && cfg.previewMaxWidth > 0) {
        const uint32_t pw = std::min<uint32_t>(cfg.previewMaxWidth, uint32_t(crop.w)) & ~1u;
        const uint32_t ph = uint32_t(double(pw) * crop.h / std::max(1.0, crop.w)) & ~1u;
        previewScratch.resize(size_t(pw) * ph * 4);
        const auto view = vcam::packedView(vcam::kFourccRGB32, pw, ph, previewScratch.data());
        if (pw && ph && previewConverter.convert(f, view, &crop)) {
            for (size_t i = 3; i < previewScratch.size(); i += 4) previewScratch[i] = 255;  // BGR0 -> opaque BGRA
            if (cfg.debugFaces) {
                const double sx = pw / crop.w, sy = ph / crop.h;
                for (const Face& face : faces) {
                    drawBox(previewScratch, pw, ph, (face.x * f->width - crop.x) * sx, (face.y * f->height - crop.y) * sy,
                            ((face.x + face.w) * f->width - crop.x) * sx, ((face.y + face.h) * f->height - crop.y) * sy,
                            40, 220, 90);
                }
            }
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

        renegotiate(now);
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
    impl_->framingEnabled = impl_->cfg.framing;
    impl_->st.framing = impl_->cfg.framing;
    impl_->st.capture = {{"width", 0}, {"height", 0}, {"fps", 0}};
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

void Engine::setCamera(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(impl_->prefsMutex);
        if (impl_->wantedCamera == id) return;
        impl_->wantedCamera = id;
    }
    impl_->renegotiateNow = true;  // housekeeping applies it within 100 ms
}

void Engine::setFraming(bool enabled) {
    if (impl_->framingEnabled.exchange(enabled) == enabled) return;
    {
        std::lock_guard<std::mutex> lock(impl_->statusMutex);
        impl_->st.framing = enabled;
    }
    impl_->renegotiateNow = true;  // framing changes the capture size we ask for
}

void Engine::setFramingOptions(double tightness, double speed, bool group) {
    std::lock_guard<std::mutex> lock(impl_->prefsMutex);
    impl_->framingKnobs.setTightness(tightness);
    impl_->framingKnobs.speed = std::clamp(speed, 0.3, 3.0);
    impl_->framingKnobs.group = group;
}

void Engine::moveFraming(double centerX, double centerY) {
    double offsetX = 0, offsetY = 0;
    if (centerX >= 0 && centerY >= 0) {  // negative = back to where the framer would look
        std::lock_guard<std::mutex> lock(impl_->cropMutex);
        const double w = impl_->lastTargetW, h = w / impl_->lastOutAspect;
        if (!impl_->lastSrcW || w <= 0) return;
        offsetX = (std::clamp(centerX, 0.0, 1.0) * impl_->lastSrcW - impl_->lastBaseX) / w;
        offsetY = (std::clamp(centerY, 0.0, 1.0) * impl_->lastSrcH - impl_->lastBaseY) / h;
    }
    std::lock_guard<std::mutex> lock(impl_->prefsMutex);
    impl_->framingKnobs.offsetX = std::clamp(offsetX, -2.0, 2.0);
    impl_->framingKnobs.offsetY = std::clamp(offsetY, -2.0, 2.0);
}

void Engine::setControls(const nlohmann::json& controls) {
    if (!controls.is_object()) return;
    {
        std::lock_guard<std::mutex> lock(impl_->prefsMutex);
        for (auto it = controls.begin(); it != controls.end(); ++it) {
            if (it.key() == "zoom" || it.key() == "ramp") continue;  // zoom has its own path
            nlohmann::json v = it.value();
            if (v.is_object()) v.erase("point");  // a tap is a one-off, not a setting to restore
            impl_->rememberedControls[it.key()] = v;
        }
    }
    impl_->sendJson(Type::ControlSet, controls);
}

void Engine::tapToFocus(double x, double y) {
    CropRect crop;
    uint32_t w = 0, h = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->cropMutex);
        crop = impl_->lastCrop;
        w = impl_->lastSrcW;
        h = impl_->lastSrcH;
    }
    if (!w || !h) return;
    // Output picture -> whole camera frame (normalized); the phone maps it to sensor coordinates.
    const double fx = std::clamp((crop.x + std::clamp(x, 0.0, 1.0) * crop.w) / w, 0.0, 1.0);
    const double fy = std::clamp((crop.y + std::clamp(y, 0.0, 1.0) * crop.h) / h, 0.0, 1.0);
    impl_->sendJson(Type::ControlSet, {{"focus", {{"point", {fx, fy}}}}, {"exposure", {{"point", {fx, fy}}}}});
    char text[96];
    std::snprintf(text, sizeof text, "Focus and exposure at %.2f, %.2f of the camera view", fx, fy);
    impl_->log(text);
}

void Engine::setCapture(uint32_t width, uint32_t height, double fps) {
    {
        std::lock_guard<std::mutex> lock(impl_->prefsMutex);
        impl_->pinWidth = width;
        impl_->pinHeight = height;
        impl_->pinFps = fps;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->statusMutex);
        impl_->st.capture = {{"width", width}, {"height", height}, {"fps", fps}};
    }
    impl_->renegotiateNow = true;
}

bool Engine::copyOverview(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height, uint64_t& seq) const {
    std::lock_guard<std::mutex> lock(impl_->overviewMutex);
    if (impl_->overviewSeq == 0 || impl_->overviewSeq == seq) return false;
    bgra = impl_->overview;
    width = impl_->overviewWidth;
    height = impl_->overviewHeight;
    seq = impl_->overviewSeq;
    return true;
}

void Engine::setZoom(double display, bool smooth) {
    std::string camera;
    double factor = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->choiceMutex);
        camera = impl_->current.camera;
        const CameraInfo* c = impl_->caps.find(camera);
        if (!c || c->zoomScale <= 0) return;
        factor = std::clamp(display / c->zoomScale, c->minZoom, c->maxZoom);
    }
    {
        std::lock_guard<std::mutex> lock(impl_->prefsMutex);
        impl_->zoomFactors[camera] = factor;
    }
    impl_->sendJson(Type::ControlSet, {{"zoom", factor}, {"ramp", smooth ? 4.0 : 0.0}});
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
