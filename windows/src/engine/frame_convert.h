#pragma once
// Decoded frame -> the format the consuming app chose. Plain copy when it already matches (no
// resampling, R-56); swscale otherwise. Temporary CPU path until the GPU pipeline (R7).

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

    bool convert(const AVFrame* src, const vcam::FrameView& dst);
    bool lastWasCopy() const { return lastWasCopy_; }

private:
    SwsContext* sws_ = nullptr;
    bool lastWasCopy_ = false;
};

// View of an NV12 AVFrame's planes (for copies and stamp reading). Invalid for other formats.
vcam::FrameView viewOf(const AVFrame* frame);

}  // namespace cabled::engine
