#include "negotiator.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace cabled::engine {

namespace {

std::vector<CameraFormat> parseFormats(const nlohmann::json& list) {
    std::vector<CameraFormat> out;
    if (!list.is_array()) return out;
    for (const auto& f : list) {
        CameraFormat c;
        c.width = f.value("width", 0u);
        c.height = f.value("height", 0u);
        c.minFps = f.value("minFps", 1.0);
        c.maxFps = f.value("maxFps", 30.0);
        c.binned = f.value("binned", false);
        if (c.width && c.height) out.push_back(c);
    }
    return out;
}

}  // namespace

std::string CameraInfo::label() const {
    // Front first: the TrueDepth front camera reports itself as a virtual device (colour + IR),
    // which once made the iPhone Air show "Auto" for its selfie camera.
    if (type == "front" || position == "front") return "Front";
    if (isVirtual) return "Auto";
    if (type == "ultraWide") return "Ultra Wide";
    if (type == "telephoto") return "Tele";
    return "Main";
}

std::vector<double> CameraInfo::presets() const {
    std::vector<double> p;
    auto add = [&](double display) {
        display = std::round(display * 10) / 10;
        if (display + 1e-6 < minDisplay() || display - 1e-6 > maxDisplay()) return;
        for (double existing : p) {
            if (std::abs(existing - display) < 0.05) return;
        }
        p.push_back(display);
    };
    add(minDisplay());
    for (double s : switchOver) add(s * zoomScale);  // each lens's native view
    add(1.0);
    add(2.0);  // the classic "2Ã—" crop of the main lens
    std::sort(p.begin(), p.end());
    return p;
}

const CameraInfo* PhoneCaps::find(const std::string& id) const {
    for (const auto& c : cameras) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

PhoneCaps parseCaps(const nlohmann::json& j) {
    PhoneCaps caps;
    if (!j.is_object()) return caps;
    caps.formats = parseFormats(j.value("formats", nlohmann::json::array()));
    for (const auto& codec : j.value("codecs", nlohmann::json::array())) {
        if (codec.is_string() && codec.get<std::string>() == "hevc") caps.hevc = true;
    }
    for (const auto& c : j.value("cameras", nlohmann::json::array())) {
        CameraInfo info;
        info.id = c.value("id", "");
        info.name = c.value("name", "");
        info.position = c.value("position", "back");
        info.type = c.value("type", "wide");
        info.isVirtual = c.value("virtual", false);
        info.minZoom = c.value("minZoom", 1.0);
        info.maxZoom = std::max(info.minZoom, c.value("maxZoom", 1.0));
        info.zoomScale = c.value("zoomScale", 1.0);
        info.defaultZoom = c.value("defaultZoom", info.minZoom);
        for (const auto& s : c.value("switchOver", nlohmann::json::array())) {
            if (s.is_number()) info.switchOver.push_back(s.get<double>());
        }
        info.formats = parseFormats(c.value("formats", nlohmann::json::array()));
        if (!info.id.empty()) caps.cameras.push_back(std::move(info));
    }
    caps.defaultCamera = j.value("defaultCamera", caps.cameras.empty() ? "" : caps.cameras.front().id);
    if (caps.formats.empty()) {
        if (const CameraInfo* d = caps.find(caps.defaultCamera)) caps.formats = d->formats;
    }
    caps.known = !caps.formats.empty();
    return caps;
}

nlohmann::json Choice::toConfigure(double zoomFactor) const {
    if (!enabled) return {{"enabled", false}};
    nlohmann::json j = {{"enabled", true}, {"codec", codec}, {"width", width}, {"height", height},
                        {"fps", fps},      {"bitrate", bitrate}, {"keyframeInterval", 2.0}};
    if (!camera.empty()) j["camera"] = camera;
    if (zoomFactor > 0) j["zoom"] = zoomFactor;
    return j;
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
    // Auto-framing crops into the picture, so capture about twice the output in each direction
    // (4K for a 1080p output) and the crop needs no upscaling (docs/video-pipeline.md, step 3).
    Want sized = want;
    if (want.framing) {
        sized.width = want.width * 2;
        sized.height = want.height * 2;
    }

    // 0. Which camera: the user's choice if the phone has it, else the phone's default.
    const CameraInfo* camera = caps.find(want.camera);
    if (!camera) camera = caps.find(caps.defaultCamera);
    if (camera) c.camera = camera->id;
    const std::vector<CameraFormat>& formats =
        camera && !camera->formats.empty() ? camera->formats : caps.formats;

    if (!caps.known || formats.empty()) {  // old companion: the R3 fixed setup
        c.width = 1920;
        c.height = 1080;
        c.fps = 30;
    } else {
        // 1. Formats that can do the frame rate (never go below the app's fps if avoidable).
        std::vector<CameraFormat> usable;
        for (const auto& f : formats) {
            if (f.maxFps + 0.01 >= fps && f.minFps - 0.01 <= fps) usable.push_back(f);
        }
        double chosenFps = fps;
        if (usable.empty()) {  // nothing that fast: take the fastest the phone has
            double best = 0;
            for (const auto& f : formats) best = std::max(best, f.maxFps);
            for (const auto& f : formats) {
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
            if (f.width == sized.width && f.height == sized.height && (!pick || better(f, *pick))) pick = f;
        }
        if (!pick) {
            for (const auto& f : usable) {
                const bool covers = f.width >= sized.width && f.height >= sized.height;
                const bool sameAspect = uint64_t(f.width) * sized.height == uint64_t(f.height) * sized.width;
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
