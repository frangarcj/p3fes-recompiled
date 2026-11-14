#pragma once

// Fast regeneration of the per-draw vertex data of a native mesh part (skinned position, lit vertex colour, UV).
//
// The reference is poseSkinVertex() + shadeVertexColor() (skinning.h / lighting.h): per vertex it normalises
// twice, runs one divide per normal component and walks vectors of lights through std::array copies. On the
// Cortex-A9 that came to ~1000 cycles per vertex. This header does the same arithmetic with one NEON pass:
//   * the skinned normal is never normalised on its own: normalize(M * normalize(s)) == normalize(M * s);
//   * every directional light is a lane of three vectors (LX, LY, LZ), so all dot products are 3 multiplies;
//   * 1/length is a reciprocal-square-root estimate with two Newton steps, no divide, no ARM<->NEON transfer;
//   * the prelight byte -> float step uses a 256-entry table holding the exact value of byte / 255.0f;
//   * the colour is clamped, scaled, rounded and packed in registers.
// Draws with local lights or more than four directional lights are not handled (supported() is false) and keep
// the scalar reference path. Results differ from the reference only by float rounding (<= 1 colour LSB).

#include "renderer.h"
#include "uv_transform.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define P3FES_REGEN_NEON 1
#else
#define P3FES_REGEN_NEON 0
#endif

namespace p3fes::native_renderer::detail
{
// What the fixed-function mesh path reads per vertex (the normal is only an input of the CPU shading).
struct MeshGpuVertex
{
    float position[3];
    float uv[2];
    std::uint8_t color[4];
};
static_assert(sizeof(MeshGpuVertex) == 24u);

// Skinned parts: ~93% of the vertices follow exactly one bone with weight 1 (rigid skinning). They are stored
// grouped by bone, so a whole run shares one matrix and is processed with that matrix held in registers; the
// others (blended vertices) come last and use the general kernel.
struct SkinRun
{
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    std::uint32_t bone = 0;
};

struct SkinLayout
{
    std::vector<SkinRun> runs;
    std::uint32_t genericFirst = 0; // vertices [genericFirst, end) are blended / irregular
};

inline bool isRigidlyWeighted(const Vertex &v)
{
    return v.weights[0] == 1.0f && !(v.weights[1] > 0.0f) && !(v.weights[2] > 0.0f) && !(v.weights[3] > 0.0f);
}

// Reorders `vertices` (and remaps `indices`) into [rigid runs sorted by bone][generic vertices].
inline void orderSkinnedVertices(std::vector<Vertex> &vertices, std::vector<std::uint16_t> &indices, SkinLayout &layout)
{
    layout.runs.clear();
    const std::size_t count = vertices.size();
    std::vector<std::uint32_t> order(count);
    for (std::size_t i = 0; i < count; ++i)
        order[i] = static_cast<std::uint32_t>(i);
    auto key = [&](std::uint32_t i) -> std::uint32_t
    {
        const Vertex &v = vertices[i];
        return isRigidlyWeighted(v) ? static_cast<std::uint32_t>(v.bones[0]) : 256u;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return key(a) < key(b); });
    std::vector<Vertex> sorted(count);
    std::vector<std::uint16_t> newIndex(count);
    for (std::size_t n = 0; n < count; ++n)
    {
        sorted[n] = vertices[order[n]];
        newIndex[order[n]] = static_cast<std::uint16_t>(n);
    }
    for (std::uint16_t &index : indices)
        index = newIndex[index];
    vertices.swap(sorted);
    layout.genericFirst = static_cast<std::uint32_t>(count);
    for (std::size_t n = 0; n < count;)
    {
        if (!isRigidlyWeighted(vertices[n]))
        {
            layout.genericFirst = static_cast<std::uint32_t>(n);
            break;
        }
        std::size_t end = n;
        while (end < count && isRigidlyWeighted(vertices[end]) && vertices[end].bones[0] == vertices[n].bones[0])
            ++end;
        layout.runs.push_back({static_cast<std::uint32_t>(n), static_cast<std::uint32_t>(end - n), vertices[n].bones[0]});
        n = end;
    }
}

struct RegenContext
{
    const Matrix4 *model = nullptr;
    const Matrix4 *palette = nullptr;
    std::size_t paletteCount = 0;
    bool usePrelight = true;
    int lightCount = 0; // directional lights, 0..4
    alignas(16) float ambient[4]{};   // ambientLight * material.ambient (0 when the part is not lit)
    alignas(16) float lightX[4]{};    // lane d = direction of light d
    alignas(16) float lightY[4]{};
    alignas(16) float lightZ[4]{};
    alignas(16) float lightColor[4][4]{}; // colour * material.diffuse (rgb, 0)
    alignas(16) float scale[4]{};     // material colour / 255
    std::uint8_t materialAlpha = 255;
    MeshDrawState::UvTransform uv{};
};

// False when the draw needs something (or when the build has no NEON: callers then run the scalar reference) the fast kernel does not do (local lights, > 4 directional lights).
inline bool prepareRegenContext(RegenContext &ctx,
                                const Matrix4 &model,
                                const std::vector<Matrix4> &palette,
                                const Material &material,
                                const std::array<std::uint8_t, 4> &materialColor,
                                bool meshLighting,
                                bool hasPrelight,
                                const MeshDrawState &state,
                                const MeshDrawState::UvTransform &uvTransform)
{
#if !P3FES_REGEN_NEON
    return false;
#endif
    const bool lit = meshLighting && state.lightingEnabled;
    if (lit && (!state.localLights.empty() || state.directionalLights.size() > 4u))
        return false;
    ctx.model = &model;
    ctx.palette = palette.data();
    ctx.paletteCount = palette.size();
    ctx.usePrelight = lit ? hasPrelight : true;
    ctx.lightCount = lit ? static_cast<int>(state.directionalLights.size()) : 0;
    for (int c = 0; c < 4; ++c)
    {
        ctx.ambient[c] = (lit && c < 3) ? state.ambientLight[c] * material.ambient : 0.0f;
        ctx.lightX[c] = ctx.lightY[c] = ctx.lightZ[c] = 0.0f;
        for (int k = 0; k < 4; ++k)
            ctx.lightColor[c][k] = 0.0f;
        ctx.scale[c] = materialColor[c] / 255.0f;
    }
    for (int d = 0; d < ctx.lightCount; ++d)
    {
        const MeshDrawState::DirectionalLight &light = state.directionalLights[static_cast<std::size_t>(d)];
        ctx.lightX[d] = light.direction[0];
        ctx.lightY[d] = light.direction[1];
        ctx.lightZ[d] = light.direction[2];
        for (int c = 0; c < 3; ++c)
            ctx.lightColor[d][c] = light.color[c] * material.diffuse;
    }
    ctx.materialAlpha = materialColor[3];
    ctx.uv = uvTransform;
    return true;
}

inline std::uint8_t regenAlpha(std::uint8_t vertexAlpha, std::uint8_t materialAlpha)
{
    return static_cast<std::uint8_t>((static_cast<unsigned int>(vertexAlpha) * materialAlpha + 127u) / 255u);
}

#if P3FES_REGEN_NEON
struct ByteToUnit
{
    float v[256];
    ByteToUnit()
    {
        for (int i = 0; i < 256; ++i)
            v[i] = static_cast<float>(i) / 255.0f; // exact byte / 255.0f, as the reference computes it
    }
};
inline const ByteToUnit kByteToUnit;
inline const float *byteToUnitTable() { return kByteToUnit.v; }

// Cortex-A9 NEON issues in order and a dependent multiply-accumulate chain costs ~10 cycles per link, so a loop
// that handles one vertex at a time spends most of its time waiting. The kernel therefore works on blocks of four
// vertices whose chains are independent (skinning per vertex in xyz lanes, lighting transposed so a lane is a
// vertex). Blocks that contain a vertex without a usable influence, and the tail, take the one-vertex path.
template <bool Skinned, bool UsePrelight, int Lights>
inline void regenKernel(const RegenContext &ctx, const Vertex *src, MeshGpuVertex *dst, std::size_t count)
{
    constexpr bool kNeedNormal = Lights > 0;
    const float *unit = byteToUnitTable();
    const float *model = ctx.model->data();
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t tiny = vdupq_n_f32(1.0e-16f);
    const uint32x4_t laneMask = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0u};
    // Model rotation columns with lane 3 forced to zero.
    const float32x4_t m0 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 0)), laneMask));
    const float32x4_t m1 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 4)), laneMask));
    const float32x4_t m2 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 8)), laneMask));
    const float32x4_t ambient = vld1q_f32(ctx.ambient);
    const float32x4_t lx = vld1q_f32(ctx.lightX);
    const float32x4_t ly = vld1q_f32(ctx.lightY);
    const float32x4_t lz = vld1q_f32(ctx.lightZ);
    const float32x4_t col0 = vld1q_f32(ctx.lightColor[0]);
    const float32x4_t col1 = vld1q_f32(ctx.lightColor[1]);
    const float32x4_t col2 = vld1q_f32(ctx.lightColor[2]);
    const float32x4_t col3 = vld1q_f32(ctx.lightColor[3]);
    const float32x4_t scale = vld1q_f32(ctx.scale);
    const bool uvEnabled = ctx.uv.enabled;

    auto writeUv = [&](const Vertex &v, MeshGpuVertex &out)
    {
        if (uvEnabled)
        {
            const auto uv = transformTexCoord(v.texCoord, ctx.uv);
            out.uv[0] = uv[0];
            out.uv[1] = uv[1];
        }
        else
        {
            out.uv[0] = v.texCoord[0];
            out.uv[1] = v.texCoord[1];
        }
    };

    // One vertex, any case (the path of the reference, with the NEON arithmetic).
    auto single = [&](const Vertex &v, MeshGpuVertex &out)
    {
        float32x4_t normal;
        if (Skinned)
        {
            const float32x4_t sp = vld1q_f32(v.position.data());
            const float32x4_t sn = vld1q_f32(v.normal.data());
            const float32x2_t pl = vget_low_f32(sp), ph = vget_high_f32(sp);
            const float32x2_t nl = vget_low_f32(sn), nh = vget_high_f32(sn);
            float32x4_t pos = zero, nrm = zero;
            float weightSum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                const float w = v.weights[static_cast<std::size_t>(k)];
                const std::uint32_t bone = v.bones[static_cast<std::size_t>(k)];
                if (!(w > 0.0f) || !(w <= 3.0e38f) || bone >= ctx.paletteCount)
                    continue;
                const float *m = ctx.palette[bone].data();
                const float32x4_t c0 = vld1q_f32(m + 0), c1 = vld1q_f32(m + 4);
                const float32x4_t c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
                float32x4_t t = vmlaq_lane_f32(c3, c0, pl, 0);
                t = vmlaq_lane_f32(t, c1, pl, 1);
                t = vmlaq_lane_f32(t, c2, ph, 0);
                float32x4_t u = vmulq_lane_f32(c0, nl, 0);
                u = vmlaq_lane_f32(u, c1, nl, 1);
                u = vmlaq_lane_f32(u, c2, nh, 0);
                pos = vmlaq_n_f32(pos, t, w);
                nrm = vmlaq_n_f32(nrm, u, w);
                weightSum += w;
            }
            if (weightSum > 1.0e-8f)
            {
                if (weightSum != 1.0f)
                    pos = vmulq_n_f32(pos, 1.0f / weightSum);
                normal = nrm;
            }
            else
            {
                pos = sp;
                normal = sn;
            }
            vst1q_f32(out.position, pos); // lane 3 lands in uv[0], overwritten below
        }
        else
            normal = vld1q_f32(v.normal.data());

        writeUv(v, out);

        float32x4_t rgb = ambient;
        if (UsePrelight)
        {
            const float pre[4] = {unit[v.color[0]], unit[v.color[1]], unit[v.color[2]], 0.0f};
            rgb = vaddq_f32(vld1q_f32(pre), ambient);
        }
        if (Lights > 0)
        {
            const float32x2_t nl = vget_low_f32(normal), nh = vget_high_f32(normal);
            float32x4_t world = vmulq_lane_f32(m0, nl, 0);
            world = vmlaq_lane_f32(world, m1, nl, 1);
            world = vmlaq_lane_f32(world, m2, nh, 0);
            const float32x4_t sq = vmulq_f32(world, world);
            const float32x2_t pair = vpadd_f32(vget_low_f32(sq), vget_high_f32(sq));
            const float32x2_t sum = vpadd_f32(pair, pair);
            const float32x4_t len2 = vmaxq_f32(vcombine_f32(sum, sum), tiny);
            float32x4_t inv = vrsqrteq_f32(len2);
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            const float32x2_t wl = vget_low_f32(world), wh = vget_high_f32(world);
            float32x4_t dots = vmulq_lane_f32(lx, wl, 0);
            dots = vmlaq_lane_f32(dots, ly, wl, 1);
            dots = vmlaq_lane_f32(dots, lz, wh, 0);
            const float32x4_t diffuse = vmaxq_f32(vnegq_f32(vmulq_f32(dots, inv)), zero);
            const float32x2_t dl = vget_low_f32(diffuse), dh = vget_high_f32(diffuse);
            rgb = vmlaq_lane_f32(rgb, col0, dl, 0);
            if (Lights > 1)
                rgb = vmlaq_lane_f32(rgb, col1, dl, 1);
            if (Lights > 2)
            {
                rgb = vmlaq_lane_f32(rgb, col2, dh, 0);
                rgb = vmlaq_lane_f32(rgb, col3, dh, 1);
            }
        }
        rgb = vminq_f32(vmaxq_f32(rgb, zero), one);
        rgb = vmulq_f32(rgb, scale);
        const uint32x4_t quant = vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(rgb, one), 255.0f));
        const uint8x8_t bytes = vmovn_u16(vcombine_u16(vmovn_u32(quant), vmovn_u32(quant)));
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(out.color), vreinterpret_u32_u8(bytes), 0);
        out.color[3] = v.color[3] == 255u ? ctx.materialAlpha : regenAlpha(v.color[3], ctx.materialAlpha);
    };

    const float ambR = ctx.ambient[0], ambG = ctx.ambient[1], ambB = ctx.ambient[2];
    const float scaleR = ctx.scale[0], scaleG = ctx.scale[1], scaleB = ctx.scale[2];

    std::size_t i = 0;
    for (; i + 4 <= count; i += 4)
    {
        // Streaming read of the 56-byte source vertices: ask for the next blocks early.
        __builtin_prefetch(&src[i + 8]);
        __builtin_prefetch(reinterpret_cast<const char *>(&src[i + 8]) + 32);
        __builtin_prefetch(&src[i + 10]);
        __builtin_prefetch(reinterpret_cast<const char *>(&src[i + 10]) + 32);
        const Vertex *v[4] = {&src[i], &src[i + 1], &src[i + 2], &src[i + 3]};
        float32x4_t pos[4];
        float32x4_t nrm[4];
        if (Skinned)
        {
            float w[4][4];
            std::uint32_t bone[4][4];
            float wsum[4];
            int influences = 0;
            bool usable = true;
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
            {
                int cnt = 0;
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    const float wk = v[j]->weights[static_cast<std::size_t>(k)];
                    const std::uint32_t bk = v[j]->bones[static_cast<std::size_t>(k)];
                    if (!(wk > 0.0f) || !(wk <= 3.0e38f) || bk >= ctx.paletteCount)
                        continue;
                    w[j][cnt] = wk;
                    bone[j][cnt] = bk;
                    sum += wk;
                    ++cnt;
                }
                for (int k = cnt; k < 4; ++k)
                {
                    w[j][k] = 0.0f;
                    bone[j][k] = 0u;
                }
                wsum[j] = sum;
                usable = usable && sum > 1.0e-8f;
                influences = cnt > influences ? cnt : influences;
            }
            if (!usable)
            {
                for (int j = 0; j < 4; ++j)
                    single(*v[j], dst[i + static_cast<std::size_t>(j)]);
                continue;
            }
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
            {
                pos[j] = zero;
                nrm[j] = zero;
            }
            for (int k = 0; k < influences; ++k)
            {
#pragma GCC unroll 4
                for (int j = 0; j < 4; ++j)
                {
                    const float *m = ctx.palette[bone[j][k]].data();
                    const float32x4_t c0 = vld1q_f32(m + 0), c1 = vld1q_f32(m + 4);
                    const float32x4_t c2 = vld1q_f32(m + 8), c3 = vld1q_f32(m + 12);
                    const float32x4_t sp = vld1q_f32(v[j]->position.data());
                    const float32x2_t pl = vget_low_f32(sp), ph = vget_high_f32(sp);
                    float32x4_t t = vmlaq_lane_f32(c3, c0, pl, 0);
                    t = vmlaq_lane_f32(t, c1, pl, 1);
                    t = vmlaq_lane_f32(t, c2, ph, 0);
                    pos[j] = vmlaq_n_f32(pos[j], t, w[j][k]);
                    if (kNeedNormal)
                    {
                        const float32x4_t sn = vld1q_f32(v[j]->normal.data());
                        const float32x2_t nl = vget_low_f32(sn), nh = vget_high_f32(sn);
                        float32x4_t u = vmulq_lane_f32(c0, nl, 0);
                        u = vmlaq_lane_f32(u, c1, nl, 1);
                        u = vmlaq_lane_f32(u, c2, nh, 0);
                        nrm[j] = vmlaq_n_f32(nrm[j], u, w[j][k]);
                    }
                }
            }
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
            {
                if (wsum[j] != 1.0f)
                    pos[j] = vmulq_n_f32(pos[j], 1.0f / wsum[j]);
                vst1q_f32(dst[i + static_cast<std::size_t>(j)].position, pos[j]);
            }
        }
        else if (kNeedNormal)
        {
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
                nrm[j] = vld1q_f32(v[j]->normal.data());
        }

#pragma GCC unroll 4
        for (int j = 0; j < 4; ++j)
            writeUv(*v[j], dst[i + static_cast<std::size_t>(j)]);

        // Lighting with one vertex per lane.
        float32x4_t r, g, b;
        if (UsePrelight)
        {
            float pre[3][4];
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
            {
                pre[0][j] = unit[v[j]->color[0]];
                pre[1][j] = unit[v[j]->color[1]];
                pre[2][j] = unit[v[j]->color[2]];
            }
            r = vaddq_f32(vld1q_f32(pre[0]), vdupq_n_f32(ambR));
            g = vaddq_f32(vld1q_f32(pre[1]), vdupq_n_f32(ambG));
            b = vaddq_f32(vld1q_f32(pre[2]), vdupq_n_f32(ambB));
        }
        else
        {
            r = vdupq_n_f32(ambR);
            g = vdupq_n_f32(ambG);
            b = vdupq_n_f32(ambB);
        }
        if (Lights > 0)
        {
            float32x4_t world[4];
#pragma GCC unroll 4
            for (int j = 0; j < 4; ++j)
            {
                const float32x2_t nl = vget_low_f32(nrm[j]), nh = vget_high_f32(nrm[j]);
                float32x4_t wv = vmulq_lane_f32(m0, nl, 0);
                wv = vmlaq_lane_f32(wv, m1, nl, 1);
                world[j] = vmlaq_lane_f32(wv, m2, nh, 0);
            }
            const float32x4x2_t t01 = vtrnq_f32(world[0], world[1]);
            const float32x4x2_t t23 = vtrnq_f32(world[2], world[3]);
            const float32x4_t wx = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
            const float32x4_t wy = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
            const float32x4_t wz = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
            float32x4_t len2 = vmulq_f32(wx, wx);
            len2 = vmlaq_f32(len2, wy, wy);
            len2 = vmlaq_f32(len2, wz, wz);
            len2 = vmaxq_f32(len2, tiny);
            float32x4_t inv = vrsqrteq_f32(len2);
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            const float32x4_t nx = vmulq_f32(wx, inv), ny = vmulq_f32(wy, inv), nz = vmulq_f32(wz, inv);
#pragma GCC unroll 4
            for (int d = 0; d < Lights; ++d)
            {
                float32x4_t dot = vmulq_n_f32(nx, ctx.lightX[d]);
                dot = vmlaq_n_f32(dot, ny, ctx.lightY[d]);
                dot = vmlaq_n_f32(dot, nz, ctx.lightZ[d]);
                const float32x4_t diffuse = vmaxq_f32(vnegq_f32(dot), zero);
                r = vmlaq_n_f32(r, diffuse, ctx.lightColor[d][0]);
                g = vmlaq_n_f32(g, diffuse, ctx.lightColor[d][1]);
                b = vmlaq_n_f32(b, diffuse, ctx.lightColor[d][2]);
            }
        }
        r = vmulq_n_f32(vminq_f32(vmaxq_f32(r, zero), one), scaleR);
        g = vmulq_n_f32(vminq_f32(vmaxq_f32(g, zero), one), scaleG);
        b = vmulq_n_f32(vminq_f32(vmaxq_f32(b, zero), one), scaleB);
        const uint16x4_t r16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(r, one), 255.0f)));
        const uint16x4_t g16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(g, one), 255.0f)));
        const uint16x4_t b16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(b, one), 255.0f)));
        const uint8x8_t r8 = vmovn_u16(vcombine_u16(r16, r16));
        const uint8x8_t g8 = vmovn_u16(vcombine_u16(g16, g16));
        const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
        const uint8x8_t a8 = vdup_n_u8(ctx.materialAlpha);
        const uint8x8x2_t rg = vzip_u8(r8, g8);
        const uint8x8x2_t ba = vzip_u8(b8, a8);
        const uint16x4x2_t px = vzip_u16(vreinterpret_u16_u8(rg.val[0]), vreinterpret_u16_u8(ba.val[0]));
        const uint32x2_t lo = vreinterpret_u32_u16(px.val[0]);
        const uint32x2_t hi = vreinterpret_u32_u16(px.val[1]);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(dst[i + 0].color), lo, 0);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(dst[i + 1].color), lo, 1);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(dst[i + 2].color), hi, 0);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(dst[i + 3].color), hi, 1);
#pragma GCC unroll 4
        for (int j = 0; j < 4; ++j)
            if (v[j]->color[3] != 255u)
                dst[i + static_cast<std::size_t>(j)].color[3] = regenAlpha(v[j]->color[3], ctx.materialAlpha);
    }
    for (; i < count; ++i)
        single(src[i], dst[i]);
}

// A run of vertices that share one transform: positions are transformed by `bone` (skinned) and the world normal
// by `world` = model rotation * bone rotation, both held in registers for the whole run. Four vertices per step,
// one vertex per lane for the lighting.
template <bool UsePrelight, int Lights, bool Skinned>
inline void regenRun(const RegenContext &ctx, const Matrix4 *bone, const Vertex *src, MeshGpuVertex *dst,
                     std::size_t count)
{
    const float *model = ctx.model->data();
    const float *unit = byteToUnitTable();
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t one = vdupq_n_f32(1.0f);
    const float32x4_t half = vdupq_n_f32(0.5f);
    const float32x4_t tiny = vdupq_n_f32(1.0e-16f);
    const uint32x4_t laneMask = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0u};
    const float32x4_t m0 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 0)), laneMask));
    const float32x4_t m1 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 4)), laneMask));
    const float32x4_t m2 = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(vld1q_f32(model + 8)), laneMask));
    float32x4_t b0 = zero, b1 = zero, b2 = zero, b3 = zero; // bone columns (position)
    float32x4_t w0 = m0, w1 = m1, w2 = m2;                   // world rotation columns (normal)
    if (Skinned)
    {
        const float *bm = bone->data();
        b0 = vld1q_f32(bm + 0);
        b1 = vld1q_f32(bm + 4);
        b2 = vld1q_f32(bm + 8);
        b3 = vld1q_f32(bm + 12);
        if (Lights > 0)
        {
            // (model * bone) column k = model rotation * bone column k
            auto column = [&](float32x4_t c)
            {
                const float32x2_t cl = vget_low_f32(c), ch = vget_high_f32(c);
                float32x4_t out = vmulq_lane_f32(m0, cl, 0);
                out = vmlaq_lane_f32(out, m1, cl, 1);
                return vmlaq_lane_f32(out, m2, ch, 0);
            };
            w0 = column(b0);
            w1 = column(b1);
            w2 = column(b2);
        }
    }
    const float32x2_t w0l = vget_low_f32(w0), w0h = vget_high_f32(w0);
    const float32x2_t w1l = vget_low_f32(w1), w1h = vget_high_f32(w1);
    const float32x2_t w2l = vget_low_f32(w2), w2h = vget_high_f32(w2);
    const float32x4_t ambR = vdupq_n_f32(ctx.ambient[0]), ambG = vdupq_n_f32(ctx.ambient[1]),
                      ambB = vdupq_n_f32(ctx.ambient[2]);
    const float scaleR = ctx.scale[0], scaleG = ctx.scale[1], scaleB = ctx.scale[2];
    const uint8x8_t alpha8 = vdup_n_u8(ctx.materialAlpha);

    // Four consecutive vertices s[0..3] -> d[0..3].
    auto block = [&](const Vertex *s, MeshGpuVertex *d)
    {
        float32x4_t nrm[4];
#pragma GCC unroll 4
        for (int j = 0; j < 4; ++j)
        {
            if (Skinned)
            {
                const float32x4_t sp = vld1q_f32(s[j].position.data());
                const float32x2_t pl = vget_low_f32(sp), ph = vget_high_f32(sp);
                float32x4_t t = vmlaq_lane_f32(b3, b0, pl, 0);
                t = vmlaq_lane_f32(t, b1, pl, 1);
                t = vmlaq_lane_f32(t, b2, ph, 0);
                vst1q_f32(d[j].position, t); // lane 3 lands in uv[0], overwritten below
            }
            if (Lights > 0)
                nrm[j] = vld1q_f32(s[j].normal.data());
            vst1_f32(d[j].uv, vld1_f32(s[j].texCoord.data()));
        }

        float32x4_t r, g, b;
        if (UsePrelight)
        {
            // Exact byte / 255.0f through the table, gathered straight into the lanes.
            float32x4_t pr = ambR, pg = ambG, pb = ambB; // placeholders, replaced below
            float32x4_t tr = zero, tg = zero, tb = zero;
            tr = vld1q_lane_f32(unit + s[0].color[0], tr, 0);
            tg = vld1q_lane_f32(unit + s[0].color[1], tg, 0);
            tb = vld1q_lane_f32(unit + s[0].color[2], tb, 0);
            tr = vld1q_lane_f32(unit + s[1].color[0], tr, 1);
            tg = vld1q_lane_f32(unit + s[1].color[1], tg, 1);
            tb = vld1q_lane_f32(unit + s[1].color[2], tb, 1);
            tr = vld1q_lane_f32(unit + s[2].color[0], tr, 2);
            tg = vld1q_lane_f32(unit + s[2].color[1], tg, 2);
            tb = vld1q_lane_f32(unit + s[2].color[2], tb, 2);
            tr = vld1q_lane_f32(unit + s[3].color[0], tr, 3);
            tg = vld1q_lane_f32(unit + s[3].color[1], tg, 3);
            tb = vld1q_lane_f32(unit + s[3].color[2], tb, 3);
            (void)pr; (void)pg; (void)pb;
            r = vaddq_f32(tr, ambR);
            g = vaddq_f32(tg, ambG);
            b = vaddq_f32(tb, ambB);
        }
        else
        {
            r = ambR;
            g = ambG;
            b = ambB;
        }
        if (Lights > 0)
        {
            const float32x4x2_t t01 = vtrnq_f32(nrm[0], nrm[1]);
            const float32x4x2_t t23 = vtrnq_f32(nrm[2], nrm[3]);
            const float32x4_t nx = vcombine_f32(vget_low_f32(t01.val[0]), vget_low_f32(t23.val[0]));
            const float32x4_t ny = vcombine_f32(vget_low_f32(t01.val[1]), vget_low_f32(t23.val[1]));
            const float32x4_t nz = vcombine_f32(vget_high_f32(t01.val[0]), vget_high_f32(t23.val[0]));
            // world = columns w0,w1,w2 applied to (nx,ny,nz)
            float32x4_t wx = vmulq_lane_f32(nx, w0l, 0);
            wx = vmlaq_lane_f32(wx, ny, w1l, 0);
            wx = vmlaq_lane_f32(wx, nz, w2l, 0);
            float32x4_t wy = vmulq_lane_f32(nx, w0l, 1);
            wy = vmlaq_lane_f32(wy, ny, w1l, 1);
            wy = vmlaq_lane_f32(wy, nz, w2l, 1);
            float32x4_t wz = vmulq_lane_f32(nx, w0h, 0);
            wz = vmlaq_lane_f32(wz, ny, w1h, 0);
            wz = vmlaq_lane_f32(wz, nz, w2h, 0);
            float32x4_t len2 = vmulq_f32(wx, wx);
            len2 = vmlaq_f32(len2, wy, wy);
            len2 = vmlaq_f32(len2, wz, wz);
            len2 = vmaxq_f32(len2, tiny);
            float32x4_t inv = vrsqrteq_f32(len2);
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            inv = vmulq_f32(inv, vrsqrtsq_f32(vmulq_f32(len2, inv), inv));
            wx = vmulq_f32(wx, inv);
            wy = vmulq_f32(wy, inv);
            wz = vmulq_f32(wz, inv);
#pragma GCC unroll 4
            for (int l = 0; l < Lights; ++l)
            {
                float32x4_t dot = vmulq_n_f32(wx, ctx.lightX[l]);
                dot = vmlaq_n_f32(dot, wy, ctx.lightY[l]);
                dot = vmlaq_n_f32(dot, wz, ctx.lightZ[l]);
                const float32x4_t diffuse = vmaxq_f32(vnegq_f32(dot), zero);
                r = vmlaq_n_f32(r, diffuse, ctx.lightColor[l][0]);
                g = vmlaq_n_f32(g, diffuse, ctx.lightColor[l][1]);
                b = vmlaq_n_f32(b, diffuse, ctx.lightColor[l][2]);
            }
        }
        r = vmulq_n_f32(vminq_f32(vmaxq_f32(r, zero), one), scaleR);
        g = vmulq_n_f32(vminq_f32(vmaxq_f32(g, zero), one), scaleG);
        b = vmulq_n_f32(vminq_f32(vmaxq_f32(b, zero), one), scaleB);
        const uint16x4_t r16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(r, one), 255.0f)));
        const uint16x4_t g16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(g, one), 255.0f)));
        const uint16x4_t b16 = vmovn_u32(vcvtq_u32_f32(vmlaq_n_f32(half, vminq_f32(b, one), 255.0f)));
        const uint8x8_t r8 = vmovn_u16(vcombine_u16(r16, r16));
        const uint8x8_t g8 = vmovn_u16(vcombine_u16(g16, g16));
        const uint8x8_t b8 = vmovn_u16(vcombine_u16(b16, b16));
        const uint8x8x2_t rg = vzip_u8(r8, g8);
        const uint8x8x2_t ba = vzip_u8(b8, alpha8);
        const uint16x4x2_t px = vzip_u16(vreinterpret_u16_u8(rg.val[0]), vreinterpret_u16_u8(ba.val[0]));
        const uint32x2_t lo = vreinterpret_u32_u16(px.val[0]);
        const uint32x2_t hi = vreinterpret_u32_u16(px.val[1]);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(d[0].color), lo, 0);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(d[1].color), lo, 1);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(d[2].color), hi, 0);
        vst1_lane_u32(reinterpret_cast<std::uint32_t *>(d[3].color), hi, 1);
    };

    // Vertex alpha other than 255 (the block wrote the material alpha).
    auto fixAlpha = [&](const Vertex *s, MeshGpuVertex *d, std::size_t n)
    {
        for (std::size_t j = 0; j < n; ++j)
            if (s[j].color[3] != 255u)
                d[j].color[3] = regenAlpha(s[j].color[3], ctx.materialAlpha);
    };

    std::size_t i = 0;
    // Two blocks per step: their dependency chains are independent, so the in-order NEON pipeline overlaps them.
    for (; i + 8 <= count; i += 8)
    {
        __builtin_prefetch(&src[i + 16]);
        __builtin_prefetch(reinterpret_cast<const char *>(&src[i + 16]) + 32);
        __builtin_prefetch(&src[i + 20]);
        __builtin_prefetch(reinterpret_cast<const char *>(&src[i + 20]) + 32);
        block(src + i, dst + i);
        block(src + i + 4, dst + i + 4);
        fixAlpha(src + i, dst + i, 8);
    }
    if (i + 4 <= count)
    {
        block(src + i, dst + i);
        fixAlpha(src + i, dst + i, 4);
        i += 4;
    }
    if (i < count)
    {
        // Tail of 1..3 vertices: run a full block on copies (the last vertex repeated) and keep the real ones.
        const std::size_t rest = count - i;
        Vertex tmpSrc[4];
        MeshGpuVertex tmpDst[4];
        for (std::size_t j = 0; j < 4; ++j)
        {
            tmpSrc[j] = src[i + (j < rest ? j : rest - 1u)];
            tmpDst[j] = dst[i + (j < rest ? j : rest - 1u)]; // rigid runs keep the stored positions
        }
        block(tmpSrc, tmpDst);
        fixAlpha(tmpSrc, tmpDst, 4);
        for (std::size_t j = 0; j < rest; ++j)
            dst[i + j] = tmpDst[j];
    }
    if (ctx.uv.enabled)
        for (std::size_t j = 0; j < count; ++j)
        {
            const auto uv = transformTexCoord(src[j].texCoord, ctx.uv);
            dst[j].uv[0] = uv[0];
            dst[j].uv[1] = uv[1];
        }
}

template <bool UsePrelight, int Lights>
inline void regenFast(const RegenContext &ctx, bool skinned, const SkinLayout *layout, const Vertex *src,
                      MeshGpuVertex *dst, std::size_t count)
{
    if (!skinned)
    {
        regenRun<UsePrelight, Lights, false>(ctx, nullptr, src, dst, count);
        return;
    }
    std::size_t generic = 0;
    if (layout)
    {
        generic = layout->genericFirst;
        for (const SkinRun &run : layout->runs)
        {
            if (run.bone < ctx.paletteCount)
                regenRun<UsePrelight, Lights, true>(ctx, &ctx.palette[run.bone], src + run.first, dst + run.first,
                                                    run.count);
            else // no such bone: the reference keeps the source position and normal
                regenKernel<true, UsePrelight, Lights>(ctx, src + run.first, dst + run.first, run.count);
        }
    }
    if (generic < count)
        regenKernel<true, UsePrelight, Lights>(ctx, src + generic, dst + generic, count - generic);
}

template <bool UsePrelight>
inline void regenDispatchLights(const RegenContext &ctx, bool skinned, const SkinLayout *layout, const Vertex *src,
                                MeshGpuVertex *dst, std::size_t count)
{
    switch (ctx.lightCount)
    {
    case 0: regenFast<UsePrelight, 0>(ctx, skinned, layout, src, dst, count); break;
    case 1: regenFast<UsePrelight, 1>(ctx, skinned, layout, src, dst, count); break;
    case 2: regenFast<UsePrelight, 2>(ctx, skinned, layout, src, dst, count); break;
    default: regenFast<UsePrelight, 4>(ctx, skinned, layout, src, dst, count); break;
    }
}

// Writes colour and UV for every vertex and, when `skinned`, the posed position. Rigid parts keep the source
// positions already present in `dst`. `layout` (skinned parts ordered by orderSkinnedVertices) enables the
// per-bone run kernel.
inline void regenerateVertices(const RegenContext &ctx, bool skinned, const SkinLayout *layout, const Vertex *src,
                               MeshGpuVertex *dst, std::size_t count)
{
    if (ctx.usePrelight)
        regenDispatchLights<true>(ctx, skinned, layout, src, dst, count);
    else
        regenDispatchLights<false>(ctx, skinned, layout, src, dst, count);
}
#endif
} // namespace p3fes::native_renderer::detail
