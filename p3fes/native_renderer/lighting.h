#pragma once

#include "renderer.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace p3fes::native_renderer
{
inline std::array<float, 3> transformNormal(const Matrix4 &model, const std::array<float, 3> &normal)
{
    std::array<float, 3> out{
        model[0] * normal[0] + model[4] * normal[1] + model[8] * normal[2],
        model[1] * normal[0] + model[5] * normal[1] + model[9] * normal[2],
        model[2] * normal[0] + model[6] * normal[1] + model[10] * normal[2],
    };
    const float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
    if (length > 1.0e-8f)
    {
        out[0] /= length;
        out[1] /= length;
        out[2] /= length;
    }
    return out;
}

inline std::array<std::uint8_t, 4> shadeVertexColor(const Vertex &vertex,
                                                    const Material &material,
                                                    const std::array<std::uint8_t, 4> &materialColor,
                                                    bool meshLighting,
                                                    bool hasPrelight,
                                                    const MeshDrawState &state,
                                                    const Matrix4 &model)
{
    float rgb[3]{};
    if (meshLighting && state.lightingEnabled)
    {
        // Matches librw-vita's default.vert ordering: prelight first, then
        // ambient and diffuse directional terms, clamp, finally matColor.
        if (hasPrelight)
            for (int c = 0; c < 3; ++c)
                rgb[c] = vertex.color[c] / 255.0f;

        for (int c = 0; c < 3; ++c)
            rgb[c] += state.ambientLight[c] * material.ambient;

        const auto normal = transformNormal(model, vertex.normal);
        for (const MeshDrawState::DirectionalLight &light : state.directionalLights)
        {
            const float diffuse = std::max(0.0f,
                -(normal[0] * light.direction[0] + normal[1] * light.direction[1] + normal[2] * light.direction[2]));
            for (int c = 0; c < 3; ++c)
                rgb[c] += diffuse * light.color[c] * material.diffuse;
        }
    }
    else
    {
        for (int c = 0; c < 3; ++c)
            rgb[c] = vertex.color[c] / 255.0f;
    }

    std::array<std::uint8_t, 4> out{};
    for (int c = 0; c < 3; ++c)
    {
        const float value = std::clamp(rgb[c], 0.0f, 1.0f) * (materialColor[c] / 255.0f);
        out[c] = static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }
    out[3] = static_cast<std::uint8_t>((static_cast<unsigned int>(vertex.color[3]) * materialColor[3] + 127u) / 255u);
    return out;
}
} // namespace p3fes::native_renderer
