#pragma once

#include "common.h"
#include "media_stream.h"
#include "vcam_shared.h"

#include <mutex>

namespace cabled::vcam {

// The virtual camera's media source. One stream. Loaded by Frame Server (and FrameServerMonitor
// when the camera is created), or in-process by our self-test.
class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    static HRESULT Create(IMFAttributes* activateAttributes, MediaSource** out);

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

    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl: no custom controls yet (camera controls go to the phone through the engine).
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength,
                            ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength,
                          ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength,
                         ULONG* bytesReturned) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                   MFSampleAllocatorUsage* usage) override;

private:
    MediaSource() = default;
    ~MediaSource();
    HRESULT Initialize(IMFAttributes* activateAttributes);

    enum class State { Stopped, Started, Shutdown };

    ModuleRef moduleRef_;
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    State state_ = State::Stopped;
    ComPtr<IMFMediaEventQueue> events_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<MediaStream> stream_;
    Mapping shared_;
};

}  // namespace cabled::vcam
