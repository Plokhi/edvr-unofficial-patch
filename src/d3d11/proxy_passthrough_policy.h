#pragma once

#include <cstddef>

namespace edvr {

// Latched once in PROCESS_ATTACH, before any EDVR hook. Only the exact
// EDVR_PASSTHROUGH=1 value opts in; missing, truncated and other values retain
// normal behavior. No config parsing, allocation or logging under loader lock.
constexpr bool proxyPassthroughRequested(const wchar_t* value,
                                        std::size_t copiedLength) noexcept {
    return value && copiedLength == 1 && value[0] == L'1';
}

struct ProxyPassthroughPolicy {
    bool passthrough = false;

    constexpr bool edvrEnabled() const noexcept { return !passthrough; }

    // A configured DLL may invoke a creation export from its own DllMain.
    // Its same-thread callback must reach the system DLL without entering the
    // INIT_ONCE that is already loading it.
    constexpr bool initialiseCreation(bool reentrant) const noexcept {
        return !reentrant;
    }

    constexpr bool attachDeviceAllowed(bool reentrant) const noexcept {
        return edvrEnabled() && !reentrant;
    }
};

}  // namespace edvr
