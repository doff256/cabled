#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace cabled::vcam {

std::atomic<long> g_moduleRefs{0};

void logf(const char* format, ...) {
    static std::mutex mutex;
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%lu:%lu] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, GetCurrentProcessId(), GetCurrentThreadId(), message);
    OutputDebugStringA(line);

    std::lock_guard<std::mutex> lock(mutex);
    // Prefer %ProgramData%\Cabled\logs (the dev install script creates it, writable by LOCAL SERVICE
    // and readable by the user). Otherwise the process's temp folder.
    wchar_t path[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramData", path, MAX_PATH);
    if (n > 0 && n < MAX_PATH - 40) {
        wcscat_s(path, L"\\Cabled\\logs");
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) path[0] = 0;
        else wcscat_s(path, L"\\vcam.log");
    } else {
        path[0] = 0;
    }
    if (!path[0]) {
        const DWORD tempLength = GetTempPathW(MAX_PATH, path);
        if (tempLength == 0 || tempLength > MAX_PATH - 20) return;
        wcscat_s(path, L"CabledVCam.log");
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") == 0 && f) {
        fputs(line, f);
        fclose(f);
    }
}

}  // namespace cabled::vcam
