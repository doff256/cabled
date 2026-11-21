#include "usbmux.h"

#include "product.h"

#include <cstring>

namespace cabled::usbmux {
namespace {

void putLE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

uint32_t getLE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int64_t resultNumber(const plist::Value& msg) {
    const plist::Value* type = msg.find("MessageType");
    if (!type || type->asString() != "Result") return -1;
    const plist::Value* number = msg.find("Number");
    return number ? number->asInt(-1) : -1;
}

}  // namespace

std::string describeResult(int64_t code) {
    switch (code) {
        case kResultOk: return "OK";
        case kResultBadCommand: return "bad command";
        case kResultBadDevice: return "bad device (unplugged, or not available)";
        case kResultConnectionRefused: return "connection refused (nothing listening on that port on the iPhone)";
        case kResultBadVersion: return "bad protocol version";
        default: return "result " + std::to_string(code);
    }
}

std::vector<uint8_t> encodeMessage(const plist::Value& dict, uint32_t tag) {
    const std::string xml = plist::toXml(dict);
    std::vector<uint8_t> out(kHeaderSize + xml.size());
    putLE32(out.data() + 0, static_cast<uint32_t>(out.size()));
    putLE32(out.data() + 4, kProtocolVersion);
    putLE32(out.data() + 8, kMessagePlist);
    putLE32(out.data() + 12, tag);
    std::memcpy(out.data() + kHeaderSize, xml.data(), xml.size());
    return out;
}

Header decodeHeader(const uint8_t* bytes16) {
    return Header{getLE32(bytes16), getLE32(bytes16 + 4), getLE32(bytes16 + 8), getLE32(bytes16 + 12)};
}

uint16_t toNetworkPortNumber(uint16_t port) {
    return static_cast<uint16_t>(((port & 0xFF) << 8) | (port >> 8));
}

plist::Value makeRequest(std::string_view messageType) {
    plist::Value req = plist::Value::dict();
    req.set("MessageType", plist::Value::string(std::string(messageType)));
    req.set("ClientVersionString", plist::Value::string(std::string(CABLED_PRODUCT_NAME) + " " + CABLED_VERSION));
    req.set("ProgName", plist::Value::string(CABLED_PRODUCT_NAME));
    req.set("kLibUSBMuxVersion", plist::Value::integer(3));
    return req;
}

std::optional<Device> deviceFromProperties(uint32_t fallbackId, const plist::Value& props) {
    if (!props.is(plist::Value::Type::Dict)) return std::nullopt;
    Device d;
    d.properties = props;
    const plist::Value* id = props.find("DeviceID");
    d.id = static_cast<uint32_t>(id ? id->asInt(fallbackId) : fallbackId);
    if (const auto* v = props.find("SerialNumber")) d.serial = v->asString();
    if (const auto* v = props.find("ConnectionType")) d.connectionType = v->asString();
    if (const auto* v = props.find("ProductID")) d.productId = v->asInt();
    if (const auto* v = props.find("ConnectionSpeed")) d.connectionSpeed = v->asInt();
    return d;
}

std::optional<Event> eventFromMessage(const plist::Value& message) {
    const plist::Value* type = message.find("MessageType");
    if (!type) return std::nullopt;
    Event e;
    e.messageType = type->asString();
    const plist::Value* id = message.find("DeviceID");
    e.deviceId = static_cast<uint32_t>(id ? id->asInt() : 0);
    if (e.messageType == "Attached") {
        e.kind = Event::Kind::Attached;
        if (const plist::Value* props = message.find("Properties")) e.device = deviceFromProperties(e.deviceId, *props);
    } else if (e.messageType == "Detached") {
        e.kind = Event::Kind::Detached;
    } else if (e.messageType == "Paired") {
        e.kind = Event::Kind::Paired;
    }
    return e;
}

Client::Client(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}

std::optional<net::Socket> Client::open(std::string* error) {
    std::string err;
    auto s = net::Socket::connectTcp(host_, port_, &err);
    if (!s) {
        if (error) {
            *error = "Apple Mobile Device Service isn't reachable on " + host_ + ":" + std::to_string(port_) +
                     " (" + err + ")";
        }
        return std::nullopt;
    }
    return s;
}

bool Client::isServiceReachable(std::string* error) { return open(error).has_value(); }

bool Client::send(net::Socket& s, const plist::Value& dict, std::string* error) {
    const auto bytes = encodeMessage(dict, nextTag_++);
    if (!s.sendAll(bytes.data(), bytes.size())) {
        if (error) *error = "usbmux send failed: " + net::lastErrorString();
        return false;
    }
    return true;
}

std::optional<plist::Value> Client::receive(net::Socket& s, std::string* error) {
    uint8_t headerBytes[kHeaderSize];
    if (!s.recvExact(headerBytes, sizeof(headerBytes))) {
        if (error) *error = "usbmux connection closed or timed out";
        return std::nullopt;
    }
    const Header h = decodeHeader(headerBytes);
    if (h.length < kHeaderSize || h.length > kMaxMessage) {
        if (error) *error = "usbmux: bad message length " + std::to_string(h.length);
        return std::nullopt;
    }
    if (h.message != kMessagePlist) {
        if (error) *error = "usbmux: unexpected message type " + std::to_string(h.message);
        return std::nullopt;
    }
    std::string payload(h.length - kHeaderSize, '\0');
    if (!payload.empty() && !s.recvExact(payload.data(), payload.size())) {
        if (error) *error = "usbmux: truncated message";
        return std::nullopt;
    }
    return plist::fromXml(payload, error);
}

std::optional<std::vector<Device>> Client::listDevices(std::string* error) {
    auto s = open(error);
    if (!s) return std::nullopt;
    s->setRecvTimeout(5000);
    if (!send(*s, makeRequest("ListDevices"), error)) return std::nullopt;
    auto reply = receive(*s, error);
    if (!reply) return std::nullopt;

    const plist::Value* list = reply->find("DeviceList");
    if (!list) {
        if (error) *error = "ListDevices failed: " + describeResult(resultNumber(*reply));
        return std::nullopt;
    }
    std::vector<Device> devices;
    for (const plist::Value& entry : list->items()) {
        const plist::Value* id = entry.find("DeviceID");
        const plist::Value* props = entry.find("Properties");
        if (!props) continue;
        if (auto d = deviceFromProperties(static_cast<uint32_t>(id ? id->asInt() : 0), *props)) {
            devices.push_back(std::move(*d));
        }
    }
    return devices;
}

bool Client::listen(const std::function<bool(const Event&)>& onEvent, std::string* error) {
    auto s = open(error);
    if (!s) return false;
    s->setRecvTimeout(5000);
    if (!send(*s, makeRequest("Listen"), error)) return false;
    auto reply = receive(*s, error);
    if (!reply) return false;
    const int64_t code = resultNumber(*reply);
    if (code != kResultOk) {
        if (error) *error = "Listen failed: " + describeResult(code);
        return false;
    }
    s->setRecvTimeout(0);  // events can be minutes apart
    while (true) {
        auto msg = receive(*s, error);
        if (!msg) return false;
        if (auto e = eventFromMessage(*msg)) {
            if (!onEvent(*e)) return true;
        }
    }
}

Client::ConnectResult Client::connect(uint32_t deviceId, uint16_t devicePort) {
    ConnectResult result;
    auto s = open(&result.error);
    if (!s) return result;
    s->setRecvTimeout(5000);

    plist::Value req = makeRequest("Connect");
    req.set("DeviceID", plist::Value::integer(deviceId));
    req.set("PortNumber", plist::Value::integer(toNetworkPortNumber(devicePort)));
    if (!send(*s, req, &result.error)) return result;
    auto reply = receive(*s, &result.error);
    if (!reply) return result;

    result.code = resultNumber(*reply);
    if (result.code != kResultOk) {
        result.error = "Connect to port " + std::to_string(devicePort) + " failed: " + describeResult(result.code);
        return result;
    }
    s->setRecvTimeout(0);
    result.socket = std::move(*s);  // from here on, the socket is a raw tunnel to the iPhone
    return result;
}

}  // namespace cabled::usbmux
