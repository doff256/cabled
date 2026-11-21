#include "protocol.h"

#include <gtest/gtest.h>

#include <thread>

namespace proto = cabled::proto;
namespace net = cabled::net;

TEST(Protocol, HeaderLayoutMatchesSpec) {
    // u32 length (of type+flags+payload) | u16 type | u16 flags, little-endian.
    const uint8_t payload[3] = {0xAA, 0xBB, 0xCC};
    const auto bytes = proto::encode(proto::Type::Ping, 0x0102, payload, sizeof(payload));
    ASSERT_EQ(bytes.size(), 11u);
    EXPECT_EQ(bytes[0], 7);  // 4 + 3
    EXPECT_EQ(bytes[1], 0);
    EXPECT_EQ(bytes[2], 0);
    EXPECT_EQ(bytes[3], 0);
    EXPECT_EQ(bytes[4], 0x04);  // PING
    EXPECT_EQ(bytes[5], 0x00);
    EXPECT_EQ(bytes[6], 0x02);  // flags low byte
    EXPECT_EQ(bytes[7], 0x01);
    EXPECT_EQ(bytes[8], 0xAA);
}

TEST(Protocol, VersionCompatibility) {
    EXPECT_EQ(proto::versionString(), "0.1");
    EXPECT_TRUE(proto::isCompatible("0.1"));
    EXPECT_FALSE(proto::isCompatible("0.2"));  // major 0: minors must match
    EXPECT_FALSE(proto::isCompatible("1.1"));
    EXPECT_FALSE(proto::isCompatible("garbage"));
    EXPECT_FALSE(proto::isCompatible(""));
}

TEST(Protocol, ProbePattern) {
    std::vector<uint8_t> chunk(300);
    proto::fillProbePattern(chunk.data(), chunk.size());
    proto::writeProbeSequence(chunk.data(), 0x0102030405060708ull);
    uint64_t seq = 0;
    EXPECT_TRUE(proto::verifyProbeChunk(chunk.data(), chunk.size(), &seq));
    EXPECT_EQ(seq, 0x0102030405060708ull);
    EXPECT_EQ(chunk[0], 0x08);  // little-endian sequence
    EXPECT_EQ(chunk[8], 8);
    EXPECT_EQ(chunk[256], 0);  // pattern wraps at 256
    chunk[200] ^= 0xFF;
    EXPECT_FALSE(proto::verifyProbeChunk(chunk.data(), chunk.size(), &seq));
}

TEST(Protocol, PingPongCodec) {
    proto::Message m;
    const auto pong = proto::encodePong({123, 456});
    m.payload.assign(pong.begin(), pong.end());
    auto decoded = proto::decodePong(m);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->pcTimeNs, 123u);
    EXPECT_EQ(decoded->phoneTimeNs, 456u);
    m.payload.resize(4);
    EXPECT_FALSE(proto::decodePong(m));
}

TEST(Protocol, MessagesSurviveASocket) {
    net::WinsockSession winsock;
    std::string err;
    auto listener = net::Socket::listenTcp("127.0.0.1", 0, &err);
    ASSERT_TRUE(listener) << err;
    const uint16_t port = listener->localPort();

    std::thread sender([&] {
        auto c = net::Socket::connectTcp("127.0.0.1", port, nullptr);
        ASSERT_TRUE(c);
        proto::writeJson(*c, proto::Type::Hello, {{"protocol", "0.1"}});
        std::vector<uint8_t> big(200 * 1024, 0x5A);  // takes the two-send path
        proto::writeMessage(*c, proto::Type::ProbeData, 0, big.data(), big.size());
        proto::writeText(*c, proto::Type::Log, "");
    });

    auto server = listener->accept(&err);
    ASSERT_TRUE(server) << err;
    proto::Message m;
    ASSERT_TRUE(proto::readMessage(*server, m, &err)) << err;
    EXPECT_EQ(m.type(), proto::Type::Hello);
    EXPECT_EQ(proto::parseJson(m)->value("protocol", ""), "0.1");
    ASSERT_TRUE(proto::readMessage(*server, m, &err)) << err;
    EXPECT_EQ(m.type(), proto::Type::ProbeData);
    EXPECT_EQ(m.payload.size(), 200u * 1024);
    ASSERT_TRUE(proto::readMessage(*server, m, &err)) << err;
    EXPECT_EQ(m.type(), proto::Type::Log);
    EXPECT_TRUE(m.payload.empty());
    sender.join();
}
