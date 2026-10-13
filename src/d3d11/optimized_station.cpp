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
#include "explorer_cam.h"        // explorerCamSessionActive(): Explorer Cam on releases the station and stops its sealing
#include "journal_watch.h"
#include "kinematic_eval_hook.h"
#include "optimized_station_core.h"
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
bool g_engagePending = false;
uint32_t g_engageTries = 0;
float g_padAlong = 0.0f, g_padLowest = 0.0f;
bool g_padAlongValid = false, g_padLowestSet = false;
uintptr_t g_lowestPad = 0;
char g_ownPrefix[32] = {};
char g_ownCandidateName[48] = {};
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

void prodSync() noexcept {
    const uint32_t s = g_frameStamp.load(std::memory_order_relaxed);
    if (s != g_prodStampSeen) {
        g_prodStampSeen = s;
        ++g_prodGen;
    }
}

size_t prodIndex(uintptr_t rec) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rec) >> 4) * 0x9E3779B97F4A7C15ull >> 51) & (kProdSlots - 1);
}

void prodPut(uintptr_t rec, const float c[3], float r) noexcept {
    size_t i = prodIndex(rec);
    for (size_t n = 0; n < kProdSlots; ++n, i = (i + 1) & (kProdSlots - 1)) {
        ProdSlot& s = g_prodSlots[i];
        if (s.gen != g_prodGen || s.rec == rec) {
            s.rec = rec;
            s.gen = g_prodGen;
            s.c[0] = c[0];
            s.c[1] = c[1];
            s.c[2] = c[2];
            s.r = r;
            return;
        }
    }
}

bool prodGet(uintptr_t rec, float c[3], float* r) noexcept {
    size_t i = prodIndex(rec);
    for (size_t n = 0; n < kProdSlots; ++n, i = (i + 1) & (kProdSlots - 1)) {
        const ProdSlot& s = g_prodSlots[i];
        if (s.gen != g_prodGen) return false;
        if (s.rec == rec) {
            c[0] = s.c[0];
            c[1] = s.c[1];
            c[2] = s.c[2];
            *r = s.r;
            return true;
        }
    }
    return false;
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

// Built v13 (fix 1): at each named engage, the own ship's type prefix from the candidate record (the one nearest the camera that contains it), logged.
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

struct CollSlot {
    uintptr_t coll = 0;
    uintptr_t rig = 0;
};

CollSlot g_collSlots[kCollSlots];

size_t collIndex(uintptr_t coll) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(coll) >> 4) * 0x9E3779B97F4A7C15ull >> 54) & (kCollSlots - 1);
}

void collInsert(uintptr_t coll, uintptr_t rig) noexcept {
    size_t i = collIndex(coll);
    for (size_t n = 0; n < kCollSlots; ++n, i = (i + 1) & (kCollSlots - 1)) {
        CollSlot& s = g_collSlots[i];
        if (s.coll == coll || s.coll == 0) {
            s.coll = coll;
            s.rig = rig;
            return;
        }
    }
}

uintptr_t collFind(uintptr_t coll) noexcept {
    size_t i = collIndex(coll);
    for (size_t n = 0; n < kCollSlots; ++n, i = (i + 1) & (kCollSlots - 1)) {
        const CollSlot& s = g_collSlots[i];
        if (s.coll == coll) return s.rig;
        if (s.coll == 0) return 0;
    }
    return 0;
}

// The job path's one-entry cache per frame: the last store bucket and the rig it resolved to (a bucket's records come in a run).
uintptr_t g_rigBucketCache = 0, g_rigBucketRig = 0;

uint32_t g_rigBucketStamp = 0;

size_t g_rigSlotsUsed = 0;

uint64_t g_rigNamesLogged = 0;

size_t rigIndex(uintptr_t rig) noexcept {
    return static_cast<size_t>((static_cast<uint64_t>(rig) >> 4) * 0x9E3779B97F4A7C15ull >> 52) & (kRigSlots - 1);
}

RigSlot* rigFind(uintptr_t rig, bool insert) noexcept {
    size_t i = rigIndex(rig);
    for (size_t n = 0; n < kRigSlots; ++n, i = (i + 1) & (kRigSlots - 1)) {
        RigSlot& s = g_rigSlots[i];
        if (s.rig == rig) return &s;
        if (s.rig == 0) {
            if (!insert) return nullptr;
            s.rig = rig;
            ++g_rigSlotsUsed;
            return &s;
        }
    }
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
    const bool allMode = false;
    for (RigSlot& s : g_rigSlots) {
        if (s.rig == 0) continue;
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
bool sealedTrialRigVeto(uintptr_t rig, uintptr_t ctx) noexcept {
    (void)ctx;
    if (!g_heldValid || !rig) return false;
    std::lock_guard<std::recursive_mutex> lock(g_sealedMutex);
    const uint32_t stamp = g_frameStamp.load(std::memory_order_relaxed);
    ++g_rigTally.seen;
    RigSlot* s = nullptr;
    if (rig == g_rigCacheRig && g_rigCacheStamp == stamp && g_rigCacheSlot && g_rigCacheSlot->rig == rig) {
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
            s->coll = coll;
            collInsert(coll, rig);
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
std::atomic<bool> g_sealedFlag{false};     // mirror of g_heldValid for the draw path and the hooks (atomic: read on the render thread)
bool g_vetoesAttached = false;             // the producer and rig vetoes are attached (under g_installMutex-free single writer: the render tick)
bool g_storeWanted = false;                // the store observer is wanted (the setting is on)
std::atomic<bool> g_dockedNow{false};     // Status.json Flags bit 0 (Docked), read at each tick while known (the store thread reads it)
std::atomic<bool> g_dockedKnownNow{false};// Status.json has a Flags sample (the waiting line says unknown until then)
sealed::WaitView g_waitLast;              // the waiting line last written (built v14.1)
bool g_waitLastSet = false;
uint64_t g_waitLastMs = 0;                // when the waiting line was last written (at most every 10 s while docked)
sealed::SealState g_sealState;             // the seal trigger (optimized_station_core.h sealStep)
bool g_flagHangarChanged = false;          // promote saw the held pad go or another own pad nearest: the tick releases on it
bool g_engageAtLoadIn = false;             // the seal in force was requested at load-in (built v14.3): the status line says so
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
        case sealed::ReleaseWhy::kExplorerCam: return "Explorer Cam is on (the station does not seal with it)";
        case sealed::ReleaseWhy::kCameraLeft: return "the camera left the bay by more than 2 m (riding out of the hangar, or an external camera)";
        case sealed::ReleaseWhy::kPadMoved: return g_releaseWhy;
        case sealed::ReleaseWhy::kHangarChanged: return g_releaseWhy;
        case sealed::ReleaseWhy::kLaunch: return "launch (the pad's state object flipped)";
        default: return "released";
    }
}

void releaseSeal(const char* why, uint32_t frame) noexcept {
    if (!g_heldValid) return;
    Log::get().note("optimized station: released (frame %u): %s. Everything is drawn as without the station until the seal conditions hold again.",
                    frame, why);
    g_heldValid = false;
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
}

// The seal's status line, once per seal (called from the engage, after the cell is held and localized).
void sealStatus(const sealed::OwnCell& cell, uint32_t frame) noexcept {
    ++g_sealCount;
    Log::get().note("optimized station: sealed in %s hangar (%s, frame %u, seal %u): pad %s, box %.0f x %.0f x %.0f m (length, width, height), "
                    "floor %.0f m under the pad, shell %s.", sizeWord(cell.size), g_engageAtLoadIn ? "sealed at load-in" : "sealed after the descent settled",
                    frame, g_sealCount, cell.padName,
                    static_cast<double>(cell.boxHalf[0] * 2.0f), static_cast<double>(cell.boxHalf[1] * 2.0f),
                    static_cast<double>(cell.boxHalf[2] * 2.0f), static_cast<double>(belowFor(cell.size)),
                    g_shellValid ? g_shellName : "none (the pad's motion is not checked)");
}

// The 30 s summary: what the station did in the window. A user's log shows the station's work here.
void summaryLine(uint64_t now) noexcept {
    Log::get().note("optimized station: last %llu s: sealed for %llu of %llu frames; rigs vetoed early %llu, records vetoed early %llu, "
                    "records culled after the store %llu, records kept unread %llu, faults %llu.",
                    static_cast<unsigned long long>((now - g_summaryStartMs) / 1000), static_cast<unsigned long long>(g_winSealedFrames),
                    static_cast<unsigned long long>(g_winFrames), static_cast<unsigned long long>(g_winRigVetoes),
                    static_cast<unsigned long long>(g_winProducerVetoes), static_cast<unsigned long long>(g_winCulled),
                    static_cast<unsigned long long>(g_winKeptUnknown), static_cast<unsigned long long>(g_winFaults));
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
void applySetting(bool on, uint32_t frame) noexcept {
    if (on == g_vetoesAttached) return;
    g_sealState = sealed::SealState{};   // a setting toggle starts the trigger and its memory afresh (built v14.4)
    g_sealGuard = sealed::SealRateGuard{};
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
        kinematicEvalSetDirectVeto(nullptr);
        kinematicEvalSetRigVeto(nullptr);
        kinematicEvalDirectVetoDetach();
        kinematicEvalRigVetoDetach();
        g_storeWanted = false;
        storeHookRecompute();
        releaseSeal("setting off", frame);
        Log::get().note("optimized station: off (frame %u).", frame);
    }
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
            g_padAlongValid = false;
            if (haveAxes) {
                for (int k = 0; k < 3; ++k) e2[k] = axes[2][k];
                g_padAlong = next.centre[0] * e2[0] + next.centre[1] * e2[1] + next.centre[2] * e2[2];
                g_padAlongValid = true;
                if (!g_padLowestSet || g_lowestPad != next.pad) {
                    g_padLowest = g_padAlong;
                    g_padLowestSet = true;
                    g_lowestPad = next.pad;
                } else if (g_padAlong < g_padLowest) {
                    g_padLowest = g_padAlong;
                }
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
        if (next.has && next.radius > 0.0f && g_padAlongValid) {
            setOwnPrefixAtEngage(frame);
            pickOwnRecord(frame);
            g_heldCell = next;
            g_heldValid = true;
            setHeldFrame(g_heldCell, frame);
            g_engagePending = false;
            g_sealedFlag.store(true, std::memory_order_release);
            sealStatus(g_heldCell, frame);
        } else if (g_engageTries >= kEngageTries) {
            g_engagePending = false;
            Log::get().note("optimized station: no cell could be built at the seal (%u promotes; last: %s); the seal waits for the pad to move "
                            "and settle again.", g_engageTries, next.reason);
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
    if (!g_viewReady || (!g_view.poseOk && g_viewTries < kViewTries)) {
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
    } else if (cls == sealed::NameClass::kExplicit && g_explicitCount < sealed::kExplicitSlots) {
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
bool optimizedStationSealed() noexcept { return g_sealedFlag.load(std::memory_order_acquire); }

// The draw path asks: while sealed, the sun pair is not drawn.
bool optimizedStationSkipsDraw(uint64_t vs, uint64_t ps) noexcept {
    return g_sealedFlag.load(std::memory_order_acquire) && vs == kSunVs && ps == kSunPs;
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
    sealed::SealInputs in;
    in.setting = true;   // the tick returns above while the setting is off
    in.docked = g_dockedNow;
    in.explorerOn = explorerCamSessionActive();
    in.pickOk = g_pickCell.has && g_padAlongValid;
    in.along = g_padAlong;
    in.lowest = g_padLowest;
    in.pad = g_pickCell.has ? g_pickCell.pad : 0;
    in.held = g_heldValid;
    in.cameraOutside = cameraOutside;
    in.padMoved = g_flagPadMoved;
    if (g_flagHangarChanged) g_sealGuard = sealed::SealRateGuard{};   // a new hangar: the safety net starts afresh
    in.hangarChanged = g_flagHangarChanged;
    in.launched = false;   // the pad state object's reader is not in this build (optimized-station doc, "Not yet")
    in.nowMs = now;
    sealed::ReleaseWhy why = sealed::ReleaseWhy::kNone;
    const sealed::SealAction act = sealed::sealStep(in, &g_sealState, &why);
    g_flagPadMoved = false;
    g_flagHangarChanged = false;
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
            g_engageTries = 0;
            g_engageAtLoadIn = g_sealState.immediate;
        }
    }
    // Built v14.1: while docked and unsealed, one line when the seal's inputs change, and at most every 10 s while docked: the unmet condition.
    if (!g_heldValid) {
        sealed::WaitView w;
        w.dockedKnown = g_dockedKnownNow.load(std::memory_order_acquire);
        w.docked = g_dockedNow.load(std::memory_order_acquire);
        w.havePad = g_pickCell.has;
        if (g_pickCell.has) std::snprintf(w.padName, sizeof(w.padName), "%s", g_pickCell.padName);
        w.atLowest = g_padAlongValid && (g_padAlong - g_padLowest) <= sealed::kSealStillM;
        w.stillS = g_sealState.stillValid ? static_cast<float>(now - g_sealState.stillSinceMs) / 1000.0f : 0.0f;
        w.cellBuilt = g_pickCell.has && g_pickCell.radius > 0.0f && g_padAlongValid;
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
            if (!g_pickCell.has) std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", g_pickCell.reason);
            else std::snprintf(w.cellReason, sizeof(w.cellReason), "%s", "no pad height (the pad's pose is unreadable)");
        }
        const bool changed = !g_waitLastSet || sealed::waitingKeyChanged(g_waitLast, w);
        const bool periodic = w.docked && g_waitLastSet && now - g_waitLastMs >= 10000;
        if (changed || periodic) {
            char line[640] = {};
            sealed::waitingLine(w, static_cast<float>(sealed::kSealDelayMs) / 1000.0f, line, sizeof(line));
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
