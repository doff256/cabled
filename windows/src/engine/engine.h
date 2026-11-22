#pragma once
// The Cabled engine (R4): finds the phone, keeps the connection alive, decodes video and feeds the
// virtual camera and the preview. Long-running; reconnects by itself (docs/architecture.md state
// machine, docs/reliability-and-testing.md F1-F5, F9, F17).
//
// Threads:
//   connection  find phone -> connect -> handshake -> receive loop; reconnect with backoff
//   decode      bounded packet queue -> FFmpeg -> virtual camera + preview
//   housekeeping  pings + watchdog, status cards into the camera when there's no live video, stats

#include "image_adjust.h"
#include "net.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct AVFrame;

namespace cabled::engine {

enum class EngineState {
    Stopped,
    NoService,            // Apple Mobile Device Service not reachable (F2)
    NoDevice,             // no iPhone on USB (F1)
    WaitingForCompanion,  // iPhone there, app not listening (F4, F18)
    Connecting,           // handshake / waiting for the first frame
    Incompatible,         // protocol mismatch (F17)
    Streaming,
    Idle,                 // connected, video off because nobody is watching (R5)
    Paused,               // phone reports its camera is interrupted (F5, F6, F7)
    Error,                // phone refused the configuration etc.; retried
};
const char* stateName(EngineState s);

struct EngineStatus {
    EngineState state = EngineState::Stopped;
    std::string headline;  // short, for the UI / status card
    std::string detail;    // what to do next
    std::string transport;  // "usb" or "tcp"
    std::string deviceModel, deviceSystem, deviceId, companionVersion;
    uint32_t width = 0, height = 0;
    double fps = 0, mbps = 0, decodeMs = 0, latencyMs = 0;
    bool hardwareDecode = false;
    uint64_t framesDecoded = 0, keyframeRequests = 0, reconnects = 0;
    std::string vcam;  // virtual camera state, e.g. "STREAMING NV12 1920x1080@30"
    bool vcamInUse = false;
    std::string thermal;
    double battery = -1;
    double linkMbps = 0;     // measured at connect (R5)
    std::string videoConfig;  // what was negotiated, e.g. "1920x1080@60 H.264 40 Mbit/s"
    // Cameras and zoom (R6). `cameras`: [{id, label, name, type, minZoom, maxZoom, presets}] in
    // display units ("1×" = main lens).
    nlohmann::json cameras = nlohmann::json::array();
    std::string camera;    // id in use
    double zoom = 1;       // display units, as the phone last reported
    std::string lens;      // physical lens in use (virtual cameras switch by themselves)
    bool framing = true;   // auto-framing on (R7)
    int faces = 0;         // faces the phone currently sees
    // R8
    nlohmann::json framingInfo = nlohmann::json::object();  // knobs + current crop (normalized)
    nlohmann::json controls = nlohmann::json::object();     // phone's camera state: iso, shutter, focus, wb...
    nlohmann::json capture = nlohmann::json::object();      // pinned capture size/fps (0 = auto)
    nlohmann::json toJson() const;
};

struct EngineConfig {
    std::string tcpTarget;  // "ip:port" -> talk to a fake phone over TCP; empty -> USB
    std::string udid;       // USB: a specific iPhone (empty = first)
    uint16_t devicePort = 47800;
    uint32_t width = 1920, height = 1080;
    double fps = 30;
    uint32_t bitrate = 25'000'000;
    bool hardwareDecode = true;
    bool virtualCamera = true;
    uint32_t previewMaxWidth = 960;  // 0 = no preview
    double watchdogSeconds = 5;      // silence before the connection is dropped
    bool framing = true;             // auto-framing on at start (R-42)
    bool debugFaces = false;         // draw detected faces and the framing target into the preview
    double retrySeconds = 1;
    std::function<void(const std::string&)> log;
    // Tests: every decoded frame (decode thread).
    std::function<void(const AVFrame*, uint32_t frameSeq)> frameSink;
};

class Engine {
public:
    explicit Engine(EngineConfig config);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();
    void stop();

    EngineStatus status() const;
    // Latest preview frame (BGRA, top-down). Returns false if there's nothing newer than `seq`.
    bool copyPreview(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height, uint64_t& seq) const;
    void setPreviewEnabled(bool enabled) { previewEnabled_ = enabled; }

    // R6. Camera by id from status().cameras (empty = phone default); applied at once.
    void setCamera(const std::string& id);
    // Zoom in display units ("1" = main lens, "0.5" = ultra wide). smooth = ramp instead of jump.
    void setZoom(double display, bool smooth);
    // R7: auto-framing on/off (also changes what the phone is asked to capture).
    void setFraming(bool enabled);

    // R8. tightness 0 = show more .. 1 = show less; speed 0.6 calm, 1 normal, 1.6 fast.
    void setFramingOptions(double tightness, double speed, bool group);
    // Focus / exposure / white balance: a CONTROL_SET body, e.g. {"exposure":{"mode":"auto","bias":0.3}}.
    // Remembered and re-applied after reconnects and camera changes.
    void setControls(const nlohmann::json& controls);
    // Tap on the preview (0..1 in the output picture) -> focus and expose there.
    void tapToFocus(double x, double y);
    // Box-in-box drag: put the crop's centre here (normalized whole camera view). The nudge is kept
    // relative to the face, so following carries on. Negative values remove the nudge.
    void moveFraming(double centerX, double centerY);
    // R9: image adjustments for the camera and the preview (neutral = off, no cost).
    void setImageAdjust(const ImageAdjust& adjust);
    // Pin the capture size / frame rate (0 = automatic).
    void setCapture(uint32_t width, uint32_t height, double fps);
    // Small picture of the whole camera view (the crop is status().framingInfo["crop"]).
    bool copyOverview(std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height, uint64_t& seq) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> previewEnabled_{true};
};

}  // namespace cabled::engine
