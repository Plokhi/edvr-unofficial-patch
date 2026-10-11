# Intel B390 planetary generation startup crash

## Status

- State: open. Elite crashes at zero percent startup planetary generation
  without EDVR; the user confirmed the percentage on October 11.
- Device: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, xe driver, CachyOS,
  kernel 7.2.9-1-cachyos-deckify, Mesa 26.2.4-arch3.1.
- Game: Steam 359320, installed build 25433925, Proton 11.0-100, DXVK
  v2.7.1-498-ga6764047e587178, flat 1280x720. No headset or VR runtime
  involved; EDVR and DLSS are not loaded. No internal table limit identified.
- Ruled out: an older-than-25.2.4 Mesa installation as the sole explanation,
  because the fresh October 11 game log reports Intel Mesa 26.2.4 and crashes.
- Ruled out: EDVR is required to reproduce, because the game has no local
  d3d11.dll or EDVR logs and both startup dumps contain Wine's graphics DLLs.
- Ruled out: OpenGL as a startup workaround, because WineD3D rejects a required
  CreateTexture3D with E_INVALIDARG and Elite deliberately crashes before the
  generation stall. This does not test the Vulkan hypothesis.
- Ruled out: Proton Experimental alone fixes startup, because
  experimental-11.0-20261001-x86_64 with DXVK v3.1.1-47-g685301564ea3486 still
  crashes at zero percent after discarding the old DXVK cache.
- Evidence: one shader worker uses a full CPU core inside libvulkan_intel.so
  for over 73 seconds, while the other workers wait. The watchdog captures
  vkCreateComputePipelines. This supports prolonged CPU-side Intel pipeline
  compilation or processing, rather than a GPU or mutex wait at that moment.
- Open hypotheses: expensive Intel shader register allocation exceeding the
  game's watchdog; a compiler defect producing excessive allocation work.
  Binary/source matching identifies setup_live_interference's overlap scan.
- Next startup: prepared; awaiting the user's Play and outcome. Keep Proton
  Experimental and DXVK fixed; compare a private Intel Vulkan build from Mesa
  main, pinned at 37bf953a58d8baa6c29142c714f46418d4d596a4. Verify the selected
  driver before requesting Play, then capture native CPU timing and stacks
  during startup. No system driver replacement. No specific upstream fix
  verified yet.
- Temporary settings: Elite-only Proton user_settings.py enables PROTON_LOG,
  PROTON_LOG_DIR, DXVK_LOG_LEVEL and DXVK_LOG_PATH. Experimental also selects
  the private Intel ICD through VK_DRIVER_FILES.
  /tmp/prepare_elite_mesa_main.py --restore [--dry-run] restores its
  logging-only settings. Remove both owned temporary settings files after the
  investigation. No EDVR INI changes.
- Evidence directory: /home/deck/.cache/elite-startup-diagnostics/2026-10-11.

## October 11 evidence

The user reproduced the startup crash at approximately 06:17 MDT. The game
starts at 06:15:38; Frontier writes its dump about 84 seconds later. DXVK
initializes successfully on B390, reports shaderFloat64 support and creates the
swapchain. Its log contains no device loss or fatal shader error. The kernel
has display pipe DSB errors, but no GPU reset or out-of-memory event identified
at the crash. These absences do not prove a correct GPU path.

Frontier.CrashReport.11.10.2026-06.17.01.dmp has no standard exception stream.
The watchdog names ThreadID 896 (0x380), but its saved context is in
win32u.dll+0x150b4. The return into Elite+0x553d16 follows a normal
MsgWaitForMultipleObjects call; this is not a verified faulting instruction.
Custom stream 0xfff0 is a native module list, including libvulkan_intel.so,
rather than fault details.

One dxvk-shader-h worker is inside winevulkan.dll+0x100ef at
vkCreateComputePipelines, with DXVK return offsets +0x17cecc, +0x144fa2 and
+0x178fa7. The October 5 dump repeats those offsets and the main-thread wait.
This supports investigating a compilation stall, but does not identify an
offending shader or a Mesa fault. A native worker stack during the stall would
distinguish compiler work from a Vulkan or synchronization wait.

## OpenGL comparison

The OpenGL run fails about six seconds after launch, before the original stall.
Its game-authored dump contains an exception stream: thread 0x328, c0000005,
null write, game+0x34c9322. Disassembly shows an intentional null write trap.
The F3D crash log records CreateTexture3D returning 0x80070057 (E_INVALIDARG),
from f3dResource_DX11.cpp(456), with device remove reason S_OK. WineD3D's
depth-slice warning precedes the failure, but its causal role is unproven. The
ID3D12 warning prefix is generic; the call is DX11.

This comparison does not distinguish the original causes. Its logs and
restoration copy are preserved in opengl-comparison. Vulkan was restored before
another test was requested.

## Proton Experimental comparison

The user again reports zero percent. Logs verify
experimental-11.0-20261001-x86_64 and DXVK v3.1.1-47-g685301564ea3486 on the
same Mesa 26.2.4 driver. DXVK discarded the older v2.7.1 cache automatically;
the stall persists with the new version and cache. The latest watchdog dump is
Frontier.CrashReport.11.10.2026-06.41.25.dmp.

Native observation of game PID 25239 shows thread 25301, dxvk-shader-h,
continuously runnable from 35 to 108 seconds. With 100 clock ticks per second,
its CPU ticks increase from 709 to 8014 over that interval, approximately one
full core. The 45-second stack places its PC at 0x7f0acaf7b50f, Intel ICD ELF
offset 0x97b50f. Earlier compiler workers sampled within 0x1f of that location.
The other shader workers are waiting by 45 seconds. The watchdog's saved
context places one worker in winevulkan.dll+0x1102f, vkCreateComputePipelines
after the Wine Unix dispatch call, repeating the stable Proton API location.

The loaded /usr/lib/libvulkan_intel.so has build ID
63f230b320e0d6489fc30f11cc74223efc3534c8. It is stripped, so nearest-exported
symbol names do not identify the internal function. The captures support active
CPU-side Intel pipeline work; they do not establish an infinite loop, the
offending shader, or a specific compiler pass. Native stacks, maps and timing
records are preserved under proton-experimental in the evidence directory.

## Intel compiler identification and proposed comparison

Matching debug symbols are unavailable for this CachyOS build ID from Arch,
CachyOS and elfutils debug services. The driver's disassembly at
0x97b490-0x97b53d structurally matches Mesa 26.2.4's
brw_reg_alloc::setup_live_interference(unsigned, brw_range). Both hot PCs
0x97b50f and 0x97b52e are within its virtual-register overlap scan. Loop
bounds, live-range tests and the interference-bit helper match the source. This
is binary/source identification, not a debug-symbol-verified stack trace.

build_interference_graph calls the scan for each virtual register; the scan
checks prior registers. The phase therefore performs quadratic candidate
overlap checks. This explains a plausible source of expensive compilation, but
does not establish a malformed shader, an infinite loop or the exact cause of
excessive work.

The user suggested Mesa 26.3. On October 11, the official Git remote reports
main at 37bf953a58d8baa6c29142c714f46418d4d596a4 and no 26.3 branch; the first
26.3 release candidate is scheduled for October 14. Prepare an isolated Intel
Vulkan build from that main commit and verify its version and driver path
before another startup. Keep the system graphics libraries intact. SPIR-V
capture is available if needed later, but disabling Mesa's cache for capture
would perturb timing during the driver comparison.

The private build and prefix install passed. Native vulkaninfo and a second
check inside SteamLinuxRuntime_4 both enumerate only B390 and report Mesa
26.3.0-devel (git-37bf953a58). The container check set VK_DRIVER_FILES after
container startup, matching Proton user_settings.py's timing. The build has X11
WSI; the baseline game loads winex11.drv. No system libraries or packages were
replaced. Source, private build tools and prefix are under
/tmp/edvr-mesa-main-20261011. The ICD is
install/share/vulkan/icd.d/intel_icd.x86_64.json, and its library is
install/lib/libvulkan_intel.so. Build logs are in that root directory.

The private driver is selected only for SteamAppId 359320 in Experimental's
owned temporary settings. Logs and the logging-only restoration copy go to
mesa-main-37bf953a in the evidence directory. The launcher was reopened and the
native observer armed; the user was asked to press Play. Confirm the game log's
Mesa version and native mapped library before interpreting this startup.
Upstream main changes register-selection and spill logic, but the hot overlap
scan's body remains unchanged; a fix is not established by building
successfully.

## Sources

- [Mesa 25.2.4 release notes](https://docs.mesa3d.org/relnotes/25.2.4.html)
  list an Intel Elite crash at zero percent generation. The current driver is
  newer; the release note alone does not identify this crash's mechanism.
- [Proton runtime
  options](https://github.com/ValveSoftware/Proton#runtime-config-options)
  document PROTON_USE_WINED3D as a reversible OpenGL renderer comparison.
- [Mesa Intel allocator
  source](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.2.4/src/intel/compiler/brw/brw_reg_allocate.cpp#L389)
  contains the identified overlap scan.
- [Mesa release calendar](https://docs.mesa3d.org/release-calendar.html)
  schedules the 26.3 release candidates.
- [Mesa local-build guide](https://docs.mesa3d.org/install.html) documents a
  private prefix and Vulkan ICD selection.
