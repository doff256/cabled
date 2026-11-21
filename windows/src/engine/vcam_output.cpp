#include "vcam_output.h"

#include <cstdio>

namespace cabled::engine {

int64_t nsToQpc(uint64_t ns) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    // MSVC's steady_clock is QPC scaled to ns, so this is an exact inverse up to rounding.
    return int64_t((long double)ns * (long double)frequency / 1e9L);
}

bool VcamOutput::ensureOpen() {
    if (ring_.valid()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastAttempt_ < std::chrono::seconds(1)) return false;
    lastAttempt_ = now;
    DWORD error = 0;
    ring_ = vcam::Mapping::open(vcam::kDefaultSectionName, vcam::kDefaultEventName, &error);
    if (ring_.valid()) InterlockedExchange(&ring_.header()->feederProcessId, LONG(GetCurrentProcessId()));
    return ring_.valid();
}

VcamOutput::State VcamOutput::state() {
    if (!ensureOpen()) return State::NoCamera;
    return ring_.header()->sourceState == vcam::kSourceStreaming ? State::Streaming : State::Idle;
}

std::string VcamOutput::describe() {
    const State s = state();
    if (s == State::NoCamera) return "no app has the camera open";
    const vcam::Header* h = ring_.header();
    if (s == State::Idle) return "camera idle";
    char buf[96];
    std::snprintf(buf, sizeof(buf), "STREAMING %s %ldx%ld@%ld", vcam::fourccName(uint32_t(h->activeFourcc)).c_str(),
                  h->activeWidth, h->activeHeight, h->activeFpsDen ? h->activeFpsNum / h->activeFpsDen : 0);
    return buf;
}

bool VcamOutput::activeSize(uint32_t& width, uint32_t& height) {
    if (state() != State::Streaming) return false;
    width = uint32_t(ring_.header()->activeWidth);
    height = uint32_t(ring_.header()->activeHeight);
    return width > 0 && height > 0;
}

bool VcamOutput::activeFormat(uint32_t& width, uint32_t& height, double& fps) {
    if (!activeSize(width, height)) return false;
    const vcam::Header* h = ring_.header();
    fps = h->activeFpsDen > 0 ? double(h->activeFpsNum) / double(h->activeFpsDen) : 30.0;
    return true;
}

VcamOutput::State VcamOutput::publishBgra(const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t stride,
                                          int64_t captureQpc) {
    const State s = state();
    if (s != State::Streaming) return s;
    const vcam::Header* h = ring_.header();
    const uint32_t fourcc = uint32_t(h->activeFourcc);
    const uint32_t w = uint32_t(h->activeWidth), hh = uint32_t(h->activeHeight);
    const size_t bytes = vcam::packedFrameBytes(fourcc, w, hh);
    if (bytes == 0) return s;
    buffer_.resize(bytes);
    const vcam::FrameView view = vcam::packedView(fourcc, w, hh, buffer_.data());
    if (converter_.convertBgra(bgra, width, height, stride, view)) ring_.publish(view, captureQpc);
    return s;
}

VcamOutput::State VcamOutput::publish(const AVFrame* frame, int64_t captureQpc) {
    const State s = state();
    if (s != State::Streaming) {
        // Keep the source's "engine is alive" view fresh even when we have nothing to send.
        if (ring_.valid()) InterlockedExchange64(&ring_.header()->feederHeartbeatQpc, vcam::qpcNow());
        return s;
    }
    const vcam::Header* h = ring_.header();
    const uint32_t fourcc = uint32_t(h->activeFourcc);
    const uint32_t w = uint32_t(h->activeWidth), hh = uint32_t(h->activeHeight);
    const size_t bytes = vcam::packedFrameBytes(fourcc, w, hh);
    if (bytes == 0) return s;
    buffer_.resize(bytes);
    const vcam::FrameView view = vcam::packedView(fourcc, w, hh, buffer_.data());
    if (converter_.convert(frame, view)) ring_.publish(view, captureQpc);
    return s;
}

}  // namespace cabled::engine
