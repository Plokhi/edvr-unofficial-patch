#pragma once

#include "../../src/d3d11/proxy_passthrough_policy.h"

namespace proxy_passthrough_tests {

template <typename Check>
void run(Check check) {
    using edvr::proxyPassthroughRequested;
    check(proxyPassthroughRequested(L"1", 1), "passthrough: exact environment opt-in");
    check(!proxyPassthroughRequested(nullptr, 1), "passthrough: absent value is normal");
    check(!proxyPassthroughRequested(L"", 0), "passthrough: empty value is normal");
    check(!proxyPassthroughRequested(L"0", 1), "passthrough: zero is normal");
    check(!proxyPassthroughRequested(L"true", 4), "passthrough: words do not opt in");
    check(!proxyPassthroughRequested(L" 1", 2), "passthrough: leading space does not opt in");
    check(!proxyPassthroughRequested(L"1 ", 2), "passthrough: trailing space does not opt in");
    check(!proxyPassthroughRequested(L"11", 3),
          "passthrough: GetEnvironmentVariable required-size result is rejected");

    // The two-character Windows buffer can be left with an initial '1' after
    // a too-small result. Length, rather than the buffer's first byte, settles
    // whether that truncated value is allowed.
    const wchar_t truncated[] = {L'1', L'\0'};
    check(!proxyPassthroughRequested(truncated, 2), "passthrough: truncated prefix rejected");

    edvr::ProxyPassthroughPolicy normal{};
    edvr::ProxyPassthroughPolicy bypass{};
    bypass.passthrough = true;
    check(normal.edvrEnabled(), "passthrough: default enables EDVR startup and teardown");
    check(!bypass.edvrEnabled(), "passthrough: opt-in disables EDVR startup and teardown");
    check(normal.initialiseCreation(false) && bypass.initialiseCreation(false),
          "passthrough: outer creation initializes the configured DLL chain in both modes");
    check(!normal.initialiseCreation(true) && !bypass.initialiseCreation(true),
          "passthrough: chain callback never re-enters active INIT_ONCE");
    check(normal.attachDeviceAllowed(false), "passthrough: normal outer creation may attach");
    check(!normal.attachDeviceAllowed(true), "passthrough: callback never attaches twice");
    check(!bypass.attachDeviceAllowed(false), "passthrough: outer creation never publishes or hooks");
    check(!bypass.attachDeviceAllowed(true), "passthrough: callback never publishes or hooks");
}

}  // namespace proxy_passthrough_tests
