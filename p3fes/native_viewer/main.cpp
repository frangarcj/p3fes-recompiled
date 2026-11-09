#include <raylib.h>

#include "native_renderer/native_model.h"
#include "native_renderer/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace
{
struct Options
{
    const char *modelPath = nullptr;
    int frames = 0;
};

void usage(const char *argv0)
{
    std::fprintf(stderr, "Usage: %s MODEL.p3n|MODEL.gltf [--frames N]\n", argv0);
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

Vector3 centerOf(const BoundingBox &box)
{
    return {
        (box.min.x + box.max.x) * 0.5f,
        (box.min.y + box.max.y) * 0.5f,
        (box.min.z + box.max.z) * 0.5f,
    };
}

float radiusOf(const BoundingBox &box)
{
    const float dx = box.max.x - box.min.x;
    const float dy = box.max.y - box.min.y;
    const float dz = box.max.z - box.min.z;
    return std::max({dx, dy, dz, 1.0f}) * 0.75f;
}

struct NativeBounds
{
    std::array<float, 3> min{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> max{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
    };
};

NativeBounds nativeBoundsOf(const p3fes::native_renderer::NativeModel &model)
{
    NativeBounds bounds;
    for (const auto &mesh : model.meshes)
        for (const auto &vertex : mesh.vertices)
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
                bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
            }
    return bounds;
}

int runNativePack(const Options &options)
{
    p3fes::native_renderer::NativeModel model;
    std::string error;
    if (!p3fes::native_renderer::loadNativeModel(options.modelPath, model, error) || model.meshes.empty())
    {
        std::fprintf(stderr, "Could not load native model pack: %s\n", error.c_str());
        return 1;
    }

    std::unique_ptr<p3fes::native_renderer::Renderer> renderer =
        p3fes::native_renderer::createDesktopRenderer();
    if (!renderer->open(960, 544, "Persona 3 FES native model viewer", error))
    {
        std::fprintf(stderr, "Could not open native renderer: %s\n", error.c_str());
        return 1;
    }
    for (const auto &texture : model.textures)
    {
        if (!renderer->uploadTexture(texture, error))
        {
            std::fprintf(stderr, "Could not upload texture %s: %s\n", texture.name.c_str(), error.c_str());
            return 1;
        }
    }
    for (std::size_t i = 0; i < model.meshes.size(); ++i)
    {
        if (!renderer->uploadMesh(static_cast<p3fes::native_renderer::MeshId>(i + 1), model.meshes[i], error))
        {
            std::fprintf(stderr, "Could not upload mesh %zu: %s\n", i, error.c_str());
            return 1;
        }
    }

    const NativeBounds bounds = nativeBoundsOf(model);
    const std::array<float, 3> center{
        (bounds.min[0] + bounds.max[0]) * 0.5f,
        (bounds.min[1] + bounds.max[1]) * 0.5f,
        (bounds.min[2] + bounds.max[2]) * 0.5f,
    };
    const float radius = std::max({
        bounds.max[0] - bounds.min[0],
        bounds.max[1] - bounds.min[1],
        bounds.max[2] - bounds.min[2],
        1.0f,
    }) * 0.75f;

    p3fes::native_renderer::Camera camera{};
    camera.target = center;
    camera.position = {center[0] + radius * 1.6f, center[1] + radius * 0.8f, center[2] + radius * 1.6f};
    renderer->setTargetFps(60);
    int frame = 0;
    while (!renderer->shouldClose() && (options.frames <= 0 || frame < options.frames))
    {
        const float phase = static_cast<float>(frame) * 0.008f;
        camera.position[0] = center[0] + std::cos(phase) * radius * 2.2f;
        camera.position[2] = center[2] + std::sin(phase) * radius * 2.2f;
        renderer->beginFrame(camera, {});
        for (std::size_t i = 0; i < model.meshes.size(); ++i)
            renderer->drawMesh(static_cast<p3fes::native_renderer::MeshId>(i + 1));
        renderer->endFrame();
        ++frame;
    }
    renderer->close();
    return 0;
}

int runGltf(const Options &options)
{
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(960, 544, "Persona 3 FES native asset viewer");

    Model model = LoadModel(options.modelPath);
    if (model.meshCount <= 0)
    {
        std::fprintf(stderr, "Could not load model: %s\n", options.modelPath);
        CloseWindow();
        return 1;
    }

    const BoundingBox bounds = GetModelBoundingBox(model);
    const Vector3 center = centerOf(bounds);
    const float radius = radiusOf(bounds);
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
        camera.position.y = center.y + radius * 0.8f;
        BeginDrawing();
        ClearBackground(BLACK);
        BeginMode3D(camera);
        DrawModel(model, {0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        DrawGrid(20, std::max(radius * 0.1f, 1.0f));
        EndMode3D();
        EndDrawing();
        ++frame;
    }
    UnloadModel(model);
    CloseWindow();
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseArgs(argc, argv, options))
    {
        usage(argv[0]);
        return 2;
    }

    const std::string modelPath(options.modelPath);
    if (modelPath.ends_with(".p3n"))
        return runNativePack(options);
    if (modelPath.ends_with(".gltf") || modelPath.ends_with(".glb"))
        return runGltf(options);
    std::fprintf(stderr, "Unsupported native viewer model: %s\n", options.modelPath);
    return 2;
}
