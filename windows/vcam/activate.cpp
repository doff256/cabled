#include "activate.h"

namespace cabled::vcam {

HRESULT Activate::Create(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    auto* activate = new (std::nothrow) Activate();
    if (!activate) return E_OUTOFMEMORY;
    HRESULT hr = MFCreateAttributes(&activate->a_, 4);
    if (SUCCEEDED(hr)) hr = activate->QueryInterface(riid, ppv);
    activate->Release();
    return hr;
}

STDMETHODIMP Activate::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFAttributes) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) Activate::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) Activate::Release() {
    const ULONG count = --refs_;
    if (count == 0) delete this;
    return count;
}

STDMETHODIMP Activate::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!source_) {
        ComPtr<MediaSource> source;
        CABLED_RETURN_IF_FAILED(MediaSource::Create(a_.Get(), &source));
        source_ = source;
    }
    return source_->QueryInterface(riid, ppv);
}

STDMETHODIMP Activate::ShutdownObject() {
    // Like Microsoft's sample: the caller owns shutting the source down.
    return S_OK;
}

STDMETHODIMP Activate::DetachObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    source_.Reset();
    return S_OK;
}

}  // namespace cabled::vcam
