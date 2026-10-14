#pragma once
#include <cstdint>

namespace edvr {
// Profiling is opt-in. The old explicit flat-clock switch remains an alias;
// an explicit new switch takes precedence. Read once, outside the draw path.
inline bool cpuProfilePolicy(const char* profile, const char* flatClocks) noexcept {
    const char* value = profile ? profile : flatClocks;
    return value && value[0] == '1' && value[1] == '\0';
}
// Keep sparse, randomized frame sampling even when a malformed value is supplied.
// The report window stays independent of this interval (5 seconds in flat).
inline unsigned cpuSamplePeriodPolicy(const char* value) noexcept {
    if (!value || !*value) return 32;
    unsigned n = 0;
    for (; *value; ++value) {
        if (*value < '0' || *value > '9' || n > 1024) return 32;
        n = n * 10 + static_cast<unsigned>(*value - '0');
    }
    return n >= 32 && n <= 1024 && (n & (n - 1)) == 0 ? n : 32;
}
inline bool cpuGateHasThread(uint64_t gate) noexcept {
    const auto tid = static_cast<uint32_t>(gate);
    return tid != 0 && tid != UINT32_MAX;
}
// Preserve exactly one forward and its result. Capture clocks every call;
// ordinary calls use the scope's sampling decision, including a zero-clock off path.
template<class Scope, class Clock, class Call>
auto cpuTimedForward(Scope& scope, bool capture, Clock now, Call call, int64_t& elapsed)
    -> decltype(call()) {
    const int64_t start = capture ? now() : scope.pause();
    if (capture) scope.pauseAt(start);
    const auto result = call();
    const int64_t stop = capture ? now() : scope.resume();
    if (capture) scope.resumeAt(stop);
    elapsed = stop - start;
    return result;
}
} // namespace edvr
