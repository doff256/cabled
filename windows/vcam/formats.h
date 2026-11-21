#pragma once

#include "common.h"

#include <vector>

namespace cabled::vcam {

struct AdvertisedFormat {
    GUID subtype;
    UINT32 width;
    UINT32 height;
    UINT32 fps;
};

// The list apps choose from. Order matters: many apps take the first entry (docs/virtual-camera.md).
const std::vector<AdvertisedFormat>& AdvertisedFormats();

HRESULT CreateMediaType(const AdvertisedFormat& format, IMFMediaType** type);
HRESULT CreateAdvertisedTypes(std::vector<ComPtr<IMFMediaType>>* types);

}  // namespace cabled::vcam
