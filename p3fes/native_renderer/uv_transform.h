#pragma once

#include "renderer.h"

#include <array>

namespace p3fes::native_renderer
{
inline std::array<float, 2> transformTexCoord(const std::array<float, 2> &uv,
                                              const MeshDrawState::UvTransform &transform)
{
    if (!transform.enabled)
        return uv;
    return {
        uv[0] * transform.rightX + uv[1] * transform.upX + transform.posX,
        uv[0] * transform.rightY + uv[1] * transform.upY + transform.posY,
    };
}
} // namespace p3fes::native_renderer
