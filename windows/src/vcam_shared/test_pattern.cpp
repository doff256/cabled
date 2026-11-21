#include "test_pattern.h"

#include <algorithm>
#include <cstring>

namespace cabled::vcam {
namespace {

struct Yuv {
    uint8_t y, u, v;
};

uint8_t clamp8(double v) { return static_cast<uint8_t>(std::clamp(v + 0.5, 0.0, 255.0)); }

// BT.709, limited (video) range: what the virtual camera advertises.
Yuv toYuv(Rgb c) {
    const double r = c.r, g = c.g, b = c.b;
    return {clamp8(16.0 + (0.2126 * r + 0.7152 * g + 0.0722 * b) * 219.0 / 255.0),
            clamp8(128.0 + (-0.1146 * r - 0.3854 * g + 0.5 * b) * 224.0 / 255.0),
            clamp8(128.0 + (0.5 * r - 0.4542 * g - 0.0458 * b) * 224.0 / 255.0)};
}

int blockWidth(const FrameView& f) { return std::max<int>(4, int(f.width / 80) & ~1); }
int bandHeight(const FrameView& f) { return std::max(8, blockWidth(f)); }

constexpr Rgb kBlack{0, 0, 0};
constexpr Rgb kWhite{255, 255, 255};

void drawBits(const FrameView& f, int band, uint64_t value, int bits) {
    const int bw = blockWidth(f);
    const int bh = bandHeight(f);
    for (int i = 0; i < bits; ++i) {
        const bool one = (value >> i) & 1;
        fillRect(f, i * bw, band * bh, (i + 1) * bw, (band + 1) * bh, one ? kWhite : kBlack);
    }
}

uint64_t readBits(const FrameView& f, int band, int bits) {
    const int bw = blockWidth(f);
    const int bh = bandHeight(f);
    const int y = band * bh + bh / 2;
    uint64_t value = 0;
    for (int i = 0; i < bits; ++i) {
        const int x = i * bw + bw / 2;
        if (x >= int(f.width) || y >= int(f.height)) break;
        uint8_t luma = 0;
        switch (f.fourcc) {
            case kFourccNV12: luma = f.plane0[size_t(y) * f.pitch0 + x]; break;
            case kFourccYUY2: luma = f.plane0[size_t(y) * f.pitch0 + size_t(x) * 2]; break;
            case kFourccRGB32: luma = f.plane0[size_t(y) * f.pitch0 + size_t(x) * 4 + 1]; break;  // green
            default: return 0;
        }
        if (luma > 128) value |= (uint64_t(1) << i);
    }
    return value;
}

}  // namespace

void fillRect(const FrameView& f, int x0, int y0, int x1, int y1, Rgb color) {
    x0 = std::clamp(x0, 0, int(f.width));
    x1 = std::clamp(x1, 0, int(f.width));
    y0 = std::clamp(y0, 0, int(f.height));
    y1 = std::clamp(y1, 0, int(f.height));
    if (x0 >= x1 || y0 >= y1) return;

    switch (f.fourcc) {
        case kFourccNV12: {
            const Yuv c = toYuv(color);
            for (int y = y0; y < y1; ++y) std::memset(f.plane0 + size_t(y) * f.pitch0 + x0, c.y, size_t(x1 - x0));
            const int cx0 = x0 / 2, cx1 = (x1 + 1) / 2;
            for (int cy = y0 / 2; cy < (y1 + 1) / 2; ++cy) {
                uint8_t* row = f.plane1 + size_t(cy) * f.pitch1;
                for (int cx = cx0; cx < cx1; ++cx) {
                    row[cx * 2] = c.u;
                    row[cx * 2 + 1] = c.v;
                }
            }
            break;
        }
        case kFourccYUY2: {
            const Yuv c = toYuv(color);
            for (int y = y0; y < y1; ++y) {
                uint8_t* row = f.plane0 + size_t(y) * f.pitch0;
                for (int x = x0 & ~1; x < x1; x += 2) {
                    row[x * 2 + 0] = c.y;
                    row[x * 2 + 1] = c.u;
                    row[x * 2 + 2] = c.y;
                    row[x * 2 + 3] = c.v;
                }
            }
            break;
        }
        case kFourccRGB32: {
            const uint32_t px = 0xFF000000u | (uint32_t(color.r) << 16) | (uint32_t(color.g) << 8) | color.b;
            for (int y = y0; y < y1; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(f.plane0 + size_t(y) * f.pitch0);
                std::fill(row + x0, row + x1, px);
            }
            break;
        }
        default:
            break;
    }
}

void drawTestPattern(const FrameView& f, uint32_t frameNumber, int64_t stampQpc) {
    static constexpr Rgb kBars[] = {{191, 191, 191}, {191, 191, 0}, {0, 191, 191}, {0, 191, 0},
                                    {191, 0, 191},   {191, 0, 0},   {0, 0, 191},   {16, 16, 16}};
    const int w = int(f.width), h = int(f.height);
    const int barW = (w + 7) / 8;
    for (int i = 0; i < 8; ++i) fillRect(f, i * barW, 0, (i + 1) * barW, h, kBars[i]);

    // Moving white box: makes stalls and repeated frames obvious to the eye.
    const int box = std::max(16, h / 6) & ~1;
    const int travel = std::max(1, w - box);
    const int x = int((uint64_t(frameNumber) * uint64_t(std::max(2, w / 240))) % uint64_t(travel)) & ~1;
    fillRect(f, x, h / 2 - box / 2, x + box, h / 2 + box / 2, kWhite);

    drawBits(f, 0, uint64_t(stampQpc), 64);
    drawBits(f, 1, frameNumber, 32);
}

void drawPlaceholder(const FrameView& f, uint64_t tick) {
    const int w = int(f.width), h = int(f.height);
    fillRect(f, 0, 0, w, h, {24, 26, 32});
    const int barW = std::max(2, w / 16) & ~1;
    for (int i = 0; i < 16; i += 2) fillRect(f, i * barW, h / 3, (i + 1) * barW, 2 * h / 3, {40, 44, 54});
    const int stripe = std::max(4, w / 64) & ~1;
    const int x = int((tick * uint64_t(std::max(2, w / 320))) % uint64_t(std::max(1, w))) & ~1;
    fillRect(f, x, h / 3, x + stripe, 2 * h / 3, {90, 120, 200});
    drawBits(f, 0, 0, 64);
    drawBits(f, 1, 0, 32);
}

Stamp readStamp(const FrameView& f) {
    Stamp s;
    s.qpc = int64_t(readBits(f, 0, 64));
    s.frame = uint32_t(readBits(f, 1, 32));
    return s;
}

}  // namespace cabled::vcam
