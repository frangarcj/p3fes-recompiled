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

    bool operator==(const Camera &) const = default;
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
    // Normalized GS depth: 0 is far and 1 is near. Backends map this to
    // window depth 1-z so the GS greater/greater-equal tests match GL's
    // less/less-equal depth comparisons.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float u = 0.0f, v = 0.0f;
    std::array<std::uint8_t, 4> color{255u, 255u, 255u, 255u};
};

enum class ScreenBlend : std::uint8_t
{
    Alpha,
    Additive,
    Subtractive,
    DestinationOnly,
    DestinationAlphaAdditive,
    Opaque,
    // GS ALPHA_1 0x54: Cs * Ad + Cd * (1 - Ad). Ad is the frame buffer's alpha, which the game fills with an
    // alpha-only pass (see ScreenDrawState::alphaOnlyWrite) just before.
    DestinationAlpha,
};

enum class ScreenDepthFunction : std::uint8_t
{
    Never,
    Always,
    GreaterEqual,
    Greater,
};

enum class ScreenAlphaFailAction : std::uint8_t
{
    Keep,
    FramebufferOnly,
    DepthOnly,
    RgbOnly,
};

constexpr std::uint8_t complementGsAlphaFunction(std::uint8_t function)
{
    constexpr std::uint8_t kComplement[8]{1u, 0u, 5u, 6u, 7u, 2u, 3u, 4u};
    return kComplement[function & 0x7u];
}

struct AlphaTestState
{
    bool enabled = false;
    // Raw GS TEST.ATST encoding: 0 NEVER, 1 ALWAYS, 2 LESS, 3 LEQUAL,
    // 4 EQUAL, 5 GEQUAL, 6 GREATER, 7 NOTEQUAL.
    std::uint8_t function = 1u;
    std::uint8_t reference = 0u;
};

struct FogState
{
    bool enabled = false;
    float start = 0.0f;
    float end = 1.0f;
    std::array<std::uint8_t, 3> color{0u, 0u, 0u};
};

struct ScreenDrawState
{
    ScreenBlend blend = ScreenBlend::Alpha;
    bool depthTest = false;
    bool depthWrite = false;
    ScreenDepthFunction depthFunction = ScreenDepthFunction::Always;
    ScreenAlphaFailAction alphaFailAction = ScreenAlphaFailAction::Keep;
    bool gouraudShading = true;
    bool clampU = false;
    bool clampV = false;
    bool linearFilter = true;
    AlphaTestState alphaTest{};
    FogState fog{};
    // GS TEST.DATE with DATM = 1: only pixels whose destination alpha is set (>= 0x80) are
    // written. Used on render targets whose alpha is a mask (the shadow map's silhouette);
    // backends approximate it by multiplying the written alpha by the destination alpha.
    bool destinationAlphaTest = false;
    // GS FRAME_1.FBMSK = 0x00FFFFFF: the draw writes only the frame buffer's alpha (its source alpha As), the colour
    // channels are masked. The game brackets effect passes with it (mdlEffect_003294d0 / 00329550) and blends the
    // following passes with Ad (ScreenBlend::DestinationAlpha / DestinationAlphaAdditive).
    bool alphaOnlyWrite = false;
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
    bool gouraudShading = true;
    bool clampU = false;
    bool clampV = false;
    bool linearFilter = true;
    AlphaTestState alphaTest{};
    FogState fog{};

    struct SamplerState
    {
        enum class Address : std::uint8_t
        {
            Wrap,
            Mirror,
            Clamp,
        };
        bool valid = false;
        Address u = Address::Wrap;
        Address v = Address::Wrap;
        bool linearFilter = true;
    };
    std::vector<SamplerState> materialSamplers;
    // Live resource handles, independent of the lifetime of cached geometry.
    // An entry (including an empty one) overrides its mesh material's binding.
    std::vector<std::string> materialTextures;

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
        SamplerState sampler{};
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
        SamplerState sampler{};
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
        // GS ALPHA_1 0x42, Cd - Cs * As: the equation is reverse subtract (src = SRCALPHA, dst = ONE).
        bool subtract = false;
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

    // Render-to-texture pass (RenderWare camera-texture rasters, e.g. the character shadow map).
    // Called outside beginFrame/endFrame, before the frame that samples the result. World draws,
    // setCamera and clear between the two calls go to a width x height colour+depth target whose
    // colour buffer is then sampled under `name` like an uploaded texture (same orientation: v = 0
    // is the top row). uploadTexture for that name is ignored while the target exists; removeTexture
    // destroys it.
    virtual void beginTextureTarget(std::string_view name, int width, int height) = 0;
    virtual void endTextureTarget() = 0;
    // Widescreen. 0: everything in the centred 4:3 area. 1: world and screen-space draws both use
    // the whole host framebuffer (2-D is stretched horizontally, as on a 16:9 TV). 2: world draws
    // use the whole framebuffer and screen-space draws keep square pixels (uniform scale), placed
    // by setScreenPlacement.
    virtual void setWidescreen(int mode) = 0;
    // Wide is Centre without the 4:3 clip: same uniform scale and position, but the viewport is the whole
    // framebuffer, so geometry beyond the source area (x < 0 or x > sourceWidth) shows in the side margins.
    // Used to extend flat edge-flush bars (see p3fes_native_present.cpp, menu frames).
    enum class ScreenPlacement : std::uint8_t { Centre, Left, Right, Stretch, Wide };
    // Widescreen mode 2 only: where the following screen-space draws put their 4:3 source area on
    // the wider framebuffer (against the left or right edge, centred, stretched over all of it, or centred
    // and unclipped).
    virtual void setScreenPlacement(ScreenPlacement placement) = 0;
    // Diagnostic: write the finished frame as a raw file ("P3FB" + u32 width + u32 height + RGBA8
    // rows, top row first) during the next endFrame. Returns false when unsupported.
    virtual bool captureNextFrame(const std::string &path) { (void)path; return false; }

    virtual void beginFrame(const Camera &camera, ClearColor clear) = 0;
    // Change view/projection state inside the current frame without clearing
    // or presenting. The next world-space draw uses this camera.
    virtual void setCamera(const Camera &camera) = 0;
    virtual void clear(ClearColor clear, bool colorBuffer, bool depthBuffer) = 0;
    virtual void drawMesh(MeshId id, const Matrix4 &model, const MeshDrawState &state = {}) = 0;
    void drawMesh(MeshId id) { drawMesh(id, kIdentityMatrix, {}); }
    // Transient world-space lines (RenderWare Im3D/debug/effect paths). The
    // vertex vector is already expanded as pairs, so no persistent GPU mesh is
    // required and callers can submit short-lived line batches cheaply.
    virtual void drawWorldLines(const std::vector<Vertex> &vertices, const Matrix4 &model,
                                std::string_view textureName = {}, const MeshDrawState &state = {}) = 0;
    virtual void drawScreenTriangles(const std::vector<ScreenVertex> &vertices,
                                     float sourceWidth, float sourceHeight,
                                     std::string_view textureName = {},
                                     const ScreenDrawState &state = {}) = 0;
    virtual void drawScreenLines(const std::vector<ScreenVertex> &vertices,
                                 float sourceWidth, float sourceHeight,
                                 std::string_view textureName = {},
                                 const ScreenDrawState &state = {}) = 0;
    virtual void endFrame() = 0;
    // P3FES_RESOURCE_DIAG: the backend's own timing counters since the last call, as printable lines
    // (empty when it keeps none). Host thread only.
    virtual std::string drainDiagReport() { return {}; }
};

// One platform backend behind the same asset/draw contract: raylib/OpenGL on
// desktop, vitaGL on Vita. The asset path stays free of either graphics API.
std::unique_ptr<Renderer> createPlatformRenderer();
} // namespace p3fes::native_renderer
