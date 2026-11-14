#pragma once

#include <cstdint>

class PS2Runtime;

namespace p3fes_native_camera
{
    // Install this only with native RenderWare enabled. It replaces the PS2
    // packet-building camera path for raster types 0..4 and keeps
    // type 5 on the original callback. The camera D8 plugin data is not
    // rebuilt: its discovered consumers are the PS2 morph/world callbacks at
    // 0x46EB90, 0x4718A0, 0x47ABF0, 0x47B280, 0x47B750, 0x478CC0, 0x4A09D0,
    // and 0x4A3F70. Native atomic/sector rendering uses the guest camera/frame
    // directly and must not dispatch those callbacks as a fallback.
    bool install(PS2Runtime &runtime);

    // Applies the RAM bookkeeping for the native display-camera path. Exposed for
    // unit tests; returns false when the raster is not a supported camera type.
    bool applyNativeBeginState(uint8_t *rdram, uint32_t gp, uint32_t camera,
                               uint32_t raster);

    // Clears the pending camera-ready flag for the non-type-5 end path. The
    // PS2 GIF packet cursor is never advanced by this helper.
    bool applyNativeEndState(uint8_t *rdram, uint32_t gp, uint32_t camera,
                             uint32_t raster);

    // True between a type-5 (texture camera) BeginUpdate and its EndUpdate.
    // Native recording hooks use it to leave the GS-resident shadow-map pass on
    // the original guest path instead of flushing it as a screen frame.
    bool inTextureCamera();

    // Logs cumulative begin/end path totals, raster-type buckets and the first
    // native/fallback camera+raster observed. Call from the host frame thread.
    void logCounters();
}
