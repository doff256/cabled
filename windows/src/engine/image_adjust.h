#pragma once
// Image adjustments (R9, windows-app.md "Image" page): brightness, contrast, saturation,
// sharpness and mirroring, applied in place to the converted output picture (the virtual camera's
// frame and the preview), so the preview shows exactly what apps get. CPU, one pass, skipped
// entirely when everything is neutral.

#include "vcam_shared.h"

#include <array>
#include <cstdint>
#include <vector>

namespace cabled::engine {

struct ImageAdjust {
    double brightness = 0;  // -1 .. 1   (0 = neutral; 1 lifts by a quarter of the range)
    double contrast = 1;    //  0 .. 2   (1 = neutral)
    double saturation = 1;  //  0 .. 2   (1 = neutral, 0 = black and white)
    double sharpness = 0;   //  0 .. 1   (0 = off; unsharp mask on luma, NV12 only)
    bool mirror = false;    //  flip left-right

    bool neutral() const;
    bool operator==(const ImageAdjust&) const = default;
};

class ImageAdjuster {
public:
    // Adjusts `frame` in place. NV12, YUY2 and RGB32 (BGRX) are supported; others are left alone.
    void apply(const vcam::FrameView& frame, const ImageAdjust& a);

private:
    void buildLuts(const ImageAdjust& a, bool fullRange);
    ImageAdjust lutFor_{};
    bool lutFullRange_ = false;
    bool lutValid_ = false;
    std::array<uint8_t, 256> luma_{};    // brightness + contrast (Y, or each RGB channel)
    std::array<uint8_t, 256> chroma_{};  // saturation (U, V)
    std::vector<uint8_t> rows_;          // sharpening scratch: the previous source rows
};

}  // namespace cabled::engine
