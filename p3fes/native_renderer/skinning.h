#pragma once

#include "renderer.h"

#include <array>
#include <cmath>

namespace p3fes::native_renderer
{
inline std::array<float, 3> skinTransformPoint(const Matrix4 &m, const std::array<float, 3> &v)
{
    return {
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14],
    };
}

inline std::array<float, 3> skinTransformVector(const Matrix4 &m, const std::array<float, 3> &v)
{
    return {
        m[0] * v[0] + m[4] * v[1] + m[8] * v[2],
        m[1] * v[0] + m[5] * v[1] + m[9] * v[2],
        m[2] * v[0] + m[6] * v[1] + m[10] * v[2],
    };
}

inline Vertex poseSkinVertex(const Vertex &source, const std::vector<Matrix4> &palette)
{
    if (palette.empty())
        return source;

    Vertex out = source;
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    float weightSum = 0.0f;
    for (std::size_t i = 0; i < source.weights.size(); ++i)
    {
        const float weight = source.weights[i];
        const std::uint32_t bone = source.bones[i];
        if (!(weight > 0.0f) || !std::isfinite(weight) || bone >= palette.size())
            continue;
        const auto p = skinTransformPoint(palette[bone], source.position);
        const auto n = skinTransformVector(palette[bone], source.normal);
        for (int c = 0; c < 3; ++c)
        {
            position[c] += p[c] * weight;
            normal[c] += n[c] * weight;
        }
        weightSum += weight;
    }
    if (!(weightSum > 1.0e-8f))
        return source;

    const float invWeight = 1.0f / weightSum;
    for (int c = 0; c < 3; ++c)
        out.position[c] = position[c] * invWeight;
    const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length > 1.0e-8f)
    {
        out.normal = {normal[0] / length, normal[1] / length, normal[2] / length};
    }
    return out;
}
} // namespace p3fes::native_renderer
