#include "formats.h"

#include "vcam_shared.h"

#include <algorithm>

namespace cabled::vcam {

const std::vector<AdvertisedFormat>& AdvertisedFormats() {
    // NV12 1080p30 first: the safe default. No SD sizes (R5): with no constraints Chromium asks for
    // 640x480 and takes the closest format, which is now 1280x720 instead of an SD one. 4K60 and
    // 1440p for OBS/browsers that ask for them (R-51); the engine negotiates the phone to match.
    static const std::vector<AdvertisedFormat> kFormats = {
        {MFVideoFormat_NV12, 1920, 1080, 30}, {MFVideoFormat_NV12, 1280, 720, 30},
        {MFVideoFormat_NV12, 1920, 1080, 60}, {MFVideoFormat_NV12, 1280, 720, 60},
        {MFVideoFormat_NV12, 3840, 2160, 30}, {MFVideoFormat_NV12, 3840, 2160, 60},
        {MFVideoFormat_NV12, 2560, 1440, 30}, {MFVideoFormat_YUY2, 1920, 1080, 30},
        {MFVideoFormat_YUY2, 1280, 720, 30},  {MFVideoFormat_RGB32, 1280, 720, 30},
    };
    return kFormats;
}

HRESULT CreateMediaType(const AdvertisedFormat& f, IMFMediaType** out) {
    ComPtr<IMFMediaType> type;
    CABLED_RETURN_IF_FAILED(MFCreateMediaType(&type));
    CABLED_RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    CABLED_RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, f.subtype));
    CABLED_RETURN_IF_FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, f.width, f.height));
    CABLED_RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, f.fps, 1));
    CABLED_RETURN_IF_FAILED(MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));

    const uint32_t fourcc = f.subtype.Data1;
    const UINT32 stride = packedStride(fourcc, f.width);
    const UINT32 bytes = UINT32(packedFrameBytes(fourcc, f.width, f.height));
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, stride));  // positive = top-down
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_SAMPLE_SIZE, bytes));
    CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_AVG_BITRATE, UINT32(std::min<uint64_t>(
                                                                  uint64_t(bytes) * 8 * f.fps, 0xFFFFFFFFull))));
    if (fourcc != kFourccRGB32) {
        // BT.709 limited range, end to end (docs/video-pipeline.md, Colour).
        CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709));
        CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235));
        CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709));
        CABLED_RETURN_IF_FAILED(type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709));
    }
    *out = type.Detach();
    return S_OK;
}

HRESULT CreateAdvertisedTypes(std::vector<ComPtr<IMFMediaType>>* types) {
    types->clear();
    for (const auto& f : AdvertisedFormats()) {
        ComPtr<IMFMediaType> t;
        CABLED_RETURN_IF_FAILED(CreateMediaType(f, &t));
        types->push_back(t);
    }
    return S_OK;
}

}  // namespace cabled::vcam
