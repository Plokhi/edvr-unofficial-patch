# XeSS for flat EDVR on Intel B390

## Status

- State: open. The user wants the latest XeSS in EDVR, tested only in flat mode
  on this device. Current investigation targets XeSS-SR first.
- Environment: ONEXPLAYER 3, Intel Arc B390 PTL 8086:b080, CachyOS xe, Proton
  Experimental experimental-11.0-20261001-x86_64, DXVK
  v3.1.1-47-g685301564ea3486, private Mesa 26.3.0-devel git-37bf953a58. Normal
  Elite startup now reaches the menu without EDVR or a debugger; see
  planetary-startup-intel-b390-2026-10-11.md.
- Current EDVR has no XeSS backend, SDK pin or payload. fix.temporal_aa accepts
  off/on/dlss/fsr. No config values or source have changed here.
- Latest official package: Intel XeSS SDK v3.0.2, peeled commit
  8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0. Its D3D11 DLL reports XeSS-SR
  runtime 2.0.2; package and component versions differ.
- Ruled out: a straightforward native D3D11 XeSS integration on this
  Proton/Mesa stack. D3D11CreateDevice succeeds, the SDK loads, but
  xessD3D11CreateContext returns -2, XESS_RESULT_ERROR_UNSUPPORTED_DRIVER, and
  a null context. Init/Execute were correctly not called.
- Evidence: disposable-prefix probe and DXVK logs under
  /tmp/edvr-xess-probe-20261011. DXVK identifies B390 and driver 26.2.99, the
  private development build; system Mesa reports 26.2.4.
- Open hypothesis: the separate XeSS Vulkan API may work through DXVK's Vulkan
  interop interfaces. Confirm with context creation/init on the actual DXVK
  device and its enabled features, then valid resource execution. Physical GPU
  support or DLL loading alone is insufficient evidence.
- Build gate: this Linux checkout lacks the Windows MSVC/SDK environment for
  the required full build.bat validation. Proton's Wine/cmd are available at
  absolute paths. Resolve a complete build path before reporting C++ done.
- Temporary settings: none in EDVR or the live game. Capability tests use
  disposable prefixes and files under /tmp. No EDVR build has been installed.

## October 11: D3D11 capability probe

The latest official SDK was fetched by tag into /tmp. libxess_dx11.dll has
SHA256 c7cfe86f0c9d94e4fb3696d3cd5035e2bbb6a8b1b0572f8b7395a4cdfd0c625e. An
official, SHA-verified Windows Python 3.14.7 embeddable package ran a ctypes
probe in a fresh disposable Proton prefix through Steam Runtime 4. The private
ICD was selected before the container started. No game files, live prefix, EDVR
config or rendering source were changed.

D3D11CreateDevice returned S_OK with feature level 11_0. xessGetVersion
returned success and version 2.0.2. xessD3D11CreateContext then returned
XESS_RESULT_ERROR_UNSUPPORTED_DRIVER (-2), not unsupported device (-1). This is
an SDK driver-path refusal after successful D3D11 device creation. It does not
establish that every XeSS API path is unusable on this hardware.

Results: /tmp/edvr-xess-probe-20261011/pywin/probe-result.jsonl. Driver
evidence: /tmp/edvr-xess-probe-20261011/logs/python_dxgi.log and
python_d3d11.log. The Windows process does not retain the Linux ICD variables;
DXVK's driver identity establishes the effective selection.

DXVK v3.1.1 exposes IDXGIVkInteropDevice, IDXGIVkInteropSurface and
IDXGIVkInteropFactory. They provide device/queue/image handles and explicit
flushing, layout transitions and queue locking. A Vulkan integration must
respect those ownership and synchronization rules and XeSS's required enabled
features. No Vulkan XeSS context or execution has yet been tested.

## Sources

- [Intel XeSS SDK v3.0.2](https://github.com/intel/xess/releases/tag/v3.0.2).
- [XeSS-SR
  guide](https://github.com/intel/xess/blob/v3.0.2/doc/xess_sr_developer_guide_english.md)
  documents Intel-only D3D11 support and separate Vulkan requirements.
- [D3D11 API](https://github.com/intel/xess/blob/v3.0.2/inc/xess/xess_d3d11.h)
  and [result
  codes](https://github.com/intel/xess/blob/v3.0.2/inc/xess/xess.h).
- [DXVK v3.1.1 interop
  interfaces](https://github.com/doitsujin/dxvk/blob/v3.1.1/src/dxgi/dxgi_interfaces.h).
