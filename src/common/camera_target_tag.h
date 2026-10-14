#pragma once
// The render target a camera-block write is drawn into, the rule that filters on it (fix.camera_eye_target), the burst offset of a frame and the
// P3 mini-render record (docs\sky-flicker-2026-10-13.md; reviews\skybox-flicker-2026-10-13.md, the second follow-up; vr-view-caches-2026-10-13.md, P3).
//
// WHY. The 1.006 s camera-row excursions are the reflection probe's forward cube-face camera, written into the same scene constant block as the eyes'
// and picked by the chooser whenever it lies within 3.0 deg of the last pick. The chooser sees buffers, not targets: a write carries rows, a
// projection, a frame and an order, and nothing about what is drawn with it. The write precedes its draw, so the target cannot be known when the
// write is made; it is known at the FIRST DRAW AFTER the write, which tags the write (one tag per write, cached per bound view). The chooser runs
// at the first treat of the frame, after every draw of the frame, so every write that was drawn with is tagged by then. A write no draw followed
// is UNKNOWN, and an unknown write is never rejected: the rule only ever removes a write it can show is drawn into something that is not an eye.
//
// "Eye-sized": each axis within a quarter of the eye's render size (the size of the region the temporal pass is treating). The probe faces are 256x256
// against 2016x2016. The sun cascades (2048x1024) and every UI target miss it too.
//
// Pure: no Windows, no globals. The rig is tools\sky_flicker_test.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace edvr::camtag {

// What a write was drawn into. src: 0 none, 1 the first render target, 2 the depth-stencil view (a depth-only pass has no render target).
struct Target {
    uint16_t w = 0, h = 0;
    uint16_t fmt = 0;         // DXGI_FORMAT
    uint8_t src = 0;
    uint8_t colours = 0;      // render targets bound at the tag, 0 to 8
    bool known() const { return src != 0 && w != 0 && h != 0; }
};

inline const char* srcName(uint8_t src) { return src == 1 ? "rtv" : src == 2 ? "dsv" : "-"; }

// "256x256:11/rtv/c4" for a known target, "-" for an unknown one.
inline int formatTarget(char* buf, size_t n, const Target& t) {
    const int len = t.known()
        ? std::snprintf(buf, n, "%ux%u:%u/%s/c%u", t.w, t.h, t.fmt, srcName(t.src), t.colours)
        : std::snprintf(buf, n, "-");
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

// Each axis within a quarter of the eye's. An eye size of zero (not known yet) makes every target eye-sized: the rule never fires blind.
inline bool eyeSized(const Target& t, uint32_t eyeW, uint32_t eyeH) {
    if (!eyeW || !eyeH) return true;
    // (not named near: windows.h defines that away.)
    const auto withinQuarter = [](uint32_t v, uint32_t ref) {
        const uint32_t d = v > ref ? v - ref : ref - v;
        return d * 4u <= ref;
    };
    return withinQuarter(t.w, eyeW) && withinQuarter(t.h, eyeH);
}

// The rule: a known target that is not eye-sized rejects the write. Unknown never rejects.
inline bool rejects(const Target& t, uint32_t eyeW, uint32_t eyeH) {
    return t.known() && !eyeSized(t, eyeW, eyeH);
}

// The frame's verdict over all its writes: fed each write's tag once, then asked whether the filter applies. With nothing eligible (every write tagged
// non-eye: a wrong eye size, or an eye drawn into a target of another shape) it stands down for the frame rather than leave the chooser empty.
struct Filter {
    uint32_t cut = 0;               // writes the rule rejects (counted with the key off too: would-be)
    bool eligibleAny = false;       // at least one write the rule keeps
    void note(const Target& t, uint32_t eyeW, uint32_t eyeH) {
        if (rejects(t, eyeW, eyeH)) ++cut; else eligibleAny = true;
    }
    bool active(bool keyOn) const { return keyOn && cut && eligibleAny; }
    bool failedOpen(bool keyOn) const { return keyOn && cut && !eligibleAny; }
};

// ---- the rule's 30 s line -----------------------------------------------------------------------------------------------------------------------------------

struct SizeCount {
    uint16_t w = 0, h = 0;
    uint32_t n = 0;
};
constexpr int kSizeSlots = 6;

struct Stats {
    uint32_t frames = 0;           // frames the chooser ran
    uint32_t writes = 0;           // camera writes it looked at
    uint32_t tagged = 0;           // ...with a known target
    uint32_t unknown = 0;          // ...with none (no draw followed): kept
    uint32_t rejected = 0;         // ...rejected (key on) or that would be (key off)
    uint32_t rejectFrames = 0;     // frames with at least one rejection
    uint32_t failOpen = 0;         // frames where nothing was eligible, so nothing was rejected (key on)
    SizeCount sizes[kSizeSlots];   // the rejected sizes by count; a seventh size is counted in other
    uint32_t other = 0;
    uint32_t eyeW = 0, eyeH = 0;   // the eye size the last frame was judged against
};

inline void noteRejected(Stats& s, uint16_t w, uint16_t h) {
    ++s.rejected;
    for (SizeCount& c : s.sizes) {
        if (c.n && c.w == w && c.h == h) { ++c.n; return; }
    }
    for (SizeCount& c : s.sizes) {
        if (!c.n) { c.w = w; c.h = h; c.n = 1; return; }
    }
    ++s.other;
}

inline void resetWindow(Stats& s) { s = Stats{}; }

// "temporal camera eye target: frames=.. writes=.. tagged=.. unknown=.. rejected=.. reject_frames=.. fail_open=.. eye=WxH key=on|off rejected_sizes=256x256:12,...".
// With the key off, rejected is what the rule WOULD have rejected. Returns the length written, or 0 when the buffer is too small.
inline int formatReport(char* buf, size_t n, const Stats& s, bool keyOn) {
    int len = std::snprintf(buf, n,
        "temporal camera eye target: frames=%u writes=%u tagged=%u unknown=%u rejected=%u reject_frames=%u fail_open=%u eye=%ux%u key=%s rejected_sizes=",
        s.frames, s.writes, s.tagged, s.unknown, s.rejected, s.rejectFrames, s.failOpen, s.eyeW, s.eyeH, keyOn ? "on" : "off");
    if (len <= 0 || static_cast<size_t>(len) >= n) return 0;
    bool any = false;
    for (const SizeCount& c : s.sizes) {
        if (!c.n) continue;
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "%s%ux%u:%u", any ? "," : "", c.w, c.h, c.n);
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) return 0;
        len += w;
        any = true;
    }
    if (s.other) {
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "%sother:%u", any ? "," : "", s.other);
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) return 0;
        len += w;
        any = true;
    }
    if (!any) {
        const int w = std::snprintf(buf + len, n - static_cast<size_t>(len), "none");
        if (w < 0 || static_cast<size_t>(len) + static_cast<size_t>(w) >= n) return 0;
        len += w;
    }
    return len;
}

// ---- the burst offset ---------------------------------------------------------------------------------------------------------------------------------------
// A frame whose camera-block writes exceed the baseline by more than kBurstExcess is in a burst; its offset is the frames since the burst began (0 on
// the first). The baseline is the median of the previous kBaselineFrames frames' counts (the bursts are 6 of 91 frames, so the median is the quiet
// count), judged once kMinBaselineFrames exist. The offsets are kept for the last kKeepFrames frames, for the dump to join to its rows by frame.

constexpr uint32_t kBurstExcess = 15;
constexpr uint32_t kBaselineFrames = 32;
constexpr uint32_t kMinBaselineFrames = 8;
constexpr uint32_t kKeepFrames = 2048;

class BurstTracker {
public:
    // The frame's full count of camera-block writes, pushed at the frame boundary, frames ascending.
    void push(uint32_t frame, uint32_t writes) {
        uint32_t base = 0;
        const bool judged = baseline(&base);
        bool burst = judged && writes > base + kBurstExcess;
        uint32_t offset = kNone;
        if (burst) offset = inBurst_ ? run_ + 1 : 0;
        inBurst_ = burst;
        run_ = burst ? offset : 0;
        Slot& s = slots_[frame % kKeepFrames];
        s.frame = frame;
        s.writes = writes;
        s.offset = offset;
        s.base = judged ? base : 0;
        s.valid = true;
        counts_[next_ % kBaselineFrames] = writes;
        ++next_;
    }
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    // The frame's burst offset (kNone when it was not in a burst, or is not kept). writes and baseline are out when given.
    uint32_t offsetOf(uint32_t frame, uint32_t* writes = nullptr, uint32_t* base = nullptr) const {
        const Slot& s = slots_[frame % kKeepFrames];
        if (!s.valid || s.frame != frame) return kNone;
        if (writes) *writes = s.writes;
        if (base) *base = s.base;
        return s.offset;
    }
    bool known(uint32_t frame) const {
        const Slot& s = slots_[frame % kKeepFrames];
        return s.valid && s.frame == frame;
    }
private:
    struct Slot {
        uint32_t frame = 0, writes = 0, offset = kNone, base = 0;
        bool valid = false;
    };
    bool baseline(uint32_t* out) const {
        const uint32_t have = next_ < kBaselineFrames ? next_ : kBaselineFrames;
        if (have < kMinBaselineFrames) return false;
        uint32_t tmp[kBaselineFrames];
        std::memcpy(tmp, counts_, sizeof(uint32_t) * have);
        std::sort(tmp, tmp + have);
        *out = tmp[have / 2];
        return true;
    }
    Slot slots_[kKeepFrames];
    uint32_t counts_[kBaselineFrames] = {};
    uint32_t next_ = 0;
    bool inBurst_ = false;
    uint32_t run_ = 0;
};

// " burst=3 wr=114/86" in a burst, " burst=- wr=86/86" outside one, " burst=? wr=?" for a frame not kept.
inline int formatBurst(char* buf, size_t n, const BurstTracker& t, uint32_t frame) {
    uint32_t w = 0, b = 0;
    const uint32_t off = t.offsetOf(frame, &w, &b);
    int len;
    if (!t.known(frame)) len = std::snprintf(buf, n, " burst=? wr=?");
    else if (off == BurstTracker::kNone) len = std::snprintf(buf, n, " burst=- wr=%u/%u", w, b);
    else len = std::snprintf(buf, n, " burst=%u wr=%u/%u", off, w, b);
    return (len <= 0 || static_cast<size_t>(len) >= n) ? 0 : len;
}

}  // namespace edvr::camtag
