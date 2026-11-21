// End-to-end: cabled-probe's session logic against the fake phone over loopback TCP.

#include "fake_phone.h"
#include "net.h"
#include "probe_session.h"

#include <gtest/gtest.h>

#include <thread>

using namespace cabled;

namespace {

probe::Report runAgainstFakePhone(const fakephone::Options& phone, const probe::Options& pc) {
    net::WinsockSession winsock;
    std::string err;
    auto listener = net::Socket::listenTcp("127.0.0.1", 0, &err);
    EXPECT_TRUE(listener) << err;
    if (!listener) return {};
    const uint16_t port = listener->localPort();

    std::thread phoneThread([&] {
        auto client = listener->accept(nullptr);
        if (client) fakephone::serveClient(std::move(*client), phone);
    });

    auto socket = net::Socket::connectTcp("127.0.0.1", port, &err);
    EXPECT_TRUE(socket) << err;
    probe::Report report;
    if (socket) report = probe::run(*socket, pc);
    socket.reset();  // closes, so the fake phone's read loop ends
    phoneThread.join();
    return report;
}

probe::Options quickOptions() {
    probe::Options o;
    o.pings = 5;
    o.seconds = 0.5;
    o.chunk = 64 * 1024;
    return o;
}

}  // namespace

TEST(ProbeIntegration, FullProbeSucceeds) {
    const probe::Report r = runAgainstFakePhone({}, quickOptions());
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.peerHello.value("role", ""), "phone");
    EXPECT_EQ(r.rttMs.size(), 5u);
    EXPECT_TRUE(r.down.ok);
    EXPECT_GT(r.down.chunks, 0u);
    EXPECT_EQ(r.down.corruptChunks, 0u);
    EXPECT_EQ(r.down.outOfOrderChunks, 0u);
    EXPECT_GT(r.down.mbps, 0.0);
    EXPECT_TRUE(r.up.ok) << r.up.error;
    EXPECT_GT(r.up.peerMbps, 0.0);
    EXPECT_EQ(r.peerStatus.value("thermal", ""), "nominal");
}

TEST(ProbeIntegration, ThrottledLinkIsMeasuredAsSlow) {
    fakephone::Options phone;
    phone.throttleMbps = 40;
    probe::Options pc = quickOptions();
    pc.up = false;
    pc.seconds = 1.0;
    const probe::Report r = runAgainstFakePhone(phone, pc);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_GT(r.down.mbps, 10.0);
    EXPECT_LT(r.down.mbps, 80.0);  // generous: CI machines are noisy
}

TEST(ProbeIntegration, RefusesIncompatibleProtocol) {
    fakephone::Options phone;
    phone.protocolVersion = "0.9";
    const probe::Report r = runAgainstFakePhone(phone, quickOptions());
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("incompatible protocol"), std::string::npos) << r.error;
}

TEST(ProbeIntegration, VerdictLines) {
    const auto lines = probe::bitrateVerdict(200);
    ASSERT_EQ(lines.size(), 6u);
    EXPECT_NE(lines[0].find("140"), std::string::npos);  // 70% of 200
    EXPECT_NE(lines[5].find("4K60"), std::string::npos);
}
