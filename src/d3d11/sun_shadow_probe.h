#pragma once
// The sun-shadow probe (sun_shadow_probe_core.h): LOG ONLY. While the station's sun skip is on, it counts the draws into the 2048x1024 D16 cascade map, the
// draws that sample it through a pixel-shader slot 0 to 3, and the dispatches that bind it as a UAV, from the binding shadow. It changes no draw.
#include <cstdint>

namespace edvr {

// Owner-context draw, called by the draw verdict only while the sun skip is on: the vertex and pixel shader hashes of the draw about to be issued.
void sunShadowProbeDraw(uint64_t vs, uint64_t ps) noexcept;
// Owner-context dispatch, called only while the sun skip is on.
void sunShadowProbeDispatch(uint64_t cs) noexcept;
// Once per frame, on the station's tick: sunSkipped is the sun skip's state; a new frame also drops the probe's view cache.
void sunShadowProbeFrame(bool sunSkipped) noexcept;
// The 30 s line (the station's summary calls it): writes it to the log, only when the sun skip was on in the window, and starts a new window.
void sunShadowProbeReport(uint64_t seconds) noexcept;

}  // namespace edvr
