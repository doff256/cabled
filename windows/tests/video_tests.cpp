#include "video.h"

#include <gtest/gtest.h>

#include <vector>

namespace video = cabled::video;

namespace {

std::vector<uint8_t> concat(std::initializer_list<std::vector<uint8_t>> parts) {
    std::vector<uint8_t> out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

const std::vector<uint8_t> kStart4 = {0, 0, 0, 1};
const std::vector<uint8_t> kStart3 = {0, 0, 1};
const std::vector<uint8_t> kSps = {0x67, 0x64, 0x00, 0x28, 0xAC};
const std::vector<uint8_t> kPps = {0x68, 0xEE, 0x3C, 0x80};
const std::vector<uint8_t> kIdr = {0x65, 0x88, 0x84, 0x00, 0x33};       // first_mb_in_slice = 0
const std::vector<uint8_t> kP1 = {0x41, 0x9A, 0x02, 0x10};             // new picture
const std::vector<uint8_t> kP1Slice2 = {0x41, 0x00, 0x22, 0x04};       // same picture, 2nd slice
const std::vector<uint8_t> kP2 = {0x41, 0x9A, 0x04, 0x20};             // new picture

}  // namespace

TEST(Video, HeaderRoundTripWithMetadata) {
    video::Header h;
    h.ptsNs = 0x1122334455667788ull;
    h.frameSeq = 42;
    h.codec = video::Codec::H264;
    h.flags = video::kKeyframe | video::kDiscontinuity;
    const std::vector<uint8_t> meta = {1, 2, 3};
    const std::vector<uint8_t> data = {0, 0, 0, 1, 0x65};
    const auto payload = video::buildPayload(h, meta.data(), meta.size(), data.data(), data.size());
    ASSERT_EQ(payload.size(), video::kHeaderSize + 3 + 5);
    EXPECT_EQ(payload[0], 0x88);  // little-endian pts
    EXPECT_EQ(payload[8], 42);
    EXPECT_EQ(payload[12], 1);  // codec
    EXPECT_EQ(payload[14], 3);  // meta length

    auto v = video::parsePayload(payload.data(), payload.size());
    ASSERT_TRUE(v);
    EXPECT_EQ(v->header.ptsNs, h.ptsNs);
    EXPECT_EQ(v->header.frameSeq, 42u);
    EXPECT_EQ(v->header.flags, h.flags);
    EXPECT_EQ(v->metaSize, 3u);
    EXPECT_EQ(v->meta[2], 3);
    EXPECT_EQ(v->dataSize, 5u);
    EXPECT_EQ(v->data[4], 0x65);

    EXPECT_FALSE(video::parsePayload(payload.data(), 10));  // shorter than the header
}

TEST(Video, SplitsAnnexBWithBothStartCodeLengths) {
    const auto stream = concat({kStart4, kSps, kStart3, kPps, kStart4, kIdr});
    const auto nals = video::splitAnnexB(stream.data(), stream.size());
    ASSERT_EQ(nals.size(), 3u);
    EXPECT_EQ(nals[0].type(), video::kNalSps);
    EXPECT_EQ(nals[0].size, kSps.size());
    EXPECT_EQ(nals[1].type(), video::kNalPps);
    EXPECT_EQ(nals[1].size, kPps.size());
    EXPECT_EQ(nals[2].type(), video::kNalIdr);
    EXPECT_TRUE(video::containsIdr(nals));
}

TEST(Video, AvccRoundTrip) {
    const auto annexB = concat({kStart4, kSps, kStart4, kPps, kStart4, kIdr});
    const auto avcc = video::annexBToAvcc(annexB.data(), annexB.size());
    ASSERT_EQ(avcc.size(), 3 * 4 + kSps.size() + kPps.size() + kIdr.size());
    EXPECT_EQ(avcc[3], kSps.size());  // big-endian length
    std::vector<uint8_t> back;
    ASSERT_TRUE(video::avccToAnnexB(avcc.data(), avcc.size(), back));
    EXPECT_EQ(back, annexB);

    std::vector<uint8_t> broken = avcc;
    broken[3] = 200;  // length past the end
    EXPECT_FALSE(video::avccToAnnexB(broken.data(), broken.size(), back));
}

TEST(Video, GroupsAccessUnits) {
    const auto stream = concat({kStart4, kSps, kStart4, kPps, kStart4, kIdr,  // AU 1
                                kStart4, kP1, kStart4, kP1Slice2,             // AU 2 (two slices)
                                kStart4, kP2});                               // AU 3
    const auto units = video::splitAccessUnits(stream.data(), stream.size());
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(video::splitAnnexB(units[0].data(), units[0].size()).size(), 3u);
    EXPECT_TRUE(video::containsIdr(video::splitAnnexB(units[0].data(), units[0].size())));
    EXPECT_EQ(video::splitAnnexB(units[1].data(), units[1].size()).size(), 2u);
    EXPECT_FALSE(video::containsIdr(video::splitAnnexB(units[1].data(), units[1].size())));
    EXPECT_EQ(video::splitAnnexB(units[2].data(), units[2].size()).size(), 1u);
}
