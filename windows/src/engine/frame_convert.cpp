#include "frame_convert.h"

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <algorithm>

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

bool FrameConverter::convert(const AVFrame* src, const vcam::FrameView& dst, const CropRect* crop) {
    if (!src || !dst.valid()) return false;

    // Crop window, on even pixels so the 4:2:0 chroma planes stay aligned.
    int cx = 0, cy = 0, cw = src->width, ch = src->height;
    if (crop) {
        cw = std::clamp(int(crop->w + 0.5) & ~1, 2, src->width & ~1);
        ch = std::clamp(int(crop->h + 0.5) & ~1, 2, src->height & ~1);
        cx = std::clamp(int(crop->x) & ~1, 0, (src->width - cw) & ~1);
        cy = std::clamp(int(crop->y) & ~1, 0, (src->height - ch) & ~1);
    }
    const uint8_t* planes[4] = {src->data[0], src->data[1], src->data[2], nullptr};
    const int strides[4] = {src->linesize[0], src->linesize[1], src->linesize[2], 0};
    switch (src->format) {
        case AV_PIX_FMT_NV12:
            planes[0] += size_t(cy) * strides[0] + cx;
            planes[1] += size_t(cy / 2) * strides[1] + cx;  // interleaved UV: one byte pair per 2 pixels
            break;
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
            planes[0] += size_t(cy) * strides[0] + cx;
            planes[1] += size_t(cy / 2) * strides[1] + cx / 2;
            planes[2] += size_t(cy / 2) * strides[2] + cx / 2;
            break;
        case AV_PIX_FMT_BGRA:
            planes[0] += size_t(cy) * strides[0] + size_t(cx) * 4;
            break;
        default:  // unknown layout: no cropping
            cx = cy = 0;
            cw = src->width;
            ch = src->height;
    }

    // Fast path: same format and size -> straight copy, no resampling.
    if (src->format == AV_PIX_FMT_NV12 && dst.fourcc == vcam::kFourccNV12 && uint32_t(cw) == dst.width &&
        uint32_t(ch) == dst.height) {
        lastWasCopy_ = true;
        vcam::FrameView v;
        v.fourcc = vcam::kFourccNV12;
        v.width = uint32_t(cw);
        v.height = uint32_t(ch);
        v.plane0 = const_cast<uint8_t*>(planes[0]);
        v.pitch0 = uint32_t(strides[0]);
        v.plane1 = const_cast<uint8_t*>(planes[1]);
        v.pitch1 = uint32_t(strides[1]);
        return vcam::copyFrame(v, dst);
    }
    lastWasCopy_ = false;
    const AVPixelFormat dstFormat = ffFormat(dst.fourcc);
    if (dstFormat == AV_PIX_FMT_NONE) return false;
    sws_ = sws_getCachedContext(sws_, cw, ch, AVPixelFormat(src->format), int(dst.width), int(dst.height), dstFormat,
                                SWS_BICUBIC, nullptr, nullptr, nullptr);
    if (!sws_) return false;

    // BT.709 limited range in; BT.709 out (limited for YUV, full for RGB). docs/video-pipeline.md, Colour.
    const int* bt709 = sws_getCoefficients(SWS_CS_ITU709);
    const int dstFullRange = dst.fourcc == vcam::kFourccRGB32 ? 1 : 0;
    sws_setColorspaceDetails(sws_, bt709, 0, bt709, dstFullRange, 0, 1 << 16, 1 << 16);

    uint8_t* dstData[4] = {dst.plane0, dst.plane1, nullptr, nullptr};
    int dstStride[4] = {int(dst.pitch0), int(dst.pitch1), 0, 0};
    const int rows = sws_scale(sws_, planes, strides, 0, ch, dstData, dstStride);
    return rows == int(dst.height);
}

bool FrameConverter::convertBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                                 const vcam::FrameView& dst) {
    AVFrame* f = av_frame_alloc();
    if (!f) return false;
    f->format = AV_PIX_FMT_BGRA;
    f->width = int(width);
    f->height = int(height);
    f->data[0] = const_cast<uint8_t*>(bgra);  // read-only use; no buffer refs, so free won't touch it
    f->linesize[0] = int(stride);
    const bool ok = convert(f, dst);
    av_frame_free(&f);
    return ok;
}

}  // namespace cabled::engine
