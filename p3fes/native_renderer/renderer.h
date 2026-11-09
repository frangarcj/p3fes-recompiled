// Minimal native renderer boundary inspired by HenryKun55/celosia's
// native_renderer API (GPL-3.0). Keep this header free of raylib/vitaGL/Metal
// so the same asset path can feed the Mac and Vita backends.
#pragma once

#include "mesh.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace p3fes::native_renderer
{
using MeshId = std::uint64_t;

struct Camera
{
    std::array<float, 3> position{};
    std::array<float, 3> target{};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    float fovy = 45.0f;
};

struct ClearColor
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

class Renderer
{
public:
    virtual ~Renderer() = default;

    virtual bool open(int width, int height, const char *title, std::string &error) = 0;
    virtual void close() = 0;
    virtual bool shouldClose() const = 0;
    virtual void setTargetFps(int fps) = 0;

    virtual bool uploadTexture(const TextureData &texture, std::string &error) = 0;
    virtual void removeTexture(std::string_view name) = 0;
    virtual bool uploadMesh(MeshId id, const MeshData &mesh, std::string &error) = 0;
    virtual void removeMesh(MeshId id) = 0;

    virtual void beginFrame(const Camera &camera, ClearColor clear) = 0;
    virtual void drawMesh(MeshId id) = 0;
    virtual void endFrame() = 0;
};

// Desktop proof backend. Vita intentionally gets its own vitaGL implementation
// instead of growing the Celosia raylib shim into a second 3D API.
std::unique_ptr<Renderer> createDesktopRenderer();
} // namespace p3fes::native_renderer
