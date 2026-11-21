#pragma once
// Cabled wire protocol framing (docs/transport-and-protocol.md section 2).
// Wire format: u32 length (of everything after this field) | u16 type | u16 flags | payload.
// All integers little-endian. Must stay byte-identical to ios/Cabled/Transport/WireProtocol.swift.

#include "net.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cabled::proto {

// Major 0 = unstable spike protocol: minor versions must match exactly.
constexpr uint16_t kVersionMajor = 0;
constexpr uint16_t kVersionMinor = 1;
constexpr uint16_t kDefaultDevicePort = 47800;
constexpr uint32_t kMaxBodyLength = 32u << 20;
constexpr size_t kHeaderSize = 8;

enum class Type : uint16_t {
    Hello = 0x0001,
    Bye = 0x0002,
    Error = 0x0003,
    Ping = 0x0004,
    Pong = 0x0005,
    Log = 0x0006,
    Status = 0x0007,
    ProbeRequest = 0x0010,
    ProbeData = 0x0011,
    ProbeEnd = 0x0012,
    ProbeResult = 0x0013,
    // Reserved for later runs: Caps 0x0020, Configure 0x0021, Configured 0x0022, Video 0x0030,
    // KeyframeRequest 0x0031, ControlSet 0x0040, ControlState 0x0041, Stats 0x0042.
};

std::string_view typeName(Type t);
std::string typeName(uint16_t raw);

struct Message {
    uint16_t rawType = 0;
    uint16_t flags = 0;
    std::vector<uint8_t> payload;

    Type type() const { return static_cast<Type>(rawType); }
    std::string_view text() const {
        return {reinterpret_cast<const char*>(payload.data()), payload.size()};
    }
};

std::string versionString();
bool isCompatible(std::string_view peerVersion);

void encodeHeader(uint8_t out[kHeaderSize], Type type, uint16_t flags, size_t payloadSize);
std::vector<uint8_t> encode(Type type, uint16_t flags, const void* payload, size_t size);

bool writeMessage(net::Socket& s, Type type, uint16_t flags, const void* payload, size_t size);
bool writeJson(net::Socket& s, Type type, const nlohmann::json& body);
bool writeText(net::Socket& s, Type type, std::string_view text);
// Reads one message into `out`, reusing its buffer. False on disconnect, timeout or garbage.
bool readMessage(net::Socket& s, Message& out, std::string* error);
std::optional<nlohmann::json> parseJson(const Message& m);

// PING: u64 pcTimeNs. PONG: u64 pcTimeNs (echoed) | u64 phoneTimeNs.
std::array<uint8_t, 8> encodePing(uint64_t pcTimeNs);
struct Pong {
    uint64_t pcTimeNs = 0;
    uint64_t phoneTimeNs = 0;
};
std::array<uint8_t, 16> encodePong(const Pong& p);
std::optional<Pong> decodePong(const Message& m);
std::optional<uint64_t> decodePing(const Message& m);

// PROBE_DATA payload: u64 sequence number, then byte[i] = i & 0xFF for i >= 8.
constexpr size_t kMinProbeChunk = 8;
void fillProbePattern(uint8_t* payload, size_t size);    // writes the i & 0xFF pattern
void writeProbeSequence(uint8_t* payload, uint64_t seq);  // writes the first 8 bytes
bool verifyProbeChunk(const uint8_t* payload, size_t size, uint64_t* seqOut);

uint64_t nowNs();  // steady clock

void putLE64(uint8_t* p, uint64_t v);
uint64_t getLE64(const uint8_t* p);

}  // namespace cabled::proto
