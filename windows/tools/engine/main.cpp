// cabled-engine: the headless Cabled engine. Finds the iPhone on USB (or a fake phone over TCP),
// keeps the connection alive, decodes video and feeds the Cabled Camera. Runs until Ctrl+C.

#include "engine.h"
#include "net.h"
#include "protocol.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace cabled;

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onConsoleSignal(DWORD) {
    g_stop = true;
    return TRUE;
}

constexpr const char* kUsage = R"(cabled-engine - headless Cabled engine

  cabled-engine [--tcp <ip:port> | --udid <UDID>] [options]

  --tcp <ip:port>    Talk to a fake phone over TCP instead of an iPhone on USB
  --udid <UDID>      Use this iPhone (default: the first on USB)
  --port <n>         Companion port on the iPhone (default 47800)
  --size <WxH>       Requested capture size (default 1920x1080)
  --fps <n>          Requested frame rate (default 30)
  --bitrate <Mbps>   Requested bitrate (default 25)
  --software         Don't try D3D11VA hardware decoding
  --seconds <s>      Stop after s seconds (default: run until Ctrl+C)
  --no-vcam          Decode only; don't feed the Cabled Camera
  --quiet            Only print state changes, no per-second stats
)";

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCtrlHandler(onConsoleSignal, TRUE);

    engine::EngineConfig config;
    config.previewMaxWidth = 0;  // no UI here
    double seconds = 0;
    bool quiet = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--tcp" && hasValue) config.tcpTarget = argv[++i];
        else if (a == "--udid" && hasValue) config.udid = argv[++i];
        else if (a == "--port" && hasValue) config.devicePort = uint16_t(std::stoi(argv[++i]));
        else if (a == "--size" && hasValue) {
            if (std::sscanf(argv[++i], "%ux%u", &config.width, &config.height) != 2) return 64;
        } else if (a == "--fps" && hasValue) config.fps = std::stod(argv[++i]);
        else if (a == "--bitrate" && hasValue) config.bitrate = uint32_t(std::stod(argv[++i]) * 1e6);
        else if (a == "--software") config.hardwareDecode = false;
        else if (a == "--seconds" && hasValue) seconds = std::stod(argv[++i]);
        else if (a == "--no-vcam") config.virtualCamera = false;
        else if (a == "--quiet") quiet = true;
        else {
            std::printf("%s", kUsage);
            return a == "--help" || a == "-h" ? 0 : 64;
        }
    }
    config.log = [](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        std::fflush(stdout);
    };

    net::WinsockSession winsock;
    engine::Engine e(config);
    std::printf("Cabled engine (%s). Ctrl+C to stop.\n",
                config.tcpTarget.empty() ? "iPhone over USB" : ("fake phone at " + config.tcpTarget).c_str());
    e.start();

    const auto start = std::chrono::steady_clock::now();
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (seconds > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= seconds)
            break;
        if (quiet) continue;
        const auto s = e.status();
        if (s.state == engine::EngineState::Streaming || s.state == engine::EngineState::Paused) {
            std::printf("[%s] %ux%u %5.1f fps %5.1f Mbit/s | %s decode %.1f ms | capture->decoded %.1f ms | vcam: %s | "
                        "kf-req %llu, reconnects %llu\n",
                        engine::stateName(s.state), s.width, s.height, s.fps, s.mbps, s.hardwareDecode ? "HW" : "SW",
                        s.decodeMs, s.latencyMs, s.vcam.c_str(), (unsigned long long)s.keyframeRequests,
                        (unsigned long long)s.reconnects);
            std::fflush(stdout);
        }
    }
    e.stop();
    std::printf("Stopped.\n");
    return 0;
}
