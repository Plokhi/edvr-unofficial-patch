#pragma once
// The optimized station (fix.optimized_station = on | off). See optimized_station.cpp for the rules and docs\optimized-station-2026-10-12.md.
#include <cstdint>

namespace edvr {

// Once, at install: reads fix.optimized_station (on in VR; off in the flat profile unless set) and attaches the hooks when it is on.
void optimizedStationInstall(bool flatProfile);
// The setting, as last read (one atomic load).
bool optimizedStationOn() noexcept;
// Built v14.20 (review 7b): the setting is on and the commander is docked: the only time the store observer gathers (the store gate follows it).
bool optimizedStationGathers() noexcept;
// Whether the station is sealed in a hangar this frame (one atomic load). The draw path and the hooks read it.
bool optimizedStationSealed() noexcept;
// Whether the station's sun skip is on now (one atomic load; built v14.30: the sun-shadow probe and the dispatch hook read it).
bool optimizedStationSunOff() noexcept;
// The draw path asks: true skips the draw (the sun pair while the sun skip is on, and, once a scan has cleared it, the draws into the sun's shadow cascade
// map). ctx is the immediate context (the shadow skip's scan enumerates its shader-resource bindings).
bool optimizedStationSkipsDraw(uint64_t vs, uint64_t ps, void* ctx) noexcept;
// The admission store calls this after it has written a record (bucket = the collection, record = its record).
void optimizedStationAfterStore(uintptr_t bucket, uintptr_t record) noexcept;
// The render thread's frame tick: the setting, the seal and release rules, the 30 s summary.
void optimizedStationFrame(uint32_t frame);

}  // namespace edvr
