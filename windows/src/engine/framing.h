#pragma once
// Auto-framing (R-42, docs/camera-controls-and-framing.md). Pure: faces in, crop rectangle out, so
// it's unit-testable and replayable. The phone detects faces (camera hardware, D-10); this decides
// where to look and how to move.

#include <cstdint>
#include <vector>

namespace cabled::engine {

struct Face {
    int id = -1;  // stable while the phone keeps tracking the same face
    double x = 0, y = 0, w = 0, h = 0;  // normalized to the decoded frame (0..1, top-left origin)
};

struct CropRect {
    double x = 0, y = 0, w = 0, h = 0;  // source pixels
};

struct FramingSettings {
    bool enabled = true;      // R-42: on by default
    bool group = false;       // frame everyone in view instead of one person
    double headroom = 0.18;   // top of the face at this fraction of the crop height
    double widthFactor = 3.2; // crop width = face width x this (head and shoulders)
    double maxUpscale = 1.0;  // quality guard: crop never smaller than output / this (1 = no upscaling)
    double deadZone = 0.05;   // ignore moves smaller than this fraction of the crop width
    double speed = 1.0;       // spring speed multiplier (calm < 1 < fast)
    double holdSeconds = 1.5; // keep framing this long after the face is lost

    // User-facing knobs (R8). tightness 0 = show more (wide), 1 = show less (tight).
    void setTightness(double tightness);
    double tightness() const;
};

// Largest centered rectangle with the output's aspect ratio (fills the output, crops the rest).
CropRect fillCrop(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH);

class Framer {
public:
    // Call once per output frame. `now` in seconds (any monotonic origin).
    CropRect update(const std::vector<Face>& faces, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
                    double now, const FramingSettings& settings);
    void reset() { initialized_ = false; }

private:
    // `subject`: one face, or (group framing) the union of everyone; `faceW` drives the width.
    CropRect target(const Face& subject, double faceW, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
                    const FramingSettings& s) const;

    bool initialized_ = false;
    uint32_t srcW_ = 0, srcH_ = 0;
    double cx_ = 0, cy_ = 0, w_ = 0;    // current crop centre and width (pixels)
    double vx_ = 0, vy_ = 0, vw_ = 0;   // spring velocities
    double tx_ = 0, ty_ = 0, tw_ = 0;   // current target
    double lastTime_ = 0;
    int primaryId_ = -1;
    double lastSeen_ = -1e9;
};

}  // namespace cabled::engine
