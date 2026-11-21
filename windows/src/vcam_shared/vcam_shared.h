#pragma once
// Shared-memory frame ring between the Cabled engine (user session) and the virtual camera media
// source (inside Frame Server: session 0, LOCAL SERVICE). See docs/virtual-camera.md, D-09.
//
// Who creates what: only the media source side can create Global\ objects (it has
// SeCreateGlobalPrivilege; normal user processes don't). So the source creates the section and the
// event with a DACL that lets the interactive user open them, and the engine opens them by name.
// The section only exists while some app has the camera open, so the engine can also use it to
// tell when the camera is in use.

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace cabled::vcam {

// Fixed forever (D-23): changing it would orphan cameras already registered on users' PCs.
// {5A7CE6A3-D22D-4EF6-BC88-E7775C0315E6}
inline constexpr GUID kMediaSourceClsid = {
    0x5a7ce6a3, 0xd22d, 0x4ef6, {0xbc, 0x88, 0xe7, 0x77, 0x5c, 0x03, 0x15, 0xe6}};
inline constexpr wchar_t kMediaSourceClsidString[] = L"{5A7CE6A3-D22D-4EF6-BC88-E7775C0315E6}";

inline constexpr wchar_t kDefaultSectionName[] = L"Global\\CabledVCam.v1";
inline constexpr wchar_t kDefaultEventName[] = L"Global\\CabledVCam.v1.Frame";

inline constexpr uint32_t kMagic = 0x43564243;  // "CBVC"
inline constexpr uint32_t kLayoutVersion = 1;
inline constexpr uint32_t kSlotCount = 3;
inline constexpr uint32_t kMaxWidth = 3840;
inline constexpr uint32_t kMaxHeight = 2160;
inline constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 4;  // RGB32 is the largest format
inline constexpr uint32_t kHeaderBytes = 4096;
inline constexpr uint32_t kSlotHeaderBytes = 64;

// Pixel formats as FOURCC codes (same values as the MFVideoFormat_* Data1 fields).
inline constexpr uint32_t kFourccNV12 = 0x3231564E;  // 'NV12'
inline constexpr uint32_t kFourccYUY2 = 0x32595559;  // 'YUY2'
inline constexpr uint32_t kFourccRGB32 = 22;          // D3DFMT_X8R8G8B8 = Data1 of MFVideoFormat_RGB32

enum SourceState : int32_t { kSourceIdle = 0, kSourceStreaming = 1 };

struct Header {
    uint32_t magic;
    uint32_t layoutVersion;
    uint32_t slotCount;
    uint32_t slotBytes;  // bytes of pixel data each slot can hold

    // --- written by the media source (Frame Server side) ---
    volatile LONG64 sourceHeartbeatQpc;
    volatile LONG sourceState;  // SourceState
    volatile LONG sourceProcessId;
    volatile LONG activeWidth;  // the format the consuming app chose
    volatile LONG activeHeight;
    volatile LONG activeFpsNum;
    volatile LONG activeFpsDen;
    volatile LONG activeFourcc;
    volatile LONG startCount;  // how many times a stream started
    volatile LONG64 samplesDelivered;
    volatile LONG64 feederFramesDelivered;
    volatile LONG64 placeholderFramesDelivered;
    volatile LONG64 requestsDropped;
    char lastMessage[256];

    // --- written by the engine / feeder (user side) ---
    volatile LONG64 feederHeartbeatQpc;
    volatile LONG64 latestSeq;  // newest complete frame; 0 = none yet. Slot = seq % slotCount
    volatile LONG feederProcessId;
};
static_assert(sizeof(Header) <= kHeaderBytes);

struct SlotHeader {
    volatile LONG64 seq;  // frame sequence held in this slot; written last
    uint32_t width;
    uint32_t height;
    uint32_t fourcc;
    uint32_t bytes;
    int64_t captureQpc;
};
static_assert(sizeof(SlotHeader) <= kSlotHeaderBytes);

// A frame in memory: one plane (YUY2, RGB32) or two (NV12: Y, then interleaved UV).
struct FrameView {
    uint32_t fourcc = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t* plane0 = nullptr;
    uint32_t pitch0 = 0;
    uint8_t* plane1 = nullptr;  // NV12 only
    uint32_t pitch1 = 0;
    bool valid() const { return plane0 != nullptr; }
};

size_t sectionBytes();
size_t packedFrameBytes(uint32_t fourcc, uint32_t width, uint32_t height);  // 0 = unsupported format
uint32_t packedStride(uint32_t fourcc, uint32_t width);
std::string fourccName(uint32_t fourcc);
int64_t qpcNow();
double qpcToMs(int64_t ticks);

// Tightly packed frame at `data` (the layout used inside slots).
FrameView packedView(uint32_t fourcc, uint32_t width, uint32_t height, uint8_t* data);
// A locked Media Foundation 2D buffer. For NV12 the UV plane is placed from the buffer length,
// because some allocators pad the Y plane's height.
FrameView bufferView(uint32_t fourcc, uint32_t width, uint32_t height, uint8_t* scanline0, LONG pitch,
                     uint32_t bufferLength);
// Same format and size required. Returns false otherwise.
bool copyFrame(const FrameView& src, const FrameView& dst);

// A mapped view of the section. Move-only.
class Mapping {
public:
    Mapping() = default;
    ~Mapping();
    Mapping(Mapping&& other) noexcept;
    Mapping& operator=(Mapping&& other) noexcept;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    // Media source side. Creates (or opens, if it already exists) the section and the event with a
    // DACL that lets interactive users read and write them. Creating Global\ names needs
    // SeCreateGlobalPrivilege.
    static Mapping create(const wchar_t* sectionName, const wchar_t* eventName, DWORD* error);
    // Engine side: open what the media source created.
    static Mapping open(const wchar_t* sectionName, const wchar_t* eventName, DWORD* error);

    bool valid() const { return header_ != nullptr; }
    Header* header() const { return header_; }
    HANDLE frameEvent() const { return event_; }

    // Engine side: copy a frame into the next slot, bump latestSeq, signal the event.
    bool publish(const FrameView& frame, int64_t captureQpc);

    // Media source side: copy the newest frame into `dst` if it has dst's format and size and is
    // newer than `lastSeq`. Returns its seq, or 0 if there was nothing usable.
    LONG64 readLatest(const FrameView& dst, LONG64 lastSeq, int64_t* captureQpc) const;

    void setMessage(const char* text);

private:
    SlotHeader* slot(uint32_t index) const;
    uint8_t* slotData(uint32_t index) const;
    void reset();

    HANDLE section_ = nullptr;
    HANDLE event_ = nullptr;
    Header* header_ = nullptr;
};

}  // namespace cabled::vcam
