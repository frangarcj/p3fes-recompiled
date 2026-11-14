#pragma once

#include <cstdint>

class PS2Runtime;

namespace p3fes_native_math
{
    struct Counters
    {
        uint64_t nativeAtan = 0u;
        uint64_t nativeSin = 0u;
        uint64_t nativeCos = 0u;
        uint64_t guestFallbacks = 0u;
        uint64_t installFailures = 0u;
    };

    // Replace the selected P3FES libm functions with host libm for ordinary
    // finite inputs. Special values and large magnitudes keep the guest path.
    // Call with enabled=false to leave the guest table unchanged.
    bool install(PS2Runtime &runtime, bool enabled = true);

    Counters counters();
    void resetCounters();
}
