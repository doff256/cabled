#include "media_stream.h"

#include "formats.h"
#include "test_pattern.h"

#include <algorithm>
#include <vector>

namespace cabled::vcam {

HRESULT MediaStream::Create(IMFMediaSource* parent, DWORD streamId, Mapping* shared, MediaStream** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* stream = new (std::nothrow) MediaStream(parent, streamId, shared);
    if (!stream) return E_OUTOFMEMORY;
    const HRESULT hr = stream->Initialize();
    if (FAILED(hr)) {
        stream->Release();
        return hr;
    }
    *out = stream;
    return S_OK;
}

MediaStream::MediaStream(IMFMediaSource* parent, DWORD streamId, Mapping* shared)
    : parent_(parent), streamId_(streamId), shared_(shared) {}

MediaStream::~MediaStream() {
    if (worker_.joinable()) {  // only if the source never called Shutdown
        workerStop_ = true;
        SetEvent(wakeEvent_);
        worker_.join();
    }
    if (wakeEvent_) CloseHandle(wakeEvent_);
}

HRESULT MediaStream::Initialize() {
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wakeEvent_) return HRESULT_FROM_WIN32(GetLastError());

    CABLED_RETURN_IF_FAILED(MFCreateEventQueue(&events_));

    std::vector<ComPtr<IMFMediaType>> types;
    CABLED_RETURN_IF_FAILED(CreateAdvertisedTypes(&types));
    std::vector<IMFMediaType*> raw;
    for (auto& t : types) raw.push_back(t.Get());
    CABLED_RETURN_IF_FAILED(MFCreateStreamDescriptor(streamId_, DWORD(raw.size()), raw.data(), &descriptor_));

    ComPtr<IMFMediaTypeHandler> handler;
    CABLED_RETURN_IF_FAILED(descriptor_->GetMediaTypeHandler(&handler));
    CABLED_RETURN_IF_FAILED(handler->SetCurrentMediaType(raw[0]));

    // Same attributes on the stream and its descriptor, as in Microsoft's sample.
    CABLED_RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
    for (IMFAttributes* a : {attributes_.Get(), static_cast<IMFAttributes*>(descriptor_.Get())}) {
        CABLED_RETURN_IF_FAILED(a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
        CABLED_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, streamId_));
        CABLED_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
        CABLED_RETURN_IF_FAILED(a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color));
    }
    return S_OK;
}

// ------------------------------------------------------------------ IUnknown

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) MediaStream::Release() {
    const ULONG count = --refs_;
    if (count == 0) delete this;
    return count;
}

// ------------------------------------------------------------------ events

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = events_;
    }
    return queue->GetEvent(flags, event);  // may block: never hold the lock here
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(type, extendedType, status, value);
}

// ------------------------------------------------------------------ IMFMediaStream

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return parent_.CopyTo(source);
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    if (pendingTokens_.size() >= 16) {  // a misbehaving client; don't grow forever
        pendingTokens_.pop_front();
        if (shared_->valid()) InterlockedIncrement64(&shared_->header()->requestsDropped);
    }
    pendingTokens_.emplace_back(token);
    SetEvent(wakeEvent_);
    return S_OK;
}

// ------------------------------------------------------------------ IMFMediaStream2

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    logf("SetStreamState %d (was %d)", int(state), int(state_));
    switch (state) {
        case MF_STREAM_STATE_RUNNING:
            if (state_ == MF_STREAM_STATE_RUNNING) return S_OK;
            if (!currentType_) return MF_E_INVALIDREQUEST;
            return StartLocked(currentType_.Get());
        case MF_STREAM_STATE_PAUSED:
            if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            state_ = MF_STREAM_STATE_PAUSED;
            return S_OK;
        case MF_STREAM_STATE_STOPPED:
            StopLocked();
            return S_OK;
        default:
            return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

// ------------------------------------------------------------------ control from the source

HRESULT MediaStream::Start(IMFMediaType* type) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    selected_ = true;
    CABLED_RETURN_IF_FAILED(StartLocked(type));
    return events_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Stop(bool sendEvent) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    selected_ = false;
    StopLocked();
    if (sendEvent) return events_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
    return S_OK;
}

void MediaStream::Shutdown() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_) return;
        shutdown_ = true;
        workerStop_ = true;
        SetEvent(wakeEvent_);
        worker = std::move(worker_);
    }
    if (worker.joinable()) worker.join();
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = MF_STREAM_STATE_STOPPED;
    pendingTokens_.clear();
    if (events_) events_->Shutdown();
    events_.Reset();
    parent_.Reset();
    attributes_.Reset();
    descriptor_.Reset();
    allocator_.Reset();
    providedAllocator_.Reset();
    currentType_.Reset();
}

HRESULT MediaStream::SetAllocator(IMFVideoSampleAllocator* allocator) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ == MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    providedAllocator_ = allocator;
    logf("Frame Server provided a sample allocator");
    return S_OK;
}

bool MediaStream::IsSelected() {
    std::lock_guard<std::mutex> lock(mutex_);
    return selected_;
}

HRESULT MediaStream::GetAttributes(IMFAttributes** attributes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

// ------------------------------------------------------------------ internals (lock held)

HRESULT MediaStream::ConfigureFormatLocked(IMFMediaType* type) {
    GUID subtype{};
    CABLED_RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype));
    UINT32 w = 0, h = 0;
    CABLED_RETURN_IF_FAILED(MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h));
    UINT32 num = 30, den = 1;
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    if (num == 0 || den == 0) {
        num = 30;
        den = 1;
    }
    if (packedFrameBytes(subtype.Data1, w, h) == 0) return MF_E_INVALIDMEDIATYPE;
    fourcc_ = subtype.Data1;
    width_ = w;
    height_ = h;
    fpsNum_ = num;
    fpsDen_ = den;
    currentType_ = type;

    allocator_.Reset();
    if (providedAllocator_) {
        allocator_ = providedAllocator_;
    } else {
        // In-process use (our self-test, plain source readers): nobody provides an allocator.
        ComPtr<IMFVideoSampleAllocatorEx> own;
        CABLED_RETURN_IF_FAILED(MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&own)));
        allocator_ = own;
    }
    CABLED_RETURN_IF_FAILED(allocator_->InitializeSampleAllocator(10, type));
    logf("Stream format %s %ux%u @ %u/%u, %s allocator", fourccName(fourcc_).c_str(), width_, height_, fpsNum_,
         fpsDen_, providedAllocator_ ? "provided" : "own");
    return S_OK;
}

HRESULT MediaStream::StartLocked(IMFMediaType* type) {
    const bool sameType = currentType_ && type == currentType_.Get() && allocator_;
    if (worker_.joinable() && !sameType) {
        // Format change while running: stop the worker before touching the allocator.
        workerStop_ = true;
        SetEvent(wakeEvent_);
        std::thread worker = std::move(worker_);
        mutex_.unlock();
        worker.join();
        mutex_.lock();
    }
    if (!sameType) CABLED_RETURN_IF_FAILED(ConfigureFormatLocked(type));

    state_ = MF_STREAM_STATE_RUNNING;
    lastSeq_ = 0;
    lastDeliveryQpc_ = 0;
    if (shared_->valid()) {
        Header* h = shared_->header();
        InterlockedExchange(&h->activeWidth, LONG(width_));
        InterlockedExchange(&h->activeHeight, LONG(height_));
        InterlockedExchange(&h->activeFpsNum, LONG(fpsNum_));
        InterlockedExchange(&h->activeFpsDen, LONG(fpsDen_));
        InterlockedExchange(&h->activeFourcc, LONG(fourcc_));
        InterlockedExchange(&h->sourceProcessId, LONG(GetCurrentProcessId()));
        InterlockedIncrement(&h->startCount);
        InterlockedExchange(&h->sourceState, kSourceStreaming);
    }
    if (!worker_.joinable()) {
        workerStop_ = false;
        worker_ = std::thread([this] { WorkerLoop(); });
    }
    return S_OK;
}

void MediaStream::StopLocked() {
    state_ = MF_STREAM_STATE_STOPPED;
    pendingTokens_.clear();
    if (worker_.joinable()) {
        workerStop_ = true;
        SetEvent(wakeEvent_);
        std::thread worker = std::move(worker_);
        mutex_.unlock();
        worker.join();
        mutex_.lock();
    }
    if (shared_->valid()) InterlockedExchange(&shared_->header()->sourceState, kSourceIdle);
}

void MediaStream::WorkerLoop() {
    // The worker is always joined (Stop, Shutdown, format change, destructor) before `this` dies.
    // Plain wait timeouts round up to the ~15.6 ms system tick (30 fps came out as ~21), so pace with
    // a high-resolution waitable timer on a fixed cadence instead.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // pre-1803 fallback

    const double intervalMs = 1000.0 * double(fpsDen_) / double(fpsNum_);
    const double feederTimeoutMs = 500.0;
    HANDLE handles[3] = {wakeEvent_, timer, shared_->valid() ? shared_->frameEvent() : nullptr};
    const DWORD handleCount = handles[2] ? 3 : 2;
    int64_t nextDueQpc = 0;  // when the next repeat/placeholder frame is due

    while (true) {
        double waitMs = 100.0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (workerStop_ || shutdown_) break;
            const int64_t now = qpcNow();
            bool feederActive = false;
            if (shared_->valid()) {
                const int64_t beat = shared_->header()->feederHeartbeatQpc;
                feederActive = beat != 0 && qpcToMs(now - beat) < feederTimeoutMs;
                InterlockedExchange64(&shared_->header()->sourceHeartbeatQpc, now);
            }
            // With a live engine, its frames set the pace and we only repeat if it stalls.
            const double cadenceMs = feederActive ? 2.0 * intervalMs : intervalMs;
            const int64_t cadenceQpc = int64_t(cadenceMs / qpcToMs(1));

            if (state_ == MF_STREAM_STATE_RUNNING && !pendingTokens_.empty()) {
                bool delivered = false;
                const LONG64 latest = feederActive ? shared_->header()->latestSeq : 0;
                if (feederActive && latest > lastSeq_) {
                    DeliverLocked(/*newFrameOnly=*/true, &delivered);  // fresh engine frame: send now
                    if (delivered) {
                        nextDueQpc = now + cadenceQpc;
                    } else {
                        // Unusable (another format/size, e.g. mid-switch, or torn): skip it so the
                        // repeat/placeholder cadence below keeps the app fed instead of stalling.
                        lastSeq_ = latest;
                    }
                }
                if (!delivered && (nextDueQpc == 0 || now >= nextDueQpc)) {
                    DeliverLocked(/*newFrameOnly=*/false, &delivered);  // repeat or placeholder
                    if (delivered) {
                        // Keep a steady cadence; resync if we fell more than a frame behind.
                        nextDueQpc = (nextDueQpc == 0 || now - nextDueQpc > cadenceQpc) ? now + cadenceQpc
                                                                                         : nextDueQpc + cadenceQpc;
                    }
                }
                waitMs = nextDueQpc > now ? qpcToMs(nextDueQpc - now) : 1.0;
            }
        }
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(waitMs * 10000.0));  // relative, 100 ns units
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        WaitForMultipleObjects(handleCount, handles, FALSE, INFINITE);
    }
    CloseHandle(timer);
}

HRESULT MediaStream::DeliverLocked(bool newFrameOnly, bool* delivered) {
    *delivered = false;
    ComPtr<IMFSample> sample;
    HRESULT hr = allocator_->AllocateSample(&sample);
    if (hr == MF_E_SAMPLEALLOCATOR_EMPTY) return hr;  // the app still holds every sample: try next tick
    CABLED_RETURN_IF_FAILED(hr);

    ComPtr<IMFMediaBuffer> buffer;
    CABLED_RETURN_IF_FAILED(sample->GetBufferByIndex(0, &buffer));
    ComPtr<IMF2DBuffer2> buffer2d;
    CABLED_RETURN_IF_FAILED(buffer.As(&buffer2d));
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    BYTE* start = nullptr;
    DWORD length = 0;
    CABLED_RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length));
    const DWORD usable = length - DWORD(scanline0 - start);
    const FrameView view = bufferView(fourcc_, width_, height_, scanline0, pitch, usable);

    int64_t captureQpc = 0;
    LONG64 seq = 0;
    if (view.valid() && shared_->valid()) {
        seq = shared_->readLatest(view, newFrameOnly ? lastSeq_ : 0, &captureQpc);
    }
    if (seq == 0 && newFrameOnly) {
        buffer2d->Unlock2D();
        return S_FALSE;  // the frame didn't match our format or was torn; try again next tick
    }
    if (seq == 0 && view.valid()) drawPlaceholder(view, placeholderTick_++);
    buffer2d->Unlock2D();

    const LONGLONG now100ns = MFGetSystemTime();
    const LONGLONG age100ns = seq ? LONGLONG(qpcToMs(qpcNow() - captureQpc) * 10000.0) : 0;
    sample->SetSampleTime(now100ns - age100ns);
    sample->SetSampleDuration(LONGLONG(10'000'000.0 * double(fpsDen_) / double(fpsNum_)));
    ComPtr<IUnknown> token = pendingTokens_.front();
    pendingTokens_.pop_front();
    if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());

    CABLED_RETURN_IF_FAILED(events_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get()));
    lastDeliveryQpc_ = qpcNow();
    if (seq) lastSeq_ = seq;
    *delivered = true;
    if (shared_->valid()) {
        Header* h = shared_->header();
        InterlockedIncrement64(&h->samplesDelivered);
        InterlockedIncrement64(seq ? &h->feederFramesDelivered : &h->placeholderFramesDelivered);
    }
    return S_OK;
}

}  // namespace cabled::vcam
