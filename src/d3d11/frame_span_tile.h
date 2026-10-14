#pragma once
// The overlay's GPU TIME tile (perf_monitor.cpp, 2026-10-14): which number it shows. Pure, so tools/aa_off_test runs the
// decision the overlay makes. The application-render mean (advanced.app_gpu_timing on) wins when it has samples; otherwise
// the frame GPU span from gpuCensusFrame (the census's own frame scope, on with the application clock off) when one has
// completed; otherwise "--" with the reason.
#include <cstddef>
#include <cstdio>

namespace edvr {

// The frame span's timer slots (gpu_census.cpp): a pair is held until it is read back four frames later, so five are
// in flight at most; six leaves one spare. Two timestamps a pair.
constexpr unsigned kFrameSpanSlots = 6;

inline bool gpuTimeTileText(bool appHave, double appMeanMs, bool spanHave, double spanMs, const char* noneReason,
                            char* value, size_t valueLen, char* sub, size_t subLen) {
    if (!value || !sub || !valueLen || !subLen) return false;
    if (appHave) {
        std::snprintf(value, valueLen, "%.1f", appMeanMs);
        std::snprintf(sub, subLen, "ms application render; elapsed");
        return true;
    }
    if (spanHave) {
        std::snprintf(value, valueLen, "%.1f", spanMs);
        std::snprintf(sub, subLen, "ms frame GPU work span (first command to last; includes GPU idle inside the frame)");
        return true;
    }
    std::snprintf(value, valueLen, "--");
    std::snprintf(sub, subLen, "%s", noneReason ? noneReason : "");
    return false;
}

// The work span's arithmetic (gpu_census.cpp's pair): the GPU time from the first command's timestamp to the door end's.
// Clamped at zero: a pair the GPU reports out of order is not a negative frame.
inline double frameWorkSpanMs(double firstCommandMs, double doorEndMs) {
    const double span = doorEndMs - firstCommandMs;
    return span > 0.0 ? span : 0.0;
}

}  // namespace edvr
