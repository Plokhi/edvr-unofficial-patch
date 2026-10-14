# Supercruise sky flicker, 2026-10-13: the diagnostic build

## Status

- State: instrumented build on branch `claude/sky-flicker-diag`, cut from origin/main 7e44b86c. BUILT, NOT INSTALLED, NOT FLOWN. No fix ships from it. Source of the findings: `reviews/skybox-flicker-2026-10-13.md` (gitignored; the numbers below are copied from it) and `reviews/vr-view-caches-2026-10-13.md`.
- Environment the findings hold for: Elite build 332841, SteamVR/OpenXR and Pimax OpenXR (both show it), DLSS preset K at 2x (2016x2016 to 4032x4032), 89.9998 Hz. Flat (Epic) shows the F8 render-size flip, which is INFERRED to be the same cause.
- Finding 1 (log, proven): once every 1.006 s, and again 11 frames later, the camera rows the temporal pass selects jump for ONE frame (0.118 m, 1.35 to 3.04 deg against 0.05 deg a frame) and return the next. Both eyes together. Under SteamVR and Pimax alike, so the game's camera block and EDVR's chooser, not the runtime.
- Finding 2 (log, proven for VR): the excursion is the reflection probe's FORWARD cube-face camera. The probes write one face a frame over 6 frames (back, four sides, forward last) into the same scene block as the eyes, at one point 0.112 to 0.122 m from the head; the chooser (`chooseCameraRows`) keeps the latest write within 3.0 deg of the last pick and has no notion of render target, so the forward face is picked whenever it lies within 2.9 deg of the head.
- Finding 3 (inferred): the probe's faces are the 256x256 deferred "mini-render" (about 400 draws) seen in 5 of 24 captured frames; on flat the same render outdraws the scene and flips `scene=` between 1440x810 and 256x256.
- Unproven: that the sky VISIBLY glitches on those frames. The sky ring answers it.
- Ruled out: the runtime as the cause, because SteamVR and Pimax logs show the same 1.006 s cycle and 11-frame partner.
- Ruled out: DLSS history resets, because `events=8` only and the "6 started afresh" count is constant across the window.
- Ruled out: celestial motion, because the planet left the view at 19:29:44 (draws 0, all fallback counters 0) and events continue.
- Ruled out: the pose cache, because it is not on main (`git grep pose_cache` finds one comment) and the events occur with it on and off.
- Ruled out: transition-flash holds, app-side frame pacing, the world route and vScreen clears, long frames, in the 19:29:30-19:31:00 window (review, "Refuted for the window").
- Ruled out: "the 256x256 render is drawn every frame", because the bursts are 12 of 91 frames (13%).
- Not ruled out, not observed: exposure, Elite's own sky, 8-phase jitter at 2x, SteamVR compositor reprojection.
- TEMPORARY KEYS (all default off, all live, listed here per AGENTS.md; each is `ui: hidden`):
  - `diag.sky_ring = off|on` (new `[diag]` section). I-2.
  - `fix.camera_eye_target = off|on`. The root-cause arm. VR: a camera write whose first following draw goes into a render target that is not eye-sized (each axis within a quarter of the eye's render size) is not a candidate for the pick. Flat: a square scene candidate under a sixteenth of the output's area is not a candidate. Also in the flat allow-list.
  - `fix.camera_nearest = off|on`. Comparison arm, ported from branch `claude/hangar-test-4` / `claude/camera-nearest` (d3ed2787, e40d6674), the veto form.
  - `fix.camera_jump_guard = off|on`. Comparison arm, ported from 998921da (present in builds -53 and -58, where no events appeared). If both pick rules are on, the nearest-write rule WINS and the guard only counts; the log says so ("superseded").
  - Not keys but TEMPORARY instruments: the `rt=`/`drt=`/`burst=`/`rej=` tag on every TCAM line, the P3 mini-render record, the flat candidate log, the "sky ring" item in the census line. They run with the keys off, cost nothing measurable, and go when the arc closes.
- When the arc closes: ship the winning behaviour with no key, remove the other keys (quoting each, per Scope control), delete the `[diag]` section if empty.
- The chooser CANNOT know a candidate's render target when it is made (the write precedes its draw). It learns it from the first draw after the write and judges at the first treat, when the frame's draws are done. A write no draw followed is unknown and is never rejected. If nothing at all would be eligible the filter stands down for the frame (`fail_open`).
- Exposure is NOT in the sky ring: Elite's exposure state is a GPU buffer that only exposure_fix.cpp's dispatch hook sees and nothing exports; the column prints `exp=n/a`. The out/raw luma ratio on the SKY lines is the proxy.
- The sky ring samples a sparse 128x128 grid: it sees the sky's brightness, motion vectors and a few bright points, not a single star. A glitch of the whole sky field shows; a single-star shimmer may not.
- NEXT FLIGHT (supercruise near a station, looking FORWARD with the cockpit LEVEL, which puts the forward face inside the 3 degree gate most often). Two minutes a leg, press PAUSE (the history key) when flicker is seen and once in a clean stretch; the +2 s dump is the one to read:
  1. `diag.sky_ring = on`, the three fix keys off. Read: events per dump, `SKY` lines around them (does `mvmax` or out/raw jump on the event frames and nowhere else?), the TCAM `rt=` of the selected write on event frames (expected `256x256`), `P3 join` (expected: an event frame has a mini-render the same frame or the one before), the 30 s `eye target` line's `rejected_sizes`.
  2. Add `fix.camera_eye_target = on`. Expected: `events 0`, `rejected` unchanged in kind, the flicker gone. If `fail_open` climbs, the eye size judged against is wrong: read `eye=` on the line and the `rt=` sizes.
  3. `fix.camera_nearest = on` (eye_target off). Expected events 0 by the symptom.
  4. `fix.camera_jump_guard = on` (nearest off). Expected events 0 or few.
  5. One flat check if convenient (Epic, flat profile, a settlement with the F8 panel): watch `flat scene candidates` lines for the 256x256 and the `scene=` flips with the key off, then `fix.camera_eye_target = on` in edvr-flat.ini and watch the flip stop.
  6. Optionally leg 1 again on the Pimax runtime.
- Decision rules: H1 (the face is picked and DLSS is handed a wrong world delta) is confirmed when SKY `mvmax` or out/raw glitch ONLY on event frames; it is refuted for the visible flicker if the sky stays flat on event frames while the flicker is reported elsewhere. Do not ship any arm before step 1 shows the sky reacting.

## Log lines (formats)

- `sky ring: no samples (off or never ran) key=off dispatches=0` when the ring never ran. Otherwise `sky ring: N eye-frames kept, frames A..B, M with sky samples; E camera-row events, L SKY lines` then `SKYSEC f<a>-<b> eyeframes= sky= raw= out= (min..max) bright= mv= mvmax=@f<frame>` a bucket per 90 frames, then `SKY f<frame> eye=<e> n= raw= rawmax= out= outmax= bright= mv= mvmax= exp=n/a d=<frames from event> rot= mv= EVENT` for the frames within 45 of each event.
- `camera-row events: N of M frames (...; run1= run2= run3=; both eyes= left only= right only=); peak rot= mv=; frames: ...; gaps: ...; leaders= partners= partner_gap= period=` and `camera-row events: 0 of M frames`. Each TEMP line ends `rot=<deg> mv=<m>` (`gap=N` when the previous row is not the previous frame, `rot=- mv=-` with none) and ` ev=N` inside an event.
- TCAM lines end `rt=<W>x<H>:<fmt>/<rtv|dsv>/c<n> drt=<same> burst=<k|-> wr=<writes>/<baseline> rej=<n>`.
- `P3 mini-render frames: N in the kept M frames (x%), first size WxH, cadence median G frames; ...`, `P3 f<frame> mini=256x256 colours=4 passes= draws= rva=0x.. writes= first_write_t=(..)`, `P3 join: events= mini_same_frame= mini_previous_frame= either= neither= no_record=`, `P3 event f<frame> run=: mini-render same frame=yes|no, frame before=yes|no`; every 30 s `temporal P3 mini-render: frames= with= (x%) pass_binds= draws= first_caller_rva=0x..`.
- Every 30 s: `temporal camera eye target: frames= writes= tagged= unknown= rejected= reject_frames= fail_open= eye=WxH key= rejected_sizes=`, `temporal camera nearest: frames= differs_from_legacy= reanchors= max_dist_m= key=`, `temporal camera jump guard: frames= rejected= replaced= passed_through= key=on|off|superseded max_jump_m=`. A window with no line means the chooser never ran in it.
- Flat, once per 5 s and on a change of winner: `flat scene candidates (frame N, output WxH, fix.camera_eye_target=, why): winner ...; candidates: <size fmt= draws= on-shape|off-shape [tiny-square]>, ...; frames won in this window (n frames): WxH:count ... none:count`.
- Census line: `sky ring <ms> (<n>/frame)` among the door items, `sky ring -` when it never ran.

## Journal

### 2026-10-13: the build

Ported, not merged: `camera_nearest.h` (e40d6674, the veto) and `camera_jump_guard.h` (998921da) with their pose helpers moved to `camera_pose_math.h`, because main has none of `camera_ipd_pair.h`, `camera_writes.h` or the headset tag they sat on. Chooser changes are only the filter before the candidate loop and the two rules after the pick. They judge the continuous frame's legacy pick, not a refollow or a resync (whose pick is meant to leave the chain). Both rules run and count with their keys off, so leg 1's 30 s lines already say what each arm would have done. Conflict: the two rules cannot both apply; nearest wins and the guard reports `key=superseded`.

I-1 reads the existing history (no new storage): the step between consecutive same-eye selected rows, and events judged against the line from the frame before an excursion to the frame after it, then required to return to the track the earlier frames set (constant-velocity), so a steady turn, a flight at 270 m/s and a persistent cut are not events. Thresholds 0.5 deg or 0.05 m, runs of 1 to 3 frames.

I-2: one 1024-thread group per eye-frame after the upscaler, 16384 samples, a 32-byte entry in a 2048-entry raw buffer; no readback until a press; the copy is made on the render thread's next eye-frame and polled without waiting. Estimated cost under 20 us a eye-frame (one group on a 5090's one SM); measured by the census section `DoorSkyRing`, which the 30 s line prints as `sky ring`. The rig runs the shipped HLSL on WARP against the CPU reference.

Tests: `tools/sky_flicker_test` (in the build gate as `:rig_sky_flicker_test`), `tools/gpu_census_test` (the new door section), the shader builder's self-test (the sky ring's contract: symbol, 1024-thread group, bindings, the 48-byte block).
