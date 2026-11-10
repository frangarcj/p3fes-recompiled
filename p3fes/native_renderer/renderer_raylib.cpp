#include "renderer.h"

#include <raylib.h>
#include <raymath.h>

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

std::string textureKey(std::string_view name)
{
    std::string out(name);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return out;
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
        m_textures.emplace(textureKey(source.name), texture);
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
            if (!source.materials.empty())
            {
                part.color = source.materials[submesh.materialIndex].color;
                part.textureName = source.materials[submesh.materialIndex].textureName;
            }
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
        Camera3D rayCamera{};
        rayCamera.position = {camera.position[0], camera.position[1], camera.position[2]};
        rayCamera.target = {camera.target[0], camera.target[1], camera.target[2]};
        rayCamera.up = {camera.up[0], camera.up[1], camera.up[2]};
        rayCamera.fovy = camera.fovy;
        rayCamera.projection = CAMERA_PERSPECTIVE;
        m_camera = rayCamera;

        BeginDrawing();
        ClearBackground({clear.r, clear.g, clear.b, clear.a});
        BeginMode3D(m_camera);
    }

    void drawMesh(MeshId id) override
    {
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;

        for (const DrawPart &part : found->second)
        {
            ::Material drawMaterial = m_material;
            std::array<MaterialMap, kMaterialMapCount> maps{};
            std::copy_n(m_material.maps, kMaterialMapCount, maps.data());
            drawMaterial.maps = maps.data();
            MaterialMap &diffuse = drawMaterial.maps[MATERIAL_MAP_DIFFUSE];
            diffuse.color = {part.color[0], part.color[1], part.color[2], part.color[3]};
            if (!part.textureName.empty())
            {
                auto texture = m_textures.find(textureKey(part.textureName));
                if (texture != m_textures.end())
                    diffuse.texture = texture->second;
            }
            DrawMesh(part.mesh, drawMaterial, MatrixIdentity());
        }
    }

    void endFrame() override
    {
        EndMode3D();
        EndDrawing();
    }

private:
    bool m_open = false;
    bool m_ownsWindow = false;
    ::Material m_material{};
    Camera3D m_camera{};
    std::unordered_map<MeshId, std::vector<DrawPart>> m_meshes;
    std::unordered_map<std::string, Texture2D> m_textures;
};
} // namespace

std::unique_ptr<Renderer> createPlatformRenderer()
{
    return std::make_unique<RaylibRenderer>();
}
} // namespace p3fes::native_renderer
