#pragma once

// Which images this process mapped and unmapped, and when.
//
// WHY IT EXISTS. A crash at an address "in no module" cannot say whether the
// address never belonged to an image or belonged to one that was unmapped a
// moment earlier -- the module list the crash filter walks has already lost it.
// The shutdown crash (docs/shutdown-freelibrary-race-2026-09-21.md) lands on
// the same 64 KB base plus 0xF780 in every session, which is the shape of a
// DLL that was loaded and then freed, and nothing recorded which. This keeps a
// ring of loads and unloads from the loader's own notification so the crash
// filter can name the image that covered the address and how long ago it left.
//
// The callback runs under the loader lock, on whichever thread loaded or freed
// the image. It does no allocation, no logging and no waiting: a counter, a
// slot, a copy of at most 26 characters of the name. Readers (the crash filter)
// take no lock either; a slot being rewritten while it is read is detected by
// its sequence number and skipped.
//
// LdrRegisterDllNotification is exported by ntdll and documented only in the
// Windows headers' LDR_DLL_NOTIFICATION comments; it is looked up at run time
// and a missing export just leaves the ledger empty ("events=0" in the crash
// lines), never an error.

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace edvr::dll_ledger {

constexpr unsigned kEntries = 512;
constexpr unsigned kNameChars = 26;

struct Entry {
    uint64_t  tick = 0;      // GetTickCount64() at the event, the clock breadcrumbs use
    uintptr_t base = 0;
    uint32_t  size = 0;      // SizeOfImage
    uint32_t  sequence = 0;  // 1-based event number; 0 while the slot is being written
    uint8_t   unloaded = 0;  // 0 loaded, 1 unloaded
    char      name[kNameChars + 1] = {};
};

namespace detail {

// ntdll's own layouts, declared here because the SDK headers do not carry them.
struct CountedString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
};
struct Notification {
    ULONG                Flags;
    const CountedString* FullDllName;
    const CountedString* BaseDllName;
    PVOID                DllBase;
    ULONG                SizeOfImage;
};
constexpr ULONG kReasonLoaded = 1;
constexpr ULONG kReasonUnloaded = 2;

using NotifyFn = void(NTAPI*)(ULONG reason, const Notification* data, PVOID context);
using RegisterFn = LONG(NTAPI*)(ULONG flags, NotifyFn function, PVOID context, PVOID* cookie);
using UnregisterFn = LONG(NTAPI*)(PVOID cookie);

inline Entry         g_entries[kEntries];
inline volatile LONG g_events = 0;
inline PVOID         g_cookie = nullptr;

inline void NTAPI notify(ULONG reason, const Notification* data, PVOID) {
    if (!data || (reason != kReasonLoaded && reason != kReasonUnloaded)) return;
    const LONG number = InterlockedIncrement(&g_events);
    Entry& slot = g_entries[static_cast<unsigned>(number - 1) % kEntries];
    slot.sequence = 0;
    slot.tick = GetTickCount64();
    slot.base = reinterpret_cast<uintptr_t>(data->DllBase);
    slot.size = data->SizeOfImage;
    slot.unloaded = reason == kReasonUnloaded ? 1 : 0;
    unsigned n = 0;
    if (data->BaseDllName && data->BaseDllName->Buffer) {
        const unsigned chars = data->BaseDllName->Length / sizeof(wchar_t);
        for (; n < chars && n < kNameChars; ++n) {
            const wchar_t c = data->BaseDllName->Buffer[n];
            slot.name[n] = (c > 0 && c < 0x80) ? static_cast<char>(c) : '?';
        }
    }
    slot.name[n] = 0;
    MemoryBarrier();
    slot.sequence = static_cast<uint32_t>(number);
}

}  // namespace detail

// Starts recording. Idempotent. False when ntdll does not export the
// registration call or it refused.
inline bool install() noexcept {
    if (detail::g_cookie) return true;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    const auto registerFn = reinterpret_cast<detail::RegisterFn>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "LdrRegisterDllNotification")));
    if (!registerFn) return false;
    PVOID cookie = nullptr;
    if (registerFn(0, &detail::notify, nullptr, &cookie) < 0 || !cookie) return false;
    detail::g_cookie = cookie;
    return true;
}

// Stops recording. The callback lives in this image, so it must be taken down
// before the image can be freed; the entries stay readable.
inline void remove() noexcept {
    if (!detail::g_cookie) return;
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto unregisterFn = ntdll ? reinterpret_cast<detail::UnregisterFn>(reinterpret_cast<void*>(
                                          GetProcAddress(ntdll, "LdrUnregisterDllNotification")))
                                    : nullptr;
    if (unregisterFn) unregisterFn(detail::g_cookie);
    detail::g_cookie = nullptr;
}

// Events recorded so far, loads and unloads together (not capped at the ring).
inline uint32_t events() noexcept { return static_cast<uint32_t>(detail::g_events); }

// The `back`-th most recent event, 0 the newest. False past the ring, past the
// first event, or when the slot was being rewritten as it was read.
inline bool recent(unsigned back, Entry& out) noexcept {
    const uint32_t total = events();
    if (back >= total || back >= kEntries) return false;
    const uint32_t number = total - back;
    const Entry& slot = detail::g_entries[(number - 1) % kEntries];
    if (slot.sequence != number) return false;
    out = slot;
    MemoryBarrier();
    return slot.sequence == number;
}

// The newest event whose image range covers `address`. An unload answers for a
// range that has been freed; a load answers for one that is still mapped.
inline bool covering(uintptr_t address, Entry& out) noexcept {
    Entry e;
    for (unsigned back = 0; recent(back, e); ++back) {
        if (address >= e.base && address - e.base < e.size) {
            out = e;
            return true;
        }
    }
    return false;
}

// Test hook: forgets everything recorded.
inline void clearForTest() noexcept {
    detail::g_events = 0;
    for (Entry& e : detail::g_entries) e = Entry{};
}

}  // namespace edvr::dll_ledger
