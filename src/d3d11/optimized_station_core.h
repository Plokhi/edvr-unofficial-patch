#pragma once
// The optimized station's pure half (optimized_station.cpp). No Windows and no engine memory: the record rules of the sealed hangar, the own pad's
// cell (its centre, its shaft to the hangar's BlastShieldBase, its oriented box), the cell held in the pad's own frame, the release rules, the
// rig verdicts of the early veto, and the fault-safe cull. tools\optimized_station_test compiles it alone.
//
// The rule (named mode): a record is kept when its centre lies in the sealed cell (the box along the shaft, or the pad's sphere before a shaft is
// found), when it is an explicit part of the hangar nearest the shaft, when it is an own-ship part, or when its centre cannot be read. Everything
// else is culled from the views. A record whose centre cannot be read is kept, never judged (rawCentreUsable, optimized_station.cpp).
#include <cctype>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace edvr::sealed {

// The builder's transformed centre: out = R(q) * c + t (the same expression as decomp_42B4420.txt:196-237). The identity quaternion gives c + t.
inline void builderCentre(const float q[4], const float t[4], const float c[4], float out[4]) noexcept {
    const float w2 = q[3] + q[3];
    const float dot = q[0] * c[0] + q[1] * c[1] + q[2] * c[2];
    const float ww = q[3] * q[3] + q[3] * q[3];
    out[0] = (c[2] * q[1] - c[1] * q[2]) * w2 + ww * c[0] + (-c[0]) + dot * (q[0] + q[0]) + t[0];
    out[1] = (c[0] * q[2] - c[2] * q[0]) * w2 + ww * c[1] + (-c[1]) + dot * (q[1] + q[1]) + t[1];
    out[2] = (c[1] * q[0] - c[0] * q[1]) * w2 + ww * c[2] + (-c[2]) + dot * (q[2] + q[2]) + t[2];
    out[3] = (c[3] * q[3] - c[3] * q[3]) * w2 + ww * c[3] + (-c[3]) + dot * w2 + t[3];
}

// Where a record's centre came from: the pose (the builder's), the raw rec+0x240 (the pose was unreadable: a counted fallback), or none.
enum class CentreSource : uint8_t { kPose = 0, kRaw = 1, kNone = 2 };

inline bool finite3(const float v[3]) noexcept { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// The record's centre from what was read: the pose centre when it is readable and finite; else rec+0x240 when that is; else none.
inline CentreSource chooseCentre(bool poseOk, const float pose[3], bool rawOk, const float raw[3], float out[3]) noexcept {
    if (poseOk && pose && finite3(pose)) {
        out[0] = pose[0];
        out[1] = pose[1];
        out[2] = pose[2];
        return CentreSource::kPose;
    }
    if (rawOk && raw && finite3(raw)) {
        out[0] = raw[0];
        out[1] = raw[1];
        out[2] = raw[2];
        return CentreSource::kRaw;
    }
    out[0] = out[1] = out[2] = 0.0f;
    return CentreSource::kNone;
}

// rec+0x240 is exactly the origin: the value the flight showed for dozens of distinct records.
inline bool isZeroCentre(const float c[3]) noexcept { return c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f; }

// Named mode's cell (Built v8.3b; Built v11 capsule). The radii are GUESSES until measured on the flight: 30 m for a small pad (Built v11: the
// capsule's radius, the addendum's guess; was 60), 90 m medium, 130 m large. The ladder is what hotkey.sealed_cell_larger and
// hotkey.sealed_cell_smaller (PAGEUP and PAGEDOWN) step the CURRENT size's radius through, ascending (Built v11: 15 to 60 m).
constexpr float kCellDefaultM[3] = {30.0f, 90.0f, 130.0f};

constexpr float kPadFallbackM = 150.0f;      // no pad sphere contains the camera: the nearest pad centre within this

constexpr float kExplicitM = 200.0f;         // an explicit part's nearest instance to the cell centre must be within this (no capsule)

constexpr float kExplicitCapsuleM = 15.0f;   // Built v11: with a capsule, an explicit part's nearest instance must be within this of the axis

constexpr size_t kPadSlots = 32;             // pad records a frame keeps for the pick

constexpr size_t kExplicitSlots = 128;       // explicit-part records a frame keeps for the nearest-instance pick

constexpr size_t kExplicitNodes = 64;        // distinct part nodes the nearest-instance pick tracks

inline float distance(const float a[3], const float b[3]) noexcept {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The record's sphere contains the camera.
inline bool containsCamera(const float centre[3], float radius, const float camera[3]) noexcept {
    return distance(centre, camera) <= radius;
}

// The record's sphere intersects the sphere of radius R around the camera.
inline bool intersectsCameraSphere(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return distance(centre, camera) <= radius + R;
}

// The sphere rule: keep a record that contains the camera or intersects the camera's sphere.
inline bool keepRecord(const float centre[3], float radius, const float camera[3], float R) noexcept {
    return containsCamera(centre, radius, camera) || intersectsCameraSphere(centre, radius, camera, R);
}

// Case-insensitive prefix test on ASCII text.
inline bool hasPrefixNoCase(const char* text, const char* word) noexcept {
    for (size_t k = 0; word[k]; ++k) {
        if (!text[k] || std::tolower(static_cast<unsigned char>(text[k])) != std::tolower(static_cast<unsigned char>(word[k]))) return false;
    }
    return true;
}

// Case-insensitive substring test on ASCII text.
inline bool containsNoCase(const char* text, const char* word) noexcept {
    const size_t n = std::strlen(word);
    for (size_t i = 0; text[i]; ++i) {
        size_t k = 0;
        while (k < n && text[i + k] && std::tolower(static_cast<unsigned char>(text[i + k])) ==
                                           std::tolower(static_cast<unsigned char>(word[k])))
            ++k;
        if (k == n) return true;
    }
    return false;
}

// The class of a record's name, as the lock-free cache stores it. kPad: a LandingPad* record (GuideMarkers excluded). kExplicit: a part whose
// centre is judged against the cell by its nearest instance (Hanger, Hangar, BlastShield, SmallHangerSkirt, MediumHangerSkirt, LargeHangerSkirt).
// kOther: any other readable name. kUnnamed: no readable name. kNone is the empty cache slot, never a verdict.
enum class NameClass : uint8_t { kNone = 0, kPad = 1, kExplicit = 2, kOther = 3, kUnnamed = 4 };

inline bool isPadName(const char* name) noexcept {
    return name && hasPrefixNoCase(name, "LandingPad") && !containsNoCase(name, "GuideMarkers");
}

inline bool isExplicitName(const char* name) noexcept {
    if (!name) return false;
    static const char* const kPrefixes[] = {"Hanger", "Hangar", "BlastShield", "SmallHangerSkirt", "MediumHangerSkirt", "LargeHangerSkirt"};
    for (const char* p : kPrefixes)
        if (hasPrefixNoCase(name, p)) return true;
    return false;
}

inline NameClass classifyName(bool readable, const char* name) noexcept {
    if (!readable || !name || !*name) return NameClass::kUnnamed;
    if (isPadName(name)) return NameClass::kPad;
    if (isExplicitName(name)) return NameClass::kExplicit;
    return NameClass::kOther;
}

// The own pad's size, from its name: LandingPadSmall*, LandingPadMedium*, LandingPadLarge* (GuideMarkers and other names: none).
enum class PadSize : uint8_t { kNone = 0, kSmall = 1, kMedium = 2, kLarge = 3 };

inline PadSize padSizeOf(const char* name) noexcept {
    if (!isPadName(name)) return PadSize::kNone;
    const char* rest = name + 10;   // past "LandingPad"
    if (hasPrefixNoCase(rest, "Small")) return PadSize::kSmall;
    if (hasPrefixNoCase(rest, "Medium")) return PadSize::kMedium;
    if (hasPrefixNoCase(rest, "Large")) return PadSize::kLarge;
    return PadSize::kNone;
}

inline char padSizeLetter(PadSize s) noexcept {
    switch (s) {
        case PadSize::kSmall: return 'S';
        case PadSize::kMedium: return 'M';
        case PadSize::kLarge: return 'L';
        default: return 'n';
    }
}

// One pad record of a frame, for the pick.
struct PadObs {
    uintptr_t addr = 0;
    float centre[3] = {};
    float radius = 0.0f;       // the pad's own sphere (rec+0x280)
    PadSize size = PadSize::kNone;
    char name[64] = {};
    // Built v14 (fix 1): the pad's up axis (its builder pose, axes[2]); upOk false when the pose is unreadable.
    float up[3] = {};
    bool upOk = false;
};

// The own cell of a frame. has false: no own pad this frame (named mode is the sphere rule for it) and reason says why.
struct OwnCell {
    bool has = false;
    PadSize size = PadSize::kNone;
    float centre[3] = {};      // the pad's centre
    float radius = 0.0f;       // the cell radius for the pad's size
    uintptr_t pad = 0;
    float padDist = -1.0f;     // the camera to the pad's centre
    bool padContains = false;  // the pad's own sphere contains the camera (else it was the fallback within kPadFallbackM)
    char padName[64] = {};
    char reason[80] = {};
    // Built v11: the capsule. When capsule is true the cell test is the distance to the segment capA..capB (radius kept) instead of the sphere
    // about centre; capLen is the pad-to-target distance and capTarget the target's name.
    bool capsule = false;
    float capA[3] = {};
    float capB[3] = {};
    float capLen = 0.0f;
    char capTarget[48] = {};
    // Built v11 (second addendum): the oriented box along the shaft. When box is true the named rule's cell is the box: boxCentre, the length
    // axis boxU, the width axis boxV, the shaft axis boxW (unit vectors), half-extents boxHalf (length, width, height). boxInfo says how it was
    // built (the pad's axes, extents, the choice), or why it was not (then the capsule or the pad sphere is the cell).
    bool box = false;
    float boxCentre[3] = {};
    float boxU[3] = {};
    float boxV[3] = {};
    float boxW[3] = {};
    float boxHalf[3] = {};
    float boxMargin = 0.0f;
    float boxLateral = 0.0f;         // Built v12: the BlastShieldBase (the target) off the pad's up axis, metres (the box stands on the up axis)
    char boxInfo[320] = {};
};

// The index into radii[] (small, medium, large) for a size; -1 for none.
inline int sizeIndex(PadSize s) noexcept { return s == PadSize::kNone ? -1 : static_cast<int>(s) - 1; }

// The own pad: among the frame's pad records with a size, the one whose own sphere contains the camera, nearest centre first; else the nearest
// centre within kPadFallbackM. The cell's radius is radii[size]. Built v14 (fix 1): a pad within kPadTieM of the nearest is a near-tie; the tie goes
// to the pad whose centre is nearest the camera laterally to its own up axis, then to the platform named A (LandingPadMediumA over ...B), then to the
// lower record address (so the order of the records never decides). With no candidate, has is false and reason says why.
constexpr float kPadTieM = 5.0f;

// The distance of the camera from the pad's up axis through its centre (the height along the axis is not counted). Without an up axis the total
// distance stands in.
inline float padLateralM(const PadObs& p, const float camera[3]) noexcept {
    const float d[3] = {camera[0] - p.centre[0], camera[1] - p.centre[1], camera[2] - p.centre[2]};
    if (!p.upOk) return std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float a = d[0] * p.up[0] + d[1] * p.up[1] + d[2] * p.up[2];
    const float l[3] = {d[0] - a * p.up[0], d[1] - a * p.up[1], d[2] - a * p.up[2]};
    return std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
}

// True when the pad's platform name ends in A.
inline bool padIsPlatformA(const PadObs& p) noexcept {
    const size_t len = std::strlen(p.name);
    return len > 0 && p.name[len - 1] == 'A';
}

inline void fillOwnFromPad(const PadObs& p, const float radii[3], float d, OwnCell* out) noexcept {
    out->has = true;
    out->size = p.size;
    for (int k = 0; k < 3; ++k) out->centre[k] = p.centre[k];
    out->radius = radii[sizeIndex(p.size)];
    out->pad = p.addr;
    out->padDist = d;
    out->padContains = d <= p.radius;
    std::snprintf(out->padName, sizeof(out->padName), "%s", p.name);
}

inline void pickOwnPad(const PadObs* obs, size_t n, const float camera[3], const float radii[3], OwnCell* out) noexcept {
    *out = OwnCell{};
    if (n == 0) {
        std::snprintf(out->reason, sizeof(out->reason), "no LandingPad record this frame");
        return;
    }
    bool containing = false;
    for (size_t i = 0; i < n && !containing; ++i) {
        if (obs[i].size != PadSize::kNone && distance(obs[i].centre, camera) <= obs[i].radius) containing = true;
    }
    float nearest = -1.0f;
    for (size_t i = 0; i < n; ++i) {
        if (obs[i].size == PadSize::kNone) continue;
        const float d = distance(obs[i].centre, camera);
        if (!(containing ? d <= obs[i].radius : d <= kPadFallbackM)) continue;
        if (nearest < 0.0f || d < nearest) nearest = d;
    }
    if (nearest < 0.0f) {
        std::snprintf(out->reason, sizeof(out->reason), "no pad sphere contains the camera and none is within %.0f m", static_cast<double>(kPadFallbackM));
        return;
    }
    int best = -1;
    float bestD = 0.0f, bestLat = 0.0f;
    bool bestA = false;
    for (size_t i = 0; i < n; ++i) {
        if (obs[i].size == PadSize::kNone) continue;
        const float d = distance(obs[i].centre, camera);
        if (!(containing ? d <= obs[i].radius : d <= kPadFallbackM)) continue;
        if (d > nearest + kPadTieM) continue;   // not a near-tie with the nearest
        const float lat = padLateralM(obs[i], camera);
        const bool isA = padIsPlatformA(obs[i]);
        bool better = best < 0;
        if (!better) {
            if (lat < bestLat - 1e-3f) better = true;
            else if (lat <= bestLat + 1e-3f) {
                if (isA != bestA) better = isA;
                else better = obs[i].addr < obs[best].addr;
            }
        }
        if (better) {
            best = static_cast<int>(i);
            bestD = d;
            bestLat = lat;
            bestA = isA;
        }
    }
    fillOwnFromPad(obs[best], radii, bestD, out);
}

// One explicit-part record of a frame: its node identifies the part name (records of one part share it).
struct ExplicitObs {
    uintptr_t addr = 0;
    uintptr_t node = 0;
    float centre[3] = {};
    char name[48] = {};        // Built v11: the part's name (the capsule's target is picked by it)
};

// For each part node, the nearest instance to the cell centre within kExplicitM. Writes their addresses to out (at most outCap); returns the count.
inline size_t pickExplicit(const ExplicitObs* obs, size_t n, const float cellCentre[3], uintptr_t* out, size_t outCap) noexcept {
    struct Best {
        uintptr_t node;
        uintptr_t addr;
        float d;
    };
    Best best[kExplicitNodes];
    size_t nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const float d = distance(obs[i].centre, cellCentre);
        if (d > kExplicitM) continue;
        size_t k = 0;
        while (k < nb && best[k].node != obs[i].node) ++k;
        if (k < nb) {
            if (d < best[k].d) {
                best[k].addr = obs[i].addr;
                best[k].d = d;
            }
        } else if (nb < kExplicitNodes) {
            best[nb++] = Best{obs[i].node, obs[i].addr, d};
        }
    }
    size_t written = 0;
    for (size_t k = 0; k < nb && written < outCap; ++k) out[written++] = best[k].addr;
    return written;
}

// Built v11: the cell capsule. Its target is the nearest BlastShieldBase* explicit instance within kCapsuleMaxM of the pad (else the nearest own
// HangerSmall, HangerMedium or HangerLarge within the same reach). The axis runs from the pad's centre to the target and on kCapsuleExtendM past
// it, so a 90 m shaft that a sphere on the pad cannot hold is inside the cell. The cell test is the distance to that segment.
constexpr float kCapsuleMaxM = 400.0f;

constexpr float kCapsuleExtendM = 20.0f;

inline bool hasPrefix(const char* s, const char* p) noexcept { return std::strncmp(s, p, std::strlen(p)) == 0; }

inline bool isBlastBase(const char* name) noexcept { return hasPrefix(name, "BlastShieldBase"); }

inline bool isOwnHanger(const char* name) noexcept {
    return hasPrefix(name, "HangerSmall") || hasPrefix(name, "HangerMedium") || hasPrefix(name, "HangerLarge");
}

// The distance from p to the segment ab (its end points included).
inline float distToSegment(const float p[3], const float a[3], const float b[3]) noexcept {
    const float ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float ap[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float t = len2 > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float q[3] = {a[0] + t * ab[0], a[1] + t * ab[1], a[2] + t * ab[2]};
    return distance(p, q);
}

// The capsule from the pad centre p to the target q, extended past q by extend along the axis. False when the target is not a distance away.
inline bool buildCapsule(const float p[3], const float q[3], float extend, float a[3], float b[3], float* length) noexcept {
    const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len) || !(len >= 1.0f)) return false;
    for (int k = 0; k < 3; ++k) {
        a[k] = p[k];
        b[k] = q[k] + d[k] / len * extend;
    }
    if (length) *length = len;
    return finite3(a) && finite3(b);
}

// Built v13.1 (b): the own hangar's parts are kept in named mode. The bay volume is the shaft from the pad's centre o along its unit axis u:
// lateral distance from the shaft within kBayLateralM, along it from -below (under the pad; diag.sealed_cell_below_m, default kBelowDefaultM) to
// the target's distance plus kBayPastM. Built v13.2: the below distance is the box's (one key), 60 m.
constexpr float kBayLateralM = 60.0f;

constexpr float kBayPastM = 20.0f;

// Built v13.2 (a): the below distance: the box bottom and the bay's lower bound, measured from the pad record's centre along the shaft.
// Built v13.4 (a): one default per pad size (small, medium, large), from the measured lift doors (small -72, medium -104, large -126 m) plus a
// margin: 85, 120, 145 m. Index is sizeIndex(size). diag.sealed_cell_below_<size>_m; diag.sealed_cell_below_m, when set, overrides all three.
constexpr float kBelowDefaultsM[3] = {85.0f, 120.0f, 145.0f};

// Built v13.4 (a): the below distance for a size: the override when one is set, else that size's table entry (small for no size).
inline float belowForSize(PadSize size, const float table[3], bool hasOverride, float overrideM) noexcept {
    if (hasOverride) return overrideM;
    const int i = sizeIndex(size);
    return table[i < 0 ? 0 : i];
}

// Built v13.3 (c): a raw centre is usable only when it is finite, not the origin (rec+0x240 zero is an unreadable centre, not a place) and its
// radius is positive and finite. A zero raw centre with a radius would be a fake sphere at the origin, judged as far away and culled.
inline bool rawCentreUsable(const float raw[3], float radius) noexcept {
    return finite3(raw) && !isZeroCentre(raw) && std::isfinite(radius) && radius > 0.0f;
}

// Built v13.3 (b): the own pad candidates: a pad record containing the camera, or with its centre within kOwnPadNearM of it, of any size.
constexpr float kOwnPadNearM = 40.0f;

inline bool ownPadCandidate(float centreDist, float radius) noexcept {
    return centreDist <= radius || centreDist <= kOwnPadNearM;
}

// Built v13.3 (b): the held cell is dropped when its pad record is absent for limit consecutive frames, or another own pad candidate is the
// nearest one for limit consecutive frames.
constexpr uint32_t kHeldLostFrames = 30;

inline bool heldCellDrops(uint32_t absentFrames, uint32_t otherFrames, uint32_t limit) noexcept {
    return absentFrames >= limit || otherFrames >= limit;
}

// Built v13.7 (a): the held cell in the held PAD's own frame. A point is written as its coordinates along the pad's three unit axes from the
// pad's centre; a direction the same way without the centre. The frame the records are expressed in can shift (the v13.6 flight: a static pad
// moved 34 m when the player went on foot, the same offset for the box), so the cell is frozen in the pad's frame at engage and rebuilt from the
// pad's live pose before every judgement. axes[k] is the k-th unit axis in world coordinates (axes[2] is up).
inline void pointToLocal(const float p[3], const float centre[3], const float axes[3][3], float out[3]) noexcept {
    const float d[3] = {p[0] - centre[0], p[1] - centre[1], p[2] - centre[2]};
    for (int k = 0; k < 3; ++k) out[k] = axes[k][0] * d[0] + axes[k][1] * d[1] + axes[k][2] * d[2];
}

inline void pointToWorld(const float local[3], const float centre[3], const float axes[3][3], float out[3]) noexcept {
    for (int j = 0; j < 3; ++j) out[j] = centre[j] + local[0] * axes[0][j] + local[1] * axes[1][j] + local[2] * axes[2][j];
}

inline void dirToLocal(const float v[3], const float axes[3][3], float out[3]) noexcept {
    for (int k = 0; k < 3; ++k) out[k] = axes[k][0] * v[0] + axes[k][1] * v[1] + axes[k][2] * v[2];
}

inline void dirToWorld(const float local[3], const float axes[3][3], float out[3]) noexcept {
    for (int j = 0; j < 3; ++j) out[j] = local[0] * axes[0][j] + local[1] * axes[1][j] + local[2] * axes[2][j];
}

// The cell's points (centre, capA, capB, boxCentre) and directions (boxU, boxV, boxW) written in the pad's frame. Every other field is copied.
inline void localizeCell(const OwnCell& world, const float centre[3], const float axes[3][3], OwnCell* local) noexcept {
    *local = world;
    pointToLocal(world.centre, centre, axes, local->centre);
    pointToLocal(world.capA, centre, axes, local->capA);
    pointToLocal(world.capB, centre, axes, local->capB);
    pointToLocal(world.boxCentre, centre, axes, local->boxCentre);
    dirToLocal(world.boxU, axes, local->boxU);
    dirToLocal(world.boxV, axes, local->boxV);
    dirToLocal(world.boxW, axes, local->boxW);
}

// The inverse: the local cell placed in the pad's CURRENT frame (its live centre and axes). Only the points and directions of *world are written;
// its other fields (the names, the reasons, the sizes) keep their values.
inline void worldizeCell(const OwnCell& local, const float centre[3], const float axes[3][3], OwnCell* world) noexcept {
    pointToWorld(local.centre, centre, axes, world->centre);
    pointToWorld(local.capA, centre, axes, world->capA);
    pointToWorld(local.capB, centre, axes, world->capB);
    pointToWorld(local.boxCentre, centre, axes, world->boxCentre);
    dirToWorld(local.boxU, axes, world->boxU);
    dirToWorld(local.boxV, axes, world->boxV);
    dirToWorld(local.boxW, axes, world->boxW);
}

// Built v13.7 (b): the pad's height along the shaft (up) relative to a static reference, the hangar shell record: the pad minus the shell, dotted
// with up. A frame shift moves both alike and cancels; only the pad's own motion changes it.
constexpr float kPadMoveReleaseM = 2.0f;   // the station releases when the pad moves more than this along the shaft against the shell

inline float shellRelative(const float pad[3], const float shell[3], const float up[3]) noexcept {
    return (pad[0] - shell[0]) * up[0] + (pad[1] - shell[1]) * up[1] + (pad[2] - shell[2]) * up[2];
}

// True when the pad has moved more than limitM along the shaft since the engage (the two relative heights differ).
inline bool padMovedAway(float engageRel, float nowRel, float limitM = kPadMoveReleaseM) noexcept {
    return std::isfinite(engageRel) && std::isfinite(nowRel) && std::fabs(nowRel - engageRel) > limitM;
}

// The name classes the bay keeps: substrings of the game's record names (the own hangar's parts at any size).
inline bool bayPartName(const char* name) noexcept {
    static const char* const kParts[] = {"Hanger", "Hangar", "Lobby", "BlastShield", "Skirt", "LandingPad", "Lift", "Gate"};
    if (!name || !name[0]) return false;
    for (const char* part : kParts) {
        if (std::strstr(name, part)) return true;
    }
    return false;
}

// The distance from p to the infinite line through o along the unit u; along (optional) receives the position of p's foot along u.
inline float lateralToAxis(const float p[3], const float o[3], const float u[3], float* along = nullptr) noexcept {
    const float d[3] = {p[0] - o[0], p[1] - o[1], p[2] - o[2]};
    const float a = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    const float l[3] = {d[0] - a * u[0], d[1] - a * u[1], d[2] - a * u[2]};
    if (along) *along = a;
    return std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
}

// True when p lies in the bay volume of the shaft from o along u, the target at targetA metres from o, and below metres under o.
inline bool inBayVolume(const float p[3], const float o[3], const float u[3], float targetA, float below) noexcept {
    float a = 0.0f;
    const float lat = lateralToAxis(p, o, u, &a);
    return std::isfinite(lat) && std::isfinite(a) && lat <= kBayLateralM && a >= -below && a <= targetA + kBayPastM;
}

// Built v13.2 (a): diag.sealed_cell_below_m, a number of metres in [0, 200]; empty is the default. False for text that is not one.
inline bool parseBelowM(const char* text, float defaultM, float* out) noexcept {
    *out = defaultM;
    if (!text || !*text) return true;
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v) || v < 0.0 || v > 200.0) return false;
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return false;
    *out = static_cast<float>(v);
    return true;
}

// Built v13.2 (b): the own ship's record name is the type prefix without its trailing underscore ("CobraMkV_" -> "CobraMkV").
inline bool strippedPrefix(const char* prefix, char* out, size_t cap) noexcept {
    out[0] = '\0';
    if (!prefix || !cap) return false;
    const size_t n = std::strlen(prefix);
    if (n < 2 || prefix[n - 1] != '_' || n >= cap) return false;
    std::memcpy(out, prefix, n - 1);
    out[n - 1] = '\0';
    return true;
}

// True when the record's name is exactly the stripped prefix (both non-empty).
inline bool ownRecordMatch(const char* name, const char* stripped) noexcept {
    return name && stripped && stripped[0] && std::strcmp(name, stripped) == 0;
}

// Built v13.2 (b): the own ship's per-frame keep: a record whose centre lies within the own record's CURRENT sphere plus marginM.
inline bool ownSphereKeeps(const float centre[3], const float ownCentre[3], float ownRadius, float marginM) noexcept {
    if (!std::isfinite(ownRadius) || !(ownRadius > 0.0f)) return false;
    return distance(centre, ownCentre) <= ownRadius + marginM;
}

// The unit vector from a to b (false when they are closer than a metre, or not finite).
inline bool unitBetween(const float a[3], const float b[3], float u[3]) noexcept {
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(len) || !(len >= 1.0f)) return false;
    for (int k = 0; k < 3; ++k) u[k] = d[k] / len;
    return true;
}

// The nearest BlastShieldBase* instance within kCapsuleMaxM of p (else the nearest own Hanger*Small/Medium/Large). Writes its centre and name.
inline bool pickCapsuleTarget(const ExplicitObs* obs, size_t n, const float p[3], float q[3], char* name, size_t cap) noexcept {
    for (int pass = 0; pass < 2; ++pass) {
        int best = -1;
        float bestD = 0.0f;
        for (size_t i = 0; i < n; ++i) {
            const bool match = pass == 0 ? isBlastBase(obs[i].name) : isOwnHanger(obs[i].name);
            if (!match) continue;
            const float d = distance(obs[i].centre, p);
            if (d > kCapsuleMaxM) continue;
            if (best < 0 || d < bestD) {
                best = static_cast<int>(i);
                bestD = d;
            }
        }
        if (best >= 0) {
            for (int k = 0; k < 3; ++k) q[k] = obs[best].centre[k];
            std::snprintf(name, cap, "%s", obs[best].name);
            return true;
        }
    }
    return false;
}

// Built v11 (second addendum): the oriented box. Elite's pad sizes (the wiki), length x width in metres, small, medium, large. The hangar is the
// pad doubled along one axis (diag.sealed_cell_double), so each half-extent is half the pad's size times 2 on that axis and 1 on the other, plus
// the margin (diag.sealed_cell_margin_m). Vertically the box runs from the below distance (diag.sealed_cell_below_m) under the pad's centre up the shaft to kBoxBeyondM past the
// target (the nearest BlastShieldBase). A round cell cannot hold a rectangular bay without the neighbours: this is the bay's footprint.
constexpr float kPadWikiM[3][2] = {{66.0f, 55.0f}, {150.0f, 90.0f}, {213.0f, 128.0f}};

constexpr float kBoxBeyondM = 20.0f;

constexpr float kMarginDefaultM = 10.0f;   // v12.1: 10 m (was 5; a guess)

// The local axes of a rotation given as the builder's quaternion (R(q)): axes[k] is R(q) e_k, from builderCentre.
inline void quatAxes(const float q[4], float axes[3][3]) noexcept {
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float o0[4] = {};
    builderCentre(q, zero, zero, o0);
    for (int k = 0; k < 3; ++k) {
        float e[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        e[k] = 1.0f;
        float ok[4] = {};
        builderCentre(q, zero, e, ok);
        for (int j = 0; j < 3; ++j) axes[k][j] = ok[j] - o0[j];
    }
}

// The local axes of a store-style pose (the node block through rec+0x190; decomp_4312040.txt): with A the pose block (3x3 at A[4k+j], translation
// A[12..14]) and B the matrix at A+0x50 (B[4i+j], translation B[12..14]), the world image of local e_k is axes[k][j] = sum_i B[4i+j] * A[4k+i].
inline void storeAxes(const float A[15], const float B[16], float axes[3][3]) noexcept {
    for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int i = 0; i < 3; ++i) s += B[4 * i + j] * A[4 * k + i];
            axes[k][j] = s;
        }
}

// The footprint of the pad's box: of its three local axes, the one most along the shaft u is the up axis; of the other two, the one with the
// larger builder extent is the length axis (lenIdx), the other the width (widIdx). False with no extent: the box cannot tell length from width.
inline bool footprintAxes(const float axes[3][3], const float u[3], const float* ext, int* lenIdx, int* widIdx, int* upIdx) noexcept {
    if (!ext) return false;
    int up = 0;
    float best = -1.0f;
    for (int k = 0; k < 3; ++k) {
        const float d = std::fabs(axes[k][0] * u[0] + axes[k][1] * u[1] + axes[k][2] * u[2]);
        if (d > best) {
            best = d;
            up = k;
        }
    }
    if (upIdx) *upIdx = up;
    int a = -1, b = -1;
    for (int k = 0; k < 3; ++k) {
        if (k == up) continue;
        if (a < 0) a = k;
        else b = k;
    }
    if (ext[a] >= ext[b]) {
        *lenIdx = a;
        *widIdx = b;
    } else {
        *lenIdx = b;
        *widIdx = a;
    }
    return true;
}

// v12.1 (flight): the box's axis W is the unit vector from the pad's centre to the target (BlastShieldBase): the shaft is the direction the pad
// travels, not the pad's own e2 (the v12 flight: the pad rose along the pad-to-BlastShieldBase line). The pad is on the axis, so the box is
// centred laterally on it: from the below distance (diag.sealed_cell_below_m) under the pad's centre to kBoxBeyondM past the target along W. The footprint's length axis lenAxis is
// made orthogonal to W (Gram-Schmidt, length first); the width is W x length. False when the target is closer than 1 m or the axes are degenerate.
inline bool buildBox(const float pad[3], const float target[3], const float lenAxis[3], float halfLen, float halfWid, float margin,
                     float below, OwnCell* cell) noexcept {
    const float d[3] = {target[0] - pad[0], target[1] - pad[1], target[2] - pad[2]};
    const float proj = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!std::isfinite(proj) || !(proj >= 1.0f)) return false;
    float W[3] = {d[0] / proj, d[1] / proj, d[2] / proj};
    const float lateral = 0.0f;
    const float dp = lenAxis[0] * W[0] + lenAxis[1] * W[1] + lenAxis[2] * W[2];
    float U[3] = {lenAxis[0] - dp * W[0], lenAxis[1] - dp * W[1], lenAxis[2] - dp * W[2]};
    const float ul = std::sqrt(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    if (!std::isfinite(ul) || !(ul > 1e-3f)) return false;
    for (float& v : U) v /= ul;
    const float w[3] = {W[0], W[1], W[2]};
    const float V[3] = {w[1] * U[2] - w[2] * U[1], w[2] * U[0] - w[0] * U[2], w[0] * U[1] - w[1] * U[0]};
    const float lo = -below, hi = proj + kBoxBeyondM;
    const float along = (lo + hi) * 0.5f;
    const float half = (hi - lo) * 0.5f;
    float hx = halfLen + margin, hy = halfWid + margin;
    if (hx < 1.0f) hx = 1.0f;
    if (hy < 1.0f) hy = 1.0f;
    cell->box = true;
    cell->boxMargin = margin;
    cell->boxLateral = lateral;
    for (int k = 0; k < 3; ++k) {
        cell->boxCentre[k] = pad[k] + w[k] * along;
        cell->boxU[k] = U[k];
        cell->boxV[k] = V[k];
        cell->boxW[k] = w[k];
    }
    cell->boxHalf[0] = hx;
    cell->boxHalf[1] = hy;
    cell->boxHalf[2] = half;
    return true;
}

// A record's position in the box: across the length (U), across the width (V), up the shaft (W), metres from the box's centre.
inline void boxCoords(const float c[3], const OwnCell& cell, float out[3]) noexcept {
    const float d[3] = {c[0] - cell.boxCentre[0], c[1] - cell.boxCentre[1], c[2] - cell.boxCentre[2]};
    out[0] = d[0] * cell.boxU[0] + d[1] * cell.boxU[1] + d[2] * cell.boxU[2];
    out[1] = d[0] * cell.boxV[0] + d[1] * cell.boxV[1] + d[2] * cell.boxV[2];
    out[2] = d[0] * cell.boxW[0] + d[1] * cell.boxW[1] + d[2] * cell.boxW[2];
}

// Whether a centre is inside the cell's box (its centre, the three half-extents).
inline bool inBox(const float c[3], const OwnCell& cell) noexcept {
    float o[3];
    boxCoords(c, cell, o);
    return std::fabs(o[0]) <= cell.boxHalf[0] && std::fabs(o[1]) <= cell.boxHalf[1] && std::fabs(o[2]) <= cell.boxHalf[2];
}

// Built v12: the off-screen emulation's steps, in Elite's order (decomp_431AFE0.txt lines 99 to 102; bytes 0x431B221..0x431B240). Kind 1: the byte
// store [coll+0x629] = 0. Kind 2: the call of 0x434DD50 with rcx = coll+0x300 (the owner count reset). Kind 3: the call of 0x42B5760 with rcx = the
// pipe, only when the pipe is non-null (it adds 0x78 and jumps to 0x434DD50: pipe+0x31C). Returns the number of steps written to kinds and args.
inline int offScreenSteps(uintptr_t coll, uintptr_t pipe, int* kinds, uintptr_t* args) noexcept {
    int n = 0;
    kinds[n] = 1;
    args[n] = coll + 0x629;
    ++n;
    kinds[n] = 2;
    args[n] = coll + 0x300;
    ++n;
    if (pipe) {
        kinds[n] = 3;
        args[n] = pipe;
        ++n;
    }
    return n;
}

// Built v12: the early rig veto's rule (pure). A rig's bound is the box of its records' last-frame world spheres; its bounding sphere has the box
// centre and the half-diagonal as radius (conservative). The cell in force is the named box, the capsule or the pad's sphere; mode all has no cell
// and vetoes every bounded rig. A rig is OUTSIDE when its bounding sphere lies wholly outside the cell. A rig that is outside is vetoed, except every
// kRigRefreshN-th time, when it is let through so its records are re-judged (vetoedRun counts the vetoes since that).
constexpr uint32_t kRigRefreshN = 30;

enum class RigVerdict : uint8_t { kUnknown = 0, kMixed = 1, kOutside = 2, kRefresh = 3 };

// The distance from a point to the cell (0 inside it): the box, the capsule's segment less its radius, or the sphere less its radius.
inline float distanceToCell(const float c[3], const OwnCell& cell) noexcept {
    if (cell.box) {
        float o[3];
        boxCoords(c, cell, o);
        const float dx = std::max(std::fabs(o[0]) - cell.boxHalf[0], 0.0f);
        const float dy = std::max(std::fabs(o[1]) - cell.boxHalf[1], 0.0f);
        const float dz = std::max(std::fabs(o[2]) - cell.boxHalf[2], 0.0f);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (cell.capsule) return std::max(distToSegment(c, cell.capA, cell.capB) - cell.radius, 0.0f);
    return std::max(distance(c, cell.centre) - cell.radius, 0.0f);
}

// The verdict for one rig. bounded: the rig has a bound from its records. c and r: the bounding sphere's centre and radius. hasCell: a named cell is
// in force (mode all does not need one). vetoedRun: the vetoes since the rig was last let through.
inline RigVerdict rigVerdict(bool bounded, const float c[3], float r, bool hasCell, const OwnCell& cell, bool allMode, uint32_t vetoedRun) noexcept {
    if (!bounded) return RigVerdict::kUnknown;
    bool outside;
    if (allMode) {
        outside = true;
    } else {
        if (!hasCell) return RigVerdict::kUnknown;
        outside = distanceToCell(c, cell) > r;
    }
    if (!outside) return RigVerdict::kMixed;
    if (vetoedRun >= kRigRefreshN) return RigVerdict::kRefresh;
    return RigVerdict::kOutside;
}

// Built v13 (fix 1): the own ship is never culled in sphere or named mode (only all culls it). Exempt: a record or rig whose sphere contains the
// camera with a radius under kOwnShipRadiusM, or whose name starts with the own ship's type prefix (taken at engage).
constexpr float kOwnShipRadiusM = 40.0f;

// The own ship's type prefix from its name: everything up to and including the first underscore ("CobraMkV_01" -> "CobraMkV_"). Empty when the
// name has no underscore.
inline void ownPrefixOf(const char* name, char* out, size_t cap) noexcept {
    out[0] = '\0';
    if (!name || !cap) return;
    const char* u = std::strchr(name, '_');
    if (!u) return;
    const size_t n = static_cast<size_t>(u - name) + 1;
    if (n >= cap) return;
    std::memcpy(out, name, n);
    out[n] = '\0';
}

// Whether a record (or rig) is the own ship: the camera inside a small sphere, or the type prefix matches the name.
inline bool ownShipExempt(bool containsCamera, float radius, const char* name, const char* prefix) noexcept {
    if (containsCamera && radius < kOwnShipRadiusM) return true;
    if (prefix && prefix[0] && name && std::strncmp(name, prefix, std::strlen(prefix)) == 0) return true;
    return false;
}

// Built v13 (fix 3): the shaft's target is the instance of the given name class nearest the LINE through the pad's centre along the pad's up
// axis e2 (perpendicular distance; ties by the distance along the axis). Only instances above the pad (along > 0) and within kCapsuleMaxM of it.
// Writes the target's centre and name and its perpendicular and along distances. False when there is none.
inline bool pickTargetOnLine(const ExplicitObs* obs, size_t n, const float pad[3], const float e2[3], int passClass, float q[3], char* name,
                             size_t cap, float* perp, float* along) noexcept {
    int best = -1;
    float bestPerp = 0.0f, bestAlong = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        const bool match = passClass == 0 ? isBlastBase(obs[i].name) : isOwnHanger(obs[i].name);
        if (!match) continue;
        const float v[3] = {obs[i].centre[0] - pad[0], obs[i].centre[1] - pad[1], obs[i].centre[2] - pad[2]};
        const float a = v[0] * e2[0] + v[1] * e2[1] + v[2] * e2[2];
        if (!(a > 0.0f)) continue;
        const float d = distance(obs[i].centre, pad);
        if (d > kCapsuleMaxM) continue;
        const float pd = std::sqrt(std::fmax(d * d - a * a, 0.0f));
        if (best < 0 || pd < bestPerp || (pd == bestPerp && a < bestAlong)) {
            best = static_cast<int>(i);
            bestPerp = pd;
            bestAlong = a;
        }
    }
    if (best < 0) return false;
    for (int k = 0; k < 3; ++k) q[k] = obs[best].centre[k];
    std::snprintf(name, cap, "%s", obs[best].name);
    *perp = bestPerp;
    *along = bestAlong;
    return true;
}


// Built v12.1: the precomputed verdict's code, one byte per rig (the frame tick writes it, the decision reads it): 2 outside, 1 mixed, 0 unknown or
// a refresh (the decision decides the refresh from its own run count).
inline uint8_t verdictCode(RigVerdict v) noexcept {
    return v == RigVerdict::kOutside ? uint8_t{2} : (v == RigVerdict::kMixed ? uint8_t{1} : uint8_t{0});
}

// Built v11 (addendum): with a capsule, the explicit picks are the nearest instance of each part node to the capsule's AXIS, and only within
// kExplicitCapsuleM of it. They backstop the own hangar's parts and nothing further: a neighbouring pad's part is farther than that from this
// axis, so it is not picked. Same shape as pickExplicit.
inline size_t pickExplicitSegment(const ExplicitObs* obs, size_t n, const float a[3], const float b[3], uintptr_t* out, size_t outCap) noexcept {
    struct Best {
        uintptr_t node;
        uintptr_t addr;
        float d;
    };
    Best best[kExplicitNodes];
    size_t nb = 0;
    for (size_t i = 0; i < n; ++i) {
        const float d = distToSegment(obs[i].centre, a, b);
        if (d > kExplicitCapsuleM) continue;
        size_t k = 0;
        while (k < nb && best[k].node != obs[i].node) ++k;
        if (k < nb) {
            if (d < best[k].d) {
                best[k].addr = obs[i].addr;
                best[k].d = d;
            }
        } else if (nb < kExplicitNodes) {
            best[nb++] = Best{obs[i].node, obs[i].addr, d};
        }
    }
    size_t written = 0;
    for (size_t k = 0; k < nb && written < outCap; ++k) out[written++] = best[k].addr;
    return written;
}

// Why a record is kept in named mode.
enum class KeepWhy : uint8_t { kNone = 0, kCell = 1, kExplicit = 2, kSphere = 3 };

struct NamedKeep {
    bool keep = false;
    KeepWhy why = KeepWhy::kNone;
    bool containing = false;      // the record's sphere contains the camera
    bool sphereJudged = false;    // the sphere rule alone would keep it (so a cell keep is "by cell" only when this is false)
};

inline bool inAddrSet(const uintptr_t* set, size_t n, uintptr_t addr) noexcept {
    for (size_t i = 0; i < n; ++i)
        if (set[i] == addr) return true;
    return false;
}

// The named rule for one record (Built v8.3b). No own cell: the sphere rule for the frame. With one: kept when the record's centre lies within
// the cell radius of the cell centre (why cell), or when it is an explicit part's nearest instance (why explicit). Otherwise culled, including a
// record whose sphere contains the camera.
inline NamedKeep keepNamedCell(const float centre[3], float radius, const float camera[3], float R, const OwnCell& cell,
                               const uintptr_t* explicitSet, size_t explicitN, uintptr_t addr) noexcept {
    NamedKeep v;
    v.containing = containsCamera(centre, radius, camera);
    v.sphereJudged = keepRecord(centre, radius, camera, R);
    if (!cell.has) {
        v.keep = v.sphereJudged;
        v.why = v.keep ? KeepWhy::kSphere : KeepWhy::kNone;
        return v;
    }
    // The box when one is in force (Built v11, second addendum), else the capsule, else the pad's sphere.
    const bool inCell = cell.box ? inBox(centre, cell)
                        : cell.capsule ? distToSegment(centre, cell.capA, cell.capB) <= cell.radius
                                       : distance(centre, cell.centre) <= cell.radius;
    if (inCell) {
        v.keep = true;
        v.why = KeepWhy::kCell;
        return v;
    }
    if (inAddrSet(explicitSet, explicitN, addr)) {
        v.keep = true;
        v.why = KeepWhy::kExplicit;
    }
    return v;
}

// The class cache's slot (lock-free): node>>3 in bits 0..43, the class in bits 44..46 (never 0 for a stored slot), the frame's stamp in bits
// 47..63. A zero word is an empty slot. A node that is not 8-aligned or is out of the 47-bit user range is not cached.
constexpr uint64_t kSlotNodeMask = (uint64_t(1) << 44) - 1;

constexpr uint32_t kSlotStampMask = (1u << 17) - 1;

inline bool slotFits(uintptr_t node) noexcept { return (node & 7u) == 0 && (uint64_t(node) >> 3) <= kSlotNodeMask; }

inline uint64_t packSlot(uintptr_t node, NameClass cls, uint32_t stamp) noexcept {
    return (uint64_t(node) >> 3) | (uint64_t(static_cast<uint8_t>(cls)) << 44) | (uint64_t(stamp & kSlotStampMask) << 47);
}

// True when the slot holds this node's class in this frame; the class is written to *out.
inline bool unpackSlot(uint64_t v, uintptr_t node, uint32_t stamp, NameClass* out) noexcept {
    if (v == 0) return false;
    if ((v & kSlotNodeMask) != (uint64_t(node) >> 3)) return false;
    if (((v >> 47) & kSlotStampMask) != (stamp & kSlotStampMask)) return false;
    const uint32_t c = static_cast<uint32_t>((v >> 44) & 7u);
    if (c == 0) return false;
    *out = static_cast<NameClass>(c);
    return true;
}

// Built v10: a record's world sphere centre the way the store computes it for the records producer B appends (decomp_4312040.txt, lines
// 212-262; the derivation is in docs\concourse-culling-2026-10-11.md, Built v10). L is the node's bounds centre (*(node+8)+0x20, xyz), A the
// record's pose block (*(rec+0x190): 15 floats), B the matrix at *(A+0x50) (16 floats). With u_j = sum_k A[4k+j] L_k + A[12+j] and
// v_j = sum_i B[4i+j] u_i + B[12+j], the centre is v. The radius is the node block's +0x2C, unscaled. False when the result is not finite.
inline bool storeWorldSphere(const float L[3], const float A[15], const float B[16], float out[3]) noexcept {
    float u[3];
    for (int j = 0; j < 3; ++j) {
        u[j] = A[12 + j];
        for (int k = 0; k < 3; ++k) u[j] += A[4 * k + j] * L[k];
    }
    for (int j = 0; j < 3; ++j) {
        out[j] = B[12 + j];
        for (int i = 0; i < 3; ++i) out[j] += B[4 * i + j] * u[i];
    }
    return finite3(out);
}

// The cull of one record, with its fault handling (sealed_trial.cpp; the rig tests it against a fake memory). The record's view mask and its
// admitted flag are read first and written only after both are read: a failed read touches nothing, a failed mask write touches nothing,
// and a failed admitted write puts the mask back, so a fault always leaves the record as the store wrote it (kept). A failed put-back is
// reported as torn: the one case where the record may be left half culled, counted and named.
enum class Cull : uint8_t { kCulled = 0, kUntouched = 1, kRestored = 2, kTorn = 3 };

enum class CullField : uint8_t { kMaskRead = 0, kAdmittedRead = 1, kMaskWrite = 2, kAdmittedWrite = 3, kRestoreWrite = 4 };

template <typename Mem>
inline Cull cullRecordFaultSafe(Mem& m, CullField* failed, uint64_t* value) noexcept {
    uint64_t oldMask = 0;
    uint8_t oldAdmitted = 0;
    if (!m.readMask(&oldMask)) {
        *failed = CullField::kMaskRead;
        *value = 0;
        return Cull::kUntouched;
    }
    if (!m.readAdmitted(&oldAdmitted)) {
        *failed = CullField::kAdmittedRead;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeMask(0)) {
        *failed = CullField::kMaskWrite;
        *value = oldMask;
        return Cull::kUntouched;
    }
    if (!m.writeAdmitted(0)) {
        *failed = CullField::kAdmittedWrite;
        *value = oldAdmitted;
        if (m.writeMask(oldMask)) return Cull::kRestored;
        *failed = CullField::kRestoreWrite;
        *value = oldMask;
        return Cull::kTorn;
    }
    return Cull::kCulled;
}

// ---- The seal's trigger (the station's state machine). Pure: the engine feeds the inputs each tick and acts on the result. ----------------------

// The pad is LOWERED when its height above the hangar shell along the pad's up axis is below kLoweredM. Built v14.5: this replaces the lowest
// height seen (the v14.4 flight never re-sealed: the origin rebase on foot and in Explorer Cam shifted the frame, so a static pad was never "at its
// lowest" again, and the toggles did not reset it). The height is the shell-relative quantity the motion check uses (same function, same centre
// sources), so a frame shift of pad and shell together leaves it unchanged. Lowered pads read about -45 m (small) and -89 m (medium) above the shell;
// raised pads +35 m and +76 m (the v13.9 data).
constexpr float kLoweredM = -10.0f;
// Still: the height above the shell changes less than kSealStillM over kSealDelayMs before the seal. kSealDelayMs is the hatch's close time: a guess
// (6 s) that the maintainer will measure in flight. Change it here and nowhere else.
constexpr float kSealStillM = 0.2f;
constexpr uint64_t kSealDelayMs = 6000;
// The camera has left the held box by more than this: the seal is released (riding the elevator down, an external camera flying out).
constexpr float kCameraLeaveM = 2.0f;
// Built v14.7: the camera counts in both directions over this many consecutive frames: it must be outside the held box for this long to release,
// and inside the bay for this long to re-seal (a free camera skimming a wall must not flicker the seal). Explorer Cam is no condition.
constexpr uint32_t kCameraFrames = 10;

enum class SealAction : uint8_t { kNone = 0, kRequestEngage = 1, kRelease = 2 };
enum class ReleaseWhy : uint8_t { kNone = 0, kSettingOff, kUndocked, kCameraLeft, kPadMoved, kHangarChanged, kLaunch };

struct SealInputs {
    bool setting = false;        // fix.optimized_station is on
    bool docked = false;         // Status.json Flags bit 0
    bool pickOk = false;         // the own pad is found this frame, with a pose
    float rel = 0.0f;            // the pad's height above the hangar shell along its up axis now (metres; frame-independent)
    bool relOk = false;          // rel is readable this frame (an unreadable frame holds the trigger's state)
    bool sourceChanged = false;  // a centre source changed since the last reading: the still test restarts, the motion reference follows
    bool cameraInBox = false;    // the camera is inside the candidate box: the seal needs the camera in the bay (not used while held)
    uintptr_t pad = 0;           // the own pad's record: its motion memory is kept per pad (built v14.3)
    bool held = false;           // sealed now
    bool cameraOutside = false;  // the camera is outside the held box by more than kCameraLeaveM (only meaningful while held)
    bool padMoved = false;       // the pad moved more than kPadMoveReleaseM along the shaft relative to the hangar shell
    bool hangarChanged = false;  // the held pad is gone from the store, or another own pad is the nearest for kHeldLostFrames
    bool launched = false;       // the pad's state object flipped at launch (the reader is not in this build yet: always false)
    uint64_t nowMs = 0;
};

struct SealState {
    bool stillValid = false;     // the stillness timer is running
    float anchor = 0.0f;         // the height above the shell when the timer last restarted
    uint64_t stillSinceMs = 0;   // when the timer last restarted
    bool requested = false;      // one engage per still period
    bool immediate = false;      // the request in force was made at load-in (no descent observed): the delay did not apply
    // Built v14.3 (the load-in rule): the own pad in hand, its height above the shell when first seen, and whether it has moved off that height since
    // (a descent observed this session). The memory is per pad and survives a release; a release by motion counts as a descent.
    bool seen = false;
    uintptr_t padAddr = 0;
    float firstRel = 0.0f;
    bool moved = false;
    // Built v14.7: consecutive frames the camera has been outside the held box (camOutRun) or inside the bay (camInRun; starts satisfied).
    uint32_t camOutRun = 0;
    uint32_t camInRun = kCameraFrames;
};

// A release, or the trigger lapsing: the trigger starts again, the pad's motion memory stays (a pad that has descended still waits its delay).
inline void clearTrigger(SealState* st) noexcept {
    st->stillValid = false;
    st->requested = false;
    st->immediate = false;
}

// Built v14.5: the centre sources a height was read from. A change is a different reading: the still test restarts.
struct ShellHeightSources {
    bool have = false;
    uint8_t pad = 4;
    uint8_t shell = 4;
    // Notes the sources of this reading; returns true when they differ from the last reading's.
    bool note(uint8_t p, uint8_t s) noexcept {
        const bool changed = have && (p != pad || s != shell);
        have = true;
        pad = p;
        shell = s;
        return changed;
    }
};

// Built v14.6: the own ship's type prefix, read from the candidate record each tick. A change of prefix is a ship change, but only after
// kShipChangeTicks readings in a row of the new prefix: a flicker between two near records is not a swap. The first reading only sets the prefix.
constexpr uint32_t kShipChangeTicks = 10;

struct ShipWatch {
    char prefix[32] = {};
    char pending[32] = {};
    uint32_t run = 0;
    // Returns true when the ship changed (the prefix is now the new one). A missing reading (empty) is no reading.
    bool step(const char* cur) noexcept {
        if (cur == nullptr || cur[0] == '\0') return false;
        if (prefix[0] == '\0') {
            std::snprintf(prefix, sizeof(prefix), "%s", cur);
            run = 0;
            return false;
        }
        if (std::strcmp(cur, prefix) == 0) {
            run = 0;
            pending[0] = '\0';
            return false;
        }
        if (std::strcmp(cur, pending) != 0) {
            std::snprintf(pending, sizeof(pending), "%s", cur);
            run = 0;
        }
        if (++run < kShipChangeTicks) return false;
        std::snprintf(prefix, sizeof(prefix), "%s", cur);
        run = 0;
        pending[0] = '\0';
        return true;
    }
};

// Advances the state and returns the action. With kRelease, *why says which rule released (the order of the brief: setting off, undocked,
// Explorer Cam, camera left, pad moved, hangar changed, launch). A pad first seen lowered with no descent observed seals on the first eligible
// frame (the load-in rule, v14.3); after an observed descent the still delay applies. A raised pad never seals. The camera must be in the bay.
inline SealAction sealStep(const SealInputs& in, SealState* st, ReleaseWhy* why) noexcept {
    *why = ReleaseWhy::kNone;
    if (!in.setting) {
        *st = SealState{};
        if (in.held) {
            *why = ReleaseWhy::kSettingOff;
            return SealAction::kRelease;
        }
        return SealAction::kNone;
    }
    // The camera's runs (built v14.7): the camera rule is the only camera condition, and it needs kCameraFrames frames either way.
    if (in.cameraOutside) ++st->camOutRun;
    else st->camOutRun = 0;
    if (in.cameraInBox) {
        if (st->camInRun < kCameraFrames) ++st->camInRun;
    } else {
        st->camInRun = 0;
    }
    if (in.held) {
        ReleaseWhy w = ReleaseWhy::kNone;
        if (!in.docked) w = ReleaseWhy::kUndocked;
        else if (st->camOutRun >= kCameraFrames) w = ReleaseWhy::kCameraLeft;
        else if (in.padMoved) w = ReleaseWhy::kPadMoved;
        else if (in.hangarChanged) w = ReleaseWhy::kHangarChanged;
        else if (in.launched) w = ReleaseWhy::kLaunch;
        if (w != ReleaseWhy::kNone) {
            if (in.padMoved || in.launched) st->moved = true;   // a release by motion is a descent
            clearTrigger(st);
            st->camOutRun = 0;
            if (w == ReleaseWhy::kCameraLeft) st->camInRun = 0;   // the re-seal waits kCameraFrames inside the bay
            *why = w;
            return SealAction::kRelease;
        }
        return SealAction::kNone;
    }
    if (!in.docked || !in.pickOk) {
        clearTrigger(st);
        return SealAction::kNone;
    }
    if (!in.relOk) return SealAction::kNone;   // unreadable this frame: the trigger keeps its state
    if (!st->seen || st->padAddr != in.pad) {  // a pad first in hand: its height is the reference for a descent
        st->seen = true;
        st->padAddr = in.pad;
        st->firstRel = in.rel;
        st->moved = false;
    }
    if (in.sourceChanged) st->firstRel = in.rel;   // a different centre source is a different reading: the motion reference follows it
    if (std::fabs(in.rel - st->firstRel) > kSealStillM) st->moved = true;
    const bool lowered = in.rel < kLoweredM;
    if (!lowered || st->camInRun < kCameraFrames) {
        clearTrigger(st);
        return SealAction::kNone;
    }
    if (!st->moved) {   // load-in: no descent observed, the pad rests lowered: seal at the first eligible frame
        if (st->requested) return SealAction::kNone;
        st->requested = true;
        st->immediate = true;
        return SealAction::kRequestEngage;
    }
    if (!st->stillValid || in.sourceChanged || std::fabs(in.rel - st->anchor) > kSealStillM) {
        st->stillValid = true;
        st->anchor = in.rel;
        st->stillSinceMs = in.nowMs;
        st->requested = false;
        st->immediate = false;
    }
    if (!st->requested && in.nowMs - st->stillSinceMs >= kSealDelayMs) {
        st->requested = true;
        st->immediate = false;
        return SealAction::kRequestEngage;
    }
    return SealAction::kNone;
}

// Built v14.4 (the flicker fix): the pad's motion against its shell, measured along the shaft. The reference is captured ONCE, at the first sample after
// the seal is held, by the same function and inputs as every later sample (the v14.3 flight took it from the gather snapshot, a different centre by a
// constant: the pad read 2.4 m off its shell at once). A sample whose centre sources differ from the reference's is another centre: it is not
// compared, and the run is kept. A release needs the offset beyond kPadMoveReleaseM on kPadMoveFrames consecutive samples: motion is continuous.
constexpr uint32_t kPadMoveFrames = 10;

struct ShellMotion {
    bool captured = false;
    float ref = 0.0f;        // the pad's height along the shaft above the shell, at the capture
    uint8_t padSrc = 4;      // the centre sources the reference was read from
    uint8_t shellSrc = 4;
    uint32_t run = 0;        // consecutive samples beyond the limit
    float delta = 0.0f;      // the latest sample minus the reference
    // Returns true when the pad has been beyond the limit for kPadMoveFrames samples in a row.
    bool sample(const float pad[3], uint8_t ps, const float shell[3], uint8_t ss, const float up[3]) noexcept {
        if (!captured) {
            ref = shellRelative(pad, shell, up);
            padSrc = ps;
            shellSrc = ss;
            captured = true;
            run = 0;
            delta = 0.0f;
            return false;
        }
        if (ps != padSrc || ss != shellSrc) return false;
        const float now = shellRelative(pad, shell, up);
        delta = now - ref;
        run = padMovedAway(ref, now) ? run + 1 : 0;
        return run >= kPadMoveFrames;
    }
};

// Built v14.4 (the safety net): more than kSealRateMax seals in kSealRateWindowMs, in one hangar, stops the sealing until a hangar change or a
// setting toggle. A user must never see the sun flicker from this feature again.
constexpr uint32_t kSealRateMax = 6;
constexpr uint64_t kSealRateWindowMs = 60000;

struct SealRateGuard {
    uint64_t times[kSealRateMax + 1] = {};
    uint32_t n = 0;
    uint32_t head = 0;
    bool tripped = false;
    // Records a seal request at now. Returns true when it trips the guard (the first time, and every time after while tripped).
    bool note(uint64_t now) noexcept {
        if (tripped) return true;
        times[head] = now;
        head = (head + 1) % (kSealRateMax + 1);
        if (n < kSealRateMax + 1) ++n;
        uint32_t inWindow = 0;
        for (uint32_t i = 0; i < n; ++i) {
            if (now >= times[i] && now - times[i] <= kSealRateWindowMs) ++inWindow;
        }
        if (inWindow > kSealRateMax) tripped = true;
        return tripped;
    }
};

// Built v14 (fix 1): the own pad is sticky. Once chosen it stays the own pad while its record is in the store; another pad can replace it only after
// the held one has been unseen for kHeldLostFrames frames (the hangar-change rule). While it is unseen and not yet replaced there is no pick.
struct OwnPadSticky {
    uintptr_t pad = 0;      // the sticky own pad's record (0: none chosen yet)
    uint32_t unseen = 0;    // frames the sticky pad has been absent
};

inline void pickOwnPadSticky(const PadObs* obs, size_t n, const float camera[3], const float radii[3], OwnPadSticky* st, OwnCell* out) noexcept {
    if (st->pad != 0) {
        for (size_t i = 0; i < n; ++i) {
            if (obs[i].addr != st->pad || obs[i].size == PadSize::kNone) continue;
            st->unseen = 0;
            *out = OwnCell{};
            fillOwnFromPad(obs[i], radii, distance(obs[i].centre, camera), out);
            return;
        }
        if (++st->unseen < kHeldLostFrames) {
            *out = OwnCell{};
            std::snprintf(out->reason, sizeof(out->reason), "the own pad is not in the store this frame (%u of %u frames before another can take over)",
                          static_cast<unsigned>(st->unseen), static_cast<unsigned>(kHeldLostFrames));
            return;
        }
        st->pad = 0;
        st->unseen = 0;
    }
    pickOwnPad(obs, n, camera, radii, out);
    if (out->has) st->pad = out->pad;
}

// Built v14 (fix 2): the shell the pad's motion is checked against, chosen once at the freeze. A usable shell is a Hanger or Hangar part that is not a
// gate, a ramp or a skirt. The part named for the pad's size (HangerSmall, HangerMedium, HangerLarge) comes first, then the other sized hangers, then
// the rest; within each rank the nearest to the pad. Returns n when none qualifies.
inline bool hangerWord(const char* n) noexcept {
    return std::strstr(n, "Hanger") != nullptr || std::strstr(n, "Hangar") != nullptr;
}

inline bool shellNameUsable(const char* n) noexcept {
    if (!hangerWord(n)) return false;
    return std::strstr(n, "Gate") == nullptr && std::strstr(n, "Ramp") == nullptr && std::strstr(n, "Skirt") == nullptr;
}

inline int shellRank(const char* n, PadSize size) noexcept {
    const char* word = size == PadSize::kSmall ? "Small" : size == PadSize::kMedium ? "Medium" : size == PadSize::kLarge ? "Large" : nullptr;
    if (word != nullptr && std::strstr(n, word) != nullptr) return 0;
    if (std::strstr(n, "Small") != nullptr || std::strstr(n, "Medium") != nullptr || std::strstr(n, "Large") != nullptr) return 1;
    return 2;
}

inline size_t pickShell(const ExplicitObs* obs, size_t n, const float centre[3], PadSize size, float maxM) noexcept {
    size_t best = n;
    int bestRank = 9;
    float bestD = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        if (!shellNameUsable(obs[i].name)) continue;
        const float d = distance(obs[i].centre, centre);
        if (d > maxM) continue;
        const int r = shellRank(obs[i].name, size);
        if (best == n || r < bestRank || (r == bestRank && d < bestD)) {
            best = i;
            bestRank = r;
            bestD = d;
        }
    }
    return best;
}

// Built v14.1: what the store observer does with a record. Judging (the keep and the cull) is sealed-only. Gathering (the camera, the frame's
// pads, the explicit parts and so the own-pad pick the seal trigger needs) runs while docked and unsealed, so a pad is in hand before the seal.
// Setting off: nothing. The v14 flight never sealed because the gathering was inside the sealed-only judge (no pad, so no seal, ever).
enum class StoreMode : uint8_t { kNone = 0, kGather = 1, kJudge = 2 };

inline StoreMode storeMode(bool setting, bool isSealed, bool docked) noexcept {
    if (!setting) return StoreMode::kNone;
    if (isSealed) return StoreMode::kJudge;
    return docked ? StoreMode::kGather : StoreMode::kNone;
}

// Built v14.1: the waiting line's inputs, as they stand this frame. The still time is not a state change (it runs every frame).
struct WaitView {
    bool dockedKnown = false;   // Status.json has a Flags sample
    bool docked = false;        // Flags bit 0
    bool havePad = false;       // the own pad is picked this frame
    char padName[64] = {};      // when havePad
    bool lowered = false;       // the pad is lowered: its height above the shell is below kLoweredM (built v14.5)
    float relM = 0.0f;          // the pad's height above the shell along its up axis, metres
    bool relOk = false;         // relM is readable this frame
    float stillS = 0.0f;        // seconds the stillness timer has run
    bool cellBuilt = false;     // the pad's cell can be built (a pad with a radius and a readable height)
    char cellReason[96] = {};   // when not built: why
    // Built v14.2: the evidence (what the gather saw). gathered: store records the observer took last frame; padCount: own-pad records in hand;
    // padEv: the first three of them, with the distance from the camera and whether their own sphere contains it.
    uint32_t gathered = 0;
    bool cameraKnown = false;
    bool descent = false;       // the held pad has moved since first seen (the delay applies to its seal)
    uint32_t padCount = 0;
    struct PadEvidence {
        char name[64] = {};
        float d = -1.0f;
        bool contains = false;
    };
    PadEvidence padEv[3];
};

// True when the seal's state changed: docked, the pad, at lowest, the cell.
inline bool waitingKeyChanged(const WaitView& a, const WaitView& b) noexcept {
    return a.dockedKnown != b.dockedKnown || a.docked != b.docked || a.havePad != b.havePad || std::strcmp(a.padName, b.padName) != 0 ||
           a.lowered != b.lowered || a.cellBuilt != b.cellBuilt || std::strcmp(a.cellReason, b.cellReason) != 0;
}

// The waiting line, with the unmet condition in its words: docked, pad, at lowest, still for X of the delay, cell.
inline void waitingLine(const WaitView& w, float delayS, char* out, size_t cap) noexcept {
    const char* docked = !w.dockedKnown ? "unknown" : (w.docked ? "yes" : "no");
    char cell[128];
    if (w.cellBuilt) std::snprintf(cell, sizeof(cell), "built");
    else std::snprintf(cell, sizeof(cell), "failed:%s", w.cellReason[0] != '\0' ? w.cellReason : "no cell");
    char pads[400] = {};
    if (w.padCount == 0) {
        std::snprintf(pads, sizeof(pads), "none");
    } else {
        size_t used = 0;
        for (uint32_t i = 0; i < w.padCount && i < 3; ++i) {
            if (used >= sizeof(pads) - 1) break;
            const int n = std::snprintf(pads + used, sizeof(pads) - used, "%s%s d=%.1f contains=%s", i > 0 ? "; " : "", w.padEv[i].name,
                                        static_cast<double>(w.padEv[i].d), w.padEv[i].contains ? "yes" : "no");
            if (n < 0) break;
            used += static_cast<size_t>(n);
        }
    }
    char lowerText[64] = {};
    if (!w.relOk) std::snprintf(lowerText, sizeof(lowerText), "unknown");
    else std::snprintf(lowerText, sizeof(lowerText), "%s (%.1f m above the shell)", w.lowered ? "yes" : "no", static_cast<double>(w.relM));
    std::snprintf(out, cap, "optimized station: waiting (docked=%s, pad=%s, lowered=%s, still for %.1f of %.0f s, cell=%s; descent=%s; gathered=%u, camera=%s, pads=%u [%s])",
                  docked, w.havePad ? w.padName : "none", lowerText, static_cast<double>(w.stillS), static_cast<double>(delayS), cell, w.descent ? "seen" : "none yet",
                  static_cast<unsigned>(w.gathered), w.cameraKnown ? "yes" : "no", static_cast<unsigned>(w.padCount), pads);
}

// Built v14.2: promoteFrame runs once per frame, at the frame's first record. The frame stamp advances at each tick; it never did (the v14.1
// flight: the stamp stayed 0 and the cell stamp started at 0, so no promote ever ran, and no pad was ever picked).
inline bool framePromoteDue(uint32_t cellStamp, uint32_t frameStamp) noexcept { return cellStamp != frameStamp; }

// The position of p along the cell box's own axes (u, v, w), when the cell is a box. False otherwise.
inline bool boxCoordsOf(const OwnCell& c, const float p[3], float coords[3]) noexcept {
    if (!c.box) return false;
    const float d[3] = {p[0] - c.boxCentre[0], p[1] - c.boxCentre[1], p[2] - c.boxCentre[2]};
    coords[0] = d[0] * c.boxU[0] + d[1] * c.boxU[1] + d[2] * c.boxU[2];
    coords[1] = d[0] * c.boxV[0] + d[1] * c.boxV[1] + d[2] * c.boxV[2];
    coords[2] = d[0] * c.boxW[0] + d[1] * c.boxW[1] + d[2] * c.boxW[2];
    return true;
}

// ---- Names: the text a record's name is read from (the census's accept rules, asset_census_core.h) ----
constexpr size_t kNameBytes = 96;                    // bytes read at a pointer when looking for text

constexpr size_t kNameMax = 48;                      // longest text kept

// ---------------------------------------------------------------------------------------------------------------------
// Names: text reached through a pointer in an object's first 0x100 bytes. Accepted: 4 to 48 characters from the allowed set,
// as ASCII or as UTF-16LE with ASCII code units, NUL-terminated within kNameBytes. Anything else is not a name.
inline bool nameChar(uint8_t c) noexcept {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '/' ||
           c == ' ' || c == ':' || c == '(' || c == ')' || c == '#' || c == '+' || c == '-';
}

// Writes the text found in bytes (ascii or utf16) to out. False when the bytes are not a name.
inline bool textName(const uint8_t* bytes, size_t n, bool wide, std::string* out) {
    std::string text;
    const size_t step = wide ? 2 : 1;
    for (size_t i = 0; i + step <= n; i += step) {
        const uint8_t c = bytes[i];
        if (wide && bytes[i + 1] != 0) return false;
        if (c == 0) {
            if (text.size() < 4 || text.size() > kNameMax) return false;
            *out = text;
            return true;
        }
        if (!nameChar(c) || text.size() >= kNameMax) return false;
        text.push_back(static_cast<char>(c));
    }
    return false;
}

}  // namespace edvr::sealed
