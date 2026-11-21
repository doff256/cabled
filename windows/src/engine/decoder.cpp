#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>

namespace cabled::engine {
namespace {

AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* formats) {
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    return avcodec_default_get_format(ctx, formats);  // software fallback
}

}  // namespace

std::string ffmpegError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

Decoder::~Decoder() { close(); }

void Decoder::close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (hwDevice_) av_buffer_unref(&hwDevice_);
    if (packet_) av_packet_free(&packet_);
    if (frame_) av_frame_free(&frame_);
    if (swFrame_) av_frame_free(&swFrame_);
    hardwareFrames_ = false;
}

bool Decoder::open(bool preferHardware, std::string* error, bool hevc) {
    close();
    hevc_ = hevc;
    const AVCodec* codec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    if (!codec) {
        if (error) *error = std::string("this FFmpeg build has no ") + (hevc ? "HEVC" : "H.264") + " decoder";
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    swFrame_ = av_frame_alloc();
    if (!ctx_ || !packet_ || !frame_ || !swFrame_) {
        if (error) *error = "out of memory";
        return false;
    }
    // Lowest latency: output every picture as soon as it's decoded, no frame threading.
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_type = FF_THREAD_SLICE;

    if (preferHardware && av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) == 0) {
        ctx_->hw_device_ctx = av_buffer_ref(hwDevice_);
        ctx_->get_format = pickFormat;
        ctx_->thread_count = 1;
    } else {
        ctx_->thread_count = 0;  // auto, slice threads only
    }

    const int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        if (error) *error = "avcodec_open2: " + ffmpegError(rc);
        return false;
    }
    return true;
}

void Decoder::flush() {
    if (ctx_) avcodec_flush_buffers(ctx_);
}

bool Decoder::decode(const uint8_t* annexB, size_t size, int64_t pts,
                     const std::function<void(const AVFrame*)>& onFrame, std::string* error) {
    // FFmpeg reads a little past the end of packets: give it the padding it asks for.
    padded_.resize(size + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(padded_.data(), annexB, size);
    std::memset(padded_.data() + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    av_packet_unref(packet_);
    packet_->data = padded_.data();
    packet_->size = static_cast<int>(size);
    packet_->pts = pts;

    int rc = avcodec_send_packet(ctx_, packet_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        if (error) *error = "avcodec_send_packet: " + ffmpegError(rc);
        return false;
    }
    while (true) {
        rc = avcodec_receive_frame(ctx_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) return true;
        if (rc < 0) {
            if (error) *error = "avcodec_receive_frame: " + ffmpegError(rc);
            return false;
        }
        if (frame_->format == AV_PIX_FMT_D3D11) {
            hardwareFrames_ = true;
            av_frame_unref(swFrame_);
            rc = av_hwframe_transfer_data(swFrame_, frame_, 0);  // GPU -> system memory (NV12)
            if (rc < 0) {
                av_frame_unref(frame_);
                if (error) *error = "av_hwframe_transfer_data: " + ffmpegError(rc);
                return false;
            }
            swFrame_->pts = frame_->pts;
            onFrame(swFrame_);
        } else {
            hardwareFrames_ = false;
            onFrame(frame_);
        }
        av_frame_unref(frame_);
    }
}

}  // namespace cabled::engine
