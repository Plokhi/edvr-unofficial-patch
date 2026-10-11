# EDVR v0.19.1 + MetalFX 3/4 integration — reviewed 2026-10-12

## Status

- Source port prepared on `mfx/0.19.1-port`, based on verified upstream `d78d9d8591f2f4c0be789b6fecc3d8ec6060756e`.
- Worktree: `/Users/ploki/GitHub/edvr-0.19.1-mfx`.
- Final review found no port-specific production blocker; MFX3, FP16 and optional MFX4 are retained.
- Windows production DLL/shaders and targeted tests passed. The full runner stopped
  after 3/138 jobs; no full-build receipt exists. Remaining rigs and Elite checks are outstanding.
- The user explicitly authorized one local commit after this partial validation on
  2026-10-12, overriding the usual full-build-before-commit gate for this handoff.
- No performance audit, deployment, game launch, bottle/override changes or push.
- No new settings or temporary investigation switches. Existing worktrees remain
  rollback points, including dirty checkouts; none was reset, rebased or cleaned.

## Sources and history

The annotated upstream tag object is `5a56d01247a07782681a4bdc89f1b97ee221be28`;
its peeled commit matches the requested `d78d9d8`. Remote `ls-remote` agreed.
The broad fetch encountered root-owned legacy ref/reflog paths; a targeted fetch
obtained the release tag. Repository ownership was not changed.

No single source branch contained the latest requested feature set:

| Source | Role |
|---|---|
| `feature/metalfx3` `1697fb9eeadc7ef38774f1c3007a998002725e67` | Complete modern MFX3 series on upstream v0.19.0, including spatial fallback and fp16-view safeguards. |
| `fork/feature/metalfx3` `4d92a433f5019983d443664bfbb097d757f26715` | Different commit identities, byte-identical tree to `feature/metalfx3`. |
| `mfx/0.19-port` `d1cc9f1214225437a9c681f2869670701db5a988` | Earlier MFX3 port, missing five subsequent fixes. |
| `exp/fp16-copy-source` `e7e2db7d83b16ae26372631be986db60f83aa3d9` | MFX3 plus committed FP16 copy-source/HDR compatibility and its GPU-test correction. Flight validation was still pending in the source. |
| `mfx/0.18.3-port`: `9650c449f5c0a7062e35c370cdc8f7674713c7ec` | Latest combined MFX3/4 implementation; supplies only the generation-specific pieces missing from the modern port. |

The MFX3 source series is `c98f0bea`, `d1cc9f12`, `22af2aaa`, `d05d68c1`,
`776a52b6`, `0d0308c4`, `1697fb9e`; HDR supplements are `3476d74a`, `e7e2db7d`.
`git cherry` marks these and `9650c449` as not patch-equivalent to upstream.
Existing macOS compatibility is already upstream: explicit DXMT state capture
`1c2519f1`, HDR breadcrumbs `e44db9c2`/`c7241450`, menu context-state removal
`2ebc970b`, and guarded input forwarding `e20dcad2` are ancestors of the release.
The original local `c8fbec4e` is not an ancestor; its compatibility purpose is
served by the newer upstream implementation rather than replaying its patch.

## Carried forward and adaptations

- `mfx` and `mfx4` selection, DXMT Ext/Ext1/Ext2 ABI, 88-byte descriptor,
  capability queries and strict generation dispatch. Unsupported MFX4 never
  invokes the MFX3 scaler. Existing spatial recovery remains available.
- Shared upstream colour/depth/engine-motion preparation, +1 raster jitter,
  reset/history behavior, trained-backend resolution routing and fp16 outputs.
- Independent MetalFX cbuffer lane, UI/late-overlay finish paths, source captures,
  diagnostics, menu choices and flat installer documentation.
- FP16 copy-source admission for render-below-output presentation, fp16 history
  and HDR-colour handoff, without relaxing the HDR route's R >= D requirement.
- VR on-foot routing retains its existing own-TAA behavior for both MetalFX
  selections; this is flat/desktop scaler support, not a new native VR scaler.

Only one patch application conflicted: resolver includes. Both upstream map
renderers and the MetalFX header were retained. All later MFX3/FP16 patches
applied cleanly; MFX4 was adapted selectively, not cherry-picked wholesale.

Focused review exposed a new overlap: upstream map EASU's sRGB return could
select a null view for MetalFX's fp16 output. The return now checks whether the
sRGB view exists, as primary resolve and bilinear fallback already do. Added
MFX3/MFX4 × sRGB × EASU/SMAA GPU fixtures and a mutation guard cover that seam.
Existing motion goldens were retained; new MFX4 dispatch/history cases run after
their comparison rather than replacing expected hashes. All 28 modern reference
motion goldens remain identical. Windows fixture corrections supply valid engine
inputs for FP16 continuation, assert the earlier HDR size refusal, and reset
expected jitter to the actual fixture phase; production is not changed to satisfy tests.

## Upstream preserved; exclusions

Upstream per-draw trace reductions, on-foot/weapon/skin classification and motion,
UI quality/layer composition, all three maps' temporal bypass and zero jitter,
EASU/SMAA map filtering, supersampling gates, fixed HDR exposure/composition,
context isolation/GPU capture and input/hotkey behavior are retained. Sixteen
protected subsystem files were compared byte-for-byte with the release and match.
Configuration still contains the same 64 active keys; only MetalFX mode choices
and explanatory text were added. No retired key was restored.

Excluded: kinematic allocation/retry experiments `37ce2e99`/`6db0485d`, dirty
checkout experiments, old context-isolation settings, removed System Map motion,
old foveation/mover/config machinery, and unrelated local reviews/tools. These
exclusions do not remove a feature from the new upstream base. No performance
optimization or architectural rewrite was undertaken.

## Validation

| Check | Result / limit |
|---|---|
| Python build self-test gates | 91/98 pass; remaining seven also fail on pristine v0.19.1 with the same interpreter. Bundled NumPy/Pillow resolved eleven initial dependency failures. |
| MetalFX guard | 109 checks pass, including live read-only comparison with the neighboring DXMT ABI. |
| MetalFX mutation self-tests | 38 fixtures caught, including generation misrouting and unsafe map sRGB return. |
| Native CPU tests | Temporal parser/math, copy-structure, HDR-route and copy-weapon suites pass. Windows type stubs used by the targeted flat CPU harnesses do not emulate GPU behavior. |
| Real Windows object cross-compilation | `metal_fx_engine.cpp`, `vr_world_route.cpp`, `flat_ui_layer.cpp` compile with MinGW. |
| C++ syntax/type checks | Resolver, flat runtime, resolver GPU rig, VR GPU rig and temporal rig pass MinGW checks. Separate one-byte shader declarations are explicitly syntax-only; they are not production bytecode. Resolver rig additionally needs host-only SEH parsing substitutions and an initializer-list include. |
| Upstream/source checks | Protected files identical, unchanged key set, backend code tokens identical to MFX4 source, `git diff --check` clean. |
| Windows production build and shaders | Muse compiled the production graphics DLL and temporal shaders; the saved log confirms shader compiler tests and DLL linking. Both DLL copies match SHA-256 below. |
| Windows targeted tests (Muse report) | flat_mono_resolve_test, temporal_test, flat_temporal_test PASS; openxr_native_test 5079/5079 PASS from mapped F:; vtable_test final PASS with known nondeterministic flake; backend 106/106 plus 38 mutation fixtures; config 126/126; FP16/F1/goldens/MFX3/MFX4 dispatch assertions PASS. |
| Final macOS checks, 2026-10-12 | Backend source 109/109 with local DXMT Ext/Ext1 cross-check, mutation fixtures 38/38, config 126/126 and its self-test, installer/log self-tests, rebuilt native temporal/copy-structure/HDR-weapon CPU harnesses all PASS. |
| Full build.bat receipt / remaining rigs / installer | INCOMPLETE. Saved runner log: OpenXR fails under UNC execution, 3/138 jobs ran. Direct mapped-F: OpenXR execution passed per Muse. Approximately 130 remaining rigs were not run by the full runner. No complete green build receipt or qualified installer. |

Remaining Python gates: Windows ctypes/DLL availability (`compile_variants`,
`cpu_profile`, `dxbc_disasm`), two missing Windows test executables
(`kinematic_json_selftest`, `scheduler_stack_json_selftest`), and upstream
macOS self-test failures (`flat_packet_replay`, `install_edvr`). They are
documented, not suppressed or fixed as unrelated work.

Full logs, commands and comparison patches are under `build/port-validation`.
See [source comparison](metalfx-0.19.1-source-comparison.md).

## Final review and binary correspondence

`build/d3d11.dll` and `build/edvr_openxr_graphics.dll` both hash to:
`35100a5a59a356a8aa6f1c71f06bc087e2903294a43a989bdd76862a07d9fe83`.
The production inputs match every blob in the saved pre-Windows integration
snapshot. Final review changes only documentation and two stale test comments;
the test-only jitter corrections are included. Thus the existing Windows DLL
corresponds to this commit's production source, based on the matching snapshot,
saved build log and Muse's build report. Its embedded pre-commit version stamp
is not the new commit identity; this is not a reproducible-build attestation.
No Windows rebuild was needed or performed during final review.

All five vendored SMAA files are byte-identical to upstream, including their
line endings. No generated files or temporary diagnostics enter the commit.
The validated 0.19.0 reference, including its dirty test header/index, is untouched.

The inherited build wiring runs `check_metal_fx_backend.py --self-test` on
synthetic fixtures, not the actual source tree. Run the checker without arguments
as a separate source gate; final review did so successfully. The checker does not
fully prove COM virtual-method order or Ext2 behavior on a live driver; local
DXMT lacks Ext2, whose declarations remain pinned to the reviewed source commit.
No validation-workflow changes are included in this port.

Inherited backend lifetime/diagnostic behavior is also unchanged: `cached()`
allocates a retained state per call, and generation dispatch logging shares a
once flag. These are source limitations, not changes introduced by this port;
long-session resource behavior and exhaustive switch diagnostics remain for the
separate audit. No speculative lifetime or performance changes were made.

## Outstanding validation

For a complete validation receipt, run this worktree's `build.bat` by absolute
mapped-drive Windows path using the normal full workflow, not `--dll-only`.
Require all remaining rigs and installer checks to pass; do not infer their
success from the compiled DLL or targeted runs. Do not deploy or push here.

Separate authorized runtime validation must cover MetalFX 3 and MetalFX 4 on
compatible DXMT/macOS/GPU combinations, strict MFX4 refusal on stock Ext1-only
DXMT, backend switching/reset/resize, R < D and supersampled/native sizes,
SDR and EDHDR FP16 presentation, on-foot weapons/depth/engine motion, UI/late
overlays, and Galaxy/System/Orrery transitions. Confirm maps remain temporal-off
with EASU/SMAA recovery, then restart temporal history on return to gameplay.
Windows non-MetalFX and VR smoke checks remain necessary. These behaviors are
source-preserved but unverified in Elite; no claim of successful runtime scaling
is made because DXMT's dispatch API returns void.
