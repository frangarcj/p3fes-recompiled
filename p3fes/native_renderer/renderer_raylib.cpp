#include "renderer.h"
#include "lighting.h"
#include "skinning.h"
#include "static_vertex_cache.h"
#include "uv_transform.h"
#include "screen_batch.h"

#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>
#include <external/glad.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>
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

struct FitViewport
{
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
};

FitViewport fitViewport(float sourceWidth, float sourceHeight)
{
    const int hostWidth = std::max(GetRenderWidth(), 1);
    const int hostHeight = std::max(GetRenderHeight(), 1);
    if (!(sourceWidth > 0.0f) || !(sourceHeight > 0.0f))
        return {0, 0, hostWidth, hostHeight};

    const float scale = std::min(hostWidth / sourceWidth, hostHeight / sourceHeight);
    const int width = std::clamp(static_cast<int>(std::lround(sourceWidth * scale)), 1, hostWidth);
    const int height = std::clamp(static_cast<int>(std::lround(sourceHeight * scale)), 1, hostHeight);
    return {(hostWidth - width) / 2, (hostHeight - height) / 2, width, height};
}

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
    detail::StaticVertexCacheKey diagKey; // P3FES_CACHE_DIAG only
};

namespace
{
// Mac-side diagnostic of the vitaGL static-vertex cache: histogram of why an unskinned part would be regenerated.
struct CacheDiag
{
    std::array<std::uint64_t, 256> hist{};
    std::array<std::uint64_t, 256> verts{};
    std::uint64_t skinnedVerts = 0, dirSum = 0, localSum = 0, lightDraws = 0, lightFlagDraws = 0;
    std::uint64_t skinnedParts = 0, draws = 0;
    std::uint64_t lastReport = 0;
};
CacheDiag g_cacheDiag;
bool cacheDiagEnabled()
{
    static const bool on = [] { const char *v = std::getenv("P3FES_CACHE_DIAG"); return v && *v == '1'; }();
    return on;
}
} // namespace

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
        // Diagnostic: P3FES_HOST_SIZE=WxH sets the host framebuffer size in pixels (e.g. 960x544, the Vita's
        // screen) so the widescreen placement code can be exercised on the desktop, where the window is 4:3.
        if (const char *size = std::getenv("P3FES_HOST_SIZE"))
        {
            int w = 0, h = 0;
            if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
            {
                const Vector2 dpi = GetWindowScaleDPI();
                SetWindowSize(static_cast<int>(w / std::max(dpi.x, 1.0f)), static_cast<int>(h / std::max(dpi.y, 1.0f)));
            }
        }
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
        for (auto &[name, target] : m_targets)
        {
            m_textures.erase(name);
            UnloadRenderTexture(target);
        }
        m_targets.clear();
        m_target = nullptr;
        flushScreenBatch();
        for (Texture2D &page : m_atlasPages)
            UnloadTexture(page);
        m_atlasPages.clear();
        m_atlas.clear();
        m_smallTextures.clear();
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

        flushScreenBatch(); // pending quads sample the old pixels
        const std::string key = textureKey(source.name);
        if (m_targets.find(key) != m_targets.end())
            return true; // the name is a render target: its pixels come from beginTextureTarget passes
        auto existing = m_textures.find(key);
        if (existing != m_textures.end() && existing->second.width == static_cast<int>(source.width) &&
            existing->second.height == static_cast<int>(source.height))
        {
            // Dynamic RenderWare rasters (notably Sofdec/movie frames) retain
            // the same RwRaster identity while their pixels change. Updating
            // the existing GL object avoids a texture delete/create every
            // frame and keeps sampler state/cache references stable.
            UpdateTexture(existing->second, source.rgba.data());
            storeSmallTexture(key, source);
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
        storeSmallTexture(key, source);
        return true;
    }

    void removeTexture(std::string_view name) override
    {
        flushScreenBatch();
        dropSmallTexture(textureKey(name));
        auto found = m_textures.find(textureKey(name));
        if (found == m_textures.end())
            return;
        const auto target = m_targets.find(found->first);
        if (target != m_targets.end())
        {
            if (m_target == &target->second)
                endTextureTarget();
            UnloadRenderTexture(target->second); // owns the colour texture
            m_targets.erase(target);
        }
        else
            UnloadTexture(found->second);
        m_textures.erase(found);
    }

    void beginTextureTarget(std::string_view name, int width, int height) override
    {
        if (!m_open || width <= 0 || height <= 0)
            return;
        flushScreenBatch();
        if (m_target)
            endTextureTarget();
        leave3D();
        rlDrawRenderBatchActive();
        const std::string key = textureKey(name);
        auto target = m_targets.find(key);
        if (target == m_targets.end() || target->second.texture.width != width ||
            target->second.texture.height != height)
        {
            removeTexture(name); // a placeholder upload or a target of another size
            const RenderTexture2D created = LoadRenderTexture(width, height);
            if (created.id == 0u)
                return;
            SetTextureFilter(created.texture, TEXTURE_FILTER_BILINEAR);
            target = m_targets.emplace(key, created).first;
            m_textures[key] = created.texture;
        }
        m_target = &target->second;
        rlEnableFramebuffer(m_target->id);
        // The pass is rendered upside down (see targetProjection), which mirrors the winding.
        glFrontFace(GL_CW);
        rlViewport(0, 0, width, height);
    }

    void endTextureTarget() override
    {
        flushScreenBatch();
        if (!m_target)
            return;
        leave3D();
        rlDrawRenderBatchActive();
        rlDisableFramebuffer();
        glFrontFace(GL_CCW);
        m_target = nullptr;
        rlViewport(0, 0, std::max(GetRenderWidth(), 1), std::max(GetRenderHeight(), 1));
    }

    bool snapshotFramebuffer(std::string_view name, int texWidth, int texHeight, int srcWidth, int srcHeight) override
    {
        if (!m_open || m_target || texWidth <= 0 || texHeight <= 0 || srcWidth <= 0 || srcHeight <= 0 || srcWidth > texWidth ||
            srcHeight > texHeight)
            return false;
        flushScreenBatch();
        leave3D();
        rlDrawRenderBatchActive();
        const std::string key = textureKey(name);
        auto target = m_targets.find(key);
        if (target == m_targets.end() || target->second.texture.width != texWidth || target->second.texture.height != texHeight)
        {
            removeTexture(name);
            const RenderTexture2D created = LoadRenderTexture(texWidth, texHeight);
            if (created.id == 0u)
                return false;
            SetTextureFilter(created.texture, TEXTURE_FILTER_BILINEAR);
            target = m_targets.emplace(key, created).first;
            m_textures[key] = created.texture;
        }
        const int hostWidth = std::max(GetRenderWidth(), 1), hostHeight = std::max(GetRenderHeight(), 1);
        const FitViewport area = m_widescreen != 0 ? FitViewport{0, 0, hostWidth, hostHeight} : fitViewport(640.0f, 448.0f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target->second.id);
        glDisable(GL_SCISSOR_TEST);
        // top row of the picture -> row 0 of the texture (v = 0 is the top row of every sampled texture)
        glBlitFramebuffer(area.x, area.y + area.height, area.x + area.width, area.y, 0, 0, srcWidth, srcHeight,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return true;
    }

    void setWidescreen(int mode) override
    {
        flushScreenBatch();
        m_widescreen = mode;
    }
    void setScreenPlacement(ScreenPlacement placement) override { m_screenPlacement = placement; }
    bool setWorldPlacement(WorldPlacement placement) override
    {
        if (m_widescreen != 2 || m_target)
            placement = WorldPlacement::Full;
        flushScreenBatch();
        rlDrawRenderBatchActive();
        m_worldPlacement = placement;
        if (m_in3D)
            applyWorldViewport();
        else if (placement == WorldPlacement::Full)
            glDisable(GL_SCISSOR_TEST);
        return m_widescreen == 2 && !m_target;
    }

    bool captureNextFrame(const std::string &path) override
    {
        m_capturePath = path;
        return true;
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
        rlViewport(0, 0, std::max(GetRenderWidth(), 1), std::max(GetRenderHeight(), 1));
        ClearBackground(BLACK);
        const FitViewport viewport = worldViewport();
        glEnable(GL_SCISSOR_TEST);
        glScissor(viewport.x, viewport.y, viewport.width, viewport.height);
        glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        m_hostProjection = rlGetMatrixProjection();
        m_hostModelview = rlGetMatrixModelview();
        if (m_exactCamera)
        {
            m_exactProjection = targetProjection(rayMatrix(camera.projection));
            m_exactView = rayMatrix(camera.view);
            rlSetMatrixProjection(m_exactProjection);
            rlSetMatrixModelview(m_exactView);
        }
        else
        {
            BeginMode3D(m_camera);
            rlSetMatrixProjection(MatrixPerspective(m_camera.fovy * DEG2RAD, 640.0 / 448.0,
                                                   rlGetCullDistanceNear(), rlGetCullDistanceFar()));
        }
        rlViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        m_in3D = true;
    }

    void clear(ClearColor clear, bool colorBuffer, bool depthBuffer) override
    {
        if (!colorBuffer && !depthBuffer)
            return;
        flushScreenBatch();
        // Keep clears ordered with raylib's immediate batch and match
        // RwCameraClear's independent colour/Z flags.
        rlDrawRenderBatchActive();
        const FitViewport viewport = worldViewport();
        glEnable(GL_SCISSOR_TEST);
        glScissor(viewport.x, viewport.y, viewport.width, viewport.height);
        GLboolean oldDepthMask = GL_TRUE;
        glGetBooleanv(GL_DEPTH_WRITEMASK, &oldDepthMask);
        GLbitfield mask = 0u;
        if (colorBuffer)
        {
            glClearColor(clear.r / 255.0f, clear.g / 255.0f, clear.b / 255.0f, clear.a / 255.0f);
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (depthBuffer)
        {
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        glClear(mask);
        glDisable(GL_SCISSOR_TEST);
        rlViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        if (depthBuffer && oldDepthMask == GL_FALSE)
            glDepthMask(GL_FALSE);
    }

    void setCamera(const Camera &camera) override
    {
        const bool previousExactCamera = m_exactCamera;
        if (m_in3D && !previousExactCamera)
            EndMode3D();
        else if (m_in3D && previousExactCamera)
        {
            rlSetMatrixProjection(m_hostProjection);
            rlSetMatrixModelview(m_hostModelview);
        }

        m_exactCamera = camera.useMatrices;
        Camera3D rayCamera{};
        rayCamera.position = {camera.position[0], camera.position[1], camera.position[2]};
        rayCamera.target = {camera.target[0], camera.target[1], camera.target[2]};
        rayCamera.up = {camera.up[0], camera.up[1], camera.up[2]};
        rayCamera.fovy = camera.fovy;
        rayCamera.projection = CAMERA_PERSPECTIVE;
        m_camera = rayCamera;
        if (m_exactCamera)
        {
            m_exactProjection = targetProjection(rayMatrix(camera.projection));
            m_exactView = rayMatrix(camera.view);
        }

        // The next world draw starts a new mode with the updated camera or
        // reloads its exact matrices, without clearing/presenting the frame.
        m_in3D = false;
    }

    void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state) override
    {
        flushScreenBatch();
        if (!m_in3D)
        {
            if (m_exactCamera)
            {
                rlSetMatrixProjection(m_exactProjection);
                rlSetMatrixModelview(m_exactView);
            }
            else
            {
                BeginMode3D(m_camera);
                rlSetMatrixProjection(MatrixPerspective(m_camera.fovy * DEG2RAD, 640.0 / 448.0,
                                                       rlGetCullDistanceNear(), rlGetCullDistanceFar()));
            }
            applyWorldViewport();
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
            // FB_ONLY alpha test (see lighting.h): unskinned parts are drawn twice, skinned ones once.
            const bool alphaTwoPass = alphaTestFbOnly(state.alphaTest) && !part.skinned &&
                                      alphaTestFailPass(state.alphaTest).function != 0u;
            const AlphaTestState partAlpha = alphaTwoPass ? alphaTestPassOne(state.alphaTest)
                                                          : alphaTestSinglePass(state.alphaTest);
            if ((state.alphaTest.enabled || state.fog.enabled || !state.gouraudShading) && m_stateShaderReady)
            {
                setStateShaderUniforms(partAlpha, state.fog, state.gouraudShading, 0);
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
                if (blend->subtract)
                {
                    // Cd - Cs * As; the alpha channel keeps As (GS writes the source alpha)
                    rlSetBlendFactorsSeparate(rwBlendFactor(blend->src), rwBlendFactor(blend->dst), RL_ONE, RL_ZERO,
                                              RL_FUNC_REVERSE_SUBTRACT, RL_FUNC_ADD);
                    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
                }
                else
                {
                    rlSetBlendFactors(rwBlendFactor(blend->src), rwBlendFactor(blend->dst), RL_FUNC_ADD);
                    rlSetBlendMode(RL_BLEND_CUSTOM);
                }
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
                if (cacheDiagEnabled())
                {
                    const MeshDrawState::UvTransform defaultUv{};
                    const auto &uvCurrent = uvTransform ? *uvTransform : defaultUv;
                    ++g_cacheDiag.lightDraws;
                    g_cacheDiag.dirSum += state.directionalLights.size();
                    g_cacheDiag.localSum += state.localLights.size();
                    g_cacheDiag.lightFlagDraws += (part.lighting && state.lightingEnabled) ? 1u : 0u;
                    if (part.skinned)
                    {
                        ++g_cacheDiag.skinnedParts;
                        g_cacheDiag.skinnedVerts += part.sourceVertices.size();
                    }
                    else
                    {
                        const unsigned why = part.diagKey.mismatch(model, part.material, color, part.lighting,
                                                                    part.hasPrelight, state, uvCurrent);
                        ++g_cacheDiag.hist[why];
                        g_cacheDiag.verts[why] += part.sourceVertices.size();
                        part.diagKey.capture(model, part.material, color, part.lighting, part.hasPrelight, state, uvCurrent);
                    }
                    if (++g_cacheDiag.draws - g_cacheDiag.lastReport >= 3000)
                    {
                        g_cacheDiag.lastReport = g_cacheDiag.draws;
                        std::fprintf(stderr, "[cache-diag] part-draws=%llu skinned=%llu static:",
                                     (unsigned long long)g_cacheDiag.draws, (unsigned long long)g_cacheDiag.skinnedParts);
                        for (unsigned m = 0; m < 256; ++m)
                            if (g_cacheDiag.hist[m])
                                std::fprintf(stderr, " [0x%02x]=%llu(%lluv)", m, (unsigned long long)g_cacheDiag.hist[m],
                                             (unsigned long long)g_cacheDiag.verts[m]);
                        std::fprintf(stderr, " skinned-verts=%llu avg-dir=%.2f avg-local=%.2f lit-draws=%llu/%llu\n",
                                     (unsigned long long)g_cacheDiag.skinnedVerts,
                                     double(g_cacheDiag.dirSum) / double(g_cacheDiag.lightDraws),
                                     double(g_cacheDiag.localSum) / double(g_cacheDiag.lightDraws),
                                     (unsigned long long)g_cacheDiag.lightFlagDraws, (unsigned long long)g_cacheDiag.lightDraws);
                        g_cacheDiag.hist.fill(0);
                        g_cacheDiag.verts.fill(0);
                        g_cacheDiag.skinnedVerts = g_cacheDiag.dirSum = g_cacheDiag.localSum = g_cacheDiag.lightDraws = g_cacheDiag.lightFlagDraws = 0;
                        g_cacheDiag.skinnedParts = 0;
                    }
                }
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
            const auto &textureName = part.materialIndex < state.materialTextures.size()
                ? state.materialTextures[part.materialIndex] : part.textureName;
            if (!textureName.empty())
            {
                auto texture = m_textures.find(textureKey(textureName));
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
            if (alphaTwoPass && m_stateShaderReady)
            {
                // The pixels that failed the test: still drawn, Z not written.
                setStateShaderUniforms(alphaTestFailPass(state.alphaTest), state.fog, state.gouraudShading, 0);
                rlDisableDepthMask();
                DrawMesh(part.mesh, drawMaterial, transform);
                if (state.depthWrite)
                    rlEnableDepthMask();
                setStateShaderUniforms(partAlpha, state.fog, state.gouraudShading, 0);
            }

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
                        // Texture coordinate set 1 (carried in weights, see decodeNativeMesh) when the geometry has one.
                        const Vertex &dualSource = part.sourceVertices[i];
                        const bool hasUv1 = !part.skinned && dualSource.weights[2] == 1.0f;
                        const std::array<float, 2> dualBase = hasUv1
                            ? std::array<float, 2>{dualSource.weights[0], dualSource.weights[1]} : dualSource.texCoord;
                        const auto uv = transformTexCoord(dualBase, dual->uvTransform);
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
        flushScreenBatch();
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
            if (state.materialBlends[0].subtract)
            {
                rlSetBlendFactorsSeparate(rwBlendFactor(state.materialBlends[0].src), rwBlendFactor(state.materialBlends[0].dst),
                                          RL_ONE, RL_ZERO, RL_FUNC_REVERSE_SUBTRACT, RL_FUNC_ADD);
                rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
            }
            else
            {
                rlSetBlendFactors(rwBlendFactor(state.materialBlends[0].src),
                                  rwBlendFactor(state.materialBlends[0].dst), RL_FUNC_ADD);
                rlSetBlendMode(RL_BLEND_CUSTOM);
            }
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
        if (!detail::screenBatchEnabled() || m_target || vertices.empty() || sourceWidth <= 0.0f || sourceHeight <= 0.0f)
        {
            flushScreenBatch();
            drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, RL_TRIANGLES);
            return;
        }
        submitScreenTriangles(vertices, sourceWidth, sourceHeight, textureName, state);
    }

    void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                         float sourceWidth, float sourceHeight,
                         std::string_view textureName,
                         const ScreenDrawState &state) override
    {
        flushScreenBatch();
        drawScreenPrimitive(vertices, sourceWidth, sourceHeight, textureName, state, RL_LINES);
    }

    void endFrame() override
    {
        flushScreenBatch();
        if (m_in3D && !m_exactCamera)
            EndMode3D();
        m_in3D = false;
        if (!m_capturePath.empty())
        {
            rlDrawRenderBatchActive();
            const int width = std::max(GetRenderWidth(), 1), height = std::max(GetRenderHeight(), 1);
            if (unsigned char *pixels = rlReadScreenPixels(width, height))
            {
                if (FILE *file = std::fopen(m_capturePath.c_str(), "wb"))
                {
                    const std::uint32_t header[2] = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
                    std::fwrite("P3FB", 1, 4, file);
                    std::fwrite(header, sizeof(std::uint32_t), 2, file);
                    std::fwrite(pixels, 4u, static_cast<std::size_t>(width) * height, file);
                    std::fclose(file);
                }
                RL_FREE(pixels);
            }
            m_capturePath.clear();
        }
        EndDrawing();
    }

private:
    FitViewport worldViewport() const
    {
        if (m_target)
            return {0, 0, m_target->texture.width, m_target->texture.height};
        if (m_widescreen == 2 && m_worldPlacement != WorldPlacement::Full)
        {
            const FitViewport centre = fitViewport(640.0f, 448.0f);
            if (m_worldPlacement == WorldPlacement::Centre)
                return centre;
            // Edge passes: magnify the picture so that its outermost kEdgeColumns pixel columns span the margin.
            constexpr float kEdgeColumns = 8.0f;
            const int hostWidth = std::max(GetRenderWidth(), 1);
            const int margin = std::max(m_worldPlacement == WorldPlacement::EdgeLeft ? centre.x : hostWidth - centre.x - centre.width, 1);
            const int width = std::clamp(static_cast<int>(static_cast<float>(margin) * static_cast<float>(centre.width) / kEdgeColumns), centre.width, 16000);
            return {m_worldPlacement == WorldPlacement::EdgeLeft ? 0 : hostWidth - width, centre.y, width, centre.height};
        }
        if (m_widescreen != 0)
            return {0, 0, std::max(GetRenderWidth(), 1), std::max(GetRenderHeight(), 1)};
        return fitViewport(640.0f, 448.0f);
    }

    // Viewport (and, for the edge placements, the scissor to the margin) of the current world placement.
    void applyWorldViewport()
    {
        const FitViewport viewport = worldViewport();
        rlViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        if (m_widescreen == 2 && !m_target && (m_worldPlacement == WorldPlacement::EdgeLeft || m_worldPlacement == WorldPlacement::EdgeRight))
        {
            const FitViewport centre = fitViewport(640.0f, 448.0f);
            const int hostWidth = std::max(GetRenderWidth(), 1);
            glEnable(GL_SCISSOR_TEST);
            if (m_worldPlacement == WorldPlacement::EdgeLeft)
                glScissor(0, 0, centre.x, std::max(GetRenderHeight(), 1));
            else
                glScissor(centre.x + centre.width, 0, hostWidth - centre.x - centre.width, std::max(GetRenderHeight(), 1));
        }
        else
            glDisable(GL_SCISSOR_TEST);
    }


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

    // A GL colour attachment stores its bottom row first, while the game samples its camera
    // textures like any other raster (v = 0 is the top row): render target passes upside down.
    Matrix targetProjection(Matrix projection) const
    {
        if (m_target)
        {
            projection.m1 = -projection.m1;
            projection.m5 = -projection.m5;
            projection.m9 = -projection.m9;
            projection.m13 = -projection.m13;
        }
        return projection;
    }

    void leave3D()
    {
        if (!m_in3D)
            return;
        if (m_exactCamera)
        {
            rlSetMatrixProjection(m_hostProjection);
            rlSetMatrixModelview(m_hostModelview);
        }
        else
            EndMode3D();
        m_in3D = false;
    }

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
                             const ScreenDrawState &state, int primitive, long forcedTexture = -1)
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
        rlDrawRenderBatchActive();
        // Inside a render-target pass the source space is the target itself, drawn upside down like
        // the world draws of the pass (see targetProjection).
        const FitViewport viewport = m_target ? worldViewport()
            : screenViewport(sourceWidth, sourceHeight,
                             FitViewport{0, 0, std::max(GetRenderWidth(), 1), std::max(GetRenderHeight(), 1)}, fitViewport);
        rlViewport(viewport.x, viewport.y, viewport.width, viewport.height);
        // Wide placement: the source area keeps its centred 4:3 position and scale, the rest of the framebuffer
        // shows the source plane beyond x = 0 / x = sourceWidth.
        double orthoLeft = 0.0, orthoRight = sourceWidth;
        if (!m_target && m_widescreen == 2 && m_screenPlacement == ScreenPlacement::Wide)
        {
            const double span = static_cast<double>(viewport.width) * sourceWidth /
                                std::max(fitViewport(sourceWidth, sourceHeight).width, 1);
            orthoLeft = (sourceWidth - span) * 0.5;
            orthoRight = orthoLeft + span;
        }
        rlSetMatrixProjection(m_target ? MatrixOrtho(0.0, sourceWidth, 0.0, sourceHeight, -1.0, 0.0)
                                       : MatrixOrtho(orthoLeft, orthoRight, sourceHeight, 0.0, -1.0, 0.0));
        rlSetMatrixModelview(MatrixIdentity());
        // FBMSK = 0x00FFFFFF: alpha-only write (see ScreenDrawState::alphaOnlyWrite)
        if (state.alphaOnlyWrite)
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        else
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (state.depthTest)
            rlEnableDepthTest();
        else
            rlDisableDepthTest();
        if (state.depthWrite)
            rlEnableDepthMask();
        else
            rlDisableDepthMask();
        switch (state.depthFunction)
        {
        case ScreenDepthFunction::Never: glDepthFunc(GL_NEVER); break;
        case ScreenDepthFunction::GreaterEqual: glDepthFunc(GL_LEQUAL); break;
        case ScreenDepthFunction::Greater: glDepthFunc(GL_LESS); break;
        case ScreenDepthFunction::Always:
        default: glDepthFunc(GL_ALWAYS); break;
        }
        unsigned int textureId = 0u;
        if (forcedTexture >= 0 || !textureName.empty())
        {
            const auto texture = forcedTexture >= 0 ? m_textures.end() : m_textures.find(textureKey(textureName));
            if (forcedTexture > 0 || (forcedTexture < 0 && texture != m_textures.end()))
            {
                textureId = forcedTexture > 0 ? static_cast<unsigned int>(forcedTexture) : texture->second.id;
                rlTextureParameters(textureId, RL_TEXTURE_WRAP_S,
                                    state.clampU ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                rlTextureParameters(textureId, RL_TEXTURE_WRAP_T,
                                    state.clampV ? RL_TEXTURE_WRAP_CLAMP : RL_TEXTURE_WRAP_REPEAT);
                const int filter = state.linearFilter ? RL_TEXTURE_FILTER_LINEAR : RL_TEXTURE_FILTER_NEAREST;
                rlTextureParameters(textureId, RL_TEXTURE_MIN_FILTER, filter);
                rlTextureParameters(textureId, RL_TEXTURE_MAG_FILTER, filter);
            }
        }
        GLenum blendSrcRgb = GL_SRC_ALPHA;
        GLenum blendDstRgb = GL_ONE_MINUS_SRC_ALPHA;
        GLenum blendEquationRgb = GL_FUNC_ADD;
        switch (state.blend)
        {
        case ScreenBlend::Opaque:
            rlDisableColorBlend();
            break;
        case ScreenBlend::Additive:
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ADDITIVE);
            blendDstRgb = GL_ONE;
            break;
        case ScreenBlend::Subtractive:
            rlEnableColorBlend();
            rlSetBlendFactors(RL_SRC_ALPHA, RL_ONE, RL_FUNC_REVERSE_SUBTRACT);
            rlSetBlendMode(RL_BLEND_CUSTOM);
            blendDstRgb = GL_ONE;
            blendEquationRgb = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case ScreenBlend::DestinationOnly:
            rlEnableColorBlend();
            rlSetBlendFactors(RL_ZERO, RL_ONE, RL_FUNC_ADD);
            rlSetBlendMode(RL_BLEND_CUSTOM);
            blendSrcRgb = GL_ZERO;
            blendDstRgb = GL_ONE;
            break;
        case ScreenBlend::DestinationAlphaAdditive:
            rlEnableColorBlend();
            rlSetBlendFactors(RL_DST_ALPHA, RL_ONE, RL_FUNC_ADD);
            rlSetBlendMode(RL_BLEND_CUSTOM);
            blendSrcRgb = GL_DST_ALPHA;
            blendDstRgb = GL_ONE;
            break;
        case ScreenBlend::DestinationAlpha:
            rlEnableColorBlend();
            rlSetBlendFactors(RL_DST_ALPHA, RL_ONE_MINUS_DST_ALPHA, RL_FUNC_ADD);
            rlSetBlendMode(RL_BLEND_CUSTOM);
            blendSrcRgb = GL_DST_ALPHA;
            blendDstRgb = GL_ONE_MINUS_DST_ALPHA;
            break;
        default:
            rlEnableColorBlend();
            rlSetBlendMode(RL_BLEND_ALPHA);
            break;
        }
        if (state.blend != ScreenBlend::Opaque)
        {
            glBlendEquationSeparate(blendEquationRgb, GL_FUNC_ADD);
            glBlendFuncSeparate(blendSrcRgb, blendDstRgb, state.destinationAlphaTest ? GL_DST_ALPHA : GL_ONE, GL_ZERO);
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
        const auto drawVertices = [&]
        {
            rlBegin(primitive);
            // rlBegin resets the pending draw's texture to the 1x1 white default whenever the draw
            // mode changes (the batch starts as RL_QUADS after every flush), so the texture must be
            // set after it, not before: otherwise every screen draw samples opaque white.
            rlSetTexture(textureId);
            for (const ScreenVertex &v : vertices)
            {
                rlColor4ub(v.color[0], v.color[1], v.color[2], v.color[3]);
                rlTexCoord2f(v.u, v.v);
                rlVertex3f(v.x, v.y, v.z);
            }
            rlEnd();
            rlDrawRenderBatchActive();
        };
        const bool alphaFailPass = state.alphaTest.enabled &&
                                   state.alphaFailAction != ScreenAlphaFailAction::Keep;
        if (state.alphaFailAction == ScreenAlphaFailAction::FramebufferOnly &&
            (!state.depthWrite || !state.depthTest))
        {
            // With no observable depth side effect, both alpha outcomes write
            // the same framebuffer result; one draw without alpha test is exact.
            if (stateShader)
                setStateShaderUniforms(AlphaTestState{}, state.fog, state.gouraudShading, 1);
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
                if (stateShader)
                    setStateShaderUniforms(failedAlpha, state.fog, state.gouraudShading, 1);
                switch (state.alphaFailAction)
                {
                case ScreenAlphaFailAction::FramebufferOnly:
                    rlDisableDepthMask();
                    break;
                case ScreenAlphaFailAction::DepthOnly:
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    rlEnableDepthMask();
                    break;
                case ScreenAlphaFailAction::RgbOnly:
                    glColorMask(!state.alphaOnlyWrite, !state.alphaOnlyWrite, !state.alphaOnlyWrite, GL_FALSE);
                    rlDisableDepthMask();
                    break;
                case ScreenAlphaFailAction::Keep:
                default:
                    break;
                }
                drawVertices();
            }
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        rlViewport(0, 0, std::max(GetRenderWidth(), 1), std::max(GetRenderHeight(), 1));
        rlSetMatrixProjection(m_hostProjection);
        rlSetMatrixModelview(m_hostModelview);
        if (stateShader)
            EndShaderMode();
        rlSetTexture(0);
        rlEnableColorBlend();
        rlSetBlendMode(RL_BLEND_ALPHA);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        rlEnableDepthTest();
        rlEnableDepthMask();
        glDepthFunc(GL_LEQUAL);
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
    std::vector<Texture2D> m_atlasPages;
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
            writeAtlasSlot(small);
    }

    void dropSmallTexture(const std::string &key)
    {
        const auto found = m_smallTextures.find(key);
        if (found == m_smallTextures.end())
            return;
        m_atlas.release(found->second.slot);
        m_smallTextures.erase(found);
    }

    Texture2D &atlasPage(int page)
    {
        while (static_cast<int>(m_atlasPages.size()) <= page)
        {
            std::vector<std::uint8_t> blank(static_cast<std::size_t>(detail::AtlasAllocator::kPageSize) *
                                                detail::AtlasAllocator::kPageSize * 4u, 0u);
            Image image{};
            image.data = blank.data();
            image.width = image.height = detail::AtlasAllocator::kPageSize;
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            Texture2D texture = LoadTextureFromImage(image);
            SetTextureFilter(texture, TEXTURE_FILTER_BILINEAR);
            m_atlasPages.push_back(texture);
        }
        return m_atlasPages[static_cast<std::size_t>(page)];
    }

    void writeAtlasSlot(const SmallTexture &small)
    {
        detail::padAtlasPixels(small.rgba.data(), small.slot.w, small.slot.h, m_atlasScratch);
        const Rectangle area{static_cast<float>(small.slot.x - 1), static_cast<float>(small.slot.y - 1),
                             static_cast<float>(small.slot.w + 2), static_cast<float>(small.slot.h + 2)};
        UpdateTextureRec(atlasPage(small.slot.page), area, m_atlasScratch.data());
    }

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
        unsigned int texture = 0u;
        SmallTexture *small = nullptr;
        if (!textureName.empty())
        {
            const std::string key = textureKey(textureName);
            const auto found = m_textures.find(key);
            if (found != m_textures.end())
            {
                texture = found->second.id;
                bool inside = true;
                for (const ScreenVertex &v : vertices)
                    if (!(v.u >= -1e-4f && v.u <= 1.0001f && v.v >= -1e-4f && v.v <= 1.0001f))
                    {
                        inside = false;
                        break;
                    }
                if (inside)
                    small = atlasSlotFor(key);
            }
        }
        const unsigned int id = small ? atlasPage(small->slot.page).id : texture;
        if (!m_batch.accepts(sourceWidth, sourceHeight, id, state.linearFilter, m_screenPlacement, state, vertices.data(),
                             vertices.size()))
        {
            flushScreenBatch();
            m_batch.begin(sourceWidth, sourceHeight, id, state.linearFilter, m_screenPlacement, state);
        }
        m_batch.added(vertices.data(), vertices.size());
        const std::size_t first = m_batch.vertices.size();
        m_batch.vertices.insert(m_batch.vertices.end(), vertices.begin(), vertices.end());
        if (small)
        {
            ScreenVertex *added = m_batch.vertices.data() + first;
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
    }

    void flushScreenBatch()
    {
        if (!m_batch.active)
            return;
        m_batch.active = false;
        if (m_batch.vertices.empty())
            return;
        const ScreenPlacement saved = m_screenPlacement;
        m_screenPlacement = m_batch.placement;
        drawScreenPrimitive(m_batch.vertices, m_batch.sourceWidth, m_batch.sourceHeight, std::string_view{}, m_batch.state,
                            RL_TRIANGLES, static_cast<long>(m_batch.texture));
        m_screenPlacement = saved;
        m_batch.vertices.clear();
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
    std::unordered_map<std::string, RenderTexture2D> m_targets;
    RenderTexture2D *m_target = nullptr;
    int m_widescreen = 0;
    ScreenPlacement m_screenPlacement = ScreenPlacement::Centre;
    WorldPlacement m_worldPlacement = WorldPlacement::Full;
    std::string m_capturePath;
};
} // namespace

std::unique_ptr<Renderer> createPlatformRenderer()
{
    return std::make_unique<RaylibRenderer>();
}
} // namespace p3fes::native_renderer
