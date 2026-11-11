#include "p3fes_native_present.h"
#include "../gs/gs_draw_control.h"

#if !defined(__ANDROID__)

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace p3fes_native_present
{
using namespace p3fes::native_renderer;

namespace
{
struct MeshDraw
{
    MeshId id = 0;
    Matrix4 model = kIdentityMatrix;
    MeshDrawState state{};
};

struct TransientMeshDraw
{
    MeshData mesh;
    Matrix4 model = kIdentityMatrix;
    MeshDrawState state{};
};

struct WorldLineDraw
{
    std::vector<Vertex> vertices;
    Matrix4 model = kIdentityMatrix;
    std::string textureName;
    MeshDrawState state{};
};

struct ScreenDraw
{
    std::vector<ScreenVertex> vertices;
    float sourceWidth = 640.0f;
    float sourceHeight = 448.0f;
    std::string textureName;
    ScreenDrawState state{};
    bool lines = false;
};

struct TextureDefinition
{
    TextureData texture;
    uint64_t revision = 0;
};

using Draw = std::variant<MeshDraw, TransientMeshDraw, WorldLineDraw, ScreenDraw>;

struct Frame
{
    Camera camera{};
    ClearColor clear{};
    std::vector<Draw> draws;
    uint64_t sequence = 0;
};

std::atomic<bool> g_active{false};
std::mutex g_mutex;
Frame g_building;
bool g_buildingOpen = false;
std::optional<Frame> g_pending;
uint64_t g_sequence = 0;
std::unordered_map<MeshId, MeshData> g_meshDefinitions;
std::unordered_map<std::string, TextureDefinition> g_textureDefinitions;

// Host-thread-only state.
std::unique_ptr<Renderer> g_renderer;
std::unordered_set<MeshId> g_uploaded;
std::unordered_map<std::string, uint64_t> g_uploadedTextures;
std::optional<Frame> g_hostFrame;
uint32_t g_errors = 0;
bool g_textureUploadLogged = false;
bool g_textureUpdateLogged = false;

bool ensureRenderer()
{
    if (!g_renderer)
        g_renderer = createPlatformRenderer();
    if (!g_renderer)
        return false;
    std::string error;
    if (!g_renderer->open(960, 544, "Persona 3 FES native", error))
    {
        if (g_errors++ < 4u)
            std::fprintf(stderr, "[p3fes-native] host renderer open failed: %s\n", error.c_str());
        return false;
    }
    return true;
}

bool ensureMesh(MeshId id)
{
    if (g_uploaded.find(id) != g_uploaded.end())
        return true;

    MeshData mesh;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_meshDefinitions.find(id);
        if (found == g_meshDefinitions.end())
            return false;
        mesh = found->second;
    }
    std::string error;
    if (!g_renderer->uploadMesh(id, mesh, error))
    {
        if (g_errors++ < 8u)
            std::fprintf(stderr, "[p3fes-native] host mesh upload failed: %s\n", error.c_str());
        return false;
    }
    g_uploaded.insert(id);
    return true;
}

bool ensureTexture(const std::string &name)
{
    if (name.empty())
        return true;

    TextureDefinition definition;
    bool updating = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto found = g_textureDefinitions.find(name);
        if (found == g_textureDefinitions.end())
            return false;
        const auto uploaded = g_uploadedTextures.find(name);
        if (uploaded != g_uploadedTextures.end() && uploaded->second == found->second.revision)
            return true;
        updating = uploaded != g_uploadedTextures.end();
        definition = found->second;
    }

    std::string error;
    if (!g_renderer->uploadTexture(definition.texture, error))
    {
        if (g_errors++ < 8u)
            std::fprintf(stderr, "[p3fes-native] host texture upload failed: %s\n", error.c_str());
        return false;
    }
    g_uploadedTextures[name] = definition.revision;
    if (!g_textureUploadLogged)
    {
        g_textureUploadLogged = true;
        std::fprintf(stderr, "[p3fes-native] host texture upload active: %s %ux%u\n",
                     name.c_str(), definition.texture.width, definition.texture.height);
    }
    if (updating && !g_textureUpdateLogged)
    {
        g_textureUpdateLogged = true;
        std::fprintf(stderr, "[p3fes-native] host texture in-place update active: %s %ux%u\n",
                     name.c_str(), definition.texture.width, definition.texture.height);
    }
    return true;
}
} // namespace

void setActive(bool active)
{
    g_active.store(active, std::memory_order_release);
    ps2x::gs::setPrimitiveRasterSuppressed(active);
    if (!active)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_building = {};
        g_buildingOpen = false;
        g_pending.reset();
    }
}

bool active()
{
    return g_active.load(std::memory_order_acquire);
}

void beginFrame(const Camera &camera, ClearColor clear)
{
    if (!active())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        return;
    g_building = {};
    g_building.camera = camera;
    g_building.clear = clear;
    g_building.sequence = ++g_sequence;
    g_buildingOpen = true;
}

void defineMesh(MeshId id, const MeshData &mesh)
{
    if (!active())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_meshDefinitions.try_emplace(id, mesh);
}

void defineTexture(const TextureData &texture, uint64_t revision)
{
    if (!active() || texture.name.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto &definition = g_textureDefinitions[texture.name];
    if (definition.revision == revision && !definition.texture.rgba.empty())
        return;
    definition.texture = texture;
    definition.revision = revision;
}

void drawMesh(MeshId id, const Matrix4 &model, MeshDrawState state)
{
    if (!active())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(MeshDraw{id, model, std::move(state)});
}

void drawTransientMesh(MeshData mesh, const Matrix4 &model, MeshDrawState state)
{
    if (!active() || mesh.vertices.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(TransientMeshDraw{std::move(mesh), model, std::move(state)});
}

void drawWorldLines(std::vector<Vertex> vertices, const Matrix4 &model,
                    std::string textureName, MeshDrawState state)
{
    if (!active() || vertices.size() < 2u)
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(WorldLineDraw{std::move(vertices), model, std::move(textureName), std::move(state)});
}

void drawScreenTriangles(std::vector<ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                         std::string textureName, ScreenDrawState state)
{
    if (!active() || vertices.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(ScreenDraw{std::move(vertices), sourceWidth, sourceHeight,
                                                std::move(textureName), state, false});
}

void drawScreenLines(std::vector<ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                     std::string textureName, ScreenDrawState state)
{
    if (!active() || vertices.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(ScreenDraw{std::move(vertices), sourceWidth, sourceHeight,
                                                std::move(textureName), state, true});
}

void endFrame()
{
    if (!active())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_buildingOpen)
        return;
    // Latest complete frame wins. The host may present at 60 Hz while the game
    // submits at 30 Hz, so keeping only one pending frame bounds memory/latency.
    g_pending = std::move(g_building);
    g_building = {};
    g_buildingOpen = false;
}

bool renderHostFrame()
{
    if (!active())
        return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_pending)
        {
            g_hostFrame = std::move(g_pending);
            g_pending.reset();
        }
    }
    if (!g_hostFrame || !ensureRenderer())
        return false;

    const Frame &frame = *g_hostFrame;
    g_renderer->beginFrame(frame.camera, frame.clear);
    uint64_t transientIndex = 0u;
    for (const Draw &draw : frame.draws)
    {
        if (const auto *mesh = std::get_if<MeshDraw>(&draw))
        {
            if (ensureMesh(mesh->id))
                g_renderer->drawMesh(mesh->id, mesh->model, mesh->state);
        }
        else if (const auto *transient = std::get_if<TransientMeshDraw>(&draw))
        {
            bool texturesReady = true;
            for (const Material &material : transient->mesh.materials)
                if (!ensureTexture(material.textureName))
                    texturesReady = false;
            if (!texturesReady)
                continue;

            // Bit 62 is outside the persistent atomic/sector ID spaces used by
            // this port. The mesh is removed immediately after the draw.
            const MeshId id = (1ull << 62u) | ((frame.sequence & 0x3FFFFFFFull) << 20u) |
                              (transientIndex++ & 0xFFFFFull);
            std::string error;
            if (!g_renderer->uploadMesh(id, transient->mesh, error))
            {
                if (g_errors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] transient mesh upload failed: %s\n", error.c_str());
                continue;
            }
            g_renderer->drawMesh(id, transient->model, transient->state);
            g_renderer->removeMesh(id);
        }
        else if (const auto *lines = std::get_if<WorldLineDraw>(&draw))
        {
            if (ensureTexture(lines->textureName))
                g_renderer->drawWorldLines(lines->vertices, lines->model, lines->textureName, lines->state);
        }
        else if (const auto *screen = std::get_if<ScreenDraw>(&draw))
        {
            if (ensureTexture(screen->textureName))
            {
                if (screen->lines)
                    g_renderer->drawScreenLines(screen->vertices, screen->sourceWidth, screen->sourceHeight,
                                                screen->textureName, screen->state);
                else
                    g_renderer->drawScreenTriangles(screen->vertices, screen->sourceWidth, screen->sourceHeight,
                                                    screen->textureName, screen->state);
            }
        }
    }
    g_renderer->endFrame();
    return true;
}

void shutdownHost()
{
    if (g_renderer)
        g_renderer->close();
    g_renderer.reset();
    g_uploaded.clear();
    g_uploadedTextures.clear();
    g_textureUploadLogged = false;
    g_textureUpdateLogged = false;
    g_hostFrame.reset();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_meshDefinitions.clear();
        g_textureDefinitions.clear();
        g_pending.reset();
        g_building = {};
        g_buildingOpen = false;
    }
    g_active.store(false, std::memory_order_release);
    ps2x::gs::setPrimitiveRasterSuppressed(false);
}
} // namespace p3fes_native_present

#endif
