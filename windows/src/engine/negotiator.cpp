#include "negotiator.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace cabled::engine {

PhoneCaps parseCaps(const nlohmann::json& j) {
    PhoneCaps caps;
    if (!j.is_object()) return caps;
    for (const auto& f : j.value("formats", nlohmann::json::array())) {
        CameraFormat c;
        c.width = f.value("width", 0u);
        c.height = f.value("height", 0u);
        c.minFps = f.value("minFps", 1.0);
        c.maxFps = f.value("maxFps", 30.0);
        c.binned = f.value("binned", false);
        if (c.width && c.height) caps.formats.push_back(c);
    }
    for (const auto& codec : j.value("codecs", nlohmann::json::array())) {
        if (codec.is_string() && codec.get<std::string>() == "hevc") caps.hevc = true;
    }
    caps.known = !caps.formats.empty();
    return caps;
}

nlohmann::json Choice::toConfigure() const {
    if (!enabled) return {{"enabled", false}};
    return {{"enabled", true}, {"codec", codec}, {"width", width}, {"height", height},
            {"fps", fps},      {"bitrate", bitrate}, {"keyframeInterval", 2.0}};
}

std::string Choice::describe() const {
    if (!enabled) return "video off (nobody is watching)";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%ux%u@%.0f %s %.0f Mbit/s", width, height, fps, codec == "hevc" ? "HEVC" : "H.264",
                  bitrate / 1e6);
    return buf;
}

uint32_t defaultBitrate(uint32_t width, uint32_t height, double fps) {
    // Fits the table: 720p30 ~15, 1080p30 25, 1080p60 ~40, 4K30 ~66, 4K60 ~107 Mbit/s.
    const double ratio = (double(width) * height * fps) / (1920.0 * 1080.0 * 30.0);
    return uint32_t(25e6 * std::pow(std::max(ratio, 0.05), 0.7));
}

Choice negotiate(const PhoneCaps& caps, const Want& want, double linkMbps) {
    Choice c;
    if (!want.consumer) return c;  // enabled = false: let the phone's camera rest
    c.enabled = true;
    const double fps = want.fps > 0 ? want.fps : 30;

    if (!caps.known) {  // old companion: the R3 fixed setup
        c.width = 1920;
        c.height = 1080;
        c.fps = 30;
    } else {
        // 1. Formats that can do the frame rate (never go below the app's fps if avoidable).
        std::vector<CameraFormat> usable;
        for (const auto& f : caps.formats) {
            if (f.maxFps + 0.01 >= fps && f.minFps - 0.01 <= fps) usable.push_back(f);
        }
        double chosenFps = fps;
        if (usable.empty()) {  // nothing that fast: take the fastest the phone has
            double best = 0;
            for (const auto& f : caps.formats) best = std::max(best, f.maxFps);
            for (const auto& f : caps.formats) {
                if (f.maxFps >= best - 0.01) usable.push_back(f);
            }
            chosenFps = best;
        }
        // 2. Exact size, else the smallest that covers the app's size (no upscaling), else the largest.
        auto better = [](const CameraFormat& a, const CameraFormat& b) {  // prefer unbinned, then smaller
            if (a.binned != b.binned) return !a.binned;
            return uint64_t(a.width) * a.height < uint64_t(b.width) * b.height;
        };
        std::optional<CameraFormat> pick;
        for (const auto& f : usable) {
            if (f.width == want.width && f.height == want.height && (!pick || better(f, *pick))) pick = f;
        }
        if (!pick) {
            for (const auto& f : usable) {
                const bool covers = f.width >= want.width && f.height >= want.height;
                const bool sameAspect = uint64_t(f.width) * want.height == uint64_t(f.height) * want.width;
                if (covers && sameAspect && (!pick || better(f, *pick))) pick = f;
            }
        }
        if (!pick) {
            for (const auto& f : usable) {
                if (!pick || uint64_t(f.width) * f.height > uint64_t(pick->width) * pick->height) pick = f;
            }
        }
        c.width = pick->width;
        c.height = pick->height;
        c.fps = chosenFps;
    }

    // 3. Codec and bitrate against the link budget (70% rule).
    const double budget = (linkMbps > 0 ? linkMbps : 200.0) * 0.7 * 1e6;
    c.bitrate = defaultBitrate(c.width, c.height, c.fps);
    if (c.bitrate > budget && caps.hevc) {
        c.codec = "hevc";
        c.bitrate = uint32_t(c.bitrate * 0.7);  // HEVC: same quality for ~30% fewer bits
    }
    c.bitrate = uint32_t(std::min<double>(c.bitrate, budget));
    return c;
}

}  // namespace cabled::engine
