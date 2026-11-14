#include "../src/lib/game_overrides/p3fes_native_texture_access_internal.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

namespace
{
    void put32(std::array<uint8_t, 128> &ram, uint32_t address, uint32_t value)
    {
        std::memcpy(ram.data() + address, &value, sizeof(value));
    }
}

int main()
{
    using namespace p3fes_native_texture_access::detail;
    constexpr uint32_t kRaster = 16u;
    std::array<uint8_t, 128> ram{};
    put32(ram, kRaster + 4u, 96u); // pixels
    put32(ram, kRaster + 12u, 32u);
    put32(ram, kRaster + 16u, 16u);
    put32(ram, kRaster + 20u, 32u);
    put32(ram, kRaster + 24u, 128u);
    put32(ram, kRaster + 32u, 4u);
    assert(nativeTextureDescriptor(ram.data(), ram.size(), kRaster));

    // Camera/render-target type 5 and null-pixel rasters always fall back.
    put32(ram, kRaster + 32u, 5u);
    assert(!nativeTextureDescriptor(ram.data(), ram.size(), kRaster));
    put32(ram, kRaster + 32u, 4u);
    put32(ram, kRaster + 4u, 0u);
    assert(!nativeTextureDescriptor(ram.data(), ram.size(), kRaster));

    // Malformed dimensions/depth and out-of-range raster descriptors fail closed.
    put32(ram, kRaster + 4u, 96u);
    put32(ram, kRaster + 20u, 12u);
    assert(!nativeTextureDescriptor(ram.data(), ram.size(), kRaster));
    assert(!nativeTextureDescriptor(ram.data(), ram.size(), 80u));

    // Destruction removes the native marker, so later release/lock/restore
    // cannot trigger native handling or call the preparation callback.
    RasterSet rasters;
    rasters.mark(kRaster);
    assert(rasters.contains(kRaster));
    rasters.forget(kRaster);
    assert(!rasters.contains(kRaster));
    rasters.forget(0u);
    assert(!rasters.contains(0u));
}
