#include <raylib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    std::fprintf(stderr, "Usage: %s MODEL.gltf [--frames N]\n", argv0);
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
    if (!(modelPath.ends_with(".gltf") || modelPath.ends_with(".glb")))
    {
        std::fprintf(stderr, "The native viewer currently accepts glTF only: %s\n", options.modelPath);
        return 2;
    }

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
        DrawText(TextFormat("%s  meshes:%d materials:%d", GetFileName(options.modelPath), model.meshCount,
                            model.materialCount),
                 12, 12, 20, RAYWHITE);
        EndDrawing();

        ++frame;
    }

    UnloadModel(model);
    CloseWindow();
    return 0;
}
