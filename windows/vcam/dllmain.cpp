// COM entry points for cabled_vcam_mf.dll.

#include "activate.h"
#include "common.h"
#include "vcam_shared.h"

#include <string>

using namespace cabled::vcam;

namespace {

HMODULE g_module = nullptr;

class ClassFactory final : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG count = --refs_;
        if (count == 0) delete this;
        return count;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        return Activate::Create(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_moduleRefs;
        else --g_moduleRefs;
        return S_OK;
    }

private:
    ModuleRef moduleRef_;
    std::atomic<ULONG> refs_{1};
};

const std::wstring kClassKey = std::wstring(L"Software\\Classes\\CLSID\\") + kMediaSourceClsidString;

HRESULT setValue(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                            DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return HRESULT_FROM_WIN32(status);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != kMediaSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() { return g_moduleRefs.load() == 0 ? S_OK : S_FALSE; }

// HKLM, because Frame Server runs as LOCAL SERVICE and doesn't see the user's HKCU classes.
// Needs admin (regsvr32 from an elevated prompt).
STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    HRESULT hr = setValue(HKEY_LOCAL_MACHINE, kClassKey, nullptr, L"Cabled virtual camera source");
    if (SUCCEEDED(hr)) hr = setValue(HKEY_LOCAL_MACHINE, kClassKey + L"\\InprocServer32", nullptr, path);
    if (SUCCEEDED(hr)) hr = setValue(HKEY_LOCAL_MACHINE, kClassKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    return hr;
}

STDAPI DllUnregisterServer() {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kClassKey.c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
