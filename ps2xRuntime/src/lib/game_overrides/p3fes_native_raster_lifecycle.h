#pragma once

#include <cstdint>

class PS2Runtime;

namespace p3fes_native_rasters
{
    // Guest-raster identity plus a monotonically increasing content version.
    // The Vita renderer can use (raster, generation, revision) as its cache key
    // without hashing guest RAM during draw calls.
    struct Version
    {
        uint64_t generation = 0u;
        uint64_t revision = 0u;
    };

    using RetireCallback = void (*)(uint32_t raster, uint64_t generation);

    // Clears tracked guest resources. The generation counter and installed
    // wrappers remain valid across a runtime reset so stale host handles cannot
    // become current again if the guest allocator reuses an address.
    void reset();

    // Installs narrow wrappers around P3FES RenderWare raster lifecycle calls.
    // Callbacks are notifications only; they must enqueue host/GPU work rather
    // than issue GL calls from the guest execution thread.
    bool install(PS2Runtime &runtime, RetireCallback onRetire);

    // Bumped whenever an existing raster's version changes or a raster is retired (invalidate, retire,
    // reset). A (raster -> version) answer cached at some epoch is still valid while epoch() is unchanged.
    uint64_t epoch();

    // Returns the current version, lazily registering rasters that existed
    // before lifecycle hooks were installed. A zero raster returns {0, 0}.
    Version version(uint32_t raster);

    // Call before an image write, palette write, or unlock routine. This is
    // intentionally safe to call more than once when guest wrappers nest.
    void invalidate(uint32_t raster);

    // Retires the current generation and notifies the host resource owner once.
    // Call before the guest destroy routine, since it may free the raster RAM.
    void retire(uint32_t raster);

    // Emits aggregate lifecycle-hook counters to stderr for boot diagnostics.
    void logCounters();
}
