# Flat frame-cost isolation — 2026-10-13

## Status

- Diagnostic source ready for Bunny's focused MSVC compilation; no Windows build, deployment, game launch, commit or push performed here.
- Actual culprit remains unknown. User's two optimization rounds did not recover FPS or reduce encoders; stop speculative performance changes.
- Latest supplied OFF: ~65 FPS, 15.28 ms interval, 11.86 ms GPU, 85 render / 18 compute / 9 blit. MFX3: ~32 FPS, ~31 ms interval, ~17 ms GPU, 215 / 45 / 29. VSync made no difference.
- Temporary keys: `diagnostic.flat_session`, `diagnostic.flat_motion`, `diagnostic.flat_preparation`, `diagnostic.flat_composition`, `diagnostic.flat_backend`. Keep until the culprit is identified; propose their removal explicitly when this arc closes.
- Native tests cover actual policy/profile access, all 16 combinations, source wiring and mutation controls. Extended WARP cases cover real resolver shaders, SDK seams, FP16/HDR outputs, cold/frozen inputs and return to normal history; GPU execution awaits Windows.
- Next experiment: one compiled DLL, fixed station viewpoint and resolution, normal baseline then the controlled live matrix below. No claimed FPS/encoder improvement without those measurements.

## Live controls

Add this section to the INI already read by the flat installation. The `flat isolation:` activation line names that file (`edvr-flat.ini`, or `edvr.ini` for a fallback installation). Merge into an existing `[diagnostic]` section rather than creating duplicate sections. The shipped INI template contains commented defaults.

```ini
[diagnostic]
flat_session = on
flat_motion = on
flat_preparation = on
flat_composition = on
flat_backend = on
```

Save the INI to change experiments. Existing config polling reloads about once per second; choices latch together at the following Present and log an epoch, mask and exact stage states. No new hotkeys, UI rows, shader compilation or rebuild between experiments. The session default is **off**; stage defaults are **on**. With the session off, stage choices are inert and the normal rendering path runs.

**Every session-on combination, including all-on, intentionally has zero jitter and a reset on every resolve. Temporal history and upscaling quality are invalid.** The all-on combination is the comparison control: compare bypasses against it, and separately compare it against normal rendering. If that control alone changes timing substantially, history/jitter-dependent backend work is already a discriminating observation. SDK output and history changes can affect its workload; a bypass delta is not an exact additive subsystem cost.

## What each bypass removes

| Stage set off | Actual D3D11/engine work omitted | Work retained / dependencies |
|---|---|---|
| `flat_motion` | Engine producer/emit observers; lazy producer substitution/MRT6 bindings; foreign ownership markers; MRT7 alternate-camera coverage brackets; source-free producer preparation and source-view acquisition. | CPU source/provenance selection and original game draws remain. The pixel coverage requirement is deliberately bypassed at both qualification and accounting, so a missing mask cannot silently turn the experiment into a refusal. Foreground capture/resolve also stops: it requires producer ownership. |
| `flat_preparation` | Resolver prep binding/dispatch, motion/depth conversion, MetalFX RGBA expansion, steady-depth preparation and refusal-class sampling. | Backend stays selected and is called. Prefer warming with prep on first: backend reads frozen, previously initialized motion/depth and (MetalFX only) colour, with the last depth actually written. Cold/resize/mode change initializes finite neutral inputs once. Rejection is forced to all pixels; finish displays the current game image. Source-colour copies needed for HDR read/write separation, SDR interpretation and SDK inputs remain; so do overlay colour copies when composition stays on. |
| `flat_composition` | Flat crisp UI layer redirection, tonemap/write-back reissues and composites; private late-overlay MRT/capture/replay; foreground geometry capture/replay and `prepareH` motion-map passes/copies. | Producer/coverage/domain-marker work remains when motion is on, giving a comparison of owner markers against extra geometry. Protected-overlay metadata still classifies and seals at the consumer; private overlay availability is intentionally bypassed. UI/weapon pixels pass through the scene path instead of their usual quality treatment. |
| `flat_backend` | `mfxEvaluate` (MFX3 or optional MFX4), `fsr3Evaluate`, NGX evaluation, or the EDVR TAA dispatch. SDK temporal dispatches disappear. | One existing EDVR bilinear spatial dispatch writes initialized current-frame output; HDR still performs its necessary in-place finish draw. Copy-route SDK finish is omitted because spatial output is already final. SDR uses its SRGB-capable second output; FP16 output retains values above one. Prep, producer and composition choices remain independent. |

All-off remains an EDVR diagnostic rendering path with selection, state capture/restore, required colour isolation, a spatial dispatch and HDR finish. It is **not** an EDVR-OFF measurement. Motion-off necessarily disables foreground work even if composition is on; use motion-on/composition-off to isolate the latter. Prep-off changes backend inputs; compare prep-on/off with backend-off as a cross-check that excludes SDK input sensitivity. Backend-off measures removal relative to spatial replacement, not an empty presentation path.

At selection changes, producer state is flushed/restored, retained private UI/coverage/foreground state is released, histories invalidated and timing-query windows discarded. Automatic stand-down is suppressed only during the diagnostic session, so intentional missing image contracts do not make later stages disappear. Other source/format/extent/ownership refusals remain. Original D3D11 draw forwarding, proxy chaining, swapchain formats, MFX generation selection and shader bytecodes are unchanged. Finish uses current colour, so freezing backend inputs does not freeze game presentation.

## One-session matrix

Hold viewpoint, resolution, HDR configuration and scene conditions fixed. Start with the user's working MFX3 mode. Record the existing normal baseline, then these steps; return to the all-on control between individual cuts. All four stage keys accept independent combinations, not just this order.

| Experiment | Session | Motion / Prep / Composition / Backend | Mask |
|---|---|---|---:|
| Normal MFX3 baseline | off | on / on / on / on | 0 |
| Diagnostic all-on control | on | on / on / on / on | 16 |
| Producer/coverage cut | on | off / on / on / on | 17 |
| Preparation cut, after warm-up | on | on / off / on / on | 18 |
| UI/weapon cut | on | on / on / off / on | 20 |
| Backend cut | on | on / on / on / off | 24 |
| Backend plus prep cut | on | on / off / on / off | 26 |
| All cuts | on | off / off / off / off | 31 |
| Restored normal MFX3 | off | on / on / on / on | 0 |

Wait for the new epoch line, discard transition/warm-up frames, then collect at least two full five-second windows per experiment. Return to the control to check scene drift. Fill in FPS, frame interval, MetalHUD GPU time, command-buffer count and CPU time, render/compute/blit counts, and the timing/call data below. No measurements from this mission exist yet. Use the same matrix with the existing FSR3 mode if it works on the machine; no unavailable backend investigation is needed.

## Timing evidence and validity checks

Existing CPU/GPU profiling stays opt-in. For timing, start the single session with `EDVR_CPU_PROFILE=1` in its process environment (the existing environment gate is read once); leave the existing sparse sample period unchanged. Use equal instrumentation settings throughout. No new GPU queries or synchronization waits were introduced by these controls. Avoid F10 captures and other optional probes while measuring: their readbacks/copies/replays are separate diagnostic work and are not eliminated by the four production cuts.

The log emits `flat isolation timing: epoch=... mask=...` immediately before the existing five-second census, and `flat isolation work 5s:` for actually executed work. Require nonzero `resolve` and `finished`; a missing resolver is a refusal, not an isolated speedup. Confirm `prepared=0` for prep-off, `evaluated=0`/`spatial>0` for backend-off, and producer/coverage/domain/foreground/replay counts against the relevant cuts. The seed line distinguishes `frozen-last-prepared` from `neutral-cold-start`; `seed-init` should stop increasing after transitions. Work and timing windows have independent five-second cadences; match their epoch/mask and collect complete stable windows.

- EDVR CPU family totals and preparation/backend/foreground/UI breakdowns measure host-side work, including EDVR's own D3D calls. They are not GPU execution durations.
- Downstream original-draw/resource submission and real Present CPU spans are excluded from EDVR CPU totals. They include time spent inside DXMT/driver calls and may include waits; this is the available evidence for submission pressure, not a pure DXMT encoder-construction timer.
- Existing GPU frame span runs from first game draw to before Present, and GPU resolve encloses the whole resolver. Neither provides individual prep/backend GPU times; the paired cuts isolate their effect without adding queries. Missing, skipped or invalid query samples remain explicitly reported.
- Present interval/MetalHUD GPU time/CPU spans overlap and must not be added. The ~31 ms frame interval versus ~17 ms GPU time leaves CPU/queue/presentation possibilities open; subtraction does not locate the stall.
- Fewer encoders with unchanged interval/GPU time argues against encoder count being the limiting factor. A large interval reduction with little GPU-time change points toward host/submission/queue effects; a matching GPU-span reduction implicates GPU work. Treat interactions and backend input/history changes as confounders, and use the repeated control and combined cuts to check them.

## Focused validation handoff

On macOS: native `edvr_perf_test --self-test` (716 targeted checks plus the existing CPU rig); existing sibling/shadow policy, source-wiring and mutation tests with `-ffp-contract=off`; HDR breadcrumb/isolation/pass-order regressions; config contract dry-run; focused MinGW syntax checks of changed production modules and test translation units; final diff/whitespace and task-start file-hash scope review. MinGW uses the existing test-only MSVC SEH compatibility adapter for the shared temporal module/Windows fixture; it does not validate MSVC SEH semantics.

Bunny should rebuild and run the affected `edvr_perf_test`, `flat_temporal_test` and `flat_mono_resolve_test` rigs, then compile the diagnostic DLL with the normal generated config contract. Extended WARP cases execute all 16 selector combinations for TAA/FSR/MFX3/MFX4 on FP16 copy and HDR routes, prove real SDK call suppression, validate cold/frozen input handling, preserve current colour above one and original state, and prove normal history resumes after leaving diagnostics. Their GPU execution is pending; native tests/syntax checks are not substitutes. No new shader source or shader bytecode was changed.
