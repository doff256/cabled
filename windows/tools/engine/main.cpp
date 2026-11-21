// cabled-engine: the headless Cabled engine (R3). Connects to the phone (USB via Apple Mobile Device
// Service, or the fake phone over TCP), receives H.264, decodes it and feeds the Cabled Camera.

#include "net.h"
#include "product.h"
#include "protocol.h"
#include "session.h"
#include "usbmux.h"
#include "vcam_output.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cabled;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onConsoleSignal(DWORD) {
    g_stop = true;
    return TRUE;
}

constexpr const char* kUsage = R"(cabled-engine - headless Cabled engine (R3)

  cabled-engine [--tcp <ip:port> | --udid <UDID>] [options]

  --tcp <ip:port>    Connect to a fake phone over TCP instead of USB
  --udid <UDID>      Use this iPhone (default: first one on USB)
  --port <n>         Companion port on the iPhone (default 47800)
  --size <WxH>       Requested capture size (default 1920x1080)
  --fps <n>          Requested frame rate (default 30)
  --bitrate <Mbps>   Requested bitrate (default 25)
  --software         Don't try D3D11VA hardware decoding
  --seconds <s>      Stop after s seconds (default: run until Ctrl+C or disconnect)
  --no-vcam          Decode only; don't feed the Cabled Camera
)";

std::optional<net::Socket> connectUsb(const std::string& udid, uint16_t port) {
    usbmux::Client client;
    bool announced = false;
    while (!g_stop) {
        std::string err;
        auto devices = client.listDevices(&err);
        if (!devices) {
            std::fprintf(stderr, "Apple Mobile Device Service isn't running (%s)\n", err.c_str());
            return std::nullopt;
        }
        for (const auto& d : *devices) {
            if (!d.isUsb() || (!udid.empty() && d.serial != udid)) continue;
            auto r = client.connect(d.id, port);
            if (r.socket) {
                std::printf("Connected over USB to %s\n", d.serial.c_str());
                return std::move(r.socket);
            }
            if (!announced) {
                std::printf("%s\n", r.code == usbmux::kResultConnectionRefused
                                        ? "iPhone found; waiting for the Cabled app to be opened..."
                                        : r.error.c_str());
                announced = true;
            }
        }
        if (!announced) {
            std::printf("Waiting for an iPhone on USB...\n");
            announced = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return std::nullopt;
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, size_t(double(v.size()) * p))];
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onConsoleSignal, TRUE);

    std::string tcp, udid;
    uint16_t port = proto::kDefaultDevicePort;
    bool useVcam = true;
    engine::SessionOptions options;
    options.log = [](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        std::fflush(stdout);
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--tcp" && hasValue) tcp = argv[++i];
        else if (a == "--udid" && hasValue) udid = argv[++i];
        else if (a == "--port" && hasValue) port = uint16_t(std::stoi(argv[++i]));
        else if (a == "--size" && hasValue) {
            if (std::sscanf(argv[++i], "%ux%u", &options.width, &options.height) != 2) return 64;
        } else if (a == "--fps" && hasValue) options.fps = std::stod(argv[++i]);
        else if (a == "--bitrate" && hasValue) options.bitrate = uint32_t(std::stod(argv[++i]) * 1e6);
        else if (a == "--software") options.hardwareDecode = false;
        else if (a == "--seconds" && hasValue) options.seconds = std::stod(argv[++i]);
        else if (a == "--no-vcam") useVcam = false;
        else {
            std::printf("%s", kUsage);
            return a == "--help" || a == "-h" ? 0 : 64;
        }
    }

    net::WinsockSession winsock;
    std::optional<net::Socket> socket;
    if (!tcp.empty()) {
        const size_t colon = tcp.rfind(':');
        std::string err;
        socket = net::Socket::connectTcp(tcp.substr(0, colon), uint16_t(std::stoi(tcp.substr(colon + 1))), &err);
        if (!socket) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 3;
        }
        std::printf("Connected over TCP to %s\n", tcp.c_str());
    } else {
        socket = connectUsb(udid, port);
        if (!socket) return 3;
    }

    engine::VcamOutput vcam;
    uint64_t published = 0;
    uint64_t lastPublished = 0;
    const engine::FrameSink sink = [&](const AVFrame* frame, const engine::FrameInfo& info) {
        if (!useVcam) return;
        if (vcam.publish(frame, engine::nsToQpc(info.captureNs)) == engine::VcamOutput::State::Streaming) ++published;
    };
    const auto onSecond = [&](const engine::SessionStats& s) {
        const double fps = s.windowSeconds > 0 ? double(s.windowFrames) / s.windowSeconds : 0;
        const double mbps = s.windowSeconds > 0 ? double(s.windowBytes) * 8 / s.windowSeconds / 1e6 : 0;
        std::printf(
            "recv+decode %5.1f fps %5.1f Mbit/s | %s decode p50 %.1f ms | capture->decoded p50 %.1f p95 %.1f ms | "
            "vcam: %s, +%llu published%s | gaps %llu, kf-req %llu\n",
            fps, mbps, s.hardwareDecode ? "HW" : "SW", percentile(s.windowDecodeMs, 0.5),
            percentile(s.windowLatencyMs, 0.5), percentile(s.windowLatencyMs, 0.95),
            useVcam ? vcam.describe().c_str() : "off", (unsigned long long)(published - lastPublished),
            useVcam && vcam.lastWasCopy() ? " (copy)" : "", (unsigned long long)s.sequenceGaps,
            (unsigned long long)s.keyframeRequests);
        std::fflush(stdout);
        lastPublished = published;
    };

    const engine::SessionStats st = engine::runSession(*socket, options, sink, &g_stop, onSecond);
    std::printf("\nSession ended%s%s. Frames decoded %llu, decode errors %llu, gaps %llu, keyframe requests %llu.\n",
                st.error.empty() ? "" : ": ", st.error.c_str(), (unsigned long long)st.framesDecoded,
                (unsigned long long)st.decodeErrors, (unsigned long long)st.sequenceGaps,
                (unsigned long long)st.keyframeRequests);
    return st.error.empty() || g_stop ? 0 : 1;
}
