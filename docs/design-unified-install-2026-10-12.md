# One install for flat and VR: Elite's 3D mode picks the profile (design, 2026-10-12)

## Status

- **State (2026-10-12):** phase 1 BUILT on branch
  `claude/unified-install-feature-74bc24`, NOT FLOWN, not on main (Sean
  approves before it ships). Installed to Steam (`--scope both`).
  - The launch decision (`src/common/install_scope.h`, rig
    `runtime_profile_test`); the Settings.xml watch, log-only
    (`src/d3d11/mode_watch*`, rig `mode_watch_test`); Passthrough plus a
    restart toast at a confirmed switch; the H7 swap-chain line;
    `install_edvr.py --scope`; `edvr_log.py --switches`.
  - Built differently from 3.2: the watch keeps the file's last confirmed
    class apart from the class EDVR runs, and starts from the launch's
    effective mode. A read posts only when the FILE changes, so neither a
    live rise nor a `/vr` launch over a file at 0 is undone by the file.
  - Rigs run under a pinned LOCALAPPDATA (Settings.xml mode 4), so the gate
    does not follow the 3D mode the maintainer last flew (549e8d91).
  - Not built yet: the switch rig (phase 2, with its leak count).
- **Goal (Sean, 2026-10-12):**
  - one installer;
  - EDVR runs its flat profile when Elite's 3D mode is off (or anaglyph), and
    its VR profile otherwise;
  - it follows a mid-session switch seamlessly.
- **Decided (Sean, 2026-10-12; section 8):** Q1 switch live; Q2 unknown is
  VR; Q3 no descriptor follows the 3D setting; Q4 no override key (the
  launcher's `/vr` and `/novr` are the game's own, so they count); Q5 one
  installer with a scope prompt; Q6 modes 0-1 flat, 2-6 VR; Q7
  creation-time settings follow at the next launch; Q8 SBS is VR, low
  priority.
- **Reverses:** the two-installer plan (`design-flat-temporal-aa-2026-09-23.md`,
  `design-plugin-architecture-2026-09-30.md`) and the "set once per process"
  profile. **Keeps:** one graphics DLL, separate mono and VR adapters.
- **No hook sees the 3D mode value** (section 2): the switch signal is a
  watch on Settings.xml, prompted by four side-effect hooks (3.2).
- **Design:** scope vs mode (3.1), the switch signal (3.2), one commit point
  at the frame boundary (3.3), what cannot switch live (3.4), the hold keyed
  to the mode (3.5), the scope prompt in one installer (4).
- **Still open for Sean:** none.
- **Hypotheses (section 7 names the evidence for each):**
  - H1: a flat launch never loads `openvr_api.dll`, and 3D-on loads it then.
  - H2 HOLDS both ways (Steam 2026-10-12): `/steam /vr` over a file at 0
    went VR; `/steam /novr` over 4 stayed flat with no HMD mode offered.
    Both applied (6fbb4919). The command line is logged redacted.
  - H10 (open): a `/vr` launch over a file at 0 left the file stale: 3D off
    took the game out of the headset but re-wrote 0, so the watch missed
    it. Flight D: what an unrelated apply writes there, and whether the eye
    submit rate (ed0bddd8, log only) stops at 3D off.
  - H3 REFUTED under a launcher switch: the switch, not the file, is the
    start mode (H2). Holds for a launch with neither.
  - H4: after 3D off the OpenXR session idles at no flat cost.
  - H5: the Settings.xml path resolves on every store and on Proton.
  - H6 HOLDS (Steam B): a 3D apply rewrites the file; the watch saw the
    write 70-80 ms after the setter call, confirmed 250 ms later.
  - H7: a toggle keeps the device and swapchain (no new-chain line in B).
  - H8: the always-installed hooks cost VR nothing measurable.
- **Ruled out while designing:** section 9.
- **Temporary keys:** none. No user-facing key is added.
- **Next:** flight D on Steam (H10). Then a launch with neither switch, if
  the launcher can make one (H1's second half).

## 1. What exists today

| Piece | VR edition | Flat edition | Where |
|---|---|---|---|
| `d3d11.dll` | same binary | same binary | `build.bat:605-611`, `tools/package_native.py:57-60` |
| `edvr_profile.ini` | `profile = vr` | `profile = flat` | `plan.cpp:221`, `gen_installer_rc.py:360-362` |
| Settings file | `edvr.ini` | `edvr-flat.ini`, which falls back to `edvr.ini` while absent | `config.cpp:142-165`, `ini_name.h` |
| `Openvr\win64\openvr_api.dll` (EDVR's OpenXR runtime), loader, licence, `edvr_openxr.ini` | installed; the game's original is renamed `openvr_api_orig.dll` | not installed; converting from VR restores the original | `plan.cpp:571-662` |
| Installer | `edvr-installer.exe` | `edvr-flat-installer.exe`, the same sources built with `/DEDVR_INSTALLER_FLAT=1` | `build.bat:2563-2620`, `payload.cpp:52-54` |
| Release archives | `edvr-<ver>.zip`, `edvr-installer-<ver>.zip` | `edvr-flat-<ver>.zip`, `edvr-flat-installer-<ver>.zip` | `package.bat:11-14`, `package_native.py:254-317` |

- **When the profile is chosen.** `runtimeProfileInitialize` is the first line
  of `Config::init` (`config.cpp:134`). That runs from `initOnceCallback`
  (`d3d11_proxy.cpp:271`) at the first `D3D11CreateDevice`, or from the fxcfg
  loader hook (`vr_ssaa_hold.cpp:196`). It never runs in DllMain.
- **How it is stored.** The profile is a plain, non-atomic global
  (`runtime_profile.h:8`). About 131 `runtimeFlatProfile()` sites and 18
  `runtimeVrProfile()` sites branch on it.
- **Profile-gated installs at device creation:**
  - flat only: the hull, domain and GS creation hooks
    (`device_hook.cpp:2566-2579`), ResizeBuffers (`:2953-2963`) and
    vscreen's PSSetConstantBuffers hook (`vscreen.cpp:6908`);
  - VR only: the window trampolines (`vr_ssaa_gate.cpp:135`).
- **Mismatches today.**
  - VR profile in a flat game: the VR fixes starve and log so (`vscreen.cpp`
    around 6150-6190).
  - Flat profile in a VR game: nothing detects it. `flat_standdown.h` is temporal
    AA's shape refusal, not this.
- **The OpenXR runtime** (`src/openxr`) has no profile logic. The Oculus route
  is installed in DllMain, profile-blind, and pushes Elite to OpenVR.

## 2. The signal: Elite's 3D mode

- **Where it is saved:**
  `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml`,
  element `<StereoscopicMode>`.
  - The folder is spelled once in `elite_graphics_folder.h`. The DLL reads
    `LOCALAPPDATA` from the environment.
  - The parser is `parseStereoscopicMode` (`vr_ssaa_hold_math.h:15-34`). It
    takes a whole number from 0 to 6 and is rig-tested.
- **Values:**
  - 0 is off.
  - 1 is anaglyph and 2 is side-by-side (Sean, 2026-10-12).
  - 3 was seen at startup in VR flights.
  - 4 shows after a menu 3D-on. Gate H9 is open on it.
  - 5 is HMD Cinema (`fss-scanner.md`).
  - 6 is unnamed.
- **No hook sees the value.** A static-address read was ruled out: the mode
  sits behind virtual calls on heap objects
  (`vr-supersampling-gate-2026-10-10.md`). `vr_ssaa_gate.cpp:165` logs "live
  3D mode not located". What exists, and in which profiles:

| Hook | Profiles | What it sees at a toggle |
|---|---|---|
| Supersampling setter and getter, vtable 0x52E90B0 and 0x52E90B8 (`ui_panel_scale.cpp:444-507`) | both | Fires at toggles ("calls 5-7 passed 1.0 (3D off and on)"), and at ordinary Supersampling edits too |
| fxcfg loader slot 0x52E8368 (`vr_ssaa_hold.cpp:184-281`) | both, from DllMain | Reads Settings.xml itself. It ran about 12 times at startup; no evidence it fires at a toggle |
| Present window observer (`vr_display_observer.cpp:29-52`) | both | At 0 to 4: style 0x94020000 at 3840x2160 became 0x14C80000 at 1280x768, then ResizeBuffers on the same swapchain |
| Window mode request 0x7E9D50 (`vr_window_trampolines.cpp:110-160`) | VR | A kind-0 request at 3D-on, and a fresh Settings.xml read while VR is not live |
| Settings copy 0x281C0D0 (`vr_window_trampolines.cpp:163-169`) | VR | The render context and its Supersampling field; the copy source is unresolved |
| Runtime live flag `edvrQueryNativeRenderSizing().valid`, polled per Present | both | True at VR_Init. False only at VR_Shutdown (`native_runtime_host.h:2543`); no shutdown is recorded at 3D off |

- **Settings.xml timing is unmeasured.** The game writes it on menu-apply.
  The flights disagree on when:
  - the cf11d33c sessions read 3 to 0 and 0 to 4 correctly, 1.5 s after a
    setter call;
  - flight 8a591fba read the file "before the game rewrote it", 1.5 s after a
    setter call.
- **No flight has toggled 3D in the flat profile.**
- **Launcher switches (launch only).**
  - Forum reports say EDLaunch's `/novr` starts the game flat whatever
    Settings.xml says.
  - Epic's "Launch in VR" overrides a `/novr` set in the launcher's settings.
  - Steam lists `/VR`, with its effect unconfirmed.
  - Sources: [Frontier forum, launching without SteamVR](https://forums.frontier.co.uk/threads/is-it-possible-to-launch-ed-without-steam-vr.638753),
    [Frontier forum, VR with the launcher](https://forums.frontier.co.uk/threads/vr-with-elite-dangerous-launcher.638207/latest),
    [Steam discussion](https://steamcommunity.com/app/359320/discussions/0/2968397584533695071).
  - The DLL does not log the game's command line today.

## 3. Runtime design

### 3.1 Scope and mode

- **Scope** is what the user installed, read once from the descriptor:
  `both`, `vr` or `flat`.
  - New descriptor text: `[install]\r\nschema = 2\r\nscope = both\r\n`.
  - An older DLL reads schema 2 as `Invalid`: the fixes are off and chaining
    is kept, which is the right failure.
  - Reading older descriptors (Q3):
    - absent means `both`;
    - schema 1 `profile = vr` means `both`;
    - schema 1 `profile = flat` means `flat`. Such an install has no runtime,
      so VR there runs on the game's own `openvr_api.dll`, untouched.
  - A malformed descriptor stays `Invalid`, which is terminal.
- **Mode** is what the game renders: Flat or Vr. It is live (3.2).
  - Modes 0 and 1 are Flat. Modes 2 to 6 are Vr.
  - Unknown is Vr (Q2). The mode is unknown when the file is absent or
    unreadable, the tag is absent, or the value is out of range.
- **The active profile** is the mode when the scope includes it. Otherwise it
  is a new `Passthrough`: forward only, like `Invalid` but not terminal, so
  the next switch can leave it.
  - `runtimeFlatProfile()` and `runtimeVrProfile()` keep their names and
    become relaxed atomic reads of the active profile.
  - A setter changes it, called only at the commit point (3.3).
  - `runtimeProfileAllowsKey` follows the active profile, as now.
- **The launch decision**, in `runtimeProfileInitialize` (first in
  `Config::init`, not in DllMain), is a pure Windows-free function driven by
  a rig. It takes the descriptor, Settings.xml's text or its absence, and the
  command line. It applies the rules above.
  - `/novr` or `/vr` on the command line overrides the file for this launch.
    This ships only if H2 holds.
  - `parseStereoscopicMode` moves to `src/common`, so the decision, the switch
    watch and the hold share one parser.
- **Log.** The `installation profile:` line (`d3d11_proxy.cpp:289`) becomes,
  for example:
  `profile: flat (scope both; 3D mode 0 from <path>; command line ...)`.
  - It also logs the game's command line once.
  - If the new code never ran, a schema-2 descriptor logs `invalid`, so
    failure cannot pass for success.
  - `edvr_log.py --version` reports the line.

### 3.2 Seeing a switch

Since no hook sees the value, the game's own record of the setting is the
source, read when it is written:

- **The watch.** A change notification on the Options\Graphics folder runs on
  a worker. It could be the journal watcher's worker (`journal_watch.cpp:769`),
  which already watches files off the render thread.
  - On each write to Settings.xml it re-parses the mode.
  - When the mode moves between Flat and Vr, it posts a pending mode.
  - Reading at the write removes the timing problem behind flight 8a591fba's
    stale read (H6).
- **Hints.** These ask the worker to re-check now, and a stat of the file's
  write time backs them up once a second:
  - the Supersampling setter call (both profiles);
  - a window style or size change at Present (both profiles);
  - the runtime's live flag rising.
  - A pending mode must survive one re-read 250 ms later, so a half-written
    file cannot flip the mode. This follows the Bindings poll's two-check rule
    (`device_hook.cpp:1318-1336`).
- **The runtime's live flag rising** also posts Vr when the file still says
  Flat. That covers a VR start that writes no file, such as Epic's "Launch in
  VR".
  - It is not used for the fall: it never falls at 3D off.
- **Log.** Each hint, write, re-read and pending mode is logged with its time.
  Phase 1 ships the watch in this log-only form, so the first flight measures
  the order of events before phase 2 acts on them.

### 3.3 Applying a switch

- **One commit point:** the top of `presentFrameBoundary`
  (`device_hook.cpp:1493`). Both flat Present halves have run there, and no
  draw of the next frame has started. Draw thunks run on any thread, so a
  flip anywhere else splits a frame between pipelines.
- **The sequence on the render thread:**
  1. Tear down the outgoing pipeline while its predicate still answers true.
     `flatRuntimePresent` returns early once not flat (`flat_runtime.cpp:3265`).
  2. Flip the active profile.
  3. Switch the settings file (below).
  4. Run the configure sweep, forced.
  5. Log `profile switch: flat -> vr (Settings.xml mode 4 at <t>; hints ...)`,
     and show a toast.
- **Hooks are installed for every mode in scope, from the first device.**
  - Each hook gates per call on the active profile.
  - This matters most for the flat creation-time capture: input-layout
    private data, hull, domain and GS packets, PS bytecode for
    `FlatOverlayLayer`, and `flatProbeShaders` (`device_hook.cpp:599-757`).
    After a VR-to-flat switch, objects created earlier would otherwise have
    none.
  - The rest install at startup too. That keeps one hook set to reason about:
    - ResizeBuffers (`hookSwapChain` aborts if it fails, `:2964`);
    - vscreen's PSSetConstantBuffers hook;
    - the window trampolines and settings copy;
    - terrain cull cover.
  - Cost is H8. If a hot hook costs VR anything, it installs at the first
    switch into flat and is never removed.
- **Settings file.** A new `Config::switchProfile`:
  - It re-picks the candidates under the lock. The flat profile reads
    `edvr-flat.ini` and falls back to `edvr.ini`; VR reads `edvr.ini`.
  - It snapshots `m_path` for readers. `m_path` is read unlocked today by
    `reloadIfChanged`, the menu writer and `iniName()`.
  - It parses with the `m_lastWrite` shortcut bypassed.
  - It starts from defaults when the file is missing. Today `parse()` on a
    missing file keeps the old map (`config.cpp:215-222`), which would carry
    one profile's values into the other.
  - It bumps `m_generation`.
- **The configure sweep** is `vScreenRefreshConfig`'s roughly 35
  `*Configure(cfg)` calls (`vscreen.cpp:5526`), forced. It is extended with
  the pieces outside it:

| Subsystem | At a switch |
|---|---|
| Flat runtime, temporal, camera inject, flat UI layer | `flatRuntimeResize` (`flat_runtime.cpp:2544`): the full flat teardown, already used at resize and mode-off |
| VR UI layers, panel scale | `releaseLayers`, re-run `uiLayerConfigure`, `uiPanelScaleSetFlatTemporal` |
| DLSS, FSR | bump `g_presetGen` (recreates features). Expose dlaa's file-local `releaseFeatures`; `dlaaShutdown` drops NGX whole. Call `fsr3ReleaseFeatures` |
| Engine velocity | `engineVelocityFlatAbandon`, then `engineVelocityConfigure(false)` and `(true)` |
| Menu | `closeMenu`, `inputGateSetPrivate(false)`, `menuFlatResize`, clear `s.configured`, rebuild pages and the snapshot |
| vscreen | reset `quietPasses`, re-sample `drawGateSet` |
| Supersampling hold | re-set `g_holdAllowed` (set once at the loader today; 3.5) |
| Terrain checkerboard | re-arm `g_startTried`; its reader thread stays |
| Journal watcher | nothing: one worker for both, filtered per call |

### 3.4 What cannot switch live (Q7)

These are patched into the game or the device at creation:

- texture anisotropy and LOD bias (`device_hook.cpp:2606-2608`);
- `fix.vscreen_res_width`, patched into game code (`device_hook.cpp:2894`).
  Images already allocated keep their size.

Decided (Q7): they keep the launch mode's values until the next launch. The
switch's log line names any whose value differs between the two settings
files.

### 3.5 The OpenXR runtime across switches

- **3D off.** Elite is not recorded calling VR_Shutdown, so the session stays
  up and the live flag stays true (H4).
  - The Supersampling hold therefore keys on the active profile being Vr, not
    on the flag alone. Otherwise a VR-to-flat switch leaves flat Supersampling
    held at 1.0. `edvr_log.py:5222` records that as "not chased, by design";
    it must be chased now.
- **3D on after a flat launch.** Whether Elite delay-loads `openvr_api.dll`
  and calls VR_Init at that moment is H1. The runtime needs no profile logic
  either way.
- **A new swapchain at a toggle** would be ignored silently today
  (`hookSwapChain`, `device_hook.cpp:2944`). It gets a log line now (H7).

## 4. Installer and packaging

- **One installer.** `edvr-installer.exe`.
  - Retire `EDVR_INSTALLER_FLAT`, `edvr-flat-installer.exe`,
    `rig_flat_installer` (`build.bat:2603-2621`),
    `gen_installer_rc.py --profile flat` and `package_native.py --profile flat`.
  - The release is `edvr-<ver>.zip` and `edvr-installer-<ver>.zip`.
  - The flat README text folds into `release/README.txt`.
- **The scope prompt (Q5).** The main window asks "Install EDVR for:", with
  three choices:
  - VR and flat (recommended);
  - VR only;
  - Flat only.
  - It defaults to the installed scope, or to both on a fresh install.
  - The command line takes `--scope both|vr|flat`, with the same default, for
    unattended runs.
  - The status line reads the game's 3D mode through the shared parser:
    "Elite's 3D mode is off: EDVR runs its flat fixes."

| Scope | Installs |
|---|---|
| both | graphics DLL, descriptor, runtime with loader, licence and `edvr_openxr.ini`, `edvr.ini`, `edvr-flat.ini` |
| vr | everything `both` installs; flat mode runs `Passthrough` |
| flat | graphics DLL, descriptor, `edvr-flat.ini`; no runtime, so the game's `openvr_api.dll` stays |

- **Changing scope** re-runs the plan.
  - Moving to `flat` removes the runtime through today's VR-to-flat code: the
    ownership proofs at `plan.cpp:249-267`, then the restore at `629-662`.
  - Moving from `flat` installs the runtime.
  - No settings file is ever deleted by a scope change.
  - `--convert-profile`, its MessageBox (`gui.cpp:750-757`) and the profile
    mismatch block (`plan.cpp:234-240`) retire.
- **Payload.** Today's VR payload, plus the generated `edvr-flat.ini`.
- **Two merge bases:** `edvr.ini.base` and `edvr-flat.ini.base`. Today one
  base is shared and overwritten with the incoming edition's ini
  (`plan.h:84-86`, `plan.cpp:791-797`), which leaves one merge two-way.
- **Upgrades.** `installedProfile()` (`plan.cpp:106-114`) learns schema 2, and
  each earlier state upgrades in place:
  - `vr` and legacy installs default to `both`;
  - `flat` installs default to `flat`, with `both` one click away.
- **Settings screen.** A VR/Flat selector, limited to the scope, picks which
  file the list edits. It defaults to the current 3D mode's file. The flat
  list keeps its three rows (`settings.cpp:186-192`).
- **Mirror, bundler and uninstall.**
  - The mirror offers a restore when the scope's files are absent.
  - `editionOf()` reports the scope.
  - `log.dir` is read from both inis, and both folders are swept.
  - `--remove-settings` removes both inis.
  - `docs/installer.md` gets a Status block when this lands.

## 5. Developer tools

- **`install_edvr.py`.**
  - `--profile` becomes `--scope both|vr|flat`, default `both`, writing the
    schema-2 descriptor.
  - The flat path's refusal of VR leftovers (`install_edvr.py:1190-1198`)
    retires.
  - `--ini` writes the scope's settings files, backing up each.
  - `--dry-run` still writes nothing, and the self-test asserts it.
  - Flights pick a mode in the game's menu, not with a pin (Q4).
- **`package_native.py --check-installer`.** One installer embedding both inis
  and the schema-2 descriptor, and no flat artifacts.
- **`edvr_log.py`.**
  - It parses the profile line, the switch lines and the watch's event lines,
    with self-test fixtures.
  - A `--switches` view lists each switch with its hints and timings.

## 6. Tests and build gates

- **A decision rig** (`tools\runtime_profile_test`, added to `build.bat`).
  - Inputs: scope by mode by Settings.xml text, including a BOM, CRLF, an
    absent tag, 7, `-1`, empty and absent.
  - Old descriptors, `/vr` and `/novr`.
  - An older parser must read schema 2 as `Invalid`.
- **A switch rig.**
  - It loads `build\d3d11.dll` on WARP, as `journal_unload_test` does, drives
    frames, and flips the mode through a test-only entry point. It flips flat
    to VR to flat to Passthrough.
  - It asserts one switch line per flip and the teardown lines in order.
  - It asserts that no flat state survives into VR and that the settings file
    follows.
  - It asserts that GPU objects return to their count before the flip: no
    leak per switch.
  - Its control flips with teardown skipped, and must fail.
- **A watch rig.** It covers a write, a half-written file, the 250 ms confirm
  and a hint with no change, against the pure watch logic.
- **`installer_test`.**
  - Each scope fresh.
  - Each old state upgrading.
  - Each scope change, including the runtime removal proofs.
  - The two merge bases.
  - Dry-run writes nothing.
- **Unchanged:** `config_test`'s ini-name resolver and
  `tools\flat_sharpen_test`.

## 7. Flights

Environment: Steam, EDVR's OpenXR runtime via `openvr_api.dll`, Sean's
headset, per-eye size and DLSS version. Run `edvr_log.py --expect-build HEAD`
on every log. The phase-1 instrument flight is one session with three
launches. The phase-1 build switches to Passthrough rather than live.

- **Launch A, mode 0.**
  - Expect `profile: flat (scope both; 3D mode 0 ...)`.
  - The census shows no `openvr_api.dll` while flat (H1, first half).
  - Turn 3D on in the menu, then off, then on. Each toggle logs the setter,
    the window change, the Settings.xml write and the confirm, with times
    (H6).
  - It also logs whether `openvr_api.dll` loads and VR_Init runs at the first
    3D-on (H1, second half), and whether the device or swapchain changes (H7).
- **Launch B, the file now says 4.**
  - Expect `profile: vr (... 3D mode 4)` (H3).
  - Toggle off and on. Expect no VR_Shutdown line, the live flag staying up,
    and flat frame time with the session idle against launch A's (H4).
- **Launch C, the file not 0, EDLaunch with `/novr`.**
  - The logged command line, and whether VR goes live, settle H2.
- **The H8 A/B.** Two VR launches at the same spot, with the always-installed
  hooks against today's VR-only set, read with the CPU census.
- **The Oculus native-SDK supporter's rig**, 3D on: expect the VR profile,
  with the Oculus route still pushing Elite to OpenVR.
- **A Proton user's log:** the profile line names the Settings.xml path (H5).
- **Phase 2's flight** repeats launches A and B with the live switch: each
  toggle's switch line, the flat or VR fixes visibly in effect after it, and
  no leak across ten toggles (GPU memory from the VRAM watch).

## 8. Decisions (Sean, 2026-10-12)

- **Q1, seamless switching.** Decided: switch live (sections 3.2 to 3.5).
  Until phase 2 lands, a mid-session switch goes to Passthrough, with a toast
  to restart.
- **Q2, unknown mode.** Decided: VR.
- **Q3, no descriptor.** Decided: follows the 3D setting (scope `both`).
- **Q4, override.** Decided: none. There are no pins, and flights use the
  game's menu.
- **Q5, release.** Decided: one installer, with the scope prompt of section 4.
  `README.md`'s download section (around lines 59-74) collapses to one. The
  release notes tell flat players the prompt now chooses what they get.
- **Q6, modes.** Decided: 0 and 1 are flat; 2 to 6 are VR.
- **Q7, settings fixed at device creation** (3.4). Decided: they keep the
  launch mode's values until the next launch, and the switch's log line names
  them.
- **Q8, SBS (mode 2).** Decided: VR, for future development and low
  priority. No phase or flight waits on it.
  - SBS renders both eyes to the monitor's swapchain with no OpenXR session.
    The VR fixes are driven by the runtime's eye targets, so today they
    probably starve there and leave SBS as stock Elite.
  - The flat fixes would run temporal AA across two half-width views, which
    is why SBS is not flat.

## 9. Ruled out while designing

- ruled out: "the 3D setting is already hooked" as a source of the value,
  because the four hooks see side effects only, and the live mode was never
  located (`vr_ssaa_gate.cpp:165`).
- ruled out: the runtime's live flag as the 3D-off signal, because it falls
  only at VR_Shutdown, and no shutdown is recorded at a toggle.
- ruled out: re-reading Settings.xml on a fixed delay after a hint, because a
  read 1.5 s after a setter call returned the pre-toggle value in flight
  8a591fba.
- ruled out: flipping the profile anywhere but the frame boundary, because
  draw thunks run on any thread and one frame would split between pipelines.
- ruled out: installing the flat creation-time hooks at the first switch,
  because objects created before then would carry no capture.
- ruled out: deciding in DllMain, because file I/O is not allowed there
  (`oculus_route.h:22-25`).
- ruled out: "no VR frames means flat", because a missing stream proves
  nothing (`design-flat-temporal-aa-2026-09-23.md`).
- ruled out: one merged settings file, because it renames keys or changes their
  meaning by profile, and it loses per-profile tuning.

## 10. Order of work

1. **Launch decision and scope.**
   - The pure decision function, the schema-2 descriptor, and the profile and
     command-line log lines.
   - `Passthrough` on a mid-session switch: the atomic profile, flipped once
     at the commit point after `flatRuntimeResize`.
   - The watch in log-only form, with its hints.
   - `install_edvr.py --scope`, the `edvr_log.py` parsers, and the three
     rigs.
   - Then the instrument flight: launches A to C, and the H8 A/B.
2. **Live switch.** Built on what the flight measured:
   - every hook installed for the scope from the start;
   - `Config::switchProfile`;
   - the extended sweep and the teardown table;
   - the hold keyed on the profile;
   - the switch rig with its leak count.
   - Then phase 2's flight.
3. **Installer and packaging.**
   - One payload, the scope prompt, two merge bases, upgrades, the settings
     selector and the bundler.
   - Update `README.md`, `installer.md`, `manual-install.md`,
     `release/README.txt`, and AGENTS.md's `edvr-flat.ini` bullet.
4. **Release.** Notes via `reflow_notes.py`, and credits via
   `release_credits.py`.
