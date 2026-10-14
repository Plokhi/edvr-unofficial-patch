#include "glitch_frame.h"

#include <atomic>
#include <cmath>
#include <cstdio>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "transition_flash_eye_base.h"

// The camera-buffer tap of the transition-flash engine fix (glitch_frame.h).
//
// The old independent detector that lived here -- a jump chain over the camera's
// position, a separation memory, certified orbits and parks, a pool-coherence
// cross-check, a camera-history ring and its dump -- was removed on 2026-10-13
// (docs/transition-flash.md). It ran only when the engine fix was not armed, and
// it was dormant in every flight that mattered.
namespace edvr {
namespace {

struct State {
    // The camera buffer is observed: its size is nonzero and three floats fit at
    // the position offset inside it. Set on every install that passes the check.
    bool observing = false;
    uint32_t bufferBytes = 0;
    uint32_t posOffset = 0;
    // Frames since install, advanced at the frame boundary. A fill carries the
    // PRE-advance value (the boundary that closes the frame records it one
    // higher); the engine fix compensates.
    uint32_t frameNo = 0;
};

State* g_state = nullptr;

// The engine fix reported Armed. Until it does (and for good when it cannot),
// the tap asks for no buffer and records nothing.
std::atomic<bool> g_engineFixArmed{false};
// The engine fix's event window is open (armed event .. its two bad renders):
// the camera CB's Unmap tap is the one place the fix looks at a frame.
std::atomic<bool> g_engineWindowOpen{false};

bool finite3(const float* p) {
    for (uint32_t a = 0; a < 3; ++a) {
        if (!std::isfinite(p[a])) return false;
    }
    return true;
}

}  // namespace

void glitchFrameEngineWindow(bool open) {
    g_engineWindowOpen.store(open, std::memory_order_relaxed);
}

void glitchFrameNoteEngineFix(bool armed) {
    g_engineFixArmed.store(armed, std::memory_order_relaxed);
}

// The engine fix marks frames from the camera-buffer tap inside
// glitchFrameObserve, so "armed" means the tap can deliver. It reaches the
// engine module only when the buffer is observed (size nonzero and the position
// offset inside it -- install below), Map nominates that buffer by size
// (bufferBytes == the engine's 5376-byte scene CB), and the position passes the
// finite check per fill. This answers the static part.
bool glitchFrameEngineTapAvailable(char* why, size_t whySize) {
    State* s = g_state;
    if (!s) { snprintf(why, whySize, "the tap is not installed"); return false; }
    if (!s->observing) {
        snprintf(why, whySize, "the camera buffer is not observed -- camera_buffer_bytes = %u and "
                 "camera_buffer_offset = %u do not describe a buffer holding three floats (see the "
                 "line above)", s->bufferBytes, s->posOffset);
        return false;
    }
    if (s->bufferBytes != 5376) {
        snprintf(why, whySize, "camera_buffer_bytes = %u, but the engine fix reads the game's 5376-byte "
                 "scene buffer", s->bufferBytes);
        return false;
    }
    return true;
}

bool glitchFrameEngineFixHold() {
    // A camera jump: the FSS arrival trigger and the openvr half key on it.
    noteWorldJump();
    markGlitchFrame();
    // The jump verdict is NOT published here: see glitchFrameEngineFixVerdict.
    return glitchConsumerPresent();
}

void glitchFrameEngineFixVerdict() {
    // "Stayed": a change of reference frame, the next frame is coherent, so
    // the temporal pass keeps its history.
    noteJumpVerdict(2);
}

void installGlitchFrameFix() {
    static State s;
    g_state = &s;
    // A re-install on the same static State (the test rigs, a hot reload) must
    // not inherit a stale "observing" from a geometry that has since been refused.
    s.observing = false;

    Config& cfg = Config::get();
    s.bufferBytes = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_bytes", 5376));
    s.posOffset = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_offset", 1100));

    if (s.bufferBytes == 0) {
        Log::get().note("transition flash: camera-buffer tap off: the camera buffer size is zero.");
        return;
    }
    // Three floats have to fit at that offset, inside that buffer.
    //
    // Checked here and not only at the read, so an impossible pair is refused
    // rather than printed. Echoing a value is what makes a wrong one visible --
    // but echoing one without checking it just states the absurdity and arms
    // anyway: "looking for the camera at float 4294967295 of a 256-byte buffer".
    if (static_cast<uint64_t>(s.posOffset) * 4u + 12u > s.bufferBytes) {
        Log::get().note(
            "transition flash: camera-buffer tap off: camera_buffer_offset %u needs three floats "
            "inside a %u-byte buffer, which does not fit. Check [advanced] in %s; the defaults are "
            "5376 and 1100.",
            s.posOffset, s.bufferBytes, Config::get().iniName());
        return;
    }
    s.observing = true;
    // Said at install, so a log from a session that never reached a rendered
    // scene still shows whether the tap was set up at all.
    Log::get().note(
        "transition flash: camera-buffer tap ready: reading float %u of the %u-byte constant "
        "buffer while the engine fix has an event open.",
        s.posOffset, s.bufferBytes);
}

bool glitchFrameWantsHooks() {
    State* s = g_state;
    return s && s->observing;
}

bool glitchFrameWantsBuffer(uint32_t bytes) {
    State* s = g_state;
    // One size compare per Map. Nominated whether or not the engine fix is
    // armed: vScreen's Unmap tee hands the same block to the sun-glare fix and
    // the temporal pass, which depend on this nomination. The tap itself stays
    // idle until an event window opens (glitchFrameObserve).
    return s && s->observing && bytes == s->bufferBytes;
}

void glitchFrameObserve(const void* data, uint32_t bytes, const void* /*resource*/) {
    State* s = g_state;
    if (!s || !s->observing) return;
    // Idle: three relaxed flag tests, nothing recorded.
    if (!g_engineWindowOpen.load(std::memory_order_relaxed)) return;
    if (bytes != s->bufferBytes) return;
    // Widened deliberately. posOffset comes from the ini through getInt and is
    // stored unsigned, so a negative or very large value becomes something near
    // 0xFFFFFFFF; (posOffset + 3) * 4 then wraps to a tiny number, this check
    // passes, and the read below lands gigabytes past the buffer. The identical
    // wrap in vscreen.cpp was a hard crash on a documented, hand-edited setting.
    if (static_cast<uint64_t>(s->posOffset) * 4u + 12u > bytes) return;

    const float* pos = &static_cast<const float*>(data)[s->posOffset];
    // Non-finite values compare false in both directions, so a NaN here would
    // pass every threshold test silently rather than failing one.
    if (!finite3(pos)) return;

    // The engine fix's render tap (transition_flash_eye_base.cpp): it sees this
    // pre-Unmap fill while an event's window is open and holds the frame at the
    // first head-only one. The frame is s->frameNo -- the PRE-advance counter.
    transitionFlashEyeBaseNoteSceneCB(s->frameNo, data, bytes);
}

void glitchFrameBoundary() {
    State* s = g_state;
    if (!s || !s->observing) return;
    ++s->frameNo;
}

// NOT REACHED ON A NORMAL GAME EXIT, and nothing that matters may live here.
//
// shutdown() calls this, and DllMain calls shutdown() only for a FreeLibrary
// unload -- which does not happen to a game's d3d11.dll. Process exit takes the
// other branch on purpose: every other thread is already dead and may have been
// holding the log's spinlock, so that path must not take one. This runs for the
// test harness and for a hot unload, and that is all.
void shutdownGlitchFrameFix() {
    g_state = nullptr;
}

}  // namespace edvr
