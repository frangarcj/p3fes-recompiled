#include "renderer.h"
#include "lighting.h"
#include "skinning.h"
#include "uv_transform.h"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace p3fes::native_renderer
{
namespace
{
constexpr std::size_t kMaterialMapCount = static_cast<std::size_t>(MATERIAL_MAP_BRDF) + 1u;

constexpr const char *kStateVertexShader = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec4 vertexColor;
out vec2 fragTexCoord;
out vec4 fragColor;
flat out vec4 fragFlatColor;
out float fragFogCoord;
uniform mat4 mvp;
uniform int uFogCoordMode;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragFlatColor = vertexColor;
    gl_Position = mvp*vec4(vertexPosition, 1.0);
    // librw-vita uses clip W for ordinary 3-D/Im3D and the incoming Z for
    // Im2D. Both are positive RenderWare camera-space distances here.
    fragFogCoord = (uFogCoordMode != 0) ? vertexPosition.z : gl_Position.w;
}
)";

constexpr const char *kStateFragmentShader = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
flat in vec4 fragFlatColor;
in float fragFogCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform int uAlphaEnabled;
uniform int uAlphaFunc;
uniform int uAlphaRef;
uniform int uFogEnabled;
uniform float uFogStart;
uniform float uFogEnd;
uniform vec3 uFogColor;
uniform int uGouraudShading;
bool alphaPass(int a)
{
    if (uAlphaFunc == 0) return false;
    if (uAlphaFunc == 1) return true;
    if (uAlphaFunc == 2) return a < uAlphaRef;
    if (uAlphaFunc == 3) return a <= uAlphaRef;
    if (uAlphaFunc == 4) return a == uAlphaRef;
    if (uAlphaFunc == 5) return a >= uAlphaRef;
    if (uAlphaFunc == 6) return a > uAlphaRef;
    return a != uAlphaRef;
}
void main()
{
    vec4 vertexColor = (uGouraudShading != 0) ? fragColor : fragFlatColor;
    finalColor = texture(texture0, fragTexCoord)*colDiffuse*vertexColor;
    if (uAlphaEnabled != 0)
    {
        int alpha = int(clamp(finalColor.a, 0.0, 1.0)*255.0 + 0.5);
        if (!alphaPass(alpha)) discard;
    }
    if (uFogEnabled != 0)
    {
        float fog = clamp((fragFogCoord - uFogEnd)/(uFogStart - uFogEnd), 0.0, 1.0);
        finalColor.rgb = mix(uFogColor, finalColor.rgb, fog);
    }
}
)";

std::string textureKey(std::string_view name)
{
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return out;
}

int rwWrapMode(MeshDrawState::SamplerState::Address address)
{
    switch (address)
    {
    case MeshDrawState::SamplerState::Address::Mirror: return RL_TEXTURE_WRAP_MIRROR_REPEAT;
    case MeshDrawState::SamplerState::Address::Clamp: return RL_TEXTURE_WRAP_CLAMP;
    case MeshDrawState::SamplerState::Address::Wrap:
    default: return RL_TEXTURE_WRAP_REPEAT;
    }
}

void applyMeshSampler(unsigned int textureId, const MeshDrawState &state,
                      const MeshDrawState::SamplerState *sampler = nullptr)
{
    if (!textureId)
        return;
    const bool materialSampler = sampler && sampler->valid;
    const int wrapS = materialSampler ? rwWrapMode(sampler->u)
        : (state.clampU ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
    const int wrapT = materialSampler ? rwWrapMode(sampler->v)
        : (state.clampV ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
    const bool linear = materialSampler ? sampler->linearFilter : state.linearFilter;
    rlTextureParameters(textureId, RL_TEXTURE_WRAP_S, wrapS);
    rlTextureParameters(textureId, RL_TEXTURE_WRAP_T, wrapT);
    const int filter = linear ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
    rlTextureParameters(textureId, RL_TEXTURE_MIN_FILTER, filter);
    rlTextureParameters(textureId, RL_TEXTURE_MAG_FILTER, filter);
}

int rwBlendFactor(std::uint8_t factor)
{
    switch (factor)
    {
    case 1u: return RL_ZERO;
    case 2u: return RL_ONE;
    case 3u: return RL_SRC_COLOR;
    case 4u: return RL_ONE_MINUS_SRC_COLOR;
    case 5u: return RL_SRC_ALPHA;
    case 6u: return RL_ONE_MINUS_SRC_ALPHA;
    case 7u: return RL_DST_ALPHA;
    case 8u: return RL_ONE_MINUS_DST_ALPHA;
    case 9u: return RL_DST_COLOR;
    case 10u: return RL_ONE_MINUS_DST_COLOR;
    case 11u: return RL_SRC_ALPHA_SATURATE;
    default: return RL_ONE;
    }
}

Matrix rayMatrix(const Matrix4 &m)
{
    return Matrix{
        m[0], m[4], m[8], m[12],
        m[1], m[5], m[9], m[13],
        m[2], m[6], m[10], m[14],
        m[3], m[7], m[11], m[15]};
}

std::vector<std::uint16_t> triangleList(const MeshData &mesh,
                                        std::size_t begin,
                                        std::size_t count,
                                        std::string &error)
{
    std::vector<std::uint16_t> out;
    auto append = [&](std::uint32_t index) -> bool
    {
        if (index > std::numeric_limits<std::uint16_t>::max())
        {
            error = "desktop proof backend only supports 16-bit mesh indices";
            return false;
        }
        out.push_back(static_cast<std::uint16_t>(index));
        return true;
    };

    if (begin > mesh.indices.size() || count > mesh.indices.size() - begin)
    {
        error = "submesh index range is outside the mesh index buffer";
        return {};
    }

    const std::size_t end = begin + count;
    if (!mesh.triangleStrip)
    {
        out.reserve(count);
        for (std::size_t i = begin; i < end; ++i)
            if (!append(mesh.indices[i]))
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
        if (!append(a) || !append(b) || !append(c))
            return {};
    }
    return out;
}

struct DrawPart
{
    Mesh mesh{};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    std::string textureName;
    std::uint32_t materialIndex = 0;
    Material material{};
    std::vector<Vertex> sourceVertices;
    std::vector<Vertex> posedVertices;
    std::vector<unsigned char> shadedColors;
    bool lighting = false;
    bool hasPrelight = false;
    bool hasNormals = false;
    bool skinned = false;
};

class RaylibRenderer final : public Renderer
{
public:
    ~RaylibRenderer() override { close(); }

    bool open(int width, int height, const char *title, std::string &) override
    {
        if (m_open)
            return true;
        // The standalone viewers own their window. The in-game HLE renderer
        // instead borrows the runtime's already-open raylib/GL context so it
        // can replace GS presentation without creating a second host window.
        if (!IsWindowReady())
        {
            SetTraceLogLevel(LOG_WARNING);
            SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
            InitWindow(width, height, title);
            m_ownsWindow = true;
        }
        if (!IsWindowReady())
            return false;
        m_material = LoadMaterialDefault();
        m_stateShader = LoadShaderFromMemory(kStateVertexShader, kStateFragmentShader);
        if (m_stateShader.id != rlGetShaderIdDefault())
        {
            m_alphaEnabledLoc = GetShaderLocation(m_stateShader, "uAlphaEnabled");
            m_alphaFuncLoc = GetShaderLocation(m_stateShader, "uAlphaFunc");
            m_alphaRefLoc = GetShaderLocation(m_stateShader, "uAlphaRef");
            m_fogEnabledLoc = GetShaderLocation(m_stateShader, "uFogEnabled");
            m_fogStartLoc = GetShaderLocation(m_stateShader, "uFogStart");
            m_fogEndLoc = GetShaderLocation(m_stateShader, "uFogEnd");
            m_fogColorLoc = GetShaderLocation(m_stateShader, "uFogColor");
            m_fogCoordModeLoc = GetShaderLocation(m_stateShader, "uFogCoordMode");
            m_gouraudLoc = GetShaderLocation(m_stateShader, "uGouraudShading");
            m_stateShaderReady = m_alphaEnabledLoc >= 0 && m_alphaFuncLoc >= 0 && m_alphaRefLoc >= 0 &&
                                 m_fogEnabledLoc >= 0 && m_fogStartLoc >= 0 && m_fogEndLoc >= 0 &&
                                 m_fogColorLoc >= 0 && m_fogCoordModeLoc >= 0 && m_gouraudLoc >= 0;
        }
        m_open = true;
        return true;
    }

    void close() override
    {
        if (!m_open)
            return;
        for (auto &[id, parts] : m_meshes)
            for (DrawPart &part : parts)
                UnloadMesh(part.mesh);
        m_meshes.clear();
        for (auto &[name, texture] : m_textures)
            UnloadTexture(texture);
        m_textures.clear();
        if (m_stateShader.id != 0u)
            UnloadShader(m_stateShader);
        m_stateShader = {};
        m_stateShaderReady = false;
        UnloadMaterial(m_material);
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

        const std::string key = textureKey(source.name);
        auto existing = m_textures.find(key);
        if (existing != m_textures.end() && existing->second.width == static_cast<int>(source.width) &&
            existing->second.height == static_cast<int>(source.height))
        {
            // Dynamic RenderWare rasters (notably Sofdec/movie frames) retain
            // the same RwRaster identity while their pixels change. Updating
            // the existing GL object avoids a texture delete/create every
            // frame and keeps sampler state/cache references stable.
            UpdateTexture(existing->second, source.rgba.data());
            return true;
        }
        removeTexture(source.name);
        Image image{};
        image.data = const_cast<std::uint8_t *>(source.rgba.data());
        image.width = static_cast<int>(source.width);
        image.height = static_cast<int>(source.height);
        image.mipmaps = 1;
        image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        Texture2D texture = LoadTextureFromImage(image);
        if (texture.id == 0u)
        {
            error = "raylib failed to upload RGBA texture";
            return false;
        }
        SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
        m_textures.emplace(key, texture);
        return true;
    }

    void removeTexture(std::string_view name) override
    {
        auto found = m_textures.find(textureKey(name));
        if (found == m_textures.end())
            return;
        UnloadTexture(found->second);
        m_textures.erase(found);
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

        removeMesh(id);
        std::vector<Submesh> submeshes = source.submeshes;
        if (submeshes.empty())
            submeshes.push_back({0u, static_cast<std::uint32_t>(source.indices.size()), 0u});

        std::vector<DrawPart> parts;
        parts.reserve(submeshes.size());
        const auto discardParts = [&]()
        {
            for (DrawPart &part : parts)
                UnloadMesh(part.mesh);
            parts.clear();
        };
        for (const Submesh &submesh : submeshes)
        {
            if (!source.materials.empty() && submesh.materialIndex >= source.materials.size())
            {
                error = "submesh material index is outside the material list";
                discardParts();
                return false;
            }
            std::vector<std::uint16_t> indices = triangleList(source, submesh.firstIndex, submesh.indexCount, error);
            if (indices.empty())
            {
                if (!error.empty())
                {
                    discardParts();
                    return false;
                }
                continue;
            }

            DrawPart part;
            part.materialIndex = submesh.materialIndex;
            part.sourceVertices = source.vertices;
            part.posedVertices = source.vertices;
            part.shadedColors.resize(source.vertices.size() * 4u);
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
            Mesh &mesh = part.mesh;
            mesh.vertexCount = static_cast<int>(source.vertices.size());
            mesh.triangleCount = static_cast<int>(indices.size() / 3);
            mesh.vertices = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 3));
            mesh.normals = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 3));
            mesh.texcoords = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 2));
            mesh.colors = static_cast<unsigned char *>(MemAlloc(sizeof(unsigned char) * source.vertices.size() * 4));
            mesh.indices = static_cast<unsigned short *>(MemAlloc(sizeof(unsigned short) * indices.size()));

            for (std::size_t i = 0; i < source.vertices.size(); ++i)
            {
                const Vertex &vertex = source.vertices[i];
                std::memcpy(mesh.vertices + i * 3, vertex.position.data(), sizeof(float) * 3);
                std::memcpy(mesh.normals + i * 3, vertex.normal.data(), sizeof(float) * 3);
                std::memcpy(mesh.texcoords + i * 2, vertex.texCoord.data(), sizeof(float) * 2);
                std::memcpy(mesh.colors + i * 4, vertex.color.data(), 4);
            }
            std::memcpy(mesh.indices, indices.data(), sizeof(unsigned short) * indices.size());
            UploadMesh(&mesh, false);
            parts.push_back(std::move(part));
        }
        if (parts.empty())
        {
            if (error.empty())
                error = "mesh has no drawable triangles";
            return false;
        }
        m_meshes.emplace(id, std::move(parts));
        return true;
    }

    void removeMesh(MeshId id) override
    {
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;
        for (DrawPart &part : found->second)
            UnloadMesh(part.mesh);
        m_meshes.erase(found);
    }

    void beginFrame(const Camera &camera, ClearColor clear) override
    {
        m_exactCamera = camera.useMatrices;
        Camera3D rayCamera{};
        rayCamera.position = {camera.position[0], camera.position[1], camera.position[2]};
        rayCamera.target = {camera.target[0], camera.target[1], camera.target[2]};
        rayCamera.up = {camera.up[0], camera.up[1], camera.up[2]};
        rayCamera.fovy = camera.fovy;
        rayCamera.projection = CAMERA_PERSPECTIVE;
        m_camera = rayCamera;

        BeginDrawing();
        ClearBackground({clear.r, clear.g, clear.b, clear.a});
        if (m_exactCamera)
        {
            m_hostProjection = rlGetMatrixProjection();
            m_hostModelview = rlGetMatrixModelview();
            m_exactProjection = rayMatrix(camera.projection);
            m_exactView = rayMatrix(camera.view);
            rlSetMatrixProjection(m_exactProjection);
            rlSetMatrixModelview(m_exactView);
        }
        else
            BeginMode3D(m_camera);
        m_in3D = true;
    }

    void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state) override
    {
        if (!m_in3D)
        {
            if (m_exactCamera)
            {
                rlSetMatrixProjection(m_exactProjection);
                rlSetMatrixModelview(m_exactView);
            }
            else
                BeginMode3D(m_camera);
            m_in3D = true;
        }
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;

        if (state.depthTest)
            rlEnableDepthTest();
        else
            rlDisableDepthTest();
        if (state.depthWrite)
            rlEnableDepthMask();
        else
            rlDisableDepthMask();
        if (state.cullMode == MeshDrawState::CullMode::None)
            rlDisableBackfaceCulling();
        else
        {
            rlEnableBackfaceCulling();
            rlSetCullFace(state.cullMode == MeshDrawState::CullMode::Back ? RL_CULL_FACE_BACK : RL_CULL_FACE_FRONT);
        }

        for (DrawPart &part : found->second)
        {
            ::Material drawMaterial = m_material;
            if ((state.alphaTest.enabled || state.fog.enabled || !state.gouraudShading) && m_stateShaderReady)
            {
                setStateShaderUniforms(state.alphaTest, state.fog, state.gouraudShading, 0);
                drawMaterial.shader = m_stateShader;
            }
            std::array<MaterialMap, kMaterialMapCount> maps{};
            std::copy_n(m_material.maps, kMaterialMapCount, maps.data());
            drawMaterial.maps = maps.data();
            MaterialMap &diffuse = drawMaterial.maps[MATERIAL_MAP_DIFFUSE];
            const auto &color = part.materialIndex < state.materialColors.size()
                ? state.materialColors[part.materialIndex]
                : part.color;
            const MeshDrawState::BlendState *blend = part.materialIndex < state.materialBlends.size()
                ? &state.materialBlends[part.materialIndex]
                : nullptr;
            if (!state.blendEnabled)
                rlDisableColorBlend();
            else if (blend && blend->enabled)
            {
                rlEnableColorBlend();
                rlSetBlendFactors(rwBlendFactor(blend->src), rwBlendFactor(blend->dst), RL_FUNC_ADD);
                rlSetBlendMode(RL_BLEND_CUSTOM);
            }
            else
            {
                rlEnableColorBlend();
                rlSetBlendMode(RL_BLEND_ALPHA);
            }
            if (part.mesh.colors && part.sourceVertices.size() == static_cast<std::size_t>(part.mesh.vertexCount))
            {
                const MeshDrawState::UvTransform *uvTransform =
                    part.materialIndex < state.materialUvTransforms.size()
                        ? &state.materialUvTransforms[part.materialIndex]
                        : nullptr;
                for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                {
                    Vertex &posed = part.posedVertices[i];
                    posed = part.skinned ? poseSkinVertex(part.sourceVertices[i], state.skinPalette) : part.sourceVertices[i];
                    if (uvTransform && uvTransform->enabled)
                        posed.texCoord = transformTexCoord(part.sourceVertices[i].texCoord, *uvTransform);
                    std::memcpy(part.mesh.texcoords + i * 2u, posed.texCoord.data(), sizeof(float) * 2u);
                    const auto shaded = shadeVertexColor(posed, part.material, color,
                                                         part.lighting, part.hasPrelight, state, model);
                    std::memcpy(part.shadedColors.data() + i * 4u, shaded.data(), 4u);
                    if (part.skinned)
                    {
                        std::memcpy(part.mesh.vertices + i * 3u, posed.position.data(), sizeof(float) * 3u);
                        std::memcpy(part.mesh.normals + i * 3u, posed.normal.data(), sizeof(float) * 3u);
                    }
                }
                if (part.skinned)
                {
                    UpdateMeshBuffer(part.mesh, 0, part.mesh.vertices,
                                     part.mesh.vertexCount * static_cast<int>(sizeof(float) * 3u), 0);
                    UpdateMeshBuffer(part.mesh, 2, part.mesh.normals,
                                     part.mesh.vertexCount * static_cast<int>(sizeof(float) * 3u), 0);
                }
                UpdateMeshBuffer(part.mesh, 1, part.mesh.texcoords,
                                 part.mesh.vertexCount * static_cast<int>(sizeof(float) * 2u), 0);
                UpdateMeshBuffer(part.mesh, 3, part.shadedColors.data(),
                                 static_cast<int>(part.shadedColors.size()), 0);
                diffuse.color = WHITE;
            }
            else
                diffuse.color = {color[0], color[1], color[2], color[3]};
            if (!part.textureName.empty())
            {
                auto texture = m_textures.find(textureKey(part.textureName));
                if (texture != m_textures.end())
                {
                    const MeshDrawState::SamplerState *sampler = part.materialIndex < state.materialSamplers.size()
                        ? &state.materialSamplers[part.materialIndex] : nullptr;
                    applyMeshSampler(texture->second.id, state, sampler);
                    diffuse.texture = texture->second;
                }
            }
            // Matrix4 uses the normal OpenGL column-major order. raylib's
            // Matrix struct names are laid out by rows, so spell the mapping
            // out rather than relying on the struct's memory layout.
            const Matrix transform = rayMatrix(model);
            DrawMesh(part.mesh, drawMaterial, transform);

            const MeshDrawState::DualPass *dual = part.materialIndex < state.materialDualPasses.size()
                ? &state.materialDualPasses[part.materialIndex]
                : nullptr;
            if (dual && dual->enabled && !dual->textureName.empty())
            {
                const auto dualTexture = m_textures.find(textureKey(dual->textureName));
                if (dualTexture != m_textures.end())
                {
                    applyMeshSampler(dualTexture->second.id, state, &dual->sampler);
                    for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                    {
                        const auto uv = transformTexCoord(part.sourceVertices[i].texCoord, dual->uvTransform);
                        std::memcpy(part.mesh.texcoords + i * 2u, uv.data(), sizeof(float) * 2u);
                    }
                    UpdateMeshBuffer(part.mesh, 1, part.mesh.texcoords,
                                     part.mesh.vertexCount * static_cast<int>(sizeof(float) * 2u), 0);
                    diffuse.texture = dualTexture->second;
                    rlEnableColorBlend();
                    rlSetBlendFactors(rwBlendFactor(dual->srcBlend), rwBlendFactor(dual->dstBlend), RL_FUNC_ADD);
                    rlSetBlendMode(RL_BLEND_CUSTOM);
                    DrawMesh(part.mesh, drawMaterial, transform);
                    rlSetBlendMode(RL_BLEND_ALPHA);
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
                    applyMeshSampler(envTexture->second.id, state, &env->sampler);
                    const float coefficient = std::clamp(env->coefficient, 0.0f, 1.0f);
                    for (std::size_t i = 0; i < part.sourceVertices.size(); ++i)
                    {
                        const Vertex &posed = part.posedVertices[i];
                        const auto uv = environmentTexCoord(posed, model, *env);
                        std::memcpy(part.mesh.texcoords + i * 2u, uv.data(), sizeof(float) * 2u);
                        const float alpha = env->frameBufferAlpha
                            ? static_cast<float>(part.shadedColors[i * 4u + 3u]) / 255.0f
                            : 1.0f;
                        const auto level = static_cast<unsigned char>(std::lround(255.0f * coefficient * alpha));
                        part.shadedColors[i * 4u + 0u] = level;
                        part.shadedColors[i * 4u + 1u] = level;
                        part.shadedColors[i * 4u + 2u] = level;
                        part.shadedColors[i * 4u + 3u] = 255u;
                    }
                    UpdateMeshBuffer(part.mesh, 1, part.mesh.texcoords,
                                     part.mesh.vertexCount * static_cast<int>(sizeof(float) * 2u), 0);
                    UpdateMeshBuffer(part.mesh, 3, part.shadedColors.data(),
                                     static_cast<int>(part.shadedColors.size()), 0);
                    diffuse.texture = envTexture->second;
                    diffuse.color = WHITE;
                    rlEnableColorBlend();
                    rlSetBlendFactors(RL_ONE, RL_ONE, RL_FUNC_ADD);
                    rlSetBlendMode(RL_BLEND_CUSTOM);
                    DrawMesh(part.mesh, drawMaterial, transform);
                    rlSetBlendMode(RL_BLEND_ALPHA);
                }
            }
            rlSetBlendMode(RL_BLEND_ALPHA);
            rlEnableColorBlend();
        }

        rlEnableDepthTest();
        rlEnableDepthMask();
        rlEnableBackfaceCulling();
        rlSetCullFace(RL_CULL_FACE_BACK);
    }

    void drawWorldLines(const std::vector<Vertex> &vertices, const Matrix4 &model,
                        std::string_view textureName, const MeshDrawState &state) override
    {
        if (vertices.size() < 2u)
            return;
        if (!m_in3D)
        {
            if (m_exactCamera)
            {
                rlSetMatrixProjection(m_exactProjection);
                rlSetMatrixModelview(m_exactView);
            }
            else
                BeginMode3D(m_camera);
            m_in3D = true;
        }

        if (state.depthTest) rlEnableDepthTest(); else rlDisableDepthTest();
        if (state.depthWrite) rlEnableDepthMask(); else rlDisableDepthMask();
        if (!state.blendEnabled)
            rlDisableColorBlend();
        else if (!state.materialBlends.empty() && state.materialBlends[0].enabled)
        {
            rlEnableColorBlend();
            rlSetBlendFactors(rwBlendFactor(state.materialBlends[0].src),
                              rwBlendFactor(state.materialBlends[0].dst), RL_FUNC_ADD);
            rlSetBlendMode(RL_BLEND_CUSTOM);
        }
        else
        {
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ALPHA);
        }

        unsigned int textureId = 0u;
        if (!textureName.empty())
        {
            const auto found = m_textures.find(textureKey(textureName));
            if (found != m_textures.end())
            {
                textureId = found->second.id;
                applyMeshSampler(textureId, state);
            }
        }
        rlSetTexture(textureId);
        const bool stateShader = (state.alphaTest.enabled || state.fog.enabled || !state.gouraudShading) && m_stateShaderReady;
        if (stateShader)
        {
            setStateShaderUniforms(state.alphaTest, state.fog, state.gouraudShading, 0);
            const Vector4 white{1.0f, 1.0f, 1.0f, 1.0f};
            SetShaderValue(m_stateShader, m_stateShader.locs[SHADER_LOC_COLOR_DIFFUSE], &white, SHADER_UNIFORM_VEC4);
            BeginShaderMode(m_stateShader);
        }
        rlPushMatrix();
        rlMultMatrixf(model.data());
        rlBegin(RL_LINES);
        for (const Vertex &v : vertices)
        {
            rlColor4ub(v.color[0], v.color[1], v.color[2], v.color[3]);
            rlTexCoord2f(v.texCoord[0], v.texCoord[1]);
            rlVertex3f(v.position[0], v.position[1], v.position[2]);
        }
        rlEnd();
        rlPopMatrix();
        if (stateShader)
            EndShaderMode();
        rlSetTexture(0);

        rlEnableDepthTest();
        rlEnableDepthMask();
        rlEnableColorBlend();
        rlSetBlendMode(RL_BLEND_ALPHA);
        rlEnableBackfaceCulling();
        rlSetCullFace(RL_CULL_FACE_BACK);
    }

    void drawScreenTriangles(const std::vector<ScreenVertex> &vertices,
                             float sourceWidth, float sourceHeight,
                             std::string_view textureName,
                             const ScreenDrawState &state) override
    {
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, RL_TRIANGLES);
    }

    void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                         float sourceWidth, float sourceHeight,
                         std::string_view textureName,
                         const ScreenDrawState &state) override
    {
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, RL_LINES);
    }

    void endFrame() override
    {
        if (m_in3D && !m_exactCamera)
            EndMode3D();
        m_in3D = false;
        EndDrawing();
    }

private:
    void setStateShaderUniforms(const AlphaTestState &alpha, const FogState &fog,
                                bool gouraudShading, int fogCoordMode)
    {
        if (!m_stateShaderReady)
            return;
        const int alphaEnabled = alpha.enabled ? 1 : 0;
        const int function = alpha.function & 0x7u;
        const int reference = alpha.reference;
        const int fogEnabled = fog.enabled ? 1 : 0;
        const int gouraud = gouraudShading ? 1 : 0;
        const float fogColor[3]{fog.color[0] / 255.0f, fog.color[1] / 255.0f, fog.color[2] / 255.0f};
        SetShaderValue(m_stateShader, m_alphaEnabledLoc, &alphaEnabled, SHADER_UNIFORM_INT);
        SetShaderValue(m_stateShader, m_alphaFuncLoc, &function, SHADER_UNIFORM_INT);
        SetShaderValue(m_stateShader, m_alphaRefLoc, &reference, SHADER_UNIFORM_INT);
        SetShaderValue(m_stateShader, m_fogEnabledLoc, &fogEnabled, SHADER_UNIFORM_INT);
        SetShaderValue(m_stateShader, m_fogStartLoc, &fog.start, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_stateShader, m_fogEndLoc, &fog.end, SHADER_UNIFORM_FLOAT);
        SetShaderValue(m_stateShader, m_fogColorLoc, fogColor, SHADER_UNIFORM_VEC3);
        SetShaderValue(m_stateShader, m_fogCoordModeLoc, &fogCoordMode, SHADER_UNIFORM_INT);
        SetShaderValue(m_stateShader, m_gouraudLoc, &gouraud, SHADER_UNIFORM_INT);
    }

    void drawScreenPrimitive(const std::vector<ScreenVertex> &vertices,
                             float sourceWidth, float sourceHeight,
                             std::string_view textureName,
                             const ScreenDrawState &state, int primitive)
    {
        if (vertices.empty() || sourceWidth <= 0.0f || sourceHeight <= 0.0f)
            return;
        if (m_in3D)
        {
            if (m_exactCamera)
            {
                rlSetMatrixProjection(m_hostProjection);
                rlSetMatrixModelview(m_hostModelview);
            }
            else
                EndMode3D();
            m_in3D = false;
        }
        const float sx = static_cast<float>(std::max(GetScreenWidth(), 1)) / sourceWidth;
        const float sy = static_cast<float>(std::max(GetScreenHeight(), 1)) / sourceHeight;
        unsigned int textureId = 0u;
        if (!textureName.empty())
        {
            const auto texture = m_textures.find(textureKey(textureName));
            if (texture != m_textures.end())
            {
                textureId = texture->second.id;
                rlTextureParameters(textureId, RL_TEXTURE_WRAP_S,
                                    state.clampU ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                rlTextureParameters(textureId, RL_TEXTURE_WRAP_T,
                                    state.clampV ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                const int filter = state.linearFilter ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
                rlTextureParameters(textureId, RL_TEXTURE_MIN_FILTER, filter);
                rlTextureParameters(textureId, RL_TEXTURE_MAG_FILTER, filter);
            }
        }
        switch (state.blend)
        {
        case ScreenBlend::Opaque:
            rlDisableColorBlend();
            break;
        case ScreenBlend::Additive:
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ADDITIVE);
            break;
        case ScreenBlend::Subtractive:
            rlEnableColorBlend();
            rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE, RL_FUNC_REVERSE_SUBTRACT);
            rlSetBlendMode(RL_BLEND_CUSTOM);
            break;
        default:
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ALPHA);
            break;
        }
        rlSetTexture(textureId);
        const bool stateShader = (state.alphaTest.enabled || state.fog.enabled || !state.gouraudShading) && m_stateShaderReady;
        if (stateShader)
        {
            setStateShaderUniforms(state.alphaTest, state.fog, state.gouraudShading, 1);
            const Vector4 white{1.0f, 1.0f, 1.0f, 1.0f};
            SetShaderValue(m_stateShader, m_stateShader.locs[SHADER_LOC_COLOR_DIFFUSE], &white, SHADER_UNIFORM_VEC4);
            BeginShaderMode(m_stateShader);
        }
        rlDisableBackfaceCulling();
        rlBegin(primitive);
        for (const ScreenVertex &v : vertices)
        {
            rlColor4ub(v.color[0], v.color[1], v.color[2], v.color[3]);
            rlTexCoord2f(v.u, v.v);
            rlVertex3f(v.x * sx, v.y * sy, v.z);
        }
        rlEnd();
        if (stateShader)
            EndShaderMode();
        rlSetTexture(0);
        rlEnableColorBlend();
        rlSetBlendMode(RL_BLEND_ALPHA);
    }
    bool m_open = false;
    bool m_ownsWindow = false;
    bool m_in3D = false;
    bool m_exactCamera = false;
    ::Material m_material{};
    Shader m_stateShader{};
    int m_alphaEnabledLoc = -1;
    int m_alphaFuncLoc = -1;
    int m_alphaRefLoc = -1;
    int m_fogEnabledLoc = -1;
    int m_fogStartLoc = -1;
    int m_fogEndLoc = -1;
    int m_fogColorLoc = -1;
    int m_fogCoordModeLoc = -1;
    int m_gouraudLoc = -1;
    bool m_stateShaderReady = false;
    Camera3D m_camera{};
    Matrix m_exactProjection{};
    Matrix m_exactView{};
    Matrix m_hostProjection{};
    Matrix m_hostModelview{};
    std::unordered_map<MeshId, std::vector<DrawPart>> m_meshes;
    std::unordered_map<std::string, Texture2D> m_textures;
};
} // namespace

std::unique_ptr<Renderer> createPlatformRenderer()
{
    return std::make_unique<RaylibRenderer>();
}
} // namespace p3fes::native_renderer
