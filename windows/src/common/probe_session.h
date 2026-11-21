#pragma once
// PC side of the R1 link probe: handshake, ping round trips, throughput in both directions.

#include "net.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace cabled::probe {

struct Options {
    int pings = 50;
    double seconds = 5.0;
    size_t chunk = 64 * 1024;
    bool down = true;  // phone -> PC (the direction video uses)
    bool up = true;    // PC -> phone
    std::function<void(const std::string&)> log;
};

struct DirectionResult {
    bool ran = false;
    bool ok = false;
    uint64_t bytes = 0;  // wire bytes, including the 8-byte message header
    uint64_t chunks = 0;
    double seconds = 0;
    double mbps = 0;      // measured on the receiving side when possible
    double peerMbps = 0;  // what the other side measured
    uint64_t corruptChunks = 0;
    uint64_t outOfOrderChunks = 0;
    std::string error;
    nlohmann::json toJson() const;
};

struct Report {
    bool ok = false;
    std::string error;
    nlohmann::json peerHello;
    nlohmann::json peerStatus;
    std::vector<double> rttMs;
    double rttMinMs = 0;
    double rttMedianMs = 0;
    double rttP95Ms = 0;
    double rttMaxMs = 0;
    int64_t clockOffsetNs = 0;  // phone clock minus PC clock, from the fastest ping
    DirectionResult down;
    DirectionResult up;
    nlohmann::json toJson() const;
};

// Runs the full probe over an already-connected socket (usbmux tunnel or direct TCP).
Report run(net::Socket& socket, const Options& options);

// Which video modes from docs/video-pipeline.md fit a link of this speed (70% budget rule).
std::vector<std::string> bitrateVerdict(double linkMbps);

}  // namespace cabled::probe
