#pragma once
// The pad-state probe (TEMPORARY, 2026-10-13): four detours on Elite's docking-state readers, build 332841. LOG ONLY: it changes no behaviour
// and the seal logic does not read it. Installed when fix.optimized_station is on (there is no key and no hotkey).
#include <cstdint>

namespace edvr {

// The setting, as applied (applySetting): on installs the readers once (identity first, then each prologue) and opens the observers; off closes them.
void padStateSetOn(bool on) noexcept;
// The station's frame tick (setting on): prints the change lines, the first-seen lines, the seal and release lines, and the 30 s summary.
void padStateTick(uint64_t nowMs) noexcept;
// A seal or release happened ("seal" or "release"); the next tick writes the line with the occupied objects' tuples.
void padStateEvent(const char* what) noexcept;

}  // namespace edvr
