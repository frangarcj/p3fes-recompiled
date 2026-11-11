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
    enum class CullMode : std::uint8_t
    {
        None,
        Back,
        Front,
    };

    bool depthTest = true;
    bool depthWrite = true;
    CullMode cullMode = CullMode::Back;
    bool blendEnabled = true;

    // Optional per-draw RenderWare material colours. Persona 3 mutates these
    // for model fades/tints immediately before AtomicDefaultRenderCallBack,
    // so they cannot be baked permanently into the cached GPU mesh.
    std::vector<std::array<std::uint8_t, 4>> materialColors;

    struct UvTransform
    {
        bool enabled = false;
        // RenderWare Matrix rows 0/1/3 reduced to the 2D transform sent by
        // the PS2 MatFX UV bridge: uv' = uv.x * right + uv.y * up + pos.
        float rightX = 1.0f, rightY = 0.0f;
        float upX = 0.0f, upY = 1.0f;
        float posX = 0.0f, posY = 0.0f;
    };
    std::vector<UvTransform> materialUvTransforms;

    struct DualPass
    {
        bool enabled = false;
        std::string textureName;
        UvTransform uvTransform{};
        // Raw RenderWare BlendFunction values (ZERO=1, ONE=2,
        // SRCALPHA=5, INVSRCALPHA=6, DESTALPHA=7, ...).
        std::uint8_t srcBlend = 5u;
        std::uint8_t dstBlend = 6u;
    };
    std::vector<DualPass> materialDualPasses;

    struct EnvPass
    {
        bool enabled = false;
        std::string textureName;
        float coefficient = 0.0f;
        bool frameBufferAlpha = false;
        // World-space axes of the environment frame. librw-vita generates
        // env UVs by transforming the world normal by inverse(frame LTM), then
        // mapping x/y from [-1,1] to [0,1].
        std::array<float, 3> frameRight{1.0f, 0.0f, 0.0f};
        std::array<float, 3> frameUp{0.0f, 1.0f, 0.0f};
    };
    std::vector<EnvPass> materialEnvPasses;

    struct BlendState
    {
        bool enabled = false;
        // Raw RenderWare BlendFunction values (ZERO=1, ONE=2,
        // SRCCOLOR=3, INVSRCCOLOR=4, SRCALPHA=5, INVSRCALPHA=6, ...).
        std::uint8_t src = 5u;
        std::uint8_t dst = 6u;
    };
    std::vector<BlendState> materialBlends;

    // RenderWare world lighting, kept in the same form used by librw-vita's
    // default shader: ambient, directional, point and spot lights.
    struct DirectionalLight
    {
        std::array<float, 3> direction{};
        std::array<float, 3> color{};
    };
    struct LocalLight
    {
        enum class Type : std::uint8_t
        {
            Point,
            Spot,
            SoftSpot,
        };
        Type type = Type::Point;
        std::array<float, 3> position{};
        std::array<float, 3> direction{};
        std::array<float, 3> color{};
        float radius = 1.0f;
        float minusCosAngle = -1.0f;
    };
    bool lightingEnabled = false;
    std::array<float, 3> ambientLight{};
    std::vector<DirectionalLight> directionalLights;
    std::vector<LocalLight> localLights;

    // RenderWare's per-atomic skin palette, produced by the game's own
    // _rpSkinMatrixUpdating. Matrices transform bind-pose vertices back into
    // the atomic's local space; the regular model matrix is applied after.
    std::vector<Matrix4> skinPalette;
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
