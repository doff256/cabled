#include "frame_convert.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

namespace cabled::engine {
namespace {

AVPixelFormat ffFormat(uint32_t fourcc) {
    switch (fourcc) {
        case vcam::kFourccNV12: return AV_PIX_FMT_NV12;
        case vcam::kFourccYUY2: return AV_PIX_FMT_YUYV422;
        case vcam::kFourccRGB32: return AV_PIX_FMT_BGR0;  // MFVideoFormat_RGB32 is B,G,R,X in memory
        default: return AV_PIX_FMT_NONE;
    }
}

}  // namespace

vcam::FrameView viewOf(const AVFrame* f) {
    vcam::FrameView v;
    if (!f || f->format != AV_PIX_FMT_NV12) return v;
    v.fourcc = vcam::kFourccNV12;
    v.width = uint32_t(f->width);
    v.height = uint32_t(f->height);
    v.plane0 = f->data[0];
    v.pitch0 = uint32_t(f->linesize[0]);
    v.plane1 = f->data[1];
    v.pitch1 = uint32_t(f->linesize[1]);
    return v;
}

FrameConverter::~FrameConverter() {
    if (sws_) sws_freeContext(sws_);
}

bool FrameConverter::convert(const AVFrame* src, const vcam::FrameView& dst) {
    if (!src || !dst.valid()) return false;
    // Fast path: same format and size -> straight copy, no resampling.
    if (src->format == AV_PIX_FMT_NV12 && dst.fourcc == vcam::kFourccNV12 && uint32_t(src->width) == dst.width &&
        uint32_t(src->height) == dst.height) {
        lastWasCopy_ = true;
        return vcam::copyFrame(viewOf(src), dst);
    }
    lastWasCopy_ = false;
    const AVPixelFormat dstFormat = ffFormat(dst.fourcc);
    if (dstFormat == AV_PIX_FMT_NONE) return false;
    sws_ = sws_getCachedContext(sws_, src->width, src->height, AVPixelFormat(src->format), int(dst.width),
                                int(dst.height), dstFormat, SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_) return false;

    // BT.709 limited range in; BT.709 out (limited for YUV, full for RGB). docs/video-pipeline.md, Colour.
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    const int dstFullRange = dst.fourcc == vcam::kFourccRGB32 ? 1 : 0;
    sws_setColorspaceDetails(sws_, bt709, 0, bt709, dstFullRange, 0, 1 << 16, 1 << 16);

    uint8_t* dstData[4] = {dst.plane0, dst.plane1, nullptr, nullptr};
    int dstStride[4] = {int(dst.pitch0), int(dst.pitch1), 0, 0};
    const int rows = sws_scale(sws_, src->data, src->linesize, 0, src->height, dstData, dstStride);
    return rows == int(dst.height);
}

}  // namespace cabled::engine
