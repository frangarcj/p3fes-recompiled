#pragma once

#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace p3fes_native_texture_access::detail
{
    inline bool readU32(const uint8_t *rdram, uint32_t rdramSize, uint32_t address, uint32_t &value)
    {
        if (!rdram || rdramSize < sizeof(value) || address > rdramSize - sizeof(value))
            return false;
        std::memcpy(&value, rdram + address, sizeof(value));
        return true;
    }

    // Native bypass is deliberately limited to ordinary, CPU-backed type-4
    // textures. Camera/render-target type 5 stays on the guest GS path.
    inline bool nativeTextureDescriptor(const uint8_t *rdram, uint32_t rdramSize, uint32_t raster)
    {
        uint32_t pixels = 0u, widthRaw = 0u, heightRaw = 0u, depthRaw = 0u, strideRaw = 0u, typeRaw = 0u;
        if (!raster || raster > rdramSize || rdramSize - raster < 52u ||
            !readU32(rdram, rdramSize, raster + 4u, pixels) ||
            !readU32(rdram, rdramSize, raster + 12u, widthRaw) ||
            !readU32(rdram, rdramSize, raster + 16u, heightRaw) ||
            !readU32(rdram, rdramSize, raster + 20u, depthRaw) ||
            !readU32(rdram, rdramSize, raster + 24u, strideRaw) ||
            !readU32(rdram, rdramSize, raster + 32u, typeRaw))
            return false;

        const int32_t width = static_cast<int32_t>(widthRaw);
        const int32_t height = static_cast<int32_t>(heightRaw);
        const int32_t depth = static_cast<int32_t>(depthRaw);
        const int32_t stride = static_cast<int32_t>(strideRaw);
        const uint8_t type = static_cast<uint8_t>(typeRaw);
        return pixels != 0u && pixels < rdramSize && (type & 0x07u) == 4u &&
               width > 0 && width <= 4096 && height > 0 && height <= 4096 &&
               (depth == 4 || depth == 8 || depth == 16 || depth == 24 || depth == 32) &&
               stride > 0 && stride <= (1 << 20);
    }

    class RasterSet
    {
    public:
        bool contains(uint32_t raster) const { return m_rasters.contains(raster); }
        void mark(uint32_t raster) { if (raster) m_rasters.insert(raster); }
        void forget(uint32_t raster) { m_rasters.erase(raster); }

    private:
        std::unordered_set<uint32_t> m_rasters;
    };
}
