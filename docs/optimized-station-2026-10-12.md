# Optimized station (fix.optimized_station), 2026-10-12

## Status

- **Built (`claude/optimized-station`, commit a1309799, DLL v0.19.1-18-ga1309799; full `build.bat` green with `optimized_station_test` 72 checks, 0 failures; `build.bat --dll-only` green). Pushed. Not installed, not flown.**
- **Flight 07b49558 (`edvr_gfx_20261012_191925.log`): never sealed.** Docked in the hangar with the pad lowered, from 19:19:55 to 19:24:25: every 30 s "sealed for 0 of N frames", no vetoes. Cause: the store observer returned at the top of `optimizedStationAfterStore` while unsealed, so `afterStoreJudge` never ran, and no camera, pad or explicit part was ever gathered. With no pad `pickOk` stayed false and `sealStep` reset its still timer every tick. Fixed in e580730b: `sealed::storeMode` gathers (camera, pads with up axes, explicit parts, the pick) while docked and unsealed; judging and the vetoes stay sealed-only.
- **Waiting line (new):** while docked and unsealed, `optimized station: waiting (docked=yes|no|unknown, pad=NAME|none, at lowest=yes|no, still for X of 6 s, cell=built|failed:REASON)`, once when docked, pad, at lowest or the cell changes, and at most every 10 s while docked. The next flight's log names the unmet condition. The docked input (Status.json Flags bit 0) is still unverified; this line answers it.
- **Flight e580730b (`edvr_gfx_20261012_2013*.log`): docked in a LARGE hangar, waiting every 10 s with pad=none, the summaries all zero.** Cause: `g_frameStamp` was loaded by the observer and never advanced, so `g_cellStamp == stamp` (both 0) and `promoteFrame` never ran. Fixed in a1309799: the tick advances the stamp once per frame; the promote gate is `sealed::framePromoteDue`.
- **Load-in rule (maintainer, a1309799):** a pad first seen at rest with no descent observed seals on the first eligible frame (the hatch is shut in a closed bay). After an observed descent, or a release by motion, the delay applies. The memory is per pad. The status line says "sealed at load-in" or "sealed after the descent settled". Risk: a raised pad first seen at rest also seals at once; the bay floor is not checked for it.
- **Evidence on the waiting line:** records gathered last frame, camera yes/no, the first three pads (name, distance, contains camera), and whether a descent has been seen.
- **Ships:** `fix.optimized_station = on | off`, F8 menu "Optimized station". VR default on. The flat profile is off and not on the flat allow-list (VR first).
- **Seal:** docked (Status.json Flags bit 0), own pad found and lowered to its lowest seen height and still (within 0.2 m) for `kSealDelayMs` (6 s, a guess), and the cell builds.
- **Effect while sealed:** records outside the held hangar box are skipped (producer-B pre-forward veto, rig early veto, post-store clear), and the sun pass (VS 7E38A6AA1269C901 / PS 7CECABDE34FFBE9E) is skipped.
- **Release rules (each on the next frame):** setting off; undocked; Explorer Cam on; camera more than 2 m outside the held box; own pad more than 2 m along the shaft against the reference shell; hangar change (held pad absent for 30 frames). A re-seal needs the full still delay again.
- **Launch release: NOT wired.** The pad state object (+0x3C8, readers r0/r1/r3) is not ported and the launch input is always false. The pad-motion rule is the backstop. The pad moves about 2.8 s after its state flips, so launch pop-in is up to that much late. Not measured.
- **Fix (v13.8 flight, bug 1): own pad sticky.** The medium bay's LandingPadMediumA and B tie within 5 m with antiparallel axes. A tie goes to the pad nearest the camera laterally to its own up axis, then to the name ending in A, then to the lower record address. A chosen pad stays the own pad while its record is seen; a replacement needs 30 unseen frames.
- **Fix (v13.8 flight, bug 2): reference shell chosen once.** At the freeze: HangerSmall/Medium/Large matching the pad's size first, then the other sized hangers, then the rest. Gate, Ramp and Skirt parts are never the shell. If that record goes, the pad-motion check stands down (logged) and no other shell is taken.
- **Seal trigger (v13.8 item 3): unchanged.** Settle plus the 6 s delay. The candidate signal (HangarGate_Ramp1_0/1_1/2_0/2_1 rotating about 90 deg and settling about 70 ms after the pad) is NOT used; it needs one cleaner flight.
- **Measured gains (from the v13.7 trial flight, as the coordinator reports them; the logs are not in this worktree and this port is not yet re-measured):** fpsVR app CPU about 5.4 to 4.0 ms; Elite GPU about 0.9 ms less.
- **Not yet measured:** launch pop-in, the elevator (the medium back section; the 85 m floor missed it in v13.3; floors are now per size, 85/120/145 m), Explorer Cam, the flat profile.
- **Open:** SEAL_DELAY_MS is a guess until the hatch close time is known; docked is Status.json Flags bit 0, not yet checked against a live Status file; the launch reader is not ported; the producer-B veto has no own-sphere keep (as flown in v13.7).
- **Trial record:** `claude/hangar-test-9` (archive), `docs/concourse-culling-2026-10-11.md` (v8 to v13.8). Nothing from the trial's probes is on this branch.

## What ships

Core (pure logic, `src/d3d11/optimized_station_core.h`): the seal state machine (`sealStep`), the release priority (setting off, undocked, Explorer Cam, camera left, pad moved, hangar changed, launch), the own-pad pick with the near-tie rule and the sticky state (`pickOwnPad`, `pickOwnPadSticky`), the shell pick (`pickShell`), the held-cell rules (`keepNamedCell`, `heldCellDrops`, `ownSphereKeeps`, `inBayVolume`, `boxCoordsOf`), and the pad-frame release (`padMovedAway`, `localizeCell`, `worldizeCell`).

Engine (`src/d3d11/optimized_station.cpp`): the store judgement, the rig and producer vetoes, the held-cell refresh each frame, the seal tick, and the two status kinds (one line per state change, a 30 s summary line).

Hooks: `store_hook.cpp` (path A, store observer), `kinematic_eval_hook.cpp` (producer-B veto on the engine-motion bracket of 0x4312E00; rig early veto on `rigEvalObserved` emulating Elite's off-screen branch), `vscreen.cpp` (sun-pass skip while sealed), `journal_watch.cpp` (Flags bit 0 as docked), `view_source.cpp` (explorer branch removed).

## Ported and dropped

Ported: the producer-B veto, rig early veto, post-store clear, world-of-record and builder centre, unreadable rule (unreadable records kept), lock-free name-class cache, own-pad pick, bay box at the freeze, wiki footprint doubled on length with a 10 m margin, per-size floors (85/120/145 m, constants), bay keep, explicit nearest-part keep, own-ship rule, pad-local cell, drop and re-freeze on hangar change, sun-pass skip.

Dropped: sphere, all and count modes, the radius ladder, PAGEUP/PAGEDOWN, kept reports, culled-top lines, drawlist, builder, hatch geometry, look and census probes, all diag and hotkey keys. The launch reader (not ported; see above).

## Config and menu

`edvr.ini`: `# ui: Optimized station | choices on, off | menu` above `fix.optimized_station = on`. The only new key. Flat reads "off" for unlisted keys, so the flat profile stays off.

## Gates

`tools/optimized_station_test` (55 checks, in the build's gate list): seal trigger, release priorities, ship swap, re-seal, pure logic, and the v13.8 fixes (tie, stickiness, shell choice). `build.bat --dll-only` promotes the DLLs after a clean commit with a matching receipt.
