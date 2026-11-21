#pragma once
// VIDEO message payload and H.264 bitstream helpers (docs/transport-and-protocol.md, VIDEO payload).
// No FFmpeg here: the fake phone and the engine both use these.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace cabled::video {

enum class Codec : uint8_t { H264 = 1, HEVC = 2 };

enum Flags : uint8_t {
    kKeyframe = 1 << 0,
    kConfigChanged = 1 << 1,
    kDiscontinuity = 1 << 2,
};

// VIDEO payload: 16-byte header | metadata (metaLength bytes) | AVCC data (4-byte big-endian NAL
// lengths, as VideoToolbox produces). Parameter sets (SPS/PPS) travel in-band on every keyframe.
constexpr size_t kHeaderSize = 16;

struct Header {
    uint64_t ptsNs = 0;  // capture time on the phone's clock
    uint32_t frameSeq = 0;
    Codec codec = Codec::H264;
    uint8_t flags = 0;
    uint16_t metaLength = 0;
};

struct PayloadView {
    Header header;
    const uint8_t* meta = nullptr;
    size_t metaSize = 0;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
};

void encodeHeader(uint8_t out[kHeaderSize], const Header& h);
std::optional<PayloadView> parsePayload(const uint8_t* payload, size_t size);
std::vector<uint8_t> buildPayload(Header h, const uint8_t* meta, size_t metaSize, const uint8_t* avcc,
                                  size_t avccSize);

// ------------------------------------------------------------------ H.264

enum NalType : uint8_t {
    kNalSlice = 1,
    kNalIdr = 5,
    kNalSei = 6,
    kNalSps = 7,
    kNalPps = 8,
    kNalAud = 9,
};

struct Nal {
    const uint8_t* data = nullptr;  // starts at the NAL header byte (no start code / length)
    size_t size = 0;
    uint8_t type() const { return size ? (data[0] & 0x1F) : 0; }
};

// Splits an Annex-B stream (00 00 01 / 00 00 00 01 start codes) into NAL units.
std::vector<Nal> splitAnnexB(const uint8_t* data, size_t size);
// Splits an AVCC buffer (4-byte big-endian lengths). Empty result on malformed input.
std::optional<std::vector<Nal>> splitAvcc(const uint8_t* data, size_t size);

// Groups an Annex-B elementary stream into access units (one coded picture each, with the SPS/PPS/
// SEI that precede it), each returned as Annex-B.
std::vector<std::vector<uint8_t>> splitAccessUnits(const uint8_t* data, size_t size);

std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t size);
bool avccToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>& out);
bool containsIdr(const std::vector<Nal>& nals);

}  // namespace cabled::video
