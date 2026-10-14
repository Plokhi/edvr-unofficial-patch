# Working-upscaler performance transfer — 2026-10-13

## Status

- Source implemented on `mfx/0.19.1-port`, atop `3428a02d` and the existing uncommitted performance port.
- Two independently reversible CPU optimizations: indexed resource lookup (including camera integration), and immutable bound-view metadata reuse.
- Native focused tests and MinGW syntax checks passed; MSVC and D3D11/game execution remain pending.
- No FPS gain measured. No deployment, game launch, commits, pushes or full build performed.
- Next: Muse rebuilds into separate Windows output paths, then an authorized controlled in-game A/B.
- EDHDR remained read-only; the 0.18.1 checkout and validated binaries were not modified.

## EDHDR source examined first

EDHDR was inspected on `feat/d7e646e3-linear-hdr-tone`, HEAD `95d95dd`, including all local branches, recent history, status and the relevant uncommitted production diff. The performance techniques are **dirty source**, not a new committed performance revision. Its `src/edhdr.cpp` SHA-256 at review was `b217b881a6ca6e590771e5cd47a6752a588f99d2b87667616c4b8b778670c782`.

History includes `1295596` (recover views excluded by stale eligibility memos) and `4247198` (forward the game's original array when remapping returns null). The dirty code preserves those contracts while adding:

- Fixed hash buckets over existing view records: `healBucket`/`healFindLocked` at [edhdr.cpp](/Users/ploki/GitHub/edelite-hdr/src/edhdr.cpp:1323). Previously bind-time memo lookup scanned up to 512 records under a critical section.
- Second-chance retention at `healRecord`, line 1363: repeatedly bound views survive one-use creation entries. This is useful evidence but was not copied; EDVR's tables already have their own correctness-sensitive eviction policy.
- Immutable clone-backed identities bypass unrelated clone-generation rescans in `resolveBindAs`, lines 1531–1580. Provisional negative eligibility still revalidates after publication; making every memo permanent would reproduce the bug fixed by `1295596`.
- Output-pass removal of nine unused state calls, while retaining VS SRV handling for RTV alias hazards, around lines 3586–3670. Not transferred: EDVR isolates arbitrary backend state changes and its HDR finishing pass uses a sampler.
- Earlier feature-specific gates and later sampled profiling repair. Existing EDVR early exits and opt-in profiling already address parts of that work; this change adds no diagnostic framework or profiler optimization.

The supplied EDHDR FPS comparisons motivate the audit but do not establish transferable savings. Its October 12 notes also report a large profiler-induced slowdown, so none of those elapsed differences is used as an EDVR estimate.

## 1. Indexed lookup in retained camera and projection tables

EDHDR analogue: fixed pointer hashing and index+1 bucket records above. EDVR adapts that technique within its existing [ResourceSlotCache](/Users/ploki/GitHub/edvr-0.19.1-mfx/src/d3d11/resource_slot_cache.h:15), using two candidate slots per bucket and linear fallback rather than copying EDHDR's mutable clone decisions or eviction machinery.

Before: the existing projection buffer cache had one last-hit hint. Alternating retained buffers missed the hint and searched up to 64 slots on each Map/Unmap, write, preflight or prepare. [FlatCameraTable::find](/Users/ploki/GitHub/edvr-0.19.1-mfx/src/d3d11/flat_camera_table.h:84) always searched its active range on registration, writes/copies and fresh camera answers. Its separate kept-answer cache already skips stable draws; this optimization does not count those eliminated historical searches as new savings.

After: repeated identity uses the existing one-comparison last-hit path; interleaving uses a bounded positive hash-to-slot hint. Every indexed hit checks the live count and current slot identity. Only a miss or index displacement scans the original range. No allocations, locks, COM calls, negative memo, or cached camera data are added. Two 64-slot indexes add approximately 4.25 KiB of fixed storage in total.

Correctness constraints: entries own retained resource references and live identities are unique. Reused slots are rechecked; current width, rows, validity, mutation serial and sequence come from the current entry. Camera claims, capture, maps, invalidation, newFrame and kept-answer generations are untouched. Full context/table clear resets the index. Discarding one projection buffer now preserves unrelated warm entries; writes/newFrame need no index reset because it stores only slot hints. Calls remain under existing owner-thread rules.

Evidence: the persistent performance test compares 20,000 alternating late-bank lookups against the **previous last-hit implementation**, not just an uncached strawman. Every returned entry agrees. Identity reads were **1,289,999 → 40,124 (96.9% fewer)** including cold admissions. Single-resource hits remain one comparison. Collision displacement, slot/pointer reuse, changed metadata, negative miss followed by admission, count shrink/growth and empty-table cases pass. The existing camera randomized reference and all ten mutation controls pass byte-for-byte row/sequence equivalence.

This is CPU table work. No GPU operation is removed. Savings depend on actual bank occupancy and interleaving; stable kept-camera answers were already cheap. Revert the resource helper and camera integration together to reverse this optimization; no other optimization depends on it.

## 2. Reuse pinned bound-view metadata

EDHDR analogue: immutable identity remains authoritative despite unrelated generation changes; provisional decisions must still retry. EDVR's [view()](/Users/ploki/GitHub/edvr-0.19.1-mfx/src/d3d11/flat_runtime.cpp:2162) applies that distinction to physical D3D11 view/resource metadata only.

Before: every identical OM binding or frame-generation bump discarded a successfully held RTV/DSV and reran `bindingResolve`. Its actual path is GetResource → GetType → GetDesc → Release, followed by reacquiring the held view. With an existing pin, this also releases/re-AddRefs the view. Both target and depth are consulted for admitted draws; tone/copy SRV slots use the same resolver.

After: the same successfully pinned view retains its immutable description/resource identity. The recorded binding generation still advances. A changed view or null binding releases and refreshes as before; failed resolution remains unpinned and retries on the next generation. Existing resize/reset clears all View records. No rendering decision, contents, camera generation, shader identity or proxy binding changes.

Evidence: a native harness compiles the **actual extracted before/after view function bodies**, with fake owned views and a resolver stub. It checks identical rebind/frame bumps, new identities and dimensions/formats, null, unsuccessful resolution/retry, unchanged-generation retry suppression, generation wrap and resize/reset. All **20,028 checks pass**, including balanced ownership. For 10,000 redundant reads of two bound views: resolver calls **10,000 → 2**, held AddRefs **10,000 → 2**. The real resolver therefore avoids four metadata/resource COM calls plus pin Release/AddRef for each successful redundant refresh. There is no measured driver latency or FPS attribution.

Revert only this view-function hunk to reverse the optimization independently.

## Focused validation on macOS

- Fresh `edvr_perf_test --self-test`, compiled with Clang C++17 and the existing native adapter: **298 targeted checks plus the complete CPU census rig passed**.
- Existing `flatCameraTableTests()`: randomized reference and ten deliberate invalidation/key mutants passed.
- Actual before/after view harness: **20,028 checks, zero failures**.
- Extracted existing CPU, camera-table and query-wiring checks: **157 checks, zero failures**.
- MinGW C++17 syntax-only compilation of the entire production `flat_runtime.cpp` (using existing `build/gen`) and `flat_projection_runtime.cpp`: passed. This is not MSVC or GPU validation.
- Task-relative diff and `git diff --check`: reviewed/passed. Only four production headers/source and one cache test header changed, plus this report. Other task-entry files retained their hashes; all tracked/nonignored EDHDR files retained their hashes.

Native validation sources/binaries and pre-edit snapshots are retained at `/private/tmp/edvr-0191-working-upscaler-8k7zizto`. Existing build outputs were only read for generated headers; validation outputs went to that temporary directory.

## Remaining likely bottlenecks

1. Full draw classification, projection ownership/preflight and per-producer motion MRT setup, especially on foot. Existing lazy state forwarding and kept-camera answers already help; their residual cost needs existing opt-in CPU census data from a working backend.
2. GPU color CopyResource, depth/motion extraction, temporal preparation, backend evaluation and UI/foreground composition. These remain full-resolution bandwidth/dispatch work. Copies currently separate read/write resources and preserve overlay/temporal ordering; removing them without hazard proof risks stale inputs and composition.
3. Backend context isolation and restores. EDHDR's narrower output state block does not establish the mutation contract for MFX/FSR/NGX and cannot safely replace EDVR's isolation.
4. Immutable inputTexture validation: roughly eight COM calls per input per evaluation before optional foreground/overlay inputs. A future bounded descriptor cache must recheck current size/device/format/role admission rather than memoizing acceptance. It is lower frequency than the changes above and remains untouched.

No source evidence establishes that either CPU change alone explains the observed 2× EDVR-on/off gap. Separating CPU hook time from GPU evaluation/bandwidth remains necessary for the next optimization choice; use existing facilities without adding instrumentation.

## Windows/MSVC handoff for Muse

Rebuild from this exact dirty source tree, keeping the previous performance DLL intact. Use the project's existing x64 MSVC flags/dependencies and shader headers, with **fresh isolated object, executable and DLL output paths**; do not promote via the clean-tree `--dll-only` path or overwrite baseline outputs.

1. Compile the production proxy, including `flat_runtime.cpp` and `flat_projection_runtime.cpp`, and link the unchanged MFX3/MFX4/FSR3 integrations.
2. Rebuild current `edvr_perf_test` and `flat_temporal_test` sources using the existing invocations at build.bat's `rig_edvr_perf_test`/`rig_flat_temporal_test` labels. Run their `--self-test` and existing dry-run checks; do not use stale executables.
3. Rebuild/run the affected projection/context cases in `flat_mono_resolve_test` with D3D11/WARP, then existing FP16/HDR, temporal/backend and UI preservation cases. Confirm resource replacement/resize and motion/jitter/history behavior on the Windows runtime.
4. Record source/binary fingerprints and outputs so the A/B cannot accidentally compare stale DLLs. No deployment or game launch was performed here.

## Short authorized in-game A/B plan

Use the previous performance DLL as A and the new isolated optimized DLL as B. Same settings, working MFX3 + FP16 HDR, render scale, output resolution, GPU/driver and proxy chain; profiling off for the FPS comparison. Preserve both binaries and configs.

Run warm-up followed by at least three 60-second **A–B–B–A** pairs at the same stationary on-foot settlement/hangar camera, then a repeatable on-foot movement route. Record median and p95 frame time, FPS, GPU utilization and any CPU/GPU limiter indication using the existing capture method. Compare within pairs to reduce shader warming and scene drift; do not credit differences within normal run variance.

Repeat the primary scene with working FSR3 and, where available, MFX4. Check HUD/weapon motion, UI, depth edges, jitter/history stability, scene transitions and resize; exercise LDR/FP16 HDR coverage without changing reconstruction quality. A separate matched short capture with the existing opt-in CPU/GPU census can identify residual costs, but its timings must not be mixed with the profiler-off FPS result. Stop and retain the relevant source/binary/log identity on any correctness regression.
