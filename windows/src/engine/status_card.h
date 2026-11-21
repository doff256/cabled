#pragma once
// The picture apps see when there's no live video (R-73): a dark card with a short status line and
// the next step, e.g. "Open Cabled on your iPhone". Drawn with GDI into a top-down BGRA buffer.
// Note: apps that mirror their self-view show this text mirrored to you, not to the other side.

#include <cstdint>
#include <string>
#include <vector>

namespace cabled::engine {

struct StatusCard {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra;  // stride = width * 4
};

StatusCard renderStatusCard(uint32_t width, uint32_t height, const std::wstring& headline,
                            const std::wstring& detail);

}  // namespace cabled::engine
