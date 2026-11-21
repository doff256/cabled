#include "vcam_shared.h"

#include <sddl.h>

#include <cstring>
#include <utility>

namespace cabled::vcam {
namespace {

constexpr size_t kSlotStride = (kSlotHeaderBytes + kMaxFrameBytes + 4095) & ~size_t(4095);

// SYSTEM, LOCAL SERVICE, Administrators: full. Interactive users: read + write (the engine runs as
// the signed-in user). Generic rights map onto FILE_MAP_* / EVENT_* for these object types.
constexpr wchar_t kSddl[] = L"D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGWGX;;;IU)";

LARGE_INTEGER qpcFrequency() {
    static const LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    return f;
}

}  // namespace

size_t sectionBytes() { return kHeaderBytes + kSlotStride * kSlotCount; }

uint32_t packedStride(uint32_t fourcc, uint32_t width) {
    switch (fourcc) {
        case kFourccNV12: return width;
        case kFourccYUY2: return width * 2;
        case kFourccRGB32: return width * 4;
        default: return 0;
    }
}

size_t packedFrameBytes(uint32_t fourcc, uint32_t width, uint32_t height) {
    switch (fourcc) {
        case kFourccNV12: return size_t(width) * height * 3 / 2;
        case kFourccYUY2: return size_t(width) * height * 2;
        case kFourccRGB32: return size_t(width) * height * 4;
        default: return 0;
    }
}

std::string fourccName(uint32_t fourcc) {
    switch (fourcc) {
        case kFourccNV12: return "NV12";
        case kFourccYUY2: return "YUY2";
        case kFourccRGB32: return "RGB32";
        default: return "0x" + std::to_string(fourcc);
    }
}

int64_t qpcNow() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double qpcToMs(int64_t ticks) { return double(ticks) * 1000.0 / double(qpcFrequency().QuadPart); }

FrameView packedView(uint32_t fourcc, uint32_t width, uint32_t height, uint8_t* data) {
    FrameView v;
    v.fourcc = fourcc;
    v.width = width;
    v.height = height;
    v.plane0 = data;
    v.pitch0 = packedStride(fourcc, width);
    if (fourcc == kFourccNV12) {
        v.plane1 = data + size_t(width) * height;
        v.pitch1 = width;
    }
    return v;
}

FrameView bufferView(uint32_t fourcc, uint32_t width, uint32_t height, uint8_t* scanline0, LONG pitch,
                     uint32_t bufferLength) {
    FrameView v;
    if (!scanline0 || pitch <= 0) return v;  // bottom-up buffers aren't produced by our allocators
    v.fourcc = fourcc;
    v.width = width;
    v.height = height;
    v.plane0 = scanline0;
    v.pitch0 = static_cast<uint32_t>(pitch);
    if (fourcc == kFourccNV12) {
        // Total = pitch * paddedHeight * 3/2, so the UV plane starts at pitch * paddedHeight.
        uint32_t paddedHeight = bufferLength / v.pitch0 * 2 / 3;
        if (paddedHeight < height) paddedHeight = height;
        v.plane1 = scanline0 + size_t(v.pitch0) * paddedHeight;
        v.pitch1 = v.pitch0;
    }
    return v;
}

bool copyFrame(const FrameView& src, const FrameView& dst) {
    if (!src.valid() || !dst.valid() || src.fourcc != dst.fourcc || src.width != dst.width ||
        src.height != dst.height) {
        return false;
    }
    const uint32_t rowBytes = packedStride(src.fourcc, src.width);
    for (uint32_t y = 0; y < src.height; ++y) {
        std::memcpy(dst.plane0 + size_t(y) * dst.pitch0, src.plane0 + size_t(y) * src.pitch0, rowBytes);
    }
    if (src.fourcc == kFourccNV12) {
        for (uint32_t y = 0; y < src.height / 2; ++y) {
            std::memcpy(dst.plane1 + size_t(y) * dst.pitch1, src.plane1 + size_t(y) * src.pitch1, src.width);
        }
    }
    return true;
}

// ------------------------------------------------------------------ Mapping

Mapping::~Mapping() { reset(); }

Mapping::Mapping(Mapping&& other) noexcept
    : section_(std::exchange(other.section_, nullptr)),
      event_(std::exchange(other.event_, nullptr)),
      header_(std::exchange(other.header_, nullptr)) {}

Mapping& Mapping::operator=(Mapping&& other) noexcept {
    if (this != &other) {
        reset();
        section_ = std::exchange(other.section_, nullptr);
        event_ = std::exchange(other.event_, nullptr);
        header_ = std::exchange(other.header_, nullptr);
    }
    return *this;
}

void Mapping::reset() {
    if (header_) UnmapViewOfFile(header_);
    if (section_) CloseHandle(section_);
    if (event_) CloseHandle(event_);
    header_ = nullptr;
    section_ = nullptr;
    event_ = nullptr;
}

Mapping Mapping::create(const wchar_t* sectionName, const wchar_t* eventName, DWORD* error) {
    Mapping m;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd, nullptr)) {
        if (error) *error = GetLastError();
        return m;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    const unsigned long long size = sectionBytes();
    m.section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, DWORD(size >> 32), DWORD(size),
                                    sectionName);
    const DWORD sectionError = GetLastError();
    if (!m.section_) {
        LocalFree(sd);
        if (error) *error = sectionError;
        return m;
    }
    const bool existed = sectionError == ERROR_ALREADY_EXISTS;
    m.event_ = CreateEventW(&sa, FALSE, FALSE, eventName);
    LocalFree(sd);
    if (!m.event_) {
        if (error) *error = GetLastError();
        m.reset();
        return m;
    }
    m.header_ = static_cast<Header*>(MapViewOfFile(m.section_, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!m.header_) {
        if (error) *error = GetLastError();
        m.reset();
        return m;
    }
    if (!existed || m.header_->magic != kMagic) {
        std::memset(m.header_, 0, sizeof(Header));
        m.header_->layoutVersion = kLayoutVersion;
        m.header_->slotCount = kSlotCount;
        m.header_->slotBytes = kMaxFrameBytes;
        MemoryBarrier();
        m.header_->magic = kMagic;
    }
    if (error) *error = 0;
    return m;
}

Mapping Mapping::open(const wchar_t* sectionName, const wchar_t* eventName, DWORD* error) {
    Mapping m;
    m.section_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, sectionName);
    if (!m.section_) {
        if (error) *error = GetLastError();
        return m;
    }
    m.event_ = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName);
    if (!m.event_) {
        if (error) *error = GetLastError();
        m.reset();
        return m;
    }
    m.header_ = static_cast<Header*>(MapViewOfFile(m.section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
    if (!m.header_ || m.header_->magic != kMagic || m.header_->layoutVersion != kLayoutVersion) {
        if (error) *error = m.header_ ? ERROR_REVISION_MISMATCH : GetLastError();
        m.reset();
        return m;
    }
    if (error) *error = 0;
    return m;
}

SlotHeader* Mapping::slot(uint32_t index) const {
    auto* base = reinterpret_cast<uint8_t*>(header_) + kHeaderBytes + kSlotStride * index;
    return reinterpret_cast<SlotHeader*>(base);
}

uint8_t* Mapping::slotData(uint32_t index) const {
    return reinterpret_cast<uint8_t*>(slot(index)) + kSlotHeaderBytes;
}

bool Mapping::publish(const FrameView& frame, int64_t captureQpc) {
    if (!header_ || !frame.valid()) return false;
    const size_t bytes = packedFrameBytes(frame.fourcc, frame.width, frame.height);
    if (bytes == 0 || bytes > header_->slotBytes) return false;

    const LONG64 seq = header_->latestSeq + 1;
    const uint32_t index = uint32_t(seq % kSlotCount);
    SlotHeader* s = slot(index);
    InterlockedExchange64(&s->seq, 0);  // mark as being written
    if (!copyFrame(frame, packedView(frame.fourcc, frame.width, frame.height, slotData(index)))) return false;
    s->width = frame.width;
    s->height = frame.height;
    s->fourcc = frame.fourcc;
    s->bytes = uint32_t(bytes);
    s->captureQpc = captureQpc;
    InterlockedExchange64(&s->seq, seq);
    InterlockedExchange64(&header_->latestSeq, seq);
    InterlockedExchange64(&header_->feederHeartbeatQpc, qpcNow());
    SetEvent(event_);
    return true;
}

LONG64 Mapping::readLatest(const FrameView& dst, LONG64 lastSeq, int64_t* captureQpc) const {
    if (!header_) return 0;
    const LONG64 seq = header_->latestSeq;
    if (seq <= lastSeq || seq == 0) return 0;
    const uint32_t index = uint32_t(seq % kSlotCount);
    SlotHeader* s = slot(index);
    if (s->seq != seq || s->fourcc != dst.fourcc || s->width != dst.width || s->height != dst.height) return 0;
    const int64_t qpc = s->captureQpc;
    if (!copyFrame(packedView(s->fourcc, s->width, s->height, slotData(index)), dst)) return 0;
    // The writer may have lapped us (3 slots) while we copied; then the copy is torn.
    if (s->seq != seq) return 0;
    if (captureQpc) *captureQpc = qpc;
    return seq;
}

void Mapping::setMessage(const char* text) {
    if (!header_) return;
    strncpy_s(header_->lastMessage, text, _TRUNCATE);
}

}  // namespace cabled::vcam
