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
- Open hypotheses: a Vulkan compute compilation stall; a problem common to the
  game and Wine startup; a compiler issue common to both Mesa renderers.
- Next startup: the user proposed Proton Experimental. Test it with Vulkan,
  Elite-only logging and native stacks at 15 and 45 seconds. Native stack
  access succeeded on the idle launcher. Logs go into proton-experimental;
  compare the loaded DXVK version and generation outcome with Proton 11.
- Temporary settings: Elite-only Proton user_settings.py enables PROTON_LOG,
  PROTON_LOG_DIR, DXVK_LOG_LEVEL and DXVK_LOG_PATH. PROTON_USE_WINED3D was
  removed after the failed comparison. Remove the temporary settings file after
  the investigation. No EDVR INI changes.
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

## Sources

- [Mesa 25.2.4 release notes](https://docs.mesa3d.org/relnotes/25.2.4.html)
  list an Intel Elite crash at zero percent generation. The current driver is
  newer; the release note alone does not identify this crash's mechanism.
- [Proton runtime
  options](https://github.com/ValveSoftware/Proton#runtime-config-options)
  document PROTON_USE_WINED3D as a reversible OpenGL renderer comparison.
