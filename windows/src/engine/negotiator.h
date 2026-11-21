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

struct PhoneCaps {
    std::vector<CameraFormat> formats;
    bool hevc = false;
    bool known = false;  // false = phone sent no CAPS (assume 1080p30 H.264)
};
PhoneCaps parseCaps(const nlohmann::json& j);

struct Want {
    bool consumer = false;  // an app has the camera, or the preview is visible
    uint32_t width = 1920;  // what the app chose (default when only the preview wants video)
    uint32_t height = 1080;
    double fps = 30;
};

struct Choice {
    bool enabled = false;
    uint32_t width = 1920;
    uint32_t height = 1080;
    double fps = 30;
    std::string codec = "h264";
    uint32_t bitrate = 25'000'000;
    bool operator==(const Choice&) const = default;
    nlohmann::json toConfigure() const;
    std::string describe() const;
};

// H.264 bitrate for a size/rate, from the video-pipeline.md table (25 Mbit/s at 1080p30).
uint32_t defaultBitrate(uint32_t width, uint32_t height, double fps);

// linkMbps <= 0 means unknown (assume a USB 2 iPhone, ~200 Mbit/s usable).
Choice negotiate(const PhoneCaps& caps, const Want& want, double linkMbps);

}  // namespace cabled::engine
