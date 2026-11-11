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
#include <vector>

namespace p3fes::native_renderer
{
using MeshId = std::uint64_t;
using Matrix4 = std::array<float, 16>;

inline constexpr Matrix4 kIdentityMatrix{
    1.0f, 0.0f, 0.0f, 0.0f,
    0.0f, 1.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 1.0f, 0.0f,
    0.0f, 0.0f, 0.0f, 1.0f,
};

struct Camera
{
    std::array<float, 3> position{};
    std::array<float, 3> target{};
    std::array<float, 3> up{0.0f, 1.0f, 0.0f};
    float fovy = 45.0f;
    // In-game RenderWare cameras provide their exact host view/projection
    // conversion. Standalone viewers can leave this false and use the
    // position/target/fovy fields above.
    bool useMatrices = false;
    Matrix4 view = kIdentityMatrix;
    Matrix4 projection = kIdentityMatrix;
};

struct ClearColor
{
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

// Host-side RenderWare Im2D vertex. Coordinates stay in the game's logical
// 640x448-ish screen space; the backend scales them to the progressive host
// framebuffer. Texture binding is intentionally separate so untextured fades
// and colour-only UI can move off the GS before live RwRaster upload support.
struct ScreenVertex
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float u = 0.0f, v = 0.0f;
    std::array<std::uint8_t, 4> color{255u, 255u, 255u, 255u};
};

enum class ScreenBlend : std::uint8_t
{
    Alpha,
    Additive,
    Subtractive,
    Opaque,
};

struct ScreenDrawState
{
    ScreenBlend blend = ScreenBlend::Alpha;
    bool clampU = false;
    bool clampV = false;
    bool linearFilter = true;
};

struct MeshDrawState
{
    // Optional per-draw RenderWare material colours. Persona 3 mutates these
    // for model fades/tints immediately before AtomicDefaultRenderCallBack,
    // so they cannot be baked permanently into the cached GPU mesh.
    std::vector<std::array<std::uint8_t, 4>> materialColors;
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
    virtual void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state = {}) = 0;
    void drawMesh(MeshId id) { drawMesh(id, kIdentityMatrix, {}); }
    virtual void drawScreenTriangles(const std::vector<ScreenVertex> &vertices,
                                     float sourceWidth, float sourceHeight,
                                     std::string_view textureName = {},
                                     const ScreenDrawState &state = {}) = 0;
    virtual void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                                 float sourceWidth, float sourceHeight,
                                 std::string_view textureName = {},
                                 const ScreenDrawState &state = {}) = 0;
    virtual void endFrame() = 0;
};

// One platform backend behind the same asset/draw contract: raylib/OpenGL on
// desktop, vitaGL on Vita. The asset path stays free of either graphics API.
std::unique_ptr<Renderer> createPlatformRenderer();
} // namespace p3fes::native_renderer
