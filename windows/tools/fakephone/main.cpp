// cabled-fakephone: pretends to be the iPhone companion over plain TCP (D-21).
// Pair it with: cabled-probe test --tcp 127.0.0.1:47800

#include "fake_phone.h"
#include "net.h"
#include "protocol.h"

#include <windows.h>

#include <cstdio>
#include <string>

using namespace cabled;

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    uint16_t port = proto::kDefaultDevicePort;
    fakephone::Options options;
    options.log = [](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        std::fflush(stdout);
    };
    std::string clipPath;
    uint32_t clipWidth = 1920, clipHeight = 1080;
    double clipFps = 30;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--port" && hasValue) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--throttle-mbps" && hasValue) options.throttleMbps = std::stod(argv[++i]);
        else if (arg == "--model" && hasValue) options.model = argv[++i];
        else if (arg == "--protocol" && hasValue) options.protocolVersion = argv[++i];
        else if (arg == "--clip" && hasValue) clipPath = argv[++i];
        else if (arg == "--clip-size" && hasValue) {
            if (std::sscanf(argv[++i], "%ux%u", &clipWidth, &clipHeight) != 2) return 64;
        } else if (arg == "--clip-fps" && hasValue) clipFps = std::stod(argv[++i]);
        else if (arg == "--drop-every" && hasValue) options.dropEveryFrames = uint32_t(std::stoul(argv[++i]));
        else if (arg == "--disconnect-after" && hasValue) options.disconnectAfterSeconds = std::stod(argv[++i]);
        else if (arg == "--pause-after" && hasValue) options.pauseAfterSeconds = std::stod(argv[++i]);
        else if (arg == "--pause-for" && hasValue) options.pauseForSeconds = std::stod(argv[++i]);
        else if (arg == "--freeze-after" && hasValue) options.freezeAfterSeconds = std::stod(argv[++i]);
        else {
            std::printf(
                "cabled-fakephone [--port 47800] [--throttle-mbps N] [--model ID] [--protocol X.Y]\n"
                "                 [--clip file.h264 [--clip-size 1920x1080] [--clip-fps 30]]\n"
                "                 [--drop-every N] [--disconnect-after S] [--pause-after S [--pause-for S]]\n"
                "                 [--freeze-after S]   (misbehaviour, timed from video start, once per connection)\n"
                "Serves one PC at a time on 127.0.0.1, forever (Ctrl+C to stop). With --clip it streams\n"
                "that Annex-B H.264 file in a loop after CONFIGURE, like the iPhone streams its camera.\n");
            return arg == "--help" || arg == "-h" ? 0 : 64;
        }
    }
    if (!clipPath.empty()) {
        std::string clipError;
        options.clip = fakephone::loadClip(clipPath, clipWidth, clipHeight, clipFps, &clipError);
        if (!options.clip) {
            std::fprintf(stderr, "%s\n", clipError.c_str());
            return 2;
        }
        std::printf("Clip: %zu frames, %ux%u @ %.0f fps\n", options.clip->units.size(), clipWidth, clipHeight,
                    clipFps);
    }

    net::WinsockSession winsock;
    std::string err;
    auto listener = net::Socket::listenTcp("127.0.0.1", port, &err);
    if (!listener) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    std::printf("Fake phone listening on 127.0.0.1:%u (protocol %s%s)\n", listener->localPort(),
                options.protocolVersion.empty() ? proto::versionString().c_str() : options.protocolVersion.c_str(),
                options.throttleMbps > 0 ? (", throttled to " + std::to_string(options.throttleMbps) + " Mbit/s").c_str()
                                         : "");
    while (true) {
        auto client = listener->accept(&err);
        if (!client) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 2;
        }
        std::printf("PC connected\n");
        fakephone::serveClient(std::move(*client), options);
        std::printf("PC disconnected\n");
    }
}
