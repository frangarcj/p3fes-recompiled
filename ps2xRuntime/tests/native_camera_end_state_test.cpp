#include "game_overrides/p3fes_native_camera.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint32_t kGp = 0x00030000u;
    constexpr uint32_t kCamera = 0x00001000u;
    constexpr uint32_t kRaster = 0x00002000u;

    void put32(std::vector<uint8_t> &ram, uint32_t address, uint32_t value)
    {
        std::memcpy(ram.data() + address, &value, sizeof(value));
    }

    uint32_t get32(const std::vector<uint8_t> &ram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, ram.data() + address, sizeof(value));
        return value;
    }
}

int main()
{
    std::vector<uint8_t> ram(32u * 1024u * 1024u);
    put32(ram, kCamera + 0x60u, kRaster);
    put32(ram, kRaster + 0x20u, 2u);
    put32(ram, kGp - 0x42C8u, 1u);
    put32(ram, kGp - 0x42FCu, 0x00123450u);

    // Normal successful EndUpdate clears ready and never advances the GS
    // packet cursor; the caller's return value is set to one by the hook.
    assert(p3fes_native_camera::applyNativeEndState(ram.data(), kGp, kCamera, kRaster));
    assert(get32(ram, kGp - 0x42C8u) == 0u);
    assert(get32(ram, kGp - 0x42FCu) == 0x00123450u);

    // With no pending ready state, the original returns success without a
    // guest RAM write; the helper preserves that state and still handles it.
    put32(ram, kGp - 0x42C8u, 0u);
    assert(p3fes_native_camera::applyNativeEndState(ram.data(), kGp, kCamera, kRaster));
    assert(get32(ram, kGp - 0x42C8u) == 0u);
    assert(get32(ram, kGp - 0x42FCu) == 0x00123450u);

    // Type 5 needs the original TEXFLUSH/lock callback path.
    put32(ram, kRaster + 0x20u, 5u);
    put32(ram, kGp - 0x42C8u, 1u);
    assert(!p3fes_native_camera::applyNativeEndState(ram.data(), kGp, kCamera, kRaster));
    assert(get32(ram, kGp - 0x42C8u) == 1u);
    assert(get32(ram, kGp - 0x42FCu) == 0x00123450u);
}
