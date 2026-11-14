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

    uint64_t get64(const std::vector<uint8_t> &ram, uint32_t address)
    {
        uint64_t value = 0u;
        std::memcpy(&value, ram.data() + address, sizeof(value));
        return value;
    }
}

int main()
{
    std::vector<uint8_t> ram(32u * 1024u * 1024u);
    put32(ram, kCamera + 0x60u, kRaster);
    put32(ram, kRaster + 0x20u, 2u);

    put32(ram, kGp - 0x4278u, 1u);
    put32(ram, kGp - 0x4358u, 1u);
    put32(ram, kGp - 0x435Cu, 0xAAu);
    put32(ram, kGp - 0x4360u, 1u);
    put32(ram, kGp - 0x4344u, 0x55u);
    put32(ram, kGp - 0x4270u, 0x1234u);
    put32(ram, kGp - 0x4268u, 0x5678u);
    put32(ram, kGp - 0x4258u, 8u);
    put32(ram, kGp - 0x425Cu, 9u);
    put32(ram, kGp - 0x42C8u, 0u);
    put32(ram, kGp - 0x42FCu, 0x00123450u); // Packet cursor must remain untouched.
    const uint64_t textureState = 0x1122334455667788ull;
    std::memcpy(ram.data() + 0x009649F0u, &textureState, sizeof(textureState));

    assert(p3fes_native_camera::applyNativeBeginState(ram.data(), kGp, kCamera, kRaster));
    assert(get32(ram, kGp - 0x42C8u) == 1u);
    assert(get32(ram, kGp - 0x4258u) == 9u);
    assert(get32(ram, kGp - 0x425Cu) == 0u);
    assert(get64(ram, kGp - 0x42B8u) == textureState);
    // LBU 0xAA XOR 1 XOR 0x55 = 0xFE, without byte sign extension.
    const uint64_t expectedGsState = (uint64_t{0xFEu} << 35u) |
                                     (uint64_t{0x1234u} << 4u) |
                                     (uint64_t{0x5678u} << 36u);
    assert(get64(ram, kGp - 0x42A8u) == expectedGsState);
    assert(get32(ram, kGp - 0x42FCu) == 0x00123450u);

    // The original uses LD here; preserve the upper half of the middle word.
    put32(ram, kGp - 0x4270u + 4u, 0x12345678u);
    assert(p3fes_native_camera::applyNativeBeginState(ram.data(), kGp, kCamera, kRaster));
    assert(get64(ram, kGp - 0x42A8u) ==
           ((uint64_t{0xFEu} << 35u) | (uint64_t{0x1234567800001234u} << 4u) |
            (uint64_t{0x5678u} << 36u)));

    put32(ram, kRaster + 0x20u, 5u);
    const uint32_t oldCount = get32(ram, kGp - 0x4258u);
    assert(!p3fes_native_camera::applyNativeBeginState(ram.data(), kGp, kCamera, kRaster));
    assert(get32(ram, kGp - 0x4258u) == oldCount);
}
