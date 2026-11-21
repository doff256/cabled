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
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--port" && hasValue) port = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (arg == "--throttle-mbps" && hasValue) options.throttleMbps = std::stod(argv[++i]);
        else if (arg == "--model" && hasValue) options.model = argv[++i];
        else if (arg == "--protocol" && hasValue) options.protocolVersion = argv[++i];
        else {
            std::printf(
                "cabled-fakephone [--port 47800] [--throttle-mbps N] [--model ID] [--protocol X.Y]\n"
                "Serves one PC at a time on 127.0.0.1, forever (Ctrl+C to stop).\n");
            return arg == "--help" || arg == "-h" ? 0 : 64;
        }
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
