#include "video.h"

#include <cstring>

namespace cabled::video {
namespace {

void putLE(uint8_t* p, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint64_t getLE(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

bool isVcl(uint8_t type) { return type == kNalSlice || type == kNalIdr; }

// first_mb_in_slice is the first ue(v) of the slice header; its encoding starts with a 1 bit exactly
// when the value is 0, i.e. when this slice begins a new picture.
bool startsPicture(const Nal& n) { return n.size > 1 && (n.data[1] & 0x80) != 0; }

}  // namespace

void encodeHeader(uint8_t out[kHeaderSize], const Header& h) {
    putLE(out, h.ptsNs, 8);
    putLE(out + 8, h.frameSeq, 4);
    out[12] = static_cast<uint8_t>(h.codec);
    out[13] = h.flags;
    putLE(out + 14, h.metaLength, 2);
}

std::optional<PayloadView> parsePayload(const uint8_t* payload, size_t size) {
    if (size < kHeaderSize) return std::nullopt;
    PayloadView v;
    v.header.ptsNs = getLE(payload, 8);
    v.header.frameSeq = static_cast<uint32_t>(getLE(payload + 8, 4));
    v.header.codec = static_cast<Codec>(payload[12]);
    v.header.flags = payload[13];
    v.header.metaLength = static_cast<uint16_t>(getLE(payload + 14, 2));
    if (kHeaderSize + v.header.metaLength > size) return std::nullopt;
    v.meta = payload + kHeaderSize;
    v.metaSize = v.header.metaLength;
    v.data = v.meta + v.metaSize;
    v.dataSize = size - kHeaderSize - v.metaSize;
    return v;
}

std::vector<uint8_t> buildPayload(Header h, const uint8_t* meta, size_t metaSize, const uint8_t* avcc,
                                  size_t avccSize) {
    h.metaLength = static_cast<uint16_t>(metaSize);
    std::vector<uint8_t> out(kHeaderSize + metaSize + avccSize);
    encodeHeader(out.data(), h);
    if (metaSize) std::memcpy(out.data() + kHeaderSize, meta, metaSize);
    if (avccSize) std::memcpy(out.data() + kHeaderSize + metaSize, avcc, avccSize);
    return out;
}

std::vector<Nal> splitAnnexB(const uint8_t* data, size_t size) {
    std::vector<Nal> nals;
    size_t i = 0;
    size_t start = SIZE_MAX;
    while (i + 3 <= size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                while (end > start && data[end - 1] == 0) --end;  // trailing zero / 4-byte start code
                nals.push_back({data + start, end - start});
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < size) nals.push_back({data + start, size - start});
    return nals;
}

std::optional<std::vector<Nal>> splitAvcc(const uint8_t* data, size_t size) {
    std::vector<Nal> nals;
    size_t i = 0;
    while (i < size) {
        if (i + 4 > size) return std::nullopt;
        const size_t length = (size_t(data[i]) << 24) | (size_t(data[i + 1]) << 16) | (size_t(data[i + 2]) << 8) |
                              size_t(data[i + 3]);
        i += 4;
        if (length == 0 || i + length > size) return std::nullopt;
        nals.push_back({data + i, length});
        i += length;
    }
    return nals;
}

std::vector<std::vector<uint8_t>> splitAccessUnits(const uint8_t* data, size_t size) {
    std::vector<std::vector<uint8_t>> units;
    std::vector<uint8_t> current;
    bool currentHasPicture = false;
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};

    for (const Nal& n : splitAnnexB(data, size)) {
        const uint8_t t = n.type();
        const bool beginsNext = (t == kNalAud || t == kNalSps || t == kNalPps || t == kNalSei) ||
                                (isVcl(t) && startsPicture(n));
        if (beginsNext && currentHasPicture) {
            units.push_back(std::move(current));
            current.clear();
            currentHasPicture = false;
        }
        current.insert(current.end(), kStart, kStart + 4);
        current.insert(current.end(), n.data, n.data + n.size);
        if (isVcl(t)) currentHasPicture = true;
    }
    if (currentHasPicture) units.push_back(std::move(current));
    return units;
}

std::vector<uint8_t> annexBToAvcc(const uint8_t* data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size + 16);
    for (const Nal& n : splitAnnexB(data, size)) {
        const uint32_t len = static_cast<uint32_t>(n.size);
        const uint8_t prefix[4] = {uint8_t(len >> 24), uint8_t(len >> 16), uint8_t(len >> 8), uint8_t(len)};
        out.insert(out.end(), prefix, prefix + 4);
        out.insert(out.end(), n.data, n.data + n.size);
    }
    return out;
}

bool avccToAnnexB(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    auto nals = splitAvcc(data, size);
    if (!nals) return false;
    out.clear();
    out.reserve(size + 16);
    static constexpr uint8_t kStart[4] = {0, 0, 0, 1};
    for (const Nal& n : *nals) {
        out.insert(out.end(), kStart, kStart + 4);
        out.insert(out.end(), n.data, n.data + n.size);
    }
    return true;
}

bool containsIdr(const std::vector<Nal>& nals) {
    for (const Nal& n : nals) {
        if (n.type() == kNalIdr) return true;
    }
    return false;
}

}  // namespace cabled::video
