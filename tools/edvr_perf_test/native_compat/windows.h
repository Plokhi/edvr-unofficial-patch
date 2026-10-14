// Test-only OS adapter for the header-only CPU rigs on a native non-Windows host.
// Production builds use the SDK, never this include directory.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#define __declspec(x) __attribute__((x))
using DWORD = uint32_t;
using BYTE = unsigned char;
using UINT8 = unsigned char;
using UINT = unsigned int;
using UINT32 = uint32_t;
using HMODULE = void*;
struct LARGE_INTEGER { int64_t QuadPart; };
inline int QueryPerformanceCounter(LARGE_INTEGER* n) {
    n->QuadPart = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return 1;
}
inline int QueryPerformanceFrequency(LARGE_INTEGER* n) { n->QuadPart = 1000000000; return 1; }
inline DWORD GetCurrentThreadId() {
    static std::atomic<DWORD> next{1};
    thread_local DWORD id = next.fetch_add(1);
    return id;
}
inline DWORD GetEnvironmentVariableA(const char* name, char* out, DWORD size) {
    const char* value = std::getenv(name);
    if (!value) return 0;
    const auto n = std::strlen(value);
    if (n >= size) return static_cast<DWORD>(n + 1);
    std::memcpy(out, value, n + 1);
    return static_cast<DWORD>(n);
}
inline HMODULE GetModuleHandleA(const char*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char*) { return nullptr; }

// runtime_profile.h policy tests do not read installation files. Fail-closed
// file stubs only let its unused inline OS loader parse on the native host.
using HANDLE=void*;
inline HANDLE INVALID_HANDLE_VALUE=reinterpret_cast<HANDLE>(intptr_t(-1));
constexpr DWORD GENERIC_READ=0x80000000u,FILE_SHARE_READ=1,FILE_SHARE_WRITE=2,FILE_SHARE_DELETE=4;
constexpr DWORD OPEN_EXISTING=3,FILE_ATTRIBUTE_NORMAL=128,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3;
inline HANDLE CreateFileW(const wchar_t*,DWORD,DWORD,void*,DWORD,DWORD,HANDLE){return INVALID_HANDLE_VALUE;}
inline DWORD GetLastError(){return ERROR_FILE_NOT_FOUND;}
inline int GetFileSizeEx(HANDLE,LARGE_INTEGER*){return 0;}
inline int ReadFile(HANDLE,void*,DWORD,DWORD*,void*){return 0;}
inline int CloseHandle(HANDLE){return 1;}
