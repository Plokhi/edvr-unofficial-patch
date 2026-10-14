# EDVR encoder reduction — 2026-10-13

## Status

- Superseded by the frame-cost isolation mission: the user reports both optimization rounds failed to recover FPS or reduce encoder counts. Existing optimizations retained; no further speculative optimization.
- Implemented: foreground constant batching, grouped sibling transfers, and direct FP16 input sampling for MFX3/4.
- Previous native tests and focused MinGW syntax checks passed. Latest supplied MFX3 measurement: ~32 FPS, ~31 ms interval, ~17 ms GPU, 215 render / 45 compute / 29 blit; OFF remains ~65 FPS, 15.28 ms interval, 11.86 ms GPU, 85 / 18 / 9.
- Ruled out as effective FPS remedies in the reported scene: CPU metadata/resource lookup caching, constant batching, grouped copies and FP16 input-copy removal. The source call reductions did not yield the claimed runtime opportunity; encoder counts alone do not explain latency.
- Ruled out as a regression: the ARM Clang shadow-oracle mismatch with floating-point contraction enabled also occurs in the unchanged baseline; the suite passes with `-ffp-contract=off`.
- Next: Bunny compiles the diagnostic DLL and focused rigs; run the live matrix in [flat-isolation-2026-10-13.md](flat-isolation-2026-10-13.md) during one session. The old optimization A/B below is retained as historical context.
- No full build, commits, pushes, deployment or game launches performed. EDHDR, older EDVR worktrees and DXMT were not accessed or changed during this task.

## Supplied measurement

Same station concourse, MFX input **1752×1132**, output **2336×1510**:

| MetalHUD | EDVR OFF | MFX3 before | MFX3 optimized |
|---|---:|---:|---:|
| FPS | 65.45 | 33.88 | pending |
| Frame interval, ms | 15.28 | 29.51 | pending |
| GPU time, ms | 11.86 | 16.12 | pending |
| Command buffers | 7 | 13 | pending |
| Render encoders | 85 | 209 | pending |
| Compute encoders | 18 | 46 | pending |
| Blit encoders | 9 | 29 | pending |
| Command-buffer CPU, ms | 5.08 | 7.69 | pending |

Warnings: excessive interleaving of blit and other encoders; too many render passes with similar attachments. These are pre-change measurements. No measured gain is claimed.

## Implemented reductions, in priority order

### 1. Upload foreground constants before the compute/render runs

Before, `FlatForegroundMotion::prepareH` updated one DEFAULT constant buffer before every replay draw into the same motion-map target; `runSibling` updated the same buffer before every donor dispatch. For **N replay draws and D donor dispatches**, that is **N+D interleaved UpdateSubresource calls**.

After, up to two WRITE_DISCARD maps populate one retained 32KiB dynamic constant buffer before the respective runs. Each draw/dispatch binds its own 256-byte window through Context1. No DEFAULT settings updates occur inside either loop when batching succeeds. Receiver-only frames perform no donor upload. Shader draw/dispatch counts are unchanged.

Removing the intervening uploads should let DXMT keep the same render attachments and compute encoder active longer; range binding changes data selection without requesting a transfer. This is an inference to test with MetalHUD, not a measured encoder reduction. Some drivers implement DEFAULT updates without a distinct blit, so source call reduction alone cannot quantify savings.

Equivalence: the same `settingsFor` populates all 160 payload bytes after rescue cancellation. Donor and replay sibling flags, draw index, vertex count, identity, provenance, prior phases and canonical near remain identical. Slots have zero padding and cannot overlap. DISCARD allows the driver to retain donor data while later replay data replaces the CPU-visible allocation. CS b0 now saves/restores its first/count window; existing VS/PS ranged restoration remains intact. Reset releases the batch resource and capability state.

Only immediate contexts with Context1 and successful `ConstantBufferOffsetting` support use batching. Unsupported contexts, allocation failures and map failures use the original DEFAULT upload path. Capability/allocation decisions are retained rather than repeated every draw. The [Microsoft window-binding requirements and deferred-context caveat](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-cssetconstantbuffers1) explain the alignment and immediate-context restriction.

Dependencies preventing further consolidation: per-draw geometry, ownership, phases and priors differ; merging draws would change rasterization/provenance. The donor/fit UAV dependency and the fit/replay dependency remain. The motion-map clear still marks uncovered pixels as invalid.

### 2. Group sibling receiver copies and upload fit constants before compute

All existing 16-byte receiver identity copies and the one FitSettings upload now complete before clearing/binding donor work. Previously the copies alternated with donor dispatches, and the fit upload separated donors from fit. The **copy count and fit upload count are unchanged**; their placement removes those transfer interruptions from the compute run, including on the legacy settings fallback.

Equivalence: copies retain their source, destination slot, size and relative order. Donors read captured geometry and identities, write only Donors, and do not read Receivers or FitSettings. Fit alone consumes those resources, after all donor dispatches. The clear, dispatch order, UAV unbinding, diagnostics/readback and sibling decisions remain unchanged.

Dependencies: receiver identities live in separate captured buffers, so these small copies cannot be collapsed into a single ordinary CopyResource. Fit must wait for donors, and replay must wait for fit. No synchronization or readback was removed speculatively.

### 3. Remove the full input-color copy for FP16 MFX3/4

The non-HDR, non-overlay MFX3/4 path with a `R16G16B16A16_FLOAT` input view now samples the original in both prep and finish. It eliminates **one CopyResource per resolve**, moving **15,866,112 bytes** at the supplied render size (plus its corresponding texture read/write traffic).

This removes a full-image transfer before motion/depth preparation and may avoid a blit transition and resource dependency. Prep still produces private `mfxColor`; DXMT/MetalFX receives that private converted image. Finish writes private `output[1]`, so neither pass writes or aliases the source. The FP16 view has the same sampling interpretation as the former private FP16 view; RGB values, jitter, rejection, depth/motion and temporal history rules remain identical. Returned output and swapchain handling are unchanged.

Dependencies: retain copies for FSR3/other SDK inputs, SDR/SRGB interpretation, and HDR/UI composition that writes back into the source target. Keep the separate MetalFX expansion image because the backend requires its existing RGBA/alpha/finite-value contract. The allocation of the ordinary private input remains available for fallback and other modes.

## Remaining operations reviewed

| Area | Finding / reason to retain |
|---|---|
| Domain-marker draws | Begin/End changes MRT/shader/blend state around original draws. Potentially large fragmentation, but the runtime's classifiers and probes require canonical game state between draws. Deferring restoration requires a broader hook/lifetime proof; no safe consolidation was established. |
| Foreground replay | One shared motion-map attachment/clear; constants were the avoidable transfer between its draws. Keep per-draw raster/scissor/geometry and ownership rules. |
| Motion/depth prep | One prep dispatch already emits canonical depth, motion, rejection, expected depth and MetalFX color. Keep alternating history-depth resources and required UAV-to-SRV dependencies. |
| UI/weapon composition | Overlay clean-color capture is once per coverage frame. Replay-depth copies preserve depth at each original draw. Target changes and coverage draws remain required by the current composition contract. |
| Context capture/restoration | Get calls read CPU-side binding state; ClearState and restoration can terminate/restart passes. Arbitrary SDK/backend changes and RTV/SRV alias hazards require full isolation. |
| FSR3 reconstruction | EDVR calls the FSR3 SDK dispatcher. Its internal dependency graph is retained; no SDK/DXMT modification or unavailable-backend optimization. |
| Scene/pool uploads | Existing once-per-frame/first-eye gates already prevent repeated source publication. Captured identity/provenance and history copies cannot be dropped based on encoder counts alone. |

EDHDR comparison uses only the existing local [transfer review](edhdr-performance-transfer-2026-10-13.md): its nine unused output-state calls were a narrower output-pass saving, while its lookup/generation optimizations did not eliminate GPU calls. EDVR's backend isolation and sampler use prevent copying that state block verbatim. The changes above remove/reorder production transfers instead of revisiting those CPU optimizations.

## Validation and Windows handoff

- Native `edvr_perf_test --self-test`: **569 targeted checks plus the existing flat CPU rig pass**. New tests cover all 128 windows, exact payload bytes, padding, bounds, idle/reuse and refused packing.
- Native harness compiling the actual extracted `uploadSettings` and `settingsFor`: **2,249 checks pass**. Legacy/batched payload equality, donor/replay flags, capability-query retention, immediate/deferred contexts, allocation failure, map failure and subsequent recovery.
- Existing sibling policy and shader/runtime mutation controls pass. Sibling source pins now require the hoisted receiver/fit transfers and CS window restoration. Shadow call-order pin follows the updated sibling signature; all original mutation controls remain.
- Existing shadow model/accumulation/wiring tests pass with `-ffp-contract=off`. Default ARM Clang contraction reproduces the same randomized oracle mismatch before and after the change; production shadow math is untouched.
- Existing HDR isolation, pass-order, budget and breadcrumb tests pass using the native Windows/CRT test adapters.
- MinGW syntax-only checks pass for `flat_runtime.cpp`, `flat_mono_resolve.cpp`, and the temporal, resolver and weapon-motion test sources. Test-only compatibility adapters handle MSVC SEH, the Windows `near` macro and the missing DirectXPackedVector declaration; these checks are not MSVC compilation or GPU execution.
- Task-only diff reviewed; hash comparison verifies that all pre-existing edits outside the changed files remain identical. `git diff --check` passes.

Muse must rebuild and run `edvr_perf_test`, `flat_temporal_test`, `weapon_motion_test`, and `flat_mono_resolve_test`. The new Windows checks verify nonzero CS/VS/PS b0 windows across actual donor/replay shaders, and both MetalFX generations with FP16 input, values 2.5/3.5, reset/continuation, jitter, mixed rejection, full converted-input readback, private output and unchanged source. Existing FSR3, HDR/overlay, depth/motion and UI regressions remain in those rigs. The resolver rig stubs MetalFX at the backend seam; it cannot validate the actual native scaler.

## MetalHUD A/B after Windows validation

Use matching baseline and optimized binaries with the same existing CPU/performance port, settings, DXMT/CrossOver version, output/render sizes, fixed concourse view, and warmed temporal history. Record the build identities and whether the source route is FP16. Keep CPU profiling off. Compare stable, equal-duration intervals and repeat baseline/optimized ordering to expose scene/load variance.

Record every metric in the table plus both warnings and visible correctness (HDR highlights, UI/weapon edges, motion during camera turns, history/jitter). Repeat working FSR3 and optional MFX4 for correctness when available. Constant batching is capability-gated: if the driver lacks offsetting, its legacy path remains and only grouped transfers/direct FP16 input can contribute. If the actual MFX3 source view is SDR, the direct-input saving is deliberately inactive.

Do not infer success from a source-call count or mix the optimized run with the supplied OFF/ON measurements from a different build. Fill the optimized column only after the new capture. No A/B, installation or game launch was performed by this task.
