#pragma once
// Decoded frame -> the format the consuming app chose. Plain copy when it already matches (no
// resampling, R-56); swscale otherwise. Temporary CPU path until the GPU pipeline (R7).

#include "framing.h"
#include "vcam_shared.h"

struct AVFrame;
struct SwsContext;

namespace cabled::engine {

class FrameConverter {
public:
    FrameConverter() = default;
    ~FrameConverter();
    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    // crop (source pixels; null = whole frame) is scaled to fill dst. A crop exactly dst's size in
    // the same format is a straight copy (no resampling, R-56).
    bool convert(const AVFrame* src, const vcam::FrameView& dst, const CropRect* crop = nullptr);
    // Same, from a top-down BGRA image (e.g. a rendered status card).
    bool convertBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride, const vcam::FrameView& dst);
    bool lastWasCopy() const { return lastWasCopy_; }

private:
    SwsContext* sws_ = nullptr;
    bool lastWasCopy_ = false;
};

// View of an NV12 AVFrame's planes (for copies and stamp reading). Invalid for other formats.
vcam::FrameView viewOf(const AVFrame* frame);

}  // namespace cabled::engine
