#include "protocol.h"

#include <charconv>
#include <chrono>
#include <cstring>

namespace cabled::proto {

std::string_view typeName(Type t) {
    switch (t) {
        case Type::Hello: return "HELLO";
        case Type::Bye: return "BYE";
        case Type::Error: return "ERROR";
        case Type::Ping: return "PING";
        case Type::Pong: return "PONG";
        case Type::Log: return "LOG";
        case Type::Status: return "STATUS";
        case Type::ProbeRequest: return "PROBE_REQUEST";
        case Type::ProbeData: return "PROBE_DATA";
        case Type::ProbeEnd: return "PROBE_END";
        case Type::ProbeResult: return "PROBE_RESULT";
        case Type::Caps: return "CAPS";
        case Type::Configure: return "CONFIGURE";
        case Type::Configured: return "CONFIGURED";
        case Type::Video: return "VIDEO";
        case Type::KeyframeRequest: return "KEYFRAME_REQUEST";
    }
    return "UNKNOWN";
}

std::string typeName(uint16_t raw) {
    const std::string_view name = typeName(static_cast<Type>(raw));
    if (name != "UNKNOWN") return std::string(name);
    return "UNKNOWN(0x" + [&] {
        char buf[8];
        auto [p, ec] = std::to_chars(buf, buf + sizeof(buf), raw, 16);
        return std::string(buf, p);
    }() + ")";
}

std::string versionString() { return std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor); }

bool isCompatible(std::string_view peer) {
    const size_t dot = peer.find('.');
    if (dot == std::string_view::npos) return false;
    int major = -1;
    int minor = -1;
    auto r1 = std::from_chars(peer.data(), peer.data() + dot, major);
    auto r2 = std::from_chars(peer.data() + dot + 1, peer.data() + peer.size(), minor);
    if (r1.ec != std::errc() || r2.ec != std::errc()) return false;
    if (kVersionMajor == 0) return major == 0 && minor == kVersionMinor;
    return major == kVersionMajor;
}

void putLE64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t getLE64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

void encodeHeader(uint8_t out[kHeaderSize], Type type, uint16_t flags, size_t payloadSize) {
    const uint32_t length = static_cast<uint32_t>(4 + payloadSize);
    out[0] = static_cast<uint8_t>(length);
    out[1] = static_cast<uint8_t>(length >> 8);
    out[2] = static_cast<uint8_t>(length >> 16);
    out[3] = static_cast<uint8_t>(length >> 24);
    const auto t = static_cast<uint16_t>(type);
    out[4] = static_cast<uint8_t>(t);
    out[5] = static_cast<uint8_t>(t >> 8);
    out[6] = static_cast<uint8_t>(flags);
    out[7] = static_cast<uint8_t>(flags >> 8);
}

std::vector<uint8_t> encode(Type type, uint16_t flags, const void* payload, size_t size) {
    std::vector<uint8_t> out(kHeaderSize + size);
    encodeHeader(out.data(), type, flags, size);
    if (size) std::memcpy(out.data() + kHeaderSize, payload, size);
    return out;
}

bool writeMessage(net::Socket& s, Type type, uint16_t flags, const void* payload, size_t size) {
    if (size <= 64 * 1024) {
        const auto bytes = encode(type, flags, payload, size);  // one send for small messages
        return s.sendAll(bytes.data(), bytes.size());
    }
    uint8_t header[kHeaderSize];
    encodeHeader(header, type, flags, size);
    return s.sendAll(header, sizeof(header)) && s.sendAll(payload, size);
}

bool writeJson(net::Socket& s, Type type, const nlohmann::json& body) {
    const std::string text = body.dump();
    return writeMessage(s, type, 0, text.data(), text.size());
}

bool writeText(net::Socket& s, Type type, std::string_view text) {
    return writeMessage(s, type, 0, text.data(), text.size());
}

bool readMessage(net::Socket& s, Message& out, std::string* error) {
    uint8_t lengthBytes[4];
    if (!s.recvExact(lengthBytes, 4)) {
        if (error) *error = "connection closed or timed out";
        return false;
    }
    const uint32_t length = uint32_t(lengthBytes[0]) | (uint32_t(lengthBytes[1]) << 8) |
                            (uint32_t(lengthBytes[2]) << 16) | (uint32_t(lengthBytes[3]) << 24);
    if (length < 4 || length > kMaxBodyLength) {
        if (error) *error = "bad message length " + std::to_string(length);
        return false;
    }
    uint8_t typeFlags[4];
    if (!s.recvExact(typeFlags, 4)) {
        if (error) *error = "truncated message header";
        return false;
    }
    out.rawType = static_cast<uint16_t>(typeFlags[0] | (typeFlags[1] << 8));
    out.flags = static_cast<uint16_t>(typeFlags[2] | (typeFlags[3] << 8));
    out.payload.resize(length - 4);
    if (!out.payload.empty() && !s.recvExact(out.payload.data(), out.payload.size())) {
        if (error) *error = "truncated message payload";
        return false;
    }
    return true;
}

std::optional<nlohmann::json> parseJson(const Message& m) {
    auto j = nlohmann::json::parse(m.payload.begin(), m.payload.end(), nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    return j;
}

std::array<uint8_t, 8> encodePing(uint64_t pcTimeNs) {
    std::array<uint8_t, 8> out{};
    putLE64(out.data(), pcTimeNs);
    return out;
}

std::optional<uint64_t> decodePing(const Message& m) {
    if (m.payload.size() < 8) return std::nullopt;
    return getLE64(m.payload.data());
}

std::array<uint8_t, 16> encodePong(const Pong& p) {
    std::array<uint8_t, 16> out{};
    putLE64(out.data(), p.pcTimeNs);
    putLE64(out.data() + 8, p.phoneTimeNs);
    return out;
}

std::optional<Pong> decodePong(const Message& m) {
    if (m.payload.size() < 16) return std::nullopt;
    return Pong{getLE64(m.payload.data()), getLE64(m.payload.data() + 8)};
}

void fillProbePattern(uint8_t* payload, size_t size) {
    for (size_t i = kMinProbeChunk; i < size; ++i) payload[i] = static_cast<uint8_t>(i);
}

void writeProbeSequence(uint8_t* payload, uint64_t seq) { putLE64(payload, seq); }

bool verifyProbeChunk(const uint8_t* payload, size_t size, uint64_t* seqOut) {
    if (size < kMinProbeChunk) return false;
    if (seqOut) *seqOut = getLE64(payload);
    for (size_t i = kMinProbeChunk; i < size; ++i) {
        if (payload[i] != static_cast<uint8_t>(i)) return false;
    }
    return true;
}

uint64_t nowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace cabled::proto
