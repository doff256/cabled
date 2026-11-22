#include "image_adjust.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cabled::engine {

bool ImageAdjust::neutral() const {
    return std::abs(brightness) < 1e-3 && std::abs(contrast - 1) < 1e-3 && std::abs(saturation - 1) < 1e-3 &&
           sharpness < 1e-3 && !mirror;
}

void ImageAdjuster::buildLuts(const ImageAdjust& a, bool fullRange) {
    // Luma (or an RGB channel): contrast around mid-grey, then brightness, in the format's range.
    const double lo = fullRange ? 0 : 16, hi = fullRange ? 255 : 235;
    const double contrast = std::clamp(a.contrast, 0.0, 2.0), brightness = std::clamp(a.brightness, -1.0, 1.0);
    for (int v = 0; v < 256; ++v) {
        double n = (v - lo) / (hi - lo);
        n = (n - 0.5) * contrast + 0.5 + 0.25 * brightness;
        luma_[size_t(v)] = uint8_t(std::lround(std::clamp(lo + n * (hi - lo), lo, hi)));
    }
    // Chroma: distance from neutral (128) scaled by saturation, in 16..240.
    const double saturation = std::clamp(a.saturation, 0.0, 2.0);
    for (int v = 0; v < 256; ++v) {
        chroma_[size_t(v)] = uint8_t(std::lround(std::clamp(128 + (v - 128) * saturation, 16.0, 240.0)));
    }
    lutFor_ = a;
    lutFullRange_ = fullRange;
    lutValid_ = true;
}

void ImageAdjuster::apply(const vcam::FrameView& f, const ImageAdjust& a) {
    if (a.neutral() || !f.valid()) return;
    const bool rgb = f.fourcc == vcam::kFourccRGB32;
    if (!lutValid_ || !(lutFor_ == a) || lutFullRange_ != rgb) buildLuts(a, rgb);
    const bool tone = std::abs(a.brightness) >= 1e-3 || std::abs(a.contrast - 1) >= 1e-3;
    const bool sat = std::abs(a.saturation - 1) >= 1e-3;
    const uint32_t w = f.width, h = f.height;

    if (f.fourcc == vcam::kFourccNV12) {
        // Sharpen first (on the original luma), then tone, then mirror.
        if (a.sharpness >= 1e-3 && w >= 3 && h >= 3) {
            // Unsharp mask with a 3x3 box blur; keeps two original rows so it can work in place.
            // The box blur is separable: horizontal 3-sums per row, then three rows added. That's
            // ~3 additions per pixel instead of 9 loads (R14: 4K60 must fit in a frame).
            const int k = int(std::lround(std::clamp(a.sharpness, 0.0, 1.0) * 256 * 1.5));  // amount, /256
            rows_.resize(size_t(w) * 2);
            hsums_.resize(size_t(w) * 3);
            uint8_t* cur = rows_.data();    // this row's original luma (before it's overwritten)
            uint8_t* next = cur + w;        // next row's original luma
            uint16_t* hPrev = hsums_.data();
            uint16_t* hCur = hPrev + w;
            uint16_t* hNext = hCur + w;
            auto hsum = [w](const uint8_t* r, uint16_t* out) {
                out[0] = uint16_t(r[0] * 2 + r[1]);  // edges repeat the border pixel
                for (uint32_t x = 1; x + 1 < w; ++x) out[x] = uint16_t(r[x - 1] + r[x] + r[x + 1]);
                out[w - 1] = uint16_t(r[w - 2] + r[w - 1] * 2);
            };
            std::memcpy(cur, f.plane0, w);
            hsum(cur, hCur);
            std::memcpy(hPrev, hCur, size_t(w) * sizeof(uint16_t));
            const int64_t gain = (int64_t(k) << 16) / (9 * 256);  // detail * gain >> 16 == detail * k / (9*256)
            for (uint32_t y = 0; y < h; ++y) {
                uint8_t* out = f.plane0 + size_t(y) * f.pitch0;
                if (y + 1 < h) {
                    std::memcpy(next, f.plane0 + size_t(y + 1) * f.pitch0, w);
                    hsum(next, hNext);
                } else {
                    std::memcpy(hNext, hCur, size_t(w) * sizeof(uint16_t));
                }
                for (uint32_t x = 0; x < w; ++x) {
                    const int c = cur[x];
                    const int detail = c * 9 - (hPrev[x] + hCur[x] + hNext[x]);  // 9 x (pixel - blur)
                    const int v = c + int((detail * gain) >> 16);
                    out[x] = luma_[size_t(v < 16 ? 16 : v > 235 ? 235 : v)];  // tone folded in (identity if neutral)
                }
                std::swap(cur, next);
                uint16_t* t = hPrev;
                hPrev = hCur;
                hCur = hNext;
                hNext = t;
            }
        }
        const bool sharpened = a.sharpness >= 1e-3 && w >= 3 && h >= 3;  // tone already applied above
        if ((tone && !sharpened) || a.mirror) {
            for (uint32_t y = 0; y < h; ++y) {
                uint8_t* row = f.plane0 + size_t(y) * f.pitch0;
                if (tone && !sharpened) for (uint32_t x = 0; x < w; ++x) row[x] = luma_[row[x]];
                if (a.mirror) std::reverse(row, row + w);
            }
        }
        for (uint32_t y = 0; y < h / 2; ++y) {
            uint8_t* row = f.plane1 + size_t(y) * f.pitch1;
            if (sat) for (uint32_t x = 0; x < (w & ~1u); ++x) row[x] = chroma_[row[x]];
            if (a.mirror) {  // reverse U,V pairs
                for (uint32_t i = 0, j = (w & ~1u) - 2; i < j; i += 2, j -= 2) {
                    std::swap(row[i], row[j]);
                    std::swap(row[i + 1], row[j + 1]);
                }
            }
        }
        return;
    }

    if (f.fourcc == vcam::kFourccYUY2) {  // Y0 U Y1 V per two pixels
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = f.plane0 + size_t(y) * f.pitch0;
            const uint32_t bytes = (w & ~1u) * 2;
            for (uint32_t i = 0; i < bytes; i += 4) {
                if (tone) row[i] = luma_[row[i]], row[i + 2] = luma_[row[i + 2]];
                if (sat) row[i + 1] = chroma_[row[i + 1]], row[i + 3] = chroma_[row[i + 3]];
            }
            if (a.mirror) {
                for (uint32_t i = 0, j = bytes - 4; i < j; i += 4, j -= 4) {
                    for (int b = 0; b < 4; ++b) std::swap(row[i + b], row[j + b]);
                }
                for (uint32_t i = 0; i < bytes; i += 4) std::swap(row[i], row[i + 2]);  // Y0/Y1 inside each pair
            }
        }
        return;
    }

    if (rgb) {  // B G R X, full range
        const double s = std::clamp(a.saturation, 0.0, 2.0);
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t* row = f.plane0 + size_t(y) * f.pitch0;
            for (uint32_t x = 0; x < w; ++x) {
                uint8_t* p = row + size_t(x) * 4;
                if (tone) p[0] = luma_[p[0]], p[1] = luma_[p[1]], p[2] = luma_[p[2]];
                if (sat) {
                    const double l = 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2];  // BT.709 luma
                    for (int c = 0; c < 3; ++c) p[c] = uint8_t(std::clamp(std::lround(l + (p[c] - l) * s), 0L, 255L));
                }
            }
            if (a.mirror) {
                auto* px = reinterpret_cast<uint32_t*>(row);
                std::reverse(px, px + w);
            }
        }
    }
}

}  // namespace cabled::engine
