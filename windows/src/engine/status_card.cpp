#include "status_card.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace cabled::engine {

StatusCard renderStatusCard(uint32_t width, uint32_t height, const std::wstring& headline,
                            const std::wstring& detail) {
    StatusCard card;
    card.width = width;
    card.height = height;
    card.bgra.assign(size_t(width) * height * 4, 0);
    if (width == 0 || height == 0) return card;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = LONG(width);
    bmi.bmiHeader.biHeight = -LONG(height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return card;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    RECT all{0, 0, LONG(width), LONG(height)};
    HBRUSH background = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(dc, &all, background);
    DeleteObject(background);

    // Accent bar, so it's recognisably Cabled even as a thumbnail.
    const LONG barHeight = std::max<LONG>(4, LONG(height) / 120);
    RECT bar{0, LONG(height) - barHeight, LONG(width), LONG(height)};
    HBRUSH accent = CreateSolidBrush(RGB(90, 120, 200));
    FillRect(dc, &bar, accent);
    DeleteObject(accent);

    SetBkMode(dc, TRANSPARENT);
    const LONG margin = LONG(width) / 10;
    auto makeFont = [](int size, int weight) {
        return CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    // The headline is one line: shrink it until it fits between the margins (long messages, small
    // resolutions like 640x360).
    int headlineSize = std::max(16, int(height) / 14);
    HFONT headlineFont = makeFont(headlineSize, FW_SEMIBOLD);
    for (int attempt = 0; attempt < 8; ++attempt) {
        RECT measure{0, 0, 0, 0};
        HGDIOBJ previous = SelectObject(dc, headlineFont);
        DrawTextW(dc, headline.c_str(), int(headline.size()), &measure, DT_CALCRECT | DT_SINGLELINE);
        SelectObject(dc, previous);
        if (measure.right <= LONG(width) - 2 * margin || headlineSize <= 12) break;
        DeleteObject(headlineFont);
        headlineSize = std::max(12, headlineSize * 85 / 100);
        headlineFont = makeFont(headlineSize, FW_SEMIBOLD);
    }
    const int detailSize = std::max(12, int(height) / 26);
    HFONT detailFont = makeFont(detailSize, FW_NORMAL);

    RECT headlineRect{margin, LONG(height) * 36 / 100, LONG(width) - margin, LONG(height) * 52 / 100};
    RECT detailRect{margin, LONG(height) * 54 / 100, LONG(width) - margin, LONG(height) * 80 / 100};
    HGDIOBJ oldFont = SelectObject(dc, headlineFont);
    SetTextColor(dc, RGB(236, 238, 244));
    DrawTextW(dc, headline.c_str(), int(headline.size()), &headlineRect, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    SelectObject(dc, detailFont);
    SetTextColor(dc, RGB(160, 166, 180));
    DrawTextW(dc, detail.c_str(), int(detail.size()), &detailRect, DT_CENTER | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, oldFont);

    GdiFlush();
    std::memcpy(card.bgra.data(), bits, card.bgra.size());
    for (size_t i = 3; i < card.bgra.size(); i += 4) card.bgra[i] = 255;  // GDI leaves alpha at 0

    SelectObject(dc, oldBitmap);
    DeleteObject(headlineFont);
    DeleteObject(detailFont);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return card;
}

}  // namespace cabled::engine
