#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;

namespace p3fes_native_texture_access
{
    // Returns true only when the raster is backed by CPU-visible guest pixels
    // and its native host texture is ready for use.
    using PrepareRaster = bool (*)(const uint8_t *rdram, uint32_t raster, std::string &textureName);

    struct Counters
    {
        uint64_t speculateNative = 0u;
        uint64_t speculateFallback = 0u;
        uint64_t accessNative = 0u;
        uint64_t accessFallback = 0u;
        uint64_t releaseNative = 0u;
        uint64_t releaseFallback = 0u;
        uint64_t lockNative = 0u;
        uint64_t lockFallback = 0u;
        uint64_t restoreNative = 0u;
        uint64_t restoreFallback = 0u;
        uint64_t invalidRaster = 0u;
        uint64_t prepareFailed = 0u;
    };

    // Install narrow replacements for PS2 texture-cache routines.
    // Unsupported/GS-resident rasters continue through the original routines.
    bool install(PS2Runtime &runtime, PrepareRaster prepareRaster);

    // The native branch of the RpSkyTexCacheAccessSpeculate + RpSkyTexCacheAccessRaster pair for a caller that is not
    // guest code (the render-state shortcut of p3fes_native_frame_cuts.cpp): prepares the host texture, marks the raster
    // as native and binds it (RenderWare's and the Sky cache's current raster and access flags), exactly what the two
    // hooks do for a raster they handle. False: the guest functions must run (nothing has been bound).
    bool accessRasterNative(uint8_t *rdram, uint32_t raster, uint32_t accessFlags);

    // Drop the no-GS-cache marker before the guest frees or reuses this raster.
    void forgetRaster(uint32_t raster);

    // Cheap membership query for the native RenderWare draw path; no guest RAM
    // is inspected and no texture preparation is attempted.
    bool isNativeRaster(uint32_t raster);

    Counters counters();
    void resetCounters();
    void logCounters();
}
