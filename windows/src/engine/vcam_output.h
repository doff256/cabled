#pragma once
// Engine side of the virtual camera frame ring (D-09): publishes decoded frames in whatever format
// the consuming app picked. The ring only exists while an app has the camera open.

#include "frame_convert.h"
#include "vcam_shared.h"

#include <chrono>
#include <string>
#include <vector>

namespace cabled::engine {

class VcamOutput {
public:
    enum class State { NoCamera, Idle, Streaming };

    // Converts and publishes the frame if an app is streaming. Opens the ring lazily (retry 1/s).
    State publish(const AVFrame* frame, int64_t captureQpc);
    State state();
    std::string describe();  // e.g. "STREAMING NV12 1920x1080@30"
    bool lastWasCopy() const { return converter_.lastWasCopy(); }

private:
    bool ensureOpen();

    vcam::Mapping ring_;
    FrameConverter converter_;
    std::vector<uint8_t> buffer_;
    std::chrono::steady_clock::time_point lastAttempt_{};
};

// steady_clock nanoseconds (proto::nowNs) -> QPC ticks, the clock the frame ring uses.
int64_t nsToQpc(uint64_t ns);

}  // namespace cabled::engine
