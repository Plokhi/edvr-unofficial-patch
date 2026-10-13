// The fragment-culling cluster builder's probe (cluster_probe_hook.h). A CodeHook on the function's ENTRY (EliteDangerous64.exe+0x58B150, build 332841),
// identity-gated and pinned by its first sixteen bytes, the way the admission store's detour is. The relay is the store hook's: a gate cell read
// straight from the relay, so a closed gate runs the original with nothing in between.
#include "cluster_probe_hook.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../common/code_hook.h"
#include "../common/log.h"
#include "cluster_probe_core.h"
#include "explorer_cam_core.h"
#include "optimized_station_core.h"

namespace edvr {
namespace {

namespace cp = clusterprobe;

constexpr size_t kRelayBytes = 44, kOriginalLiteral = 36;
constexpr uint64_t kMaxClusterHashBytes = 512u * 1024u;   // the cluster table's hash covers at most this many bytes
constexpr uint64_t kMaxSliceHashBytes = 64u * 1024u;      // and the depth-slice table's
constexpr uint64_t kMaxTableBytes = 256ull * 1024u * 1024u;

// rcx the cluster context, rdx/r8/r9 pointers, then the target's width and height (32-bit stack arguments; the callee reads 32 bits).
using ClusterFn = uintptr_t(__fastcall*)(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height);

__declspec(noinline) uintptr_t __fastcall clusterObserved(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height) noexcept;

alignas(8) std::atomic<uintptr_t> g_gate{0};
static_assert(decltype(g_gate)::is_always_lock_free, "The relay reads the aligned atomic cell directly.");

struct Detour {
    std::atomic<uintptr_t> forward{0};
    CodeHook hook;
    uint8_t* relay = nullptr;
    std::atomic<bool> ready{false};
    std::atomic<bool> refused{false};
    std::atomic<const char*> status{"not wanted yet"};
};
Detour g_detour;

std::mutex g_statsMutex;
cp::Stats g_stats;
uint8_t g_prevKey[cp::kKeyBytes] = {};
bool g_havePrevKey = false;

int64_t ticks() noexcept {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
double msBetween(int64_t a, int64_t b) noexcept {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(b - a) * 1000.0 / freq;
}

uint8_t* allocateRelay(uintptr_t target) noexcept {
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
                auto* p = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(candidate), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                if (p) return p;
            }
        }
        if (end <= at) break;
        at = end;
    }
    return nullptr;
}

// mov rax,&gate; cmp qword ptr[rax],0; je original; jmp [callback]; original: jmp [trampoline].
void buildRelay(uint8_t* code, const void* gate, void* callback) noexcept {
    const uint8_t body[kRelayBytes] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x83, 0x38, 0, 0x74, 0x0E, 0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                       0xFF, 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    std::memcpy(code, body, sizeof(body));
    const uintptr_t gateAddress = reinterpret_cast<uintptr_t>(gate);
    const uintptr_t callbackAddress = reinterpret_cast<uintptr_t>(callback);
    std::memcpy(code + 2, &gateAddress, 8);
    std::memcpy(code + 22, &callbackAddress, 8);
}

bool prepareRelay(void* trampoline, void* context) noexcept {
    auto& d = *static_cast<Detour*>(context);
    const uintptr_t address = reinterpret_cast<uintptr_t>(trampoline);
    std::memcpy(d.relay + kOriginalLiteral, &address, 8);
    DWORD oldProtect = 0;
    if (!VirtualProtect(d.relay, 4096, PAGE_EXECUTE_READ, &oldProtect) || !FlushInstructionCache(GetCurrentProcess(), d.relay, kRelayBytes)) return false;
    d.forward.store(address, std::memory_order_release);
    return true;
}

bool readIdentity(uintptr_t base, uint32_t* stamp, uint32_t* size) noexcept {
    __try {
        uint32_t peOff = 0;
        std::memcpy(&peOff, reinterpret_cast<const void*>(base + 0x3C), 4);
        if (peOff > 0x1000) return false;
        std::memcpy(stamp, reinterpret_cast<const void*>(base + peOff + 8), 4);
        std::memcpy(size, reinterpret_cast<const void*>(base + peOff + 0x50), 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The key block of a cluster context (+0x00..+0x77), or false when it cannot be read.
bool readKey(const void* self, uint8_t* out) noexcept {
    __try {
        std::memcpy(out, self, cp::kKeyBytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One table: a vector of 32-byte elements (size at +0 of the pair, data at +8 of it). Hashes up to maxBytes; adds to *total and *hashed.
bool hashTable(const uint8_t* self, size_t sizeOff, size_t dataOff, uint64_t maxBytes, uint64_t* h, uint64_t* total, uint64_t* hashed) noexcept {
    __try {
        uint64_t count = 0;
        uintptr_t data = 0;
        std::memcpy(&count, self + sizeOff, 8);
        std::memcpy(&data, self + dataOff, 8);
        if (data == 0 || count == 0) return true;
        const uint64_t bytes = count * 32u;
        if (count > kMaxTableBytes / 32u) return false;
        const uint64_t n = bytes < maxBytes ? bytes : maxBytes;
        *h = cp::hashBytes(reinterpret_cast<const void*>(data), static_cast<size_t>(n)) ^ (*h * 0x9E3779B97F4A7C15ull);
        *total += bytes;
        *hashed += n;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) uintptr_t __fastcall clusterObserved(void* self, void* a2, void* a3, void* a4, uint32_t width, uint32_t height) noexcept {
    const auto forward = reinterpret_cast<ClusterFn>(g_detour.forward.load(std::memory_order_acquire));
    if (!forward) return 0;
    uint8_t before[cp::kKeyBytes];
    const int64_t t0 = ticks();
    const bool okBefore = self != nullptr && readKey(self, before);
    const int64_t t1 = ticks();
    const uintptr_t result = forward(self, a2, a3, a4, width, height);
    const int64_t t2 = ticks();
    if (!okBefore) return result;
    uint8_t after[cp::kKeyBytes];
    if (!readKey(self, after)) return result;
    cp::CallRecord rec;
    rec.keyBefore = cp::hashBytes(before, cp::kKeyBytes);
    rec.keyAfter = cp::hashBytes(after, cp::kKeyBytes);
    uint64_t h = 0, total = 0, hashed = 0;
    const uint8_t* base = static_cast<const uint8_t*>(self);
    const bool okA = hashTable(base, 0x90, 0x98, kMaxClusterHashBytes, &h, &total, &hashed);
    const bool okB = hashTable(base, 0xB0, 0xB8, kMaxSliceHashBytes, &h, &total, &hashed);
    rec.out = h;
    rec.outHashed = okA && okB && hashed > 0;
    rec.tableBytes = total;
    rec.hashedBytes = hashed;
    rec.callMs = msBetween(t1, t2);
    const int64_t t3 = ticks();
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        rec.havePrev = g_havePrevKey;
        if (g_havePrevKey) rec.vsPrev = cp::diffKeys(g_prevKey, after);
        std::memcpy(g_prevKey, after, sizeof(g_prevKey));
        g_havePrevKey = true;
        rec.probeMs = msBetween(t0, t1) + msBetween(t2, t3);
        g_stats.record(rec);
    }
    return result;
}

void installOnce() noexcept {
    if (g_detour.ready.load(std::memory_order_acquire) || g_detour.refused.load(std::memory_order_acquire)) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        g_detour.status.store("the module base is unknown", std::memory_order_release);
        return;
    }
    uint32_t stamp = 0;
    uint32_t size = 0;
    const bool identityRead = readIdentity(base, &stamp, &size);
    uint8_t got[sizeof(cp::kClusterPrologue)] = {};
    bool readOk = false;
    __try {
        std::memcpy(got, reinterpret_cast<const void*>(base + cp::kClusterRva), sizeof(got));
        readOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        readOk = false;
    }
    const sealed::StoreInstall verdict = sealed::storeInstallDecision(identityRead, stamp, size, ecm::kExpectedTimestamp, ecm::kExpectedImageSize,
                                                                     readOk ? got : nullptr, cp::kClusterPrologue, sizeof(got));
    if (verdict != sealed::StoreInstall::kInstall) {
        g_detour.refused.store(true, std::memory_order_release);
        if (verdict == sealed::StoreInstall::kWrongIdentity) {
            g_detour.status.store("not build 332841 (PE timestamp or image size)", std::memory_order_release);
            Log::get().note("cluster probe: refused: not build 332841 (PE timestamp %u, image size %u); nothing patched.", static_cast<unsigned>(stamp),
                            static_cast<unsigned>(size));
        } else if (verdict == sealed::StoreInstall::kUnreadable) {
            g_detour.status.store("the executable's PE header or the builder's prologue is unreadable", std::memory_order_release);
            Log::get().note("cluster probe: refused: the PE header or the cluster builder's prologue is unreadable; nothing patched.");
        } else {
            g_detour.status.store("the builder's prologue does not match build 332841", std::memory_order_release);
            Log::get().note("cluster probe: refused: the cluster builder's sixteen prologue bytes are not build 332841's; nothing patched.");
        }
        return;
    }
    g_detour.relay = allocateRelay(base + cp::kClusterRva);
    if (!g_detour.relay) {
        g_detour.status.store("no relay within range", std::memory_order_release);
        return;
    }
    buildRelay(g_detour.relay, &g_gate, reinterpret_cast<void*>(&clusterObserved));
    if (!g_detour.hook.install(reinterpret_cast<void*>(base + cp::kClusterRva), g_detour.relay, nullptr, "cluster-builder", &prepareRelay, &g_detour)) {
        VirtualFree(g_detour.relay, 0, MEM_RELEASE);
        g_detour.relay = nullptr;
        g_detour.status.store("CodeHook refused the builder (its own line says why)", std::memory_order_release);
        return;
    }
    g_detour.ready.store(true, std::memory_order_release);
    g_detour.status.store("installed", std::memory_order_release);
    Log::get().note("cluster probe: the fragment-culling cluster builder (0x58B150) is detoured, log only; the gate follows fix.optimized_station.");
}

}  // namespace

void clusterProbeSetOn(bool on) noexcept {
    if (on) installOnce();
    g_gate.store(on && g_detour.ready.load(std::memory_order_acquire) ? 1u : 0u, std::memory_order_release);
}

void clusterProbeFrame() noexcept {
    if (g_gate.load(std::memory_order_acquire) == 0) return;
    std::lock_guard<std::mutex> lock(g_statsMutex);
    g_stats.endFrame();
}

void clusterProbeReport(uint64_t seconds) noexcept {
    if (!g_detour.ready.load(std::memory_order_acquire)) return;
    char line[1400] = {};
    {
        std::lock_guard<std::mutex> lock(g_statsMutex);
        if (g_stats.calls() == 0) {
            std::snprintf(line, sizeof(line),
                          "optimized station: cluster probe last %llu s: no calls (the detour is installed and the gate is %s; the cluster builder did not run).",
                          static_cast<unsigned long long>(seconds), g_gate.load(std::memory_order_acquire) != 0 ? "open" : "closed");
        } else {
            g_stats.line(line, sizeof(line), seconds);
        }
        g_stats.clear();
    }
    Log::get().note("%s", line);
}

const char* clusterProbeStatus() noexcept { return g_detour.status.load(std::memory_order_acquire); }

}  // namespace edvr
