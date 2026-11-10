#pragma once

// Reused from HenryKun55/celosia (GPL-3.0), commit
// 059abbdf97097e5dbae3ca3e815364034cd31377. A presentation frame can carry
// a host GL texture instead of RGBA pixels, avoiding a GPU -> CPU -> GPU copy.

#include <cstdint>
#include <cstring>
#include <vector>

struct GSDirectFrame
{
    static constexpr uint32_t kMagic = 0x58544C47u; // "GLTX"
    uint32_t magic = kMagic;
    uint32_t texture = 0;
    uint64_t fence = 0;

    void store(std::vector<uint8_t> &pixels) const
    {
        pixels.resize(sizeof(GSDirectFrame));
        std::memcpy(pixels.data(), this, sizeof(GSDirectFrame));
    }

    static bool load(const std::vector<uint8_t> &pixels, GSDirectFrame &out)
    {
        if (pixels.size() != sizeof(GSDirectFrame))
            return false;
        std::memcpy(&out, pixels.data(), sizeof(GSDirectFrame));
        return out.magic == kMagic && out.texture != 0u;
    }
};
