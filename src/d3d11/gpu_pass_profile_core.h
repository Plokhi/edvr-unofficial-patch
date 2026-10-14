#pragma once
// Pure logic for the GPU pass profiler (gpu_pass_profile.h): what a pass IS
// (classification), how cube-face runs group, the stamp budget, the per-pass
// shader tally, the summary arithmetic and the CSV text. Nothing here touches
// D3D11 or Win32, so tools\gpu_pass_profile_test compiles it alone. The rules
// are stated in docs\gpu-pass-profile-2026-10-12.md; change them there too.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace edvr::gpupass {

constexpr uint32_t kDefaultCaptureFrames = 30; // whole frames per press when the hotkey's value names no count
constexpr uint32_t kMaxCaptureFrames = 120;    // the cap on a requested count; the pool is sized to what is captured
constexpr uint32_t kMaxStampsPerFrame = 4096;  // timestamp queries per frame; a boundary past the cap is counted, never issued
constexpr uint32_t kHashSlots = 8;             // distinct VS/PS pairs (or CS hashes) tracked per pass; the rest fold into one count
constexpr uint32_t kTopHashes = 3;             // shader pairs listed per pass
constexpr uint32_t kTopPasses = 20;            // passes in the summary's top list, by median
constexpr uint32_t kCubeRun = 6;               // consecutive same-size square passes read as one cube map

// The hotkey's value is "KEY" or "KEY COUNT" (or "Ctrl+KEY COUNT"). A trailing whole number after whitespace is the frame count, clamped
// to 1..kMaxCaptureFrames; without one the count is kDefaultCaptureFrames. The number is not taken when nothing precedes it, so a key
// named by a digit alone still binds. keyOut receives the value without the count (what the hotkey binds).
inline uint32_t splitCaptureSpec(const std::string& value, std::string& keyOut) {
    keyOut = value;
    size_t end = value.find_last_not_of(" \t");
    if (end == std::string::npos) return kDefaultCaptureFrames;
    size_t start = value.find_last_of(" \t", end);
    if (start == std::string::npos) return kDefaultCaptureFrames;
    const std::string tail = value.substr(start + 1, end - start);
    const std::string head = value.substr(0, start);
    if (head.find_first_not_of(" \t") == std::string::npos) return kDefaultCaptureFrames;
    if (tail.empty() || tail.size() > 6) return kDefaultCaptureFrames;
    for (char c : tail)
        if (c < '0' || c > '9') return kDefaultCaptureFrames;
    keyOut = head.substr(0, head.find_last_not_of(" \t") + 1);
    const unsigned long n = std::stoul(tail);
    if (n < 1) return 1;
    if (n > kMaxCaptureFrames) return kMaxCaptureFrames;
    return static_cast<uint32_t>(n);
}

// The timestamp pool holds one stamp array per captured frame. Pool frames to create for a capture of `frames`: the capture's own count,
// clamped the same way. A pool already at least that large is reused.
inline uint32_t poolFramesFor(uint32_t frames) {
    if (frames < 1) return 1;
    return frames > kMaxCaptureFrames ? kMaxCaptureFrames : frames;
}
inline bool poolNeedsGrowth(uint32_t have, uint32_t frames) { return have < poolFramesFor(frames); }

enum class PassClass : uint8_t {
    Edvr = 0,         // inside an EDVR census section: EDVR's own work, excluded from Elite's total
    Compute,          // dispatches and no draws
    Clear,            // clears and no draws
    Shadow,           // depth only, square, >= 512 or an array of slices
    ProbeCube,        // a square colour target of a cube resource, or one of kCubeRun face passes in a row
    SceneGBuffer,     // two or more colour targets at the eye size, or a depth-only pass at the eye size
    FullscreenAtEye,  // one colour target at the eye size with one or two draws (a full-screen triangle or quad)
    ForwardAtEye,     // one colour target at the eye size with three or more draws
    PostSmaller,      // draws into a colour target smaller than the eye (downsampled post chains)
    HudOverlay,       // Built v13.7: one to kHudMaxDraws draws whose most-drawn VS is a known HUD, holo or UI family (the caller's families)
    LightingVolume,   // instanced light volumes drawn into the eye-size target (the caller's lighting family, most-drawn VS)
    Other,
    Count
};
inline constexpr const char* kPassClassNames[] = {
    "edvr", "compute", "clear", "shadow", "probe_cube", "scene_gbuffer",
    "fullscreen_at_eye", "forward_at_eye", "post_smaller", "hud_overlay", "lighting_volume", "other"};
static_assert(sizeof(kPassClassNames) / sizeof(kPassClassNames[0]) == static_cast<size_t>(PassClass::Count),
              "one name for each pass class");

inline const char* passClassName(PassClass c) {
    const size_t i = static_cast<size_t>(c);
    return i < static_cast<size_t>(PassClass::Count) ? kPassClassNames[i] : "other";
}

// One bound target, as the classifier and the CSV need it. w and h are the
// view's own mip size; slices is the array size the view addresses (1 for a
// plain 2D view); cube is whether the underlying resource is a cube map.
struct TargetShape {
    uint32_t w = 0, h = 0;
    uint32_t format = 0;   // DXGI_FORMAT
    uint32_t slices = 1;
    bool cube = false;
    bool square() const { return w != 0 && w == h; }
};

// Everything a pass segment has accumulated, before classification.
struct PassFacts {
    bool edvr = false;          // inside an EDVR census section
    uint32_t colours = 0;       // non-null colour targets bound
    TargetShape colour;         // the first of them
    bool hasDepth = false;
    TargetShape depth;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    uint64_t topVs = 0;         // Built v13.7: the segment's most-drawn vertex shader (its content hash); 0 when it drew nothing
};

// A set of vertex shader hashes a class is known by, taken by the caller from the game headers (eye_draw_snapshot.h, holo_material.h,
// holo_families.h, and the lighting shapes of reviews\sealed-remaining-costs-2026-10-13.md) so this core stays free of them.
constexpr uint32_t kFamilySlots = 4;
constexpr uint32_t kHudMaxDraws = 4;        // a HUD overlay is a few draws: more than this is a forward or post pass
struct VsFamily {
    uint64_t vs[kFamilySlots] = {};
    uint32_t n = 0;
};
inline bool isVsFamily(uint64_t vs, const VsFamily& fam) noexcept {
    if (vs == 0) return false;
    for (uint32_t i = 0; i < fam.n && i < kFamilySlots; ++i)
        if (fam.vs[i] == vs) return true;
    return false;
}

// Built v13.7 (1): a known compute shader, by its content hash, from the repo's own catalogues (skin_ledger.h's kChainHash and kClearHash, given
// by the caller).
struct ComputeName {
    uint64_t hash = 0;
    const char* name = "";
};

struct EyeSize {
    uint32_t w = 0, h = 0;
};

inline std::string targetText(const TargetShape& t) {
    if (!t.w) return "-";
    return std::to_string(t.w) + "x" + std::to_string(t.h);
}

// The class of one segment. Order matters: the first rule that holds wins.
inline PassClass classifyPass(const PassFacts& f, EyeSize eye, const VsFamily& hud = VsFamily{},
                              const VsFamily& lighting = VsFamily{}) {
    if (f.edvr) return PassClass::Edvr;
    if (f.draws == 0 && f.dispatches > 0) return PassClass::Compute;
    if (f.draws == 0) return f.clears > 0 ? PassClass::Clear : PassClass::Other;
    const bool eyeColour = eye.w != 0 && f.colour.w == eye.w && f.colour.h == eye.h;
    if (f.colours == 0 && f.hasDepth) {
        // Built v13.7 (2): a depth-only map that is not at the eye size is a shadow map, square or not, large or small (the 2048x1024 D16 pair and
        // the 256x256 D16 map were other). With no eye size known, only a square map of 512 or more, or an array of slices, is one.
        if (eye.w != 0 && f.depth.w == eye.w && f.depth.h == eye.h) return PassClass::SceneGBuffer;
        if (eye.w != 0) return PassClass::Shadow;
        if (f.depth.square() && (f.depth.w >= 512 || f.depth.slices > 1)) return PassClass::Shadow;
        return PassClass::Other;
    }
    // Built v13.7 (3): a few draws whose most-drawn vertex shader is a known HUD, holo or UI family: its own class, so the HUD and holo cost is
    // not counted as fullscreen_at_eye.
    if (f.colours >= 1 && f.draws <= kHudMaxDraws && isVsFamily(f.topVs, hud)) return PassClass::HudOverlay;
    if (f.colours >= 1 && f.colour.square() && f.colour.cube) return PassClass::ProbeCube;
    // The light volumes are instanced draws into the eye-size target, so the lighting family is tested before the
    // two-target rule that would otherwise name the lighting pass scene_gbuffer.
    if (eyeColour && isVsFamily(f.topVs, lighting)) return PassClass::LightingVolume;
    if (f.colours >= 2 && eyeColour) return PassClass::SceneGBuffer;
    if (eyeColour) return f.draws <= 2 ? PassClass::FullscreenAtEye : PassClass::ForwardAtEye;
    if (eye.w != 0 && f.colours >= 1 &&
        static_cast<uint64_t>(f.colour.w) * f.colour.h < static_cast<uint64_t>(eye.w) * eye.h)
        return PassClass::PostSmaller;
    return PassClass::Other;
}

// A cube map drawn face by face is kCubeRun or more consecutive passes, each
// one colour target, square, drawn into, all the same size. Relabels those
// runs ProbeCube in place. cls[i] is the class of facts[i].
inline bool cubeCandidate(const PassFacts& f) {
    return !f.edvr && f.colours == 1 && f.colour.square() && f.draws > 0;
}
inline void markCubeRuns(const std::vector<PassFacts>& facts, std::vector<PassClass>& cls) {
    size_t i = 0;
    while (i < facts.size()) {
        if (!cubeCandidate(facts[i])) { ++i; continue; }
        size_t j = i + 1;
        while (j < facts.size() && cubeCandidate(facts[j]) && facts[j].colour.w == facts[i].colour.w) ++j;
        if (j - i >= kCubeRun)
            for (size_t k = i; k < j; ++k) cls[k] = PassClass::ProbeCube;
        i = j;
    }
}

// One stamp budget per frame. take(reserve) issues a stamp only while at
// least reserve stamps stay free after it, so a boundary (reserve 1) can never
// take the slot the frame's end needs (reserve 0). A refused stamp is counted.
struct StampBudget {
    uint32_t cap = kMaxStampsPerFrame;
    uint32_t used = 0, overflow = 0;
    bool take(uint32_t reserve) {
        if (used + reserve >= cap) { ++overflow; return false; }
        ++used;
        return true;
    }
};

struct HashTally {
    uint64_t vs = 0, ps = 0;
    uint32_t draws = 0;
};

// The shader pairs a pass drew with, by draw count. Eight slots; a pair past
// them is folded into one count, so a pass that uses many shaders still
// reports its top three correctly for the heavy ones.
class PassHashes {
public:
    void note(uint64_t vs, uint64_t ps) {
        for (uint32_t i = 0; i < used_; ++i)
            if (slots_[i].vs == vs && slots_[i].ps == ps) { ++slots_[i].draws; return; }
        if (used_ < kHashSlots) { slots_[used_++] = HashTally{vs, ps, 1}; return; }
        ++folded_;
    }
    uint32_t folded() const { return folded_; }
    // Every tracked pair (not only the top kTopHashes), for the summary's totals.
    std::vector<HashTally> all() const { return std::vector<HashTally>(slots_, slots_ + used_); }
    std::vector<HashTally> top() const {
        std::vector<HashTally> out(slots_, slots_ + used_);
        std::stable_sort(out.begin(), out.end(),
                         [](const HashTally& a, const HashTally& b) { return a.draws > b.draws; });
        if (out.size() > kTopHashes) out.resize(kTopHashes);
        return out;
    }
private:
    HashTally slots_[kHashSlots]{};
    uint32_t used_ = 0, folded_ = 0;
};

// One row of the CSV: one pass segment of one captured frame.
struct Row {
    uint32_t frame = 0, index = 0;
    PassClass cls = PassClass::Other;
    bool edvr = false;
    double ms = 0;
    uint32_t draws = 0, dispatches = 0, clears = 0;
    uint64_t tris = 0;          // estimate: count / 3 x instances for the triangle draws; 0 otherwise
    uint32_t colours = 0;
    std::string rt = "-", dsv = "-", top = "-";
    std::string topCs = "-";   // Built v13.7 (1): the segment's most-dispatched compute shaders, "hash(NAME):count;..."; "-" when none
    // The pass's shader identity without counts, so the same pass in another frame groups with it: "vs:ps" of the most-drawn pair, or
    // "cs:hash(NAME)" of the most-dispatched compute shader for a dispatch-only pass; "-" when neither.
    std::string ident = "-";
    std::vector<HashTally> cs;   // every compute shader this pass dispatched with, by dispatch count (the summary's top_cs_dispatches totals)
    uint32_t rtFormat = 0, dsvFormat = 0, slices = 1;
    bool cube = false;
    // The grouping key: class, target signature and shader identity, not the frame or the index.
    std::string key() const {
        return std::string(passClassName(cls)) + " rt=" + rt + " colours=" + std::to_string(colours) +
               " slices=" + std::to_string(slices) + (cube ? " cube" : "") + " dsv=" + dsv + " shader=" + ident;
    }
};

inline std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

inline const char* csvHeader() {
    return "frame,pass,class,edvr,ms,draws,dispatches,clears,tris,colours,rt,rt_fmt,slices,cube,dsv,dsv_fmt,top_vs_ps_draws,top_cs_dispatches";
}

inline std::string csvRow(const Row& r) {
    char ms[32];
    std::snprintf(ms, sizeof ms, "%.4f", r.ms);
    std::string out;
    out += std::to_string(r.frame) + "," + std::to_string(r.index) + "," + passClassName(r.cls) + "," +
           (r.edvr ? "1" : "0") + "," + ms + "," + std::to_string(r.draws) + "," +
           std::to_string(r.dispatches) + "," + std::to_string(r.clears) + "," + std::to_string(r.tris) + "," +
           std::to_string(r.colours) + "," + csvField(r.rt) + "," + std::to_string(r.rtFormat) + "," +
           std::to_string(r.slices) + "," + (r.cube ? "1" : "0") + "," + csvField(r.dsv) + "," +
           std::to_string(r.dsvFormat) + "," + csvField(r.top) + "," + csvField(r.topCs);
    return out;
}

// "vs:ps:draws;..." with the hashes in hex. "-" when the pass drew nothing.
inline std::string topText(const std::vector<HashTally>& top) {
    if (top.empty()) return "-";
    std::string out;
    char buf[64];
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) out += ';';
        std::snprintf(buf, sizeof buf, "%016llx:%016llx:%u", static_cast<unsigned long long>(top[i].vs),
                      static_cast<unsigned long long>(top[i].ps), top[i].draws);
        out += buf;
    }
    return out;
}

// Built v13.7 (1): "hash(NAME):count;..." for the compute shaders a pass dispatched with, most dispatches first. The name is in parentheses when the
// caller's catalogue knows the hash; the hash is the bound compute shader's content hash in hex.
inline std::string csText(const std::vector<HashTally>& top, const ComputeName* names, size_t nNames) {
    if (top.empty()) return "-";
    std::string out;
    for (size_t i = 0; i < top.size(); ++i) {
        if (i) out += ';';
        const char* name = "";
        for (size_t k = 0; k < nNames; ++k)
            if (names[k].hash == top[i].vs) {
                name = names[k].name;
                break;
            }
        char buf[96];
        if (*name)
            std::snprintf(buf, sizeof buf, "%016llx(%s):%u", static_cast<unsigned long long>(top[i].vs), name, top[i].draws);
        else
            std::snprintf(buf, sizeof buf, "%016llx:%u", static_cast<unsigned long long>(top[i].vs), top[i].draws);
        out += buf;
    }
    return out;
}

// GPU pass profile v2: the shader identity a pass groups by (Row::ident). A draw pass names its most-drawn VS:PS pair; a dispatch-only pass
// (no draws) names its most-dispatched compute shader, with the name when the catalogue knows it. A zero hash is never named: it is "-".
inline std::string identText(const std::vector<HashTally>& pairs, const std::vector<HashTally>& cs, const ComputeName* names,
                             size_t nNames) {
    char buf[96];
    if (!pairs.empty() && pairs[0].vs) {
        std::snprintf(buf, sizeof buf, "%016llx:%016llx", static_cast<unsigned long long>(pairs[0].vs),
                      static_cast<unsigned long long>(pairs[0].ps));
        return buf;
    }
    if (!cs.empty() && cs[0].vs) {
        const char* name = "";
        for (size_t k = 0; k < nNames; ++k)
            if (names[k].hash == cs[0].vs) {
                name = names[k].name;
                break;
            }
        if (*name)
            std::snprintf(buf, sizeof buf, "cs:%016llx(%s)", static_cast<unsigned long long>(cs[0].vs), name);
        else
            std::snprintf(buf, sizeof buf, "cs:%016llx", static_cast<unsigned long long>(cs[0].vs));
        return buf;
    }
    return "-";
}

struct ClassMean {
    double ms = 0, passes = 0;   // per frame
    double median = 0;           // Built v13.7 (4): the median of one pass's ms over the frames read (a single spike does not move it)
};
constexpr uint32_t kTopCs = 5;             // compute shaders listed in the summary's top_cs_dispatches
struct CsTotal {
    uint64_t hash = 0;
    double perFrame = 0;     // dispatches per captured frame, mean
};
struct Summary {
    struct Group {
        double ms = 0, passes = 0, draws = 0;
        double median = 0;       // Built v13.7 (4): the median of one pass's ms in this group over the frames read
        PassClass cls = PassClass::Other;
        std::string sig;
    };
    uint32_t frames = 0;                   // captured frames the means are over
    double frameMs = 0, eliteMs = 0, edvrMs = 0;   // per frame, mean
    ClassMean cls[static_cast<size_t>(PassClass::Count)]{};
    std::vector<Group> top;                // the passes by median, most first, at most kTopPasses (ms and the per-frame means are per group)
    std::vector<CsTotal> cs;               // GPU pass profile v2: the compute shaders by dispatches per frame, most first, at most kTopCs
};

// Built v13.7 (4): the median of a list (the mean of the two middle values for an even count); 0 for an empty list.
inline double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

// frameMs[f] is frame f's span; frameOk[f] is false for a frame whose result
// was not read (its rows must be absent). Every mean is over the frames that
// were read, so a missing frame is said, not averaged in as zero.
inline Summary summarise(const std::vector<Row>& rows, const std::vector<double>& frameMs,
                         const std::vector<bool>& frameOk) {
    Summary s;
    double frameSum = 0;
    for (size_t f = 0; f < frameOk.size(); ++f)
        if (frameOk[f]) {
            ++s.frames;
            frameSum += f < frameMs.size() ? frameMs[f] : 0;
        }
    if (!s.frames) return s;
    const double n = static_cast<double>(s.frames);
    std::map<std::string, Summary::Group> groups;
    std::map<std::string, std::vector<double>> groupSamples;              // Built v13.7 (4): one pass's ms per row, for the medians
    std::vector<double> clsSamples[static_cast<size_t>(PassClass::Count)];
    double elite = 0, edvr = 0;
    std::map<uint64_t, double> csCount;
    for (const Row& r : rows) {
        if (r.frame >= frameOk.size() || !frameOk[r.frame]) continue;
        for (const HashTally& h : r.cs) csCount[h.vs] += h.draws;
        ClassMean& c = s.cls[static_cast<size_t>(r.cls)];
        c.ms += r.ms;
        c.passes += 1;
        clsSamples[static_cast<size_t>(r.cls)].push_back(r.ms);
        groupSamples[r.key()].push_back(r.ms);
        (r.edvr ? edvr : elite) += r.ms;
        Summary::Group& g = groups[r.key()];
        g.ms += r.ms;
        g.passes += 1;
        g.draws += r.draws;
        g.cls = r.cls;
        g.sig = "rt " + r.rt + " colours " + std::to_string(r.colours) + " slices " +
                std::to_string(r.slices) + (r.cube ? " cube" : "") + " depth " + r.dsv + " shader " + r.ident;
    }
    s.frameMs = frameSum / n;
    s.eliteMs = elite / n;
    s.edvrMs = edvr / n;
    for (size_t c = 0; c < static_cast<size_t>(PassClass::Count); ++c) {
        s.cls[c].ms /= n;
        s.cls[c].passes /= n;
        s.cls[c].median = medianOf(clsSamples[c]);
    }
    for (auto& kv : groups) {
        Summary::Group g = kv.second;
        g.ms /= n;
        g.passes /= n;
        g.draws /= n;
        g.median = medianOf(groupSamples[kv.first]);
        s.top.push_back(g);
    }
    // GPU pass profile v2: the top list is by median pass time, so one spiked frame does not place a pass in it.
    std::stable_sort(s.top.begin(), s.top.end(),
                     [](const Summary::Group& a, const Summary::Group& b) { return a.median > b.median; });
    if (s.top.size() > kTopPasses) s.top.resize(kTopPasses);
    for (const auto& kv : csCount) s.cs.push_back(CsTotal{kv.first, kv.second / n});
    std::stable_sort(s.cs.begin(), s.cs.end(), [](const CsTotal& a, const CsTotal& b) { return a.perFrame > b.perFrame; });
    if (s.cs.size() > kTopCs) s.cs.resize(kTopCs);
    return s;
}

// Built v13.6: the shared clock is yielded to the profiler from the press until the batch's last frame is closed: wanted while a capture is armed
// (requested) or has a batch open. The yield flag (gpuTimingSetYield) follows this state; the frame driver and the timers test the flag.
inline bool yieldWanted(bool requested, bool batch) noexcept { return requested || batch; }

}  // namespace edvr::gpupass
