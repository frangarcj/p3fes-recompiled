// Vita native renderer: the same GPU-neutral contract as the desktop proof,
// drawn through the vitaGL/GXM context already owned by the Celosia host shim.
#include "renderer.h"
#include "lighting.h"
#include "skinning.h"
#include "uv_transform.h"
#include "mesh_compaction.h"
#include "static_vertex_cache.h"
#include "mesh_regen.h"
#include "gpu_skin_batches.h"
#include "screen_batch.h"
#if defined(PS2X_P3FES_FRAME_TRACE)
#include "../../ps2xRuntime/src/lib/game_overrides/p3fes_frame_trace.h"
#endif

#include <raylib.h>
#include <vitaGL.h>
#if defined(__vita__)
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// The Celosia-derived Vita host shim serializes all vitaGL access through
// this recursive lock; mesh/texture uploads can happen outside BeginDrawing.
std::recursive_mutex &ps2xVitaGlMutex();

namespace p3fes::native_renderer
{
namespace
{
// Position, uv, colour: the CPU shading never needs the normal in the vertex buffer (see mesh_regen.h).
using GpuVertex = detail::MeshGpuVertex;

struct GpuSkinVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    std::uint8_t color[4];
    std::uint8_t bones[4];
    float weights[4];
};

struct GpuSkinPart
{
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLsizei indexCount = 0;
    std::vector<std::uint8_t> paletteBones;
};

bool resourceDiagEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_RESOURCE_DIAG");
        return value && *value == '1';
    }();
    return enabled;
}

// P3FES_MESH_FAST=0: scalar reference regeneration (skinning.h / lighting.h) instead of the NEON kernel.
// P3FES_MESH_REUSE=0: skinned parts regenerate every draw (the cache only serves unskinned parts).
bool meshFastRegenEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_FAST");
        return !(value && *value == '0');
    }();
    return enabled;
}

bool gpuSkinDbgRigidOnly()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_GPU_SKIN_DBG");
        return value && *value == '1';
    }();
    return enabled;
}

bool meshVerifyEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_VERIFY");
        return value && *value == '1';
    }();
    return enabled;
}

bool meshBenchEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_BENCH");
        return value && *value == '1';
    }();
    return enabled;
}

// P3FES_MESH_INSTANCES=0: one baked vertex buffer per mesh part, as before.
bool meshInstancesEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_INSTANCES");
        return !(value && *value == '0');
    }();
    return enabled;
}

bool meshReuseEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_REUSE");
        return !(value && *value == '0');
    }();
    return enabled;
}

// P3FES_MESH_GLCACHE=1: the mesh path remembers the GL state it set and skips redundant calls (each vitaGL
// enable / pointer / client-state call marks the fixed-function programs dirty, so a draw re-resolves them);
// the reset to the baseline state is deferred to the end of a run of consecutive mesh draws.
bool meshGlCacheEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_GLCACHE");
        return value && *value == '1';
    }();
    return enabled;
}

// P3FES_MESH_MERGE=1 (implies the GL cache): consecutive small unskinned parts that share every piece of draw
// state are baked in world space into one client-array draw. P3FES_MESH_MERGE_VERTS = largest part merged.
bool meshMergeEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_MERGE");
        return value && *value == '1';
    }();
    return enabled;
}

std::size_t meshMergeMaxVerts()
{
    static const std::size_t limit = [] {
        const char *value = std::getenv("P3FES_MESH_MERGE_VERTS");
        const long parsed = value ? std::strtol(value, nullptr, 10) : 256;
        return static_cast<std::size_t>(parsed > 0 ? std::min(parsed, 4096l) : 256l);
    }();
    return limit;
}

// P3FES_SCREEN_GLCACHE (default on, =0 restores the old per-draw state setting): the screen-space path sets only the GL
// state that differs from the previous draw and puts the baseline back once at the end of a run of screen draws instead
// of after every draw. Pixel-identical to the old path in the A/B frame comparison (P3FES_AB_FRAME) on the console and
// in Vita3K; the 2-D pass of the shop with its menu went from 8.9 to 5.6 ms of host thread.
bool screenGlCacheEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_SCREEN_GLCACHE");
        return !(value && *value == '0');
    }();
    return enabled;
}

// P3FES_MESH_MERGE_TWICE=0 keeps the parts drawn twice (alpha test with "frame buffer only" on failure) out of the merged
// batches. By default they join, as long as their screen boxes do not overlap (see tryMergePart).
bool meshMergeTwiceEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_MERGE_TWICE");
        return !(value && *value == '0');
    }();
    return enabled;
}

std::size_t meshMergeCacheBytes()
{
    static const std::size_t bytes = [] {
        const char *value = std::getenv("P3FES_MESH_MERGE_CACHE_KB");
        const long parsed = value ? std::strtol(value, nullptr, 10) : 6144;
        return static_cast<std::size_t>(std::max(parsed, 0l)) * 1024u;
    }();
    return bytes;
}

bool meshPhasesEnabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("P3FES_MESH_PHASES");
        return value && *value == '1';
    }();
    return enabled;
}

// ScreenVertex already has the client-array layout required by vitaGL.
// Keep frame-owned data intact and let glDrawArrays consume it directly.
static_assert(std::is_standard_layout_v<ScreenVertex> && sizeof(ScreenVertex) == 24u &&
              offsetof(ScreenVertex, x) == 0u && offsetof(ScreenVertex, u) == 12u &&
              offsetof(ScreenVertex, color) == 20u);

struct FitViewport
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

FitViewport fitViewport(float sourceWidth, float sourceHeight)
{
    const int hostWidth = std::max(GetScreenWidth(), 1);
    const int hostHeight = std::max(GetScreenHeight(), 1);
    if (!(sourceWidth > 0.0f) || !(sourceHeight > 0.0f))
        return {0, 0, hostWidth, hostHeight};

    const float scale = std::min(hostWidth / sourceWidth, hostHeight / sourceHeight);
    const int width = std::clamp(static_cast<int>(std::lround(sourceWidth * scale)), 1, hostWidth);
    const int height = std::clamp(static_cast<int>(std::lround(sourceHeight * scale)), 1, hostHeight);
    return {(hostWidth - width) / 2, (hostHeight - height) / 2, width, height};
}

// Memory the GPU reads while it is drawn from: vertex and index data that a draw call points at by address.
//
// vitaGL's client-array draws do not always copy what they are given. The release build of vitaGL is made with
// INDICES_DRAW_SPEEDHACK (indices are used where they stand) and DRAW_SPEEDHACK=2 (a vertex stream of more than
// 32 KB is used where it stands), and in any case sceGxm draws are only recorded: the GPU reads the data when the
// scene is rendered, after the frame is submitted and possibly while the next frame is being built. A draw's
// arrays must therefore stay unchanged until the GPU is done with the scene. This is a ring of kSlots frames; a
// slot is reused only kSlots frames later (vitaGL itself rests on the same assumption with its display buffer
// count: its circular pool has one slice per display buffer). Chunks are kept for reuse, so a steady scene
// allocates nothing.
class FrameArena
{
public:
    static constexpr std::size_t kSlots = 6;
    static constexpr std::size_t kChunkBytes = 256u * 1024u;

    FrameArena() = default;
    FrameArena(const FrameArena &) = delete;
    FrameArena &operator=(const FrameArena &) = delete;
    ~FrameArena()
    {
        for (Slot &slot : m_slots)
            for (Chunk &chunk : slot.chunks)
                std::free(chunk.raw);
    }

    // Starts the next frame's slot (the oldest one) empty.
    void nextFrame()
    {
        m_current = (m_current + 1u) % kSlots;
        Slot &slot = m_slots[m_current];
        slot.chunk = 0;
        slot.used = 0;
    }

    // 16-byte aligned, valid until kSlots - 1 more nextFrame() calls. Null when memory is exhausted.
    void *allocate(std::size_t bytes)
    {
        bytes = (bytes + 15u) & ~static_cast<std::size_t>(15u);
        Slot &slot = m_slots[m_current];
        while (slot.chunk < slot.chunks.size() && slot.chunks[slot.chunk].size - slot.used < bytes)
        {
            ++slot.chunk;
            slot.used = 0;
        }
        if (slot.chunk == slot.chunks.size())
        {
            const std::size_t size = std::max(kChunkBytes, bytes);
            void *raw = std::malloc(size + 15u);
            if (!raw)
                return nullptr;
            const std::uintptr_t aligned = (reinterpret_cast<std::uintptr_t>(raw) + 15u) & ~static_cast<std::uintptr_t>(15u);
            slot.chunks.push_back({static_cast<std::uint8_t *>(raw), reinterpret_cast<std::uint8_t *>(aligned), size});
            slot.used = 0;
        }
        Chunk &chunk = slot.chunks[slot.chunk];
        void *result = chunk.data + slot.used;
        slot.used += bytes;
        m_bytes += bytes;
        return result;
    }

    // Copies `bytes` of `source` into the arena.
    void *copy(const void *source, std::size_t bytes)
    {
        void *destination = allocate(bytes);
        if (destination)
            std::memcpy(destination, source, bytes);
        return destination;
    }

    std::uint64_t bytesAllocated() const { return m_bytes; }
    std::size_t bytesHeld() const
    {
        std::size_t total = 0;
        for (const Slot &slot : m_slots)
            for (const Chunk &chunk : slot.chunks)
                total += chunk.size;
        return total;
    }

private:
    struct Chunk
    {
        std::uint8_t *raw;  // what malloc returned
        std::uint8_t *data; // 16-byte aligned inside it
        std::size_t size;
    };
    struct Slot
    {
        std::vector<Chunk> chunks;
        std::size_t chunk = 0, used = 0;
    };
    Slot m_slots[kSlots];
    std::size_t m_current = 0;
    std::uint64_t m_bytes = 0;
};

// Bytes of DrawPart::mergeVerts held (P3FES_MESH_MERGE_CACHE_KB bounds them).
std::size_t g_mergeCacheBytes = 0;

struct DrawPart
{
    GLuint vertexBuffer = 0;
    GLuint indexBuffer = 0;
    GLsizei indexCount = 0;
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    std::string textureName;
    std::uint32_t materialIndex = 0;
    Material material{};
    std::vector<Vertex> sourceVertices;
    std::vector<GpuVertex> gpuVertices;
    std::vector<GpuSkinPart> gpuSkinParts;
    detail::StaticVertexCacheKey staticVertexCache;
    // One mesh is often drawn several times a frame with different model matrices (shop props).
    // The baked colours depend on the model matrix, so a single buffer per part was re-baked
    // for every copy, every frame. Other copies' buffers are parked here with their keys;
    // vertexBuffer / staticVertexCache above are the slot in use.
    struct Instance
    {
        GLuint vertexBuffer = 0;
        detail::StaticVertexCacheKey key;
        std::uint32_t usedFrame = 0;
    };
    std::vector<Instance> instances;
    std::uint32_t slotUsedFrame = 0;
    std::vector<std::uint16_t> cpuIndices; // small unskinned parts only: triangle list, for the merged draw (P3FES_MESH_MERGE)
    // The part's vertices baked in world space for the merged draw, with the inputs they were made from: a part that
    // is drawn again with the same inputs (props that do not move) is copied instead of lit and transformed again.
    detail::StaticVertexCacheKey mergeKey;
    std::vector<GpuVertex> mergeVerts;
    std::array<float, 6> bounds{}; // local axis-aligned box (min xyz, max xyz) of the vertices, for the screen-overlap test of two-pass merges
    bool hasBounds = false;
    std::vector<std::uint8_t> usedBones; // skinned: bones with a usable weight in this part
    detail::SkinLayout skinLayout;       // skinned: rigid runs per bone, then blended vertices (mesh_regen.h)
    bool lighting = false;
    bool hasPrelight = false;
    bool hasNormals = false;
    bool skinned = false;
};

struct SkinProgram
{
    GLuint program = 0;
    GLint mvp = -1, modelRows = -1, skinRows = -1, skinCount = -1;
    GLint uvRows = -1, materialColor = -1, materialTerms = -1, ambient = -1;
    GLint directionalCount = -1, directionalDirections = -1, directionalColors = -1;
    GLint localCount = -1, localPositions = -1, localDirections = -1, localCones = -1, localColors = -1;
    GLint sampler = -1, useTexture = -1, alphaEnabled = -1, alphaFunction = -1, alphaReference = -1;
    GLint fogEnabled = -1, fogRange = -1, fogColor = -1;
};

using Vec3 = std::array<float, 3>;

std::string textureKey(std::string_view name)
{
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return out;
}

GLenum rwWrapMode(MeshDrawState::SamplerState::Address address)
{
    switch (address)
    {
    case MeshDrawState::SamplerState::Address::Mirror: return GL_MIRRORED_REPEAT;
    case MeshDrawState::SamplerState::Address::Clamp: return GL_CLAMP_TO_EDGE;
    case MeshDrawState::SamplerState::Address::Wrap:
    default: return GL_REPEAT;
    }
}

struct ResolvedSampler
{
    GLenum wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    bool linear = true;
    bool operator==(const ResolvedSampler &) const = default;
};

ResolvedSampler resolveMeshSampler(const MeshDrawState &state, const MeshDrawState::SamplerState *sampler)
{
    const bool materialSampler = sampler && sampler->valid;
    ResolvedSampler out;
    out.wrapS = materialSampler ? rwWrapMode(sampler->u) : (state.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    out.wrapT = materialSampler ? rwWrapMode(sampler->v) : (state.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    out.linear = materialSampler ? sampler->linearFilter : state.linearFilter;
    return out;
}

void applyMeshSampler(GLuint texture, const MeshDrawState &state,
                      const MeshDrawState::SamplerState *sampler = nullptr)
{
    if (!texture)
        return;
    const ResolvedSampler resolved = resolveMeshSampler(state, sampler);
    const GLenum wrapS = resolved.wrapS, wrapT = resolved.wrapT;
    const bool linear = resolved.linear;
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
}

GLenum rwBlendFactor(std::uint8_t factor)
{
    switch (factor)
    {
    case 1u: return GL_ZERO;
    case 2u: return GL_ONE;
    case 3u: return GL_SRC_COLOR;
    case 4u: return GL_ONE_MINUS_SRC_COLOR;
    case 5u: return GL_SRC_ALPHA;
    case 6u: return GL_ONE_MINUS_SRC_ALPHA;
    case 7u: return GL_DST_ALPHA;
    case 8u: return GL_ONE_MINUS_DST_ALPHA;
    case 9u: return GL_DST_COLOR;
    case 10u: return GL_ONE_MINUS_DST_COLOR;
    case 11u: return GL_SRC_ALPHA_SATURATE;
    default: return GL_ONE;
    }
}

GLenum gsAlphaFunc(std::uint8_t function)
{
    switch (function & 0x7u)
    {
    case 0u: return GL_NEVER;
    case 1u: return GL_ALWAYS;
    case 2u: return GL_LESS;
    case 3u: return GL_LEQUAL;
    case 4u: return GL_EQUAL;
    case 5u: return GL_GEQUAL;
    case 6u: return GL_GREATER;
    case 7u: return GL_NOTEQUAL;
    default: return GL_ALWAYS;
    }
}

void applyAlphaTest(const AlphaTestState &alpha)
{
    if (!alpha.enabled)
    {
        glDisable(GL_ALPHA_TEST);
        return;
    }
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(gsAlphaFunc(alpha.function), alpha.reference / 255.0f);
}

void applyFog(const FogState &fog)
{
    if (!fog.enabled)
    {
        glDisable(GL_FOG);
        return;
    }
    const GLfloat color[4]{fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f, 1.0f};
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, fog.start);
    glFogf(GL_FOG_END, fog.end);
    glFogfv(GL_FOG_COLOR, color);
}

void applyShadeMode(bool gouraud)
{
    glShadeModel(gouraud ? GL_SMOOTH : GL_FLAT);
}

Vec3 subtract(const Vec3 &a, const Vec3 &b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

float dot(const Vec3 &a, const Vec3 &b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3 cross(const Vec3 &a, const Vec3 &b)
{
    return {
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    };
}

Vec3 normalized(const Vec3 &v)
{
    const float length = std::sqrt(dot(v, v));
    if (!(length > 1.0e-8f))
        return {0.0f, 0.0f, -1.0f};
    return {v[0] / length, v[1] / length, v[2] / length};
}

const char *kGpuSkinVertexCg = R"(
float3 safeNormalize(float3 v, float3 fallback)
{
    float n2 = dot(v, v);
    return n2 > 1.0e-16 ? v * rsqrt(n2) : fallback;
}
void main(float3 aPos, float3 aNormal, float2 aTex,
          float4 aColor, float4 aBones, float4 aWeights,
          uniform float4 uMVP[4], uniform float4 uModelRows[3],
          uniform float4 uSkinRows[72], uniform int uSkinCount,
          uniform float4 uUvRows[2], uniform float4 uMaterialColor,
          uniform float4 uMaterialTerms, uniform float4 uAmbient,
          uniform int uDirectionalCount, uniform float4 uDirectionalDirections[4],
          uniform float4 uDirectionalColors[4], uniform int uLocalCount,
          uniform float4 uLocalPositions[4], uniform float4 uLocalDirections[4],
          uniform float4 uLocalCones[4], uniform float4 uLocalColors[4],
          float4 out oPosition : POSITION, float2 out vTex : TEXCOORD0,
          float4 out vColor : COLOR)
{
    float3 p = aPos;
    float3 n = aNormal;
    float weightSum = 0.0;
    float3 skinnedPosition = float3(0.0, 0.0, 0.0);
    float3 skinnedNormal = float3(0.0, 0.0, 0.0);
@@SKIN_LOOP@@
    if (weightSum > 1.0e-8)
    {
        p = skinnedPosition / weightSum;
        if (dot(skinnedNormal, skinnedNormal) > 1.0e-16)
            n = safeNormalize(skinnedNormal, n);
    }

    float4 p4 = float4(p, 1.0);
    float4 clip;
    clip.x = dot(uMVP[0], p4);
    clip.y = dot(uMVP[1], p4);
    clip.z = dot(uMVP[2], p4);
    clip.w = dot(uMVP[3], p4);
    oPosition = clip;

    float2 uv;
    uv.x = dot(uUvRows[0].xy, aTex) + uUvRows[0].z;
    uv.y = dot(uUvRows[1].xy, aTex) + uUvRows[1].z;
    vTex = uv;

    float3 transformedNormal = float3(dot(uModelRows[0].xyz, n),
                                     dot(uModelRows[1].xyz, n),
                                     dot(uModelRows[2].xyz, n));
    float3 worldNormal = dot(transformedNormal, transformedNormal) > 1.0e-16
        ? safeNormalize(transformedNormal, transformedNormal) : transformedNormal;
    float3 worldPos = float3(dot(uModelRows[0], p4),
                             dot(uModelRows[1], p4),
                             dot(uModelRows[2], p4));
    float3 rgb = float3(0.0, 0.0, 0.0);
    if (uDirectionalCount >= 0)
    {
        rgb += aColor.rgb * uAmbient.w;
        rgb += uAmbient.rgb;
        for (int light = 0; light < 4; ++light)
        {
            if (light < uDirectionalCount)
            {
                float diffuse = max(0.0, -dot(worldNormal, uDirectionalDirections[light].xyz));
                rgb += diffuse * uDirectionalColors[light].rgb * uMaterialTerms.y;
            }
        }
        for (int light = 0; light < 4; ++light)
        {
            if (light < uLocalCount)
            {
                float3 delta = worldPos - uLocalPositions[light].xyz;
                float distance = length(delta);
                float radius = uLocalPositions[light].w;
                if (distance > 1.0e-8 && radius > 1.0e-8)
                {
                    float3 direction = delta / distance;
                    float attenuation = max(0.0, 1.0 - distance / radius);
                    float diffuse = max(0.0, -dot(worldNormal, direction));
                    float kind = uLocalDirections[light].w;
                    if (kind > 0.5)
                    {
                        float coneCos = -uLocalCones[light].x;
                        float denom = 1.0 - coneCos;
                        float pointCos = dot(direction, uLocalDirections[light].xyz);
                        float falloff = abs(denom) > 1.0e-6 ? (pointCos - coneCos) / denom : 0.0;
                        if (falloff < 0.0)
                            diffuse = 0.0;
                        float hardSpot = kind < 1.5 ? 1.0 : 0.0;
                        diffuse *= max(falloff, hardSpot);
                    }
                    rgb += diffuse * uLocalColors[light].rgb * attenuation * uMaterialTerms.y;
                }
            }
        }
    }
    else
        rgb = aColor.rgb;

    float3 materialRgb = floor(saturate(rgb) * uMaterialColor.rgb * 255.0 + 0.5) / 255.0;
    float materialAlpha = floor(aColor.a * uMaterialColor.a * 255.0 + 0.5) / 255.0;
    vColor = float4(materialRgb, materialAlpha);
}
)";

const char *kGpuSkinFragmentCg = R"(
float4 main(float2 vTex : TEXCOORD0, float4 vColor : COLOR, float4 coords : WPOS,
            uniform sampler2D uTexture, uniform int uUseTexture,
            uniform int uAlphaEnabled, uniform int uAlphaFunction, uniform float uAlphaReference,
            uniform int uFogEnabled, uniform float2 uFogRange, uniform float4 uFogColor) : COLOR
{
    float4 color = vColor;
    if (uUseTexture != 0)
        color *= tex2D(uTexture, vTex);
    if (uAlphaEnabled != 0)
    {
        float alpha = color.a;
        float reference = uAlphaReference;
        bool passes = uAlphaFunction == 0 ? false :
                    uAlphaFunction == 1 ? true :
                    uAlphaFunction == 2 ? alpha < reference :
                    uAlphaFunction == 3 ? alpha <= reference :
                    uAlphaFunction == 4 ? alpha == reference :
                    uAlphaFunction == 5 ? alpha >= reference :
                    uAlphaFunction == 6 ? alpha > reference : alpha != reference;
        if (!passes)
            discard;
    }
    if (uFogEnabled != 0)
    {
        float range = uFogRange.y - uFogRange.x;
        float fogDistance = coords.z / coords.w;
        float amount = abs(range) > 1.0e-8 ? saturate((uFogRange.y - fogDistance) / range) : 0.0;
        color.rgb = lerp(uFogColor.rgb, color.rgb, amount);
    }
    return color;
}
)";

// P3FES_GPU_SKIN_DBG=2 unrolls the four influences with constant component selectors instead of indexing the
// attribute vectors with the loop counter (bisecting the garbled GPU skinning).
std::string gpuSkinVertexSource()
{
    std::string source = kGpuSkinVertexCg;
    const char *loop = R"(    for (int influence = 0; influence < 4; ++influence)
    {
        float weight = aWeights[influence];
        int bone = (int)aBones[influence];
        if (weight > 0.0 && bone >= 0 && bone < uSkinCount)
        {
            int base = bone * 3;
            float4 p4 = float4(p, 1.0);
            skinnedPosition.x += dot(uSkinRows[base], p4) * weight;
            skinnedPosition.y += dot(uSkinRows[base + 1], p4) * weight;
            skinnedPosition.z += dot(uSkinRows[base + 2], p4) * weight;
            float4 n4 = float4(n, 0.0);
            skinnedNormal.x += dot(uSkinRows[base], n4) * weight;
            skinnedNormal.y += dot(uSkinRows[base + 1], n4) * weight;
            skinnedNormal.z += dot(uSkinRows[base + 2], n4) * weight;
            weightSum += weight;
        }
    }
)";
    std::string body = loop;
    const char *dbg = std::getenv("P3FES_GPU_SKIN_DBG");
    if (dbg && *dbg == '2')
    {
        body.clear();
        for (const char *c : {"x", "y", "z", "w"})
        {
            body += std::string("    {\n        float weight = aWeights.") + c + ";\n        int bone = (int)(aBones." + c +
                    ");\n        if (weight > 0.0 && bone >= 0 && bone < uSkinCount)\n        {\n"
                    "            int base = bone * 3;\n            float4 p4 = float4(p, 1.0);\n"
                    "            skinnedPosition.x += dot(uSkinRows[base], p4) * weight;\n"
                    "            skinnedPosition.y += dot(uSkinRows[base + 1], p4) * weight;\n"
                    "            skinnedPosition.z += dot(uSkinRows[base + 2], p4) * weight;\n"
                    "            float4 n4 = float4(n, 0.0);\n"
                    "            skinnedNormal.x += dot(uSkinRows[base], n4) * weight;\n"
                    "            skinnedNormal.y += dot(uSkinRows[base + 1], n4) * weight;\n"
                    "            skinnedNormal.z += dot(uSkinRows[base + 2], n4) * weight;\n"
                    "            weightSum += weight;\n        }\n    }\n";
        }
    }
    const std::string marker = "@@SKIN_LOOP@@\n";
    source.replace(source.find(marker), marker.size(), body);
    return source;
}

GLuint compileCg(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[2048]{};
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        std::fprintf(stderr, "[native-skin] shader compile failed: %.*s\n", static_cast<int>(length), log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool initializeSkinProgram(SkinProgram &skin)
{
    const std::string vertexSource = gpuSkinVertexSource();
    const GLuint vertex = compileCg(GL_CG_VERTEX_SHADER_EXT, vertexSource.c_str());
    const GLuint fragment = compileCg(GL_CG_FRAGMENT_SHADER_EXT, kGpuSkinFragmentCg);
    if (!vertex || !fragment)
    {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aTex");
    glBindAttribLocation(program, 2, "aColor");
    glBindAttribLocation(program, 3, "aBones");
    glBindAttribLocation(program, 4, "aWeights");
    glBindAttribLocation(program, 5, "aNormal");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        char log[2048]{};
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        std::fprintf(stderr, "[native-skin] program link failed: %.*s\n", static_cast<int>(length), log);
        glDeleteProgram(program);
        return false;
    }
    skin.program = program;
    const auto uniformLocation = [program](const char *name) {
        GLint location = glGetUniformLocation(program, name);
        if (location < 0)
        {
            std::string base(name);
            const std::size_t suffix = base.find("[0]");
            if (suffix != std::string::npos)
                location = glGetUniformLocation(program, base.substr(0, suffix).c_str());
        }
        return location;
    };
#define SKIN_LOC(field, name) skin.field = uniformLocation(name)
    SKIN_LOC(mvp, "uMVP[0]"); SKIN_LOC(modelRows, "uModelRows[0]"); SKIN_LOC(skinRows, "uSkinRows[0]");
    SKIN_LOC(skinCount, "uSkinCount"); SKIN_LOC(uvRows, "uUvRows[0]");
    SKIN_LOC(materialColor, "uMaterialColor"); SKIN_LOC(materialTerms, "uMaterialTerms"); SKIN_LOC(ambient, "uAmbient");
    SKIN_LOC(directionalCount, "uDirectionalCount"); SKIN_LOC(directionalDirections, "uDirectionalDirections[0]");
    SKIN_LOC(directionalColors, "uDirectionalColors[0]"); SKIN_LOC(localCount, "uLocalCount");
    SKIN_LOC(localPositions, "uLocalPositions[0]"); SKIN_LOC(localDirections, "uLocalDirections[0]");
    SKIN_LOC(localCones, "uLocalCones[0]");
    SKIN_LOC(localColors, "uLocalColors[0]"); SKIN_LOC(sampler, "uTexture"); SKIN_LOC(useTexture, "uUseTexture");
    SKIN_LOC(alphaEnabled, "uAlphaEnabled"); SKIN_LOC(alphaFunction, "uAlphaFunction");
    SKIN_LOC(alphaReference, "uAlphaReference"); SKIN_LOC(fogEnabled, "uFogEnabled");
    SKIN_LOC(fogRange, "uFogRange"); SKIN_LOC(fogColor, "uFogColor");
#undef SKIN_LOC
    const GLint required[] = {skin.mvp, skin.modelRows, skin.skinRows, skin.skinCount, skin.uvRows,
                              skin.materialColor, skin.materialTerms, skin.ambient,
                              skin.directionalCount, skin.directionalDirections, skin.directionalColors,
                              skin.localCount, skin.localPositions, skin.localDirections, skin.localCones,
                              skin.localColors, skin.sampler, skin.useTexture, skin.alphaEnabled,
                              skin.alphaFunction, skin.alphaReference, skin.fogEnabled, skin.fogRange, skin.fogColor};
    for (GLint location : required)
        if (location < 0)
        {
            std::fprintf(stderr, "[native-skin] required shader uniform was optimized out\n");
            glDeleteProgram(program);
            skin = {};
            return false;
        }
    return true;
}

std::array<float, 16> lookAt(const Camera &camera)
{
    const Vec3 eye = camera.position;
    const Vec3 forward = normalized(subtract(camera.target, eye));
    Vec3 side = normalized(cross(forward, camera.up));
    if (!(dot(side, side) > 0.5f))
        side = {1.0f, 0.0f, 0.0f};
    const Vec3 up = cross(side, forward);

    // Column-major OpenGL view matrix.
    return {
        side[0], up[0], -forward[0], 0.0f,
        side[1], up[1], -forward[1], 0.0f,
        side[2], up[2], -forward[2], 0.0f,
        -dot(side, eye), -dot(up, eye), dot(forward, eye), 1.0f,
    };
}

bool appendIndex(std::vector<std::uint16_t> &out, std::uint32_t index, std::string &error)
{
    if (index > std::numeric_limits<std::uint16_t>::max())
    {
        error = "Vita backend currently supports 16-bit mesh indices";
        return false;
    }
    out.push_back(static_cast<std::uint16_t>(index));
    return true;
}

std::vector<std::uint16_t> triangleList(const MeshData &mesh, const Submesh &submesh, std::string &error)
{
    const std::size_t begin = submesh.firstIndex;
    if (begin > mesh.indices.size() || submesh.indexCount > mesh.indices.size() - begin)
    {
        error = "submesh index range is outside the mesh index buffer";
        return {};
    }
    const std::size_t end = begin + submesh.indexCount;
    std::vector<std::uint16_t> out;
    if (!mesh.triangleStrip)
    {
        out.reserve(submesh.indexCount);
        for (std::size_t i = begin; i < end; ++i)
            if (!appendIndex(out, mesh.indices[i], error))
                return {};
        return out;
    }

    for (std::size_t i = begin + 2; i < end; ++i)
    {
        std::uint32_t a = mesh.indices[i - 2];
        std::uint32_t b = mesh.indices[i - 1];
        const std::uint32_t c = mesh.indices[i];
        if (a == b || b == c || a == c)
            continue;
        if (((i - begin) & 1u) != 0u)
            std::swap(a, b);
        if (!appendIndex(out, a, error) || !appendIndex(out, b, error) || !appendIndex(out, c, error))
            return {};
    }
    return out;
}

void deleteParts(std::vector<DrawPart> &parts)
{
    for (DrawPart &part : parts)
    {
        for (GpuSkinPart &skin : part.gpuSkinParts)
        {
            if (skin.vertexBuffer)
                glDeleteBuffers(1, &skin.vertexBuffer);
            if (skin.indexBuffer)
                glDeleteBuffers(1, &skin.indexBuffer);
        }
        g_mergeCacheBytes -= std::min(g_mergeCacheBytes, part.mergeVerts.capacity() * sizeof(GpuVertex));
        if (part.vertexBuffer)
            glDeleteBuffers(1, &part.vertexBuffer);
        for (DrawPart::Instance &instance : part.instances)
            if (instance.vertexBuffer)
                glDeleteBuffers(1, &instance.vertexBuffer);
        if (part.indexBuffer)
            glDeleteBuffers(1, &part.indexBuffer);
    }
    parts.clear();
}

class VitaGlRenderer final : public Renderer
{
public:
    ~VitaGlRenderer() override { close(); }

    bool open(int width, int height, const char *title, std::string &error) override
    {
        if (m_open)
            return true;
        m_merge.enabled = meshMergeEnabled();
        m_gl.on = meshGlCacheEnabled() || m_merge.enabled;
        m_g2.on = screenGlCacheEnabled();
        // In the Vita runtime vitaGL is already initialized by the host shim.
        // Borrow that one global context; only the standalone viewer creates
        // and later destroys its own window/context.
        if (!IsWindowReady())
        {
            SetConfigFlags(FLAG_VSYNC_HINT);
            InitWindow(width, height, title);
            m_ownsWindow = true;
        }
        if (!IsWindowReady())
        {
            error = "vitaGL host window did not initialize";
            return false;
        }
        // GPU skinning + lighting (default on; P3FES_GPU_SKIN=0 keeps the CPU path). It was garbled until the
        // bone index was read as (int)(aBones + 0.5): the runtime shader compiler runs with fast integer math, whose
        // float->int conversion rounds to nearest, so the +0.5 moved every bone index to a neighbour.
        const char *gpuSkin = std::getenv("P3FES_GPU_SKIN");
        if (!(gpuSkin && *gpuSkin == '0'))
        {
            std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
            GLint uniformVectors = 0, vertexAttributes = 0;
            glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &uniformVectors);
            glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &vertexAttributes);
            std::fprintf(stderr, "[native-skin] limits vertex-uniform-vec4=%d attributes=%d batch-bones=24\n",
                         uniformVectors, vertexAttributes);
            // Budget includes 72 palette rows and the remaining camera,
            // material and four directional/four local light parameters.
            if (uniformVectors >= 111 && vertexAttributes >= 6)
                m_gpuSkinEnabled = initializeSkinProgram(m_skinProgram);
            else
                std::fprintf(stderr, "[native-skin] insufficient shader limits; keeping CPU skinning\n");
            if (!m_gpuSkinEnabled)
                std::fprintf(stderr, "[native-skin] GPU route unavailable; keeping CPU skinning\n");
        }
        m_open = true;
        return true;
    }

    void close() override
    {
        if (!m_open)
            return;
        reportSkinStats();
        {
            std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
            for (auto &[id, parts] : m_meshes)
                deleteParts(parts);
            m_meshes.clear();
            for (auto &[name, target] : m_targets)
                if (target.framebuffer)
                    glDeleteFramebuffers(1, &target.framebuffer);
            m_targets.clear();
            m_target = nullptr;
            flushScreenBatch();
            for (const GLuint page : m_atlasPages)
                glDeleteTextures(1, &page);
            m_atlasPages.clear();
            m_atlas.clear();
            m_smallTextures.clear();
            for (auto &[name, texture] : m_textures)
                if (texture)
                    glDeleteTextures(1, &texture);
            m_textures.clear();
            m_textureSizes.clear();
            if (m_skinProgram.program)
                glDeleteProgram(m_skinProgram.program);
            m_skinProgram = {};
            m_gpuSkinEnabled = false;
            m_gpuSkinReportFrames = 0;
            m_gpuSkinDraws = 0;
            m_gpuSkinCpuFallbacks = 0;
            m_gpuSkinFallbackReasons.fill(0u);
        }
        if (m_ownsWindow)
            CloseWindow();
        m_ownsWindow = false;
        m_open = false;
    }

    bool shouldClose() const override { return !m_open || WindowShouldClose(); }
    void setTargetFps(int fps) override { SetTargetFPS(fps); }

    bool uploadTexture(const TextureData &source, std::string &error) override
    {
        if (!m_open)
        {
            error = "renderer is not open";
            return false;
        }
        const std::uint64_t expected = static_cast<std::uint64_t>(source.width) * source.height * 4u;
        if (source.name.empty() || source.width == 0u || source.height == 0u || expected != source.rgba.size())
        {
            error = "invalid RGBA texture payload";
            return false;
        }

        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        flushScreenBatch(); // pending quads sample the old pixels
        const std::string key = textureKey(source.name);
        if (m_targets.find(key) != m_targets.end())
            return true; // the name is a render target: its pixels come from beginTextureTarget passes
        const auto existing = m_textures.find(key);
        const auto size = m_textureSizes.find(key);
        if (existing != m_textures.end() && size != m_textureSizes.end() &&
            size->second[0] == source.width && size->second[1] == source.height)
        {
            glBindTexture(GL_TEXTURE_2D, existing->second);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(source.width),
                            static_cast<GLsizei>(source.height), GL_RGBA, GL_UNSIGNED_BYTE, source.rgba.data());
            storeSmallTexture(key, source);
            return true;
        }
        removeTextureLocked(source.name);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(source.width), static_cast<GLsizei>(source.height),
                     0, GL_RGBA, GL_UNSIGNED_BYTE, source.rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        if (!texture)
        {
            error = "vitaGL failed to allocate texture";
            return false;
        }
        m_textures.emplace(key, texture);
        m_textureSizes.emplace(key, std::array<std::uint32_t, 2>{source.width, source.height});
        storeSmallTexture(key, source);
        return true;
    }

    void removeTexture(std::string_view name) override
    {
        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        removeTextureLocked(name);
    }

    bool uploadMesh(MeshId id, const MeshData &source, std::string &error) override
    {
        leaveMesh();
        end2D();
        if (!m_open)
        {
            error = "renderer is not open";
            return false;
        }
        if (source.vertices.empty())
        {
            error = "mesh has no vertices";
            return false;
        }

        std::vector<Submesh> submeshes = source.submeshes;
        if (submeshes.empty())
            submeshes.push_back({0u, static_cast<std::uint32_t>(source.indices.size()), 0u});

        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        removeMeshLocked(id);
        std::vector<DrawPart> parts;
        parts.reserve(submeshes.size());
        for (const Submesh &submesh : submeshes)
        {
            if (!source.materials.empty() && submesh.materialIndex >= source.materials.size())
            {
                error = "submesh material index is outside the material list";
                deleteParts(parts);
                return false;
            }
            std::vector<std::uint16_t> indices = triangleList(source, submesh, error);
            if (indices.empty())
            {
                if (!error.empty())
                {
                    deleteParts(parts);
                    return false;
                }
                continue;
            }

            DrawPart part;
            part.materialIndex = submesh.materialIndex;
            std::vector<std::uint16_t> compactIndices;
            const auto compactResult = detail::compactIndexedVertices(
                std::span<const Vertex>(source.vertices.data(), source.vertices.size()),
                std::span<const std::uint16_t>(indices.data(), indices.size()),
                part.sourceVertices,
                compactIndices);
            if (compactResult != detail::MeshCompactionResult::Success)
            {
                error = compactResult == detail::MeshCompactionResult::IndexOutOfRange
                            ? "submesh index is outside the source vertex list"
                            : "Vita backend currently supports at most 65,536 unique mesh vertices per submesh";
                deleteParts(parts);
                return false;
            }
            indices.swap(compactIndices);
            part.lighting = source.lighting && source.hasNormals;
            part.hasPrelight = source.hasPrelight;
            part.hasNormals = source.hasNormals;
            part.skinned = source.skinned;
            if (!source.materials.empty())
            {
                part.color = source.materials[submesh.materialIndex].color;
                part.textureName = source.materials[submesh.materialIndex].textureName;
                part.material = source.materials[submesh.materialIndex];
            }
            else
                part.material.color = part.color;
            if (part.skinned && meshFastRegenEnabled())
                detail::orderSkinnedVertices(part.sourceVertices, indices, part.skinLayout);
            std::vector<GpuVertex> vertices(part.sourceVertices.size());
            for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
            {
                const Vertex &in = part.sourceVertices[i];
                GpuVertex &out = vertices[i];
                std::copy(in.position.begin(), in.position.end(), out.position);
                std::copy(in.texCoord.begin(), in.texCoord.end(), out.uv);
                std::copy(in.color.begin(), in.color.end(), out.color);
            }
            part.gpuVertices = vertices;
            if (part.skinned)
            {
                std::array<bool, 256> used{};
                for (const Vertex &in : part.sourceVertices)
                    for (std::size_t k = 0; k < in.weights.size(); ++k)
                        if (in.weights[k] > 0.0f)
                            used[in.bones[k]] = true;
                for (std::size_t b = 0; b < used.size(); ++b)
                    if (used[b])
                        part.usedBones.push_back(static_cast<std::uint8_t>(b));
            }

            if (m_gpuSkinEnabled && part.skinned)
            {
                std::vector<detail::GpuSkinBatch> batches;
                const auto batchResult = detail::buildGpuSkinBatches(
                    part.sourceVertices, indices, source.boneCount, 24u, batches);
                if (batchResult == detail::SkinBatchResult::Success)
                {
                    for (detail::GpuSkinBatch &batch : batches)
                    {
                        GpuSkinPart skinPart;
                        skinPart.paletteBones = std::move(batch.paletteBones);
                        std::vector<GpuSkinVertex> skinVertices(batch.vertices.size());
                        for (std::size_t i = 0; i < batch.vertices.size(); ++i)
                        {
                            const Vertex &in = batch.vertices[i];
                            GpuSkinVertex &out = skinVertices[i];
                            std::copy(in.position.begin(), in.position.end(), out.position);
                            std::copy(in.normal.begin(), in.normal.end(), out.normal);
                            std::copy(in.texCoord.begin(), in.texCoord.end(), out.uv);
                            std::copy(in.color.begin(), in.color.end(), out.color);
                            std::copy(in.bones.begin(), in.bones.end(), out.bones);
                            std::copy(in.weights.begin(), in.weights.end(), out.weights);
                        }
                        glGenBuffers(1, &skinPart.vertexBuffer);
                        glBindBuffer(GL_ARRAY_BUFFER, skinPart.vertexBuffer);
                        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(skinVertices.size() * sizeof(GpuSkinVertex)),
                                     skinVertices.data(), GL_STATIC_DRAW);
                        glGenBuffers(1, &skinPart.indexBuffer);
                        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, skinPart.indexBuffer);
                        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                                     static_cast<GLsizei>(batch.indices.size() * sizeof(std::uint16_t)),
                                     batch.indices.data(), GL_STATIC_DRAW);
                        skinPart.indexCount = static_cast<GLsizei>(batch.indices.size());
                        if (!skinPart.vertexBuffer || !skinPart.indexBuffer)
                        {
                            if (skinPart.vertexBuffer) glDeleteBuffers(1, &skinPart.vertexBuffer);
                            if (skinPart.indexBuffer) glDeleteBuffers(1, &skinPart.indexBuffer);
                            for (GpuSkinPart &made : part.gpuSkinParts)
                            {
                                if (made.vertexBuffer) glDeleteBuffers(1, &made.vertexBuffer);
                                if (made.indexBuffer) glDeleteBuffers(1, &made.indexBuffer);
                            }
                            part.gpuSkinParts.clear();
                            break;
                        }
                        part.gpuSkinParts.push_back(std::move(skinPart));
                    }
                }
            }

            glGenBuffers(1, &part.vertexBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, part.vertexBuffer);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(vertices.size() * sizeof(GpuVertex)),
                         vertices.data(), GL_STATIC_DRAW);
            glGenBuffers(1, &part.indexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.indexBuffer);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizei>(indices.size() * sizeof(std::uint16_t)),
                         indices.data(), GL_STATIC_DRAW);
            part.indexCount = static_cast<GLsizei>(indices.size());
            if ((meshMergeEnabled() || std::getenv("P3FES_AB_FRAME")) && !part.skinned && part.sourceVertices.size() <= meshMergeMaxVerts())
            {
                part.cpuIndices = indices;
                part.bounds = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
                for (const Vertex &v : part.sourceVertices)
                    for (int c = 0; c < 3; ++c)
                    {
                        part.bounds[static_cast<std::size_t>(c)] = std::min(part.bounds[static_cast<std::size_t>(c)], v.position[static_cast<std::size_t>(c)]);
                        part.bounds[static_cast<std::size_t>(c) + 3u] = std::max(part.bounds[static_cast<std::size_t>(c) + 3u], v.position[static_cast<std::size_t>(c)]);
                    }
                part.hasBounds = !part.sourceVertices.empty();
            }
            if (!part.vertexBuffer || !part.indexBuffer)
            {
                error = "vitaGL failed to allocate mesh buffers";
                for (GpuSkinPart &skinPart : part.gpuSkinParts)
                {
                    if (skinPart.vertexBuffer) glDeleteBuffers(1, &skinPart.vertexBuffer);
                    if (skinPart.indexBuffer) glDeleteBuffers(1, &skinPart.indexBuffer);
                }
                if (part.vertexBuffer)
                    glDeleteBuffers(1, &part.vertexBuffer);
                if (part.indexBuffer)
                    glDeleteBuffers(1, &part.indexBuffer);
                deleteParts(parts);
                return false;
            }
            parts.push_back(std::move(part));
        }

        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (parts.empty())
        {
            error = "mesh has no drawable triangles";
            return false;
        }
        m_meshes.emplace(id, std::move(parts));
        if (source.skinned && resourceDiagEnabled())
        {
            std::array<std::uint32_t, 5> influenceCounts{};
            std::uint32_t singleUnitWeight0 = 0;
            for (const Vertex &vertex : source.vertices)
            {
                std::uint32_t active = 0;
                for (std::size_t i = 0; i < vertex.weights.size(); ++i)
                {
                    const float weight = vertex.weights[i];
                    if (weight > 0.0f && std::isfinite(weight) && vertex.bones[i] < source.boneCount)
                        ++active;
                }
                if (active <= 4)
                    ++influenceCounts[active];

                bool unitSingle = vertex.weights[0] == 1.0f && vertex.bones[0] < source.boneCount;
                for (std::size_t i = 1; unitSingle && i < vertex.weights.size(); ++i)
                {
                    const float weight = vertex.weights[i];
                    if (weight > 0.0f && std::isfinite(weight) && vertex.bones[i] < source.boneCount)
                        unitSingle = false;
                }
                if (unitSingle)
                    ++singleUnitWeight0;
            }
            std::fprintf(stderr,
                         "[native-skin] mesh=%llu vertices=%u influences0=%u 1=%u 2=%u 3=%u 4=%u fast-w0-unit=%u\n",
                         static_cast<unsigned long long>(id), static_cast<unsigned>(source.vertices.size()),
                         influenceCounts[0], influenceCounts[1], influenceCounts[2], influenceCounts[3],
                         influenceCounts[4], singleUnitWeight0);
        }
        return true;
    }

    void removeMesh(MeshId id) override
    {
        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        removeMeshLocked(id);
    }

    void beginTextureTarget(std::string_view name, int width, int height) override
    {
        if (!m_open || width <= 0 || height <= 0)
            return;
        flushScreenBatch();
        if (m_target)
            endTextureTarget();
        // Held until endTextureTarget, like BeginDrawing/EndDrawing hold it for the display frame.
        ps2xVitaGlMutex().lock();
        const std::string key = textureKey(name);
        auto target = m_targets.find(key);
        if (target == m_targets.end() || target->second.width != width || target->second.height != height)
        {
            removeTextureLocked(name); // a placeholder upload or a target of another size
            Target created;
            created.width = width;
            created.height = height;
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glGenFramebuffers(1, &created.framebuffer);
            glBindFramebuffer(GL_FRAMEBUFFER, created.framebuffer);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            glBindTexture(GL_TEXTURE_2D, 0);
            target = m_targets.emplace(key, created).first;
            m_textures[key] = texture;
            m_textureSizes[key] = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
            std::fprintf(stderr, "[p3fes-native] render target %s %dx%d fbo=%u texture=%u\n", key.c_str(), width, height,
                         created.framebuffer, texture);
        }
        m_target = &target->second;
        glBindFramebuffer(GL_FRAMEBUFFER, m_target->framebuffer);
        // The pass is rendered upside down (see setCamera), which mirrors the winding.
        glFrontFace(targetFlipEnabled() ? GL_CW : GL_CCW);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, width, height);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        m_in3D = false;
    }

    void endTextureTarget() override
    {
        flushScreenBatch();
        if (!m_target)
            return;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFrontFace(GL_CCW);
        m_target = nullptr;
        m_in3D = false;
        ps2xVitaGlMutex().unlock();
    }

    void setWidescreen(int mode) override
    {
        flushScreenBatch();
        m_widescreen = mode;
    }
    void setScreenPlacement(ScreenPlacement placement) override { m_screenPlacement = placement; }
    bool setWorldPlacement(WorldPlacement placement) override
    {
        const bool supported = m_widescreen == 2 && !m_target;
        flushScreenBatch();
        m_worldPlacement = supported ? placement : WorldPlacement::Full;
        if (m_in3D)
            apply3D();
        return supported;
    }

    void beginFrame(const Camera &camera, ClearColor clear) override
    {
        leaveMesh();
        ++m_instanceFrame;
        m_arena.nextFrame();
        m_screenZLayers.clear();
        m_camera = camera;
        BeginDrawing(); // holds ps2xVitaGlMutex until EndDrawing
        const int hostWidth = std::max(GetScreenWidth(), 1);
        const int hostHeight = std::max(GetScreenHeight(), 1);
        const FitViewport viewport = worldViewport();
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, hostWidth, hostHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(viewport.x, viewport.y, viewport.width, viewport.height);
        glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE); // RenderWare materials may be two-sided.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        apply3D();
    }

    void clear(ClearColor clear, bool colorBuffer, bool depthBuffer) override
    {
        if (!colorBuffer && !depthBuffer)
            return;
        flushScreenBatch();
        const FitViewport viewport = worldViewport();
        glEnable(GL_SCISSOR_TEST);
        glScissor(viewport.x, viewport.y, viewport.width, viewport.height);
        GLbitfield mask = 0u;
        if (colorBuffer)
        {
            glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (depthBuffer)
        {
            glDepthMask(GL_TRUE);
            glClearDepthf(1.0f);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        glClear(mask);
        glDisable(GL_SCISSOR_TEST);
        glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        applyWorldScissor();
    }

    void setCamera(const Camera &camera) override
    {
        leaveMesh(); // a pending merged batch is drawn with the old camera
        m_camera = camera;
        // A colour attachment stores its bottom row first, while the game samples its camera
        // textures like any other raster (v = 0 is the top row): render target passes upside down.
        if (m_target && targetFlipEnabled())
            for (const int element : {1, 5, 9, 13})
                m_camera.projection[element] = -m_camera.projection[element];
        // A camera transition invalidates the loaded world matrices, but has
        // no framebuffer effect. apply3D() reloads them on the next mesh.
        m_in3D = false;
    }

    void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state) override
    {
        if (!resourceDiagEnabled())
        {
            drawMeshImpl(id, model, state);
            return;
        }
        const std::uint64_t t0 = diagNowUs();
        drawMeshImpl(id, model, state);
        const std::uint64_t dt = diagNowUs() - t0;
        MeshTiming &t = m_target ? m_meshTimingTarget : m_meshTiming;
        t.us += dt;
        ++t.calls;
    }

    void drawMeshImpl(MeshId id, const Matrix4 &model, const MeshDrawState &state)
    {
        PhaseClock phase;
        if (meshPhasesEnabled() && resourceDiagEnabled())
        {
            phase.timing = m_target ? &m_meshTimingTarget : &m_meshTiming;
            phase.last = diagNowUs();
        }
        if (!m_gl.inRun)
            flushScreenBatch();
        if (!m_in3D)
            apply3D();
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;
        if (m_gl.on && !m_gl.inRun)
        {
            m_gl.baseline();
            m_gl.inRun = true;
        }

        // State shared by all parts of the mesh. With the GL cache it is applied right before each part that is drawn
        // directly (a pending merged batch sets its own, and the shadow keeps the calls that change nothing away).
        const auto meshState = [&] {
            // an FB_ONLY test is set per part with its own reference below
            if (!(m_gl.on && alphaTestFbOnly(state.alphaTest)))
                glsAlpha(state.alphaTest);
            glsFog(state.fog);
            glsShade(state.gouraudShading);
            glsCap(m_gl.depthTest, GL_DEPTH_TEST, state.depthTest, kCacheDepth);
            glsDepthMask(state.depthWrite);
            glsCull(state.cullMode);
        };
        if (!m_gl.on)
            meshState();

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glMultMatrixf(model.data());
        glsClient(m_gl.vtxArr, GL_VERTEX_ARRAY, true);
        phase.lap(0);
        for (DrawPart &part : found->second)
        {
            if (m_gl.on)
            {
                if (m_merge.enabled)
                {
                    if (tryMergePart(part, model, state))
                    {
                        phase.lap(5);
                        continue;
                    }
                    flushMerge(&model); // the pending batch comes before this part
                }
                meshState();
            }
            if (meshInstancesEnabled() && !part.skinned && part.staticVertexCache.valid &&
                part.staticVertexCache.model != model &&
                part.gpuVertices.size() == part.sourceVertices.size())
            {
                constexpr std::size_t kMaxInstances = 24u;
                DrawPart::Instance *hit = nullptr, *oldest = nullptr;
                for (DrawPart::Instance &instance : part.instances)
                {
                    if (instance.key.valid && instance.key.model == model)
                    {
                        hit = &instance;
                        break;
                    }
                    if (instance.usedFrame != m_instanceFrame && (!oldest || instance.usedFrame < oldest->usedFrame))
                        oldest = &instance;
                }
                if (!hit && part.slotUsedFrame == m_instanceFrame)
                {
                    // The slot in use was baked for another copy drawn this frame: park it.
                    if (part.instances.size() < kMaxInstances)
                    {
                        DrawPart::Instance fresh;
                        glGenBuffers(1, &fresh.vertexBuffer);
                        glBindBuffer(GL_ARRAY_BUFFER, fresh.vertexBuffer);
                        m_gl.arrayBuf = fresh.vertexBuffer;
                        glBufferData(GL_ARRAY_BUFFER,
                                     static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                     part.gpuVertices.data(), GL_STATIC_DRAW);
                        part.instances.push_back(std::move(fresh));
                        hit = &part.instances.back();
                    }
                    else
                        hit = oldest; // may be null: every slot was used this frame, re-bake in place
                }
                if (hit)
                {
                    std::swap(part.vertexBuffer, hit->vertexBuffer);
                    std::swap(part.staticVertexCache, hit->key);
                    std::swap(part.slotUsedFrame, hit->usedFrame);
                }
            }
            part.slotUsedFrame = m_instanceFrame;
            glsBindArray(part.vertexBuffer);
            glsBindElements(part.indexBuffer);
            const auto &color = part.materialIndex < state.materialColors.size()
                ? state.materialColors[part.materialIndex]
                : part.color;
            const MeshDrawState::BlendState *blend = part.materialIndex < state.materialBlends.size()
                ? &state.materialBlends[part.materialIndex]
                : nullptr;
            // subtract: Cd - Cs * As; alpha keeps As. The equation is put back to ADD by the next part / pass.
            if (!state.blendEnabled)
                glsBlend(false, false, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            else if (blend && blend->enabled)
                glsBlend(true, blend->subtract, rwBlendFactor(blend->src), rwBlendFactor(blend->dst));
            else
                glsBlend(true, false, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            phase.lap(1);

            if (m_gpuSkinEnabled && part.skinned)
            {
                const auto &gpuTextureName = part.materialIndex < state.materialTextures.size()
                    ? state.materialTextures[part.materialIndex] : part.textureName;
                const auto gpuTexture = gpuTextureName.empty() ? m_textures.end() : m_textures.find(textureKey(gpuTextureName));
                const MeshDrawState::DualPass *gpuDual = part.materialIndex < state.materialDualPasses.size()
                    ? &state.materialDualPasses[part.materialIndex] : nullptr;
                const MeshDrawState::EnvPass *gpuEnv = part.materialIndex < state.materialEnvPasses.size()
                    ? &state.materialEnvPasses[part.materialIndex] : nullptr;
                bool paletteAvailable = true;
                for (const GpuSkinPart &skinPart : part.gpuSkinParts)
                    for (std::uint8_t bone : skinPart.paletteBones)
                        paletteAvailable = paletteAvailable && bone < state.skinPalette.size();
                const bool rigidOnlyGate = !gpuSkinDbgRigidOnly() ||
                    (part.skinLayout.genericFirst == part.sourceVertices.size() && !part.skinLayout.runs.empty());
                const bool gpuCanDraw = m_gpuSkinEnabled && part.skinned && !part.gpuSkinParts.empty() && rigidOnlyGate &&
                    paletteAvailable && state.gouraudShading && (!part.lighting || !state.lightingEnabled ||
                        (state.directionalLights.size() <= 4u && state.localLights.size() <= 4u)) &&
                    !(gpuDual && gpuDual->enabled) && !(gpuEnv && gpuEnv->enabled);
                if (part.skinned && m_gpuSkinEnabled)
                {
                    if (!gpuCanDraw)
                    {
                        ++m_gpuSkinCpuFallbacks;
                        const std::size_t reason = part.gpuSkinParts.empty() ? 0u :
                            !paletteAvailable ? 1u : !state.gouraudShading ? 2u :
                            (part.lighting && state.lightingEnabled &&
                             (state.directionalLights.size() > 4u || state.localLights.size() > 4u)) ? 3u : 4u;
                        ++m_gpuSkinFallbackReasons[reason];
                    }
                }
                if (gpuCanDraw)
                {
                    const MeshDrawState::SamplerState *sampler = part.materialIndex < state.materialSamplers.size()
                        ? &state.materialSamplers[part.materialIndex] : nullptr;
                    phase.skip();
                    drawGpuSkinPart(part, model, state, color,
                                    gpuTexture == m_textures.end() ? 0u : gpuTexture->second,
                                    sampler);
                    if (m_gl.on)
                    {
                        // the skin path sets its own program, arrays, texture and blend state
                        m_gl.unknown();
                        m_gl.skinTouched = true;
                        m_samplerCache.clear();
                    }
                    phase.lap(7);
                    if (phase.timing) ++phase.timing->partsGpuSkin;
                    continue;
                }
            }
            if (part.gpuVertices.size() == part.sourceVertices.size())
            {
                const MeshDrawState::UvTransform *uvTransform =
                    part.materialIndex < state.materialUvTransforms.size()
                        ? &state.materialUvTransforms[part.materialIndex]
                        : nullptr;
                const MeshDrawState::UvTransform defaultUvTransform{};
                const MeshDrawState::UvTransform &currentUvTransform =
                    uvTransform ? *uvTransform : defaultUvTransform;
                const std::vector<std::uint8_t> *skinBones = part.skinned ? &part.usedBones : nullptr;
                const std::vector<Matrix4> *skinPalette = part.skinned ? &state.skinPalette : nullptr;
                const bool diag = resourceDiagEnabled();
                const unsigned mismatchReasons = part.staticVertexCache.mismatch(
                    model, part.material, color, part.lighting, part.hasPrelight, state, currentUvTransform,
                    skinBones, skinPalette);
                const bool regenerateVertices = meshReuseEnabled()
                    ? !detail::canReuseVertexData(mismatchReasons == 0u)
                    : !detail::canReuseStaticVertexData(part.skinned, mismatchReasons == 0u);
                if (diag)
                {
                    MeshTiming &mt = m_target ? m_meshTimingTarget : m_meshTiming;
                    ++mt.reasons[part.skinned ? 1 : 0][mismatchReasons & 0x1ffu];
                    if (!regenerateVertices)
                        ++mt.reusedParts[part.skinned ? 1 : 0];
                }
                if (regenerateVertices)
                {
                    const std::uint64_t regenStart = diag ? diagNowUs() : 0u;
                    std::uint64_t computeEnd = 0u;
                    struct RegenTimer
                    {
                        VitaGlRenderer *self; bool skinned; std::size_t verts; std::uint64_t start; bool target;
                        std::uint64_t *computeEnd;
                        ~RegenTimer()
                        {
                            if (!start) return;
                            const std::uint64_t now = diagNowUs();
                            MeshTiming &t = target ? self->m_meshTimingTarget : self->m_meshTiming;
                            t.regenUs += now - start;
                            t.computeUs += (*computeEnd ? *computeEnd : now) - start;
                            t.regenVerts += static_cast<std::uint32_t>(verts);
                            (skinned ? t.regenSkinnedParts : t.regenStaticParts)++;
                            (skinned ? t.regenSkinnedVerts : t.regenStaticVerts) += static_cast<std::uint32_t>(verts);
                        }
                    } regenTimer{this, part.skinned, part.sourceVertices.size(), regenStart, m_target != nullptr,
                                 &computeEnd};
                    detail::RegenContext fastContext;
                    const bool fast = meshFastRegenEnabled() &&
                        detail::prepareRegenContext(fastContext, model, state.skinPalette, part.material, color,
                                                    part.lighting, part.hasPrelight, state, currentUvTransform);
                    if (fast)
                    {
                        detail::regenerateVertices(fastContext, part.skinned, part.skinned ? &part.skinLayout : nullptr,
                                                   part.sourceVertices.data(),
                                                   part.gpuVertices.data(), part.sourceVertices.size());
                        if (diag && meshVerifyEnabled())
                        {
                            MeshTiming &mt = m_target ? m_meshTimingTarget : m_meshTiming;
                            for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                            {
                                const Vertex posed = part.skinned ? poseSkinVertex(part.sourceVertices[i], state.skinPalette)
                                                                  : part.sourceVertices[i];
                                const auto shaded = shadeVertexColor(posed, part.material, color, part.lighting,
                                                                     part.hasPrelight, state, model);
                                const auto uvRef = transformTexCoord(part.sourceVertices[i].texCoord, currentUvTransform);
                                const GpuVertex &got = part.gpuVertices[i];
                                ++mt.verifyVerts;
                                int worst = 0;
                                for (int c = 0; c < 4; ++c)
                                    worst = std::max(worst, std::abs(static_cast<int>(shaded[c]) - static_cast<int>(got.color[c])));
                                mt.verifyColorOff1 += worst == 1 ? 1u : 0u;
                                mt.verifyColorBad += worst > 1 ? 1u : 0u;
                                float pd = 0.0f;
                                for (int c = 0; c < 3; ++c)
                                    pd = std::max(pd, std::fabs(posed.position[c] - got.position[c]));
                                mt.verifyPosMax = std::max(mt.verifyPosMax, pd);
                                mt.verifyPosBad += pd > 1.0e-3f ? 1u : 0u;
                                mt.verifyUvBad += (std::fabs(uvRef[0] - got.uv[0]) > 1.0e-6f ||
                                                   std::fabs(uvRef[1] - got.uv[1]) > 1.0e-6f) ? 1u : 0u;
                            }
                        }
                        if (diag && meshBenchEnabled())
                        {
                            // Same work again with warm caches: tells memory stalls from arithmetic.
                            const std::uint64_t hotStart = diagNowUs();
                            detail::regenerateVertices(fastContext, part.skinned, part.skinned ? &part.skinLayout : nullptr,
                                                       part.sourceVertices.data(),
                                                       part.gpuVertices.data(), part.sourceVertices.size());
                            MeshTiming &mt = m_target ? m_meshTimingTarget : m_meshTiming;
                            mt.hotUs += diagNowUs() - hotStart;
                            mt.hotVerts += static_cast<std::uint32_t>(part.sourceVertices.size());
                        }
                    }
                    else
                    {
                        for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                        {
                            const Vertex posed = part.skinned ? poseSkinVertex(part.sourceVertices[i], state.skinPalette)
                                                              : part.sourceVertices[i];
                            std::copy(posed.position.begin(), posed.position.end(), part.gpuVertices[i].position);
                            if (currentUvTransform.enabled)
                            {
                                const auto uv = transformTexCoord(part.sourceVertices[i].texCoord, currentUvTransform);
                                part.gpuVertices[i].uv[0] = uv[0];
                                part.gpuVertices[i].uv[1] = uv[1];
                            }
                            else
                            {
                                part.gpuVertices[i].uv[0] = part.sourceVertices[i].texCoord[0];
                                part.gpuVertices[i].uv[1] = part.sourceVertices[i].texCoord[1];
                            }
                            const auto shaded = shadeVertexColor(posed, part.material, color,
                                                                 part.lighting, part.hasPrelight, state, model);
                            std::copy(shaded.begin(), shaded.end(), part.gpuVertices[i].color);
                        }
                    }
                    if (diag)
                        computeEnd = diagNowUs();
                    glBufferSubData(GL_ARRAY_BUFFER, 0,
                                    static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                    part.gpuVertices.data());
                    part.staticVertexCache.capture(model, part.material, color, part.lighting,
                                                   part.hasPrelight, state, currentUvTransform,
                                                   skinBones, skinPalette);
                }
            }
            phase.lap(2);
            glsClient(m_gl.colArr, GL_COLOR_ARRAY, true);
            const auto &textureName = part.materialIndex < state.materialTextures.size()
                ? state.materialTextures[part.materialIndex] : part.textureName;
            const GLuint textureId = textureName.empty() ? 0u : lookupTexture(textureName);
            glsPointers(part.vertexBuffer, textureId != 0u);
            if (textureId != 0u)
            {
                const MeshDrawState::SamplerState *sampler = part.materialIndex < state.materialSamplers.size()
                    ? &state.materialSamplers[part.materialIndex] : nullptr;
                glsTexture(textureId, resolveMeshSampler(state, sampler));
            }
            else
                glsTexture(0u, ResolvedSampler{});
            // FB_ONLY alpha test (see lighting.h): unskinned parts are drawn twice, skinned ones once.
            const bool alphaTwoPass = alphaTestFbOnly(state.alphaTest) && !part.skinned &&
                                      alphaTestFailPass(state.alphaTest).function != 0u;
            if (alphaTestFbOnly(state.alphaTest))
                glsAlpha(alphaTwoPass ? alphaTestPassOne(state.alphaTest) : alphaTestSinglePass(state.alphaTest));
            phase.lap(3);
            glDrawElements(GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT, nullptr);
            if (phase.timing) { ++phase.timing->partsStatic; ++phase.timing->glDraws; }
            if (alphaTwoPass)
            {
                // The pixels that failed the test: still drawn, Z not written.
                glsAlpha(alphaTestFailPass(state.alphaTest));
                glsDepthMask(false);
                glDrawElements(GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT, nullptr);
                glsDepthMask(state.depthWrite);
                glsAlpha(alphaTestPassOne(state.alphaTest));
                if (phase.timing) ++phase.timing->glDraws;
            }
            phase.lap(4);

            const MeshDrawState::DualPass *dual = part.materialIndex < state.materialDualPasses.size()
                ? &state.materialDualPasses[part.materialIndex]
                : nullptr;
            if (dual && dual->enabled && !dual->textureName.empty())
            {
                const auto dualTexture = m_textures.find(textureKey(dual->textureName));
                if (dualTexture != m_textures.end())
                {
                    if (m_gl.on) { m_gl.unknown(); m_samplerCache.clear(); }
                    applyMeshSampler(dualTexture->second, state, &dual->sampler);
                    for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                    {
                        // Second texture coordinate set, carried in weights (see decodeNativeMesh).
                        const Vertex &dualSource = part.sourceVertices[i];
                        const bool hasUv1 = !part.skinned && dualSource.weights[2] == 1.0f;
                        const std::array<float, 2> dualBase = hasUv1
                            ? std::array<float, 2>{dualSource.weights[0], dualSource.weights[1]} : dualSource.texCoord;
                        const auto uv = transformTexCoord(dualBase, dual->uvTransform);
                        part.gpuVertices[i].uv[0] = uv[0];
                        part.gpuVertices[i].uv[1] = uv[1];
                    }
                    glBufferSubData(GL_ARRAY_BUFFER, 0,
                                    static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                    part.gpuVertices.data());
                    part.staticVertexCache.invalidate();
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, dualTexture->second);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, uv)));
                    glEnable(GL_BLEND);
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(rwBlendFactor(dual->srcBlend), rwBlendFactor(dual->dstBlend));
                    glDrawElements(GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT, nullptr);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            }

            const MeshDrawState::EnvPass *env = part.materialIndex < state.materialEnvPasses.size()
                ? &state.materialEnvPasses[part.materialIndex]
                : nullptr;
            if (env && env->enabled && part.hasNormals && !env->textureName.empty())
            {
                const auto envTexture = m_textures.find(textureKey(env->textureName));
                if (envTexture != m_textures.end())
                {
                    if (m_gl.on) { m_gl.unknown(); m_samplerCache.clear(); }
                    applyMeshSampler(envTexture->second, state, &env->sampler);
                    const float coefficient = std::clamp(env->coefficient, 0.0f, 1.0f);
                    for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                    {
                        const Vertex posed = part.skinned ? poseSkinVertex(part.sourceVertices[i], state.skinPalette)
                                                          : part.sourceVertices[i];
                        const auto uv = environmentTexCoord(posed, model, *env);
                        part.gpuVertices[i].uv[0] = uv[0];
                        part.gpuVertices[i].uv[1] = uv[1];
                        const float alpha = env->frameBufferAlpha
                            ? static_cast<float>(part.gpuVertices[i].color[3]) / 255.0f
                            : 1.0f;
                        const auto level = static_cast<std::uint8_t>(std::lround(255.0f * coefficient * alpha));
                        part.gpuVertices[i].color[0] = level;
                        part.gpuVertices[i].color[1] = level;
                        part.gpuVertices[i].color[2] = level;
                        part.gpuVertices[i].color[3] = 255u;
                    }
                    glBufferSubData(GL_ARRAY_BUFFER, 0,
                                    static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                    part.gpuVertices.data());
                    part.staticVertexCache.invalidate();
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, envTexture->second);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, uv)));
                    glEnable(GL_BLEND);
                    glBlendEquation(GL_FUNC_ADD);
                    glBlendFunc(GL_ONE, GL_ONE);
                    glDrawElements(GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT, nullptr);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                }
            }
            if (!m_gl.on)
            {
                glBlendEquation(GL_FUNC_ADD);
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            phase.lap(5);
        }
        if (m_gl.on)
        {
            // the state is put back to the baseline by leaveMesh() when the run of mesh draws ends
            glPopMatrix();
            phase.lap(6);
            return;
        }
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4ub(255u, 255u, 255u, 255u);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_TEXTURE_2D);
        glUseProgram(0);
        for (GLuint attribute = 0; attribute < 6u; ++attribute)
            glDisableVertexAttribArray(attribute);
        glPopMatrix();
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glShadeModel(GL_SMOOTH);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
        phase.lap(6);
    }

    void drawWorldLines(const std::vector<Vertex> &vertices, const Matrix4 &model,
                        std::string_view textureName, const MeshDrawState &state) override
    {
        if (vertices.size() < 2u)
            return;
        flushScreenBatch();
        if (!m_in3D)
            apply3D();

        applyAlphaTest(state.alphaTest);
        applyFog(state.fog);
        applyShadeMode(state.gouraudShading);

        std::vector<GpuVertex> gpu(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            const Vertex &in = vertices[i];
            GpuVertex &out = gpu[i];
            std::copy(in.position.begin(), in.position.end(), out.position);
            std::copy(in.texCoord.begin(), in.texCoord.end(), out.uv);
            std::copy(in.color.begin(), in.color.end(), out.color);
        }

        if (state.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
        glBlendEquation(GL_FUNC_ADD);
        if (!state.blendEnabled)
            glDisable(GL_BLEND);
        else if (!state.materialBlends.empty() && state.materialBlends[0].enabled)
        {
            glEnable(GL_BLEND);
            if (state.materialBlends[0].subtract)
            {
                glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
                glBlendFuncSeparate(rwBlendFactor(state.materialBlends[0].src), rwBlendFactor(state.materialBlends[0].dst),
                                    GL_ONE, GL_ZERO);
            }
            else
                glBlendFunc(rwBlendFactor(state.materialBlends[0].src), rwBlendFactor(state.materialBlends[0].dst));
        }
        else
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }

        GLuint texture = 0u;
        if (!textureName.empty())
        {
            const auto found = m_textures.find(textureKey(textureName));
            if (found != m_textures.end())
            {
                texture = found->second;
                applyMeshSampler(texture, state);
            }
        }
        if (texture)
        {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture);
        }
        else
            glDisable(GL_TEXTURE_2D);

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glMultMatrixf(model.data());
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(GpuVertex), &gpu[0].position[0]);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GpuVertex), &gpu[0].color[0]);
        if (texture)
        {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), &gpu[0].uv[0]);
        }
        else
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(gpu.size()));
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glPopMatrix();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glShadeModel(GL_SMOOTH);

        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
    }

    void drawScreenTriangles(const std::vector<ScreenVertex> &vertices,
                             float sourceWidth, float sourceHeight,
                             std::string_view textureName,
                             const ScreenDrawState &state) override
    {
        leaveMesh();
        if (!detail::screenBatchEnabled() || m_target || vertices.empty() || sourceWidth <= 0.0f || sourceHeight <= 0.0f)
        {
            flushScreenBatch(true);
            drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, GL_TRIANGLES);
            return;
        }
        submitScreenTriangles(vertices, sourceWidth, sourceHeight, textureName, state);
    }

    void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                         float sourceWidth, float sourceHeight,
                         std::string_view textureName,
                         const ScreenDrawState &state) override
    {
        flushScreenBatch(true);
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, GL_LINES);
    }

    std::string drainDiagReport() override
    {
        if (!resourceDiagEnabled() || (m_screenTiming.draws == 0u && m_screenTiming.submits == 0u))
            return {};
        char meshLine[400];
        {
            const MeshTiming &a = m_meshTiming, &b = m_meshTimingTarget;
            std::snprintf(meshLine, sizeof meshLine,
                          "[native-mesh] display: calls=%u us=%llu regen-us=%llu (skinned parts=%u static parts=%u verts=%u) | target passes: calls=%u us=%llu regen-us=%llu (skinned parts=%u static parts=%u verts=%u)\n",
                          a.calls, static_cast<unsigned long long>(a.us), static_cast<unsigned long long>(a.regenUs),
                          a.regenSkinnedParts, a.regenStaticParts, a.regenVerts,
                          b.calls, static_cast<unsigned long long>(b.us), static_cast<unsigned long long>(b.regenUs),
                          b.regenSkinnedParts, b.regenStaticParts, b.regenVerts);
            for (int pass = 0; pass < 2; ++pass)
            {
                const MeshTiming &m = pass ? b : a;
                char reasonLine[600];
                int len = std::snprintf(reasonLine, sizeof reasonLine,
                                        "[native-regen] %s: hot-us=%llu hot-verts=%u compute-us=%llu upload-us=%llu verts skinned=%u static=%u reused skinned=%u static=%u | why static:",
                                        pass ? "target" : "display",
                                        static_cast<unsigned long long>(m.hotUs), m.hotVerts,
                                        static_cast<unsigned long long>(m.computeUs),
                                        static_cast<unsigned long long>(m.regenUs - m.computeUs),
                                        m.regenSkinnedVerts, m.regenStaticVerts, m.reusedParts[1], m.reusedParts[0]);
                for (int kind = 0; kind < 2 && len > 0 && len < static_cast<int>(sizeof reasonLine) - 40; ++kind)
                {
                    if (kind == 1)
                        len += std::snprintf(reasonLine + len, sizeof reasonLine - len, " | skinned:");
                    for (unsigned r = 0; r < 512 && len < static_cast<int>(sizeof reasonLine) - 40; ++r)
                        if (m.reasons[kind][r])
                            len += std::snprintf(reasonLine + len, sizeof reasonLine - len, " 0x%x=%u", r, m.reasons[kind][r]);
                }
                std::fprintf(stderr, "%s\n", reasonLine);
                if (m.verifyVerts)
                    std::fprintf(stderr, "[native-verify] %s: verts=%llu color-off-by-1=%llu color-bad=%llu pos-bad=%llu (max %.2e) uv-bad=%llu\n",
                                 pass ? "target" : "display", static_cast<unsigned long long>(m.verifyVerts),
                                 static_cast<unsigned long long>(m.verifyColorOff1),
                                 static_cast<unsigned long long>(m.verifyColorBad),
                                 static_cast<unsigned long long>(m.verifyPosBad), m.verifyPosMax,
                                 static_cast<unsigned long long>(m.verifyUvBad));
            }
            if (a.phaseUs[0] || a.phaseUs[7] || a.phaseUs[4])
                std::fprintf(stderr,
                             "[native-mesh-phase] display us/frame-window: prologue=%llu bind+blend=%llu regen+rest=%llu pointers+texture=%llu gldraw=%llu dual/env+reset=%llu epilogue=%llu gpuskin=%llu | parts static=%u gpuskin=%u gl-draws=%u\n",
                             static_cast<unsigned long long>(a.phaseUs[0]), static_cast<unsigned long long>(a.phaseUs[1]),
                             static_cast<unsigned long long>(a.phaseUs[2]), static_cast<unsigned long long>(a.phaseUs[3]),
                             static_cast<unsigned long long>(a.phaseUs[4]), static_cast<unsigned long long>(a.phaseUs[5]),
                             static_cast<unsigned long long>(a.phaseUs[6]), static_cast<unsigned long long>(a.phaseUs[7]),
                             a.partsStatic, a.partsGpuSkin, a.glDraws);
            if (a.mergedParts || a.mergeBatches)
                std::fprintf(stderr, "[native-mesh-merge] display: merged parts=%u verts=%u in %u draws | world-space vertices copied=%u baked=%u verify-bad=%u | cache %u KB\n",
                             a.mergedParts, a.mergedVerts, a.mergeBatches, a.mergeReused, a.mergeBaked, a.mergeVerifyBad,
                             static_cast<unsigned>(g_mergeCacheBytes / 1024u));
            if (a.mergedParts || a.mergeBatches)
                std::fprintf(stderr, "[native-mesh-merge] parts drawn alone: skinned=%u big/no-list=%u matrix=%u dual=%u env=%u twice=%u lights=%u\n",
                             a.mergeRefused[0], a.mergeRefused[1], a.mergeRefused[2], a.mergeRefused[3], a.mergeRefused[4],
                             a.mergeRefused[5], a.mergeRefused[6]);
            m_meshTiming = {};
            m_meshTimingTarget = {};
        }
        const ScreenTiming &t = m_screenTiming;
        const double n = std::max<double>(t.draws, 1.0);
        const double sn = std::max<double>(t.submits, 1.0);
        char line[500];
        std::snprintf(line, sizeof line,
                      "[native-screen] batch=%d atlas=%d gl-calls: submits=%u draws=%u gl-draws=%u verts/draw=%.1f | per gl call us: total=%.1f state=%.1f texture=%.1f blend=%.1f matrix=%.1f arrays=%.1f gldraw=%.1f restore=%.1f | submit=%.2f us each\n",
                      detail::screenBatchEnabled() ? 1 : 0, detail::screenAtlasEnabled() ? 1 : 0,
                      t.submits, t.draws, t.glDraws, t.vertices / n,
                      (t.us[0] + t.us[1] + t.us[2] + t.us[3] + t.us[4] + t.us[5] + t.us[6]) / n,
                      t.us[0] / n, t.us[1] / n, t.us[2] / n, t.us[3] / n, t.us[4] / n, t.us[5] / n, t.us[6] / n,
                      t.submitUs / sn);
        char breakLine[200];
        std::snprintf(breakLine, sizeof breakLine,
                      "[native-screen-breaks] first=%u size/full=%u texture=%u (big-texture draws=%u) filter=%u state=%u overlap=%u\n",
                      t.breaks[1], t.breaks[2], t.breaks[3], t.bigTextureBreaks, t.breaks[4], t.breaks[5], t.breaks[6]);
        m_screenTiming = {};
        return std::string(meshLine) + line + breakLine;
    }

    void endFrame() override
    {
        flushScreenBatch();
        glDisable(GL_DEPTH_TEST);
        EndDrawing();
        m_in3D = false;
        if (m_gpuSkinEnabled && resourceDiagEnabled() && ++m_gpuSkinReportFrames % 120u == 0u)
            reportSkinStats();
    }

    // ---- A/B frame comparison support (see AbRenderer at the end of the file) ----------------------------------------
    // The switchable fast paths. Changed between frames only.
    struct Modes
    {
        bool glCache = false, merge = false, screenCache = false;
        int skip = 0;
    };
    Modes modes() const { return {m_gl.on, m_merge.enabled, m_g2.on, m_glSkip}; }
    void setModes(const Modes &modes)
    {
        flushScreenBatch(); // ends both kinds of run
        m_merge.enabled = modes.merge;
        m_gl.on = modes.glCache || modes.merge;
        m_g2.on = modes.screenCache;
        m_glSkip = modes.skip;
    }
    void drainPending() { flushScreenBatch(); }
    // RGBA8 of the host framebuffer as drawn so far this frame (bottom row first, GL order). False when it cannot be read.
    bool readFramebuffer(std::vector<std::uint8_t> &rgba, int &width, int &height)
    {
        flushScreenBatch();
        width = std::max(GetScreenWidth(), 1);
        height = std::max(GetScreenHeight(), 1);
        rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u, 0u);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFinish();
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        m_in3D = false;
        return true;
    }

private:
    void reportSkinStats() const
    {
        if (resourceDiagEnabled() && m_gpuSkinEnabled)
            std::fprintf(stderr, "[native-skin] draws=%llu cpu-fallbacks=%llu reason(no-batch=%llu,palette=%llu,flat=%llu,lights=%llu,dual-env=%llu)\n",
                         static_cast<unsigned long long>(m_gpuSkinDraws),
                         static_cast<unsigned long long>(m_gpuSkinCpuFallbacks),
                         static_cast<unsigned long long>(m_gpuSkinFallbackReasons[0]),
                         static_cast<unsigned long long>(m_gpuSkinFallbackReasons[1]),
                         static_cast<unsigned long long>(m_gpuSkinFallbackReasons[2]),
                         static_cast<unsigned long long>(m_gpuSkinFallbackReasons[3]),
                         static_cast<unsigned long long>(m_gpuSkinFallbackReasons[4]));
    }

    static Matrix4 multiplyMatrix(const Matrix4 &a, const Matrix4 &b)
    {
        Matrix4 out{};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                for (int k = 0; k < 4; ++k)
                    out[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
        return out;
    }

    Matrix4 currentProjection() const
    {
        if (m_camera.useMatrices)
            return m_camera.projection;
        const float aspect = 640.0f / 448.0f;
        constexpr float nearPlane = 0.1f;
        constexpr float farPlane = 10000.0f;
        const float radians = m_camera.fovy * 0.008726646259971648f;
        const float top = nearPlane * std::tan(std::max(radians, 0.01f));
        const float right = top * aspect;
        Matrix4 projection{};
        projection[0] = nearPlane / right;
        projection[5] = nearPlane / top;
        projection[10] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        projection[11] = -1.0f;
        projection[14] = -(2.0f * farPlane * nearPlane) / (farPlane - nearPlane);
        return projection;
    }

    Matrix4 currentView() const
    {
        return m_camera.useMatrices ? m_camera.view : lookAt(m_camera);
    }

    static void matrixRows(const Matrix4 &matrix, float *rows, int rowCount = 4)
    {
        for (int row = 0; row < rowCount; ++row)
        {
            rows[row * 4 + 0] = matrix[row];
            rows[row * 4 + 1] = matrix[row + 4];
            rows[row * 4 + 2] = matrix[row + 8];
            rows[row * 4 + 3] = matrix[row + 12];
        }
    }

    void drawGpuSkinPart(DrawPart &part, const Matrix4 &model, const MeshDrawState &state,
                         const std::array<std::uint8_t, 4> &materialColor, GLuint texture,
                         const MeshDrawState::SamplerState *sampler)
    {
        const MeshDrawState::UvTransform *uvTransform = part.materialIndex < state.materialUvTransforms.size()
            ? &state.materialUvTransforms[part.materialIndex] : nullptr;
        const MeshDrawState::UvTransform defaultUv{};
        const MeshDrawState::UvTransform &uv = uvTransform ? *uvTransform : defaultUv;
        const Matrix4 view = currentView();
        const Matrix4 viewModel = multiplyMatrix(view, model);
        const Matrix4 mvp = multiplyMatrix(currentProjection(), viewModel);
        std::array<float, 16> mvpRows{};
        std::array<float, 12> modelRows{};
        matrixRows(mvp, mvpRows.data());
        matrixRows(model, modelRows.data(), 3);

        const auto norm = [](std::uint8_t c) { return static_cast<float>(c) / 255.0f; };
        const GLfloat material[4]{norm(materialColor[0]), norm(materialColor[1]), norm(materialColor[2]), norm(materialColor[3])};
        const GLfloat terms[4]{part.material.ambient, part.material.diffuse, part.material.specular, 0.0f};
        GLfloat ambient[4]{0.0f, 0.0f, 0.0f, part.hasPrelight ? 1.0f : 0.0f};
        const bool useLighting = part.lighting && state.lightingEnabled;
        if (useLighting)
            for (int c = 0; c < 3; ++c)
                ambient[c] = state.ambientLight[c] * part.material.ambient;

        GLfloat dirDirections[16]{}, dirColors[16]{};
        GLfloat localPositions[16]{}, localDirections[16]{}, localCones[16]{}, localColors[16]{};
        if (useLighting)
        {
            for (std::size_t i = 0; i < state.directionalLights.size(); ++i)
                for (int c = 0; c < 3; ++c)
                {
                    dirDirections[i * 4u + c] = state.directionalLights[i].direction[c];
                    dirColors[i * 4u + c] = state.directionalLights[i].color[c];
                }
            for (std::size_t i = 0; i < state.localLights.size(); ++i)
            {
                const auto &light = state.localLights[i];
                for (int c = 0; c < 3; ++c)
                {
                    localPositions[i * 4u + c] = light.position[c];
                    localDirections[i * 4u + c] = light.direction[c];
                    localColors[i * 4u + c] = light.color[c];
                }
                localPositions[i * 4u + 3u] = light.radius;
                localCones[i * 4u] = light.minusCosAngle;
                localDirections[i * 4u + 3u] =
                    light.type == MeshDrawState::LocalLight::Type::Point ? 0.0f :
                    light.type == MeshDrawState::LocalLight::Type::Spot ? 1.0f : 2.0f;
            }
        }

        const GLfloat uvRows[8]{uv.rightX, uv.upX, uv.posX, 0.0f,
                                uv.rightY, uv.upY, uv.posY, 0.0f};
        const GLfloat fogColor[4]{state.fog.color[0] / 255.0f, state.fog.color[1] / 255.0f,
                                  state.fog.color[2] / 255.0f, 1.0f};
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glUseProgram(m_skinProgram.program);
        glUniform4fv(m_skinProgram.mvp, 4, mvpRows.data());
        glUniform4fv(m_skinProgram.modelRows, 3, modelRows.data());
        glUniform4fv(m_skinProgram.uvRows, 2, uvRows);
        glUniform4fv(m_skinProgram.materialColor, 1, material);
        glUniform4fv(m_skinProgram.materialTerms, 1, terms);
        glUniform4fv(m_skinProgram.ambient, 1, ambient);
        glUniform1i(m_skinProgram.directionalCount,
                    useLighting ? static_cast<GLint>(state.directionalLights.size()) : -1);
        glUniform4fv(m_skinProgram.directionalDirections, 4, dirDirections);
        glUniform4fv(m_skinProgram.directionalColors, 4, dirColors);
        glUniform1i(m_skinProgram.localCount,
                    useLighting ? static_cast<GLint>(state.localLights.size()) : 0);
        glUniform4fv(m_skinProgram.localPositions, 4, localPositions);
        glUniform4fv(m_skinProgram.localDirections, 4, localDirections);
        glUniform4fv(m_skinProgram.localCones, 4, localCones);
        glUniform4fv(m_skinProgram.localColors, 4, localColors);
        glUniform1i(m_skinProgram.sampler, 0);
        glUniform1i(m_skinProgram.useTexture, texture ? 1 : 0);
        glUniform1i(m_skinProgram.alphaEnabled, state.alphaTest.enabled ? 1 : 0);
        glUniform1i(m_skinProgram.alphaFunction, state.alphaTest.function & 7u);
        glUniform1f(m_skinProgram.alphaReference, state.alphaTest.reference / 255.0f);
        glUniform1i(m_skinProgram.fogEnabled, state.fog.enabled ? 1 : 0);
        glUniform2f(m_skinProgram.fogRange, state.fog.start, state.fog.end);
        glUniform4fv(m_skinProgram.fogColor, 1, fogColor);
        if (texture)
        {
            glEnable(GL_TEXTURE_2D);
            applyMeshSampler(texture, state, sampler);
        }
        else
            glDisable(GL_TEXTURE_2D);

        for (GpuSkinPart &skinPart : part.gpuSkinParts)
        {
            if (skinPart.paletteBones.size() > 24u)
                continue;
            std::array<GLfloat, 72u * 4u> paletteRows{};
            bool valid = true;
            for (std::size_t bone = 0; bone < skinPart.paletteBones.size(); ++bone)
            {
                const std::uint8_t sourceBone = skinPart.paletteBones[bone];
                if (sourceBone >= state.skinPalette.size())
                {
                    valid = false;
                    break;
                }
                const Matrix4 &matrix = state.skinPalette[sourceBone];
                const std::size_t base = bone * 12u;
                paletteRows[base + 0] = matrix[0]; paletteRows[base + 1] = matrix[4];
                paletteRows[base + 2] = matrix[8]; paletteRows[base + 3] = matrix[12];
                paletteRows[base + 4] = matrix[1]; paletteRows[base + 5] = matrix[5];
                paletteRows[base + 6] = matrix[9]; paletteRows[base + 7] = matrix[13];
                paletteRows[base + 8] = matrix[2]; paletteRows[base + 9] = matrix[6];
                paletteRows[base + 10] = matrix[10]; paletteRows[base + 11] = matrix[14];
            }
            if (!valid)
                return;
            glUniform1i(m_skinProgram.skinCount, static_cast<GLint>(skinPart.paletteBones.size()));
            glUniform4fv(m_skinProgram.skinRows, static_cast<GLsizei>(skinPart.paletteBones.size() * 3u), paletteRows.data());
            glBindBuffer(GL_ARRAY_BUFFER, skinPart.vertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, skinPart.indexBuffer);
            for (GLuint attribute = 0; attribute < 6u; ++attribute)
                glEnableVertexAttribArray(attribute);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, position)));
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, uv)));
            glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, color)));
            glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_FALSE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, bones)));
            glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, weights)));
            glVertexAttribPointer(5, 3, GL_FLOAT, GL_FALSE, sizeof(GpuSkinVertex), reinterpret_cast<const void *>(offsetof(GpuSkinVertex, normal)));
            glDrawElements(GL_TRIANGLES, skinPart.indexCount, GL_UNSIGNED_SHORT, nullptr);
            ++m_gpuSkinDraws;
        }
        glUseProgram(0);
        for (GLuint attribute = 0; attribute < 6u; ++attribute)
            glDisableVertexAttribArray(attribute);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        applyAlphaTest(state.alphaTest);
        applyFog(state.fog);
        glBlendEquation(GL_FUNC_ADD);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // The draw shows a whole texture (UV 0..1, so no neighbouring atlas cell can bleed in): filtered even when the
    // game asked for point sampling.
    bool wholeTextureLinear(const std::vector<ScreenVertex> &vertices, const ScreenDrawState &state) const
    {
        if (state.linearFilter)
            return true;
        if (m_target)
            return false;
        float u0 = 1.0f, u1 = 0.0f, v0 = 1.0f, v1 = 0.0f;
        for (const ScreenVertex &v : vertices)
        {
            u0 = std::min(u0, v.u); u1 = std::max(u1, v.u);
            v0 = std::min(v0, v.v); v1 = std::max(v1, v.v);
        }
        return u0 <= 0.01f && u1 >= 0.99f && v0 <= 0.01f && v1 >= 0.99f;
    }

    // Z bias of the next GEQUAL draw at the Z layer of `vertices` (see the comment in drawScreenPrimitive); counts it.
    float screenZBias(const ScreenVertex *vertices, std::size_t count, const ScreenDrawState &state)
    {
        if (m_target || !state.depthTest || state.depthFunction != ScreenDepthFunction::GreaterEqual ||
            !screenZBiasEnabled())
            return 0.0f;
        float zMax = vertices[0].z;
        for (std::size_t i = 1; i < count; ++i)
            zMax = std::max(zMax, vertices[i].z);
        std::uint32_t *counter = nullptr;
        for (auto &layer : m_screenZLayers)
            if (layer.first == vertices[0].z)
            {
                counter = &layer.second;
                break;
            }
        if (!counter)
        {
            m_screenZLayers.emplace_back(vertices[0].z, 0u);
            counter = &m_screenZLayers.back().second;
        }
        // Never past the near plane (Z = 1), and never more than a fraction of a layer.
        const float bias = std::min({static_cast<float>(*counter) * 2.5e-7f, 1.5e-4f, std::max(1.0f - zMax, 0.0f)});
        ++*counter;
        return bias;
    }

    // ---- screen draw batching (screen_batch.h)
    struct SmallTexture
    {
        std::uint32_t width = 0, height = 0;
        std::vector<std::uint8_t> rgba;
        detail::AtlasSlot slot;
    };
    std::unordered_map<std::string, SmallTexture> m_smallTextures;
    detail::AtlasAllocator m_atlas;
    std::vector<GLuint> m_atlasPages;
    detail::ScreenBatch m_batch;
    std::vector<std::uint8_t> m_atlasScratch;

    void storeSmallTexture(const std::string &key, const TextureData &source)
    {
        if (!detail::screenAtlasEnabled() || !detail::AtlasAllocator::eligible(source.width, source.height))
            return;
        SmallTexture &small = m_smallTextures[key];
        small.width = source.width;
        small.height = source.height;
        small.rgba = source.rgba;
        if (small.slot.valid())
            writeAtlasSlot(small); // already placed: refresh its pixels
    }

    void dropSmallTexture(const std::string &key)
    {
        const auto found = m_smallTextures.find(key);
        if (found == m_smallTextures.end())
            return;
        m_atlas.release(found->second.slot);
        m_smallTextures.erase(found);
    }

    GLuint atlasPage(int page)
    {
        while (static_cast<int>(m_atlasPages.size()) <= page)
        {
            const std::vector<std::uint8_t> blank(static_cast<std::size_t>(detail::AtlasAllocator::kPageSize) *
                                                      detail::AtlasAllocator::kPageSize * 4u, 0u);
            GLuint texture = 0;
            glGenTextures(1, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, detail::AtlasAllocator::kPageSize, detail::AtlasAllocator::kPageSize,
                         0, GL_RGBA, GL_UNSIGNED_BYTE, blank.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            m_g2.boundTex = 0u; // the screen path's shadow of the bound texture
            m_atlasPages.push_back(texture);
        }
        return m_atlasPages[static_cast<std::size_t>(page)];
    }

    void writeAtlasSlot(const SmallTexture &small)
    {
        detail::padAtlasPixels(small.rgba.data(), small.slot.w, small.slot.h, m_atlasScratch);
        glBindTexture(GL_TEXTURE_2D, atlasPage(small.slot.page));
        glTexSubImage2D(GL_TEXTURE_2D, 0, small.slot.x - 1, small.slot.y - 1, small.slot.w + 2, small.slot.h + 2,
                        GL_RGBA, GL_UNSIGNED_BYTE, m_atlasScratch.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        m_g2.boundTex = 0u; // the screen path's shadow of the bound texture
    }

    // The atlas slot of a named texture, placed on first use; null when it has none (too big, atlas full, off).
    SmallTexture *atlasSlotFor(const std::string &key)
    {
        const auto found = m_smallTextures.find(key);
        if (found == m_smallTextures.end())
            return nullptr;
        SmallTexture &small = found->second;
        if (!small.slot.valid())
        {
            small.slot = m_atlas.allocate(static_cast<int>(small.width), static_cast<int>(small.height));
            if (!small.slot.valid())
                return nullptr;
            writeAtlasSlot(small);
        }
        return &small;
    }

    void submitScreenTriangles(const std::vector<ScreenVertex> &vertices, float sourceWidth, float sourceHeight,
                               std::string_view textureName, const ScreenDrawState &state)
    {
        const bool timing = resourceDiagEnabled();
        const std::uint64_t t0 = timing ? diagNowUs() : 0u;
        GLuint texture = 0u;
        SmallTexture *small = nullptr;
        if (!textureName.empty())
        {
            const std::string key = textureKey(textureName);
            // Whole-texture UVs only: a repeat or wrap the game asked for stays on the texture's own object.
            bool inside = true;
            for (const ScreenVertex &v : vertices)
                if (!(v.u >= -1e-4f && v.u <= 1.0001f && v.v >= -1e-4f && v.v <= 1.0001f))
                {
                    inside = false;
                    break;
                }
            if (inside)
                small = atlasSlotFor(key);
            if (!small)
            {
                const auto found = m_textures.find(key);
                if (found != m_textures.end())
                    texture = found->second;
            }
        }
        const bool linear = (small != nullptr || texture != 0u) && wholeTextureLinear(vertices, state);
        const GLuint page = small ? atlasPage(small->slot.page) : texture;
        const std::uintptr_t id = static_cast<std::uintptr_t>(page);
        const int why = m_batch.whyNot(sourceWidth, sourceHeight, id, linear, m_screenPlacement, state, vertices.data(),
                                       vertices.size());
        static const long traceFrame = [] { const char *v = std::getenv("P3FES_SCREEN_TRACE"); return v ? std::strtol(v, nullptr, 10) : -1l; }();
        if (traceFrame >= 0 && static_cast<long>(m_instanceFrame) >= traceFrame && static_cast<long>(m_instanceFrame) < traceFrame + 3)
        {
            const auto sizeIt = textureName.empty() ? m_textureSizes.end() : m_textureSizes.find(textureKey(textureName));
            std::fprintf(stderr, "[screen-trace] f=%u v=%u tex=%.*s %ux%u atlas=%d why=%d blend=%d dt=%d dw=%d df=%d at=%d/%u/%u af=%d z=%.4f bbox=%.0f,%.0f-%.0f,%.0f\n",
                         m_instanceFrame, static_cast<unsigned>(vertices.size()), static_cast<int>(textureName.size()), textureName.data(),
                         sizeIt != m_textureSizes.end() ? sizeIt->second[0] : 0u, sizeIt != m_textureSizes.end() ? sizeIt->second[1] : 0u,
                         small ? 1 : 0, why, static_cast<int>(state.blend), state.depthTest, state.depthWrite,
                         static_cast<int>(state.depthFunction), state.alphaTest.enabled, state.alphaTest.function,
                         state.alphaTest.reference, static_cast<int>(state.alphaFailAction), vertices[0].z,
                         vertices[0].x, vertices[0].y, vertices.back().x, vertices.back().y);
        }
        if (why)
        {
            if (timing)
            {
                ++m_screenTiming.breaks[why];
                // what a texture break is made of: draws of a big texture vs a small one
                if (why == 3 && !small && texture)
                    ++m_screenTiming.bigTextureBreaks;
            }
            flushScreenBatch(true);
            m_batch.begin(sourceWidth, sourceHeight, id, linear, m_screenPlacement, state);
        }
        m_batch.added(vertices.data(), vertices.size());
        const std::size_t first = m_batch.vertices.size();
        m_batch.vertices.insert(m_batch.vertices.end(), vertices.begin(), vertices.end());
        ScreenVertex *added = m_batch.vertices.data() + first;
        if (small)
        {
            const float scaleU = static_cast<float>(small->slot.w) / detail::AtlasAllocator::kPageSize;
            const float scaleV = static_cast<float>(small->slot.h) / detail::AtlasAllocator::kPageSize;
            const float baseU = static_cast<float>(small->slot.x) / detail::AtlasAllocator::kPageSize;
            const float baseV = static_cast<float>(small->slot.y) / detail::AtlasAllocator::kPageSize;
            for (std::size_t i = 0; i < vertices.size(); ++i)
            {
                added[i].u = baseU + added[i].u * scaleU;
                added[i].v = baseV + added[i].v * scaleV;
            }
        }
        const float bias = screenZBias(added, vertices.size(), state);
        if (bias > 0.0f)
            for (std::size_t i = 0; i < vertices.size(); ++i)
                added[i].z += bias;
        if (timing)
        {
            m_screenTiming.submitUs += diagNowUs() - t0;
            ++m_screenTiming.submits;
        }
    }

    // Draws the pending batch. keep2D: more screen draws follow at once, so the GL state of the run stays as it is;
    // every other caller wants the normal baseline back (end2D).
    void flushScreenBatch(bool keep2D = false)
    {
        leaveMesh();
        if (m_batch.active)
        {
            m_batch.active = false;
            if (!m_batch.vertices.empty())
            {
                const ScreenPlacement saved = m_screenPlacement;
                m_screenPlacement = m_batch.placement;
                drawScreenPrimitive(m_batch.vertices, m_batch.sourceWidth, m_batch.sourceHeight, std::string_view{}, m_batch.state,
                                    GL_TRIANGLES, static_cast<long>(m_batch.texture), m_batch.linear ? 1 : 0, true);
                m_screenPlacement = saved;
                m_batch.vertices.clear();
            }
        }
        if (!keep2D)
            end2D();
    }

    // ---- GL state shadow of the screen-space path (P3FES_SCREEN_GLCACHE) -------------------------------------------
    // A run of screen draws starts at the first drawScreenPrimitive after any other renderer work and ends in end2D(),
    // which every other entry point reaches through flushScreenBatch(). Inside a run only drawScreenPrimitive (and the
    // atlas uploads it triggers, which tell the shadow) touches GL, so the shadow is exact. Without the cache every draw
    // sets all of its state and puts the baseline back, as before; with it a call is made only for what differs from the
    // previous draw, and the baseline is put back once at the end of the run.
    struct Gl2D
    {
        bool on = false, active = false;
        int scissorOff = -1, shade = -1, alphaOn = -1, fogOn = -1, depthTest = -1, depthMask = -1, depthFunc = -1;
        int cullOn = -1, tex2d = -1, vtxArr = -1, colArr = -1, texArr = -1, colorMask = -1, blendKey = -1;
        int alphaFunc = -1, alphaRef = -1, arrayBuf = -1, elemBuf = -1;
        FogState fog;
        bool fogKnown = false, viewportKnown = false, orthoKnown = false;
        int vx = 0, vy = 0, vw = 0, vh = 0;
        GLuint boundTex = 0xFFFFFFFFu;
        std::array<float, 5> ortho{};
        float modelviewBias = -1.0f; // the translation loaded after glLoadIdentity; -1: unknown
        std::unordered_map<GLuint, std::uint32_t> texParams; // texture -> clampU | clampV << 1 | linear << 2
        void reset()
        {
            const bool keep = on;
            *this = Gl2D{};
            on = keep;
        }
    };
    Gl2D m_g2;

    void g2Cap(int &shadow, GLenum cap, bool enable)
    {
        if (m_g2.on && shadow == (enable ? 1 : 0))
            return;
        if (enable) glEnable(cap); else glDisable(cap);
        shadow = enable ? 1 : 0;
    }
    void g2Alpha(const AlphaTestState &alpha)
    {
        if (!alpha.enabled)
        {
            g2Cap(m_g2.alphaOn, GL_ALPHA_TEST, false);
            return;
        }
        g2Cap(m_g2.alphaOn, GL_ALPHA_TEST, true);
        if (!m_g2.on || m_g2.alphaFunc != alpha.function || m_g2.alphaRef != alpha.reference)
        {
            glAlphaFunc(gsAlphaFunc(alpha.function), alpha.reference / 255.0f);
            m_g2.alphaFunc = alpha.function;
            m_g2.alphaRef = alpha.reference;
        }
    }
    void g2Fog(const FogState &fog)
    {
        if (!fog.enabled)
        {
            g2Cap(m_g2.fogOn, GL_FOG, false);
            return;
        }
        g2Cap(m_g2.fogOn, GL_FOG, true);
        if (!m_g2.on || !m_g2.fogKnown || m_g2.fog.start != fog.start || m_g2.fog.end != fog.end || m_g2.fog.color != fog.color)
        {
            const GLfloat color[4]{fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f, 1.0f};
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, fog.start);
            glFogf(GL_FOG_END, fog.end);
            glFogfv(GL_FOG_COLOR, color);
            m_g2.fog = fog;
            m_g2.fogKnown = true;
        }
    }
    void g2Shade(bool gouraud)
    {
        if (m_g2.on && m_g2.shade == (gouraud ? 1 : 0))
            return;
        glShadeModel(gouraud ? GL_SMOOTH : GL_FLAT);
        m_g2.shade = gouraud ? 1 : 0;
    }
    void g2ColorMask(bool r, bool g, bool b, bool a)
    {
        const int mask = (r ? 1 : 0) | (g ? 2 : 0) | (b ? 4 : 0) | (a ? 8 : 0);
        if (m_g2.on && m_g2.colorMask == mask)
            return;
        glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
        m_g2.colorMask = mask;
    }
    void g2DepthMask(bool on)
    {
        if (m_g2.on && m_g2.depthMask == (on ? 1 : 0))
            return;
        glDepthMask(on ? GL_TRUE : GL_FALSE);
        m_g2.depthMask = on ? 1 : 0;
    }
    void g2DepthFunc(GLenum function)
    {
        if (m_g2.on && m_g2.depthFunc == static_cast<int>(function))
            return;
        glDepthFunc(function);
        m_g2.depthFunc = static_cast<int>(function);
    }
    void g2Client(int &shadow, GLenum array, bool enable)
    {
        if (m_g2.on && shadow == (enable ? 1 : 0))
            return;
        if (enable) glEnableClientState(array); else glDisableClientState(array);
        shadow = enable ? 1 : 0;
    }
    void g2Buffers()
    {
        if (!m_g2.on || m_g2.arrayBuf != 0)
        {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            m_g2.arrayBuf = 0;
        }
        if (!m_g2.on || m_g2.elemBuf != 0)
        {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
            m_g2.elemBuf = 0;
        }
    }
    void g2Blend(const ScreenDrawState &state)
    {
        const int key = static_cast<int>(state.blend) * 2 + (state.blend == ScreenBlend::Alpha && state.destinationAlphaTest ? 1 : 0);
        if (m_g2.on && m_g2.blendKey == key)
            return;
        m_g2.blendKey = key;
        switch (state.blend)
        {
        case ScreenBlend::Opaque:
            glDisable(GL_BLEND);
            break;
        case ScreenBlend::Additive:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
            break;
        case ScreenBlend::Subtractive:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
            break;
        case ScreenBlend::DestinationOnly:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
            break;
        case ScreenBlend::DestinationAlphaAdditive:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
            break;
        case ScreenBlend::DestinationAlpha:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ZERO);
            break;
        default:
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                                state.destinationAlphaTest ? GL_DST_ALPHA : GL_ONE, GL_ZERO);
            break;
        }
    }
    // The normal 3-D baseline after a run of screen draws (what each draw used to put back itself).
    void end2D()
    {
        if (!m_g2.active)
            return;
        m_g2.active = false;
        g2ColorMask(true, true, true, true);
        g2Client(m_g2.texArr, GL_TEXTURE_COORD_ARRAY, false);
        g2Client(m_g2.colArr, GL_COLOR_ARRAY, false);
        g2Client(m_g2.vtxArr, GL_VERTEX_ARRAY, false);
        if (m_g2.boundTex != 0u)
            glBindTexture(GL_TEXTURE_2D, 0);
        g2Cap(m_g2.tex2d, GL_TEXTURE_2D, false);
        g2Cap(m_g2.alphaOn, GL_ALPHA_TEST, false);
        g2Cap(m_g2.fogOn, GL_FOG, false);
        g2Shade(true);
        g2Cap(m_g2.depthTest, GL_DEPTH_TEST, true);
        g2DepthMask(true);
        g2DepthFunc(GL_LEQUAL);
        if (m_g2.blendKey != 0)
        {
            ScreenDrawState baseline;
            baseline.blend = ScreenBlend::Alpha;
            g2Blend(baseline);
        }
        m_g2.reset();
    }

    // forcedTexture >= 0: the GL texture to sample (0: none) instead of the one named; forcedLinear >= 0 its
    // filter; zBiased: the vertices already carry their GEQUAL Z bias (batched draws).
    void drawScreenPrimitive(const std::vector<ScreenVertex> &vertices,
                             float sourceWidth, float sourceHeight,
                             std::string_view textureName,
                             const ScreenDrawState &state, GLenum primitive,
                             long forcedTexture = -1, int forcedLinear = -1, bool zBiased = false)
    {
        if (vertices.empty() || sourceWidth <= 0.0f || sourceHeight <= 0.0f)
            return;
        const bool timing = resourceDiagEnabled() && !m_target;
        std::uint64_t timingMark = timing ? diagNowUs() : 0u;
        const auto lap = [&](int phase) {
            if (!timing) return;
            const std::uint64_t now = diagNowUs();
            m_screenTiming.us[phase] += now - timingMark;
            timingMark = now;
        };
        if (!m_g2.active)
            m_g2.reset(); // a new run: nothing is known about the GL state
        // Inside a render-target pass the source space is the target itself, drawn upside down like
        // the world draws of the pass.
        const FitViewport viewport = m_target ? worldViewport()
            : screenViewport(sourceWidth, sourceHeight,
                             FitViewport{0, 0, std::max(GetScreenWidth(), 1), std::max(GetScreenHeight(), 1)}, fitViewport);
        g2Cap(m_g2.scissorOff, GL_SCISSOR_TEST, false);
        if (!m_g2.on || !m_g2.viewportKnown || m_g2.vx != viewport.x || m_g2.vy != viewport.y ||
            m_g2.vw != viewport.width || m_g2.vh != viewport.height)
        {
            glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
            m_g2.vx = viewport.x; m_g2.vy = viewport.y; m_g2.vw = viewport.width; m_g2.vh = viewport.height;
            m_g2.viewportKnown = true;
        }

        g2Alpha(state.alphaTest);
        g2Fog(state.fog);
        g2Shade(state.gouraudShading);
        // FBMSK = 0x00FFFFFF: alpha-only write (see ScreenDrawState::alphaOnlyWrite). One state call per draw, as before.
        if (state.alphaOnlyWrite)
            g2ColorMask(false, false, false, true);
        else
            g2ColorMask(true, true, true, true);
        g2Cap(m_g2.depthTest, GL_DEPTH_TEST, state.depthTest);
        g2DepthMask(state.depthWrite);
        switch (state.depthFunction)
        {
        case ScreenDepthFunction::Never: g2DepthFunc(GL_NEVER); break;
        case ScreenDepthFunction::GreaterEqual: g2DepthFunc(GL_LEQUAL); break;
        case ScreenDepthFunction::Greater: g2DepthFunc(GL_LESS); break;
        case ScreenDepthFunction::Always:
        default: g2DepthFunc(GL_ALWAYS); break;
        }
        g2Cap(m_g2.cullOn, GL_CULL_FACE, false);
        lap(0);
        GLuint texture = 0u;
        if (forcedTexture >= 0)
            texture = static_cast<GLuint>(forcedTexture);
        else if (!textureName.empty())
        {
            const auto found = m_textures.find(textureKey(textureName));
            if (found != m_textures.end())
                texture = found->second;
        }
        if (texture)
        {
            g2Cap(m_g2.tex2d, GL_TEXTURE_2D, true);
            if (!m_g2.on || m_g2.boundTex != texture)
            {
                glBindTexture(GL_TEXTURE_2D, texture);
                m_g2.boundTex = texture;
            }
            // The game draws its 512x512 dialogue portraits texel-for-pixel with point sampling.
            // The host scales the 448-line picture to 544 lines (x1.214), where point sampling
            // repeats every fifth texel or so and the portrait looks stepped. A draw that shows a
            // whole texture (UV 0..1, so no neighbouring atlas cell can bleed in) is filtered.
            const bool linear = forcedLinear >= 0 ? forcedLinear != 0 : wholeTextureLinear(vertices, state);
            const std::uint32_t params = (state.clampU ? 1u : 0u) | (state.clampV ? 2u : 0u) | (linear ? 4u : 0u);
            bool same = false;
            if (m_g2.on)
            {
                const auto known = m_g2.texParams.find(texture);
                same = known != m_g2.texParams.end() && known->second == params;
            }
            if (!same)
            {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, state.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, state.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
                if (m_g2.on)
                    m_g2.texParams[texture] = params;
            }
        }
        else
            g2Cap(m_g2.tex2d, GL_TEXTURE_2D, false);
        lap(1);
        g2Blend(state);
        lap(2);
        // Wide placement: the source area keeps its centred 4:3 position and scale, the rest of the framebuffer
        // shows the source plane beyond x = 0 / x = sourceWidth.
        float orthoLeft = 0.0f, orthoRight = sourceWidth;
        if (!m_target && m_widescreen == 2 && m_screenPlacement == ScreenPlacement::Wide)
        {
            const float span = static_cast<float>(viewport.width) * sourceWidth /
                               static_cast<float>(std::max(fitViewport(sourceWidth, sourceHeight).width, 1));
            orthoLeft = (sourceWidth - span) * 0.5f;
            orthoRight = orthoLeft + span;
        }
        const bool flipTarget = m_target && targetFlipEnabled();
        const std::array<float, 5> orthoKey{orthoLeft, orthoRight, sourceHeight, sourceWidth, flipTarget ? 1.0f : 0.0f};
        // The GS passes a GEQUAL depth test on exactly equal Z, and the game relies on it: HUD text
        // is drawn at the same Z as the panel under it. The Vita's GPU does not give two different
        // triangles bit-identical depth for the same Z (desktop GPUs and Vita3K do), which cut
        // glyphs out. So the n-th GEQUAL draw of a frame at a given Z is moved n steps nearer: a
        // step is a few float ulps of Z, and the game's Z layers are thousands of steps apart.
        // P3FES_SCREEN_Z_BIAS=0 disables it.
        float bias = 0.0f;
        if (!zBiased)
            bias = screenZBias(vertices.data(), vertices.size(), state);
        if (!m_g2.on || !m_g2.orthoKnown || m_g2.ortho != orthoKey)
        {
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            if (flipTarget)
                glOrthof(0.0f, sourceWidth, 0.0f, sourceHeight, -1.0f, 0.0f);
            else
                glOrthof(orthoLeft, orthoRight, sourceHeight, 0.0f, -1.0f, 0.0f);
            glMatrixMode(GL_MODELVIEW);
            m_g2.ortho = orthoKey;
            m_g2.orthoKnown = true;
            m_g2.modelviewBias = -1.0f; // the next block reloads the modelview too
        }
        if (!m_g2.on || m_g2.modelviewBias != bias)
        {
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            if (bias > 0.0f)
                glTranslatef(0.0f, 0.0f, bias);
            m_g2.modelviewBias = bias;
        }
        lap(3);
        g2Buffers();
        g2Client(m_g2.vtxArr, GL_VERTEX_ARRAY, true);
        g2Client(m_g2.colArr, GL_COLOR_ARRAY, true);
        // vitaGL uses a client array of more than 32 KB where it stands (DRAW_SPEEDHACK=2) and the GPU reads it when
        // the scene renders: a batch whose vector is refilled for the next one must not be what the draw points at.
        const ScreenVertex *source = vertices.data();
#if defined(__vita__)
        if (vertices.size() * sizeof(ScreenVertex) > 24u * 1024u)
            if (const void *copy = m_arena.copy(source, vertices.size() * sizeof(ScreenVertex)))
                source = static_cast<const ScreenVertex *>(copy);
#endif
        if (texture)
        {
            g2Client(m_g2.texArr, GL_TEXTURE_COORD_ARRAY, true);
            glTexCoordPointer(2, GL_FLOAT, sizeof(ScreenVertex), &source[0].u);
        }
        else
            g2Client(m_g2.texArr, GL_TEXTURE_COORD_ARRAY, false);
        glVertexPointer(3, GL_FLOAT, sizeof(ScreenVertex), &source[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ScreenVertex), source[0].color.data());
        lap(4);
        const auto drawVertices = [&] {
            glDrawArrays(primitive, 0, static_cast<GLsizei>(vertices.size()));
            if (timing) ++m_screenTiming.glDraws;
#if defined(PS2X_P3FES_FRAME_TRACE)
            p3fes_frame_trace::gpuPass(static_cast<uint32_t>(primitive),
                static_cast<uint32_t>(vertices.size()),
                static_cast<uint32_t>(vertices.size() * sizeof(ScreenVertex)));
#endif
        };
        const bool alphaFailPass = state.alphaTest.enabled &&
                                   state.alphaFailAction != ScreenAlphaFailAction::Keep;
        if (state.alphaFailAction == ScreenAlphaFailAction::FramebufferOnly &&
            (!state.depthWrite || !state.depthTest))
        {
            // With no observable depth side effect, both alpha outcomes write
            // the same framebuffer result; one draw without alpha test is exact.
            g2Cap(m_g2.alphaOn, GL_ALPHA_TEST, false);
            drawVertices();
        }
        else
        {
            drawVertices();
            if (alphaFailPass && complementGsAlphaFunction(state.alphaTest.function) != 0u &&
                (state.alphaFailAction != ScreenAlphaFailAction::DepthOnly || state.depthWrite))
            {
                AlphaTestState failedAlpha = state.alphaTest;
                failedAlpha.function = complementGsAlphaFunction(state.alphaTest.function);
                g2Alpha(failedAlpha);
                switch (state.alphaFailAction)
                {
                case ScreenAlphaFailAction::FramebufferOnly:
                    g2DepthMask(false);
                    break;
                case ScreenAlphaFailAction::DepthOnly:
                    g2ColorMask(false, false, false, false);
                    g2DepthMask(true);
                    break;
                case ScreenAlphaFailAction::RgbOnly:
                    g2ColorMask(!state.alphaOnlyWrite, !state.alphaOnlyWrite, !state.alphaOnlyWrite, false);
                    g2DepthMask(false);
                    break;
                case ScreenAlphaFailAction::Keep:
                default:
                    break;
                }
                drawVertices();
            }
        }
        lap(5);
        m_in3D = false;
        if (m_g2.on)
            m_g2.active = true; // the baseline is put back by end2D() when the run ends
        else
        {
            g2ColorMask(true, true, true, true);
            g2Client(m_g2.texArr, GL_TEXTURE_COORD_ARRAY, false);
            g2Client(m_g2.colArr, GL_COLOR_ARRAY, false);
            g2Client(m_g2.vtxArr, GL_VERTEX_ARRAY, false);
            glBindTexture(GL_TEXTURE_2D, 0);
            g2Cap(m_g2.tex2d, GL_TEXTURE_2D, false);
            g2Cap(m_g2.alphaOn, GL_ALPHA_TEST, false);
            g2Cap(m_g2.fogOn, GL_FOG, false);
            g2Shade(true);
            // Leave the shared context in the renderer's normal 3D state. Each
            // subsequent screen primitive applies its own depth state above.
            g2Cap(m_g2.depthTest, GL_DEPTH_TEST, true);
            g2DepthMask(true);
            g2DepthFunc(GL_LEQUAL);
            glEnable(GL_BLEND);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        }
        lap(6);
        if (timing)
        {
            ++m_screenTiming.draws;
            m_screenTiming.vertices += static_cast<std::uint32_t>(vertices.size());
        }
    }
    void removeTextureLocked(std::string_view name)
    {
        leaveMesh();
        flushScreenBatch();
        const std::string key = textureKey(name);
        dropSmallTexture(key);
        auto found = m_textures.find(key);
        if (found == m_textures.end())
            return;
        const auto target = m_targets.find(key);
        if (target != m_targets.end())
        {
            if (m_target == &target->second)
            {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glFrontFace(GL_CCW);
                m_target = nullptr;
                ps2xVitaGlMutex().unlock();
            }
            glFinish();
            if (target->second.framebuffer)
                glDeleteFramebuffers(1, &target->second.framebuffer);
            m_targets.erase(target);
        }
        if (found->second)
        {
            // Texture retirement can happen after its last CPU-side reference
            // is dropped while an earlier draw still references the GL object.
            // Wait for submitted work before deleting it; this path is used
            // for unload/replacement, not for per-frame draws.
            glFinish();
            glDeleteTextures(1, &found->second);
        }
        m_textures.erase(found);
        m_textureSizes.erase(key);
    }

    void removeMeshLocked(MeshId id)
    {
        leaveMesh();
        end2D();
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;
        deleteParts(found->second);
        m_meshes.erase(found);
    }

    bool m_open = false;
    bool m_ownsWindow = false;
    bool m_in3D = false;
    bool m_gpuSkinEnabled = false;
    std::uint32_t m_instanceFrame = 0; // counts beginFrame calls, for DrawPart::Instance::usedFrame
    SkinProgram m_skinProgram{};
    std::uint64_t m_gpuSkinReportFrames = 0;
    // Time inside drawScreenPrimitive by phase (P3FES_RESOURCE_DIAG), microseconds since the last report.
    struct ScreenTiming
    {
        std::uint64_t us[7] = {};
        std::uint32_t draws = 0, glDraws = 0, vertices = 0;
        std::uint64_t submitUs = 0;
        std::uint32_t submits = 0;
        std::uint32_t breaks[7] = {}, bigTextureBreaks = 0; // batch breaks by ScreenBatch::whyNot
    };
    ScreenTiming m_screenTiming;
    struct MeshTiming
    {
        std::uint64_t us = 0, regenUs = 0, computeUs = 0;
        std::uint32_t calls = 0, regenVerts = 0, regenSkinnedParts = 0, regenStaticParts = 0;
        std::uint32_t regenSkinnedVerts = 0, regenStaticVerts = 0;
        std::uint32_t reusedParts[2] = {0, 0};
        std::uint64_t hotUs = 0;
        std::uint32_t hotVerts = 0;
        // P3FES_MESH_VERIFY: fast kernel against the scalar reference, per vertex.
        std::uint64_t verifyVerts = 0, verifyColorOff1 = 0, verifyColorBad = 0, verifyUvBad = 0, verifyPosBad = 0;
        float verifyPosMax = 0.0f;
        std::uint32_t reasons[2][512] = {};   // [skinned][mismatch bits]: how often each reason set came up
        // P3FES_MESH_PHASES=1: time by phase of drawMeshImpl (see PhaseClock), parts drawn by path, GL draw calls.
        std::uint64_t phaseUs[8] = {};
        std::uint32_t partsStatic = 0, partsGpuSkin = 0, glDraws = 0, mergedParts = 0, mergedVerts = 0, mergeBatches = 0, mergeReused = 0, mergeBaked = 0, mergeVerifyBad = 0, mergeRefused[7] = {};
    };
    MeshTiming m_meshTiming, m_meshTimingTarget; // display frame / render-target passes
    static std::uint64_t diagNowUs()
    {
#if defined(__vita__)
        return sceKernelGetProcessTimeWide();
#else
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    // ---- GL state shadow of the mesh path (P3FES_MESH_GLCACHE / P3FES_MESH_MERGE) ---------------------------------
    // A run starts at the first mesh draw after any other renderer work and ends in leaveMesh(), which every other
    // entry point reaches through flushScreenBatch() (or calls directly). Inside a run only the mesh path touches GL, so
    // the shadow below is exact; it starts at the baseline state that the old per-draw epilogue used to restore.
    static constexpr GLuint kUnknownBuffer = 0xFFFFFFFFu, kClientMemory = 0xFFFFFFFEu;
    struct GlShadow
    {
        bool on = false, inRun = false, skinTouched = false;
        std::int8_t depthTest = 1, depthMask = 1, cullOn = 0, blendOn = 1, alphaOn = 0, fogOn = 0, tex2d = 0, smooth = 1;
        std::int8_t vtxArr = 0, colArr = 0, texArr = 0;
        std::int8_t eqSub = 0;       // blend equation: 0 ADD, 1 REVERSE_SUBTRACT, -1 unknown
        bool funcSeparate = false;   // alpha factors are (ONE, ZERO), set by a subtract draw
        GLenum cullFace = GL_BACK;
        GLenum funcSrc = GL_SRC_ALPHA, funcDst = GL_ONE_MINUS_SRC_ALPHA; // 0 = unknown
        int alphaFunc = -1, alphaRef = -1;
        FogState fog;
        bool fogKnown = false;
        GLuint arrayBuf = 0, elemBuf = 0, boundTex = kUnknownBuffer, ptrBuf = kUnknownBuffer, texPtrBuf = kUnknownBuffer;
        // everything back to what leaveMesh() restores
        void baseline()
        {
            depthTest = depthMask = blendOn = smooth = 1;
            cullOn = alphaOn = fogOn = tex2d = vtxArr = colArr = texArr = 0;
            // The screen path leaves the blend factors as (SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ZERO): the alpha channel
            // is NOT blended like the colour, and a mesh draw must set its own factors (it writes the frame buffer's alpha
            // the way glBlendFunc(SRC_ALPHA, ONE_MINUS_SRC_ALPHA) does; later draws blend with that alpha). So the first
            // blend call of a run is never skipped. The same for the face to cull: the last run may have left GL_FRONT.
            eqSub = 0; funcSeparate = true;
            funcSrc = funcDst = 0;
            arrayBuf = elemBuf = 0;
            cullFace = 0;
            // The alpha test function / reference and the fog parameters outlive a run in vitaGL and the screen path sets
            // its own: what the shadow remembers from the last run is not what GL holds now (a mesh drawn after the HUD
            // used to keep the HUD's alpha test: the menu backdrop turned translucent).
            alphaFunc = alphaRef = -1;
            fogKnown = false;
            skinTouched = false;
            // ids can be reused and vitaGL keeps a buffer pointer in each array pointer: re-issue them in every run
            boundTex = ptrBuf = texPtrBuf = kUnknownBuffer;
        }
        // something outside the tracked calls changed GL state: forget all of it
        void unknown()
        {
            depthTest = depthMask = cullOn = blendOn = alphaOn = fogOn = tex2d = smooth = vtxArr = colArr = texArr = -1;
            eqSub = -1; funcSeparate = true; funcSrc = funcDst = 0; cullFace = 0;
            alphaFunc = alphaRef = -1; fogKnown = false;
            arrayBuf = elemBuf = boundTex = ptrBuf = texPtrBuf = kUnknownBuffer;
        }
    };
    GlShadow m_gl;
    // Diagnostic (P3FES_AB_MODES names like cache-alpha): which kinds of state the shadow does NOT cache, so that a
    // difference between the cached and the plain path can be traced to one kind.
    static constexpr int kCacheAlpha = 1, kCacheFog = 2, kCacheShade = 4, kCacheDepth = 8, kCacheCull = 16, kCacheBlend = 32,
                         kCacheBind = 64, kCachePointers = 128, kCacheTexture = 256;
    int m_glSkip = 0;
    bool cached(int bit) const { return m_gl.on && !(m_glSkip & bit); }
    std::unordered_map<GLuint, std::array<GLenum, 3>> m_samplerCache; // texture -> wrapS, wrapT, linear; valid inside a run only

    void glsCap(std::int8_t &shadow, GLenum cap, bool on, int bit)
    {
        if (cached(bit) && shadow == static_cast<std::int8_t>(on))
            return;
        if (on) glEnable(cap); else glDisable(cap);
        shadow = static_cast<std::int8_t>(on);
    }
    void glsDepthMask(bool on)
    {
        if (cached(kCacheDepth) && m_gl.depthMask == static_cast<std::int8_t>(on))
            return;
        glDepthMask(on ? GL_TRUE : GL_FALSE);
        m_gl.depthMask = static_cast<std::int8_t>(on);
    }
    void glsClient(std::int8_t &shadow, GLenum array, bool on)
    {
        if (cached(kCachePointers) && shadow == static_cast<std::int8_t>(on))
            return;
        if (on) glEnableClientState(array); else glDisableClientState(array);
        shadow = static_cast<std::int8_t>(on);
    }
    void glsAlpha(const AlphaTestState &alpha)
    {
        if (!alpha.enabled)
        {
            glsCap(m_gl.alphaOn, GL_ALPHA_TEST, false, kCacheAlpha);
            return;
        }
        glsCap(m_gl.alphaOn, GL_ALPHA_TEST, true, kCacheAlpha);
        const int function = alpha.function & 7u; // bit 3 is the FB_ONLY marker, not part of the test
        if (!cached(kCacheAlpha) || m_gl.alphaFunc != function || m_gl.alphaRef != alpha.reference)
        {
            glAlphaFunc(gsAlphaFunc(alpha.function), alpha.reference / 255.0f);
            m_gl.alphaFunc = function;
            m_gl.alphaRef = alpha.reference;
        }
    }
    void glsFog(const FogState &fog)
    {
        if (!fog.enabled)
        {
            glsCap(m_gl.fogOn, GL_FOG, false, kCacheFog);
            return;
        }
        glsCap(m_gl.fogOn, GL_FOG, true, kCacheFog);
        if (!cached(kCacheFog) || !m_gl.fogKnown || m_gl.fog.start != fog.start || m_gl.fog.end != fog.end ||
            m_gl.fog.color != fog.color)
        {
            const GLfloat color[4]{fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f, 1.0f};
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, fog.start);
            glFogf(GL_FOG_END, fog.end);
            glFogfv(GL_FOG_COLOR, color);
            m_gl.fog = fog;
            m_gl.fogKnown = true;
        }
    }
    void glsShade(bool gouraud)
    {
        if (cached(kCacheShade) && m_gl.smooth == static_cast<std::int8_t>(gouraud))
            return;
        glShadeModel(gouraud ? GL_SMOOTH : GL_FLAT);
        m_gl.smooth = static_cast<std::int8_t>(gouraud);
    }
    void glsCull(MeshDrawState::CullMode mode)
    {
        if (mode == MeshDrawState::CullMode::None)
        {
            glsCap(m_gl.cullOn, GL_CULL_FACE, false, kCacheCull);
            return;
        }
        const GLenum face = mode == MeshDrawState::CullMode::Back ? GL_BACK : GL_FRONT;
        glsCap(m_gl.cullOn, GL_CULL_FACE, true, kCacheCull);
        if (!cached(kCacheCull) || m_gl.cullFace != face)
        {
            glCullFace(face);
            m_gl.cullFace = face;
        }
    }
    // Blend state of one part: `enabled` false leaves blending off; `subtract` is the reverse-subtract mode.
    void glsBlend(bool enabled, bool subtract, GLenum src, GLenum dst)
    {
        if (!subtract)
        {
            if (!cached(kCacheBlend) || m_gl.eqSub != 0)
            {
                glBlendEquation(GL_FUNC_ADD);
                m_gl.eqSub = 0;
            }
            if (!enabled)
            {
                glsCap(m_gl.blendOn, GL_BLEND, false, kCacheBlend);
                return;
            }
            glsCap(m_gl.blendOn, GL_BLEND, true, kCacheBlend);
            if (!cached(kCacheBlend) || m_gl.funcSeparate || m_gl.funcSrc != src || m_gl.funcDst != dst)
            {
                glBlendFunc(src, dst);
                m_gl.funcSrc = src;
                m_gl.funcDst = dst;
                m_gl.funcSeparate = false;
            }
            return;
        }
        glsCap(m_gl.blendOn, GL_BLEND, true, kCacheBlend);
        if (!cached(kCacheBlend) || m_gl.eqSub != 1 || !m_gl.funcSeparate || m_gl.funcSrc != src || m_gl.funcDst != dst)
        {
            glBlendEquationSeparate(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
            m_gl.eqSub = 1;
            m_gl.funcSeparate = true;
            m_gl.funcSrc = src;
            m_gl.funcDst = dst;
        }
    }
    void glsBindArray(GLuint buffer)
    {
        if (cached(kCacheBind) && m_gl.arrayBuf == buffer)
            return;
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        m_gl.arrayBuf = buffer;
    }
    void glsBindElements(GLuint buffer)
    {
        if (cached(kCacheBind) && m_gl.elemBuf == buffer)
            return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
        m_gl.elemBuf = buffer;
    }
    // Texture of a part (0: none): enable + bind + sampler + the texture coordinate array. The pointer call for the
    // coordinates is left to the caller (it depends on the bound buffer).
    void glsTexture(GLuint texture, const ResolvedSampler &sampler)
    {
        if (!texture)
        {
            glsClient(m_gl.texArr, GL_TEXTURE_COORD_ARRAY, false);
            glsCap(m_gl.tex2d, GL_TEXTURE_2D, false, kCacheTexture);
            return;
        }
        glsCap(m_gl.tex2d, GL_TEXTURE_2D, true, kCacheTexture);
        if (!cached(kCacheTexture) || m_gl.boundTex != texture)
        {
            glBindTexture(GL_TEXTURE_2D, texture);
            m_gl.boundTex = texture;
        }
        const std::array<GLenum, 3> wanted{sampler.wrapS, sampler.wrapT, sampler.linear ? 1u : 0u};
        bool same = false;
        if (cached(kCacheTexture))
        {
            const auto found = m_samplerCache.find(texture);
            same = found != m_samplerCache.end() && found->second == wanted;
        }
        if (!same)
        {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler.wrapS);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler.wrapT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, sampler.linear ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, sampler.linear ? GL_LINEAR : GL_NEAREST);
            if (cached(kCacheTexture))
                m_samplerCache[texture] = wanted;
        }
        glsClient(m_gl.texArr, GL_TEXTURE_COORD_ARRAY, true);
    }
    // Vertex / colour (and, when textured, coordinate) pointers into the bound array buffer, laid out as GpuVertex.
    void glsPointers(GLuint buffer, bool textured)
    {
        if (!cached(kCachePointers) || m_gl.ptrBuf != buffer)
        {
            glVertexPointer(3, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, position)));
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, color)));
            m_gl.ptrBuf = buffer;
        }
        if (textured && (!cached(kCachePointers) || m_gl.texPtrBuf != buffer))
        {
            glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, uv)));
            m_gl.texPtrBuf = buffer;
        }
    }

    // Puts the baseline state back and draws any pending merged batch. A no-op outside a run.
    void leaveMesh()
    {
        if (!m_gl.inRun)
            return;
        flushMerge(nullptr);
        glsClient(m_gl.vtxArr, GL_VERTEX_ARRAY, false);
        glsClient(m_gl.colArr, GL_COLOR_ARRAY, false);
        glsClient(m_gl.texArr, GL_TEXTURE_COORD_ARRAY, false);
        glColor4ub(255u, 255u, 255u, 255u);
        glsBindElements(0);
        glsBindArray(0);
        glsCap(m_gl.tex2d, GL_TEXTURE_2D, false, kCacheTexture);
        if (m_gl.skinTouched)
        {
            glUseProgram(0);
            for (GLuint attribute = 0; attribute < 6u; ++attribute)
                glDisableVertexAttribArray(attribute);
        }
        glsCap(m_gl.alphaOn, GL_ALPHA_TEST, false, kCacheAlpha);
        glsCap(m_gl.fogOn, GL_FOG, false, kCacheFog);
        glsShade(true);
        glsCap(m_gl.depthTest, GL_DEPTH_TEST, true, kCacheDepth);
        glsDepthMask(true);
        glsBlend(true, false, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glsCap(m_gl.cullOn, GL_CULL_FACE, false, kCacheCull);
        m_gl.inRun = false;
        m_samplerCache.clear();
    }

    // ---- merged draws of small unskinned parts (P3FES_MESH_MERGE) ---------------------------------------------------
    struct MergeSig
    {
        AlphaTestState alpha;
        FogState fog;
        bool gouraud = true, depthTest = true, depthWrite = true;
        MeshDrawState::CullMode cull = MeshDrawState::CullMode::None;
        bool blendEnabled = true, subtract = false;
        GLenum src = GL_SRC_ALPHA, dst = GL_ONE_MINUS_SRC_ALPHA;
        GLuint texture = 0;
        ResolvedSampler sampler;
        bool twice = false; // drawn as two passes: `alpha` passing pixels with the depth state, then `alpha2` with depth writes masked
        AlphaTestState alpha2;
        bool same(const MergeSig &o) const
        {
            if (twice != o.twice || (twice && (alpha2.function != o.alpha2.function || alpha2.reference != o.alpha2.reference)))
                return false;
            if (alpha.enabled != o.alpha.enabled || (alpha.enabled && (alpha.function != o.alpha.function || alpha.reference != o.alpha.reference)))
                return false;
            if (fog.enabled != o.fog.enabled || (fog.enabled && (fog.start != o.fog.start || fog.end != o.fog.end || fog.color != o.fog.color)))
                return false;
            if (gouraud != o.gouraud || depthTest != o.depthTest || depthWrite != o.depthWrite || cull != o.cull ||
                blendEnabled != o.blendEnabled || texture != o.texture)
                return false;
            if (blendEnabled && (subtract != o.subtract || src != o.src || dst != o.dst))
                return false;
            return !texture || sampler == o.sampler;
        }
    };
    struct MergeBatch
    {
        bool enabled = false;
        MergeSig sig;
        std::vector<GpuVertex> verts;
        std::vector<std::uint16_t> indices;
        std::uint32_t parts = 0;
        std::vector<std::array<float, 4>> rects; // two-pass batches: screen boxes (NDC x0 y0 x1 y1) of the parts so far
    };
    MergeBatch m_merge;
    FrameArena m_arena; // arrays of client-memory draws (merged batches, big 2-D batches): see FrameArena

    // Screen box (NDC, grown by a few pixels) of the part's local bounding box under `model`; false when a corner is
    // not in front of the camera (the box is then unbounded) or the camera has no matrices.
    bool screenRect(const DrawPart &part, const Matrix4 &model, std::array<float, 4> &rect) const
    {
        if (!m_camera.useMatrices)
            return false;
        const Matrix4 &view = m_camera.view, &projection = m_camera.projection;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int corner = 0; corner < 8; ++corner)
        {
            const float p[3] = {part.bounds[(corner & 1) ? 3 : 0], part.bounds[(corner & 2) ? 4 : 1], part.bounds[(corner & 4) ? 5 : 2]};
            float w[3], v[3], c[4];
            for (int r = 0; r < 3; ++r)
                w[r] = model[r] * p[0] + model[4 + r] * p[1] + model[8 + r] * p[2] + model[12 + r];
            for (int r = 0; r < 3; ++r)
                v[r] = view[r] * w[0] + view[4 + r] * w[1] + view[8 + r] * w[2] + view[12 + r];
            for (int r = 0; r < 4; ++r)
                c[r] = projection[r] * v[0] + projection[4 + r] * v[1] + projection[8 + r] * v[2] + projection[12 + r];
            if (!(c[3] > 1.0e-4f))
                return false;
            const float x = c[0] / c[3], y = c[1] / c[3];
            x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        constexpr float kMargin = 0.01f; // 2.4 pixels of a 960-wide picture
        rect = {x0 - kMargin, y0 - kMargin, x1 + kMargin, y1 + kMargin};
        return true;
    }

    // Adds the part to the pending batch (drawing the batch first when the state differs). False: the part is not
    // eligible and nothing was changed. `model` is the matrix currently pushed by drawMeshImpl.
    bool tryMergePart(DrawPart &part, const Matrix4 &model, const MeshDrawState &state)
    {
        MeshTiming &refused = m_target ? m_meshTimingTarget : m_meshTiming;
        // refusal reasons, counted for the diagnostic line: 0 skinned, 1 too many vertices / no triangle list, 2 matrix
        // not affine, 3 dual pass, 4 environment pass, 5 drawn twice (alpha test), 6 lights the kernel cannot do
        if (part.skinned)
        {
            ++refused.mergeRefused[0];
            return false;
        }
        if (part.cpuIndices.empty() || part.gpuVertices.size() != part.sourceVertices.size())
        {
            ++refused.mergeRefused[1];
            return false;
        }
        const std::size_t count = part.sourceVertices.size();
        if (count > meshMergeMaxVerts() || !meshFastRegenEnabled())
        {
            ++refused.mergeRefused[1];
            return false;
        }
        if (model[3] != 0.0f || model[7] != 0.0f || model[11] != 0.0f || model[15] != 1.0f)
        {
            ++refused.mergeRefused[2];
            return false;
        }
        const std::uint32_t material = part.materialIndex;
        if (material < state.materialDualPasses.size() && state.materialDualPasses[material].enabled &&
            !state.materialDualPasses[material].textureName.empty())
        {
            ++refused.mergeRefused[3];
            return false;
        }
        if (material < state.materialEnvPasses.size() && state.materialEnvPasses[material].enabled && part.hasNormals &&
            !state.materialEnvPasses[material].textureName.empty())
        {
            ++refused.mergeRefused[4];
            return false;
        }
        MergeSig sig;
        sig.alpha = state.alphaTest;
        std::array<float, 4> rect{};
        if (alphaTestFbOnly(state.alphaTest))
        {
            if (alphaTestFailPass(state.alphaTest).function != 0u)
            {
                // Drawn twice. P1(A) P2(A) P1(B) P2(B) and P1(A) P1(B) P2(A) P2(B) differ only where A's second pass and
                // B's first pass touch the same pixel, so a part joins a batch of two-pass parts only when its screen box
                // is clear of theirs (the box is that of the part's local bounding box).
                if (!meshMergeTwiceEnabled() || !part.hasBounds || !state.depthTest || !screenRect(part, model, rect))
                {
                    ++refused.mergeRefused[5];
                    return false;
                }
                sig.twice = true;
                sig.alpha = alphaTestPassOne(state.alphaTest);
                sig.alpha2 = alphaTestFailPass(state.alphaTest);
            }
            else
                sig.alpha = alphaTestSinglePass(state.alphaTest);
        }
        const auto &color = material < state.materialColors.size() ? state.materialColors[material] : part.color;
        const MeshDrawState::UvTransform defaultUv{};
        const MeshDrawState::UvTransform &uv =
            material < state.materialUvTransforms.size() ? state.materialUvTransforms[material] : defaultUv;
        // The same inputs as the last bake of this part: its world-space vertices can be copied.
        const bool reusable = part.mergeKey.valid && part.mergeVerts.size() == count &&
            part.mergeKey.mismatch(model, part.material, color, part.lighting, part.hasPrelight, state, uv) == 0u;
        detail::RegenContext context;
        if (!reusable && !detail::prepareRegenContext(context, model, state.skinPalette, part.material, color, part.lighting,
                                                      part.hasPrelight, state, uv))
        {
            ++refused.mergeRefused[6];
            return false;
        }
        sig.fog = state.fog;
        sig.gouraud = state.gouraudShading;
        sig.depthTest = state.depthTest;
        sig.depthWrite = state.depthWrite;
        sig.cull = state.cullMode;
        sig.blendEnabled = state.blendEnabled;
        if (state.blendEnabled)
        {
            const MeshDrawState::BlendState *blend = material < state.materialBlends.size() ? &state.materialBlends[material] : nullptr;
            if (blend && blend->enabled)
            {
                sig.subtract = blend->subtract;
                sig.src = rwBlendFactor(blend->src);
                sig.dst = rwBlendFactor(blend->dst);
            }
        }
        const auto &textureName = material < state.materialTextures.size() ? state.materialTextures[material] : part.textureName;
        if (!textureName.empty())
        {
            if (const GLuint id = lookupTexture(textureName))
            {
                sig.texture = id;
                sig.sampler = resolveMeshSampler(state, material < state.materialSamplers.size() ? &state.materialSamplers[material] : nullptr);
            }
        }
        bool flushFirst = m_merge.parts && (!sig.same(m_merge.sig) || m_merge.verts.size() + count > 65535u);
        if (m_merge.parts && !flushFirst && sig.twice)
            for (const auto &known : m_merge.rects)
                if (rect[0] < known[2] && known[0] < rect[2] && rect[1] < known[3] && known[1] < rect[3])
                {
                    flushFirst = true;
                    break;
                }
        static int rectTrace = [] { const char *v = std::getenv("P3FES_MERGE_RECT_TRACE"); return v ? std::atoi(v) : 0; }();
        if (rectTrace > 0 && sig.twice && m_instanceFrame > 800)
        {
            --rectTrace;
            std::fprintf(stderr, "[merge-rect] f=%u part verts=%zu rect=(%.3f,%.3f)-(%.3f,%.3f) batch parts=%u rects=%zu flushFirst=%d depthTest=%d depthWrite=%d blend=%d model.t=(%.1f,%.1f,%.1f) bounds=(%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n",
                         m_instanceFrame, count, rect[0], rect[1], rect[2], rect[3], m_merge.parts, m_merge.rects.size(), flushFirst ? 1 : 0,
                         sig.depthTest, sig.depthWrite, sig.blendEnabled, model[12], model[13], model[14],
                         part.bounds[0], part.bounds[1], part.bounds[2], part.bounds[3], part.bounds[4], part.bounds[5]);
        }
        if (flushFirst)
            flushMerge(&model);
        if (!m_merge.parts)
            m_merge.sig = sig;
        if (sig.twice)
            m_merge.rects.push_back(rect);
        const std::size_t base = m_merge.verts.size();
        m_merge.verts.resize(base + count);
        GpuVertex *out = m_merge.verts.data() + base;
        static const bool verify = [] { const char *v = std::getenv("P3FES_MERGE_VERIFY"); return v && *v == '1'; }();
        const auto bake = [&](GpuVertex *destination) {
            detail::RegenContext fresh;
            const detail::RegenContext &ctx = reusable ? (detail::prepareRegenContext(fresh, model, state.skinPalette, part.material, color,
                                                                                      part.lighting, part.hasPrelight, state, uv), fresh)
                                                       : context;
            detail::regenerateVertices(ctx, false, nullptr, part.sourceVertices.data(), destination, count);
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto &p = part.sourceVertices[i].position;
                destination[i].position[0] = model[0] * p[0] + model[4] * p[1] + model[8] * p[2] + model[12];
                destination[i].position[1] = model[1] * p[0] + model[5] * p[1] + model[9] * p[2] + model[13];
                destination[i].position[2] = model[2] * p[0] + model[6] * p[1] + model[10] * p[2] + model[14];
            }
        };
        MeshTiming &mt = m_target ? m_meshTimingTarget : m_meshTiming;
        if (reusable)
        {
            std::memcpy(out, part.mergeVerts.data(), count * sizeof(GpuVertex));
            ++mt.mergeReused;
            if (verify)
            {
                std::vector<GpuVertex> check(count);
                bake(check.data());
                if (std::memcmp(check.data(), out, count * sizeof(GpuVertex)) != 0)
                    ++mt.mergeVerifyBad;
            }
        }
        else
        {
            bake(out);
            ++mt.mergeBaked;
            const std::size_t held = part.mergeVerts.capacity() * sizeof(GpuVertex);
            if (g_mergeCacheBytes - std::min(g_mergeCacheBytes, held) + count * sizeof(GpuVertex) <= meshMergeCacheBytes())
            {
                g_mergeCacheBytes = g_mergeCacheBytes - std::min(g_mergeCacheBytes, held);
                part.mergeVerts.assign(out, out + count);
                g_mergeCacheBytes += part.mergeVerts.capacity() * sizeof(GpuVertex);
                part.mergeKey.capture(model, part.material, color, part.lighting, part.hasPrelight, state, uv);
            }
            else
                part.mergeKey.invalidate();
        }
        const std::size_t firstIndex = m_merge.indices.size();
        m_merge.indices.resize(firstIndex + part.cpuIndices.size());
        for (std::size_t i = 0; i < part.cpuIndices.size(); ++i)
            m_merge.indices[firstIndex + i] = static_cast<std::uint16_t>(part.cpuIndices[i] + base);
        ++m_merge.parts;
        MeshTiming &t = m_target ? m_meshTimingTarget : m_meshTiming;
        ++t.mergedParts;
        t.mergedVerts += static_cast<std::uint32_t>(count);
        return true;
    }

    // Draws the pending batch. `activeModel` is the matrix drawMeshImpl has pushed (taken off for the draw, put back).
    void flushMerge(const Matrix4 *activeModel)
    {
        if (!m_merge.parts)
            return;
        if (activeModel)
            glPopMatrix();
        const MergeSig &sig = m_merge.sig;
        // The GPU reads these arrays when the scene renders, long after this call: they go to memory that is not
        // reused for several frames (a std::vector refilled by the next batch gave wrong triangles).
        const std::size_t vertexBytes = m_merge.verts.size() * sizeof(GpuVertex);
        const std::size_t indexBytes = m_merge.indices.size() * sizeof(std::uint16_t);
        // P3FES_MERGE_INPLACE=1 (diagnostic): the old behaviour, draw from the batch's own vectors, and scribble over
        // them right after the draw call, as the next batch would. If vitaGL copies the arrays at the draw call this
        // changes nothing; if it keeps pointing at them the merged geometry breaks.
        static const bool inPlace = [] { const char *v = std::getenv("P3FES_MERGE_INPLACE"); return v && *v == '1'; }();
        GpuVertex *vertices = inPlace ? m_merge.verts.data()
                                      : static_cast<GpuVertex *>(m_arena.copy(m_merge.verts.data(), vertexBytes));
        std::uint16_t *indices = inPlace ? m_merge.indices.data()
                                         : static_cast<std::uint16_t *>(m_arena.copy(m_merge.indices.data(), indexBytes));
        if (!vertices || !indices)
        {
            m_merge.verts.clear();
            m_merge.indices.clear();
            m_merge.rects.clear();
            m_merge.parts = 0;
            if (activeModel)
            {
                glPushMatrix();
                glMultMatrixf(activeModel->data());
            }
            return; // out of memory: the batch is lost rather than drawn from memory that will change
        }
        glsAlpha(sig.alpha);
        glsFog(sig.fog);
        glsShade(sig.gouraud);
        glsCap(m_gl.depthTest, GL_DEPTH_TEST, sig.depthTest, kCacheDepth);
        glsDepthMask(sig.depthWrite);
        glsCull(sig.cull);
        glsBlend(sig.blendEnabled, sig.blendEnabled && sig.subtract, sig.src, sig.dst);
        glsBindElements(0);
        glsBindArray(0);
        glsClient(m_gl.vtxArr, GL_VERTEX_ARRAY, true);
        glsClient(m_gl.colArr, GL_COLOR_ARRAY, true);
        glVertexPointer(3, GL_FLOAT, sizeof(GpuVertex), &vertices[0].position[0]);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GpuVertex), &vertices[0].color[0]);
        m_gl.ptrBuf = kClientMemory;
        glsTexture(sig.texture, sig.sampler);
        if (sig.texture)
        {
            glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), &vertices[0].uv[0]);
            m_gl.texPtrBuf = kClientMemory;
        }
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_merge.indices.size()), GL_UNSIGNED_SHORT, indices);
        if (sig.twice)
        {
            // the pixels that failed the test: still drawn, Z not written (as the unmerged two-pass draw does)
            glsAlpha(sig.alpha2);
            glsDepthMask(false);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(m_merge.indices.size()), GL_UNSIGNED_SHORT, indices);
            glsDepthMask(sig.depthWrite);
            glsAlpha(sig.alpha);
        }
        if (inPlace)
        {
            std::memset(m_merge.indices.data(), 0, indexBytes);
            std::memset(m_merge.verts.data(), 0, vertexBytes);
        }
        MeshTiming &t = m_target ? m_meshTimingTarget : m_meshTiming;
        ++t.mergeBatches;
        m_merge.verts.clear();
        m_merge.indices.clear();
        m_merge.rects.clear();
        m_merge.parts = 0;
        if (activeModel)
        {
            glPushMatrix();
            glMultMatrixf(activeModel->data());
        }
    }

    // Phase timing of drawMeshImpl (P3FES_RESOURCE_DIAG=1 and P3FES_MESH_PHASES=1): lap(i) adds the time since the previous
    // lap to bucket i of the current MeshTiming. 0 prologue, 1 per-part binds + blend, 2 regen (counted by its own
    // timer, here only the remainder), 3 pointers + texture + sampler, 4 glDrawElements, 5 dual/env + blend reset,
    // 6 epilogue, 7 GPU-skinned part.
    struct PhaseClock
    {
        MeshTiming *timing = nullptr;
        std::uint64_t last = 0;
        void lap(int bucket)
        {
            if (!timing) return;
            const std::uint64_t now = diagNowUs();
            timing->phaseUs[bucket] += now - last;
            last = now;
        }
        void skip() { if (timing) last = diagNowUs(); }
    };
    std::uint64_t m_gpuSkinDraws = 0;
    std::uint64_t m_gpuSkinCpuFallbacks = 0;
    std::array<std::uint64_t, 5> m_gpuSkinFallbackReasons{};
    Camera m_camera{};
    struct Target
    {
        GLuint framebuffer = 0;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<std::string, Target> m_targets; // colour textures live in m_textures
    Target *m_target = nullptr;
    int m_widescreen = 0;
    WorldPlacement m_worldPlacement = WorldPlacement::Full;
    ScreenPlacement m_screenPlacement = ScreenPlacement::Centre;

template <typename Viewport, typename Fit>
    Viewport screenViewport(float sourceWidth, float sourceHeight, Viewport full, Fit fit) const
    {
        if (m_widescreen == 1 || (m_widescreen == 2 && (m_screenPlacement == ScreenPlacement::Stretch || m_screenPlacement == ScreenPlacement::Wide)))
            return full;
        Viewport viewport = fit(sourceWidth, sourceHeight);
        if (m_widescreen == 2 && m_screenPlacement == ScreenPlacement::Left)
            viewport.x = full.x;
        else if (m_widescreen == 2 && m_screenPlacement == ScreenPlacement::Right)
            viewport.x = full.x + full.width - viewport.width;
        return viewport;
    }
    std::vector<std::pair<float, std::uint32_t>> m_screenZLayers; // Z -> GEQUAL draws so far this frame

    static bool screenZBiasEnabled()
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_SCREEN_Z_BIAS");
            return !value || *value != '0';
        }();
        return enabled;
    }
    static bool targetFlipEnabled()
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_TARGET_FLIP");
            return !value || *value != '0';
        }();
        return enabled;
    }

    FitViewport worldViewport() const
    {
        if (m_target)
            return {0, 0, m_target->width, m_target->height};
        if (m_widescreen == 2 && m_worldPlacement != WorldPlacement::Full)
        {
            const FitViewport centre = fitViewport(640.0f, 448.0f);
            if (m_worldPlacement == WorldPlacement::Centre)
                return centre;
            // Edge passes: magnify the picture so that its outermost kEdgeColumns pixel columns span the margin.
            constexpr float kEdgeColumns = 8.0f;
            const int hostWidth = std::max(GetScreenWidth(), 1);
            const int margin = std::max(m_worldPlacement == WorldPlacement::EdgeLeft ? centre.x : hostWidth - centre.x - centre.width, 1);
            const int width = std::clamp(static_cast<int>(static_cast<float>(margin) * static_cast<float>(centre.width) / kEdgeColumns), centre.width, 16000);
            return {m_worldPlacement == WorldPlacement::EdgeLeft ? 0 : hostWidth - width, centre.y, width, centre.height};
        }
        if (m_widescreen != 0)
            return {0, 0, std::max(GetScreenWidth(), 1), std::max(GetScreenHeight(), 1)};
        return fitViewport(640.0f, 448.0f);
    }
    std::unordered_map<MeshId, std::vector<DrawPart>> m_meshes;
    std::unordered_map<std::string, GLuint> m_textures;
    std::unordered_map<std::string, std::array<std::uint32_t, 2>> m_textureSizes;
    std::string m_keyScratch;
    // GL texture of a name (case-insensitive), 0 when there is none; no allocation in the steady state.
    GLuint lookupTexture(std::string_view name)
    {
        m_keyScratch.assign(name.data(), name.size());
        for (char &ch : m_keyScratch)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const auto found = m_textures.find(m_keyScratch);
        return found == m_textures.end() ? 0u : found->second;
    }

    // Edge placements draw into a magnified viewport and keep only the margin next to the 4:3 picture.
    void applyWorldScissor()
    {
        if (m_widescreen == 2 && !m_target &&
            (m_worldPlacement == WorldPlacement::EdgeLeft || m_worldPlacement == WorldPlacement::EdgeRight))
        {
            const FitViewport centre = fitViewport(640.0f, 448.0f);
            const int hostWidth = std::max(GetScreenWidth(), 1), hostHeight = std::max(GetScreenHeight(), 1);
            glEnable(GL_SCISSOR_TEST);
            if (m_worldPlacement == WorldPlacement::EdgeLeft)
                glScissor(0, 0, centre.x, hostHeight);
            else
                glScissor(centre.x + centre.width, 0, hostWidth - centre.x - centre.width, hostHeight);
        }
        else
            glDisable(GL_SCISSOR_TEST);
    }

    void apply3D()
    {
        const FitViewport viewport = worldViewport();
        glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        applyWorldScissor();
        glMatrixMode(GL_PROJECTION);
        if (m_camera.useMatrices)
        {
            glLoadMatrixf(m_camera.projection.data());
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(m_camera.view.data());
        }
        else
        {
            const float aspect = 640.0f / 448.0f;
            constexpr float nearPlane = 0.1f;
            constexpr float farPlane = 10000.0f;
            const float radians = m_camera.fovy * 0.008726646259971648f;
            const float top = nearPlane * std::tan(std::max(radians, 0.01f));
            const float right = top * aspect;
            glLoadIdentity();
            glFrustumf(-right, right, -top, top, nearPlane, farPlane);
            const std::array<float, 16> view = lookAt(m_camera);
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(view.data());
        }
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        m_in3D = true;
    }
};

// ---- P3FES_AB_FRAME: pixel-exact A/B comparison of the fast paths, inside one process ----------------------------------
// Comparing screenshots of two runs does not work for this game: the scene is never in the same state twice. This
// wrapper records every renderer call of a chosen frame, lets the frame go through as usual (the TEST pass, drawn with
// the switches the environment asked for), reads the picture back, then replays the recorded calls on the same
// textures and meshes with every fast path off (the REFERENCE pass), with the test switches again (TEST2) and with the
// fast paths off again (REF2: the noise of the platform, to tell from a difference of the paths), and reports how the
// pictures differ. The pictures to compare are REF and TEST2: both are replays; the live frame can differ in what the
// game changed in the frame without going through this interface (particle textures) and in what is shown when.
//   P3FES_AB_FRAME=<first beginFrame number>  P3FES_AB_EVERY=<frames between comparisons, default 240>
//   P3FES_AB_COUNT=<comparisons, default 4>   P3FES_AB_DUMP=1 writes ux0:data/p3fes/ab-<n>-{test,ref}.p3fb
// The replayed frames are presented too (the screen flickers once): a diagnostic, not for play.
class AbRenderer final : public Renderer
{
public:
    explicit AbRenderer(std::unique_ptr<VitaGlRenderer> inner) : m_in(std::move(inner))
    {
        const auto number = [](const char *name, long fallback) {
            const char *value = std::getenv(name);
            return value ? std::strtol(value, nullptr, 10) : fallback;
        };
        m_first = number("P3FES_AB_FRAME", 1000);
        m_every = std::max(number("P3FES_AB_EVERY", 240), 1l);
        m_count = number("P3FES_AB_COUNT", 4);
        const char *dump = std::getenv("P3FES_AB_DUMP");
        m_dump = dump && *dump == '1';
        m_runs = static_cast<int>(std::clamp(number("P3FES_AB_RUNS", 5), 1l, 50l));
        // P3FES_AB_MODES=cache,cache+merge,cache+merge+s2d : the switch sets to compare with the reference
        // (default: what the environment switched on).
        if (const char *list = std::getenv("P3FES_AB_MODES"))
        {
            std::string text(list);
            std::size_t from = 0;
            while (from <= text.size())
            {
                const std::size_t comma = std::min(text.find(',', from), text.size());
                const std::string name = text.substr(from, comma - from);
                VitaGlRenderer::Modes modes;
                modes.glCache = name.find("cache") != std::string::npos;
                modes.merge = name.find("merge") != std::string::npos;
                modes.screenCache = name.find("s2d") != std::string::npos;
                // "-alpha" etc.: that kind of state is not cached (see VitaGlRenderer::kCache*)
                for (const auto &[word, bit] : {std::pair<const char *, int>{"-alpha", 1}, {"-fog", 2}, {"-shade", 4}, {"-depth", 8},
                                                {"-cull", 16}, {"-blend", 32}, {"-bind", 64}, {"-ptr", 128}, {"-tex", 256}})
                    if (name.find(word) != std::string::npos)
                        modes.skip |= bit;
                if (!name.empty())
                    m_tests.emplace_back(name, modes);
                from = comma + 1;
            }
        }
    }

    bool open(int w, int h, const char *title, std::string &error) override { return m_in->open(w, h, title, error); }
    void close() override { m_in->close(); }
    bool shouldClose() const override { return m_in->shouldClose(); }
    void setTargetFps(int fps) override { m_in->setTargetFps(fps); }
    bool uploadTexture(const TextureData &t, std::string &error) override { return m_in->uploadTexture(t, error); }
    void removeTexture(std::string_view name) override { m_in->removeTexture(name); }
    bool uploadMesh(MeshId id, const MeshData &mesh, std::string &error) override { return m_in->uploadMesh(id, mesh, error); }
    void removeMesh(MeshId id) override { m_in->removeMesh(id); }
    void beginTextureTarget(std::string_view name, int w, int h) override
    {
        if (m_recording)
            m_poisoned = true; // not replayable
        m_in->beginTextureTarget(name, w, h);
    }
    void endTextureTarget() override { m_in->endTextureTarget(); }
    void setWidescreen(int mode) override
    {
        m_widescreen = mode;
        if (m_recording) record(Op::Widescreen).a = mode;
        m_in->setWidescreen(mode);
    }
    void setScreenPlacement(ScreenPlacement placement) override
    {
        m_screenPlacement = placement;
        if (m_recording) record(Op::ScreenPlacement).a = static_cast<int>(placement);
        m_in->setScreenPlacement(placement);
    }
    bool setWorldPlacement(WorldPlacement placement) override
    {
        const bool supported = m_in->setWorldPlacement(placement);
        m_worldPlacement = placement;
        if (m_recording) record(Op::WorldPlacement).a = static_cast<int>(placement);
        return supported;
    }
    bool captureNextFrame(const std::string &path) override { return m_in->captureNextFrame(path); }
    bool snapshotFramebuffer(std::string_view name, int tw, int th, int sw, int sh) override
    {
        if (m_recording)
            m_poisoned = true;
        return m_in->snapshotFramebuffer(name, tw, th, sw, sh);
    }

    void beginFrame(const Camera &camera, ClearColor clear) override
    {
        ++m_frame;
        m_recording = m_done < m_count && m_frame >= m_first && (m_frame - m_first) % m_every == 0;
        m_poisoned = false;
        m_ops.clear();
        m_meshes = 0;
        if (m_recording)
        {
            m_startCamera = camera;
            m_startClear = clear;
            m_startWidescreen = m_widescreen;
            m_startScreenPlacement = m_screenPlacement;
            m_startWorldPlacement = m_worldPlacement;
        }
        m_in->beginFrame(camera, clear);
    }
    void setCamera(const Camera &camera) override
    {
        if (m_recording)
            record(Op::Camera).camera = camera;
        m_in->setCamera(camera);
    }
    void clear(ClearColor color, bool colorBuffer, bool depthBuffer) override
    {
        if (m_recording)
        {
            Op &op = record(Op::Clear);
            op.clear = color;
            op.a = colorBuffer;
            op.b = depthBuffer;
        }
        m_in->clear(color, colorBuffer, depthBuffer);
    }
    void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state = {}) override
    {
        if (m_recording)
        {
            Op &op = record(Op::Mesh);
            op.id = id;
            op.model = model;
            op.state = state;
            ++m_meshes;
        }
        m_in->drawMesh(id, model, state);
    }
    void drawWorldLines(const std::vector<Vertex> &vertices, const Matrix4 &model, std::string_view textureName = {},
                        const MeshDrawState &state = {}) override
    {
        if (m_recording)
        {
            Op &op = record(Op::WorldLines);
            op.vertices = vertices;
            op.model = model;
            op.name = std::string(textureName);
            op.state = state;
        }
        m_in->drawWorldLines(vertices, model, textureName, state);
    }
    void drawScreenTriangles(const std::vector<ScreenVertex> &vertices, float w, float h, std::string_view textureName = {},
                             const ScreenDrawState &state = {}) override
    {
        if (m_recording)
        {
            Op &op = record(Op::ScreenTriangles);
            op.screen = vertices;
            op.w = w;
            op.h = h;
            op.name = std::string(textureName);
            op.screenState = state;
        }
        m_in->drawScreenTriangles(vertices, w, h, textureName, state);
    }
    void drawScreenLines(const std::vector<ScreenVertex> &vertices, float w, float h, std::string_view textureName = {},
                         const ScreenDrawState &state = {}) override
    {
        if (m_recording)
        {
            Op &op = record(Op::ScreenLines);
            op.screen = vertices;
            op.w = w;
            op.h = h;
            op.name = std::string(textureName);
            op.screenState = state;
        }
        m_in->drawScreenLines(vertices, w, h, textureName, state);
    }
    std::string drainDiagReport() override { return m_in->drainDiagReport(); }

    void endFrame() override
    {
        if (!m_recording)
        {
            m_in->endFrame();
            return;
        }
        m_recording = false;
        const VitaGlRenderer::Modes live = m_in->modes();
        Image a;
        m_in->readFramebuffer(a.rgba, a.w, a.h);
        m_in->endFrame();
        ++m_done; // numbers the screenshots of this comparison
        --m_done;
        if (m_poisoned || m_ops.empty())
        {
            std::fprintf(stderr, "[ab] frame %ld: skipped (%s)\n", m_frame, m_poisoned ? "render target or snapshot inside the frame" : "no draws");
            return;
        }
        ++m_done;
        std::vector<std::pair<std::string, VitaGlRenderer::Modes>> tests = m_tests;
        if (tests.empty())
            tests.emplace_back("live", live);
        std::fprintf(stderr, "[ab] frame %ld #%ld: ops=%u meshes=%u\n", m_frame, m_done, static_cast<unsigned>(m_ops.size()), m_meshes);
        Image reference = replay(VitaGlRenderer::Modes{}, nullptr, true);
        shot("ref");
        Image reference2 = replay(VitaGlRenderer::Modes{}, nullptr, true);
        shot("ref2");
        std::fprintf(stderr, "[ab]   ref-vs-ref2 (platform noise): ");
        report("", reference, reference2);
        std::fprintf(stderr, "\n[ab]   live(%s%s%s)-vs-ref: ", live.glCache ? "cache " : "", live.merge ? "merge " : "", live.screenCache ? "s2d" : "");
        report("", a, reference);
        std::fprintf(stderr, "\n");
        if (m_dump)
        {
            dump("test", a);
            dump("ref", reference);
        }
        for (std::size_t t = 0; t < tests.size(); ++t)
        {
            Image image = replay(tests[t].second, nullptr, true);
            shot(("m" + std::to_string(t)).c_str());
            std::fprintf(stderr, "[ab]   ref-vs-%s: ", tests[t].first.c_str());
            report("", reference, image);
            std::fprintf(stderr, "\n");
            if (countOver(reference, image, 32) > 50)
                bisect(tests[t].first.c_str(), tests[t].second, reference);
            if (m_dump)
                dump(("m" + std::to_string(t)).c_str(), image);
        }
        // Host-thread cost of the same frame per mode, median of m_runs replays (microseconds).
        std::vector<std::pair<std::string, VitaGlRenderer::Modes>> timed;
        timed.emplace_back("ref", VitaGlRenderer::Modes{});
        for (const auto &t : tests)
            timed.push_back(t);
        std::vector<std::vector<Timing>> samples(timed.size());
        for (int run = 0; run < m_runs; ++run)
            for (std::size_t t = 0; t < timed.size(); ++t)
            {
                Timing timing;
                replay(timed[t].second, &timing, false);
                samples[t].push_back(timing);
            }
        for (std::size_t t = 0; t < timed.size(); ++t)
        {
            const auto median = [&](double Timing::*field) {
                std::vector<double> v;
                for (const Timing &x : samples[t]) v.push_back(x.*field);
                std::sort(v.begin(), v.end());
                return v[v.size() / 2];
            };
            std::fprintf(stderr, "[ab]   cost %-14s total=%.2f ms: begin=%.2f mesh=%.2f screen=%.2f other=%.2f (runs=%d)\n",
                         timed[t].first.c_str(), median(&Timing::total) / 1000.0, median(&Timing::begin) / 1000.0,
                         median(&Timing::mesh) / 1000.0, median(&Timing::screen) / 1000.0, median(&Timing::other) / 1000.0, m_runs);
        }
        m_in->setModes(live);
    }

private:
    struct Op
    {
        enum Kind { Widescreen, ScreenPlacement, WorldPlacement, Camera, Clear, Mesh, WorldLines, ScreenTriangles, ScreenLines } kind;
        int a = 0, b = 0;
        native_renderer::Camera camera;
        ClearColor clear;
        MeshId id = 0;
        Matrix4 model{};
        MeshDrawState state;
        std::vector<Vertex> vertices;
        std::vector<ScreenVertex> screen;
        std::string name;
        float w = 0.0f, h = 0.0f;
        ScreenDrawState screenState;
    };
    struct Image
    {
        std::vector<std::uint8_t> rgba;
        int w = 0, h = 0;
    };

    Op &record(Op::Kind kind)
    {
        m_ops.emplace_back();
        m_ops.back().kind = kind;
        return m_ops.back();
    }

    struct Timing
    {
        double total = 0, begin = 0, mesh = 0, screen = 0, other = 0;
    };
    static double nowUs()
    {
#if defined(__vita__)
        return static_cast<double>(sceKernelGetProcessTimeWide());
#else
        return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }

    // Draws the recorded frame again with the given switches; with `timing` it measures the host thread's cost of the
    // calls (not the swap), with `read` it returns the picture.
    Image replay(const VitaGlRenderer::Modes &modes, Timing *timing, bool read, int testMeshes = -1)
    {
        double mark = nowUs();
        const double start = mark;
        int meshOrdinal = 0;
        const auto lap = [&](double Timing::*field) {
            if (!timing)
                return;
            const double now = nowUs();
            timing->*field += now - mark;
            mark = now;
        };
        m_in->setModes(modes);
        mark = nowUs();
        m_in->setWidescreen(m_startWidescreen);
        m_in->setScreenPlacement(m_startScreenPlacement);
        m_in->setWorldPlacement(m_startWorldPlacement);
        m_in->beginFrame(m_startCamera, m_startClear);
        lap(&Timing::begin);
        for (const Op &op : m_ops)
        {
            switch (op.kind)
            {
            case Op::Widescreen: m_in->setWidescreen(op.a); lap(&Timing::other); break;
            case Op::ScreenPlacement: m_in->setScreenPlacement(static_cast<ScreenPlacement>(op.a)); lap(&Timing::other); break;
            case Op::WorldPlacement: m_in->setWorldPlacement(static_cast<WorldPlacement>(op.a)); lap(&Timing::other); break;
            case Op::Camera: m_in->setCamera(op.camera); lap(&Timing::other); break;
            case Op::Clear: m_in->clear(op.clear, op.a != 0, op.b != 0); lap(&Timing::other); break;
            case Op::Mesh:
                // bisecting: the first `testMeshes` mesh draws use `modes`, the others the reference
                if (testMeshes >= 0 && meshOrdinal++ == testMeshes)
                    m_in->setModes(VitaGlRenderer::Modes{});
                m_in->drawMesh(op.id, op.model, op.state);
                lap(&Timing::mesh);
                break;
            case Op::WorldLines: m_in->drawWorldLines(op.vertices, op.model, op.name, op.state); lap(&Timing::other); break;
            case Op::ScreenTriangles: m_in->drawScreenTriangles(op.screen, op.w, op.h, op.name, op.screenState); lap(&Timing::screen); break;
            case Op::ScreenLines: m_in->drawScreenLines(op.screen, op.w, op.h, op.name, op.screenState); lap(&Timing::screen); break;
            }
        }
        m_in->drainPending();
        lap(&Timing::screen);
        if (timing)
            timing->total = nowUs() - start;
        Image image;
        if (read)
            m_in->readFramebuffer(image.rgba, image.w, image.h);
        m_in->endFrame();
        return image;
    }

    static std::uint64_t countOver(const Image &a, const Image &b, int limit)
    {
        if (a.rgba.size() != b.rgba.size())
            return ~0ull;
        std::uint64_t n = 0;
        for (std::size_t i = 0; i + 3 < a.rgba.size(); i += 4)
        {
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(static_cast<int>(a.rgba[i + static_cast<std::size_t>(c)]) - static_cast<int>(b.rgba[i + static_cast<std::size_t>(c)])));
            n += d > limit;
        }
        return n;
    }

    // Finds the first mesh draw of the frame whose drawing with the test switches (everything before it drawn the same
    // way) already changes the picture, and says what that draw is.
    void bisect(const char *label, const VitaGlRenderer::Modes &modes, const Image &reference)
    {
        std::vector<const Op *> meshes;
        for (const Op &op : m_ops)
            if (op.kind == Op::Mesh)
                meshes.push_back(&op);
        int lo = 0, hi = static_cast<int>(meshes.size());
        while (hi - lo > 1)
        {
            const int mid = (lo + hi) / 2;
            const Image image = replay(modes, nullptr, true, mid);
            if (countOver(reference, image, 32) > 50)
                hi = mid;
            else
                lo = mid;
        }
        const Op &op = *meshes[static_cast<std::size_t>(hi - 1)];
        const MeshDrawState &st = op.state;
        std::fprintf(stderr,
                     "[ab]   bisect %s: first differing mesh draw is #%d of %u: id=%llu depthTest=%d depthWrite=%d cull=%d blend=%d gouraud=%d "
                     "alpha(enabled=%d function=0x%x ref=%u) fog=%d lighting=%d textures=%u blends=%u dual=%u env=%u colors=%u\n",
                     label, hi - 1, static_cast<unsigned>(meshes.size()), static_cast<unsigned long long>(op.id), st.depthTest, st.depthWrite,
                     static_cast<int>(st.cullMode), st.blendEnabled, st.gouraudShading, st.alphaTest.enabled,
                     st.alphaTest.function, st.alphaTest.reference, st.fog.enabled, st.lightingEnabled,
                     static_cast<unsigned>(st.materialTextures.size()), static_cast<unsigned>(st.materialBlends.size()),
                     static_cast<unsigned>(st.materialDualPasses.size()), static_cast<unsigned>(st.materialEnvPasses.size()),
                     static_cast<unsigned>(st.materialColors.size()));
        for (std::size_t b = 0; b < st.materialBlends.size() && b < 6; ++b)
            std::fprintf(stderr, "[ab]     material %u: blend enabled=%d src=%u dst=%u subtract=%d | texture=%s | dual=%d env=%d\n", static_cast<unsigned>(b),
                         st.materialBlends[b].enabled, st.materialBlends[b].src, st.materialBlends[b].dst, st.materialBlends[b].subtract,
                         b < st.materialTextures.size() ? st.materialTextures[b].c_str() : "-",
                         b < st.materialDualPasses.size() ? st.materialDualPasses[b].enabled : 0,
                         b < st.materialEnvPasses.size() ? st.materialEnvPasses[b].enabled : 0);
        m_in->setModes(modes);
    }

    static void report(const char *, const Image &a, const Image &b)
    {
        if (a.w != b.w || a.h != b.h || a.rgba.size() != b.rgba.size())
        {
            std::fprintf(stderr, "sizes differ");
            return;
        }
        std::uint64_t differing = 0, over8 = 0, over32 = 0, sum = 0;
        int max = 0, x0 = a.w, y0 = a.h, x1 = -1, y1 = -1;
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x)
            {
                const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(a.w) + static_cast<std::size_t>(x)) * 4u;
                int d = 0;
                for (int c = 0; c < 3; ++c)
                    d = std::max(d, std::abs(static_cast<int>(a.rgba[i + c]) - static_cast<int>(b.rgba[i + c])));
                if (!d)
                    continue;
                ++differing;
                sum += static_cast<std::uint64_t>(d);
                over8 += d > 8;
                if (d > 32)
                {
                    ++over32;
                    x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y);
                }
                max = std::max(max, d);
            }
        std::fprintf(stderr, "differing=%llu (>8: %llu, >32: %llu) max=%d mean=%.2f bbox32=(%d,%d)-(%d,%d)",
                     static_cast<unsigned long long>(differing), static_cast<unsigned long long>(over8),
                     static_cast<unsigned long long>(over32), max, differing ? static_cast<double>(sum) / static_cast<double>(differing) : 0.0,
                     x0, y0, x1, y1);
    }

    // Vita3K cannot read its framebuffer back (the readback is black): P3FES_AB_SHOT=1 has it save the displayed picture
    // instead (the guest asks for a screenshot by name, see the vita3k-test skill). The pause lets the presenter show it.
    void shot(const char *which) const
    {
#if defined(__vita__)
        static const bool enabled = [] { const char *v = std::getenv("P3FES_AB_SHOT"); return v && *v == '1'; }();
        if (!enabled)
            return;
        sceKernelDelayThread(400000);
        sceClibPrintf("VITA3K_SCREENSHOT ab%ld-%s\n", m_done, which);
        sceKernelDelayThread(400000);
#else
        (void)which;
#endif
    }

    void dump(const char *which, const Image &image) const
    {
        char path[256];
#if defined(__vita__)
        std::snprintf(path, sizeof path, "ux0:data/p3fes/ab-%ld-%s.p3fb", m_done, which);
#else
        std::snprintf(path, sizeof path, "ab-%ld-%s.p3fb", m_done, which);
#endif
        if (FILE *file = std::fopen(path, "wb"))
        {
            const std::uint32_t header[2] = {static_cast<std::uint32_t>(image.w), static_cast<std::uint32_t>(image.h)};
            std::fwrite("P3FB", 1, 4, file);
            std::fwrite(header, sizeof(std::uint32_t), 2, file);
            // GL order is bottom row first; P3FB is top row first.
            for (int y = image.h - 1; y >= 0; --y)
                std::fwrite(image.rgba.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) * 4u, 1,
                            static_cast<std::size_t>(image.w) * 4u, file);
            std::fclose(file);
        }
    }

    std::unique_ptr<VitaGlRenderer> m_in;
    std::vector<std::pair<std::string, VitaGlRenderer::Modes>> m_tests;
    int m_runs = 5;
    long m_first = 1000, m_every = 240, m_count = 4, m_done = 0, m_frame = 0;
    bool m_dump = false, m_recording = false, m_poisoned = false;
    std::vector<Op> m_ops;
    unsigned m_meshes = 0;
    native_renderer::Camera m_startCamera;
    ClearColor m_startClear;
    int m_widescreen = 0, m_startWidescreen = 0;
    ScreenPlacement m_screenPlacement = ScreenPlacement::Centre, m_startScreenPlacement = ScreenPlacement::Centre;
    WorldPlacement m_worldPlacement = WorldPlacement::Full, m_startWorldPlacement = WorldPlacement::Full;
};
} // namespace

std::unique_ptr<Renderer> createPlatformRenderer()
{
    auto renderer = std::make_unique<VitaGlRenderer>();
    if (std::getenv("P3FES_AB_FRAME"))
        return std::make_unique<AbRenderer>(std::move(renderer));
    return renderer;
}
} // namespace p3fes::native_renderer
