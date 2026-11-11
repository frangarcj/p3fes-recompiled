// Vita native renderer: the same GPU-neutral contract as the desktop proof,
// drawn through the vitaGL/GXM context already owned by the Celosia host shim.
#include "renderer.h"
#include "lighting.h"
#include "skinning.h"
#include "uv_transform.h"

#include <raylib.h>
#include <vitaGL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
struct GpuVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    std::uint8_t color[4];
};

struct ScreenGpuVertex
{
    float x, y, z;
    float u, v;
    std::uint8_t r, g, b, a;
};

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
    bool lighting = false;
    bool hasPrelight = false;
    bool hasNormals = false;
    bool skinned = false;
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

void applyMeshSampler(GLuint texture, const MeshDrawState &state,
                      const MeshDrawState::SamplerState *sampler = nullptr)
{
    if (!texture)
        return;
    const bool materialSampler = sampler && sampler->valid;
    const GLenum wrapS = materialSampler ? rwWrapMode(sampler->u) : (state.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    const GLenum wrapT = materialSampler ? rwWrapMode(sampler->v) : (state.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    const bool linear = materialSampler ? sampler->linearFilter : state.linearFilter;
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
        if (part.vertexBuffer)
            glDeleteBuffers(1, &part.vertexBuffer);
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
        m_open = true;
        return true;
    }

    void close() override
    {
        if (!m_open)
            return;
        {
            std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
            for (auto &[id, parts] : m_meshes)
                deleteParts(parts);
            m_meshes.clear();
            for (auto &[name, texture] : m_textures)
                if (texture)
                    glDeleteTextures(1, &texture);
            m_textures.clear();
            m_textureSizes.clear();
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
        const std::string key = textureKey(source.name);
        const auto existing = m_textures.find(key);
        const auto size = m_textureSizes.find(key);
        if (existing != m_textures.end() && size != m_textureSizes.end() &&
            size->second[0] == source.width && size->second[1] == source.height)
        {
            glBindTexture(GL_TEXTURE_2D, existing->second);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(source.width),
                            static_cast<GLsizei>(source.height), GL_RGBA, GL_UNSIGNED_BYTE, source.rgba.data());
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
        return true;
    }

    void removeTexture(std::string_view name) override
    {
        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        removeTextureLocked(name);
    }

    bool uploadMesh(MeshId id, const MeshData &source, std::string &error) override
    {
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
            part.sourceVertices = source.vertices;
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
            std::vector<GpuVertex> vertices(source.vertices.size());
            for (std::size_t i = 0; i < source.vertices.size(); ++i)
            {
                const Vertex &in = source.vertices[i];
                GpuVertex &out = vertices[i];
                std::copy(in.position.begin(), in.position.end(), out.position);
                std::copy(in.normal.begin(), in.normal.end(), out.normal);
                std::copy(in.texCoord.begin(), in.texCoord.end(), out.uv);
                std::copy(in.color.begin(), in.color.end(), out.color);
            }
            part.gpuVertices = vertices;

            glGenBuffers(1, &part.vertexBuffer);
            glBindBuffer(GL_ARRAY_BUFFER, part.vertexBuffer);
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizei>(vertices.size() * sizeof(GpuVertex)),
                         vertices.data(), GL_STATIC_DRAW);
            glGenBuffers(1, &part.indexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.indexBuffer);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizei>(indices.size() * sizeof(std::uint16_t)),
                         indices.data(), GL_STATIC_DRAW);
            part.indexCount = static_cast<GLsizei>(indices.size());
            if (!part.vertexBuffer || !part.indexBuffer)
            {
                error = "vitaGL failed to allocate mesh buffers";
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
        return true;
    }

    void removeMesh(MeshId id) override
    {
        std::lock_guard<std::recursive_mutex> lock(ps2xVitaGlMutex());
        removeMeshLocked(id);
    }

    void beginFrame(const Camera &camera, ClearColor clear) override
    {
        m_camera = camera;
        BeginDrawing(); // holds ps2xVitaGlMutex until EndDrawing
        glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f);
        glClearDepthf(1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE); // RenderWare materials may be two-sided.
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        apply3D();
    }

    void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state) override
    {
        if (!m_in3D)
            apply3D();
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;

        applyAlphaTest(state.alphaTest);
        applyFog(state.fog);
        applyShadeMode(state.gouraudShading);
        if (state.depthTest)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        glDepthMask(state.depthWrite ? GL_TRUE : GL_FALSE);
        if (state.cullMode == MeshDrawState::CullMode::None)
            glDisable(GL_CULL_FACE);
        else
        {
            glEnable(GL_CULL_FACE);
            glCullFace(state.cullMode == MeshDrawState::CullMode::Back ? GL_BACK : GL_FRONT);
        }

        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glMultMatrixf(model.data());
        glEnableClientState(GL_VERTEX_ARRAY);
        for (DrawPart &part : found->second)
        {
            glBindBuffer(GL_ARRAY_BUFFER, part.vertexBuffer);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.indexBuffer);
            const auto &color = part.materialIndex < state.materialColors.size()
                ? state.materialColors[part.materialIndex]
                : part.color;
            const MeshDrawState::BlendState *blend = part.materialIndex < state.materialBlends.size()
                ? &state.materialBlends[part.materialIndex]
                : nullptr;
            glBlendEquation(GL_FUNC_ADD);
            if (!state.blendEnabled)
                glDisable(GL_BLEND);
            else if (blend && blend->enabled)
            {
                glEnable(GL_BLEND);
                glBlendFunc(rwBlendFactor(blend->src), rwBlendFactor(blend->dst));
            }
            else
            {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            }
            if (part.gpuVertices.size() == part.sourceVertices.size())
            {
                const MeshDrawState::UvTransform *uvTransform =
                    part.materialIndex < state.materialUvTransforms.size()
                        ? &state.materialUvTransforms[part.materialIndex]
                        : nullptr;
                for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                {
                    const Vertex posed = part.skinned ? poseSkinVertex(part.sourceVertices[i], state.skinPalette)
                                                      : part.sourceVertices[i];
                    std::copy(posed.position.begin(), posed.position.end(), part.gpuVertices[i].position);
                    std::copy(posed.normal.begin(), posed.normal.end(), part.gpuVertices[i].normal);
                    if (uvTransform && uvTransform->enabled)
                    {
                        const auto uv = transformTexCoord(part.sourceVertices[i].texCoord, *uvTransform);
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
                glBufferSubData(GL_ARRAY_BUFFER, 0,
                                static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                part.gpuVertices.data());
            }
            glVertexPointer(3, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, position)));
            glEnableClientState(GL_COLOR_ARRAY);
            glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, color)));

            auto texture = part.textureName.empty() ? m_textures.end() : m_textures.find(textureKey(part.textureName));
            if (texture != m_textures.end())
            {
                glEnable(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, texture->second);
                const MeshDrawState::SamplerState *sampler = part.materialIndex < state.materialSamplers.size()
                    ? &state.materialSamplers[part.materialIndex] : nullptr;
                applyMeshSampler(texture->second, state, sampler);
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, sizeof(GpuVertex), reinterpret_cast<const void *>(offsetof(GpuVertex, uv)));
            }
            else
            {
                glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                glDisable(GL_TEXTURE_2D);
            }
            glDrawElements(GL_TRIANGLES, part.indexCount, GL_UNSIGNED_SHORT, nullptr);

            const MeshDrawState::DualPass *dual = part.materialIndex < state.materialDualPasses.size()
                ? &state.materialDualPasses[part.materialIndex]
                : nullptr;
            if (dual && dual->enabled && !dual->textureName.empty())
            {
                const auto dualTexture = m_textures.find(textureKey(dual->textureName));
                if (dualTexture != m_textures.end())
                {
                    applyMeshSampler(dualTexture->second, state, &dual->sampler);
                    for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                    {
                        const auto uv = transformTexCoord(part.sourceVertices[i].texCoord, dual->uvTransform);
                        part.gpuVertices[i].uv[0] = uv[0];
                        part.gpuVertices[i].uv[1] = uv[1];
                    }
                    glBufferSubData(GL_ARRAY_BUFFER, 0,
                                    static_cast<GLsizei>(part.gpuVertices.size() * sizeof(GpuVertex)),
                                    part.gpuVertices.data());
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
            glBlendEquation(GL_FUNC_ADD);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4ub(255u, 255u, 255u, 255u);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_TEXTURE_2D);
        glPopMatrix();
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glShadeModel(GL_SMOOTH);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE);
    }

    void drawWorldLines(const std::vector<Vertex> &vertices, const Matrix4 &model,
                        std::string_view textureName, const MeshDrawState &state) override
    {
        if (vertices.size() < 2u)
            return;
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
            std::copy(in.normal.begin(), in.normal.end(), out.normal);
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
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, GL_TRIANGLES);
    }

    void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                         float sourceWidth, float sourceHeight,
                         std::string_view textureName,
                         const ScreenDrawState &state) override
    {
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, GL_LINES);
    }

    void endFrame() override
    {
        glDisable(GL_DEPTH_TEST);
        EndDrawing();
        m_in3D = false;
    }

private:
    void drawScreenPrimitive(const std::vector<ScreenVertex> &vertices,
                             float sourceWidth, float sourceHeight,
                             std::string_view textureName,
                             const ScreenDrawState &state, GLenum primitive)
    {
        if (vertices.empty() || sourceWidth <= 0.0f || sourceHeight <= 0.0f)
            return;
        std::vector<ScreenGpuVertex> gpu;
        gpu.reserve(vertices.size());
        for (const ScreenVertex &v : vertices)
            gpu.push_back({v.x, v.y, v.z, v.u, v.v, v.color[0], v.color[1], v.color[2], v.color[3]});

        applyAlphaTest(state.alphaTest);
        applyFog(state.fog);
        applyShadeMode(state.gouraudShading);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        GLuint texture = 0u;
        if (!textureName.empty())
        {
            const auto found = m_textures.find(textureKey(textureName));
            if (found != m_textures.end())
                texture = found->second;
        }
        if (texture)
        {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, state.clampU ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, state.clampV ? GL_CLAMP_TO_EDGE : GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, state.linearFilter ? GL_LINEAR : GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, state.linearFilter ? GL_LINEAR : GL_NEAREST);
        }
        else
            glDisable(GL_TEXTURE_2D);
        switch (state.blend)
        {
        case ScreenBlend::Opaque:
            glDisable(GL_BLEND);
            break;
        case ScreenBlend::Additive:
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        case ScreenBlend::Subtractive:
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
        default:
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        }
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrthof(0.0f, sourceWidth, sourceHeight, 0.0f, -1.0f, 1.0f);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        if (texture)
        {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(2, GL_FLOAT, sizeof(ScreenGpuVertex), &gpu[0].u);
        }
        else
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(ScreenGpuVertex), &gpu[0].x);
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(ScreenGpuVertex), &gpu[0].r);
        glDrawArrays(primitive, 0, static_cast<GLsizei>(gpu.size()));
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_FOG);
        glShadeModel(GL_SMOOTH);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_in3D = false;
    }
    void removeTextureLocked(std::string_view name)
    {
        const std::string key = textureKey(name);
        auto found = m_textures.find(key);
        if (found == m_textures.end())
            return;
        if (found->second)
            glDeleteTextures(1, &found->second);
        m_textures.erase(found);
        m_textureSizes.erase(key);
    }

    void removeMeshLocked(MeshId id)
    {
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;
        deleteParts(found->second);
        m_meshes.erase(found);
    }

    bool m_open = false;
    bool m_ownsWindow = false;
    bool m_in3D = false;
    Camera m_camera{};
    std::unordered_map<MeshId, std::vector<DrawPart>> m_meshes;
    std::unordered_map<std::string, GLuint> m_textures;
    std::unordered_map<std::string, std::array<std::uint32_t, 2>> m_textureSizes;

    void apply3D()
    {
        glMatrixMode(GL_PROJECTION);
        if (m_camera.useMatrices)
        {
            glLoadMatrixf(m_camera.projection.data());
            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(m_camera.view.data());
        }
        else
        {
            const float aspect = static_cast<float>(std::max(GetScreenWidth(), 1)) /
                                 static_cast<float>(std::max(GetScreenHeight(), 1));
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
} // namespace

std::unique_ptr<Renderer> createPlatformRenderer()
{
    return std::make_unique<VitaGlRenderer>();
}
} // namespace p3fes::native_renderer
