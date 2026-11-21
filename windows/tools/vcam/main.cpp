// cabled-vcam: R2 spike tool for the Media Foundation virtual camera.
//   selftest  load cabled_vcam_mf.dll in this process and pull frames through a source reader (no admin)
//   register  create the virtual camera (MFCreateVirtualCamera) - the COM class must be registered first
//   remove    remove it again
//   list      list the video capture devices Media Foundation sees
//   feed      be the engine: write test-pattern frames into the shared frame ring
//   check     open a camera like an app would and measure frame rate, latency and content

#include "test_pattern.h"
#include "vcam_shared.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfvirtualcamera.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace cabled::vcam;

namespace {

constexpr wchar_t kDefaultFriendlyName[] = L"Cabled Camera";

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

bool fail(const char* what, HRESULT hr) {
    std::fprintf(stderr, "%s failed: 0x%08X\n", what, unsigned(hr));
    return false;
}

struct TypeInfo {
    uint32_t fourcc = 0;
    UINT32 width = 0, height = 0, fpsNum = 0, fpsDen = 1;
};

TypeInfo describeType(IMFMediaType* t) {
    TypeInfo info;
    GUID subtype{};
    t->GetGUID(MF_MT_SUBTYPE, &subtype);
    info.fourcc = subtype.Data1;
    MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &info.width, &info.height);
    MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &info.fpsNum, &info.fpsDen);
    return info;
}

std::string typeText(const TypeInfo& i) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%-5s %4ux%-4u @ %.2f fps", fourccName(i.fourcc).c_str(), i.width, i.height,
                  i.fpsDen ? double(i.fpsNum) / i.fpsDen : 0.0);
    return buf;
}

// ------------------------------------------------------------------ frame analysis

struct ReadStats {
    int frames = 0;
    int feederFrames = 0;
    int placeholderFrames = 0;
    int repeatedFrames = 0;
    int skippedFrames = 0;
    double seconds = 0;
    std::vector<double> latencyMs;
    std::vector<double> sampleAgeMs;
    std::string error;
};

void printDistribution(const char* label, std::vector<double> v) {
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    std::printf("  %s: min %.1f ms, median %.1f ms, p95 %.1f ms, max %.1f ms\n", label, v.front(), v[v.size() / 2],
                v[std::min(v.size() - 1, v.size() * 95 / 100)], v.back());
}

ReadStats readFrames(IMFSourceReader* reader, double seconds, int maxFrames) {
    ReadStats stats;
    uint32_t lastFrameNumber = 0;
    ComPtr<IMFMediaType> current;
    reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
    const TypeInfo info = describeType(current.Get());

    const auto start = std::chrono::steady_clock::now();
    auto firstFrame = start;
    while (true) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= seconds || (maxFrames > 0 && stats.frames >= maxFrames)) break;

        DWORD streamIndex = 0, flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &streamIndex, &flags,
                                              &timestamp, &sample);
        if (FAILED(hr)) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "ReadSample 0x%08X", unsigned(hr));
            stats.error = buf;
            break;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            stats.error = "source reader reported an error";
            break;
        }
        if (!sample) continue;
        const int64_t receivedQpc = qpcNow();
        // Our media source stamps samples with capture time on the MFGetSystemTime clock, so this is
        // capture -> app latency whatever produced the frame (only meaningful if it's plausible).
        LONGLONG sampleTime = 0;
        if (SUCCEEDED(sample->GetSampleTime(&sampleTime))) {
            const double ageMs = double(MFGetSystemTime() - sampleTime) / 10000.0;
            if (ageMs > 0 && ageMs < 5000) stats.sampleAgeMs.push_back(ageMs);
        }
        if (stats.frames == 0) firstFrame = std::chrono::steady_clock::now();
        ++stats.frames;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, &buffer))) continue;
        ComPtr<IMF2DBuffer2> buffer2d;
        Stamp stamp;
        if (SUCCEEDED(buffer.As(&buffer2d))) {
            BYTE *scan = nullptr, *startPtr = nullptr;
            LONG pitch = 0;
            DWORD length = 0;
            if (SUCCEEDED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan, &pitch, &startPtr, &length))) {
                stamp = readStamp(bufferView(info.fourcc, info.width, info.height, scan, pitch,
                                             length - DWORD(scan - startPtr)));
                buffer2d->Unlock2D();
            }
        } else {
            BYTE* data = nullptr;
            DWORD length = 0;
            if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
                stamp = readStamp(packedView(info.fourcc, info.width, info.height, data));
                buffer->Unlock();
            }
        }
        if (stamp.frame == 0) {
            ++stats.placeholderFrames;
        } else {
            ++stats.feederFrames;
            stats.latencyMs.push_back(qpcToMs(receivedQpc - stamp.qpc));
            if (lastFrameNumber) {
                if (stamp.frame == lastFrameNumber) ++stats.repeatedFrames;
                else if (stamp.frame > lastFrameNumber + 1) stats.skippedFrames += int(stamp.frame - lastFrameNumber - 1);
            }
            lastFrameNumber = stamp.frame;
        }
    }
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - firstFrame).count();
    return stats;
}

void printStats(const ReadStats& s) {
    const double fps = s.seconds > 0 && s.frames > 1 ? (s.frames - 1) / s.seconds : 0;
    std::printf("  %d frames in %.2f s = %.1f fps  (feeder %d, placeholder %d, repeated %d, skipped %d)\n", s.frames,
                s.seconds, fps, s.feederFrames, s.placeholderFrames, s.repeatedFrames, s.skippedFrames);
    // The stamp's timestamp band is only live when `feed` drew the frame; clips carry 0 there.
    std::vector<double> stampLatency;
    for (double ms : s.latencyMs) {
        if (ms > 0 && ms < 5000) stampLatency.push_back(ms);
    }
    printDistribution("feeder -> app latency (stamp)", stampLatency);
    printDistribution("capture -> app latency (sample age)", s.sampleAgeMs);
    if (!s.error.empty()) std::printf("  ERROR: %s\n", s.error.c_str());
}

bool selectType(IMFSourceReader* reader, DWORD index, TypeInfo* out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), index, &type);
    if (FAILED(hr)) return false;
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    if (FAILED(hr)) return fail("SetCurrentMediaType", hr);
    if (out) *out = describeType(type.Get());
    return true;
}

std::vector<TypeInfo> listTypes(IMFSourceReader* reader) {
    std::vector<TypeInfo> types;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), i, &type))) break;
        types.push_back(describeType(type.Get()));
    }
    return types;
}

// ------------------------------------------------------------------ selftest

int cmdSelftest(const std::wstring& dllPath) {
    // A private ring: never touch the real camera's ring (an app might have it open right now).
    const std::wstring ring = L"Local\\CabledSelftest." + std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"CABLED_VCAM_RING", ring.c_str());
    HMODULE dll = LoadLibraryW(dllPath.c_str());
    if (!dll) {
        std::fprintf(stderr, "Can't load %s (error %lu)\n", narrow(dllPath).c_str(), GetLastError());
        return 2;
    }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    auto getClassObject = reinterpret_cast<GetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    if (!getClassObject) return fail("GetProcAddress(DllGetClassObject)", E_FAIL), 2;

    int failures = 0;
    {
        ComPtr<IClassFactory> factory;
        HRESULT hr = getClassObject(kMediaSourceClsid, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return fail("DllGetClassObject", hr), 2;
        ComPtr<IMFActivate> activate;
        hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate));
        if (FAILED(hr)) return fail("CreateInstance(IMFActivate)", hr), 2;
        ComPtr<IMFMediaSource> source;
        hr = activate->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) return fail("ActivateObject", hr), 2;

        ComPtr<IMFMediaSourceEx> sourceEx;
        if (FAILED(source.As(&sourceEx))) ++failures, std::printf("FAIL: no IMFMediaSourceEx\n");
        ComPtr<IMFAttributes> streamAttributes;
        if (sourceEx && SUCCEEDED(sourceEx->GetStreamAttributes(0, &streamAttributes))) {
            GUID category{};
            streamAttributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
            if (category != PINNAME_VIDEO_CAPTURE) ++failures, std::printf("FAIL: stream category\n");
        }

        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
        if (FAILED(hr)) return fail("MFCreateSourceReaderFromMediaSource", hr), 2;

        const auto types = listTypes(reader.Get());
        std::printf("Media source advertises %zu types:\n", types.size());
        for (size_t i = 0; i < types.size(); ++i) std::printf("  [%zu] %s\n", i, typeText(types[i]).c_str());

        for (DWORD i = 0; i < types.size(); ++i) {
            TypeInfo info;
            if (!selectType(reader.Get(), i, &info)) {
                ++failures;
                continue;
            }
            const bool timed = i == 0 || i == 2 || i == 10;  // 1080p30, 1080p60, 1080p120: check pacing
            const ReadStats s = readFrames(reader.Get(), timed ? 2.0 : 10.0, timed ? 0 : 8);
            std::printf("[%lu] %s\n", i, typeText(info).c_str());
            printStats(s);
            const double fps = s.seconds > 0 && s.frames > 1 ? (s.frames - 1) / s.seconds : 0;
            const double want = double(info.fpsNum) / info.fpsDen;
            bool ok = s.error.empty() && s.frames >= (timed ? 10 : 8);
            if (timed && (fps < want * 0.85 || fps > want * 1.15)) ok = false;
            if (!ok) {
                ++failures;
                std::printf("  FAIL\n");
            }
        }
        source->Shutdown();
        activate->DetachObject();
    }
    std::printf("\nSelftest: %s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

// ------------------------------------------------------------------ devices

struct DeviceEntry {
    std::wstring name;
    std::wstring link;
    ComPtr<IMFActivate> activate;
};

std::vector<DeviceEntry> enumerateCameras() {
    std::vector<DeviceEntry> out;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count))) return out;
    for (UINT32 i = 0; i < count; ++i) {
        DeviceEntry e;
        e.activate.Attach(devices[i]);
        wchar_t* s = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(e.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
            e.name = s;
            CoTaskMemFree(s);
        }
        if (SUCCEEDED(e.activate->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s,
                                                     &len))) {
            e.link = s;
            CoTaskMemFree(s);
        }
        out.push_back(std::move(e));
    }
    CoTaskMemFree(devices);
    return out;
}

int cmdList() {
    const auto cams = enumerateCameras();
    std::printf("%zu video capture device(s):\n", cams.size());
    for (const auto& c : cams) std::printf("  %s\n    %s\n", narrow(c.name).c_str(), narrow(c.link).c_str());
    return 0;
}

int cmdRegister(bool session, bool allUsers, const std::wstring& name) {
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource,
                                       session ? MFVirtualCameraLifetime_Session : MFVirtualCameraLifetime_System,
                                       allUsers ? MFVirtualCameraAccess_AllUsers : MFVirtualCameraAccess_CurrentUser,
                                       name.c_str(), kMediaSourceClsidString, nullptr, 0, &camera);
    if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr), 2;
    hr = camera->Start(nullptr);
    if (FAILED(hr)) return fail("IMFVirtualCamera::Start", hr), 2;

    wchar_t* link = nullptr;
    UINT32 len = 0;
    if (SUCCEEDED(camera->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len))) {
        std::printf("Registered \"%s\" (%s lifetime, %s)\n  %s\n", narrow(name).c_str(), session ? "session" : "system",
                    allUsers ? "all users" : "current user", narrow(link).c_str());
        CoTaskMemFree(link);
    }
    if (session) {
        std::printf("Session lifetime: the camera exists while this process runs. Press Enter to remove it.\n");
        (void)std::getchar();
        camera->Remove();
    }
    camera->Shutdown();
    return 0;
}

int cmdRemove(bool allUsers, const std::wstring& name) {
    ComPtr<IMFVirtualCamera> camera;
    HRESULT hr = MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                                       allUsers ? MFVirtualCameraAccess_AllUsers : MFVirtualCameraAccess_CurrentUser,
                                       name.c_str(), kMediaSourceClsidString, nullptr, 0, &camera);
    if (FAILED(hr)) return fail("MFCreateVirtualCamera", hr), 2;
    hr = camera->Remove();
    camera->Shutdown();
    if (FAILED(hr)) return fail("IMFVirtualCamera::Remove", hr), 2;
    std::printf("Removed \"%s\"\n", narrow(name).c_str());
    return 0;
}

// ------------------------------------------------------------------ feed

int cmdFeed(double seconds) {
    std::printf("Feeder: waiting for the camera to be opened by an app (Ctrl+C to stop)...\n");
    const auto start = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(); };
    Mapping ring;
    std::vector<uint8_t> frame;
    uint32_t frameNumber = 0;
    auto nextStatus = std::chrono::steady_clock::now();
    auto nextFrame = std::chrono::steady_clock::now();
    LONG64 lastDelivered = 0;

    while (seconds <= 0 || elapsed() < seconds) {
        if (!ring.valid()) {
            DWORD error = 0;
            ring = Mapping::open(kDefaultSectionName, kDefaultEventName, &error);
            if (!ring.valid()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            InterlockedExchange(&ring.header()->feederProcessId, LONG(GetCurrentProcessId()));
            std::printf("Connected to the frame ring (media source in process %ld)\n", ring.header()->sourceProcessId);
        }
        Header* h = ring.header();
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextStatus) {
            nextStatus = now + std::chrono::seconds(1);
            const LONG64 delivered = h->samplesDelivered;
            std::printf("[%5.1fs] source %s, %s %ldx%ld @ %ld/%ld, delivered %lld (+%lld/s: feeder %lld, placeholder %lld), msg \"%s\"\n",
                        elapsed(), h->sourceState == kSourceStreaming ? "STREAMING" : "idle",
                        fourccName(uint32_t(h->activeFourcc)).c_str(), h->activeWidth, h->activeHeight, h->activeFpsNum,
                        h->activeFpsDen, delivered, delivered - lastDelivered, h->feederFramesDelivered,
                        h->placeholderFramesDelivered, h->lastMessage);
            std::fflush(stdout);
            lastDelivered = delivered;
        }
        if (h->sourceState != kSourceStreaming || h->activeFpsNum <= 0) {
            InterlockedExchange64(&h->feederHeartbeatQpc, qpcNow());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        const uint32_t fourcc = uint32_t(h->activeFourcc);
        const uint32_t w = uint32_t(h->activeWidth), hh = uint32_t(h->activeHeight);
        const size_t bytes = packedFrameBytes(fourcc, w, hh);
        if (bytes == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        frame.resize(bytes);
        const FrameView view = packedView(fourcc, w, hh, frame.data());
        const int64_t stamp = qpcNow();
        drawTestPattern(view, ++frameNumber, stamp);
        ring.publish(view, stamp);

        const auto interval = std::chrono::duration<double>(double(h->activeFpsDen) / double(h->activeFpsNum));
        nextFrame += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
        if (nextFrame < std::chrono::steady_clock::now()) nextFrame = std::chrono::steady_clock::now();
        std::this_thread::sleep_until(nextFrame);
    }
    return 0;
}

// ------------------------------------------------------------------ check

int cmdCheck(const std::wstring& nameFilter, double seconds, int typeIndex) {
    for (auto& cam : enumerateCameras()) {
        if (cam.name.find(nameFilter) == std::wstring::npos) continue;
        std::printf("Opening \"%s\"\n", narrow(cam.name).c_str());
        ComPtr<IMFMediaSource> source;
        HRESULT hr = cam.activate->ActivateObject(IID_PPV_ARGS(&source));
        if (FAILED(hr)) return fail("ActivateObject", hr), 2;
        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
        if (FAILED(hr)) return fail("MFCreateSourceReaderFromMediaSource", hr), 2;
        const auto types = listTypes(reader.Get());
        std::printf("%zu types:\n", types.size());
        for (size_t i = 0; i < types.size(); ++i) std::printf("  [%zu] %s\n", i, typeText(types[i]).c_str());
        TypeInfo info;
        if (typeIndex >= 0 && !selectType(reader.Get(), DWORD(typeIndex), &info)) return 2;
        if (typeIndex < 0) {
            ComPtr<IMFMediaType> current;
            reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current);
            info = describeType(current.Get());
        }
        std::printf("Reading %s for %.0f s\n", typeText(info).c_str(), seconds);
        const ReadStats s = readFrames(reader.Get(), seconds, 0);
        printStats(s);
        source->Shutdown();
        cam.activate->ShutdownObject();
        return s.error.empty() && s.frames > 0 ? 0 : 1;
    }
    std::fprintf(stderr, "No camera whose name contains \"%s\"\n", narrow(nameFilter).c_str());
    return 3;
}

// Raw NV12 frames of the test pattern (frame numbers 1..N, timestamp band 0), for encoding the H.264
// test clips in windows/testdata with an external encoder.
int cmdRawPattern(uint32_t width, uint32_t height, int frames, const std::wstring& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, out.c_str(), L"wb") != 0 || !f) {
        std::fprintf(stderr, "Can't write %s\n", narrow(out).c_str());
        return 2;
    }
    std::vector<uint8_t> frame(packedFrameBytes(kFourccNV12, width, height));
    const FrameView view = packedView(kFourccNV12, width, height, frame.data());
    for (int i = 1; i <= frames; ++i) {
        drawTestPattern(view, uint32_t(i), 0);
        fwrite(frame.data(), 1, frame.size(), f);
    }
    fclose(f);
    std::printf("Wrote %d NV12 frames %ux%u to %s\n", frames, width, height, narrow(out).c_str());
    return 0;
}

std::wstring defaultDllPath() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path = exe;
    path = path.substr(0, path.find_last_of(L"\\/") + 1);
    return path + L"cabled_vcam_mf.dll";
}

constexpr const char* kUsage = R"(cabled-vcam - R2 virtual camera spike tool

  cabled-vcam selftest [--dll <path>]      Load the media source in-process and pull frames (no admin)
  cabled-vcam register [--session] [--all-users] [--name <n>]
                                           Create the virtual camera (COM class must be registered)
  cabled-vcam remove [--all-users] [--name <n>]
  cabled-vcam list                         Video capture devices Media Foundation sees
  cabled-vcam feed [--seconds <s>]         Engine side: write test frames into the frame ring
  cabled-vcam check [--name <part>] [--seconds <s>] [--type <i>]
                                           Open the camera like an app and measure fps / latency
  cabled-vcam rawpattern --out <file> [--width 1920] [--height 1080] [--frames 120]
                                           Dump raw NV12 test-pattern frames (for making test clips)
)";

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) || FAILED(MFStartup(MF_VERSION))) {
        std::fprintf(stderr, "COM / Media Foundation failed to start\n");
        return 70;
    }
    const std::wstring command = argc > 1 ? argv[1] : L"";
    std::wstring dll = defaultDllPath();
    std::wstring name = kDefaultFriendlyName;
    std::wstring filter = L"Cabled";
    bool session = false, allUsers = false;
    double seconds = command == L"check" ? 5.0 : 0.0;
    int typeIndex = -1;
    uint32_t width = 1920, height = 1080;
    int frames = 120;
    std::wstring out;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == L"--dll" && hasValue) dll = argv[++i];
        else if (a == L"--name" && hasValue) name = filter = argv[++i];
        else if (a == L"--session") session = true;
        else if (a == L"--all-users") allUsers = true;
        else if (a == L"--seconds" && hasValue) seconds = _wtof(argv[++i]);
        else if (a == L"--type" && hasValue) typeIndex = _wtoi(argv[++i]);
        else if (a == L"--width" && hasValue) width = uint32_t(_wtoi(argv[++i]));
        else if (a == L"--height" && hasValue) height = uint32_t(_wtoi(argv[++i]));
        else if (a == L"--frames" && hasValue) frames = _wtoi(argv[++i]);
        else if (a == L"--out" && hasValue) out = argv[++i];
        else {
            std::fprintf(stderr, "Unknown option %s\n\n%s", narrow(a).c_str(), kUsage);
            return 64;
        }
    }

    int rc = 64;
    if (command == L"selftest") rc = cmdSelftest(dll);
    else if (command == L"register") rc = cmdRegister(session, allUsers, name);
    else if (command == L"remove") rc = cmdRemove(allUsers, name);
    else if (command == L"list") rc = cmdList();
    else if (command == L"feed") rc = cmdFeed(seconds);
    else if (command == L"check") rc = cmdCheck(filter, seconds, typeIndex);
    else if (command == L"rawpattern" && !out.empty()) rc = cmdRawPattern(width, height, frames, out);
    else std::printf("%s", kUsage);

    MFShutdown();
    CoUninitialize();
    return rc;
}
