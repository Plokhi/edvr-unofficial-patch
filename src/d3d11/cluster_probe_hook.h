#pragma once
// The fragment-culling cluster builder's probe (EliteDangerous64.exe build 332841, RVA 0x58B150; cluster_probe_core.h has the findings). LOG ONLY: the
// detour forwards the call with its six arguments unchanged, then reads the context's key block and tables under __try and counts. Nothing is written
// to the game. It follows fix.optimized_station (no key of its own): installed on the first on, its gate open while the setting is on. A build that is
// not 332841, or a prologue that is not the pinned one, is one log line and nothing patched.
#include <cstdint>

namespace edvr {

// The setting follows the station's: true installs the detour (once) and opens the gate; false closes the gate.
void clusterProbeSetOn(bool on) noexcept;
// Once per frame, on the station's tick: closes the frame's call count.
void clusterProbeFrame() noexcept;
// The 30 s line (the station's summary calls it): writes it to the log and starts a new window.
void clusterProbeReport(uint64_t seconds) noexcept;
// The detour's status ("installed", or why not).
const char* clusterProbeStatus() noexcept;

}  // namespace edvr
