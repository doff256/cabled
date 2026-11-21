#pragma once
// One engine session with a phone (real over usbmux, or the fake phone over TCP): handshake, clock
// sync, CONFIGURE, then receive -> decode -> hand frames to a sink. R3 runs it on one thread; R4
// splits receive and decode with latest-frame mailboxes (docs/architecture.md).

#include "net.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct AVFrame;

namespace cabled::engine {

struct FrameInfo {
    uint32_t frameSeq = 0;
    uint64_t captureNs = 0;  // capture time converted to this PC's steady clock
    bool keyframe = false;
};

using FrameSink = std::function<void(const AVFrame* frame, const FrameInfo& info)>;

struct SessionOptions {
    uint32_t width = 1920;
    uint32_t height = 1080;
    double fps = 30;
    uint32_t bitrate = 25'000'000;  // docs/video-pipeline.md table: 1080p30 H.264
    bool hardwareDecode = true;
    double seconds = 0;  // 0 = until the phone disconnects or `stop` is set
    std::function<void(const std::string&)> log;
};

struct SessionStats {
    // Totals
    uint64_t videoMessages = 0;
    uint64_t videoBytes = 0;
    uint64_t framesDecoded = 0;
    uint64_t decodeErrors = 0;
    uint64_t sequenceGaps = 0;
    uint64_t keyframeRequests = 0;
    uint64_t framesDroppedWaitingForKeyframe = 0;
    bool hardwareDecode = false;
    double rttMs = 0;
    int64_t clockOffsetNs = 0;  // phone minus PC
    nlohmann::json peerHello;
    nlohmann::json configured;
    std::string error;  // why the session ended, if it wasn't asked to

    // Rolling window since the last onSecond callback
    uint64_t windowFrames = 0;
    uint64_t windowBytes = 0;
    std::vector<double> windowDecodeMs;
    std::vector<double> windowLatencyMs;  // capture -> decoded
    double windowSeconds = 0;
};

// Runs until the phone leaves, `options.seconds` passes or *stop becomes true. onSecond is called
// about once a second with stats (the window fields are then cleared).
SessionStats runSession(net::Socket& socket, const SessionOptions& options, const FrameSink& sink,
                        const std::atomic<bool>* stop, const std::function<void(const SessionStats&)>& onSecond);

}  // namespace cabled::engine
