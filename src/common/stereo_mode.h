// The game's 3D mode, read from Settings.xml's text. Pure and free of any Windows API, so the launch decision (install_scope.h), the
// Supersampling hold (src\d3d11\vr_ssaa_hold_math.h) and the rigs that drive them share one parser.
#pragma once

#include <cstring>

namespace edvr {

// The value of <StereoscopicMode> in Settings.xml's text. True with *mode = N when the tag holds a whole number from 0 to 6 (the
// game clamps it to that range), with whitespace allowed around it; false when the tag is absent or holds anything else.
inline bool parseStereoscopicMode(const char* xml, int* mode) {
    static const char kTag[] = "<StereoscopicMode>";
    const char* at = xml ? std::strstr(xml, kTag) : nullptr;
    if (!at) return false;
    const char* p = at + (sizeof(kTag) - 1);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p < '0' || *p > '9') return false;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 6) return false;
        ++p;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
    if (*p != '<') return false;
    *mode = v;
    return true;
}

}  // namespace edvr
