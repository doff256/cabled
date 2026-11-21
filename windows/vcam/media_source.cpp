#include "media_source.h"

#include <propvarutil.h>

namespace cabled::vcam {

HRESULT MediaSource::Create(IMFAttributes* activateAttributes, MediaSource** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto* source = new (std::nothrow) MediaSource();
    if (!source) return E_OUTOFMEMORY;
    const HRESULT hr = source->Initialize(activateAttributes);
    if (FAILED(hr)) {
        source->Shutdown();
        source->Release();
        return hr;
    }
    *out = source;
    return S_OK;
}

MediaSource::~MediaSource() { logf("MediaSource destroyed"); }

HRESULT MediaSource::Initialize(IMFAttributes* activateAttributes) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    logf("MediaSource created in %s", exe);

    DWORD error = 0;
    shared_ = Mapping::create(kDefaultSectionName, kDefaultEventName, &error);
    if (shared_.valid()) {
        logf("Shared frame ring ready (%s)", exe);
        shared_.setMessage("media source loaded");
    } else {
        // Expected outside Frame Server: no SeCreateGlobalPrivilege. Placeholder frames only.
        logf("Shared frame ring unavailable (error %lu); placeholder only", error);
    }

    CABLED_RETURN_IF_FAILED(MFCreateEventQueue(&events_));
    CABLED_RETURN_IF_FAILED(MFCreateAttributes(&attributes_, 4));
    if (activateAttributes) CABLED_RETURN_IF_FAILED(activateAttributes->CopyAllItems(attributes_.Get()));

    ComPtr<MediaStream> stream;
    CABLED_RETURN_IF_FAILED(MediaStream::Create(this, 0, &shared_, &stream));
    stream_ = stream;

    ComPtr<IMFStreamDescriptor> sd;
    CABLED_RETURN_IF_FAILED(stream_->GetStreamDescriptor(&sd));
    IMFStreamDescriptor* sds[] = {sd.Get()};
    CABLED_RETURN_IF_FAILED(MFCreatePresentationDescriptor(1, sds, &descriptor_));
    // Selected by default, like a real webcam. Frame Server selects explicitly anyway, but plain
    // source readers (our self-test, some apps) only read streams that start out selected.
    CABLED_RETURN_IF_FAILED(descriptor_->SelectStream(0));
    state_ = State::Stopped;
    return S_OK;
}

// ------------------------------------------------------------------ IUnknown

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) MediaSource::Release() {
    const ULONG count = --refs_;
    if (count == 0) delete this;
    return count;
}

// ------------------------------------------------------------------ events

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        queue = events_;
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                     const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return events_->QueueEventParamVar(type, extendedType, status, value);
}

// ------------------------------------------------------------------ IMFMediaSource

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return MF_E_INVALID_STATE_TRANSITION;  // live source: no pause
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPos) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    if (!pd || !startPos) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;

    DWORD count = 0;
    CABLED_RETURN_IF_FAILED(pd->GetStreamDescriptorCount(&count));
    if (count != 1) return E_INVALIDARG;

    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> sd;
    CABLED_RETURN_IF_FAILED(pd->GetStreamDescriptorByIndex(0, &selected, &sd));
    DWORD id = 0;
    CABLED_RETURN_IF_FAILED(sd->GetStreamIdentifier(&id));
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;

    const bool wasSelected = stream_->IsSelected();
    if (selected) {
        CABLED_RETURN_IF_FAILED(descriptor_->SelectStream(0));
        ComPtr<IMFMediaTypeHandler> handler;
        CABLED_RETURN_IF_FAILED(sd->GetMediaTypeHandler(&handler));
        ComPtr<IMFMediaType> type;
        CABLED_RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
        CABLED_RETURN_IF_FAILED(events_->QueueEventParamUnk(wasSelected ? MEUpdatedStream : MENewStream, GUID_NULL,
                                                            S_OK, static_cast<IMFMediaStream2*>(stream_.Get())));
        CABLED_RETURN_IF_FAILED(stream_->Start(type.Get()));
    } else if (wasSelected) {
        descriptor_->DeselectStream(0);
        stream_->Stop(false);
    }

    PROPVARIANT startTime;
    InitPropVariantFromInt64(MFGetSystemTime(), &startTime);
    const HRESULT hr = events_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &startTime);
    PropVariantClear(&startTime);
    CABLED_RETURN_IF_FAILED(hr);
    state_ = State::Started;
    logf("Source started (stream %s)", selected ? "selected" : "not selected");
    return S_OK;
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    if (state_ != State::Started) return MF_E_INVALID_STATE_TRANSITION;
    state_ = State::Stopped;
    stream_->Stop(true);
    descriptor_->DeselectStream(0);
    PROPVARIANT stopTime;
    InitPropVariantFromInt64(MFGetSystemTime(), &stopTime);
    const HRESULT hr = events_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, &stopTime);
    PropVariantClear(&stopTime);
    logf("Source stopped");
    return hr;
}

STDMETHODIMP MediaSource::Shutdown() {
    ComPtr<MediaStream> stream;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
        state_ = State::Shutdown;
        stream = std::move(stream_);
        if (events_) events_->Shutdown();
        events_.Reset();
        descriptor_.Reset();
        attributes_.Reset();
    }
    if (stream) stream->Shutdown();  // joins the worker, so outside our lock
    if (shared_.valid()) {
        InterlockedExchange(&shared_.header()->sourceState, kSourceIdle);
        shared_.setMessage("media source shut down");
    }
    logf("Source shut down");
    return S_OK;
}

// ------------------------------------------------------------------ IMFMediaSourceEx

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);  // a reference, not a copy (as Microsoft's sample)
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->GetAttributes(attributes);
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) { return E_NOTIMPL; }

// ------------------------------------------------------------------ IMFGetService

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ------------------------------------------------------------------ IKsControl

STDMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ------------------------------------------------------------------ IMFSampleAllocatorControl

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Shutdown) return MF_E_SHUTDOWN;
    if (outputStreamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    ComPtr<IMFVideoSampleAllocator> videoAllocator;
    if (allocator) CABLED_RETURN_IF_FAILED(allocator->QueryInterface(IID_PPV_ARGS(&videoAllocator)));
    return stream_->SetAllocator(videoAllocator.Get());
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                            MFSampleAllocatorUsage* usage) {
    if (!inputStreamId || !usage) return E_POINTER;
    if (outputStreamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    *inputStreamId = outputStreamId;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

}  // namespace cabled::vcam
