// The plausibility guard for the game's DisplayConfig object before a save hold writes into it (docs/vr-supersampling-gate-2026-10-10.md,
// "The save corruption"). Pure and header-only: tools\vr_ssaa_hold_test drives it, and any hold that writes the object must pass it first.
//
// WHY. The 55c0fe3d hold wrote 3840, 2160 and 2 into object offsets it had named ScreenWidth, ScreenHeight and FullScreen. The offsets
// were wrong, and the saved DisplaySettings.xml took VSync true, FullScreen 3840 and PresentInterval 2. The guard refuses to write when
// the object's current values are not those of a DisplayConfig, so a wrong layout writes nothing.
#pragma once

#include <cstdint>

namespace edvr {
namespace displaycfg {

constexpr uint32_t kMinSide = 640;    // the smallest window side EDVR will hold (a DisplayConfig's ScreenWidth or ScreenHeight)
constexpr uint32_t kMaxSide = 16384;  // D3D11's texture limit per axis

inline bool plausibleSide(uint32_t v) { return v >= kMinSide && v <= kMaxSide; }
// FullScreen: 0 windowed, 1 full, 2 borderless (inferred from the saved XML and the apply's kinds).
inline bool plausibleFullScreen(uint32_t v) { return v <= 2; }
// VSync is a boolean in the saved XML.
inline bool plausibleVSync(uint32_t v) { return v <= 1; }
// PresentInterval is the saved XML's count of vertical blanks (0..4 in the game's options).
inline bool plausiblePresentInterval(uint32_t v) { return v <= 4; }

// The whole object's fields as read: true only when every one of them is plausible for its name.
inline bool plausibleObject(uint32_t width, uint32_t height, uint32_t fullScreen, uint32_t vsync, uint32_t presentInterval) {
    return plausibleSide(width) && plausibleSide(height) && plausibleFullScreen(fullScreen) && plausibleVSync(vsync) &&
           plausiblePresentInterval(presentInterval);
}

}  // namespace displaycfg
}  // namespace edvr
