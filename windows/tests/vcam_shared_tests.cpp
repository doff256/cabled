#include "test_pattern.h"
#include "vcam_shared.h"

#include <gtest/gtest.h>

#include <vector>

using namespace cabled::vcam;

namespace {

// Local\ names: creating Global\ objects needs SeCreateGlobalPrivilege, which tests don't have.
std::wstring uniqueName(const wchar_t* base) {
    return std::wstring(L"Local\\") + base + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(GetTickCount64());
}

}  // namespace

TEST(VcamPattern, StampRoundTripsInEveryFormat) {
    for (uint32_t fourcc : {kFourccNV12, kFourccYUY2, kFourccRGB32}) {
        for (auto [w, h] : {std::pair{1920u, 1080u}, std::pair{640u, 360u}, std::pair{3840u, 2160u}}) {
            std::vector<uint8_t> buf(packedFrameBytes(fourcc, w, h));
            const FrameView v = packedView(fourcc, w, h, buf.data());
            drawTestPattern(v, 123456, 0x0123456789ABCDEFll);
            const Stamp s = readStamp(v);
            EXPECT_EQ(s.frame, 123456u) << fourccName(fourcc) << " " << w;
            EXPECT_EQ(s.qpc, 0x0123456789ABCDEFll) << fourccName(fourcc) << " " << w;

            drawPlaceholder(v, 7);
            EXPECT_EQ(readStamp(v).frame, 0u);
        }
    }
}

TEST(VcamPattern, BufferViewFindsPaddedUvPlane) {
    // 1920x1080 NV12 in a buffer padded to 1088 rows with a 2048-byte pitch (typical GPU layout).
    const uint32_t pitch = 2048, padded = 1088;
    std::vector<uint8_t> buf(size_t(pitch) * padded * 3 / 2);
    const FrameView v = bufferView(kFourccNV12, 1920, 1080, buf.data(), LONG(pitch), uint32_t(buf.size()));
    EXPECT_EQ(v.plane1, buf.data() + size_t(pitch) * padded);
    drawTestPattern(v, 9, 42);
    EXPECT_EQ(readStamp(v).frame, 9u);
}

TEST(VcamRing, PublishAndReadLatest) {
    const std::wstring section = uniqueName(L"CabledTestRing");
    const std::wstring event = section + L".Frame";
    DWORD error = 0;
    Mapping server = Mapping::create(section.c_str(), event.c_str(), &error);
    ASSERT_TRUE(server.valid()) << error;
    Mapping client = Mapping::open(section.c_str(), event.c_str(), &error);
    ASSERT_TRUE(client.valid()) << error;

    const uint32_t w = 1280, h = 720;
    std::vector<uint8_t> src(packedFrameBytes(kFourccNV12, w, h));
    std::vector<uint8_t> dst(src.size());
    const FrameView srcView = packedView(kFourccNV12, w, h, src.data());
    const FrameView dstView = packedView(kFourccNV12, w, h, dst.data());

    EXPECT_EQ(server.readLatest(dstView, 0, nullptr), 0);  // nothing yet

    for (uint32_t n = 1; n <= 5; ++n) {
        drawTestPattern(srcView, n, 1000 + n);
        ASSERT_TRUE(client.publish(srcView, 1000 + n));
    }
    EXPECT_EQ(WaitForSingleObject(server.frameEvent(), 0), WAIT_OBJECT_0);

    int64_t qpc = 0;
    const LONG64 seq = server.readLatest(dstView, 0, &qpc);
    EXPECT_EQ(seq, 5);
    EXPECT_EQ(qpc, 1005);
    EXPECT_EQ(readStamp(dstView).frame, 5u);
    EXPECT_EQ(server.readLatest(dstView, seq, nullptr), 0);  // nothing newer

    // A frame of a different size isn't handed out.
    std::vector<uint8_t> small(packedFrameBytes(kFourccNV12, 640, 360));
    EXPECT_EQ(server.readLatest(packedView(kFourccNV12, 640, 360, small.data()), 0, nullptr), 0);
}

TEST(VcamRing, OpenFailsWhenNoSourceExists) {
    const std::wstring section = uniqueName(L"CabledMissingRing");
    DWORD error = 0;
    Mapping m = Mapping::open(section.c_str(), (section + L".Frame").c_str(), &error);
    EXPECT_FALSE(m.valid());
    EXPECT_EQ(error, DWORD(ERROR_FILE_NOT_FOUND));
}
