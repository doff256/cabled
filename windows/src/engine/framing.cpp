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

void FramingSettings::setTightness(double t) { zoom = 1.0 + (kMaxZoom - 1.0) * std::clamp(t, 0.0, 1.0); }

double FramingSettings::tightness() const { return std::clamp((zoom - 1.0) / (kMaxZoom - 1.0), 0.0, 1.0); }

namespace {
// Width of the crop for the user's zoom, held back by the quality guard.
double zoomedWidth(const CropRect& full, uint32_t outW, const FramingSettings& s) {
    const double minW = std::min(full.w, double(outW) / std::max(1.0, s.maxUpscale));
    return std::clamp(full.w / std::max(1.0, s.zoom), minW, full.w);
}
}  // namespace

double reachableZoom(uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH, const FramingSettings& s) {
    if (!srcW || !srcH || !outW || !outH) return 1.0;
    const CropRect full = fillCrop(srcW, srcH, outW, outH);
    return std::max(1.0, full.w / std::min(full.w, double(outW) / std::max(1.0, s.maxUpscale)));
}

CropRect Framer::target(const Face& f, double faceW, uint32_t srcW, uint32_t srcH, uint32_t outW, uint32_t outH,
                        const FramingSettings& s) const {
    const CropRect full = fillCrop(srcW, srcH, outW, outH);
    const double aspect = double(outW) / double(outH);
    // The user's zoom; a group widens it so everyone fits with half a face of margin each side.
    double w = std::clamp(std::max(zoomedWidth(full, outW, s), (f.w + faceW) * srcW), 16.0, full.w);
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
    if (!srcW || !srcH || !outW || !outH) {
        initialized_ = false;
        return full;
    }
    const double zoomW = zoomedWidth(full, outW, s);
    const bool sameShape = initialized_ && srcW_ && srcH_ &&
                           std::abs(double(srcW) / srcH - double(srcW_) / srcH_) < 0.01;
    if (initialized_ && sameShape && (srcW != srcW_ || srcH != srcH_)) {
        // New capture resolution, same picture: carry the framing over instead of starting again.
        const double k = double(srcW) / srcW_;
        cx_ *= k, cy_ *= k, w_ *= k, tx_ *= k, ty_ *= k, tw_ *= k, vx_ *= k, vy_ *= k, vw_ *= k, bx_ *= k, by_ *= k;
        srcW_ = srcW;
        srcH_ = srcH;
    } else if (!initialized_ || srcW != srcW_ || srcH != srcH_) {  // first frame, or rotation
        initialized_ = true;
        srcW_ = srcW;
        srcH_ = srcH;
        cx_ = tx_ = bx_ = full.x + full.w / 2;
        cy_ = ty_ = by_ = full.y + full.h / 2;
        w_ = tw_ = zoomW;
        vx_ = vy_ = vw_ = 0;
        lastTime_ = now;
        primaryId_ = -1;
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

    // Group framing: everyone big enough to count (> 40% of the largest face), as one subject.
    Face groupBox;
    double groupFaceW = 0;
    if (s.group && faces.size() > 1) {
        double largest = 0;
        for (const Face& f : faces) largest = std::max(largest, f.w);
        double x0 = 1, y0 = 1, x1 = 0, y1 = 0, sumW = 0;
        int n = 0;
        for (const Face& f : faces) {
            if (f.w < 0.4 * largest) continue;
            x0 = std::min(x0, f.x);
            y0 = std::min(y0, f.y);
            x1 = std::max(x1, f.x + f.w);
            y1 = std::max(y1, f.y + f.h);
            sumW += f.w;
            ++n;
        }
        if (n > 1) {
            groupBox = {-2, x0, y0, x1 - x0, y1 - y0};
            groupFaceW = sumW / n;
            primary = &groupBox;
        }
    }

    // 2. Base point: the face(s) when following; the middle of the view when following is off.
    //    Nobody in view: stay where we are (they usually come back to the same place).
    if (!s.enabled) {
        primaryId_ = -1;
        bx_ = full.x + full.w / 2;
        by_ = full.y + full.h / 2;
        tw_ = zoomW;
    } else if (primary) {
        primaryId_ = primary->id;
        const double faceW = primary == &groupBox ? groupFaceW : primary->w;
        const CropRect t = target(*primary, faceW, srcW, srcH, outW, outH, s);
        const double ncx = t.x + t.w / 2, ncy = t.y + t.h / 2;
        // 3. Dead zone: small moves are noise (or someone breathing). Size follows the slider at once.
        const bool moved = std::abs(ncx - bx_) > s.deadZone * tw_ || std::abs(ncy - by_) > s.deadZone * tw_ / 2;
        const bool resized = std::abs(t.w - tw_) > 0.02 * tw_;
        if (moved || resized) {
            bx_ = ncx;
            by_ = ncy;
            tw_ = t.w;
        }
    } else {
        primaryId_ = -1;
        tw_ = zoomW;  // the slider still works with nobody in shot
    }
    // The user's nudge from the box-in-box (in crop sizes), then kept inside the frame.
    {
        const double aspect = double(outW) / double(outH);
        const double w = std::min(tw_, full.w), h = std::min(w / aspect, full.h);
        tx_ = std::clamp(bx_ + s.offsetX * w, w / 2, double(srcW) - w / 2);
        ty_ = std::clamp(by_ + s.offsetY * h, h / 2, double(srcH) - h / 2);
    }

    // 4. Critically damped springs (same feel at any frame rate); size moves slower than position.
    const double dt = std::clamp(now - lastTime_, 0.0, 0.1);
    lastTime_ = now;
    // ~0.65 s to cover 90% of a move, ~0.85 s for a zoom change at normal speed (R8: snappier).
    const double wPos = 6.0 * s.speed, wSize = 4.5 * s.speed;
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
