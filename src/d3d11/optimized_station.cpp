// The optimized station (fix.optimized_station): inside a station, skip rendering what cannot be seen. While the ship sits in a sealed hangar
// (docked, the pad lowered and still, the hatch closed), everything outside the hangar is skipped, including the sun's light that leaks into it.
// The rules and their geometry are in optimized_station_core.h. The seal, the release and the status lines are here. The engine hooks it uses:
// store_hook.cpp (the store's record judgement, path A), kinematic_eval_hook.cpp (the producer-B and rig vetoes), vscreen.cpp (the sun pair).
#include "optimized_station.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../common/config.h"
#include "../common/log.h"
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "optimized_station_core.h"
#include "pad_state_probe.h"
#include "store_hook.h"
#include "view_source.h"

namespace edvr {
namespace {

// ---- The frame's state (the store's observations, the own pad, the held cell) --------------------------------------------------------------
std::recursive_mutex g_sealedMutex;        // the lock: the view, the cell, the pads and explicit parts, the rig tables (never held across the game)
std::atomic<uint32_t> g_frameStamp{0};     // advanced at every frame tick: the observer resolves the view once per frame
std::atomic<bool> g_cameraKnown{false};
std::atomic<uint64_t> g_kept{0}, g_keptUnknown{0}, g_culled{0}, g_faults{0};
uint64_t g_worldSrc[5] = {};               // the named rule's world centre by source (worldOfRecord)
float g_margin = sealed::kMarginDefaultM;  // the box's margin (fixed; the trial's tuning ladder is removed)
// The sealed rule's sphere radius while no cell is held (the trial's starting radius; the named cell does not use it).
constexpr float kFallbackSphereM = 80.0f;
uint64_t g_winFaults = 0;                  // reads or writes that failed in the window
// Forward: the held cell is localized and the shell picked by setHeldFrame (below), called from the engage.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame);
struct RigTally {
    uint64_t seen = 0, vetoed = 0, mixed = 0, unknown = 0, refresh = 0;
    uint64_t assocSync = 0, assocJob = 0, assocNone = 0, matched = 0, unevaluated = 0;
};
RigTally g_rigTally;
sealed::OwnCell g_heldCell;                // the sealed cell, in the world frame of the frame that reads it (refreshed each frame)
bool g_heldValid = false;                  // sealed
std::atomic<bool> g_sealedFlag{false};     // mirror of g_heldValid for the draw path and the hooks (atomic: read on the render thread)
bool g_engagePending = false;
uintptr_t g_engagePad = 0;     // built v14.20 (review 1): the pad a pending engage was requested for
uintptr_t g_engageShell = 0;   // built v14.20 (review 1): the shell it was requested against (0: none)
uint32_t g_engageTries = 0;
// Built v14.5: the pad's height above the hangar shell, read live each tick (frame-independent). The shell is picked once per pad.
bool g_relOk = false;
sealed::ShipWatch g_shipWatch;   // the own ship's type prefix, read every tick: a change is a ship change (built v14.6)
float g_relM = 0.0f;
bool g_heightChanged = false;
sealed::ShellHeightSources g_heightSrc;
bool g_preShellValid = false;
uintptr_t g_preShellAddr = 0;
uintptr_t g_preShellPad = 0;
char g_preShellName[48] = {};
char g_ownPrefix[32] = {};
char g_ownCandidateName[48] = {};
// Built v14.8: the records within kShipRecordsM of the camera by type prefix, this frame (the ship's part count).
struct PrefixTally {
    char p[32] = {};
    uint32_t n = 0;
};
constexpr size_t kPrefixSlots = 24;
PrefixTally g_prefixTally[kPrefixSlots];
size_t g_prefixTallyN = 0;
float g_ownCandidateDist = -1.0f;
sealed::PadObs g_pads[sealed::kPadSlots];
sealed::OwnPadSticky g_ownSticky;   // Built v14 (fix 1): the own pad, kept while its record is seen (reset when it is replaced)
size_t g_padCount = 0;
sealed::ExplicitObs g_explicits[sealed::kExplicitSlots];
size_t g_explicitCount = 0;
float g_frameCam[3] = {};
bool g_frameCamValid = false;
uint32_t g_recordsFrame = 0, g_recordsLast = 0;   // store records the observer took this frame, and last frame (the waiting line's evidence)
sealed::OwnCell g_pickCell;                // the frame's pick before the seal (what the seal rule reads)
sealed::OwnCell g_cellPrev;                // the cell the observer judges against this frame
uintptr_t g_explicitPrev[sealed::kExplicitNodes] = {};
size_t g_explicitPrevCount = 0;
uint32_t g_cellStamp = 0;
uint32_t g_heldAbsent = 0, g_otherPicked = 0;
uint32_t g_viewStamp = 0;
bool g_viewReady = false;
uint32_t g_viewTries = 0;
view::Resolved g_view;
uintptr_t g_lastCtx = 0;
// The pad-local frame and the shell (the release by the pad's motion along the shaft).
sealed::OwnCell g_heldLocal;
bool g_heldLocalValid = false;
float g_engageCentre[3] = {};
float g_engageAxes[3][3] = {};
uintptr_t g_shellAddr = 0;
char g_shellName[48] = {};
bool g_shellValid = false;
sealed::ShellMotion g_shellMotion;        // the pad against its shell: the reference is captured live at the first frame held (built v14.4)
uint32_t g_shellSampleStamp = 0xFFFFFFFFu;// the frame stamp of the last motion sample (the tick and the observer both refresh; one sample a frame)
sealed::SealRateGuard g_sealGuard;        // the flicker safety net: seals in a minute (built v14.4)
float g_frameShiftM = 0.0f;
float g_shellDeltaM = 0.0f;
bool g_releaseMotion = false;
// The own record (the own ship's live sphere) and the near list the engage's own-record pick reads.
struct NearRec {
    uintptr_t addr = 0;
    char name[48] = {};
    float centre[3] = {};
    float radius = 0.0f;
    float dist = 0.0f;
};
constexpr float kOwnNearM = 40.0f;
constexpr float kOwnSphereMarginM = 5.0f;
constexpr size_t kNearSlots = 24;
NearRec g_nearCur[kNearSlots], g_nearPrev[kNearSlots];
size_t g_nearCurN = 0, g_nearPrevN = 0;
uintptr_t g_ownRecAddr = 0;
char g_ownRecName[48] = {};
bool g_ownLiveValid = false;
float g_ownLiveCentre[3] = {};
float g_ownLiveRadius = 0.0f;


// The below distance for the held pad's size (the constants in the core; diag keys are not in the product).
float belowFor(sealed::PadSize size) noexcept {
    return sealed::belowForSize(size, sealed::kBelowDefaultsM, false, 0.0f);
}

bool cameraOf(const view::Resolved& v, float out[3]) noexcept { return view::sphereCentre(v, out); }

// The render context's layout (the same offsets as shadow_gate_core.h): views at +0x40, 0x6A0 bytes each; the view count at +0x1A940.
constexpr uintptr_t kCtxCount = 0x1A940;

constexpr uintptr_t kRecCentre = 0x240;     // float3: the world sphere centre

constexpr uintptr_t kRecMask = 0x208;       // u64: the view mask the store wrote

constexpr uintptr_t kRecAdmitted = 0x234;   // u8: the admitted flag the store wrote

constexpr uintptr_t kRecRadius = 0x280;     // float: the sphere radius

constexpr uintptr_t kRecChildren = 0x2A8;   // pointer: the child list, an array of 8-byte record pointers

constexpr uintptr_t kRecChildCount = 0x2B0; // u64: the child count

constexpr uintptr_t kBucketBase = 0x28;     // the bucket's base record

constexpr uintptr_t kRecName = 0x18;        // pointer: the record's node (node+0 is the text pointer)

constexpr size_t kClassSlots = 4096;         // the class cache: one slot per node (lock-free, valid for one frame)

constexpr size_t kNameSlots = 512;           // the name cache for the builder probe and the report (under the lock)

constexpr uint32_t kViewTries = 64;

// Built v8.4: the centre measurements of the window (under the lock). rec+0x240 exactly zero; the pose unreadable (the fallback); and the distance
// between rec+0x240 and the pose centre for the records where both are readable and rec+0x240 is not zero (reserved at a toggle-on; a write past
// the reserve is not kept, so the hot path never allocates).
constexpr size_t kCentreDistCap = 8192;

uint64_t g_prodCalls = 0, g_prodVetoed = 0, g_prodKept = 0, g_prodStyle = 0, g_prodUnread = 0;


// The class cache (lock-free): a slot per node, a word per slot (sealed_trial_core.h packs it). Written by any thread that runs the store.
std::atomic<uint64_t> g_classSlots[kClassSlots] = {};

bool readMem(void* dst, uintptr_t src, size_t n) noexcept {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool writeMem(uintptr_t dst, const void* src, size_t n) noexcept {
    if (!dst) return false;
    __try {
        std::memcpy(reinterpret_cast<void*>(dst), src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <typename T>
bool readVal(T* dst, uintptr_t src) noexcept { return readMem(dst, src, sizeof(T)); }

// The name of a record, copied out under the lock (no pointer into the cache escapes: the slot may be reused as soon as the lock is let go).
// False when no name can be read. Used by the builder probe, the geometry probe and the report.
bool cachedName(uintptr_t record, char* out, size_t cap) noexcept {
    if (!out || cap == 0) return false;
    out[0] = '\0';
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    struct NameSlot {
        uintptr_t node = 0;
        uint32_t stamp = 0;
        bool ok = false;
        char name[64] = {};
    };
    static NameSlot names[kNameSlots];
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    NameSlot& slot = names[(node >> 4) % kNameSlots];
    if (slot.node != node || slot.stamp != stamp) {
        slot.node = node;
        slot.stamp = stamp;
        slot.ok = false;
        slot.name[0] = '\0';
        uintptr_t text = 0;
        uint8_t bytes[sealed::kNameBytes] = {};
        std::string name;
        if (readVal(&text, node) && text && readMem(bytes, text, sizeof(bytes)) &&
            (sealed::textName(bytes, sizeof(bytes), false, &name) || sealed::textName(bytes, sizeof(bytes), true, &name))) {
            std::snprintf(slot.name, sizeof(slot.name), "%s", name.c_str());
            slot.ok = true;
        }
    }
    if (!slot.ok) return false;
    std::snprintf(out, cap, "%s", slot.name);
    return true;
}

// --- Built v8.4: a record's centre. The builder's centre is computed from the pose it reads, *(rec+0x290): q = pose+0x10, t = pose+0x20, c =
// pose+0x80 (sealed_trial_core.h builderCentre; decomp_42B4420.txt:196-237). rec+0x240 is the fallback when the pose cannot be read (counted).
constexpr uintptr_t kRecPose = 0x290;     // pointer: the builder's rcx, the pose context

constexpr uintptr_t kPoseQuat = 0x10;     // float4: the quaternion x, y, z, w

constexpr uintptr_t kPoseT = 0x20;        // float4: the translation

constexpr uintptr_t kPoseCentre = 0x80;   // float4: the local centre c

struct CentreRead {
    sealed::CentreSource source = sealed::CentreSource::kNone;
    float world[3] = {};      // the centre the rule uses
    float pose[3] = {};       // the builder's centre (valid when poseOk)
    bool poseOk = false;
    float raw[3] = {};        // rec+0x240 (valid when rawOk)
    bool rawOk = false;
    bool rawZero = false;     // rec+0x240 is exactly the origin
    float dist = -1.0f;       // |raw - pose| when both are readable and raw is not zero; else -1
};

// The builder's centre from the pose (SEH-guarded reads only). False when the pose pointer or any of its fields cannot be read.
bool poseCentreOf(uintptr_t rec, float out[3]) noexcept {
    uintptr_t pose = 0;
    if (!readVal(&pose, rec + kRecPose) || !pose) return false;
    float q[4] = {}, t[4] = {}, c[4] = {}, w[4] = {};
    if (!readMem(q, pose + kPoseQuat, sizeof(q)) || !readMem(t, pose + kPoseT, sizeof(t)) ||
        !readMem(c, pose + kPoseCentre, sizeof(c)))
        return false;
    sealed::builderCentre(q, t, c, w);
    out[0] = w[0];
    out[1] = w[1];
    out[2] = w[2];
    return sealed::finite3(out);
}

CentreRead recordCentreRead(uintptr_t rec) noexcept {
    CentreRead r;
    r.poseOk = poseCentreOf(rec, r.pose);
    r.rawOk = readMem(r.raw, rec + kRecCentre, sizeof(r.raw)) && sealed::finite3(r.raw);
    r.rawZero = r.rawOk && sealed::isZeroCentre(r.raw);
    r.source = sealed::chooseCentre(r.poseOk, r.pose, r.rawOk, r.raw, r.world);
    if (r.poseOk && r.rawOk && !r.rawZero) r.dist = sealed::distance(r.pose, r.raw);
    return r;
}

// The store-style world sphere (decomp_4312040.txt lines 212-262, sealed_trial_core.h storeWorldSphere). False when any read fails or the result
// is not finite. Reads only.
bool storeSphereOf(uintptr_t rec, float centre[3], float* radius) noexcept {
    constexpr uintptr_t kNodeOff = 0x18;       // rec+0x18: the node (census's name pointer)
    constexpr uintptr_t kBlockOff = 0x08;      // node+0x08: the bounds block
    constexpr uintptr_t kPoseOff = 0x190;      // rec+0x190: the pose block A
    constexpr uintptr_t kMatrixOff = 0x50;     // A+0x50: the matrix B
    float L[3] = {}, A[15] = {}, B[16] = {};
    float r = 0.0f;
    uintptr_t node = 0, block = 0, pose = 0, matrix = 0;
    bool ok = readVal(&node, rec + kNodeOff) && node && readVal(&block, node + kBlockOff) && block &&
              readMem(L, block + 0x20, sizeof(L)) && readVal(&r, block + 0x2C) && readVal(&pose, rec + kPoseOff) && pose &&
              readMem(A, pose, sizeof(A)) && readVal(&matrix, pose + kMatrixOff) && matrix && readMem(B, matrix, sizeof(B));
    ok = ok && std::isfinite(r) && r > 0.0f && sealed::storeWorldSphere(L, A, B, centre);
    if (ok) *radius = r;
    return ok;
}

// Built v11 (second addendum): a record's local axes in the world. The builder's quaternion (the pose at rec+0x290: q at +0x10, the extent at
// +0x90) when the record has one (path A); else the store-style pose (rec+0x190 and its +0x50 matrix; decomp_4312040.txt). src: 1 builder,
// 2 store-style, 4 neither (false). extOk: the builder extent was read (only the builder pose gives one).
bool poseOrientOf(uintptr_t rec, float axes[3][3], float ext[3], bool* extOk, uint8_t* src) noexcept {
    *extOk = false;
    *src = 4;
    uintptr_t pose = 0;
    float q[4] = {};
    if (readVal(&pose, rec + kRecPose) && pose && readMem(q, pose + 0x10, sizeof(q)) && std::isfinite(q[0]) && std::isfinite(q[1]) &&
        std::isfinite(q[2]) && std::isfinite(q[3])) {
        sealed::quatAxes(q, axes);
        *src = 1;
        if (readMem(ext, pose + 0x90, 3 * sizeof(float)) && std::isfinite(ext[0]) && std::isfinite(ext[1]) && std::isfinite(ext[2])) *extOk = true;
        return true;
    }
    uintptr_t pa = 0, matrix = 0;
    float A[15] = {}, B[16] = {};
    if (readVal(&pa, rec + 0x190) && pa && readMem(A, pa, sizeof(A)) && readVal(&matrix, pa + 0x50) && matrix && readMem(B, matrix, sizeof(B))) {
        sealed::storeAxes(A, B, axes);
        *src = 2;
        return true;
    }
    return false;
}

// The producer B cache: per record, the centre and radius B's veto decision used this frame. A generation per frame (the stamp moves at Present)
// makes a stale slot empty; under the lock.
constexpr size_t kProdSlots = 8192;

struct ProdSlot {
    uintptr_t rec = 0;
    uint32_t gen = 0;
    float c[3] = {};
    float r = 0.0f;
};

ProdSlot g_prodSlots[kProdSlots];

uint32_t g_prodGen = 1;          // the generation the slots are valid for (0 is never valid)

uint32_t g_prodStampSeen = 0;

// Built v14.20 (review 5): the occupancy and the longest probe of each table this window (the 30 s summary reports them and resets the maximum), and
// the lookups or inserts that reached the probe bound (a full table is a miss, never a walk of every slot).
struct TableStats {
    uint32_t used = 0;
    uint32_t maxProbes = 0;
    uint64_t full = 0;
};
TableStats g_prodStats;
TableStats g_collStats;
TableStats g_rigStats;

void noteProbe(TableStats& t, const sealed::ProbeResult& p) noexcept {
    if (p.probes > t.maxProbes) t.maxProbes = p.probes;
    if (p.exhausted) ++t.full;
}

void prodSync() noexcept {
    const uint32_t s = g_frameStamp.load(std::memory_order_relaxed);
    if (s != g_prodStampSeen) {
        g_prodStampSeen = s;
        ++g_prodGen;
        g_prodStats.used = 0;   // every slot is stale now
    }
}

size_t prodIndex(uintptr_t rec) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (kProdSlots - 1);
}

void prodPut(uintptr_t rec, const float c[3], float r) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        prodIndex(rec), kProdSlots, sealed::kTableProbeMax,
        [rec](size_t i) { const ProdSlot& s = g_prodSlots[i]; return s.gen == g_prodGen && s.rec == rec; },
        [](size_t i) { return g_prodSlots[i].gen != g_prodGen; });
    noteProbe(g_prodStats, p);
    if (p.exhausted) return;   // uncached: a later lookup misses it, as a record the producer never offered (the same centre rules apply)
    ProdSlot& s = g_prodSlots[p.index];
    if (p.empty) ++g_prodStats.used;
    s.rec = rec;
    s.gen = g_prodGen;
    s.c[0] = c[0];
    s.c[1] = c[1];
    s.c[2] = c[2];
    s.r = r;
}

bool prodGet(uintptr_t rec, float c[3], float* r) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        prodIndex(rec), kProdSlots, sealed::kTableProbeMax,
        [rec](size_t i) { const ProdSlot& s = g_prodSlots[i]; return s.gen == g_prodGen && s.rec == rec; },
        [](size_t i) { return g_prodSlots[i].gen != g_prodGen; });
    noteProbe(g_prodStats, p);
    if (!p.found) return false;
    const ProdSlot& s = g_prodSlots[p.index];
    c[0] = s.c[0];
    c[1] = s.c[1];
    c[2] = s.c[2];
    *r = s.r;
    return true;
}

struct WorldRead {
    float world[3] = {};
    float radius = 0.0f;
    uint8_t source = 4;   // 0 producer B, 1 pose, 2 store-style, 3 raw fallback, 4 none
};

// The named rule's world centre and radius for a record (see the block comment above). Called under the lock.
WorldRead worldOfRecord(uintptr_t rec) noexcept {
    WorldRead w;
    prodSync();
    if (prodGet(rec, w.world, &w.radius)) {
        w.source = 0;
        return w;
    }
    float p[3] = {};
    if (poseCentreOf(rec, p)) {
        float r = 0.0f;
        if (readVal(&r, rec + kRecRadius) && std::isfinite(r) && r > 0.0f) {
            for (int k = 0; k < 3; ++k) w.world[k] = p[k];
            w.radius = r;
            w.source = 1;
        }
        return w;
    }
    if (storeSphereOf(rec, w.world, &w.radius)) {
        w.source = 2;
        return w;
    }
    float raw[3] = {};
    float r = 0.0f;
    // Built v13.3 (c): a zero raw centre is unreadable, not the origin: no sphere (the record is kept as unknown in named mode).
    if (readMem(raw, rec + kRecCentre, sizeof(raw)) && readVal(&r, rec + kRecRadius) && sealed::rawCentreUsable(raw, r)) {
        for (int k = 0; k < 3; ++k) w.world[k] = raw[k];
        w.radius = r;
        w.source = 3;
    }
    return w;
}

// The class of a node's name, read from game memory with no lock (the census's accept rules). The miss path of the class cache.
sealed::NameClass classifyNode(uintptr_t node) noexcept {
    uintptr_t text = 0;
    uint8_t bytes[sealed::kNameBytes] = {};
    if (!readVal(&text, node) || !text || !readMem(bytes, text, sizeof(bytes))) return sealed::NameClass::kUnnamed;
    std::string name;
    const bool ok = sealed::textName(bytes, sizeof(bytes), false, &name) || sealed::textName(bytes, sizeof(bytes), true, &name);
    return sealed::classifyName(ok, name.c_str());
}

// The class of a record's name and its node. Lock-free: one guarded read of the node pointer and one atomic load on a hit; the name is read only
// on a miss (once per node per frame).
sealed::NameClass classOf(uintptr_t record, uintptr_t* nodeOut) noexcept {
    *nodeOut = 0;
    uintptr_t node = 0;
    if (!record || !readVal(&node, record + kRecName) || !node) return sealed::NameClass::kUnnamed;
    *nodeOut = node;
    if (!sealed::slotFits(node)) return classifyNode(node);
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    std::atomic<uint64_t>& slot = g_classSlots[(uint64_t(node) >> 3) & (kClassSlots - 1)];
    sealed::NameClass cls = sealed::NameClass::kNone;
    if (sealed::unpackSlot(slot.load(std::memory_order_acquire), node, stamp, &cls)) return cls;
    cls = classifyNode(node);
    slot.store(sealed::packSlot(node, cls, stamp), std::memory_order_release);
    return cls;
}

// Built v14.8: count one record of a type prefix (the records within kShipRecordsM of the camera).
void tallyShipPrefix(const char* name) noexcept {
    char pfx[32] = {};
    sealed::ownPrefixOf(name, pfx, sizeof(pfx));
    if (pfx[0] == '\0') return;
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) {
            ++g_prefixTally[i].n;
            return;
        }
    }
    if (g_prefixTallyN < kPrefixSlots) {
        std::snprintf(g_prefixTally[g_prefixTallyN].p, sizeof(g_prefixTally[g_prefixTallyN].p), "%s", pfx);
        g_prefixTally[g_prefixTallyN].n = 1;
        ++g_prefixTallyN;
    }
}

uint32_t prefixTallyOf(const char* pfx) noexcept {
    for (size_t i = 0; i < g_prefixTallyN; ++i) {
        if (std::strcmp(g_prefixTally[i].p, pfx) == 0) return g_prefixTally[i].n;
    }
    return 0;
}

// Built v13 (fix 1): at each named engage, the own ship\'s type prefix from the candidate record (the one nearest the camera that contains it), logged.
void setOwnPrefixAtEngage(uint32_t frame) {
    (void)frame;
    sealed::ownPrefixOf(g_ownCandidateName, g_ownPrefix, sizeof(g_ownPrefix));
}

// Promotes the previous frame's pads into the cell for the frame now starting, and the explicit instances for that cell. Called under the lock
// at the frame's first record. The pads and explicit parts are the previous frame's; the camera is the previous frame's too.
// Built v13.2 (b): re-reads the own record by address (its centre and radius move with the pad). A record whose name no longer matches is dropped:
// the address was freed or reused, and the rule stands down until the next engage.
void readOwnLive() noexcept {
    g_ownLiveValid = false;
    if (!g_ownRecAddr) return;
    const WorldRead wr = worldOfRecord(g_ownRecAddr);
    char nm[48] = {};
    if (wr.source == 4 || !cachedName(g_ownRecAddr, nm, sizeof(nm)) || !sealed::ownRecordMatch(nm, g_ownRecName)) {
        g_ownRecAddr = 0;
        return;
    }
    if (!std::isfinite(wr.radius) || !(wr.radius > 0.0f)) return;
    std::memcpy(g_ownLiveCentre, wr.world, sizeof(g_ownLiveCentre));
    g_ownLiveRadius = wr.radius;
    g_ownLiveValid = true;
}

// Built v13.2 (b): at a named engage the own record is the one named as the stripped type prefix in the previous frame's near list: containing the
// camera preferred, then the nearest. Logged either way.
void pickOwnRecord(uint32_t frame) {
    (void)frame;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_ownRecName[0] = '\0';
    char stripped[48] = {};
    if (!sealed::strippedPrefix(g_ownPrefix, stripped, sizeof(stripped))) {
        return;
    }
    int best = -1;
    bool bestIn = false;
    for (size_t i = 0; i < g_nearPrevN; ++i) {
        const NearRec& r = g_nearPrev[i];
        if (!sealed::ownRecordMatch(r.name, stripped)) continue;
        const bool in = r.dist <= r.radius;
        if (best < 0 || (in && !bestIn) || (in == bestIn && r.dist < g_nearPrev[best].dist)) {
            best = static_cast<int>(i);
            bestIn = in;
        }
    }
    if (best < 0) {
        return;
    }
    const NearRec& r = g_nearPrev[best];
    g_ownRecAddr = r.addr;
    std::snprintf(g_ownRecName, sizeof(g_ownRecName), "%s", r.name);
    readOwnLive();
}

// Built v13.7 (a, b): the held cell rebuilt in the held pad's CURRENT frame from its live pose (its centre by worldOfRecord, its axes by
// poseOrientOf), and the pad's height along the shaft against the shell. Called at the frame's promote (before the drop decisions) and at the frame
// tick (before the rig verdicts), so every judgement of a frame uses the cell as the frame's records are expressed. A pose that cannot be read
// this frame keeps the cell as it was. A shell that cannot be read stands the motion check down until the next engage.
void refreshHeldWorld() {
    if (!g_heldValid || !g_heldLocalValid) return;
    const WorldRead pw = worldOfRecord(g_heldCell.pad);
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (pw.source == 4 || !poseOrientOf(g_heldCell.pad, axes, ext, &extOk, &src)) return;
    sealed::OwnCell w = g_heldCell;
    sealed::worldizeCell(g_heldLocal, pw.world, axes, &w);
    g_heldCell = w;
    g_cellPrev = w;
    g_frameShiftM = sealed::distance(pw.world, g_engageCentre);
    if (!g_shellValid) return;
    // One sample a frame: the tick and the observer both refresh the held cell.
    const uint32_t shellStamp = g_frameStamp.load(std::memory_order_relaxed);
    if (g_shellSampleStamp == shellStamp) return;
    g_shellSampleStamp = shellStamp;
    char nm[48] = {};
    const WorldRead sw = worldOfRecord(g_shellAddr);
    if (sw.source == 4 || !cachedName(g_shellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_shellName) != 0) {
        // Built v14 (fix 2): the reference is not switched to another part; the pad's motion check stands down for this seal.
        g_shellValid = false;
        Log::get().note("optimized station: the reference shell %s is no longer at its record; the pad's motion check stands down (no other shell is taken)", g_shellName);
        return;
    }
    const float up[3] = {g_engageAxes[2][0], g_engageAxes[2][1], g_engageAxes[2][2]};
    const bool first = !g_shellMotion.captured;
    const bool release = g_shellMotion.sample(pw.world, pw.source, sw.world, sw.source, up);
    if (first && g_shellMotion.captured) {
        Log::get().note("optimized station: motion reference read live at the first frame held: the pad sits %.2f m along the shaft above the shell %s "
                        "(centre sources pad %u, shell %u).", static_cast<double>(g_shellMotion.ref), g_shellName,
                        static_cast<unsigned>(pw.source), static_cast<unsigned>(sw.source));
    }
    g_shellDeltaM = g_shellMotion.delta;
    if (release) g_releaseMotion = true;
}

// The record's view mask and admitted flag through the guarded reads and writes, for the fault-safe cull (sealed_trial_core.h).
struct RecordFields {
    uintptr_t rec;
    bool readMask(uint64_t* out) noexcept { return readVal(out, rec + kRecMask); }
    bool readAdmitted(uint8_t* out) noexcept { return readVal(out, rec + kRecAdmitted); }
    bool writeMask(uint64_t v) noexcept { return writeMem(rec + kRecMask, &v, sizeof(v)); }
    bool writeAdmitted(uint8_t v) noexcept { return writeMem(rec + kRecAdmitted, &v, sizeof(v)); }
};

constexpr size_t kRigSlots = 4096;

struct RigSlot {
    uintptr_t rig = 0;               // 0: an empty slot
    uint32_t epoch = 0;              // built v14.20 (review 4): the lifecycle epoch the slot was claimed in; an older one is free and forwards
    uint32_t evalStamp = 0;          // the frame stamp of the rig's last decision
    uint32_t curStamp = 0;           // the frame stamp the current box was accumulated in
    bool curValid = false;
    float curMn[3] = {}, curMx[3] = {};
    bool lastValid = false;          // the box of the last frame that had records (the one before curStamp)
    float lastMn[3] = {}, lastMx[3] = {};
    uint32_t vetoedRun = 0;          // vetoes since the rig was last let through
    bool named = false;
    char name[48] = {};              // the name of the rig's first record (for the own-ship prefix)
    uintptr_t coll = 0;              // the rig's collection (rig+0x348), as last read at a decision
    uint32_t collStamp = 0;          // Built v13.1: the frame stamp the collection was last read in (once per rig per frame)
    uint8_t verdict = 0;             // kVerdictUnknown / kVerdictMixed / kVerdictOutside, refreshed once per frame (refreshRigVerdicts)
};

constexpr uint8_t kVerdictUnknown = 0, kVerdictMixed = 1, kVerdictOutside = 2;

RigSlot g_rigSlots[kRigSlots];

// The collection table (Built v12.1): collection -> rig, for the job path's association. Open addressing, written at a decision when a rig's
// collection is read, read per bucket by the store's observer. Under the trial's lock.
constexpr size_t kCollSlots = 1024;

// Built v14.20 (review 4): every association carries the lifecycle epoch it was made in. An association from an earlier epoch is free: a lookup misses
// it and an insert reuses it. The epoch is bumped at undock, hangar change, ship change, setting toggle and an association change (bumpRigEpoch).
uint32_t g_rigEpoch = 1;
bool g_rigVerdictsFresh = false;   // the verdicts were refreshed for the held cell this frame (review 7a)
uint64_t g_rigEpochs = 0;          // the lifecycle bumps this window (the summary's count)

struct CollSlot {
    uintptr_t coll = 0;
    uintptr_t rig = 0;
    uint32_t epoch = 0;
};

CollSlot g_collSlots[kCollSlots];

size_t collIndex(uintptr_t coll) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(coll) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCollSlots - 1);
}

void collInsert(uintptr_t coll, uintptr_t rig) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    if (p.exhausted) return;   // not associated: the record forwards its rig's own rules, the same as a rig with no association
    CollSlot& s = g_collSlots[p.index];
    if (p.empty) ++g_collStats.used;
    s.coll = coll;
    s.rig = rig;
    s.epoch = g_rigEpoch;
}

uintptr_t collFind(uintptr_t coll) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        collIndex(coll), kCollSlots, sealed::kTableProbeMax,
        [coll](size_t i) { const CollSlot& s = g_collSlots[i]; return s.epoch == g_rigEpoch && s.coll == coll; },
        [](size_t i) { return g_collSlots[i].epoch != g_rigEpoch; });
    noteProbe(g_collStats, p);
    return p.found ? g_collSlots[p.index].rig : 0;
}

// The job path's one-entry cache per frame: the last store bucket and the rig it resolved to (a bucket's records come in a run).
uintptr_t g_rigBucketCache = 0, g_rigBucketRig = 0;

uint32_t g_rigBucketStamp = 0;

uint64_t g_rigNamesLogged = 0;

size_t rigIndex(uintptr_t rig) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rig) >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kRigSlots - 1);
}

// Built v14.20 (review 4): claims a slot for a rig in the current epoch: its bounds, its name and its verdict start empty, so the first evaluation
// forwards until a fresh bound exists.
RigSlot* rigClaim(RigSlot& s, uintptr_t rig) noexcept {
    s = RigSlot{};
    s.rig = rig;
    s.epoch = g_rigEpoch;
    ++g_rigStats.used;
    return &s;
}

// Built v14.20 (review 5): a bounded probe. A slot of an earlier epoch is free; a full bound returns null (the rig forwards: its records are not
// bounded, the same as a rig the table never saw).
RigSlot* rigFind(uintptr_t rig, bool insert) noexcept {
    const sealed::ProbeResult p = sealed::boundedProbe(
        rigIndex(rig), kRigSlots, sealed::kTableProbeMax,
        [rig](size_t i) { return g_rigSlots[i].rig == rig; },
        [](size_t i) { const RigSlot& s = g_rigSlots[i]; return s.rig == 0 || s.epoch != g_rigEpoch; });
    noteProbe(g_rigStats, p);
    if (p.found) {
        RigSlot& s = g_rigSlots[p.index];
        return s.epoch == g_rigEpoch ? &s : rigClaim(s, rig);
    }
    if (p.empty && insert) return rigClaim(g_rigSlots[p.index], rig);
    return nullptr;
}

// The store's observer, for each record with a world sphere: the record's rig gets the record's box (this frame) and, the first time, the record's
// name is logged (eight names a session at most, so the identity can be checked by eye).
void rigNoteRecord(uintptr_t bucket, uintptr_t record, const float c[3], float r) noexcept {
    if (!(r > 0.0f) || !std::isfinite(r)) return;
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    // The association (Built v12.1, flight fix c; the record's +0x2B8 is NOT used: the v12 flight showed it is not the evaluated object). Synchronous
    // path: the rig evaluation's forward is on this thread's stack, so the record belongs to that rig. Job path: the store's bucket is the batch
    // entry copied from the Submit's entry (decomp_431AFE0 lines 106 to 117: [0] ctx, [2] = collection+0x300, [5] record array), so the rig is the
    // one whose collection is bucket[2] - 0x300. One cache entry per bucket per frame; the lookup is per bucket, not per record.
    uintptr_t rig = kinematicEvalCurrentRig();
    if (rig) {
        ++g_rigTally.assocSync;
    } else {
        if (g_rigBucketStamp != stamp) {
            g_rigBucketStamp = stamp;
            g_rigBucketCache = 0;
            g_rigBucketRig = 0;
        }
        if (bucket && bucket == g_rigBucketCache) {
            rig = g_rigBucketRig;
        } else {
            uintptr_t owner = 0;
            rig = 0;
            if (bucket && readVal(&owner, bucket + 0x10) && owner > 0x300) rig = collFind(owner - 0x300);
            g_rigBucketCache = bucket;
            g_rigBucketRig = rig;
        }
        if (rig) ++g_rigTally.assocJob;
        else ++g_rigTally.assocNone;
    }
    if (!rig) return;
    RigSlot* s = rigFind(rig, true);
    if (!s) return;
    if (s->evalStamp == stamp) ++g_rigTally.matched;
    else ++g_rigTally.unevaluated;
    if (s->curStamp != stamp) {
        if (s->curValid) {
            for (int k = 0; k < 3; ++k) {
                s->lastMn[k] = s->curMn[k];
                s->lastMx[k] = s->curMx[k];
            }
            s->lastValid = true;
        }
        s->curStamp = stamp;
        s->curValid = false;
    }
    for (int k = 0; k < 3; ++k) {
        const float lo = c[k] - r, hi = c[k] + r;
        if (!s->curValid) {
            s->curMn[k] = lo;
            s->curMx[k] = hi;
        } else {
            if (lo < s->curMn[k]) s->curMn[k] = lo;
            if (hi > s->curMx[k]) s->curMx[k] = hi;
        }
    }
    s->curValid = true;
    if (!s->named) {
        s->named = true;
        char name[64] = {};
        if (!cachedName(record, name, sizeof(name))) std::snprintf(name, sizeof(name), "(none)");
        std::snprintf(s->name, sizeof(s->name), "%s", name);   // Built v13: the own-ship prefix test reads it
    }
}

// Built v12.1 (flight fix d): the per-frame refresh of every rig's verdict, on the frame tick (render thread), from its bound: the box of its
// records' spheres in the last frame that had records. The cell is the one in force. The decision then reads one slot per rig.
void refreshRigVerdicts() noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    // Built v14.20 (review 7a): no cell held, no verdict is read (the veto answers only while sealed): no work. The first veto after an engage refreshes.
    if (!g_heldValid) {
        g_rigVerdictsFresh = false;
        return;
    }
    const bool allMode = false;
    for (RigSlot& s : g_rigSlots) {
        if (s.rig == 0 || s.epoch != g_rigEpoch) continue;
        const float* mn = nullptr;
        const float* mx = nullptr;
        if (s.curValid) {
            mn = s.curMn;
            mx = s.curMx;
        } else if (s.lastValid) {
            mn = s.lastMn;
            mx = s.lastMx;
        }
        const bool bounded = mn != nullptr;
        float c[3] = {0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (bounded) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                c[k] = 0.5f * (mn[k] + mx[k]);
                const float h = 0.5f * (mx[k] - mn[k]);
                d2 += h * h;
            }
            r = std::sqrt(d2);
        }
        const sealed::RigVerdict v = sealed::rigVerdict(bounded, c, r, g_cellPrev.has, g_cellPrev, allMode, 0);
        // Built v13 (fix 1): the own ship's rig is not vetoed in sphere or named mode: its bound holds the camera with a small radius, or its first
        // record's name has the own-ship prefix. (All mode vetoes it.)
        bool ownRig = false;
        if (!allMode && bounded && g_frameCamValid) ownRig = sealed::containsCamera(c, r, g_frameCam) && r < sealed::kOwnShipRadiusM;
        if (!allMode && !ownRig && g_ownPrefix[0] != '\0') ownRig = sealed::ownShipExempt(false, 0.0f, s.name, g_ownPrefix);
        s.verdict = ownRig ? kVerdictUnknown : sealed::verdictCode(v);   // 2 outside, 1 mixed, 0 unknown
    }
    g_rigVerdictsFresh = true;
}


uint32_t g_rigCacheStamp = 0;
uintptr_t g_rigCacheRig = 0;
RigSlot* g_rigCacheSlot = nullptr;

// The producer-B decision (FUN_144312E00's entry): true skips the record. The record's sphere is cached for the named rule (as the trial did);
// an unreadable record is kept; otherwise the cell keeps it, or the own ship's prefix does.
bool producerDecide(uintptr_t rec) noexcept {
    float centre[3] = {};
    float radius = 0.0f;
    const bool ok = storeSphereOf(rec, centre, &radius);
    if (ok) {
        prodSync();
        prodPut(rec, centre, radius);
    }
    if (!ok || !g_frameCamValid) return false;
    if (sealed::keepNamedCell(centre, radius, g_frameCam, kFallbackSphereM, g_cellPrev, g_explicitPrev, g_explicitPrevCount, rec).keep) return false;
    char nm[48] = {};
    if (g_ownPrefix[0] != '\0') cachedName(rec, nm, sizeof(nm));
    if (sealed::ownShipExempt(sealed::containsCamera(centre, radius, g_frameCam), radius, nm, g_ownPrefix)) return false;
    return true;
}

// The rig verdict's veto (the early veto, 0x431AFE0's entry): a rig whose verdict is outside the cell is skipped while its refresh counter is
// below sealed::kRigRefreshN. Answers only while sealed; the verdicts are refreshed on the frame tick.
// Built v14.20 (review 4): a lifecycle point (undock, hangar change, ship change, setting toggle, a changed association) moves to a new epoch. Every rig
// slot, association and verdict of the old epoch is free; a rig forwards until it has a fresh bound. Caller holds g_sealedMutex.
void bumpRigEpoch() noexcept {
    ++g_rigEpoch;
    ++g_rigEpochs;
    g_rigStats.used = 0;
    g_collStats.used = 0;
    g_rigVerdictsFresh = false;
    g_rigCacheStamp = 0;
    g_rigCacheRig = 0;
    g_rigCacheSlot = nullptr;
    g_rigBucketCache = 0;
    g_rigBucketRig = 0;
    g_rigBucketStamp = 0;
}

bool sealedTrialRigVeto(uintptr_t rig, uintptr_t ctx) noexcept {
    (void)ctx;
    // Built v14.20 (review 3): an atomic fast gate before the lock; g_heldValid is read again under it (engage and release write it under the lock).
    if (!g_sealedFlag.load(std::memory_order_acquire) || !rig) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    if (!g_heldValid) return false;
    if (!g_rigVerdictsFresh) refreshRigVerdicts();   // review 7a: the first veto after an engage reads verdicts refreshed for the held cell
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    ++g_rigTally.seen;
    RigSlot* s = nullptr;
    if (rig == g_rigCacheRig && g_rigCacheStamp == stamp && g_rigCacheSlot && g_rigCacheSlot->rig == rig && g_rigCacheSlot->epoch == g_rigEpoch) {
        s = g_rigCacheSlot;
    } else {
        s = rigFind(rig, true);
        if (!s) {
            ++g_rigTally.unknown;
            return false;
        }
        g_rigCacheRig = rig;
        g_rigCacheStamp = stamp;
        g_rigCacheSlot = s;
    }
    s->evalStamp = stamp;
    if (s->collStamp != stamp) {
        s->collStamp = stamp;
        uintptr_t coll = 0;
        if (readVal(&coll, rig + 0x348) && coll && coll != s->coll) {
            // Built v14.20 (review 4): an association change expires the old associations and this rig's bounds: it forwards until a fresh bound exists.
            bumpRigEpoch();
            s = rigFind(rig, true);
            if (!s) {
                ++g_rigTally.unknown;
                return false;
            }
            s->evalStamp = stamp;
            s->collStamp = stamp;
            s->coll = coll;
            collInsert(coll, rig);
            ++g_rigTally.unknown;
            return false;
        }
    }
    bool veto = false;
    switch (s->verdict) {
        case kVerdictOutside:
            if (s->vetoedRun >= sealed::kRigRefreshN) {
                ++g_rigTally.refresh;
                s->vetoedRun = 0;
            } else {
                ++g_rigTally.vetoed;
                ++s->vetoedRun;
                veto = true;
            }
            break;
        case kVerdictMixed:
            ++g_rigTally.mixed;
            s->vetoedRun = 0;
            break;
        default:
            ++g_rigTally.unknown;
            s->vetoedRun = 0;
            break;
    }
    return veto;
}

// The post-store cull of one record, with its fault handling: true when it was culled. A failed read or write leaves the record as the store wrote
// it (kept, counted as a fault); a torn put-back is counted as a fault too.
bool cullCounted(uintptr_t record, bool containing) noexcept {
    (void)containing;
    RecordFields fields{record};
    sealed::CullField failed = sealed::CullField::kMaskRead;
    uint64_t value = 0;
    switch (sealed::cullRecordFaultSafe(fields, &failed, &value)) {
        case sealed::Cull::kCulled:
            g_culled.fetch_add(1, std::memory_order_relaxed);
            return true;
        case sealed::Cull::kUntouched:
        case sealed::Cull::kRestored:
        case sealed::Cull::kTorn:
            g_faults.fetch_add(1, std::memory_order_relaxed);
            ++g_winFaults;
            return false;
    }
    return false;
}
// ---- The product's state: the setting, the seal, the release, the window counts ----------------------------------------------------------------
std::atomic<bool> g_setting{false};        // fix.optimized_station is on (read at install and every kSettingPollFrames frames)
bool g_vetoesAttached = false;             // the producer and rig vetoes are attached (under g_installMutex-free single writer: the render tick)
bool g_storeWanted = false;                // the store observer is wanted (the setting is on)
std::atomic<bool> g_dockedNow{false};     // Status.json Flags bit 0 (Docked), read at each tick while known (the store thread reads it)
bool g_dockedWasNow = false;               // built v14.20 (review 4): the docked flag at the last tick (an undock is a lifecycle point)
std::atomic<bool> g_dockedKnownNow{false};// Status.json has a Flags sample (the waiting line says unknown until then)
sealed::WaitView g_waitLast;              // the waiting line last written (built v14.1)
bool g_waitLastSet = false;
uint64_t g_waitLastMs = 0;                // when the waiting line was last written (at most every 10 s while docked)
sealed::SealState g_sealState;             // the seal trigger (optimized_station_core.h sealStep)
bool g_flagHangarChanged = false;          // promote saw the held pad go or another own pad nearest: the tick releases on it
bool g_engageAtLoadIn = false;             // the seal in force was requested at load-in (built v14.3): the status line says so
// Built v14.13: the sun inside the hangar (its own state, the skip read by the draw), the hangar watch, the names near a pad with no sized shell.
sealed::SunState g_sun;
std::atomic<bool> g_sunOff{false};
sealed::HangarWatch g_hangarWatch;
char g_hangarWhy[160] = "searching for a hangar shell";
struct NameNote {
    char name[48] = {};
    float dist = 0.0f;
    float radius = 0.0f;
};
NameNote g_nameLog[12];
size_t g_nameLogN = 0;
bool g_namesPending = false;
float g_namesPadC[3] = {};
uintptr_t g_hangarPad = 0;
// Built v14.12: the bay's mechanism, latched once per descent (core LatchSet). Candidates are gathered only while a latch is pending.
sealed::LatchSet g_latch;
sealed::LatchIdentity g_latchId;   // built v14.20 (review 2): the pad and shell the latch was built for
// Built v14.17: the dock's arrival and this dock's seal (the lock: g_sealedMutex); the pad's turn and the shell's change as still motion; the pad
// in the shell's frame this frame (the temporary track line and the sealed line).
sealed::DockWatch g_dock;
sealed::PadStillWatch g_padStill;
sealed::ShellFrame g_frameShell;
sealed::LatchCand g_latchCands[sealed::kLatchCandCap];
size_t g_latchCandN = 0;
bool g_latchPending = false;         // docked, unsealed, after a descent, not yet latched
bool g_bayWatching = false;          // docked, unsealed, after a descent: the latch is kept
bool g_bayNoting = false;            // watching, and the shell read this frame: the latched records are read
uint64_t g_watchNowMs = 0;
float g_shellNow[3] = {};
uint8_t g_shellNowSrc = 4;
bool g_shellNowValid = false;
int g_engageVia = 0;                 // 0 load-in, 1 the bay settled, 2 the 30 s fallback
uint32_t g_engageWatched = 0;
float g_engageSinceLowerS = 0.0f;
char g_engageLastMover[48] = {};
char g_engageMovers[160] = {};
bool g_mechanismMoved = false;             // built v14.14: a latched bay mechanism moved while sealed
bool g_flagPadMoved = false;               // promote saw the held pad move along the shaft against the shell: the tick releases on it
char g_releaseWhy[160] = {};               // the release text for the flags above (the pad move or the hangar change, with its names)
uint32_t g_settingPoll = 0;                // frames since the setting was last read
uint64_t g_summaryStartMs = 0;             // the 30 s summary's window
uint64_t g_winFrames = 0;                  // ticks in the window
uint64_t g_winSealedFrames = 0;            // ticks in the window while sealed
uint64_t g_winRigVetoes = 0;               // rig vetoes (before the forward) in the window
uint64_t g_winProducerVetoes = 0;          // records the producer-B veto skipped in the window
uint64_t g_winCulled = 0;                  // records culled after the store in the window
uint64_t g_winKeptUnknown = 0;             // records kept because their centre could not be read, in the window
uint32_t g_sealCount = 0;                  // seals since the setting was turned on (for the status lines)

// The sun pass the station skips while sealed (the pair the trial measured: VS 7E38A6AA1269C901 with PS 7CECABDE34FFBE9E).
constexpr uint64_t kSunVs = 0x7E38A6AA1269C901ull;
constexpr uint64_t kSunPs = 0x7CECABDE34FFBE9Eull;

// The pad must be at its lowest seen height, and still to within kSealStillM, for kSealDelayMs before the seal (the hatch closes in this time:
// a guess to be measured in flight; the maintainer will tell us the hatch close time).
// The camera leaves the held box by more than this and the seal is released (riding the elevator down, an external camera flying out).
constexpr uint32_t kSettingPollFrames = 30;
constexpr uint64_t kSummaryMs = 30000;
constexpr uint32_t kEngageTries = 90;
constexpr float kLoweredToleranceM = 15.0f;
constexpr uint32_t kRigRefreshFramesMax = 30;   // (the rig verdict refresh cadence is sealed::kRigRefreshN; kept here for the docs)

const char* sizeWord(sealed::PadSize s) noexcept {
    switch (s) {
        case sealed::PadSize::kSmall: return "small";
        case sealed::PadSize::kMedium: return "medium";
        case sealed::PadSize::kLarge: return "large";
        default: return "unknown-size";
    }
}

// Releases the seal: the held cell, its pad-local frame, the own record and the shell go; the vetoes stop from this frame (they test the
// atomic mirror). One status line says why. The next seal needs the whole seal condition again (still for kSealDelayMs at the lowest height).
// The release's words for the status line, by the rule that released.
const char* releaseText(sealed::ReleaseWhy w) noexcept {
    switch (w) {
        case sealed::ReleaseWhy::kSettingOff: return "the setting is off";
        case sealed::ReleaseWhy::kUndocked: return "undocked";
        case sealed::ReleaseWhy::kCameraLeft: return "the camera left the bay by more than 2 m (riding out of the hangar, or an external camera)";
        case sealed::ReleaseWhy::kPadMoved: return g_releaseWhy;
        case sealed::ReleaseWhy::kHangarChanged: return g_releaseWhy;
        case sealed::ReleaseWhy::kLaunch: return "launch (the pad's state object flipped)";
        case sealed::ReleaseWhy::kMechanism: return g_releaseWhy;
        default: return "released";
    }
}

void releaseSeal(const char* why, uint32_t frame) noexcept {
    // Built v14.20 (review 3): the release is a state change: it runs under the state lock, serialized with the store callbacks.
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    if (!g_heldValid) return;
    padStateEvent("release");   // built v14.22: the pad-state probe writes its line at the next tick
    Log::get().note("optimized station: released (frame %u): %s. Everything is drawn as without the station until the seal conditions hold again.",
                    frame, why);
    g_heldValid = false;
    g_rigVerdictsFresh = false;   // built v14.20 (review 7a): the next engage refreshes the verdicts before its first veto
    g_heldCell = sealed::OwnCell{};
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_ownRecAddr = 0;
    g_ownLiveValid = false;
    g_heldAbsent = 0;
    g_otherPicked = 0;
    g_engagePending = false;
    sealed::clearTrigger(&g_sealState);   // the pad's motion memory stays: a release by motion makes the next seal wait its delay (built v14.4)
    g_sealedFlag.store(false, std::memory_order_release);
    // the latch is kept across a release (built v14.16): undock, a hangar change and a ship change clear it
}

// Built v14.12: the latch is kept while docked, unsealed, after a descent; it is cleared on a hangar change, a ship change, an undock or a seal.
bool bayWatchKeep() noexcept {
    return g_setting.load(std::memory_order_acquire) &&   // kept through the seal too (built v14.14)
           g_dockedNow.load(std::memory_order_acquire);   // from docking, not only after a descent: a load-in seal latches too (built v14.15)
}

// Built v14.12: a gathered record that may be a mechanism record (its name from the cache), within 150 m of the camera. Only while a latch is pending.
void latchCandNote(uintptr_t record, const float* centre) noexcept {
    if (!g_latchPending || g_latchCandN >= sealed::kLatchCandCap) return;
    if (g_frameCamValid && sealed::distance(centre, g_frameCam) > 400.0f) return;   // a bay's span reaches ~270 m along the shaft (built v14.15)
    char nm[48] = {};
    if (!cachedName(record, nm, sizeof(nm)) || !sealed::mechanismName(nm)) return;
    sealed::LatchCand& cand = g_latchCands[g_latchCandN++];
    cand.addr = record;
    std::snprintf(cand.name, sizeof(cand.name), "%s", nm);
    cand.centre[0] = centre[0];
    cand.centre[1] = centre[1];
    cand.centre[2] = centre[2];
}

// Built v14.12: latch the bay's mechanism once: the own pad's shaft (its up axis), the pads of its size and the mechanism records near the shaft.
void latchNow(uint64_t now) noexcept {
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) return;
    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
    static sealed::LatchCand cands[sealed::kPadSlots + sealed::kLatchCandCap];
    size_t n = 0;
    for (size_t i = 0; i < g_padCount && n < sealed::kPadSlots; ++i) {
        cands[n].addr = g_pads[i].addr;
        std::snprintf(cands[n].name, sizeof(cands[n].name), "%s", g_pads[i].name);
        for (int k = 0; k < 3; ++k) cands[n].centre[k] = g_pads[i].centre[k];
        ++n;
    }
    for (size_t i = 0; i < g_latchCandN; ++i) cands[n++] = g_latchCands[i];
    const float alongMin = -belowFor(g_pickCell.size);
    // The span: the target's distance when the shaft target is built, and never less than the bay's span (built v14.15).
    const float alongMax = g_pickCell.capsule ? std::max(g_pickCell.capLen + 10.0f, sealed::kLatchSpanM) : sealed::kLatchSpanM;
    sealed::LatchReject rej[10];
    size_t rejN = 0;
    const size_t k = sealed::selectMechanism(cands, n, g_pickCell.pad, g_pickCell.padName, g_pickCell.size, g_pickCell.centre, up, alongMin, alongMax,
                                             g_latch.recs, rej, 10, &rejN);
    g_latch.n = k;
    g_latch.latched = true;
    g_latchId.bind(g_pickCell.pad, g_preShellValid ? g_preShellAddr : 0);   // built v14.20 (review 2)
    g_latch.lastMotionMs = 0;
    char names[200] = {};
    size_t used = 0;
    for (size_t i = 0; i < k; ++i) {
        const int w = std::snprintf(names + used, sizeof(names) - used, "%s%s", i > 0 ? ", " : "", g_latch.recs[i].name);
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(names)) break;
        used += static_cast<size_t>(w);
    }
    Log::get().note("optimized station: watching the bay's mechanism (%u records): %s", static_cast<unsigned>(k), names);
    for (size_t i = 0; i < rejN; ++i) Log::get().note("optimized station: latch refused %s: %s", rej[i].name, rej[i].why);
}

// Built v14.12: each latched record is read by address in the tick (its world centre and orientation), against the shell read this frame. A record
// unreadable this frame is skipped and re-based on the next reading.
void latchRead(uint64_t now) noexcept {
    const uint32_t frame = g_frameStamp.load(std::memory_order_relaxed);
    for (size_t i = 0; i < g_latch.n; ++i) {
        sealed::LatchRec& r = g_latch.recs[i];
        const WorldRead pw = worldOfRecord(r.addr);
        if (pw.source == 4) continue;
        float axes[3][3] = {};
        float ext[3] = {};
        bool extOk = false;
        uint8_t src = 4;
        if (!poseOrientOf(r.addr, axes, ext, &extOk, &src)) continue;
        const float rel[3] = {pw.world[0] - g_shellNow[0], pw.world[1] - g_shellNow[1], pw.world[2] - g_shellNow[2]};
        r.step(frame, rel, axes[0], axes[2], pw.source, g_shellNowSrc, now);
    }
    g_latch.recomputeMotion();
}

// Built v14.12: the engage's account for the status line: load-in, the bay settled, or the 30 s fallback, with the latch's numbers.
void captureEngageInfo(uint64_t now) noexcept {
    g_engageVia = g_sealState.immediate ? 0 : (g_sealState.fallback ? 2 : 1);
    g_engageWatched = static_cast<uint32_t>(g_latch.n);
    g_latch.lastMover(g_engageLastMover, sizeof(g_engageLastMover));
    g_engageSinceLowerS = (g_sealState.lowerSet && now > g_sealState.lowerSinceMs) ? static_cast<float>(now - g_sealState.lowerSinceMs) / 1000.0f : 0.0f;
    g_latch.movers(now, g_engageMovers, sizeof(g_engageMovers), 3);
}

// The own pad's still time for the seal: the pad's last motion (its latch record).
// Built v14.17: the pad's turn and the shell's change restart it too (PadStillWatch).
uint64_t padLastMotion() noexcept { return std::max(g_latch.padLastMotionMs(), g_padStill.motionMs()); }

// The seal's account for the status line (built v14.10).
const char* engageHow() noexcept {
    static char how[256];
    if (g_engageVia == 0) return "sealed at load-in";
    if (g_engageVia == 2) {
        std::snprintf(how, sizeof(how), "sealed after the 30 s fallback (bay never still: movers %s)", g_engageMovers[0] != '\0' ? g_engageMovers : "none");
        return how;
    }
    std::snprintf(how, sizeof(how), "sealed after the bay settled (%u records watched, last mover %s, %.1f s after the pad lowered)",
                  static_cast<unsigned>(g_engageWatched), g_engageLastMover[0] != '\0' ? g_engageLastMover : "none",
                  static_cast<double>(g_engageSinceLowerS));
    return how;
}

// Built v14.13: a gathered record that may name the hangar, near the pad, while the names are wanted (a docked pad with no sized shell, not yet noted).
void hangarNameNote(uintptr_t record, const float* centre, float radius) noexcept {
    if (!g_namesPending || g_nameLogN >= 12) return;
    const float d = sealed::distance(centre, g_namesPadC);
    if (d > 150.0f) return;
    char nm[48] = {};
    if (!cachedName(record, nm, sizeof(nm)) || !sealed::hangarLikeName(nm)) return;
    NameNote& nn = g_nameLog[g_nameLogN++];
    std::snprintf(nn.name, sizeof(nn.name), "%s", nm);
    nn.dist = d;
    nn.radius = radius;
}

// Built v14.13: the once-per-dock note with no sized shell: the hangar-like and pad records near the pad (the first carrier dock tells us its
// names), or, when there are none, that there is no hangar here.
void hangarNote() noexcept {
    if (g_nameLogN == 0) {
        std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no hangar here (outpost or open pad): nothing to do");
        Log::get().note("optimized station: no hangar here (outpost or open pad): nothing to do");
        return;
    }
    std::snprintf(g_hangarWhy, sizeof(g_hangarWhy), "%s", "no sized hangar shell matched near the pad (names logged)");
    char text[400] = {};
    size_t used = 0;
    for (size_t i = 0; i < g_nameLogN; ++i) {
        const int w = std::snprintf(text + used, sizeof(text) - used, "%s%s (%.1f m, r %.1f m)", i > 0 ? "; " : "", g_nameLog[i].name,
                                    static_cast<double>(g_nameLog[i].dist), static_cast<double>(g_nameLog[i].radius));
        if (w < 0 || used + static_cast<size_t>(w) >= sizeof(text)) break;
        used += static_cast<size_t>(w);
    }
    Log::get().note("optimized station: no sized hangar shell near the pad; the hangar-like and pad records near it: %s", text);
}

// Built v14.17: a log value with two decimals, or "n/a" when it is unreadable.
void numOrNa(char* out, size_t cap, bool ok, float v) noexcept {
    if (ok) std::snprintf(out, cap, "%.2f", static_cast<double>(v));
    else std::snprintf(out, cap, "n/a");
}

// Built v14.17: a whole-millisecond log value, or "n/a" when it is unreadable.
void msOrNa(char* out, size_t cap, bool ok, uint64_t ms) noexcept {
    if (ok) std::snprintf(out, cap, "%llu", static_cast<unsigned long long>(ms));
    else std::snprintf(out, cap, "n/a");
}

// The seal's status line, once per seal (called from the engage, after the cell is held and localized).
void sealStatus(const sealed::OwnCell& cell, uint32_t frame) noexcept {
    ++g_sealCount;
    padStateEvent("seal");   // built v14.22: the pad-state probe writes its line at the next tick
    char lat[24] = {};
    char yaw[24] = {};
    numOrNa(lat, sizeof(lat), g_frameShell.ok, g_frameShell.lateral);   // the last frame's shell frame (built v14.17)
    numOrNa(yaw, sizeof(yaw), g_frameShell.yawOk, g_frameShell.yawDeg);
    Log::get().note("optimized station: sealed in %s hangar (%s, frame %u, seal %u): pad %s, box %.0f x %.0f x %.0f m (length, width, height), "
                    "floor %.0f m under the pad, shell %s, lateral %s m, yaw %s deg.", sizeWord(cell.size), engageHow(),
                    frame, g_sealCount, cell.padName,
                    static_cast<double>(cell.boxHalf[0] * 2.0f), static_cast<double>(cell.boxHalf[1] * 2.0f),
                    static_cast<double>(cell.boxHalf[2] * 2.0f), static_cast<double>(belowFor(cell.size)),
                    g_shellValid ? g_shellName : "none (the pad's motion is not checked)", lat, yaw);
}

// Built v14.17 (TEMPORARY instrument: remove when the outpost flight confirms the rule). While docked, unsealed, after an arrival, at 4 Hz, at most
// 240 lines per dock: the pad's track in the shell's frame and its still times. The position and turn still times run from the later of their last
// motion and the dock's first line (so a pad that never moved reads as still for the time observed); the bay quiet time is the seal's own clock.
void padTrackLine(uint64_t now) noexcept {
    sealed::DockWatch& d = g_dock;
    if (d.trackLines >= 240) return;
    if (d.trackLastMs != 0 && now - d.trackLastMs < 250) return;
    if (d.trackStartMs == 0) d.trackStartMs = now;
    d.trackLastMs = now;
    ++d.trackLines;
    const uint64_t floorMs = d.trackStartMs;
    char rel[24] = {}, x[24] = {}, y[24] = {}, z[24] = {}, lat[24] = {}, yaw[24] = {};
    char stillS[24] = {}, stillT[24] = {}, quiet[24] = {}, shell[96] = {};
    numOrNa(rel, sizeof(rel), g_relOk, g_relM);
    if (g_preShellValid) {
        std::snprintf(shell, sizeof(shell), "%s@%06llx", g_preShellName, static_cast<unsigned long long>(g_preShellAddr & 0xFFFFFFu));
    } else {
        std::snprintf(shell, sizeof(shell), "n/a");
    }
    numOrNa(x, sizeof(x), g_frameShell.ok, g_frameShell.x);
    numOrNa(y, sizeof(y), g_frameShell.ok, g_frameShell.y);
    numOrNa(z, sizeof(z), g_frameShell.ok, g_frameShell.z);
    numOrNa(lat, sizeof(lat), g_frameShell.ok, g_frameShell.lateral);
    numOrNa(yaw, sizeof(yaw), g_frameShell.yawOk, g_frameShell.yawDeg);
    msOrNa(stillS, sizeof(stillS), g_pickCell.has, now - std::max(g_latch.padLastMotionMs(), floorMs));
    msOrNa(stillT, sizeof(stillT), g_pickCell.has, now - std::max(g_padStill.turnMs, floorMs));
    const uint64_t quietSince = std::max(std::max(g_sealState.lowerSinceMs, g_latch.lastMotionMs), padLastMotion());
    msOrNa(quiet, sizeof(quiet), g_sealState.lowerSet && now >= quietSince, now - quietSince);
    Log::get().note("optimized station: pad track: rel %s m, shell %s, pad in shell frame (x %s, y %s, z %s along the shaft) m, "
                    "lateral %s m from the shell axis, yaw %s deg against the shell, still %s ms (position), %s ms (turn), bay quiet %s ms",
                    rel, shell, x, y, z, lat, yaw, stillS, stillT, quiet);
}

// The 30 s summary: what the station did in the window. A user's log shows the station's work here.
void summaryLine(uint64_t now) noexcept {
    Log::get().note("optimized station: last %llu s: sealed for %llu of %llu frames; rigs vetoed early %llu, records vetoed early %llu, "
                    "records culled after the store %llu, records kept unread %llu, faults %llu.",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g_winSealedFrames),
                    static_cast<unsigned long long>(g_winFrames), static_cast<unsigned long long>(g_winRigVetoes),
                    static_cast<unsigned long long>(g_winProducerVetoes), static_cast<unsigned long long>(g_winCulled),
                    static_cast<unsigned long long>(g_winKeptUnknown), static_cast<unsigned long long>(g_winFaults));
    // Built v14.20 (review 5): the tables' occupancy now, the longest probe and the bounded (full) count this window, and the rig epoch count.
    Log::get().note("optimized station: last %llu s tables: producer %u of %u used, longest probe %u, bounded %llu; rigs %u of %u, longest %u, "
                    "bounded %llu; collections %u of %u, longest %u, bounded %llu; rig epochs %llu (epoch %u).",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned>(g_prodStats.used),
                    static_cast<unsigned>(kProdSlots), static_cast<unsigned>(g_prodStats.maxProbes),
                    static_cast<unsigned long long>(g_prodStats.full), static_cast<unsigned>(g_rigStats.used),
                    static_cast<unsigned>(kRigSlots), static_cast<unsigned>(g_rigStats.maxProbes), static_cast<unsigned long long>(g_rigStats.full),
                    static_cast<unsigned>(g_collStats.used), static_cast<unsigned>(kCollSlots), static_cast<unsigned>(g_collStats.maxProbes),
                    static_cast<unsigned long long>(g_collStats.full), static_cast<unsigned long long>(g_rigEpochs),
                    static_cast<unsigned>(g_rigEpoch));
    g_prodStats.maxProbes = g_rigStats.maxProbes = g_collStats.maxProbes = 0;
    g_prodStats.full = g_rigStats.full = g_collStats.full = 0;
    g_rigEpochs = 0;
    g_summaryStartMs = now;
    g_winFrames = g_winSealedFrames = g_winRigVetoes = g_winProducerVetoes = 0;
    g_winCulled = g_winKeptUnknown = g_winFaults = 0;
}

// The producer-B and rig vetoes: attached while the setting is on, and they answer only while sealed.
bool producerVetoHook(uintptr_t rec, uintptr_t owner) noexcept {
    (void)owner;
    if (!g_sealedFlag.load(std::memory_order_acquire)) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    if (!g_heldValid) return false;
    const bool veto = producerDecide(rec);
    if (veto) ++g_winProducerVetoes;
    return veto;
}

bool rigVetoHook(uintptr_t rig, uintptr_t ctx) noexcept {
    if (!g_sealedFlag.load(std::memory_order_acquire)) return false;
    const bool veto = sealedTrialRigVeto(rig, ctx);
    if (veto) ++g_winRigVetoes;
    return veto;
}

// Attaches or detaches the vetoes and the store observer to the setting. Attach is refused (logged once) when a hook stands down.
// Built v14.20 (review 3): the toggle's hook operations run OUTSIDE the state lock (a detach may wait for callbacks, and a callback needs the lock).
// Off: the callbacks are detached and the gate closed first, so no new callback starts; then the state changes under the lock, which waits for a
// callback already inside. On: the state resets under the lock, then the hooks attach. Either way the rig and collection epoch moves on.
void applySetting(bool on, uint32_t frame) noexcept {
    if (on == g_vetoesAttached) return;
    if (!on) {
        kinematicEvalSetDirectVeto(nullptr);
        kinematicEvalSetRigVeto(nullptr);
        kinematicEvalDirectVetoDetach();
        kinematicEvalRigVetoDetach();
        g_storeWanted = false;
        storeHookRecompute();   // closes the gate before the state changes
    }
    {
        std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
        g_sealState = sealed::SealState{};   // a setting toggle starts the trigger and its memory afresh (built v14.4)
        g_heightSrc = sealed::ShellHeightSources{};
        g_preShellValid = false;
        g_sealGuard = sealed::SealRateGuard{};
        bumpRigEpoch();                      // built v14.20 (review 4): the rig and collection tables start afresh
        if (!on && g_sun.off) {   // setting off: the sun comes back on (built v14.13)
            g_sun.off = false;
            g_sunOff.store(false, std::memory_order_release);
            Log::get().note("optimized station: sun light inside the hangar back on (setting off; Status docked=%s)",
                            g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no");
        }
        if (!on) releaseSeal("setting off", frame);
    }
    if (on) {
        const char* direct = kinematicEvalDirectVetoAttach();
        const char* rig = kinematicEvalRigVetoAttach();
        kinematicEvalSetDirectVeto(&producerVetoHook);
        kinematicEvalSetRigVeto(&rigVetoHook);
        g_storeWanted = true;
        storeHookRecompute();
        Log::get().note("optimized station: on (frame %u). Producer veto: %s; rig veto: %s; store observer: %s.", frame, direct, rig,
                        storeHookStatus());
    } else {
        Log::get().note("optimized station: off (frame %u).", frame);
    }
    padStateSetOn(on);   // built v14.22: the pad-state probe (temporary, log only) installs with the setting
    g_vetoesAttached = on;
}

// The oriented box for the pad at the freeze: the pad's own orientation and extent (its builder pose), the wiki footprint doubled along its length
// (the hangar is the pad doubled along one axis), the margin, and the floor. Without a pose or an extent there is no box (the capsule is the cell).
void buildBoxFor(sealed::OwnCell& cell, const float target[3]) {
    cell.box = false;
    if (!cell.has || cell.size == sealed::PadSize::kNone) return;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src) || !extOk) return;
    float u[3] = {target[0] - cell.centre[0], target[1] - cell.centre[1], target[2] - cell.centre[2]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(ul >= 1.0f)) return;
    for (float& v : u) v /= ul;
    int li = -1, wi = -1, ui = -1;
    if (!sealed::footprintAxes(axes, u, ext, &li, &wi, &ui)) return;
    const float* wiki = sealed::kPadWikiM[sealed::sizeIndex(cell.size)];
    const float halfLen = wiki[0] * 0.5f * 2.0f;
    const float halfWid = wiki[1] * 0.5f;
    sealed::buildBox(cell.centre, target, axes[li], halfLen, halfWid, g_margin, belowFor(cell.size), &cell);
}

// The promote: the frame's first record. The pad pick, the pad's heights, the capsule and box, the engage, the hangar-change drops.
void promoteFrame(uint32_t stamp, uint32_t frame) {
    sealed::OwnCell next;
    g_ownCandidateDist = -1.0f;
    g_prefixTallyN = 0;   // the tally is this frame's records (built v14.8)
    std::memcpy(g_nearPrev, g_nearCur, sizeof(g_nearPrev));
    g_nearPrevN = g_nearCurN;
    g_nearCurN = 0;
    readOwnLive();
    refreshHeldWorld();
    if (g_frameCamValid) {
        const float radii[3] = {sealed::kCellDefaultM[0], sealed::kCellDefaultM[1], sealed::kCellDefaultM[2]};
        for (size_t i = 0; i < g_padCount; ++i) {
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            g_pads[i].upOk = poseOrientOf(g_pads[i].addr, axes, ext, &extOk, &src);
            if (g_pads[i].upOk) {
                for (int k = 0; k < 3; ++k) g_pads[i].up[k] = axes[2][k];
            }
        }
        sealed::pickOwnPadSticky(g_pads, g_padCount, g_frameCam, radii, &g_ownSticky, &next);
        if (next.has) {
            float e2[3] = {};
            float axes[3][3] = {};
            float ext[3] = {};
            bool extOk = false;
            uint8_t src = 4;
            const bool haveAxes = poseOrientOf(next.pad, axes, ext, &extOk, &src);
            if (haveAxes) {
                for (int k = 0; k < 3; ++k) e2[k] = axes[2][k];
            }
            float q[3] = {};
            char qn[48] = {};
            float perp = 0.0f, along = 0.0f;
            bool picked = false;
            if (haveAxes) {
                picked = sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 0, q, qn, sizeof(qn), &perp, &along) ||
                         sealed::pickTargetOnLine(g_explicits, g_explicitCount, next.centre, e2, 1, q, qn, sizeof(qn), &perp, &along);
            }
            if (!picked) picked = sealed::pickCapsuleTarget(g_explicits, g_explicitCount, next.centre, q, qn, sizeof(qn));
            if (picked && sealed::buildCapsule(next.centre, q, sealed::kCapsuleExtendM, next.capA, next.capB, &next.capLen)) {
                next.capsule = true;
                std::snprintf(next.capTarget, sizeof(next.capTarget), "%s", qn);
                buildBoxFor(next, q);
            }
        }
    } else {
        std::snprintf(next.reason, sizeof(next.reason), "no camera for the previous frame");
    }
    g_pickCell = next;
    // The held pad: absent from the store for kHeldLostFrames frames, or another own pad nearest for that long, is a hangar change.
    if (g_heldValid) {
        bool heldSeen = false;
        const sealed::PadObs* best = nullptr;
        float bestD = 0.0f;
        for (size_t i = 0; i < g_padCount; ++i) {
            const sealed::PadObs& p = g_pads[i];
            if (p.addr == g_heldCell.pad) heldSeen = true;
            if (!g_frameCamValid) continue;
            const float d = sealed::distance(p.centre, g_frameCam);
            if (!sealed::ownPadCandidate(d, p.radius)) continue;
            if (!best || d < bestD) {
                best = &p;
                bestD = d;
            }
        }
        g_heldAbsent = heldSeen ? 0 : g_heldAbsent + 1;
        const bool otherPick = next.has && next.pad != g_heldCell.pad;   // the sticky pick: another pad only after the held one is unseen
        g_otherPicked = otherPick ? g_otherPicked + 1 : 0;
        if (sealed::heldCellDrops(g_heldAbsent, g_otherPicked, sealed::kHeldLostFrames)) {
            char why[160];
            if (g_heldAbsent >= sealed::kHeldLostFrames)
                std::snprintf(why, sizeof(why), "the hangar changed: the held pad %s is no longer in the store's records", g_heldCell.padName);
            else
                std::snprintf(why, sizeof(why), "the hangar changed: the nearest own pad is now %s", best ? best->name : "none");
            g_flagHangarChanged = true;
            std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
        }
    }
    if (g_releaseMotion && g_heldValid) {
        char why[160];
        std::snprintf(why, sizeof(why), "the pad moved %.1f m along the shaft relative to the hangar shell %s (more than %.0f m)",
                      static_cast<double>(g_shellDeltaM), g_shellName, static_cast<double>(sealed::kPadMoveReleaseM));
        g_flagPadMoved = true;
        std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", why);
    }
    // The engage: the cell is frozen from the pad when the seal rule asked for it. Held frames take the held cell.
    if (g_engagePending) {
        ++g_engageTries;
        // Built v14.20 (review 1): the freeze needs the request still in force for this pad and shell, a lowered pad in the bay, and a valid cell.
        const sealed::EngageRequest req{g_engagePending, g_engagePad, g_engageShell};
        const bool cellValid = next.has && next.radius > 0.0f && sealed::cellInvalidWhy(next) == nullptr;
        const bool admitted = sealed::engageAdmitted(req, g_sealState, next.pad, g_preShellValid ? g_preShellAddr : 0, g_relOk, g_relM, cellValid);
        if (admitted) {
            setOwnPrefixAtEngage(frame);
            pickOwnRecord(frame);
            g_heldCell = next;
            g_heldValid = true;
            g_rigVerdictsFresh = false;     // built v14.20 (review 7a): the verdicts are refreshed before this seal's first veto
            g_dock.sealedThisDock = true;   // built v14.17: this dock has sealed (the sun's arrival hold ends)
            setHeldFrame(g_heldCell, frame);
            g_engagePending = false;
            g_sealedFlag.store(true, std::memory_order_release);
            sealStatus(g_heldCell, frame);
        } else if (!g_sealState.requested || (next.has && next.pad != g_engagePad)) {
            // The request was withdrawn, or it belongs to another pad: cancelled, and never frozen later (built v14.20, review 1).
            g_engagePending = false;
            g_engageTries = 0;
        } else if (g_engageTries >= kEngageTries) {
            // Built v14.8: the cell is not valid (no box, a zero box, no target), so no seal. The seal is requested again (the pad may gain a target);
            // the note is written once per distinct reason.
            g_engagePending = false;
            g_sealState.requested = false;
            const char* why = sealed::cellInvalidWhy(next);
            if (why == nullptr) why = next.reason;
            static char lastWhy[120] = {};
            if (std::strcmp(lastWhy, why) != 0) {
                std::snprintf(lastWhy, sizeof(lastWhy), "%s", why);
                Log::get().note("optimized station: no seal: the cell is not valid (%s); the station keeps drawing everything, and the seal is tried again.", why);
            }
            g_engageTries = 0;
        }
        if (!g_heldValid) next = sealed::OwnCell{};
    } else if (g_heldValid) {
        next = g_heldCell;
    } else {
        next = sealed::OwnCell{};
    }
    if (next.has && next.capsule)
        g_explicitPrevCount = sealed::pickExplicitSegment(g_explicits, g_explicitCount, next.capA, next.capB, g_explicitPrev, sealed::kExplicitNodes);
    else
        g_explicitPrevCount = next.has ? sealed::pickExplicit(g_explicits, g_explicitCount, next.centre, g_explicitPrev, sealed::kExplicitNodes) : 0;
    g_cellPrev = next;
    g_padCount = 0;
    g_explicitCount = 0;
    g_cellStamp = stamp;
}

// Localizes the held cell to the held pad's frame and picks the shell. No logs: the seal's status line is written by the caller.
void setHeldFrame(const sealed::OwnCell& cell, uint32_t frame) {
    (void)frame;
    g_heldLocalValid = false;
    g_shellValid = false;
    g_releaseMotion = false;
    g_shellMotion = sealed::ShellMotion{};
    g_shellSampleStamp = 0xFFFFFFFFu;
    float axes[3][3] = {};
    float ext[3] = {};
    bool extOk = false;
    uint8_t src = 4;
    if (!poseOrientOf(cell.pad, axes, ext, &extOk, &src)) return;
    std::memcpy(g_engageAxes, axes, sizeof(g_engageAxes));
    std::memcpy(g_engageCentre, cell.centre, sizeof(g_engageCentre));
    sealed::localizeCell(cell, cell.centre, axes, &g_heldLocal);
    g_heldLocalValid = true;
    const size_t bestI = sealed::pickShell(g_explicits, g_explicitCount, cell.centre, cell.size, 150.0f);   // chosen once, here
    if (bestI < g_explicitCount) {
        const sealed::ExplicitObs& x = g_explicits[bestI];
        g_shellAddr = x.addr;
        std::snprintf(g_shellName, sizeof(g_shellName), "%s", x.name);
        g_shellValid = true;
    }
}

// The store observer's judgement, for one record the store has written, while sealed. Order: the centre (unreadable: kept), the named keep (the
// pad and explicit parts, then the cell), the bay keep, the own ship (the prefix, then the live sphere), otherwise culled.
// judge: the sealed judgement (the keep and the cull); false: gathering only (the frame's camera, pads and explicit parts).
void afterStoreJudge(uintptr_t bucket, uintptr_t record, bool judge) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    ++g_recordsFrame;
    if (judge && !g_heldValid) return;
    uintptr_t ctx = 0;
    uint32_t count = 0;
    if (!bucket || !record || !readVal(&ctx, bucket) || !ctx || !readVal(&count, ctx + kCtxCount) || count == 0) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        ++g_winFaults;
        return;
    }
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    if (g_viewStamp != stamp) {
        g_viewStamp = stamp;
        g_viewTries = 0;
        g_viewReady = false;
    }
    // Built v14.20 (review 7c): retry while the centre the consumer uses is missing (the origin, or the explorer cam's rows), not on a missing eye pose.
    if (!g_viewReady || (!sealed::viewCentreReady(g_view.originOk, g_view.poseOk, g_view.source == view::Source::kExplorerCam) && g_viewTries < kViewTries)) {
        ++g_viewTries;
        view::resolveView(ctx, &g_view);
        g_viewReady = true;
    }
    g_lastCtx = ctx;
    float cam[3] = {};
    if (!cameraOf(g_view, cam)) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        ++g_winFaults;
        return;
    }
    g_cameraKnown.store(true, std::memory_order_relaxed);
    if (sealed::framePromoteDue(g_cellStamp, stamp)) promoteFrame(stamp, stamp);
    std::memcpy(g_frameCam, cam, sizeof(g_frameCam));
    g_frameCamValid = true;
    const WorldRead wr = worldOfRecord(record);
    if (judge) {
        ++g_worldSrc[wr.source];
        if (wr.source != 4) rigNoteRecord(bucket, record, wr.world, wr.radius);
    }
    const float* centre = wr.world;
    const float radius = wr.radius;
    if (wr.source == 4 || !std::isfinite(radius) || !(radius > 0.0f) || !std::isfinite(centre[0]) || !std::isfinite(centre[1]) ||
        !std::isfinite(centre[2])) {
        if (judge) {
            g_keptUnknown.fetch_add(1, std::memory_order_relaxed);   // the centre cannot be read: kept, never judged
            ++g_winKeptUnknown;
        }
        return;
    }
    bool keep = false;
    bool containing = false;
    uintptr_t node = 0;
    const sealed::NameClass cls = classOf(record, &node);
    if (cls == sealed::NameClass::kPad && g_padCount < sealed::kPadSlots) {
        sealed::PadObs& p = g_pads[g_padCount];
        char name[64] = {};
        if (cachedName(record, name, sizeof(name))) {
            p.addr = record;
            p.centre[0] = centre[0];
            p.centre[1] = centre[1];
            p.centre[2] = centre[2];
            p.radius = radius;
            p.size = sealed::padSizeOf(name);
            std::snprintf(p.name, sizeof(p.name), "%s", name);
            if (p.size != sealed::PadSize::kNone) ++g_padCount;
        }
    } else if (cls == sealed::NameClass::kExplicit && g_explicitCount < sealed::kExplicitSlots && !g_hangarWatch.giveUp) {   // built v14.13: stops after the give-up
        sealed::ExplicitObs& x = g_explicits[g_explicitCount++];
        x.addr = record;
        x.node = node;
        x.centre[0] = centre[0];
        x.centre[1] = centre[1];
        x.centre[2] = centre[2];
        char xn[48] = {};
        if (cachedName(record, xn, sizeof(xn))) std::snprintf(x.name, sizeof(x.name), "%s", xn);
        else x.name[0] = '\0';
    }
    // The own ship: the own candidate (the nearest small containing sphere), the prefix rule, then the live own sphere.
    const bool containsCam = sealed::containsCamera(centre, radius, cam);
    {
        const float dCam = sealed::distance(centre, cam);
        if (dCam <= kOwnNearM && g_nearCurN < kNearSlots) {
            char nn[48] = {};
            if (cachedName(record, nn, sizeof(nn))) {
                NearRec& nr = g_nearCur[g_nearCurN++];
                nr.addr = record;
                std::snprintf(nr.name, sizeof(nr.name), "%s", nn);
                for (int k = 0; k < 3; ++k) nr.centre[k] = centre[k];
                nr.radius = radius;
                nr.dist = dCam;
            }
        }
        if (containsCam && radius < sealed::kOwnShipRadiusM) {
            if (g_ownCandidateDist < 0.0f || dCam < g_ownCandidateDist) {
                char cn[48] = {};
                if (cachedName(record, cn, sizeof(cn))) {
                    std::snprintf(g_ownCandidateName, sizeof(g_ownCandidateName), "%s", cn);
                    g_ownCandidateDist = dCam;
                }
            }
        }
    if (sealed::distance(centre, cam) <= sealed::kShipRecordsM) {   // the ship's part count by prefix (built v14.8)
        char tn[48] = {};
        if (cachedName(record, tn, sizeof(tn))) tallyShipPrefix(tn);
    }
    latchCandNote(record, centre);   // built v14.12: gathered only while a latch is pending
    hangarNameNote(record, centre, radius);   // built v14.13: only while the names are wanted
    if (!judge) return;   // gathering only: the pads and the explicit parts are in hand; the judgement is sealed-only
    const sealed::NamedKeep v = sealed::keepNamedCell(centre, radius, cam, kFallbackSphereM, g_cellPrev, g_explicitPrev, g_explicitPrevCount, record);
    keep = v.keep;
    containing = v.containing;
    // The bay: the own hangar's parts inside the bay volume, even where the box alone would cull them. The shaft is the cell's.
    if (!keep && g_cellPrev.capsule) {
        float u[3] = {};
        if (sealed::unitBetween(g_cellPrev.capA, g_cellPrev.capB, u) &&
            sealed::inBayVolume(centre, g_cellPrev.capA, u, g_cellPrev.capLen, belowFor(g_cellPrev.size))) {
            char bn[48] = {};
            if (cachedName(record, bn, sizeof(bn)) && sealed::bayPartName(bn)) keep = true;
        }
    }
        if (!keep) {
            char nm[48] = {};
            if (g_ownPrefix[0] != '\0') cachedName(record, nm, sizeof(nm));
            if (sealed::ownShipExempt(containsCam, radius, nm, g_ownPrefix)) keep = true;
        }
        if (!keep && g_ownLiveValid && sealed::ownSphereKeeps(centre, g_ownLiveCentre, g_ownLiveRadius, kOwnSphereMarginM)) keep = true;
    }
    if (keep) {
        g_kept.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (cullCounted(record, containing)) ++g_winCulled;
    }
}

}  // namespace

// ---- The engine's entry points (declared in optimized_station.h) -------------------------------------------------------------------------

// Once, at install: reads fix.optimized_station (on in VR, off in flat; the flat profile does not read it).
void optimizedStationInstall(bool flatProfile) {
    const std::string text = Config::get().getString("fix.optimized_station", flatProfile ? "off" : "on");
    const bool on = text != "off";
    g_setting.store(on, std::memory_order_release);
    g_summaryStartMs = GetTickCount64();
    if (on) applySetting(true, 0);
    Log::get().note("optimized station: fix.optimized_station = %s (%s).", on ? "on" : "off",
                    flatProfile ? "the flat profile: off unless set" : "VR profile");
}

bool optimizedStationOn() noexcept { return g_setting.load(std::memory_order_acquire); }
bool optimizedStationGathers() noexcept { return g_setting.load(std::memory_order_acquire) && g_dockedNow.load(std::memory_order_acquire); }
bool optimizedStationSealed() noexcept { return g_sealedFlag.load(std::memory_order_acquire); }

// The draw path asks: while sealed, the sun pair is not drawn.
bool optimizedStationSkipsDraw(uint64_t vs, uint64_t ps) noexcept {
    return g_sunOff.load(std::memory_order_acquire) && vs == kSunVs && ps == kSunPs;   // the sun skip follows the pad, not the seal (built v14.13)
}

// The store calls this after it has written a record (bucket = the collection, record = its record). While sealed it judges the record; while
// docked and unsealed it only gathers the frame's pads, explicit parts and camera for the seal trigger (built v14.1).
void optimizedStationAfterStore(uintptr_t bucket, uintptr_t record) noexcept {
    const sealed::StoreMode mode = sealed::storeMode(g_setting.load(std::memory_order_acquire), g_sealedFlag.load(std::memory_order_acquire),
                                                     g_dockedNow.load(std::memory_order_acquire));
    if (mode == sealed::StoreMode::kNone) return;
    afterStoreJudge(bucket, record, mode == sealed::StoreMode::kJudge);
}

// The render thread's frame tick: the setting, the held cell in the pad's frame, the seal and release rules, the rig verdicts, the summary.
void optimizedStationFrame(uint32_t frame) {
    {
        // Built v14.17: the dock's arrival is read before the setting gate, so a toggle while undocked never misses the undock (DockWatch).
        std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
        // Built v14.21: an undocked reading counts for the arrival only in a vehicle (Flags bits 24-26): never the menu or the load.
        g_dock.step(journalDockedKnown(), journalDocked(), journalLoadGames(), journalUndocks(), journalVehicleKnown() && journalVehicle());
    }
    if (++g_settingPoll >= kSettingPollFrames) {
        g_settingPoll = 0;
        const std::string text = Config::get().getString("fix.optimized_station", "on");
        const bool on = text != "off";
        if (on != g_setting.load(std::memory_order_acquire)) {
            g_setting.store(on, std::memory_order_release);
            applySetting(on, frame);
        }
    }
    const uint64_t now = GetTickCount64();
    if (!g_setting.load(std::memory_order_acquire)) {
        if (g_summaryStartMs == 0) g_summaryStartMs = now;
        return;
    }
    if (g_summaryStartMs == 0) g_summaryStartMs = now;
    // Built v14.2: the frame advances here, once per tick. The observer's promote and view resolve run once per frame off this stamp.
    g_frameStamp.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    g_recordsLast = g_recordsFrame;
    g_recordsFrame = 0;
    ++g_winFrames;
    if (g_heldValid) ++g_winSealedFrames;
    g_dockedKnownNow.store(journalDockedKnown(), std::memory_order_release);
    g_dockedNow.store(journalDockedKnown() && journalDocked(), std::memory_order_release);
    // Built v14.20 (review 4): an undock is a lifecycle point: the rig and collection tables start afresh.
    if (g_dockedWasNow && !g_dockedNow.load(std::memory_order_acquire)) bumpRigEpoch();
    g_dockedWasNow = g_dockedNow.load(std::memory_order_acquire);
    storeHookGate();   // built v14.20 (review 7b): the store observer gathers only while docked (an atomic gate: no hook is installed here)
    padStateTick(now);   // built v14.22: the pad-state probe's lines (temporary, log only)
    refreshHeldWorld();
    // The camera against the held box: outside it by more than kCameraLeaveM releases the seal.
    bool cameraOutside = false;
    if (g_heldValid && g_frameCamValid) {
        if (g_heldCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_heldCell, g_frameCam, x);
            cameraOutside = !(std::fabs(x[0]) <= g_heldCell.boxHalf[0] + sealed::kCameraLeaveM &&
                              std::fabs(x[1]) <= g_heldCell.boxHalf[1] + sealed::kCameraLeaveM &&
                              std::fabs(x[2]) <= g_heldCell.boxHalf[2] + sealed::kCameraLeaveM);
        } else {
            cameraOutside = sealed::distance(g_heldCell.centre, g_frameCam) > g_heldCell.radius + sealed::kCameraLeaveM;
        }
    }
    // Built v14.6: a ship change (the own ship's type prefix, from the candidate record, changes for kShipChangeTicks readings in a row). It
    // releases the seal, resets the trigger and the safety net, and rebuilds the own-ship rule: a swapped ship in a closed bay then seals at once.
    if (g_ownCandidateDist >= 0.0f) {
        char curPrefix[32] = {};
        sealed::ownPrefixOf(g_ownCandidateName, curPrefix, sizeof(curPrefix));
        char oldPrefix[32] = {};
        std::snprintf(oldPrefix, sizeof(oldPrefix), "%s", g_shipWatch.prefix);
        if (sealed::shipPrefixNamesShip(curPrefix, prefixTallyOf(curPrefix)) && g_shipWatch.step(curPrefix)) {
            char why[160];
            std::snprintf(why, sizeof(why), "ship changed (%s -> %s)", oldPrefix, curPrefix);
            if (g_heldValid) {
                releaseSeal(why, frame);
            } else {
                Log::get().note("optimized station: %s; the trigger starts afresh", why);
            }
            g_sealState = sealed::SealState{};
            g_sealGuard = sealed::SealRateGuard{};
            g_heightSrc = sealed::ShellHeightSources{};
            g_preShellValid = false;
            setOwnPrefixAtEngage(frame);
            g_ownRecAddr = 0;
            g_ownLiveValid = false;
            g_latch.reset();   // a new ship: the mechanism is latched again
            g_padStill = sealed::PadStillWatch{};
            bumpRigEpoch();    // built v14.20 (review 4): a ship change is a lifecycle point
        }
    }
    // Built v14.5: the height above the shell along the pad's own up axis, read live, with the shell picked once per pad (the same pick as the
    // freeze, from the gathered parts). The camera in the candidate box is the seal's other condition.
    // Built v14.9: a pad absent for fewer than kHeldLostFrames frames keeps its last reading (no still-clock reset).
    const bool padHeld = !g_pickCell.has && g_ownSticky.pad != 0 && g_ownSticky.unseen > 0 && g_ownSticky.unseen < sealed::kHeldLostFrames;
    if (!padHeld) {
        g_relOk = false;
        g_shellNowValid = false;
    }
    g_heightChanged = false;
    g_frameShell = sealed::ShellFrame{};   // built v14.17: the pad in the shell's frame, read when the height is
    if (g_pickCell.has) {
        g_relOk = false;
        if (!g_hangarWatch.giveUp && (!g_preShellValid || g_preShellPad != g_pickCell.pad)) {
            const size_t pick = sealed::pickShell(g_explicits, g_explicitCount, g_pickCell.centre, g_pickCell.size, 150.0f);
            g_preShellValid = pick < g_explicitCount;
            g_preShellPad = g_pickCell.pad;
            if (g_preShellValid) {
                g_preShellAddr = g_explicits[pick].addr;
                std::snprintf(g_preShellName, sizeof(g_preShellName), "%s", g_explicits[pick].name);
            }
        }
        if (g_preShellValid) {
            char nm[48] = {};
            if (!cachedName(g_preShellAddr, nm, sizeof(nm)) || std::strcmp(nm, g_preShellName) != 0) {
                g_preShellValid = false;   // the record moved on: the next tick picks again
            } else {
                const WorldRead pw = worldOfRecord(g_pickCell.pad);
                const WorldRead sw = worldOfRecord(g_preShellAddr);
                float axes[3][3] = {};
                float ext[3] = {};
                bool extOk = false;
                uint8_t src = 4;
                if (pw.source != 4 && sw.source != 4 && poseOrientOf(g_pickCell.pad, axes, ext, &extOk, &src)) {
                    const float up[3] = {axes[2][0], axes[2][1], axes[2][2]};
                    g_relM = sealed::shellRelative(pw.world, sw.world, up);
                    g_relOk = true;
                    g_shellNow[0] = sw.world[0];
                    g_shellNow[1] = sw.world[1];
                    g_shellNow[2] = sw.world[2];
                    g_shellNowSrc = sw.source;
                    g_shellNowValid = true;
                    g_heightChanged = g_heightSrc.note(pw.source, sw.source);
                    // Built v14.17: the pad's turn and the shell's change (the still clock), and the pad in the shell's frame (the instrument).
                    float shAxes[3][3] = {};
                    float shExt[3] = {};
                    bool shExtOk = false;
                    uint8_t shSrc = 4;
                    if (poseOrientOf(g_preShellAddr, shAxes, shExt, &shExtOk, &shSrc)) {
                        g_frameShell = sealed::shellFrameOf(pw.world, sw.world, up, shAxes[0], axes[0]);
                    }
                    // Built v14.20 (review 6): the turn is the pad's yaw against its SHELL. A rotation the pad and its shell share (the station turning)
                    // is no turn; an unreadable yaw gives no turn signal (the world axis is never used).
                    g_padStill.step(g_pickCell.pad, g_frameShell.yawOk, g_frameShell.yawDeg, pw.source, g_preShellAddr, now);
                }
            }
        }
    }
    bool camIn = false;
    if (g_frameCamValid && g_pickCell.has) {
        if (g_pickCell.box) {
            float x[3] = {};
            sealed::boxCoordsOf(g_pickCell, g_frameCam, x);
            camIn = std::fabs(x[0]) <= g_pickCell.boxHalf[0] && std::fabs(x[1]) <= g_pickCell.boxHalf[1] && std::fabs(x[2]) <= g_pickCell.boxHalf[2];
        } else {
            camIn = sealed::distance(g_pickCell.centre, g_frameCam) <= g_pickCell.radius;
        }
    }
    // Built v14.12: the latch is kept while docked, unsealed, after a descent; the candidates of this frame are consumed here.
    g_watchNowMs = now;
    g_bayWatching = bayWatchKeep();
    g_bayNoting = g_bayWatching && g_shellNowValid;
    if (!g_bayWatching) {
        g_latch.reset();
    }
    // Built v14.20 (review 2): a genuine change of the unsealed bay's pad or shell clears the latch and its motion references; it is regathered below.
    // A sample that misses the pad keeps the identity (a brief miss is no change).
    if (g_latch.latched && !g_sealedFlag.load(std::memory_order_acquire) &&
        g_latchId.step(g_pickCell.has, g_pickCell.pad, g_preShellValid, g_preShellAddr)) {
        g_latch.reset();
        Log::get().note("optimized station: the bay changed (pad %s): its mechanism is latched again", g_pickCell.padName);
    }
    g_latchPending = g_bayWatching && !g_latch.latched;
    if (g_latchPending && g_pickCell.has) latchNow(now);
    g_latchCandN = 0;
    if (g_latch.latched && g_bayNoting) latchRead(now);
    // Built v14.14: a latched mechanism record moved on two frames in a row while sealed: the launch releases the seal.
    g_mechanismMoved = false;
    if (g_sealedFlag.load(std::memory_order_acquire)) {
        const int mi = g_latch.movedIndex();
        if (mi >= 0) {
            if (mi == 0) std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "%s", "the pad started moving: launch");
            else std::snprintf(g_releaseWhy, sizeof(g_releaseWhy), "the bay mechanism moved (%s): launch", g_latch.recs[mi].name);
            g_mechanismMoved = true;
        }
    }
    // Built v14.13: the hangar watch (a sized shell near the pad) and its once-per-dock names; the sun inside the hangar (its own state).
    const bool shellFound = g_preShellValid;
    const bool padChanged = g_pickCell.has && g_pickCell.pad != g_hangarPad;
    if (g_pickCell.has) {
        g_hangarPad = g_pickCell.pad;
        for (int k = 0; k < 3; ++k) g_namesPadC[k] = g_pickCell.centre[k];
    }
    if (g_hangarWatch.step(shellFound, g_dockedNow.load(std::memory_order_acquire), padChanged)) hangarNote();
    if (!g_dockedNow.load(std::memory_order_acquire)) g_nameLogN = 0;
    g_namesPending = g_dockedNow.load(std::memory_order_acquire) && !shellFound && g_pickCell.has && !g_hangarWatch.logged;
    {
        sealed::SunInputs su;
        su.setting = g_setting.load(std::memory_order_acquire);
        su.docked = g_dockedNow.load(std::memory_order_acquire);
        su.hangar = shellFound;
        su.relOk = g_relOk;
        su.rel = g_relM;
        su.hangarChanged = g_flagHangarChanged;
        su.holdForSeal = g_dock.arrived && !g_dock.sealedThisDock;   // built v14.17: an arrival's sun waits for its first seal
        su.descentPending = g_dock.arrived && !g_sealState.dockDescended;   // built v14.19: and the arrival's skip waits for its descent
        const char* why = "";
        if (sealed::sunStep(su, &g_sun, &why)) {
            g_sunOff.store(g_sun.off, std::memory_order_release);
            // Built v14.20 (review 8): the Status docked flag is on both lines, so the sun transition is read against Elite's own docked state.
            const char* dockedText = g_dockedNow.load(std::memory_order_acquire) ? "yes" : "no";
            if (g_sun.off) Log::get().note("optimized station: sun light inside the hangar off (pad lowered; Status docked=%s)", dockedText);
            else Log::get().note("optimized station: sun light inside the hangar back on (%s; Status docked=%s)", why, dockedText);
        }
    }
    sealed::SealInputs in;
    in.setting = true;   // the tick returns above while the setting is off
    in.docked = g_dockedNow;
    in.arrived = g_dock.arrived;   // built v14.17: an arrived dock expects its descent (no load-in rule)
    in.shellAddr = g_preShellValid ? g_preShellAddr : 0;   // built v14.19: the dock reference is per shell record
    in.lateral = g_frameShell.lateral;                     // built v14.19: the pad's lateral from the shell's shaft (the reference's second measure)
    in.latOk = g_frameShell.ok;
    in.pickOk = g_pickCell.has && g_preShellValid;   // no sized shell: the trigger stays idle (built v14.13)
    in.padHeld = padHeld;
    in.mechanismMoved = g_mechanismMoved;
    in.bayLastMotionMs = g_latch.lastMotionMs;
    in.padLastMotionMs = padLastMotion();
    in.rel = g_relM;
    in.relOk = g_relOk;
    in.sourceChanged = g_heightChanged;
    in.cameraInBox = camIn;
    in.pad = g_pickCell.has ? g_pickCell.pad : 0;
    in.held = g_heldValid;
    in.cameraOutside = cameraOutside;
    in.padMoved = g_flagPadMoved;
    if (g_flagHangarChanged) g_sealGuard = sealed::SealRateGuard{};   // a new hangar: the safety net starts afresh
    if (g_flagHangarChanged) {   // a new hangar: the trigger and its memory start afresh (built v14.5)
        g_sealState = sealed::SealState{};
        g_heightSrc = sealed::ShellHeightSources{};
        g_preShellValid = false;
        g_latch.reset();
        g_padStill = sealed::PadStillWatch{};
        bumpRigEpoch();   // built v14.20 (review 4): a hangar change is a lifecycle point
    }
    in.hangarChanged = g_flagHangarChanged;
    in.launched = false;   // the pad state object's reader is not in this build (optimized-station doc, "Not yet")
    in.nowMs = now;
    sealed::ReleaseWhy why = sealed::ReleaseWhy::kNone;
    const sealed::SealAction act = sealed::sealStep(in, &g_sealState, &why);
    g_flagPadMoved = false;
    g_flagHangarChanged = false;
    {
        // Built v14.20 (review 1): the trigger withdrew its request (the pad rose, the camera left, the pad changed): the pending engage is cancelled.
        sealed::EngageRequest er{g_engagePending, g_engagePad, g_engageShell};
        sealed::engageTrigger(&er, g_sealState);
        g_engagePending = er.pending;
    }
    if (act == sealed::SealAction::kRelease) {
        releaseSeal(releaseText(why), frame);
    } else if (act == sealed::SealAction::kRequestEngage) {
        if (g_sealGuard.tripped) {
            // refused until a hangar change or a setting toggle (the safety net, built v14.4)
        } else if (g_sealGuard.note(now)) {
            Log::get().note("optimized station: sealing stopped for this hangar: more than %u seals in a minute (the pad flickers). "
                            "It resumes at a hangar change or a setting toggle.", static_cast<unsigned>(sealed::kSealRateMax));
        } else {
            g_engagePending = true;
            g_engagePad = in.pad;          // built v14.20 (review 1): bound to the pad and the shell this request was made for
            g_engageShell = in.shellAddr;
            g_engageTries = 0;
            g_engageAtLoadIn = g_sealState.immediate;
            captureEngageInfo(now);
        }
    }
    // Built v14.17 (temporary instrument): the pad's track while an arrived dock waits for its first seal.
    if (g_dockedNow.load(std::memory_order_acquire) && g_dock.arrived && !g_dock.sealedThisDock && !g_heldValid) padTrackLine(now);
    // Built v14.1: while docked and unsealed, one line when the seal's inputs change, and at most every 10 s while docked: the unmet condition.
    if (!g_heldValid) {
        sealed::WaitView w;
        w.dockedKnown = g_dockedKnownNow.load(std::memory_order_acquire);
        w.docked = g_dockedNow.load(std::memory_order_acquire);
        w.havePad = g_pickCell.has;
        if (g_pickCell.has) std::snprintf(w.padName, sizeof(w.padName), "%s", g_pickCell.padName);
        w.lowered = g_relOk && g_relM < sealed::kLoweredM;
        w.relM = g_relM;
        w.relOk = g_relOk;
        {
            const uint64_t quietSince = std::max(std::max(g_sealState.lowerSinceMs, g_latch.lastMotionMs), padLastMotion());
            w.bayStillS = (g_sealState.lowerSet && now > quietSince) ? static_cast<float>(now - quietSince) / 1000.0f : 0.0f;
            g_latch.movers(now, w.movers, sizeof(w.movers), 3);
        }
        w.cellBuilt = g_relOk && sealed::cellInvalidWhy(g_pickCell) == nullptr;
        w.gathered = g_recordsLast;
        w.descent = g_sealState.moved;
        w.cameraKnown = g_frameCamValid;
        w.padCount = static_cast<uint32_t>(g_padCount);
        for (size_t i = 0; i < g_padCount && i < 3; ++i) {
            const sealed::PadObs& p = g_pads[i];
            sealed::WaitView::PadEvidence& ev = w.padEv[i];
            std::snprintf(ev.name, sizeof(ev.name), "%s", p.name);
            ev.d = g_frameCamValid ? sealed::distance(p.centre, g_frameCam) : -1.0f;
            ev.contains = g_frameCamValid && ev.d <= p.radius;
        }
        if (!w.cellBuilt) {
            if (!g_preShellValid) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_hangarWhy);
            else if (!g_relOk) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", "no shell height (the pad or the shell pose is unreadable)");
            else if (!g_pickCell.has) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_pickCell.reason);
            else std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", sealed::cellInvalidWhy(g_pickCell));
        }
        const bool changed = !g_waitLastSet || sealed::waitingKeyChanged(g_waitLast, w);
        const bool periodic = w.docked && g_waitLastSet && now - g_waitLastMs >= 10000;
        if (changed || periodic) {
            char line[640] = {};
            sealed::waitingLine(w, static_cast<float>(sealed::kBayStillMs) / 1000.0f, line, sizeof(line));
            Log::get().note("%s", line);
            g_waitLast = w;
            g_waitLastSet = true;
            g_waitLastMs = now;
        }
    }
    refreshRigVerdicts();
    if (now - g_summaryStartMs >= kSummaryMs) summaryLine(now);
    (void)frame;
}

}  // namespace edvr
