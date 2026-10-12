# One install for flat and VR: Elite's 3D mode picks the profile (design, 2026-10-12)

## Status

- **State:** design only. Nothing is built. Written from a read-only survey of
  main `99476a0` (build, packaging, installer, runtime) and one web search on
  the launcher's VR switches.
- **Goal (Sean, 2026-10-12):** one installer and one install. EDVR runs its
  flat profile when Elite's 3D mode (Options, Graphics, 3D) is off, and its VR
  profile otherwise.
- **Reverses:** the "two installers" recommendation in the Status block of
  `design-flat-temporal-aa-2026-09-23.md`, and "the edition is fixed by the
  artifact" in `design-plugin-architecture-2026-09-30.md`. **Keeps:** one
  graphics DLL, separate mono and VR adapters, and exactly one adapter owning a
  frame stream.
- **What makes this small:** both editions already ship the same `d3d11.dll`.
  Only the descriptor (`edvr_profile.ini`), the ini and the OpenXR runtime
  differ (section 1). The profile is already chosen at run time, from a file.
  This design adds a third descriptor value, `auto`, whose profile comes from
  the game's 3D mode.
- **Proposed:**
  - Runtime: the `auto` decision and its log line (section 3.1).
  - Mid-session switches: the profile stays fixed for the launch. Flat stands
    down when VR goes live (section 3.2).
  - Installer: one installer, `edvr-installer.exe`, which always installs the
    OpenXR runtime and both settings files (section 4).
  - Developer tools and gates (sections 5 and 6), and one combined flight
    (section 7).
- **Open decisions for Sean (section 8):**
  - Q1: mid-session 3D switch.
  - Q2: unknown 3D mode.
  - Q3: manual installs without a descriptor.
  - Q4: no user override key.
  - Q5: retiring the flat release.
  - Q6: non-headset 3D modes.
- **Hypotheses to settle before code depends on them (section 7 has the
  evidence for each):**
  - H1: with 3D off, Elite never loads `openvr_api.dll`, so the
    always-installed runtime is inert in flat.
  - H2: the launcher's `/novr` (and `/vr`, Epic's "Launch in VR") reach the
    game exe's command line, and they override Settings.xml.
  - H3: Settings.xml `<StereoscopicMode>` at process start is the mode the game
    starts in, when there is no launcher switch.
  - H4: the VR profile in a flat session is inert. No flight has ever recorded
    this.
  - H5: the Settings.xml path resolves for Steam, Epic, Frontier and Proton.
- **Ruled out while designing:** section 9.
- **Temporary keys:** none proposed. No new user-facing key.
- **Next:** Sean answers Q1 to Q6. Then phase 1 (section 10): the decision, its
  log line and its rig, installed with `install_edvr.py --profile auto`, then
  launches A to C of section 7.

## 1. What exists today

| Piece | VR edition | Flat edition | Where |
|---|---|---|---|
| `d3d11.dll` | same binary | same binary | `build.bat:605-611`, `tools/package_native.py:57-60` |
| `edvr_profile.ini` | `profile = vr` | `profile = flat` | `plan.cpp:221`, `gen_installer_rc.py:360-362` |
| Settings file | `edvr.ini` | `edvr-flat.ini`, which falls back to `edvr.ini` while absent | `config.cpp:142-165`, `ini_name.h` |
| `Openvr\win64\openvr_api.dll` (EDVR's OpenXR runtime), loader, licence, `edvr_openxr.ini` | installed; the game's original is renamed `openvr_api_orig.dll` | not installed; converting from VR restores the original | `plan.cpp:571-662` |
| Installer | `edvr-installer.exe` | `edvr-flat-installer.exe`, the same sources built with `/DEDVR_INSTALLER_FLAT=1` | `build.bat:2563-2620`, `payload.cpp:52-54` |
| Release archives | `edvr-<ver>.zip`, `edvr-installer-<ver>.zip` | `edvr-flat-<ver>.zip`, `edvr-flat-installer-<ver>.zip` | `package.bat:11-14`, `package_native.py:254-317` |

Runtime facts this design rests on:

- **When the profile is chosen.** `runtimeProfileInitialize`
  (`runtime_profile.h`) is the first line of `Config::init`
  (`config.cpp:134`). Its one caller is `initOnceCallback`
  (`d3d11_proxy.cpp:271`), which runs at the first `D3D11CreateDevice` export
  call, before the real create.
  - It does not run in DllMain, so file I/O is allowed there. Its second way in
    is `edvrGraphicsEnsureInitialised` from the fxcfg loader hook
    (`vr_ssaa_hold.cpp:196`).
  - The profile is a process global set once. The ini file is chosen once.
  - About 131 `runtimeFlatProfile()` sites and 18 `runtimeVrProfile()` sites
    branch on it.
  - Flat-only device hooks are installed at device creation
    (`device_hook.cpp:2566-2579`, `2953-2963`).
- **Mismatches today.**
  - VR profile in a flat game: the VR fixes starve and log so (`vscreen.cpp`
    around 6150-6190, "inert -- not broken, starved"). Nothing else reacts.
  - Flat profile in a VR game: nothing detects it
    (`flat_standdown.h` is unrelated; it is temporal AA's shape refusal). The
    flat edition ships no runtime, so today this only meets the stock
    `openvr_api.dll`.
- **The OpenXR runtime** (`src/openxr`) has no profile logic. The Oculus route
  (`oculus_route.cpp`) is installed in DllMain, profile-blind, and pushes Elite
  to OpenVR.

## 2. The signal: Elite's 3D mode

- **Where it is saved:**
  `%LOCALAPPDATA%\Frontier Developments\Elite Dangerous\Options\Graphics\Settings.xml`,
  element `<StereoscopicMode>`.
  - The folder is spelled once in `elite_graphics_folder.h`. The DLL reads
    `LOCALAPPDATA` from the environment, as `device_hook.cpp`'s
    `eliteHmdMultiplier` does.
  - The parser already exists and is rig-tested: `parseStereoscopicMode`
    (`vr_ssaa_hold_math.h:15-34`). It takes a whole number from 0 to 6 ("the
    game clamps it to that range").
- **Values known:**
  - 0 is off.
  - 3 was seen at startup in a VR flight.
  - 4 shows in re-reads at the 3D-on switch, and what it means is open (H9 of
    `vr-supersampling-gate-2026-10-10.md`).
  - 5 is HMD Cinema (`fss-scanner.md`).
  - 1, 2 and 6 are unnamed.
  - Sean's rule needs only 0 versus not 0.
- **The game changes it while running.**
  - A 3D off-and-on toggle in one session kept the OpenXR session alive
    (`vr-supersampling-gate-2026-10-10.md`).
  - Settings.xml is written on menu-apply but never re-read mid-session, and a
    re-read 1.5 s after a change returned the stale value.
  - The live in-memory mode was never located; it sits behind virtual calls on
    heap objects.
- **Launcher switches may override it (H2).**
  - Forum reports say EDLaunch's `/novr` starts the game flat, so a 3D-on
    Settings.xml does not start SteamVR or Oculus.
  - Epic's "Launch in VR" menu item overrides a `/novr` set in the launcher's
    settings.
  - Steam's launch options list `/VR`, with its effect unconfirmed.
  - Sources: [Frontier forum, launching without SteamVR](https://forums.frontier.co.uk/threads/is-it-possible-to-launch-ed-without-steam-vr.638753),
    [Frontier forum, VR with the launcher](https://forums.frontier.co.uk/threads/vr-with-elite-dangerous-launcher.638207/latest),
    [Steam discussion](https://steamcommunity.com/app/359320/discussions/0/2968397584533695071).
  - Whether the switch reaches the game exe's command line, or travels another
    way, is unknown. The DLL does not log the game's command line today.
- **The live VR signal** is `edvrQueryNativeRenderSizing().valid`. It is true
  once EDVR's runtime has published eye sizes (`native_runtime_host.h:2833`)
  and is consumed by `vrSsaaHoldVrLive()`. It arrives after the device, the
  hooks and the ini choice, so it can confirm a decision but not make it.
- **Before the device,** the only sources are the Settings.xml file and the
  command line. `openvr_api.dll` is delay-loaded, and on the field rig the
  device is created 1.20 s before the first VR call (`vr_runtime.cpp:21-23`).

## 3. Runtime design

### 3.1 The `auto` decision

- **Descriptor.** The descriptor gains `profile = auto`; `schema = 1` is
  unchanged.
  - An older DLL parses `auto` as `Invalid`. That keeps proxy chaining and
    turns the fixes off, which is the right failure for a DLL older than its
    descriptor.
  - `vr` and `flat` stay valid as **developer pins** (section 5). An absent
    descriptor stays `LegacyVr` (Q3).
- **Decision function.** A pure, Windows-free header, so a rig drives every
  case, in the style of `vr_ssaa_hold_math.h`. It takes the descriptor's
  value, Settings.xml's text or its absence, and the game's command line. It
  returns the profile and a reason:
  1. A pinned `vr` or `flat` returns as today, and Settings.xml is not read.
  2. `auto` with `/novr` on the command line returns Flat; with `/vr`, it
     returns Vr. This step is held behind H2: it ships only if the flight
     shows the switch on the exe's command line.
  3. `auto` with `<StereoscopicMode>` 0 returns Flat; 1 to 6 returns Vr.
  4. `auto` with the mode unknown returns Q2's answer (proposed: Vr). The mode
     is unknown when the file is absent or unreadable, the tag is absent, or
     the value is out of range.
- **One parser.** `parseStereoscopicMode` moves to `src/common` so the profile
  decision and the Supersampling hold share it. The hold's own re-reads stay
  where they are; they read at other times on purpose.
- **Where it runs.** `runtimeProfileInitialize`, still first in `Config::init`.
  It is not in DllMain, so reading a file and `GetCommandLineW` are allowed.
  Nothing after it changes: the ini choice, `runtimeProfileAllowsKey` and
  every branch site see Flat or Vr exactly as they do today.
- **Log.** The existing `installation profile:` line
  (`d3d11_proxy.cpp:289`) gains the reason. Example:
  `installation profile: flat (auto: 3D mode 0 from <path>; command line has
  no /vr or /novr)`.
  - It also logs the game's command line once, which H2 needs.
  - If the new code never ran, an `auto` descriptor logs `invalid`, so failure
    cannot pass for success.
  - `tools\edvr_log.py --version` reports the line.

### 3.2 A mid-session 3D switch (Q1)

The profile stays fixed for the launch. Making it live would touch every
branch site, install both hook sets, and swap ini files under a running
config. That is a later phase only if flights show players toggle often.

- **Flat launch, then 3D turned on.** This is the unsafe direction, and today
  nothing detects it.
  - The flat pipeline stands down when the live VR signal is seen. One more
    trigger: the `vr_runtime.cpp` census sees EDVR's or a foreign
    `openvr_api.dll`, or `LibOVRRT`, loaded.
  - The pieces that stand down: the flat Present chain, flat temporal, the
    flat UI layer and the F8 panel's frame work. After that the DLL only
    forwards.
  - This is the rule `design-flat-temporal-aa-2026-09-23.md` already stated
    ("a flat-only install meeting VR rendering stands down"), now built. VR
    then runs on EDVR's OpenXR runtime with no d3d11 fixes.
  - Log line and toast: "Elite's 3D mode changed after launch; EDVR's VR fixes
    start at the next launch."
- **Under `auto` only, two VR pieces install whatever the launch decided:**
  - the window request rewrite (`vr_window_trampolines.cpp`), which already
    re-checks "VR live, or saved mode not 0" per call;
  - the Supersampling hold, which already follows the live signal.
  - Why: without them, a flat launch switched to 3D on drops to the 1280x768
    windowed request and saves it, which H6 of the Supersampling doc fixed for
    VR.
  - Their flat-forwarding behaviour (a flat request is forwarded byte for byte)
    must then key on the live signal and the saved mode, not on the profile.
    That needs its own review and is part of Q1.
- **VR launch, then 3D turned off:** the VR fixes starve (H4), and there are no
  flat fixes until the next launch. The same log line and toast appear.

### 3.3 What does not change

- **Settings files.** VR reads `edvr.ini`. Flat reads `edvr-flat.ini`, falling
  back to `edvr.ini` while it is absent. Each profile keeps its own tuning,
  which is why the split was made.
- **`runtimeProfileAllowsKey`, the flat panel rows** (`menu_flat_rows.h`) **and
  `tools\flat_sharpen_test`'s gate on them.**
- **The OpenXR runtime.** No profile logic is added to `src/openxr`, unless H1
  is refuted. In that case the runtime must refuse to start a session while
  the graphics DLL reports Flat; that is a design addition, not a guess to
  build now.

## 4. Installer and packaging

- **One installer.** `edvr-installer.exe`, built once.
  - Retire `EDVR_INSTALLER_FLAT`, `edvr-flat-installer.exe`,
    `rig_flat_installer` (`build.bat:2603-2621`),
    `gen_installer_rc.py --profile flat` and `package_native.py --profile flat`.
  - The release is `edvr-<ver>.zip` and `edvr-installer-<ver>.zip`.
  - The zip carries the `auto` descriptor, `edvr.ini` and `edvr-flat.ini`.
  - The flat README text (`gen_installer_rc.py:343-356`) folds into
    `release/README.txt`.
- **Payload.** Today's VR payload, plus the generated `edvr-flat.ini` as a new
  resource. The descriptor text is
  `[install]\r\nschema = 1\r\nprofile = auto\r\n`.
- **Plan (`planInstall`).** One path, made of today's VR block plus today's
  flat ini block:
  - the graphics DLL, the descriptor, and the runtime, loader, licence and
    `edvr_openxr.ini` (the current `!flat` block);
  - `edvr.ini` merged as VR does today;
  - `edvr-flat.ini` merged, or seeded from `edvr.ini` when absent, as flat does
    today.
  - Components:
    `graphics,profile,ini,ini-flat,openvr,openxr-loader,openxr-license,openxr-config,ngx-optional`.
- **Two merge bases.** `edvr_install\edvr.ini.base` is one file shared by both
  editions today, overwritten with the incoming edition's ini (`plan.h:84-86`,
  `plan.cpp:791-797`). With both files installed every time, each needs its
  own base: `edvr.ini.base` and `edvr-flat.ini.base`. Otherwise one of the two
  merges is always two-way.
- **Upgrades.** `installedProfile()` (`plan.cpp:106-114`) learns the `auto`
  descriptor's hash and the `auto` state value. Every earlier state upgrades
  in place, with the replaced descriptor backed up:
  - From `vr`, or legacy with no descriptor: the descriptor becomes `auto`, and
    `edvr-flat.ini` is seeded if absent.
  - From `flat`: the runtime is installed (the game's `openvr_api.dll` is
    renamed `openvr_api_orig.dll`, as the VR path does), `edvr.ini` is written
    or merged, and the descriptor becomes `auto`.
  - `--convert-profile`, its MessageBox (`gui.cpp:750-757`) and the profile
    mismatch block (`plan.cpp:234-240`) retire.
  - The runtime ownership checks that uninstall uses stay.
- **Settings screen.** Shows both files, with a VR/Flat selector above the
  list. The selector defaults to the profile the next launch will use: the
  installer reads Settings.xml through the shared parser and folder. The flat
  list keeps its three rows (`settings.cpp:186-192`).
- **Status line.** "Elite's 3D mode is off: EDVR will run its flat fixes", or
  "on: VR fixes", or "unknown: <reason>".
- **Mirror.** `holdsSettingsFor` offers a restore when neither ini is present;
  it already copies both.
- **Log bundler.** `editionOf()` learns `auto`. `log.dir` is read from both
  inis, and both folders are swept when they differ.
- **Uninstall.** Unchanged. `--remove-settings` removes both inis.
- **`docs/installer.md`.** Has no Status block today; it gets one when this arc
  changes it.

## 5. Developer tools

- **`install_edvr.py`.**
  - `--profile auto` becomes the default and installs today's VR set with the
    `auto` descriptor.
  - `--profile vr` and `--profile flat` stay as pins for flights that need one
    profile whatever Settings.xml says.
  - The flat path's refusal of VR leftovers (`install_edvr.py:1190-1198`)
    retires, because under `auto` those files are expected.
  - `--ini` writes the file of the profile `--profile` names: `edvr.ini` for
    `auto` and `vr`, `edvr-flat.ini` for `flat`.
  - `--dry-run` still writes nothing, and the self-test asserts it.
- **`package_native.py --check-installer`.** Checks one installer that embeds
  both inis and the `auto` descriptor, and no flat artifacts.
- **`edvr_log.py`.** Parses the profile line and its reason, with a self-test
  fixture.

## 6. Tests and build gates

- **A decision rig** (new `tools\runtime_profile_test`, added to `build.bat`).
  It drives the pure function:
  - pins that ignore Settings.xml;
  - modes 0 to 6, a BOM, CRLF, an absent tag, 7, `-1`, an empty file and an
    absent file;
  - `/vr` and `/novr` in each position and case, behind the H2 switch;
  - the old parser reading `auto` as `Invalid`.
- **`installer_test`.**
  - Cases: a fresh install; upgrades from vr, legacy and flat; the two merge
    bases; uninstall with and without `--remove-settings`.
  - Dry-run writes nothing.
- **A stand-down rig.** A fake live-VR signal turns the flat pipeline into
  forward-only. The rig must also show that a flat session without the signal
  is unchanged.
- **Unchanged gates:** `config_test`'s ini-name resolver (it sees Flat or Vr as
  before) and `tools\flat_sharpen_test`.

## 7. Flights

All three launches run in one session. The environment: Steam, EDVR's OpenXR
runtime via `openvr_api.dll`, Sean's headset and per-eye size, and the DLSS
version in use. First run `edvr_log.py --expect-build HEAD` on every log.

- **Launch A, Settings.xml mode 0.**
  - Expect `installation profile: flat (auto: 3D mode 0 ...)`, the F8 panel,
    and the census verdict "nothing loaded" for the whole flat stretch. That
    verdict is H1 confirmed. If `openvr_api.dll` appears in it, H1 is refuted.
  - Then turn 3D on in the menu. Expect the stand-down line and the toast, VR
    on EDVR's runtime, and the display mode kept (Q1).
- **Launch B, Settings.xml now not 0.**
  - Expect `vr (auto: 3D mode N)`. That is H3, together with launch A.
  - Turn 3D off. Expect the starved line and the toast, no visual change, and
    no crash (H4). Read the frame time against launch A's flat stretch.
- **Launch C, Settings.xml not 0, EDLaunch with `/novr`.**
  - The logged command line shows whether `/novr` reached the exe. Whether VR
    goes live shows whether the game honoured it. Together they settle H2.
  - If H2 is refuted, step 2 of the decision does not ship. Its cost is then
    only a VR profile in a flat session (H4), which is the inert direction.
- **On the supporter's Oculus native-SDK rig,** with 3D on: expect `vr (auto:
  ...)`, with the Oculus route still pushing Elite to OpenVR.
- **On a Proton user's log:** the decision line names the Settings.xml path
  (H5).

## 8. Open decisions for Sean

- **Q1, mid-session switch.**
  - (a) Proposed: the profile is fixed per launch, flat stands down on live
    VR, and under `auto` the window rewrite and the Supersampling hold install
    whatever the launch decided. A toast says to restart.
  - (b) Live profile switching: large, and a later phase.
  - (c) Fixed per launch with no stand-down. Not proposed, because flat would
    touch stereo frames.
- **Q2, unknown 3D mode (Settings.xml absent or unreadable).**
  - Proposed: VR. VR in a flat game starves, while flat in a VR game is the
    unsafe direction.
  - The alternative is flat. It matches Elite's 3D-off default on a first
    launch, but a VR player with an unreadable file then gets stand-down every
    launch.
- **Q3, manual installs without a descriptor.**
  - Proposed: keep `LegacyVr`, and have `manual-install.md` ship the `auto`
    descriptor from now on.
  - The alternative treats an absent descriptor as `auto`. That changes what an
    existing manual install does on its next launch.
- **Q4, no user override.**
  - Proposed: no new key. The descriptor is install scope, not a setting, and
    the pins stay developer-only.
  - If players need to force a profile, that is a key a user would set and
    needs its own case.
- **Q5, retiring the flat release.**
  - Proposed: the release that ships this has no flat archives. The unified
    installer upgrades a flat install in place, and its runtime sits inert in
    `Openvr\win64` while 3D is off (H1).
  - `README.md`'s download section (around lines 59-74) collapses to one.
  - The release notes say flat players now also get the VR runtime file.
- **Q6, non-headset 3D modes.** If any of modes 1 to 6 is a monitor stereo mode,
  the rule "VR otherwise" gives it the VR profile, which starves there.
  - Proposed: keep the rule as stated, and record each mode as flights name it.

## 9. Ruled out while designing

- ruled out: deciding in DllMain, because file I/O is not allowed there
  (`oculus_route.h:22-25`, `d3d11_proxy.cpp:144-145`), and nothing VR exists
  before the device.
- ruled out: the live OpenXR signal as the launch decision, because it arrives
  after the hooks and the ini choice. It is kept as the stand-down trigger.
- ruled out: reading the live in-memory 3D mode, because it sits behind virtual
  calls on heap objects (`vr-supersampling-gate-2026-10-10.md`, ruled out
  there).
- ruled out: "no VR frames means flat", because a missing frame stream proves
  nothing (`design-flat-temporal-aa-2026-09-23.md`).
- ruled out: one merged settings file, because it renames keys or changes their
  meaning by profile, which needs Scope control's quote-and-ask for every key,
  and it loses the per-profile tuning the split exists for.
- ruled out: keeping two installers with a shared auto descriptor, because the
  goal is one download, and a flat install has no runtime to run VR on.

## 10. Order of work

1. **Runtime decision.** The pure function, its rig, the log line and the
   command line, `install_edvr.py --profile auto`, and `edvr_log.py`. Then
   launches A to C.
2. **Stand-down and the Q1 hooks.** The flat stand-down on live VR, its rig,
   and the `auto` install of the window rewrite and the Supersampling hold.
   Then fly launch A's toggle again.
3. **Installer and packaging.** One payload, two merge bases, upgrades from
   every state, the settings screen selector, and the bundler. Update
   `README.md`, `installer.md`, `manual-install.md`, `release/README.txt`, and
   AGENTS.md's bullet on `edvr-flat.ini`.
4. **Release.** Notes via `reflow_notes.py`, and credits via
   `release_credits.py`.
