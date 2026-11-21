#pragma once

#include "common.h"
#include "media_source.h"

#include <mutex>

namespace cabled::vcam {

// What our CLSID creates. Frame Server sets attributes on it, then calls ActivateObject to get the
// media source (pattern from Microsoft's VirtualCamera sample).
class Activate final : public IMFActivate {
public:
    static HRESULT Create(REFIID riid, void** ppv);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFActivate
    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

    // IMFAttributes: all forwarded to an inner attribute store.
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return a_->GetItem(key, value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override { return a_->GetItemType(key, type); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
        return a_->CompareItem(key, value, result);
    }
    STDMETHODIMP Compare(IMFAttributes* other, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override {
        return a_->Compare(other, type, result);
    }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return a_->GetUINT32(key, value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return a_->GetUINT64(key, value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return a_->GetDouble(key, value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return a_->GetGUID(key, value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override { return a_->GetStringLength(key, length); }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override {
        return a_->GetString(key, value, size, length);
    }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override {
        return a_->GetAllocatedString(key, value, length);
    }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return a_->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buffer, UINT32 size, UINT32* blobSize) override {
        return a_->GetBlob(key, buffer, size, blobSize);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buffer, UINT32* size) override {
        return a_->GetAllocatedBlob(key, buffer, size);
    }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override { return a_->GetUnknown(key, riid, ppv); }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return a_->SetItem(key, value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return a_->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return a_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return a_->SetUINT32(key, value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return a_->SetUINT64(key, value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return a_->SetDouble(key, value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return a_->SetGUID(key, value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return a_->SetString(key, value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buffer, UINT32 size) override {
        return a_->SetBlob(key, buffer, size);
    }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return a_->SetUnknown(key, value); }
    STDMETHODIMP LockStore() override { return a_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return a_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* count) override { return a_->GetCount(count); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override {
        return a_->GetItemByIndex(index, key, value);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes* destination) override { return a_->CopyAllItems(destination); }

private:
    Activate() = default;
    ~Activate() = default;

    ModuleRef moduleRef_;
    std::atomic<ULONG> refs_{1};
    std::mutex mutex_;
    ComPtr<IMFAttributes> a_;
    ComPtr<MediaSource> source_;
};

}  // namespace cabled::vcam
