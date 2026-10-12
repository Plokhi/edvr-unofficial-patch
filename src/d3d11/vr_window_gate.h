// Whether the game's mode request is rewritten (vr_window_trampolines.cpp's requestReplacement). Pure and free of any Windows API, so
// tools\runtime_profile_test checks every combination here.
#pragma once

#include "../common/install_scope.h"

namespace edvr {

// The rewrite runs in the VR profile. It also runs in Passthrough when the launch was VR: the phase-1 stand-down drops to Passthrough
// at 3D off, but the game keeps toggling 3D, and this rewrite is what keeps its display mode across the toggle (flight D, 2026-10-12:
// the 3D-on windowed request was forwarded unrewritten after the drop). A launch that was flat never rewrites, in any profile.
inline bool vrWindowRewriteAllowed(RuntimeProfile current, RuntimeProfile launch) {
    return current == RuntimeProfile::Vr || current == RuntimeProfile::LegacyVr ||
           (current == RuntimeProfile::Passthrough && (launch == RuntimeProfile::Vr || launch == RuntimeProfile::LegacyVr));
}

}  // namespace edvr
