// Compares the NEON regeneration kernel with the scalar reference (poseSkinVertex + shadeVertexColor).
// Build: clang++ -std=c++20 -O2 mesh_regen_test.cpp -o /tmp/mesh_regen_test   (needs NEON, i.e. arm64 or armv7)
#include "../lighting.h"
#include "../mesh_regen.h"
#include "../skinning.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace p3fes::native_renderer;

namespace
{
std::mt19937 rng(1234);
float rnd(float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); }

Matrix4 randomRigid(bool scaled)
{
    // rotation about a random axis, optional uniform scale, translation
    float ax = rnd(-1, 1), ay = rnd(-1, 1), az = rnd(-1, 1);
    const float len = std::sqrt(ax * ax + ay * ay + az * az) + 1e-6f;
    ax /= len; ay /= len; az /= len;
    const float a = rnd(0, 6.28f), c = std::cos(a), s = std::sin(a), t = 1 - c;
    const float k = scaled ? rnd(0.5f, 2.0f) : 1.0f;
    Matrix4 m{};
    m[0] = k * (t * ax * ax + c);      m[1] = k * (t * ax * ay + s * az); m[2] = k * (t * ax * az - s * ay);
    m[4] = k * (t * ax * ay - s * az); m[5] = k * (t * ay * ay + c);      m[6] = k * (t * ay * az + s * ax);
    m[8] = k * (t * ax * az + s * ay); m[9] = k * (t * ay * az - s * ax); m[10] = k * (t * az * az + c);
    m[12] = rnd(-5, 5); m[13] = rnd(-5, 5); m[14] = rnd(-5, 5); m[15] = 1;
    return m;
}
} // namespace

int main()
{
    int maxColorDiff = 0;
    float maxPosDiff = 0.0f;
    int cases = 0;
    for (int trial = 0; trial < 400; ++trial)
    {
        const bool skinned = trial & 1;
        const bool prelight = (trial >> 1) & 1;
        const bool lighting = (trial >> 2) & 1;
        const bool lightingOn = ((trial >> 3) & 3) != 0;
        const int dirs = trial % 5;
        const bool uvT = (trial % 7) == 0;
        std::vector<Matrix4> palette;
        const std::size_t paletteSize = (trial % 11 == 0) ? 0 : 6;
        for (std::size_t i = 0; i < paletteSize; ++i)
            palette.push_back(randomRigid(true));
        const Matrix4 model = randomRigid(trial % 3 == 0);
        Material material;
        material.ambient = rnd(0.2f, 1.2f);
        material.diffuse = rnd(0.2f, 1.2f);
        const std::array<std::uint8_t, 4> mcolor{std::uint8_t(rng() % 256), std::uint8_t(rng() % 256),
                                                 std::uint8_t(rng() % 256), std::uint8_t(rng() % 256)};
        MeshDrawState state;
        state.lightingEnabled = lightingOn;
        state.ambientLight = {rnd(0, 0.6f), rnd(0, 0.6f), rnd(0, 0.6f)};
        for (int d = 0; d < dirs; ++d)
        {
            MeshDrawState::DirectionalLight l;
            l.direction = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
            l.color = {rnd(0, 1), rnd(0, 1), rnd(0, 1)};
            state.directionalLights.push_back(l);
        }
        MeshDrawState::UvTransform uv;
        if (uvT) { uv.enabled = true; uv.rightX = 0.5f; uv.upY = 2.0f; uv.posX = 0.25f; uv.posY = -0.5f; }

        std::vector<Vertex> verts(257);
        for (Vertex &v : verts)
        {
            v.position = {rnd(-2, 2), rnd(-2, 2), rnd(-2, 2)};
            v.normal = {rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)};
            const float nl = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            for (float &c : v.normal) c /= nl;
            v.texCoord = {rnd(0, 1), rnd(0, 1)};
            for (auto &c : v.color) c = std::uint8_t(rng() % 256);
            if (rng() % 4 == 0) v.color[3] = 255;
            const int infl = 1 + int(rng() % 4);
            float sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                v.bones[k] = std::uint8_t(rng() % 8); // some >= palette size
                v.weights[k] = k < infl ? rnd(0.05f, 1.0f) : 0.0f;
                sum += v.weights[k];
            }
            if (rng() % 3 == 0) { v.weights = {1, 0, 0, 0}; sum = 1; }
            else for (float &w : v.weights) w /= sum;
        }

        detail::SkinLayout layout;
        if (skinned)
        {
            std::vector<std::uint16_t> indices;
            for (std::size_t i = 0; i < verts.size(); ++i)
                indices.push_back(static_cast<std::uint16_t>(i));
            const std::vector<Vertex> before = verts;
            detail::orderSkinnedVertices(verts, indices, layout);
            for (std::size_t i = 0; i < indices.size(); ++i) // indices must still address the same vertices
                assert(verts[indices[i]].position == before[i].position);
        }
        detail::RegenContext ctx;
        const bool ok = detail::prepareRegenContext(ctx, model, palette, material, mcolor, lighting, prelight, state, uv);
        assert(ok);
        std::vector<detail::MeshGpuVertex> out(verts.size());
        for (std::size_t i = 0; i < verts.size(); ++i)
        {
            out[i].position[0] = verts[i].position[0]; out[i].position[1] = verts[i].position[1];
            out[i].position[2] = verts[i].position[2];
        }
        detail::regenerateVertices(ctx, skinned, skinned && (trial % 4 != 1) ? &layout : nullptr, verts.data(), out.data(), verts.size());

        for (std::size_t i = 0; i < verts.size(); ++i)
        {
            const Vertex posed = skinned ? poseSkinVertex(verts[i], palette) : verts[i];
            const auto shaded = shadeVertexColor(posed, material, mcolor, lighting, prelight, state, model);
            const auto uvRef = transformTexCoord(verts[i].texCoord, uv);
            for (int c = 0; c < 4; ++c)
            {
                const int d = std::abs(int(shaded[c]) - int(out[i].color[c]));
                maxColorDiff = std::max(maxColorDiff, d);
                if (d > 1)
                {
                    std::printf("color mismatch trial=%d vtx=%zu c=%d ref=%d got=%d skinned=%d lit=%d dirs=%d\n", trial, i, c,
                                shaded[c], out[i].color[c], skinned, lighting && lightingOn, dirs);
                    return 1;
                }
            }
            for (int c = 0; c < 3; ++c)
            {
                const float d = std::fabs(posed.position[c] - out[i].position[c]);
                maxPosDiff = std::max(maxPosDiff, d);
                if (d > 1e-4f) { std::printf("pos mismatch trial=%d vtx=%zu\n", trial, i); return 1; }
            }
            if (std::fabs(uvRef[0] - out[i].uv[0]) > 1e-6f || std::fabs(uvRef[1] - out[i].uv[1]) > 1e-6f)
            {
                std::printf("uv mismatch trial=%d\n", trial);
                return 1;
            }
            ++cases;
        }
    }
    std::printf("mesh_regen_test ok: %d vertices, max colour diff %d LSB, max position diff %g\n", cases, maxColorDiff, maxPosDiff);
    return 0;
}
