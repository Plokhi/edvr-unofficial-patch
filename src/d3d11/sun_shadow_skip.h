#pragma once
// The sun-shadow skip (sun_shadow_skip_core.h). While the station's sun skip is on, the draws into the sun's 2048x1024 D16 shadow cascade map (a depth
// target with no colour target) are skipped, once a scan of the first frames of the window has found no shader of any stage that reads the map; a
// consumer found at any time stands the skip down for the rest of the window, with a log line that says why. Nothing is written to the game.
#include <cstdint>

namespace edvr {

// Owner-context draw about to be issued, called by the draw verdict only while the sun skip is on: ctx the immediate context, vs and ps the bound shader
// hashes. True skips the draw.
bool sunShadowSkipDraw(void* ctx, uint64_t vs, uint64_t ps) noexcept;
// Owner-context dispatch, called only while the sun skip is on: the scan enumerates its shader-resource slots and UAVs.
void sunShadowSkipDispatch(void* ctx, uint64_t cs) noexcept;
// Once per frame, on the station's tick: the sun skip's state. Moves the window (idle, scanning, skipping, stood down).
void sunShadowSkipFrame(bool sunSkipped) noexcept;
// The 30 s line (the station's summary calls it): written only when the sun skip was on in the window.
void sunShadowSkipReport(uint64_t seconds) noexcept;

}  // namespace edvr
