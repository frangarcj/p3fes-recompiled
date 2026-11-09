#include "renderer.h"

#include <raylib.h>
#include <raymath.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace p3fes::native_renderer
{
namespace
{
std::vector<std::uint16_t> triangleList(const MeshData &mesh, std::string &error)
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

    if (!mesh.triangleStrip)
    {
        out.reserve(mesh.indices.size());
        for (std::uint32_t index : mesh.indices)
            if (!append(index))
                return {};
        return out;
    }

    for (const Submesh &submesh : mesh.submeshes)
    {
        const std::size_t begin = submesh.firstIndex;
        const std::size_t end = begin + submesh.indexCount;
        if (end > mesh.indices.size())
        {
            error = "submesh index range is outside the mesh index buffer";
            return {};
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
    }
    return out;
}

class RaylibRenderer final : public Renderer
{
public:
    ~RaylibRenderer() override { close(); }

    bool open(int width, int height, const char *title, std::string &) override
    {
        if (m_open)
            return true;
        SetTraceLogLevel(LOG_WARNING);
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
        InitWindow(width, height, title);
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
        for (auto &[id, mesh] : m_meshes)
            UnloadMesh(mesh);
        m_meshes.clear();
        UnloadMaterial(m_material);
        CloseWindow();
        m_open = false;
    }

    bool shouldClose() const override { return !m_open || WindowShouldClose(); }
    void setTargetFps(int fps) override { SetTargetFPS(fps); }

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

        std::vector<std::uint16_t> indices = triangleList(source, error);
        if (indices.empty())
        {
            if (error.empty())
                error = "mesh has no drawable triangles";
            return false;
        }

        removeMesh(id);
        Mesh mesh{};
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
        m_meshes.emplace(id, mesh);
        return true;
    }

    void removeMesh(MeshId id) override
    {
        auto found = m_meshes.find(id);
        if (found == m_meshes.end())
            return;
        UnloadMesh(found->second);
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
        if (found != m_meshes.end())
            DrawMesh(found->second, m_material, MatrixIdentity());
    }

    void endFrame() override
    {
        EndMode3D();
        EndDrawing();
    }

private:
    bool m_open = false;
    ::Material m_material{};
    Camera3D m_camera{};
    std::unordered_map<MeshId, Mesh> m_meshes;
};
} // namespace

std::unique_ptr<Renderer> createDesktopRenderer()
{
    return std::make_unique<RaylibRenderer>();
}
} // namespace p3fes::native_renderer
