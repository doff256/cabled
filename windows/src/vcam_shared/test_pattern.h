#pragma once
// Test images for the virtual camera spike. The feeder pattern carries two machine-readable bands at
// the top: band 0 = the 64-bit QPC time the frame was drawn, band 1 = a 32-bit frame number. Each bit
// is a black or white block, so it survives format conversion and mild scaling, and a capture tool
// can measure latency through the whole stack (feeder -> Frame Server -> app).

#include "vcam_shared.h"

#include <cstdint>

namespace cabled::vcam {

struct Rgb {
    uint8_t r, g, b;
};

void fillRect(const FrameView& f, int x0, int y0, int x1, int y1, Rgb color);

// Feeder pattern: colour bars, a moving box, stamp bands.
void drawTestPattern(const FrameView& f, uint32_t frameNumber, int64_t stampQpc);

// What the media source shows when no engine frames arrive: dark bars and a sliding stripe.
// Its stamp bands are all black, so they decode to zero.
void drawPlaceholder(const FrameView& f, uint64_t tick);

struct Stamp {
    int64_t qpc = 0;
    uint32_t frame = 0;
};
Stamp readStamp(const FrameView& f);

}  // namespace cabled::vcam
