// The sun-shadow probe (sun_shadow_probe.h). Reads the binding shadow only; writes nothing to the game.
#include "sun_shadow_probe.h"

#include <atomic>
#include <mutex>

#include "../common/log.h"
#include "binding_shadow.h"
#include "sun_shadow_probe_core.h"

namespace edvr {
namespace {

namespace sp = sunprobe;

std::mutex g_mutex;
sp::Counts g_counts;
void* g_resources[8] = {};
size_t g_nResources = 0;
std::atomic<uint32_t> g_frame{1};

// A view's resolution, kept for the frame (the binding shadow's generation moves every frame boundary, and a view address is trusted for no longer).
struct ViewEntry {
    void* view = nullptr;
    uint32_t frame = 0;
    void* resource = nullptr;
    bool cascade = false;
    bool known = false;
};
thread_local ViewEntry t_views[64];

const ViewEntry& resolve(void* view) noexcept {
    ViewEntry& e = t_views[(reinterpret_cast<uintptr_t>(view) >> 4) & 63];
    const uint32_t frame = g_frame.load(std::memory_order_relaxed);
    if (e.view == view && e.frame == frame) return e;
    e.view = view;
    e.frame = frame;
    e.known = false;
    e.cascade = false;
    e.resource = nullptr;
    ResourceInfo info;
    if (view != nullptr && bindingResolve(view, &info)) {
        e.known = true;
        e.resource = info.resource;
        e.cascade = sp::isCascadeMap(info.isTexture2D, info.a, info.b, info.fmt);
    }
    return e;
}

// Notes a resource as the map; true when it is new in the window.
bool noteResourceLocked(void* resource) noexcept {
    for (size_t i = 0; i < g_nResources; ++i) {
        if (g_resources[i] == resource) return false;
    }
    if (g_nResources < sizeof(g_resources) / sizeof(g_resources[0])) g_resources[g_nResources++] = resource;
    return true;
}

}  // namespace

void sunShadowProbeDraw(uint64_t vs, uint64_t ps) noexcept {
    bool into = false;
    void* intoResource = nullptr;
    void* const dsv = bindingGet(BindSlot::Dsv0);
    if (dsv != nullptr && bindingGet(BindSlot::Rtv0) == nullptr) {
        const ViewEntry& e = resolve(dsv);
        if (e.known && e.cascade) {
            into = true;
            intoResource = e.resource;
        }
    }
    bool sampler = false;
    void* samplerResource = nullptr;
    for (uint32_t k = 0; k < 4; ++k) {
        void* const srv = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::PsSrv0) + k));
        if (srv == nullptr) continue;
        const ViewEntry& e = resolve(srv);
        if (e.known && e.cascade) {
            sampler = true;
            samplerResource = e.resource;
            break;
        }
    }
    if (!into && !sampler) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (into) {
        g_counts.noteIntoCascade();
        g_counts.noteResource(noteResourceLocked(intoResource));
    }
    if (sampler) {
        g_counts.noteSampler(vs, ps);
        g_counts.noteResource(noteResourceLocked(samplerResource));
    }
}

void sunShadowProbeDispatch(uint64_t cs) noexcept {
    for (uint32_t k = 0; k < 4; ++k) {
        void* const uav = bindingGet(static_cast<BindSlot>(static_cast<uint32_t>(BindSlot::CsUav0) + k));
        if (uav == nullptr) continue;
        const ViewEntry& e = resolve(uav);
        if (e.known && e.cascade) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_counts.noteDispatchBound(cs);
            g_counts.noteResource(noteResourceLocked(e.resource));
            return;
        }
    }
}

void sunShadowProbeFrame(bool sunSkipped) noexcept {
    g_frame.fetch_add(1, std::memory_order_relaxed);
    if (!sunSkipped) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counts.noteSkippedFrame();
}

void sunShadowProbeReport(uint64_t seconds) noexcept {
    char line[1600] = {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_counts.skippedFrames() == 0) {
            g_counts.clear();
            g_nResources = 0;
            return;
        }
        g_counts.line(line, sizeof(line), seconds);
        g_counts.clear();
        g_nResources = 0;
    }
    Log::get().note("%s", line);
}

}  // namespace edvr
