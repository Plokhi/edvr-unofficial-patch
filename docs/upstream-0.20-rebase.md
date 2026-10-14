# EDVR 0.20.0-rc.1 downstream rebase — 2026-10-14

## Status

- Source rebase completed on `mfx/0.20-port`; both downstream source commits
  retain their boundaries, authors and author dates.
- Exact upstream base: `v0.20.0-rc.1`, annotated tag
  `4d02c01e179e32510b249b0a394362ab8266153d`, commit
  `c8a39656bd9694b2a93e282d5581287aa9fe48bc`.
- The original `mfx/0.19.1-port` remains at
  `3428a02de3706079253ff49ce637b8850385c168`, with its original uncommitted
  files and index unchanged.
- Portable verification passes. Full Windows/MSVC build, production shader
  compilation, WARP execution, installer validation and Elite/DXMT flights
  remain pending. No full-build receipt or runtime qualification is claimed.
- Temporary diagnostic keys retained: `diagnostic.flat_session`,
  `diagnostic.flat_motion`, `diagnostic.flat_preparation`,
  `diagnostic.flat_composition`, `diagnostic.flat_backend`.
- Next validation: run the full `build.bat` by absolute path on Windows from
  this exact branch, then the existing isolation matrix and VR/flat switch
  flight. No installation or game launch was performed here.

## Preserved state and provenance

The merge base is upstream `v0.19.1`,
`d78d9d8591f2f4c0be789b6fecc3d8ec6060756e`. The branch already contained a
single aggregate downstream port commit. This rebase does not reconstruct or
squash earlier feature-branch history; those references remain intact in the
shared repository and preservation bundle.

| Original commit | Rebased commit | Intent |
|---|---|---|
| `3428a02d` | `56fd3097` | Existing MetalFX 3/4 and FP16 presentation port |
| `bac8e9dc` | `223f1867` | Dedicated snapshot of all uncommitted diagnostic/performance source, tests and notes, adapted to 0.20 |

Backup branches:

- `backup/mfx-0.19.1-before-0.20-20261014`: original committed HEAD.
- `backup/mfx-0.19.1-diagnostics-20261014`: the complete diagnostic source
  snapshot, parented to that HEAD.

The snapshot was created through a separate Git index and `commit-tree`. The
original checkout's HEAD and staged/unstaged diffs never changed. All 3,534
original file hashes were checked after the rebase and still match.

Persistent recovery files are in the original checkout's ignored build
directory:

`/Users/ploki/GitHub/edvr-0.19.1-mfx/build/rebase-preservation-20261014/`

- `history.bundle`: verified, self-contained history with all original
  references and the diagnostic snapshot.
- `worktree.tar.gz`: complete original checkout files, including ignored build
  outputs, dependencies, local notes and `vc140.pdb`.
- `manifest.json`: per-file SHA-256 hashes, verified against every archived
  regular file.
- `staged.patch`, `unstaged.patch`, `status`, `refs`, `graph`: original Git
  state and changes.
- `validation/`: portable test logs, syntax adapters and the final two-commit
  `range-diff.txt`.

`vc140.pdb` is retained in the archive rather than added to the source patch
stack. No original file was discarded.

## Conflict decisions and semantic adaptation

| Area | Resolution |
|---|---|
| Unified installation | Keep upstream's schema-2 scope-both installer and shared flat settings generator; move MetalFX documentation into `flat_ini_bytes()` and keep XeSS choices. |
| Dynamic profile switching | Keep upstream atomic profile, install-scope hooks and commit boundary. Clear flat isolation state at the actual profile commit before publishing the next profile; ordinary resize does not disable the experiment. |
| Flat configuration | Add the five diagnostic keys to upstream's shared `flat_profile_keys.h`, preserving every upstream key and the installer's matching flat view. |
| Flat temporal routing | Retain XeSS and its slot/jitter-cycle constraints beside TAA, DLAA, DLSS, FSR, MFX3 and MFX4. Preserve both upstream jitter-cycle assignment and downstream FP16 source-format capture. |
| MetalFX generations | Keep separate MFX3/MFX4 admission, availability and strict generation dispatch. Preserve the context-based DXMT extension seam and shared trained-upscaler route. |
| FP16/directColor | Retain the FP16 copy route and source sampling optimization, with upstream HDR/UI contracts and fallback paths. No new performance gain is asserted. |
| Motion/producer isolation | Combine the diagnostic phase gate with upstream's full-work stand-down gate; preserve source-free, foreground, overlay and producer policies. Add XeSS to backend work accounting and the existing HDR/FP16 isolation fixture matrices. |
| D3D11 ownership and stations | Keep upstream hook registration, install-scope hook ownership, optimized-station draw skips and `ownedFrame` UI composite parameter; retain downstream profiling around them. |
| CPU timing | Keep upstream sampled Map-wait timing and call accounting, wrapping both forwarding branches with downstream resource-submission profiling. |
| Forwarding/chaining | Keep shared/flat `advanced.real_dll` selection, self-reference refusal, chained/system export fallback and reentrant device creation. Guard upstream's new selection log under the downstream silent `EDVR_PASSTHROUGH` policy. |
| Tests | Adapt the atomic profile test and native OS adapter; preserve foreground wiring mutation controls, adding a control that detects removal of the diagnostic gate. |

No downstream source commit was skipped, dropped or squashed. The first rebased
commit still changes all 28 originally changed files. In the second,
`runtime_profile.h` no longer differs because its key changes correctly moved
to upstream's shared list. Additional files cover that list, the profile switch
boundary and its regression checks.

## Verification and limits

Passed on macOS:

- Performance/diagnostic native rig: 720 targeted checks plus the existing flat
  CPU rig.
- Native temporal tests; mode-watch rules (67 checks); launch-profile rules
  (119); inactive-profile cost (52).
- Copy-structure and foreground routing, including XeSS/MFX3/MFX4 at native,
  upscaled and supersampled sizes; foreground source wiring and mutation
  controls.
- Sibling/shadow policy, shader/runtime wiring and mutation checks, with
  floating-point contraction disabled.
- HDR isolation, pass ordering and breadcrumb regressions.
- MetalFX source checker: 106 checks; its 38 mutation fixtures pass.
- Config contract: 132 read keys, 132 documented keys, all agree; installer
  resource and flight-log parser self-tests.
- The build's Python gates: 91 of 99 pass. Each of the eight failures also
  fails on pristine upstream `v0.20.0-rc.1` on this host.
- Windows-target syntax/type checks of all 15 changed production translation
  units. Four affected rig translation units also parse: performance, flat
  temporal, mono resolve and VR world resolve.
- Exact upstream ancestry, two-to-two commit mapping, unchanged
  authorship/author dates, clean rebased checkout, whitespace and
  conflict-marker checks.

The eight upstream-baseline Python failures are `build_lock.py`,
`cpu_profile.py`, `flat_packet_replay.py`, `compile_variants.py`,
`dxbc_disasm.py`, `install_edvr.py`, `kinematic_json_selftest.py` and
`scheduler_stack_json_selftest.py`. They involve Windows APIs, Windows
path/file behavior, or build-produced Windows executables.

Syntax checks use declaration-only shader placeholders, not production
bytecode. The two hook files require a test-only MinGW SEH adapter/permissive
function-pointer parsing; the device-hook template also requires an explicit
conversion in a temporary source copy. Those checks do not validate MSVC
exception semantics, shader execution, linkage or ABI behavior. The full
weapon-motion rig cannot parse here without DirectXMath; its affected sibling
policy is covered by the focused native checks. The new XeSS isolation GPU
cases compile but have not executed on WARP.

The system has no PowerShell, Windows `cmd`, MSVC or Wine runner. A fetched
upstream tag is not a successful build. An attempted simultaneous update of
`origin/main` encountered an existing root-owned reflog; the exact release tag
and its commit were fetched and independently verified, so the rebase does not
rely on that tracking reference. File ownership was not changed.

## Recovery and the next upstream release

To inspect the preserved source in another clean checkout, use
`backup/mfx-0.19.1-diagnostics-20261014`. To recover the original committed
branch, use `backup/mfx-0.19.1-before-0.20-20261014`. Restore the archive only
into an empty recovery directory after checking `manifest.json`; it includes
local outputs as well as source.

Future upstream incorporation can use the normal Git rebase workflow on this
stack: preserve local changes, fetch and verify the next tag, create a new
branch from `mfx/0.20-port`, and rebase with `v0.20.0-rc.1` as the old base.
The exact 0.20 release commit is now an ancestor, so a new manual aggregate
port is unnecessary.
