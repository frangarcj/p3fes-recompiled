#include "p3fes_native_present.h"

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
};

struct ScreenDraw
{
    std::vector<ScreenVertex> vertices;
    float sourceWidth = 640.0f;
    float sourceHeight = 448.0f;
};

using Draw = std::variant<MeshDraw, ScreenDraw>;

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

// Host-thread-only state.
std::unique_ptr<Renderer> g_renderer;
std::unordered_set<MeshId> g_uploaded;
std::optional<Frame> g_hostFrame;
uint32_t g_errors = 0;

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
} // namespace

void setActive(bool active)
{
    g_active.store(active, std::memory_order_release);
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

void drawMesh(MeshId id, const Matrix4 &model)
{
    if (!active())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(MeshDraw{id, model});
}

void drawScreenTriangles(std::vector<ScreenVertex> vertices, float sourceWidth, float sourceHeight)
{
    if (!active() || vertices.empty())
        return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_buildingOpen)
        g_building.draws.emplace_back(ScreenDraw{std::move(vertices), sourceWidth, sourceHeight});
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
    for (const Draw &draw : frame.draws)
    {
        if (const auto *mesh = std::get_if<MeshDraw>(&draw))
        {
            if (ensureMesh(mesh->id))
                g_renderer->drawMesh(mesh->id, mesh->model);
        }
        else if (const auto *screen = std::get_if<ScreenDraw>(&draw))
        {
            g_renderer->drawScreenTriangles(screen->vertices, screen->sourceWidth, screen->sourceHeight);
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
    g_hostFrame.reset();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_meshDefinitions.clear();
        g_pending.reset();
        g_building = {};
        g_buildingOpen = false;
    }
    g_active.store(false, std::memory_order_release);
}
} // namespace p3fes_native_present

#endif
