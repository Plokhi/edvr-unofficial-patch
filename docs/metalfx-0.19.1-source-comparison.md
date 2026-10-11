# MetalFX v0.19.1 equivalent source comparison

## Status

Final source comparison complete on 2026-10-12; targeted Windows and macOS
validation passed, with the full runner and runtime testing incomplete. The user
authorized one local commit. This equivalent source comparison separates the
modern MFX3/FP16 transplant from selective MFX4 additions and upstream adaptation.

## Reproduction and findings

1. Verify `v0.19.1^{commit}` = `d78d9d8591f2f4c0be789b6fecc3d8ec6060756e`.
2. Compare source series `git diff v0.19.0 exp/fp16-copy-source`.
3. Apply its nine individual patches to pristine v0.19.1; the only manual
   reconciliation is the union of map EASU/SMAA and MetalFX includes.
4. Compare that intermediate result with the target to isolate the selective
   MFX4 additions and new map/fp16 view guard. After the local commit, reproduce
   the final delta with `git diff v0.19.1 mfx/0.19.1-port` (one port commit).

Saved artifacts under `build/port-validation`:

| Artifact | Evidence |
|---|---|
| `source-series.patch` | Full original modern MFX3 + FP16 source delta. |
| `metalfx3-fp16-on-0191.patch` | Exact intermediate transplant, before MFX4 additions. |
| `metalfx4-adaptation.diff` | Initial final-versus-intermediate comparison; MFX4 plumbing, tests/docs and map output guard. Final fixture corrections are recorded below. |
| `integration.patch` | Complete target delta against the verified release. |
| `source-comparison.json` | Changed-path census, protected-file byte comparisons and key-set comparison. |
| `python-gates-final.json` | Target and pristine-baseline gate results. |

The backend's header and implementation have identical non-comment/non-whitespace
code tokens to `9650c449`. MFX3 resource/prep/finish integration instead comes
from the later 0.19.0 series, preserving F2 skin inputs and fixed upstream HDR
exposure that a wholesale old-branch replacement would lose. The same generation
argument uses that common frontend; source route geometry remains E >= R.

No upstream file was replaced by a full older revision except the standalone
MetalFX backend and its own checker, which do not exist upstream. The checker
also retains the modern formatting repairs and adds the map-output invariant.
Of the release's existing files, changes are confined to the MetalFX integration,
FP16 copy compatibility and their tests/config text/build inclusion. Unrelated
upstream files stay untouched.

Protected release files (all byte-identical):

- `src/d3d11/device_hook.cpp`
- `src/d3d11/device_hook.h`
- `src/d3d11/engine_velocity.cpp`
- `src/d3d11/flat_ui_layer.cpp`
- `src/d3d11/flat_ui_layer_math.h`
- `src/d3d11/flat_temporal.cpp`
- `src/d3d11/skin_join_shader.h`
- `src/d3d11/map_easu_shader.h`
- `src/d3d11/map_smaa_shader.h`
- `src/d3d11/input_gate.cpp`
- `src/d3d11/menu.cpp`
- `src/d3d11/journal_watch.cpp`
- `src/d3d11/vr_ssaa_gate.cpp`
- `src/d3d11/vr_ssaa_hold.cpp`
- `src/common/runtime_profile.h`
- `src/openxr/native_runtime_host.h`

The upstream and target INIs have the same 64 active keys, with no additions or
removals. The retired 0.18.x settings and removed System Map motion are absent.
The new map EASU return changes only view selection when no sRGB view exists;
map temporal bypass, filter order and GPU resource lifecycle remain upstream.

## Final review delta

Compared with the prepared pre-Windows port, all production blobs are unchanged.
Windows corrected only the FP16 GPU fixture's refusal expectation/engine inputs
and the FP16/MFX4 fixtures' expected-jitter bookkeeping. Two test comments were
clarified during final review. All 28 motion golden values match the validated
0.19.0 reference exactly; no production branch was altered to satisfy a test.

The final diff contains 26 code/config/tool paths plus these two reports. All five
SMAA files are byte-identical to upstream, so no line-ending noise is carried.
The graphics DLL hash matches Muse's report and the production inputs match the
pre-Windows snapshot. See the integration report for incomplete full-runner
coverage and inherited checker/lifetime limits.
