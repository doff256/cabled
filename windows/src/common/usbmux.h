#pragma once
// Client for Apple Mobile Device Service's usbmux interface (D-01, D-02).
// See docs/transport-and-protocol.md section 1.

#include "net.h"
#include "plist.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabled::usbmux {

constexpr uint16_t kServicePort = 27015;  // AMDS on Windows listens on 127.0.0.1:27015
constexpr uint32_t kHeaderSize = 16;
constexpr uint32_t kProtocolVersion = 1;  // 1 = plist payloads
constexpr uint32_t kMessagePlist = 8;
constexpr uint32_t kMaxMessage = 4u << 20;

// usbmux "Result" Number values.
enum ResultCode : int64_t {
    kResultOk = 0,
    kResultBadCommand = 1,
    kResultBadDevice = 2,
    kResultConnectionRefused = 3,
    kResultBadVersion = 6,
};
std::string describeResult(int64_t code);

struct Device {
    uint32_t id = 0;
    std::string serial;          // UDID
    std::string connectionType;  // "USB" or "Network"
    int64_t productId = 0;
    int64_t connectionSpeed = 0;  // bit/s when AMDS reports it, else 0
    plist::Value properties;      // everything AMDS sent, for diagnostics
    bool isUsb() const { return connectionType == "USB"; }
};

struct Event {
    enum class Kind { Attached, Detached, Paired, Other };
    Kind kind = Kind::Other;
    uint32_t deviceId = 0;
    std::string messageType;
    std::optional<Device> device;  // set for Attached
};

// --- Wire helpers (exposed for tests) ---
struct Header {
    uint32_t length = 0;  // total including the 16-byte header
    uint32_t version = 0;
    uint32_t message = 0;
    uint32_t tag = 0;
};
std::vector<uint8_t> encodeMessage(const plist::Value& dict, uint32_t tag);
Header decodeHeader(const uint8_t* bytes16);
// AMDS wants the port in network byte order packed into a host integer.
uint16_t toNetworkPortNumber(uint16_t port);
std::optional<Device> deviceFromProperties(uint32_t fallbackId, const plist::Value& props);
std::optional<Event> eventFromMessage(const plist::Value& message);
plist::Value makeRequest(std::string_view messageType);

class Client {
public:
    explicit Client(std::string host = "127.0.0.1", uint16_t port = kServicePort);

    // True when something is listening on the AMDS port. This is how we detect the service:
    // the Store "Apple Devices" app installs it as a packaged service that doesn't show up
    // under its classic name in the Services list.
    bool isServiceReachable(std::string* error = nullptr);

    std::optional<std::vector<Device>> listDevices(std::string* error);

    // Blocks, calling onEvent for every attach/detach, until onEvent returns false or the
    // connection drops. Returns false on error (with *error set).
    bool listen(const std::function<bool(const Event&)>& onEvent, std::string* error);

    struct ConnectResult {
        std::optional<net::Socket> socket;  // a raw byte pipe to the device port on success
        int64_t code = -1;                  // usbmux Result number, -1 if we never got one
        std::string error;
    };
    ConnectResult connect(uint32_t deviceId, uint16_t devicePort);

private:
    std::optional<net::Socket> open(std::string* error);
    bool send(net::Socket& s, const plist::Value& dict, std::string* error);
    std::optional<plist::Value> receive(net::Socket& s, std::string* error);

    std::string host_;
    uint16_t port_;
    uint32_t nextTag_ = 1;
};

}  // namespace cabled::usbmux
