// The game-code trampolines (vr_window_trampolines.h says what and why): the mode request's window rewrite, and the settings copy's hold.
#include "vr_window_trampolines.h"

#include "device_hook.h"     // deviceHookPanelSettings: DisplaySettings.xml's window size, for the log
#include "vr_ssaa_gate.h"    // vrSsaaGateFrame: the frame number the log carries
#include "vr_ssaa_hold.h"    // vrSsaaHoldVrLive, vrSsaaHoldAfterCopy, vrSsaaHoldReadSettings

#include "../common/code_hook.h"
#include "../common/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr {
namespace {

// The mode request (0x7E9D50). Its first eleven bytes, read from build 332841: mov rax,rsp; mov [rax+0x10],rbx; mov [rax+0x18],rsi.
// The request is rdx on entry; kind is [request+0x20] (a dword), the mode-table index is [request+0x18] (a qword). [request] is a
// locked compare-exchange target in that function, so it is never written here.
constexpr uint32_t kRequestRva = 0x7E9D50;
constexpr uint8_t kRequestBytes[11] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18};
constexpr uintptr_t kReqKindOff = 0x20, kReqIndexOff = 0x18;

// The settings copy W (0x281C0D0): the block copy into the render context at ctx+0x3428 (ctx = the first argument's +0x1180). Its first
// eleven bytes: mov r11,rsp; push rbx; sub rsp,0x4D0.
constexpr uint32_t kCopyRva = 0x281C0D0;
constexpr uint8_t kCopyBytes[11] = {0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x81, 0xEC, 0xD0, 0x04, 0x00, 0x00};

constexpr uint32_t kRewriteCap = 64;

using Fn8 = uint64_t (*)(void*, void*, void*, void*, void*, void*, void*, void*);
Fn8 g_requestOrig = nullptr;
Fn8 g_copyOrig = nullptr;
CodeHook g_requestHook;
CodeHook g_copyHook;

// The last non-windowed request accepted while VR was inactive: the display mode the user had (kind 1 or 2 and its mode index).
std::atomic<bool> g_remembered{false};
std::atomic<uint32_t> g_rememberedKind{0};
std::atomic<uint64_t> g_rememberedIndex{0};
std::atomic<uint32_t> g_rewrites{0};
std::atomic<bool> g_noneLogged{false};

// Guarded reads and writes: a fault in the game's memory must not become the game's crash. No objects inside the guards.
bool readU32(uintptr_t at, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint32_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readU64(uintptr_t at, uint64_t* out) {
    __try {
        *out = *reinterpret_cast<const volatile uint64_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writeU32(uintptr_t at, uint32_t v) {
    __try {
        *reinterpret_cast<volatile uint32_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool writeU64(uintptr_t at, uint64_t v) {
    __try {
        *reinterpret_cast<volatile uint64_t*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool readGuardedBytes(const uint8_t* at, uint8_t* out, size_t n) {
    __try {
        for (size_t i = 0; i < n; ++i) out[i] = at[i];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The mode request's window rewrite (H6, the maintainer's decision: keep the user's display mode when 3D is on). VR inactive: a
// non-windowed request is remembered and passes unchanged. VR active (or the settings file's mode is on at a windowed request): a
// windowed request (kind 0) is rewritten to the remembered kind and mode index before the original runs, so the apply builds the
// remembered mode. Nothing is changed while VR is inactive.
uint64_t requestReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    const uintptr_t req = reinterpret_cast<uintptr_t>(b);
    uint32_t kind = 0;
    uint64_t index = 0;
    const bool haveKind = readU32(req + kReqKindOff, &kind);
    const bool haveIndex = readU64(req + kReqIndexOff, &index);
    bool vrOn = vrSsaaHoldVrLive();
    if (!vrOn && haveKind && kind == 0) {
        int mode = 0;
        char raw[24] = {};
        vrOn = vrSsaaHoldReadSettings(&mode, raw, sizeof(raw)) && mode != 0;  // the request at the switch, before the live session is up
    }
    if (!vrOn) {
        if (haveKind && haveIndex && kind != 0) {
            g_rememberedKind.store(kind, std::memory_order_release);
            g_rememberedIndex.store(index, std::memory_order_release);
            g_remembered.store(true, std::memory_order_release);
        }
    } else if (haveKind && kind == 0) {
        uint32_t dw = 0, dh = 0;
        float hmd = 0.0f, ssaa = 0.0f;
        deviceHookPanelSettings(&hmd, &ssaa, &dw, &dh);
        if (g_remembered.load(std::memory_order_acquire)) {
            const uint32_t rk = g_rememberedKind.load(std::memory_order_acquire);
            const uint64_t ri = g_rememberedIndex.load(std::memory_order_acquire);
            if (writeU32(req + kReqKindOff, rk) && writeU64(req + kReqIndexOff, ri) &&
                g_rewrites.fetch_add(1, std::memory_order_relaxed) < kRewriteCap) {
                Log::get().note("vr window: kept the display mode at 3D on: requested kind 0 %ux%u (windowed), applied kind %u "
                                "%ux%u (remembered; mode index %llu; DisplaySettings size); frame %u",
                                dw, dh, rk, dw, dh, static_cast<unsigned long long>(ri), vrSsaaGateFrame());
            }
        } else if (!g_noneLogged.exchange(true, std::memory_order_acq_rel)) {
            Log::get().note("vr window: 3D on requested kind 0 and no non-windowed mode was remembered; the request is left as the "
                            "game made it (DisplaySettings %ux%u)", dw, dh);
        }
    }
    return g_requestOrig(a, b, c, d, e, f, g, h);
}

// The settings copy (W): the original copies the settings block into the render context; then the hold (vrSsaaHoldAfterCopy) reads
// the copied value and, while VR is live, sets the field to 1.0. ctx is the first argument's +0x1180, read before the copy.
uint64_t copyReplacement(void* a, void* b, void* c, void* d, void* e, void* f, void* g, void* h) {
    uint64_t ctx = 0;
    readU64(reinterpret_cast<uintptr_t>(a) + 0x1180, &ctx);
    const uint64_t r = g_copyOrig(a, b, c, d, e, f, g, h);
    if (ctx) vrSsaaHoldAfterCopy(static_cast<uintptr_t>(ctx));
    return r;
}

// A relay within +/-2 GB of the target: the five-byte jump CodeHook writes can reach it, and the relay jumps to the replacement, which
// may be anywhere (kinematic_eval_hook.cpp's allocateRelay, copied rather than shared, as that file does with its writer hook).
// Layout: jmp qword ptr [rip+0] (FF 25 00 00 00 00), then the replacement's address. Registers and the stack pass through unchanged.
constexpr size_t kRelayBytes = 14;
uint8_t* allocateRelayNear(uintptr_t target) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t floor = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
    const uintptr_t ceiling = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
    const uintptr_t distance = uintptr_t(INT32_MAX) - 0x10000u;
    uintptr_t at = target > distance ? target - distance : floor;
    if (at < floor) at = floor;
    const uintptr_t limit = target > ceiling - distance ? ceiling : target + distance;
    while (at < limit) {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQuery(reinterpret_cast<void*>(at), &region, sizeof(region))) break;
        const uintptr_t start = reinterpret_cast<uintptr_t>(region.BaseAddress);
        if (region.RegionSize > UINTPTR_MAX - start) break;
        const uintptr_t end = start + region.RegionSize;
        if (region.State == MEM_FREE) {
            uintptr_t candidate = at > start ? at : start;
            if (candidate > UINTPTR_MAX - (granularity - 1)) break;
            candidate = (candidate + granularity - 1) & ~(granularity - 1);
            if (candidate < limit && candidate < end && end - candidate >= 4096) {
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT,
                                                             PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

uint8_t* buildRelayNear(uintptr_t target, void* replacement) {
    uint8_t* relay = allocateRelayNear(target);
    if (!relay) return nullptr;
    const uint8_t body[kRelayBytes] = {0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(relay, body, sizeof(body));
    const uintptr_t to = reinterpret_cast<uintptr_t>(replacement);
    std::memcpy(relay + 6, &to, 8);
    DWORD old = 0;
    if (!VirtualProtect(relay, 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(GetCurrentProcess(), relay, kRelayBytes)) {
        VirtualFree(relay, 0, MEM_RELEASE);
        return nullptr;
    }
    return relay;  // kept for the process lifetime: the installed patch jumps here
}

// Pins the function's first bytes, then installs the entry through a near relay. False (and no patch) when the bytes differ, no
// relay can be placed, or CodeHook refuses.
bool installOne(const uint8_t* base, uint32_t rva, const uint8_t* want, size_t len, void* replacement, void** origOut,
                CodeHook* hook, const char* name) {
    uint8_t got[11] = {};
    if (!readGuardedBytes(base + rva, got, len) || std::memcmp(got, want, len) != 0) {
        Log::get().note("vr window: trampoline %s refused: the bytes at 0x%X are not build 332841's; nothing patched", name, rva);
        return false;
    }
    const uintptr_t target = reinterpret_cast<uintptr_t>(base) + rva;
    uint8_t* relay = buildRelayNear(target, replacement);
    if (!relay) {
        Log::get().note("vr window: trampoline %s refused: no page within +/-2 GB of the target for its relay; nothing patched", name);
        return false;
    }
    if (!hook->install(reinterpret_cast<void*>(target), relay, origOut, name)) {
        Log::get().note("vr window: trampoline %s refused by CodeHook (the prologue cannot be relocated; the reason is in the line "
                        "above); nothing patched", name);
        return false;
    }
    return true;
}

}  // namespace

void vrWindowTrampolinesInstall() {
    static bool done = false;
    if (done) return;
    done = true;
    const uint8_t* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) {
        Log::get().note("vr window: trampolines off: the game module could not be found");
        return;
    }
    const bool req = installOne(base, kRequestRva, kRequestBytes, sizeof(kRequestBytes), reinterpret_cast<void*>(&requestReplacement),
                                reinterpret_cast<void**>(&g_requestOrig), &g_requestHook, "0x7E9D50 (mode request)");
    const bool copy = installOne(base, kCopyRva, kCopyBytes, sizeof(kCopyBytes), reinterpret_cast<void*>(&copyReplacement),
                                 reinterpret_cast<void**>(&g_copyOrig), &g_copyHook, "0x281C0D0 (settings copy)");
    Log::get().note("vr window: trampolines: 0x7E9D50 (mode request, window rewrite) %s; 0x281C0D0 (settings copy, hold) %s",
                    req ? "installed" : "not installed", copy ? "installed" : "not installed");
}

}  // namespace edvr
