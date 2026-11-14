#include "../skinning.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

using namespace p3fes::native_renderer;

namespace
{
Vertex referencePoseSkinVertex(const Vertex &source, const std::vector<Matrix4> &palette)
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
        out.normal = {normal[0] / length, normal[1] / length, normal[2] / length};
    return out;
}

void assertSame(const Vertex &a, const Vertex &b)
{
    assert(a.position == b.position);
    assert(a.normal == b.normal);
    assert(a.texCoord == b.texCoord);
    assert(a.color == b.color);
    assert(a.bones == b.bones);
    for (std::size_t i = 0; i < a.weights.size(); ++i)
        assert(a.weights[i] == b.weights[i] ||
               (std::isnan(a.weights[i]) && std::isnan(b.weights[i])));
}

Matrix4 translated(float x, float y, float z)
{
    Matrix4 m = kIdentityMatrix;
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

void compareWithReference(const Vertex &source, const std::vector<Matrix4> &palette)
{
    assertSame(poseSkinVertex(source, palette), referencePoseSkinVertex(source, palette));
}
} // namespace

int main()
{
    const std::vector<Matrix4> palette{translated(3.25f, -2.5f, 1.75f), translated(-4.0f, 0.5f, 7.0f)};
    Vertex v{};
    v.position = {1.25f, -3.5f, 9.0f};
    v.normal = {0.3f, -0.4f, 0.5f};
    v.texCoord = {0.25f, 0.75f};
    v.color = {10u, 20u, 30u, 40u};
    v.bones = {0u, 1u, 0u, 0u};

    // Fast case and inactive later slots with values the production loop skips.
    v.weights = {1.0f, 0.0f, -0.25f, std::numeric_limits<float>::quiet_NaN()};
    compareWithReference(v, palette);
    v.weights[1] = std::numeric_limits<float>::infinity();
    compareWithReference(v, palette);
    v.weights = {1.0f, 4.0f, 0.0f, 0.0f};
    v.bones[1] = 9u; // positive weight, but invalid palette index is ignored
    compareWithReference(v, palette);

    // Multi-influence path, invalid weights/bones, and influence outside slot 0.
    v.bones = {0u, 1u, 0u, 1u};
    v.weights = {0.25f, 0.75f, 0.0f, 0.0f};
    compareWithReference(v, palette);
    v.weights = {0.0f, 0.0f, 0.4f, 0.0f};
    compareWithReference(v, palette);
    v.bones[2] = 8u;
    compareWithReference(v, palette);
    v.weights = {std::numeric_limits<float>::infinity(), -1.0f, 0.0f, 0.0f};
    compareWithReference(v, palette);

    // Unit influence with a degenerate normal preserves the source normal.
    v.bones = {0u, 1u, 0u, 0u};
    v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    v.normal = {0.0f, 0.0f, 0.0f};
    compareWithReference(v, palette);

    // Exercise both sides of the strict 1e-8 normal-length boundary.
    Matrix4 tiny = kIdentityMatrix;
    tiny[0] = tiny[5] = tiny[10] = 1.0f;
    std::vector<Matrix4> tinyPalette{tiny};
    v.bones = {0u, 0u, 0u, 0u};
    v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
    v.normal = {0.999e-8f, 0.0f, 0.0f};
    compareWithReference(v, tinyPalette);
    v.normal = {1.001e-8f, 0.0f, 0.0f};
    compareWithReference(v, tinyPalette);

    // No valid influences and an empty palette keep the source unchanged.
    v.weights = {0.0f, 0.0f, 0.0f, 0.0f};
    compareWithReference(v, palette);
    compareWithReference(v, {});
    return 0;
}
