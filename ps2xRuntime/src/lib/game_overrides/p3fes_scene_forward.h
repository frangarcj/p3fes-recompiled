#pragma once

#include <atomic>
#include <cstdint>

namespace p3fes_scene_forward
{
    constexpr unsigned speed = 4;
    // Published by the EE at vblank. Renderers never inspect live guest memory.
    inline std::atomic<bool> active{false};
    bool sample(const uint8_t *ram);
}
