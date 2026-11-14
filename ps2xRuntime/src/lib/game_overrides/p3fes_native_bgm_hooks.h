#pragma once

#include <cstdint>

class PS2Runtime;

namespace p3fes_native_bgm_hooks
{
    // Install the Vita BGM bridge. Unsupported or unavailable native tracks
    // continue through the original guest functions.
    bool install(PS2Runtime &runtime);

    struct Counters
    {
        uint64_t nativeStarts = 0u;
        uint64_t guestStartFallbacks = 0u;
        uint64_t nativeStops = 0u;
        uint64_t nativeFades = 0u;
        uint64_t nativeVolumeUpdates = 0u;
        uint64_t nativePauseUpdates = 0u;
    };

    Counters counters();
    void resetCounters();
    void logCounters();
}
