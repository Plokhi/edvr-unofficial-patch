// The sun-shadow skip (sun_shadow_skip.h). Reads the binding shadow and the context's shader-resource bindings; writes nothing to the game.
#include "sun_shadow_skip.h"

#include <d3d11.h>

#include <atomic>
#include <mutex>

#include "../common/log.h"
#include "binding_shadow.h"
#include "sun_shadow_skip_core.h"

namespace edvr {
namespace {

namespace ss = sunskip;

constexpr uint32_t kSlotsScanned = 8;   // shader-resource slots 0..7 of every stage

std::mutex g_mutex;                                   // the gate's rare events: the frame, a consumer, a scan count, the report
ss::SkipGate g_gate;
std::atomic<uint8_t> g_phase{static_cast<uint8_t>(ss::Phase::kIdle)};   // the draw path's fast read of the gate's phase
std::atomic<uint32_t> g_frameNo{0};
std::atomic<uint32_t> g_drawOrdinal{0};
std::atomic<uint32_t> g_dispatchOrdinal{0};
std::atomic<uint64_t> g_skipped{0}, g_into{0};

// A view's resolution, kept for the frame (the binding shadow's generation moves every frame boundary, and a view address is trusted for no longer).
struct ViewEntry {
    void* view = nullptr;
    uint32_t frame = 0;
    bool cascade = false;
    bool known = false;
};
thread_local ViewEntry t_views[64];

bool isCascadeView(void* view) noexcept {
    if (view == nullptr) return false;
    ViewEntry& e = t_views[(reinterpret_cast<uintptr_t>(view) >> 4) & 63];
    const uint32_t frame = g_frameNo.load(std::memory_order_relaxed);
    if (e.view == view && e.frame == frame) return e.known && e.cascade;
    e.view = view;
    e.frame = frame;
    e.known = false;
    e.cascade = false;
    ResourceInfo info;
    if (bindingResolve(view, &info)) {
        e.known = true;
        e.cascade = ss::isCascadeMap(info.isTexture2D, info.a, info.b, info.fmt);
    }
    return e.known && e.cascade;
}

void publishPhase() noexcept { g_phase.store(static_cast<uint8_t>(g_gate.phase()), std::memory_order_release); }

// A consumer was found: stands the skip down and says why, once per window.
void consumerFound(const ss::Consumer& c) noexcept {
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        first = g_gate.consumer(c);
        publishPhase();
    }
    if (first) {
        Log::get().note("optimized station: sun shadow map: stood down for this sun-skip window: a %s binds the map in slot %u (VS %016llX PS %016llX CS "
                        "%016llX); its draws run as before until the sun skip ends.",
                        ss::stageName(c.stage), static_cast<unsigned>(c.slot), static_cast<unsigned long long>(c.vs), static_cast<unsigned long long>(c.ps),
                        static_cast<unsigned long long>(c.cs));
    }
}

// Enumerates the shader-resource slots 0..7 of every stage through the context's own Get calls; true (and *out filled) when one holds the map.
bool scanStages(ID3D11DeviceContext* c, ss::Consumer* out) noexcept {
    static const ss::Stage kStages[] = {ss::Stage::kVs, ss::Stage::kGs, ss::Stage::kHs, ss::Stage::kDs, ss::Stage::kPs, ss::Stage::kCs};
    bool found = false;
    for (const ss::Stage stage : kStages) {
        ID3D11ShaderResourceView* views[kSlotsScanned] = {};
        switch (stage) {
            case ss::Stage::kVs: c->VSGetShaderResources(0, kSlotsScanned, views); break;
            case ss::Stage::kGs: c->GSGetShaderResources(0, kSlotsScanned, views); break;
            case ss::Stage::kHs: c->HSGetShaderResources(0, kSlotsScanned, views); break;
            case ss::Stage::kDs: c->DSGetShaderResources(0, kSlotsScanned, views); break;
            case ss::Stage::kPs: c->PSGetShaderResources(0, kSlotsScanned, views); break;
            default: c->CSGetShaderResources(0, kSlotsScanned, views); break;
        }
        for (uint32_t k = 0; k < kSlotsScanned; ++k) {
            if (views[k] == nullptr) continue;
            if (!found && isCascadeView(views[k])) {
                found = true;
                out->stage = stage;
                out->slot = k;
            }
            views[k]->Release();
        }
        if (found) break;
    }
    return found;
}

}  // namespace

bool sunShadowSkipDraw(void* ctx, uint64_t vs, uint64_t ps) noexcept {
    const auto phase = static_cast<ss::Phase>(g_phase.load(std::memory_order_acquire));
    if (phase == ss::Phase::kIdle || phase == ss::Phase::kStoodDown) return false;
    // While the window is scanned, the pixel-shader slots 0..3 (the binding shadow keeps them) are checked on every draw. Once it is skipping, resolving
    // every draw's views would cost more than the skip saves: the sparse scan below watches all stages instead.
    for (uint32_t k = 0; k < (phase == ss::Phase::kScanning ? 4u : 0u); ++k) {
        void* const srv = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + k));
        if (srv != nullptr && isCascadeView(srv)) {
            ss::Consumer c;
            c.stage = ss::Stage::kPs;
            c.slot = k;
            c.vs = vs;
            c.ps = ps;
            consumerFound(c);
            return false;
        }
    }
    void* const dsv = bindingGet(BindSlot::Dsv0);
    const bool into = dsv != nullptr && bindingGet(BindSlot::Rtv0) == nullptr && isCascadeView(dsv);
    if (into) g_into.fetch_add(1, std::memory_order_relaxed);
    // The sampled scan of every stage (the first frames of the window, then a sparse watch).
    if (phase == ss::Phase::kScanning || phase == ss::Phase::kSkipping) {
        const uint32_t frameNo = g_frameNo.load(std::memory_order_relaxed);
        if (ss::takeScan(g_drawOrdinal.fetch_add(1, std::memory_order_relaxed), frameNo, ss::drawStride(phase))) {
            { std::lock_guard<std::mutex> lock(g_mutex); g_gate.noteScannedDraw(); }
            ss::Consumer c;
            if (ctx != nullptr && scanStages(static_cast<ID3D11DeviceContext*>(ctx), &c)) {
                c.vs = vs;
                c.ps = ps;
                consumerFound(c);
                return false;
            }
        }
    }
    if (into && static_cast<ss::Phase>(g_phase.load(std::memory_order_acquire)) == ss::Phase::kSkipping) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void sunShadowSkipDispatch(void* ctx, uint64_t cs) noexcept {
    const auto phase = static_cast<ss::Phase>(g_phase.load(std::memory_order_acquire));
    if (phase != ss::Phase::kScanning && phase != ss::Phase::kSkipping) return;
    if (!ss::takeScan(g_dispatchOrdinal.fetch_add(1, std::memory_order_relaxed), 0, ss::dispatchStride(phase))) return;
    { std::lock_guard<std::mutex> lock(g_mutex); g_gate.noteScannedDispatch(); }
    ss::Consumer c;
    c.cs = cs;
    for (uint32_t k = 0; k < 4; ++k) {
        void* const uav = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + k));
        if (uav != nullptr && isCascadeView(uav)) {
            c.stage = ss::Stage::kCsUav;
            c.slot = k;
            consumerFound(c);
            return;
        }
    }
    if (ctx != nullptr && scanStages(static_cast<ID3D11DeviceContext*>(ctx), &c)) {
        c.cs = cs;
        consumerFound(c);
    }
}

void sunShadowSkipFrame(bool sunSkipped) noexcept {
    bool nowSkipping = false;
    uint64_t scannedDraws = 0, scannedDispatches = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        nowSkipping = g_gate.frame(sunSkipped);
        g_frameNo.store(g_gate.frameNo(), std::memory_order_relaxed);
        publishPhase();
        scannedDraws = g_gate.windowDraws();
        scannedDispatches = g_gate.windowDispatches();
    }
    g_drawOrdinal.store(0, std::memory_order_relaxed);
    g_dispatchOrdinal.store(0, std::memory_order_relaxed);
    if (nowSkipping) {
        Log::get().note("optimized station: sun shadow map: no shader of any stage read it in %u frames of scanning (%llu draws and %llu dispatches "
                        "enumerated); its draws are skipped while the sun is.",
                        static_cast<unsigned>(ss::kScanFrames), static_cast<unsigned long long>(scannedDraws),
                        static_cast<unsigned long long>(scannedDispatches));
    }
}

void sunShadowSkipReport(uint64_t seconds) noexcept {
    char line[1400] = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_gate.sunFrames() == 0) {
            g_into.store(0, std::memory_order_relaxed);
            g_skipped.store(0, std::memory_order_relaxed);
            g_gate.clearReport();
            return;
        }
        // The draw path counts into and skipped with atomics; the gate's line wants them in its own counters.
        const uint64_t into = g_into.exchange(0, std::memory_order_relaxed);
        const uint64_t skipped = g_skipped.exchange(0, std::memory_order_relaxed);
        g_gate.addInto(into);
        g_gate.addSkipped(skipped);
        g_gate.line(line, sizeof(line), seconds);
        g_gate.clearReport();
    }
    Log::get().note("%s", line);
}

}  // namespace edvr
