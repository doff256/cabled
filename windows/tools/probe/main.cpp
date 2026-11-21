// cabled-probe: R1 spike tool. Finds the iPhone through Apple Mobile Device Service, connects to the
// companion app over USB, and measures round-trip time and throughput in both directions.

#include "net.h"
#include "probe_session.h"
#include "product.h"
#include "protocol.h"
#include "usbmux.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace cabled;

namespace {

constexpr const char* kUsage = R"(cabled-probe - R1 link probe for the Cabled iPhone companion

Usage:
  cabled-probe list              List devices known to Apple Mobile Device Service
  cabled-probe watch             Print attach/detach events until Ctrl+C
  cabled-probe test [options]    Connect to the companion and measure the link

Options for test:
  --udid <UDID>      Use this iPhone (default: the first one connected over USB)
  --port <n>         Companion port on the iPhone (default 47800)
  --seconds <s>      Length of each throughput test (default 5)
  --chunk <bytes>    Probe chunk size (default 65536)
  --pings <n>        Number of round-trip pings (default 50)
  --wait <s>         Keep retrying for up to s seconds while the companion isn't open (default 30)
  --tcp <ip:port>    Skip USB and connect over plain TCP (e.g. to cabled-fakephone)
  --json <file>      Also write the full report as JSON
)";

constexpr const char* kNoServiceHelp =
    "Apple Mobile Device Service isn't running.\n"
    "Install \"Apple Devices\" from the Microsoft Store (or iTunes from apple.com), then plug in the iPhone.";

void printDevice(const usbmux::Device& d) {
    std::printf("  [%s] DeviceID %u  UDID %s  ProductID 0x%llX", d.connectionType.c_str(), d.id, d.serial.c_str(),
                static_cast<unsigned long long>(d.productId));
    if (d.connectionSpeed > 0) std::printf("  link %lld Mbit/s", static_cast<long long>(d.connectionSpeed / 1000000));
    if (!d.isUsb()) std::printf("  (ignored: Cabled is USB only)");
    std::printf("\n    properties: %s\n", plist::describe(d.properties, 2).c_str());
}

int cmdList() {
    usbmux::Client client;
    std::string err;
    auto devices = client.listDevices(&err);
    if (!devices) {
        std::fprintf(stderr, "%s\n(%s)\n", kNoServiceHelp, err.c_str());
        return 2;
    }
    std::printf("Apple Mobile Device Service: reachable on 127.0.0.1:%u\n", usbmux::kServicePort);
    if (devices->empty()) {
        std::printf("No devices. Plug in the iPhone, unlock it, and tap Trust if asked.\n");
        return 1;
    }
    std::printf("%zu device(s):\n", devices->size());
    for (const auto& d : *devices) printDevice(d);
    return 0;
}

int cmdWatch() {
    usbmux::Client client;
    std::string err;
    std::printf("Watching for iPhones (Ctrl+C to stop)...\n");
    const bool ok = client.listen(
        [](const usbmux::Event& e) {
            switch (e.kind) {
                case usbmux::Event::Kind::Attached:
                    std::printf("ATTACHED  DeviceID %u\n", e.deviceId);
                    if (e.device) printDevice(*e.device);
                    break;
                case usbmux::Event::Kind::Detached:
                    std::printf("DETACHED  DeviceID %u\n", e.deviceId);
                    break;
                case usbmux::Event::Kind::Paired:
                    std::printf("PAIRED    DeviceID %u (trusted)\n", e.deviceId);
                    break;
                default:
                    std::printf("EVENT     %s DeviceID %u\n", e.messageType.c_str(), e.deviceId);
            }
            std::fflush(stdout);
            return true;
        },
        &err);
    if (!ok) {
        std::fprintf(stderr, "%s\n(%s)\n", kNoServiceHelp, err.c_str());
        return 2;
    }
    return 0;
}

struct TestArgs {
    std::string udid;
    uint16_t port = proto::kDefaultDevicePort;
    double waitSeconds = 30;
    std::string tcp;
    std::string jsonPath;
    probe::Options options;
};

std::optional<net::Socket> connectUsb(const TestArgs& a) {
    usbmux::Client client;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(a.waitSeconds * 1000));
    bool printedWaiting = false;
    while (true) {
        std::string err;
        auto devices = client.listDevices(&err);
        if (!devices) {
            std::fprintf(stderr, "%s\n(%s)\n", kNoServiceHelp, err.c_str());
            return std::nullopt;
        }
        const usbmux::Device* chosen = nullptr;
        for (const auto& d : *devices) {
            if (!d.isUsb()) continue;
            if (a.udid.empty() || d.serial == a.udid) {
                chosen = &d;
                break;
            }
        }
        std::string problem;
        if (!chosen) {
            problem = a.udid.empty() ? "No iPhone connected over USB. Plug it in and unlock it."
                                     : "iPhone " + a.udid + " isn't connected over USB.";
        } else {
            auto r = client.connect(chosen->id, a.port);
            if (r.socket) {
                std::printf("Connected over USB to %s (DeviceID %u), port %u\n", chosen->serial.c_str(), chosen->id,
                            a.port);
                return std::move(r.socket);
            }
            if (r.code == usbmux::kResultConnectionRefused) {
                problem = "iPhone found, but the companion isn't listening on port " + std::to_string(a.port) +
                          ". Open the Cabled app on the iPhone and keep it in the foreground.";
            } else {
                problem = r.error + "\nIf the iPhone shows \"Trust This Computer?\", tap Trust and enter the passcode.";
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            std::fprintf(stderr, "%s\n", problem.c_str());
            return std::nullopt;
        }
        if (!printedWaiting) {
            std::printf("%s\nWaiting up to %.0f s...\n", problem.c_str(), a.waitSeconds);
            printedWaiting = true;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

std::optional<net::Socket> connectTcp(const std::string& spec) {
    const size_t colon = spec.rfind(':');
    if (colon == std::string::npos) {
        std::fprintf(stderr, "--tcp expects ip:port\n");
        return std::nullopt;
    }
    const std::string host = spec.substr(0, colon);
    const auto port = static_cast<uint16_t>(std::stoi(spec.substr(colon + 1)));
    std::string err;
    auto s = net::Socket::connectTcp(host, port, &err);
    if (!s) std::fprintf(stderr, "%s\n", err.c_str());
    else std::printf("Connected over TCP to %s\n", spec.c_str());
    return s;
}

void printReport(const probe::Report& r) {
    std::printf("\n=== Link report ===\n");
    if (!r.peerHello.is_null()) {
        std::printf("Phone:        %s, %s, companion %s (%s), bind %s\n",
                    r.peerHello.value("model", "?").c_str(), r.peerHello.value("system", "?").c_str(),
                    r.peerHello.value("appVersion", "?").c_str(), r.peerHello.value("build", "?").c_str(),
                    r.peerHello.value("bind", "?").c_str());
        const auto expiry = r.peerHello.find("signingExpiry");
        std::printf("Signing:      %s\n", expiry != r.peerHello.end() && expiry->is_string()
                                              ? ("expires " + expiry->get<std::string>()).c_str()
                                              : "no expiry found (Xcode debug build or unsigned)");
    }
    if (!r.rttMs.empty()) {
        std::printf("Round trip:   min %.2f ms, median %.2f ms, p95 %.2f ms, max %.2f ms (%zu pings)\n", r.rttMinMs,
                    r.rttMedianMs, r.rttP95Ms, r.rttMaxMs, r.rttMs.size());
    }
    if (r.down.ran) {
        std::printf("Phone -> PC:  %.1f Mbit/s over %.2f s (%llu chunks, %llu corrupt, %llu out of order)%s\n",
                    r.down.mbps, r.down.seconds, static_cast<unsigned long long>(r.down.chunks),
                    static_cast<unsigned long long>(r.down.corruptChunks),
                    static_cast<unsigned long long>(r.down.outOfOrderChunks), r.down.ok ? "" : "  FAILED");
    }
    if (r.up.ran) {
        std::printf("PC -> phone:  %.1f Mbit/s (phone measured)%s\n", r.up.mbps, r.up.ok ? "" : "  FAILED");
    }
    if (r.down.ok) {
        std::printf("\n");
        for (const auto& line : probe::bitrateVerdict(r.down.mbps)) std::printf("%s\n", line.c_str());
    }
    std::printf("\nResult: %s%s%s\n", r.ok ? "OK" : "FAILED", r.ok ? "" : " - ", r.error.c_str());
}

int cmdTest(int argc, char** argv) {
    TestArgs a;
    a.options.log = [](const std::string& line) {
        std::printf("  %s\n", line.c_str());
        std::fflush(stdout);
    };
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(64);
            }
            return argv[++i];
        };
        if (arg == "--udid") a.udid = next();
        else if (arg == "--port") a.port = static_cast<uint16_t>(std::stoi(next()));
        else if (arg == "--seconds") a.options.seconds = std::stod(next());
        else if (arg == "--chunk") a.options.chunk = std::max<size_t>(proto::kMinProbeChunk, std::stoul(next()));
        else if (arg == "--pings") a.options.pings = std::stoi(next());
        else if (arg == "--wait") a.waitSeconds = std::stod(next());
        else if (arg == "--tcp") a.tcp = next();
        else if (arg == "--json") a.jsonPath = next();
        else {
            std::fprintf(stderr, "Unknown option %s\n\n%s", arg.c_str(), kUsage);
            return 64;
        }
    }

    auto socket = a.tcp.empty() ? connectUsb(a) : connectTcp(a.tcp);
    if (!socket) return 3;

    std::printf("Running probe: %d pings, %.1f s each direction, %zu-byte chunks\n", a.options.pings,
                a.options.seconds, a.options.chunk);
    const probe::Report report = probe::run(*socket, a.options);
    printReport(report);

    if (!a.jsonPath.empty()) {
        nlohmann::json j = report.toJson();
        j["transport"] = a.tcp.empty() ? "usbmux" : "tcp";
        j["tool"] = std::string("cabled-probe ") + CABLED_VERSION;
        std::ofstream(a.jsonPath) << j.dump(2) << "\n";
        std::printf("Report written to %s\n", a.jsonPath.c_str());
    }
    return report.ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    net::WinsockSession winsock;
    if (!winsock.ok()) {
        std::fprintf(stderr, "Winsock failed to start\n");
        return 70;
    }
    const std::string command = argc > 1 ? argv[1] : "";
    if (command == "list") return cmdList();
    if (command == "watch") return cmdWatch();
    if (command == "test") return cmdTest(argc, argv);
    std::printf("%s", kUsage);
    return command.empty() || command == "--help" || command == "-h" ? 0 : 64;
}
