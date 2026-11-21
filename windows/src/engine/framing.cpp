#include "framing.h"

#include <algorithm>
#include <cmath>

namespace cabled::engine {

CropRect fillCrop(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH) {
    CropRect r{0, 0, double(srcW), double(srcH)};
    if (!srcW || !srcH || !outW || !outH) return r;
    const double outAspect = double(outW) / double(outH);
    if (double(srcW) / double(srcH) > outAspect) {
        r.w = double(srcH) * outAspect;  // source wider: trim the sides
        r.x = (double(srcW) - r.w) / 2;
    } else {
        r.h = double(srcW) / outAspect;  // source taller (portrait phone): trim top and bottom
        r.y = (double(srcH) - r.h) / 2;
    }
    return r;
}

CropRect Framer::target(const Face& f, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
                        const FramingSettings& s) const {
    const CropRect full = fillCrop(srcW, srcH, outW, outH);
    const double aspect = double(outW) / double(outH);
    // Head and shoulders, but never smaller than the quality guard allows, never bigger than the frame.
    const double minW = std::min(full.w, double(outW) / std::max(1.0, s.maxUpscale));
    double w = std::clamp(f.w * srcW * s.widthFactor, minW, full.w);
    double h = w / aspect;
    const double faceCx = (f.x + f.w / 2) * srcW;
    const double faceTop = f.y * srcH;
    double x = faceCx - w / 2;
    double y = faceTop - s.headroom * h;
    x = std::clamp(x, 0.0, double(srcW) - w);
    y = std::clamp(y, 0.0, double(srcH) - h);
    return {x, y, w, h};
}

CropRect Framer::update(const std::vector<Face>& faces, uint32_t srcW, uint32_t srcH, uint32_t outW,
                        uint32_t outH, double now, const FramingSettings& s) {
    const CropRect full = fillCrop(srcW, srcH, outW, outH);
    if (!s.enabled || !srcW || !srcH || !outW || !outH) {
        initialized_ = false;
        return full;
    }
    if (!initialized_ || srcW != srcW_ || srcH != srcH_) {  // first frame, or rotation/format change
        initialized_ = true;
        srcW_ = srcW;
        srcH_ = srcH;
        cx_ = tx_ = full.x + full.w / 2;
        cy_ = ty_ = full.y + full.h / 2;
        w_ = tw_ = full.w;
        vx_ = vy_ = vw_ = 0;
        lastTime_ = now;
        primaryId_ = -1;
        lastSeen_ = -1e9;
    }

    // 1. Primary face: keep the one we follow while it's visible; otherwise the most prominent
    //    (largest, weighted towards the centre).
    const Face* primary = nullptr;
    for (const Face& f : faces) {
        if (f.id == primaryId_ && f.id >= 0) primary = &f;
    }
    if (!primary) {
        double best = 0;
        for (const Face& f : faces) {
            const double dx = (f.x + f.w / 2) - 0.5, dy = (f.y + f.h / 2) - 0.5;
            const double score = f.w * f.h * (1.0 - 0.5 * std::min(1.0, std::sqrt(dx * dx + dy * dy)));
            if (score > best) {
                best = score;
                primary = &f;
            }
        }
    }

    // 2. Target: frame the face; after losing it, hold, then go back to the full view.
    if (primary) {
        primaryId_ = primary->id;
        lastSeen_ = now;
        const CropRect t = target(*primary, srcW, srcH, outW, outH, s);
        const double ncx = t.x + t.w / 2, ncy = t.y + t.h / 2;
        // 3. Dead zone: small moves and size changes are noise (or someone breathing).
        const bool moved = std::abs(ncx - tx_) > s.deadZone * tw_ || std::abs(ncy - ty_) > s.deadZone * tw_ / 2;
        const bool resized = std::abs(t.w - tw_) > 0.10 * tw_;
        if (moved || resized) {
            tx_ = ncx;
            ty_ = ncy;
            tw_ = t.w;
        }
    } else if (now - lastSeen_ > s.holdSeconds) {
        tx_ = full.x + full.w / 2;
        ty_ = full.y + full.h / 2;
        tw_ = full.w;
        primaryId_ = -1;
    }

    // 4. Critically damped springs (same feel at any frame rate); size moves slower than position.
    const double dt = std::clamp(now - lastTime_, 0.0, 0.1);
    lastTime_ = now;
    // ~1.3 s to cover 90% of a move, ~1.6 s for a zoom change: calm, not laggy.
    const double wPos = 3.5 * s.speed, wSize = 2.5 * s.speed;
    auto spring = [dt](double& x, double& v, double target, double omega) {
        const double a = omega * omega * (target - x) - 2.0 * omega * v;
        v += a * dt;
        x += v * dt;
    };
    spring(cx_, vx_, tx_, wPos);
    spring(cy_, vy_, ty_, wPos);
    spring(w_, vw_, tw_, wSize);

    // 5. Output rectangle, kept inside the frame and at the output's aspect ratio.
    const double aspect = double(outW) / double(outH);
    CropRect r;
    r.w = std::clamp(w_, 16.0, full.w);
    r.h = r.w / aspect;
    if (r.h > full.h) {
        r.h = full.h;
        r.w = r.h * aspect;
    }
    r.x = std::clamp(cx_ - r.w / 2, 0.0, double(srcW) - r.w);
    r.y = std::clamp(cy_ - r.h / 2, 0.0, double(srcH) - r.h);
    return r;
}

}  // namespace cabled::engine
