#include "net.h"
#include "usbmux.h"

#include <gtest/gtest.h>

#include <thread>

namespace usbmux = cabled::usbmux;
namespace plist = cabled::plist;
namespace net = cabled::net;

TEST(Usbmux, PortNumberIsByteSwapped) {
    EXPECT_EQ(usbmux::toNetworkPortNumber(47800), 0xB8BA);  // 47800 = 0xBAB8
    EXPECT_EQ(usbmux::toNetworkPortNumber(62078), 0x7EF2);  // lockdownd's port, the classic example
}

TEST(Usbmux, MessageHeaderLayout) {
    plist::Value req = usbmux::makeRequest("ListDevices");
    const auto bytes = usbmux::encodeMessage(req, 7);
    ASSERT_GE(bytes.size(), usbmux::kHeaderSize);
    const usbmux::Header h = usbmux::decodeHeader(bytes.data());
    EXPECT_EQ(h.length, bytes.size());
    EXPECT_EQ(h.version, 1u);
    EXPECT_EQ(h.message, 8u);
    EXPECT_EQ(h.tag, 7u);

    const std::string xml(bytes.begin() + usbmux::kHeaderSize, bytes.end());
    auto parsed = plist::fromXml(xml);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->find("MessageType")->asString(), "ListDevices");
    EXPECT_EQ(parsed->find("kLibUSBMuxVersion")->asInt(), 3);
}

TEST(Usbmux, ParsesAttachedEvent) {
    // Shape of what usbmuxd / AMDS sends when a phone is plugged in.
    const char* xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>DeviceID</key><integer>5</integer>
  <key>MessageType</key><string>Attached</string>
  <key>Properties</key><dict>
    <key>ConnectionSpeed</key><integer>480000000</integer>
    <key>ConnectionType</key><string>USB</string>
    <key>DeviceID</key><integer>5</integer>
    <key>LocationID</key><integer>0</integer>
    <key>ProductID</key><integer>4776</integer>
    <key>SerialNumber</key><string>00008140-001A2B3C4D5E6F70</string>
  </dict>
</dict></plist>)";
    auto msg = plist::fromXml(xml);
    ASSERT_TRUE(msg);
    auto e = usbmux::eventFromMessage(*msg);
    ASSERT_TRUE(e);
    EXPECT_EQ(e->kind, usbmux::Event::Kind::Attached);
    EXPECT_EQ(e->deviceId, 5u);
    ASSERT_TRUE(e->device);
    EXPECT_TRUE(e->device->isUsb());
    EXPECT_EQ(e->device->serial, "00008140-001A2B3C4D5E6F70");
    EXPECT_EQ(e->device->productId, 4776);
    EXPECT_EQ(e->device->connectionSpeed, 480000000);
}

TEST(Usbmux, NetworkDevicesAreNotUsb) {
    plist::Value props = plist::Value::dict();
    props.set("DeviceID", plist::Value::integer(9));
    props.set("ConnectionType", plist::Value::string("Network"));
    auto d = usbmux::deviceFromProperties(0, props);
    ASSERT_TRUE(d);
    EXPECT_FALSE(d->isUsb());
    EXPECT_EQ(d->id, 9u);
}

// A fake AMDS on a random local port: answers ListDevices and refuses Connect.
TEST(Usbmux, ClientTalksToFakeService) {
    net::WinsockSession winsock;
    std::string err;
    auto listener = net::Socket::listenTcp("127.0.0.1", 0, &err);
    ASSERT_TRUE(listener) << err;
    const uint16_t port = listener->localPort();

    std::thread server([&] {
        for (int i = 0; i < 2; ++i) {
            auto c = listener->accept(nullptr);
            if (!c) return;
            uint8_t header[16];
            if (!c->recvExact(header, 16)) return;
            const auto h = usbmux::decodeHeader(header);
            std::string body(h.length - 16, '\0');
            if (!c->recvExact(body.data(), body.size())) return;
            auto req = plist::fromXml(body);
            plist::Value reply = plist::Value::dict();
            if (req && req->find("MessageType")->asString() == "ListDevices") {
                plist::Value props = plist::Value::dict();
                props.set("DeviceID", plist::Value::integer(3));
                props.set("ConnectionType", plist::Value::string("USB"));
                props.set("SerialNumber", plist::Value::string("UDID-1"));
                plist::Value entry = plist::Value::dict();
                entry.set("DeviceID", plist::Value::integer(3));
                entry.set("MessageType", plist::Value::string("Attached"));
                entry.set("Properties", props);
                plist::Value list = plist::Value::array();
                list.push(entry);
                reply.set("DeviceList", list);
            } else {
                EXPECT_EQ(req->find("PortNumber")->asInt(), usbmux::toNetworkPortNumber(47800));
                reply.set("MessageType", plist::Value::string("Result"));
                reply.set("Number", plist::Value::integer(usbmux::kResultConnectionRefused));
            }
            const auto bytes = usbmux::encodeMessage(reply, h.tag);
            c->sendAll(bytes.data(), bytes.size());
        }
    });

    usbmux::Client client("127.0.0.1", port);
    auto devices = client.listDevices(&err);
    ASSERT_TRUE(devices) << err;
    ASSERT_EQ(devices->size(), 1u);
    EXPECT_EQ((*devices)[0].serial, "UDID-1");

    auto result = client.connect(3, 47800);
    EXPECT_FALSE(result.socket);
    EXPECT_EQ(result.code, usbmux::kResultConnectionRefused);

    server.join();
}

TEST(Usbmux, ReportsMissingService) {
    net::WinsockSession winsock;
    // Grab a free port, then close it so nothing listens there.
    uint16_t port = 0;
    {
        auto l = net::Socket::listenTcp("127.0.0.1", 0, nullptr);
        ASSERT_TRUE(l);
        port = l->localPort();
    }
    usbmux::Client client("127.0.0.1", port);
    std::string err;
    EXPECT_FALSE(client.isServiceReachable(&err));
    EXPECT_NE(err.find("Apple Mobile Device Service"), std::string::npos);
}
