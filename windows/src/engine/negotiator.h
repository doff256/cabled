#pragma once
// Capability negotiation (R-57, docs/video-pipeline.md "Capability negotiation"). A pure function:
// what the consuming app wants + what the phone can do + how fast the link is -> CONFIGURE.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace cabled::engine {

struct CameraFormat {
    uint32_t width = 0;
    uint32_t height = 0;
    double minFps = 1;
    double maxFps = 30;
    bool binned = false;
};

// One camera the phone offers (R6): a physical lens, the front camera, or a virtual multi-lens
// device that switches lenses by itself while zooming.
struct CameraInfo {
    std::string id;        // phone's device id, opaque to us
    std::string name;      // e.g. "Back Triple Camera"
    std::string position;  // "back" / "front"
    std::string type;      // "triple", "dualWide", "dual", "wide", "ultraWide", "telephoto", "front"
    bool isVirtual = false;
    double minZoom = 1, maxZoom = 1;  // device zoom factors
    double zoomScale = 1;             // display zoom = factor * zoomScale ("1×" = main lens)
    double defaultZoom = 1;           // factor the phone starts at (virtual: the main lens)
    std::vector<double> switchOver;   // factors where a virtual device changes lens
    std::vector<CameraFormat> formats;

    std::string label() const;            // "Auto", "Main", "Ultra Wide", "Tele", "Front"
    std::vector<double> presets() const;  // display zoom values for quick buttons, e.g. 0.5, 1, 2, 3
    double minDisplay() const { return minZoom * zoomScale; }
    double maxDisplay() const { return maxZoom * zoomScale; }
};

struct PhoneCaps {
    std::vector<CameraFormat> formats;  // default camera (pre-R6 phones only send this)
    std::vector<CameraInfo> cameras;
    std::string defaultCamera;
    bool hevc = false;
    bool known = false;  // false = phone sent no CAPS (assume 1080p30 H.264)
    const CameraInfo* find(const std::string& id) const;
};
PhoneCaps parseCaps(const nlohmann::json& j);

struct Want {
    bool consumer = false;  // an app has the camera, or the preview is visible
    uint32_t width = 1920;  // what the app chose (default when only the preview wants video)
    uint32_t height = 1080;
    double fps = 30;
    std::string camera;     // user's choice; empty = phone's default
};

struct Choice {
    bool enabled = false;
    std::string camera;  // empty = phone's default
    uint32_t width = 1920;
    uint32_t height = 1080;
    double fps = 30;
    std::string codec = "h264";
    uint32_t bitrate = 25'000'000;
    bool operator==(const Choice&) const = default;
    // zoomFactor <= 0: let the phone use its default for the camera.
    nlohmann::json toConfigure(double zoomFactor = 0) const;
    std::string describe() const;
};

// H.264 bitrate for a size/rate, from the video-pipeline.md table (25 Mbit/s at 1080p30).
uint32_t defaultBitrate(uint32_t width, uint32_t height, double fps);

// linkMbps <= 0 means unknown (assume a USB 2 iPhone, ~200 Mbit/s usable).
Choice negotiate(const PhoneCaps& caps, const Want& want, double linkMbps);

}  // namespace cabled::engine
