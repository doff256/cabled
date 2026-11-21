#pragma once
// Fake iPhone companion (D-21): speaks the phone side of the wire protocol over plain TCP so the
// Windows side can be built and tested without a device. Grows with the protocol run by run.

#include "net.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cabled::fakephone {

// An H.264 clip pre-split into access units, already in AVCC form for VIDEO messages.
struct Clip {
    std::vector<std::vector<uint8_t>> units;  // AVCC
    std::vector<bool> isKeyframe;
    uint32_t width = 1920;
    uint32_t height = 1080;
    double fps = 30;
};

// Loads an Annex-B .h264 file. Fails if it has no IDR frame.
std::shared_ptr<const Clip> loadClip(const std::string& path, uint32_t width, uint32_t height, double fps,
                                     std::string* error);

struct Options {
    std::string model = "FakePhone1,1";
    std::string protocolVersion;          // empty = the real version; set to test mismatches
    double throttleMbps = 0;              // > 0 limits probe throughput, to imitate a slow link
    std::shared_ptr<const Clip> clip;     // video to stream after CONFIGURE (none = CONFIGURED fails)
    uint32_t dropEveryFrames = 0;         // > 0: silently lose every Nth video frame (tests recovery)
    std::function<void(const std::string&)> log;
};

// Serves one connected PC until it says BYE or disconnects.
void serveClient(net::Socket client, const Options& options);

}  // namespace cabled::fakephone
