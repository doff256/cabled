#pragma once
// H.264 decoder on FFmpeg (D-05): D3D11VA hardware decode when available, software otherwise.
// R3 copies decoded frames to system memory; the zero-copy D3D11 pipeline comes in R7.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;

namespace cabled::engine {

class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool open(bool preferHardware, std::string* error);
    void close();

    // Feeds one access unit (Annex-B). Calls onFrame for every picture that comes out; the frame is
    // in system memory (NV12 from hardware, usually YUV420P from software) and only valid during the
    // call. False on a decode error (the caller should ask the phone for a keyframe).
    bool decode(const uint8_t* annexB, size_t size, int64_t pts, const std::function<void(const AVFrame*)>& onFrame,
                std::string* error);

    // Drops buffered state, e.g. after a gap in the stream.
    void flush();

    bool hardwareActive() const { return hardwareFrames_; }
    bool hardwareRequested() const { return hwDevice_ != nullptr; }

private:
    AVCodecContext* ctx_ = nullptr;
    AVBufferRef* hwDevice_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* swFrame_ = nullptr;
    std::vector<uint8_t> padded_;
    bool hardwareFrames_ = false;
};

std::string ffmpegError(int code);

}  // namespace cabled::engine
