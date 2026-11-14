#pragma once

#include <algorithm>
#include <cstdint>

namespace p3fes_im2d
{
// Adapt the title's screen-depth values at the RenderWare/native boundary.
// GPU backends consume a normalized depth; they never receive GS integers.
inline float inverseDepthRange(uint64_t zbuf)
{
    switch ((zbuf >> 24u) & 0xFu)
    {
    case 2u: case 10u: return 1.0f / 65535.0f;
    case 1u: return 1.0f / 16777215.0f;
    default: return 1.0f / 4294967295.0f;
    }
}

inline float normalizedDepth(float depth, float inverseRange)
{
    return std::clamp(depth * inverseRange, 0.0f, 1.0f);
}
}
