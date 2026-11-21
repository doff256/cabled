#pragma once
// Shared includes and helpers for the virtual camera media source DLL.

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/client.h>

#include <atomic>

namespace cabled::vcam {

using Microsoft::WRL::ComPtr;

// Live COM objects + server locks, for DllCanUnloadNow.
extern std::atomic<long> g_moduleRefs;

struct ModuleRef {
    ModuleRef() { ++g_moduleRefs; }
    ~ModuleRef() { --g_moduleRefs; }
    ModuleRef(const ModuleRef&) { ++g_moduleRefs; }
    ModuleRef& operator=(const ModuleRef&) = default;
};

// printf-style log line to %TEMP%\CabledVCam.log (for Frame Server that's the LOCAL SERVICE profile's
// temp folder) and to OutputDebugString. Never throws.
void logf(const char* format, ...);

#define CABLED_RETURN_IF_FAILED(expr)                                         \
    do {                                                                       \
        const HRESULT hr_ = (expr);                                            \
        if (FAILED(hr_)) {                                                     \
            ::cabled::vcam::logf("%s failed: 0x%08X (%s:%d)", #expr, unsigned(hr_), __FILE__, __LINE__); \
            return hr_;                                                        \
        }                                                                      \
    } while (0)

}  // namespace cabled::vcam
