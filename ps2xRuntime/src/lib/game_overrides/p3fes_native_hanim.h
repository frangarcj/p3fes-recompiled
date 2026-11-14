#pragma once

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace p3fes_native_hanim
{
    struct Counters
    {
        uint64_t native = 0u;
        uint64_t guestFallbacks = 0u;
        uint64_t installFailures = 0u;
    };

    // HAnim interpolation port. The input and output records are guest RDRAM
    // structures; false means the caller must run the original guest routine.
    bool interpolate(uint8_t *rdram, R5900Context *ctx);

    // Install the opt-in wrapper around RpHAnimCompressedKeyFrameInterpolate.
    bool install(PS2Runtime &runtime, bool enabled = true);
    Counters counters();
    void reset();
    void resetCounters();
    void logCounters();
}
