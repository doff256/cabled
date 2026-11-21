#pragma once

#include "common.h"
#include "vcam_shared.h"

#include <deque>
#include <mutex>
#include <thread>

namespace cabled::vcam {

// The camera's single video stream. Frames are delivered by a worker thread at the negotiated frame
// rate: a fresh engine frame as soon as it arrives, otherwise a repeat of the last one or the
// placeholder, so the consuming app never stalls (R-73).
class MediaStream final : public IMFMediaStream2 {
public:
    static HRESULT Create(IMFMediaSource* parent, DWORD streamId, Mapping* shared, MediaStream** out);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // Called by the media source.
    HRESULT Start(IMFMediaType* type);  // queues MEStreamStarted
    HRESULT Stop(bool sendEvent);
    void Shutdown();
    HRESULT SetAllocator(IMFVideoSampleAllocator* allocator);
    bool IsSelected();
    HRESULT GetAttributes(IMFAttributes** attributes);

private:
    MediaStream(IMFMediaSource* parent, DWORD streamId, Mapping* shared);
    ~MediaStream();
    HRESULT Initialize();
    HRESULT StartLocked(IMFMediaType* type);
    void StopLocked();
    HRESULT ConfigureFormatLocked(IMFMediaType* type);
    void WorkerLoop();
    HRESULT DeliverLocked(bool newFrameOnly, bool* delivered);

    ModuleRef moduleRef_;
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    ComPtr<IMFMediaSource> parent_;  // released in Shutdown to break the cycle
    DWORD streamId_;
    Mapping* shared_;  // owned by the source; may be invalid (then: placeholder only)

    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFVideoSampleAllocator> providedAllocator_;
    ComPtr<IMFVideoSampleAllocator> allocator_;  // in use: the provided one or our own
    ComPtr<IMFMediaType> currentType_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    bool selected_ = false;
    bool shutdown_ = false;

    uint32_t fourcc_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    UINT32 fpsNum_ = 30;
    UINT32 fpsDen_ = 1;

    std::deque<ComPtr<IUnknown>> pendingTokens_;  // one entry per RequestSample (entries may be null)
    std::thread worker_;
    HANDLE wakeEvent_ = nullptr;
    bool workerStop_ = false;
    LONG64 lastSeq_ = 0;
    int64_t lastDeliveryQpc_ = 0;
    uint64_t placeholderTick_ = 0;
};

}  // namespace cabled::vcam
