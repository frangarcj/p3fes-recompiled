#include <rw.h>
#include <raylib.h>
#include <raymath.h>

#include "mesh_convert.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
struct Options
{
    const char *modelPath = nullptr;
    int frames = 0;
};

std::uint32_t readLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t findRwOffset(const std::vector<std::uint8_t> &data)
{
    const std::size_t scanEnd = std::min<std::size_t>(data.size(), 0x100);
    for (std::size_t offset = 0; offset + 12 <= scanEnd; ++offset)
    {
        const std::uint32_t id = readLe32(data.data() + offset);
        const std::uint32_t size = readLe32(data.data() + offset + 4);
        if ((id == rw::ID_TEXDICTIONARY || id == rw::ID_CLUMP) &&
            size <= data.size() - offset - 12)
            return offset;
    }
    return data.size();
}

std::vector<std::uint8_t> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

rw::Clump *readClump(std::uint8_t *data, std::size_t size)
{
    rw::StreamMemory stream;
    stream.open(data, static_cast<rw::uint32>(size));
    rw::Clump *clump = nullptr;
    if (rw::findChunk(&stream, rw::ID_CLUMP, nullptr, nullptr))
        clump = rw::Clump::streamRead(&stream);
    stream.close();
    return clump;
}

bool parseInt(const char *text, int &value)
{
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!text[0] || (end && *end) || parsed < 0 || parsed > 1000000)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parseArgs(int argc, char **argv, Options &out)
{
    if (argc < 2)
        return false;
    out.modelPath = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            if (!parseInt(argv[++i], out.frames))
                return false;
        }
        else
        {
            return false;
        }
    }
    return true;
}

std::vector<std::uint16_t> triangleList(const p3fes::native_rw::MeshData &mesh)
{
    std::vector<std::uint16_t> out;
    if (!mesh.triangleStrip)
    {
        out.reserve(mesh.indices.size());
        for (std::uint32_t index : mesh.indices)
            out.push_back(static_cast<std::uint16_t>(index));
        return out;
    }

    for (const p3fes::native_rw::Submesh &submesh : mesh.submeshes)
    {
        const std::size_t begin = submesh.firstIndex;
        const std::size_t end = begin + submesh.indexCount;
        for (std::size_t i = begin + 2; i < end; ++i)
        {
            std::uint32_t a = mesh.indices[i - 2];
            std::uint32_t b = mesh.indices[i - 1];
            const std::uint32_t c = mesh.indices[i];
            if (a == b || b == c || a == c)
                continue;
            if (((i - begin) & 1u) != 0u)
                std::swap(a, b);
            out.push_back(static_cast<std::uint16_t>(a));
            out.push_back(static_cast<std::uint16_t>(b));
            out.push_back(static_cast<std::uint16_t>(c));
        }
    }
    return out;
}

Mesh makeRaylibMesh(const p3fes::native_rw::MeshData &source)
{
    Mesh mesh{};
    mesh.vertexCount = static_cast<int>(source.vertices.size());
    const std::vector<std::uint16_t> indices = triangleList(source);
    mesh.triangleCount = static_cast<int>(indices.size() / 3);
    mesh.vertices = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 3));
    mesh.normals = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 3));
    mesh.texcoords = static_cast<float *>(MemAlloc(sizeof(float) * source.vertices.size() * 2));
    mesh.colors = static_cast<unsigned char *>(MemAlloc(sizeof(unsigned char) * source.vertices.size() * 4));
    mesh.indices = static_cast<unsigned short *>(MemAlloc(sizeof(unsigned short) * indices.size()));

    for (std::size_t i = 0; i < source.vertices.size(); ++i)
    {
        const auto &vertex = source.vertices[i];
        std::memcpy(mesh.vertices + i * 3, vertex.position.data(), sizeof(float) * 3);
        std::memcpy(mesh.normals + i * 3, vertex.normal.data(), sizeof(float) * 3);
        std::memcpy(mesh.texcoords + i * 2, vertex.texCoord.data(), sizeof(float) * 2);
        std::memcpy(mesh.colors + i * 4, vertex.color.data(), 4);
    }
    std::memcpy(mesh.indices, indices.data(), sizeof(unsigned short) * indices.size());
    UploadMesh(&mesh, false);
    return mesh;
}

struct Bounds
{
    Vector3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vector3 max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
};

void include(Bounds &bounds, const p3fes::native_rw::MeshData &mesh)
{
    for (const auto &vertex : mesh.vertices)
    {
        bounds.min.x = std::min(bounds.min.x, vertex.position[0]);
        bounds.min.y = std::min(bounds.min.y, vertex.position[1]);
        bounds.min.z = std::min(bounds.min.z, vertex.position[2]);
        bounds.max.x = std::max(bounds.max.x, vertex.position[0]);
        bounds.max.y = std::max(bounds.max.y, vertex.position[1]);
        bounds.max.z = std::max(bounds.max.z, vertex.position[2]);
    }
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseArgs(argc, argv, options))
    {
        std::fprintf(stderr, "Usage: %s MODEL.RMD [--frames N]\n", argv[0]);
        return 2;
    }

    std::vector<std::uint8_t> file = readFile(options.modelPath);
    const std::size_t rwOffset = findRwOffset(file);
    if (file.empty() || rwOffset == file.size())
    {
        std::fprintf(stderr, "Could not find a RenderWare stream in %s\n", options.modelPath);
        return 1;
    }

    rw::Engine::init();
    rw::registerHAnimPlugin();
    rw::registerSkinPlugin();
    rw::registerMatFXPlugin();
    rw::ps2::registerPlatformPlugins();
    rw::ps2::registerNativeDataPlugin();
    rw::ps2::registerADCPlugin();
    if (!rw::Engine::open(nullptr) || !rw::Engine::start())
        return 1;

    rw::Clump *clump = readClump(file.data() + rwOffset, file.size() - rwOffset);
    if (clump == nullptr)
        return 1;

    std::vector<p3fes::native_rw::MeshData> neutralMeshes;
    Bounds bounds;
    FORLIST(lnk, clump->atomics)
    {
        rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
        if (atomic->geometry == nullptr)
            continue;
        std::string error;
        auto mesh = p3fes::native_rw::convertGeometry(*atomic->geometry, error);
        if (!mesh)
        {
            std::fprintf(stderr, "Could not convert an atomic: %s\n", error.c_str());
            continue;
        }
        include(bounds, *mesh);
        neutralMeshes.push_back(std::move(*mesh));
    }
    clump->destroy();
    rw::Engine::stop();
    rw::Engine::close();
    rw::Engine::term();
    if (neutralMeshes.empty())
        return 1;

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(960, 544, "Persona 3 FES direct RenderWare viewer");

    std::vector<Mesh> meshes;
    meshes.reserve(neutralMeshes.size());
    for (const auto &mesh : neutralMeshes)
        meshes.push_back(makeRaylibMesh(mesh));
    Material material = LoadMaterialDefault();

    const Vector3 center{
        (bounds.min.x + bounds.max.x) * 0.5f,
        (bounds.min.y + bounds.max.y) * 0.5f,
        (bounds.min.z + bounds.max.z) * 0.5f,
    };
    const float dx = bounds.max.x - bounds.min.x;
    const float dy = bounds.max.y - bounds.min.y;
    const float dz = bounds.max.z - bounds.min.z;
    const float radius = std::max({dx, dy, dz, 1.0f}) * 0.75f;

    Camera3D camera{};
    camera.target = center;
    camera.position = {center.x + radius * 1.6f, center.y + radius * 0.8f, center.z + radius * 1.6f};
    camera.up = {0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;
    SetTargetFPS(60);

    int frame = 0;
    while (!WindowShouldClose() && (options.frames <= 0 || frame < options.frames))
    {
        const float phase = static_cast<float>(frame) * 0.008f;
        camera.position.x = center.x + std::cos(phase) * radius * 2.2f;
        camera.position.z = center.z + std::sin(phase) * radius * 2.2f;

        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode3D(camera);
        for (const Mesh &mesh : meshes)
            DrawMesh(mesh, material, MatrixIdentity());
        DrawGrid(20, std::max(radius * 0.1f, 1.0f));
        EndMode3D();
        DrawText(TextFormat("%s  atomics:%d", GetFileName(options.modelPath), static_cast<int>(meshes.size())),
                 12, 12, 20, RAYWHITE);
        EndDrawing();
        ++frame;
    }

    UnloadMaterial(material);
    for (Mesh &mesh : meshes)
        UnloadMesh(mesh);
    CloseWindow();
    return 0;
}
