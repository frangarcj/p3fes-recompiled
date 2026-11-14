// Persona 3 FES RenderWare integration point.
//
// Guest offsets below are cross-checked against C-GBL/P3FES-Decompilation
// (render_view.h/render_rw5.h/render_rw6.h, commit 78bb5776). In particular,
// the game's normal model path does not go through RpClumpRender: every atomic
// reaches the RpAtomicRender dispatcher (the stripped map calls it
// sub_00490860), which selects a per-atomic or global pipeline descriptor.
// The hook is opt-in and still calls the original until the native path can
// submit the corresponding geometry itself.
// RenderWare camera -> OpenGL matrix conversion is adapted from
// Rinnegatamante/librw-vita @ 5e14b3f8123f8b950917ae46748dd655c723b09f,
// src/gl/gl3device.cpp (MIT), for the same positive-Z RenderWare camera model;
// see LICENSE.librw-vita.txt beside this file.
#include <set>
#include "game_overrides.h"
#include "p3fes_native_present.h"
#include "p3fes_native_movie.h"
#include "p3fes_native_raster_lifecycle.h"
#include "p3fes_native_texture_access.h"
#include "p3fes_native_camera.h"
#include "p3fes_native_bgm_hooks.h"
#include "p3fes_native_math.h"
#include "p3fes_native_rw_math.h"
#include "p3fes_native_hanim.h"
#include "p3fes_native_frame_cuts.h"
#include "p3fes_native_ymisc.h"
#include "p3fes_debug_warp.h"
#include "p3fes_im2d_depth.h"
#include "p3fes_renderware_projection.h"
#include "p3fes_guest_math_helper.h"
#if defined(PS2X_P3FES_FRAME_TRACE)
#include "p3fes_frame_trace.h"
#endif
#include "p3fes_skin_palette_math.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_perf.h"
#if !defined(__ANDROID__)
#include "native_renderer/renderer.h"
#include "native_renderer/skinning.h"
#include "native_renderer/lighting.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(PLATFORM_VITA)
extern "C" int ps2x_vita_present_software_frame_rgba(const uint8_t *rgba, unsigned width, unsigned height);
#endif

namespace
{
    // The ELF map names this sub_00490860; its instructions dispatch a pipeline
    // selected from atomic+0x6C or the global RenderWare state.
    constexpr uint32_t kRpAtomicRender = 0x00490860u;
    constexpr uint32_t kRwFrameGetLtm = 0x004CB2F0u;
    constexpr uint32_t kSectorDefaultRenderCallBack = 0x00499B90u;
    constexpr uint32_t kAtomicFlagsOffset = 0x02u;
    constexpr uint32_t kAtomicFrameOffset = 0x04u;
    constexpr uint32_t kAtomicGeometryOffset = 0x18u;
    constexpr uint32_t kAtomicCurrentMorphOffset = 0x50u;
    constexpr uint32_t kAtomicSectorTiesOffset = 0x64u;
    constexpr uint8_t kAtomicRenderFlag = 0x04u;

    constexpr uint32_t kGeometryFlagsOffset = 0x08u;
    constexpr uint32_t kGeometryNumTrianglesOffset = 0x10u;
    constexpr uint32_t kGeometryNumVerticesOffset = 0x14u;
    constexpr uint32_t kGeometryNumMorphTargetsOffset = 0x18u;
    constexpr uint32_t kGeometryMatListArrayOffset = 0x20u;
    constexpr uint32_t kGeometryMatListCountOffset = 0x24u;
    constexpr uint32_t kGeometryTrianglesOffset = 0x2Cu;
    constexpr uint32_t kGeometryPrelitOffset = 0x30u;
    constexpr uint32_t kGeometryUv0Offset = 0x34u;
    constexpr uint32_t kGeometryUv1Offset = 0x38u;
    constexpr uint32_t kGeometryTextured2Flag = 0x80u; // rpGEOMETRYTEXTURED2: a second texture coordinate set exists
    constexpr uint32_t kGeometryMorphTargetsOffset = 0x5Cu;
    constexpr uint32_t kMorphVerticesOffset = 0x14u;
    constexpr uint32_t kMorphNormalsOffset = 0x18u;
    constexpr uint32_t kMorphSize = 0x1Cu;
    constexpr uint32_t kSkinAtomicExtensionOffset = 0x0095DE14u;
    constexpr uint32_t kSkinGeometryExtensionOffset = 0x0095DE18u;
    constexpr uint32_t kSkinMatrixPaletteUpdate = 0x0046B510u;
    constexpr uint32_t kSkinNumBonesOffset = 0x00u;
    constexpr uint32_t kSkinVertexIndicesOffset = 0x14u;
    constexpr uint32_t kSkinVertexWeightsOffset = 0x18u;
    constexpr uint32_t kMaxSkinBones = 256u;
    constexpr uint32_t kTriangleMaterialOffset = 0x06u;
    constexpr uint32_t kMaterialTextureOffset = 0x00u;
    constexpr uint32_t kMaterialColorOffset = 0x04u;
    constexpr uint32_t kMaterialAmbientOffset = 0x0Cu;
    constexpr uint32_t kMaterialSpecularOffset = 0x10u;
    constexpr uint32_t kMaterialDiffuseOffset = 0x14u;
    constexpr uint32_t kTextureRasterOffset = 0x00u;
    constexpr uint32_t kTextureFilterAddressOffset = 0x50u;
    constexpr uint32_t kSectorTrianglesOffset = 0x04u;
    constexpr uint32_t kSectorVerticesOffset = 0x08u;
    constexpr uint32_t kSectorNormalsOffset = 0x0Cu;
    constexpr uint32_t kSectorUv0Offset = 0x10u;
    constexpr uint32_t kSectorPrelitOffset = 0x30u;
    constexpr uint32_t kSectorLightTiesOffset = 0x40u;
    constexpr uint32_t kSectorMaterialBaseOffset = 0x80u;
    constexpr uint32_t kSectorNumTrianglesOffset = 0x84u;
    constexpr uint32_t kWorldFlagsOffset = 0x08u;
    constexpr uint32_t kWorldMaterialsOffset = 0x10u;
    constexpr uint32_t kWorldNumMaterialsOffset = 0x14u;
    constexpr uint32_t kWorldGlobalLightsOffset = 0x3Cu;
    constexpr uint32_t kLightTypeOffset = 0x01u;
    constexpr uint32_t kLightFlagsOffset = 0x02u;
    constexpr uint32_t kLightFrameOffset = 0x04u;
    constexpr uint32_t kLightRadiusOffset = 0x14u;
    constexpr uint32_t kLightColorOffset = 0x18u;
    constexpr uint32_t kLightMinusCosAngleOffset = 0x28u;
    constexpr uint32_t kLightWorldLinkOffset = 0x34u;
    constexpr uint8_t kLightDirectional = 1u;
    constexpr uint8_t kLightAmbient = 2u;
    constexpr uint8_t kLightPoint = 0x80u;
    constexpr uint8_t kLightSpot = 0x81u;
    constexpr uint8_t kLightSoftSpot = 0x82u;
    constexpr uint8_t kLightAffectsAtomics = 1u;
    constexpr uint8_t kLightAffectsWorld = 2u;
    constexpr uint32_t kTieObjectOffset = 0x08u;
    constexpr uint32_t kTieObjectLinkOffset = 0x0Cu;
    constexpr uint32_t kTieSectorOffset = 0x14u;
    constexpr uint32_t kGeometryPrelitFlag = 0x08u;
    constexpr uint32_t kGeometryLightingFlag = 0x20u;
    constexpr uint32_t kFrameLtmOffset = 0x50u;
    constexpr uint32_t kRwCurrentCamera = 0x00960070u;
    constexpr uint32_t kRwCurrentWorld = 0x00960074u;
    constexpr uint32_t kCameraFrameOffset = 0x04u;
    constexpr uint32_t kCameraProjectionOffset = 0x14u;
    constexpr uint32_t kCameraViewWindowXOffset = 0x68u;
    constexpr uint32_t kCameraViewWindowYOffset = 0x6Cu;
    constexpr uint32_t kCameraViewOffsetXOffset = 0x78u;
    constexpr uint32_t kCameraViewOffsetYOffset = 0x7Cu;
    constexpr uint32_t kCameraNearPlaneOffset = 0x80u;
    constexpr uint32_t kCameraFarPlaneOffset = 0x84u;
    constexpr uint32_t kCameraFogPlaneOffset = 0x88u;
    constexpr uint32_t kRwCameraClear = 0x004C9E90u;
    constexpr uint32_t kRwCameraShowRaster = 0x004C9ED0u;
    // K_SPipe_UpdateShadowNodeTask starts its pass by clearing the main camera
    // and resumes here. Some field/shadow paths never call ShowRaster. Use this
    // clear as a fallback boundary, but at most once per PS2 vblank: the task
    // can spin through many shadow clears inside one field and flushing each
    // one overwhelms Vita3K's GXM queue.
    constexpr uint32_t kShadowNodeClearReturn = 0x00199774u;
    constexpr auto kShadowFallbackPeriod = std::chrono::microseconds(16667);
    // H_SfdPlay_UpdateTask drives Sofdec outside the normal root ShowRaster
    // task.  Hook its entry as the movie-frame boundary: only the 0x10A860
    // entry slot is replaced, so internal resumptions at later PCs continue to
    // dispatch straight to the generated function instead of re-entering us.
    constexpr uint32_t kSofdecUpdateTask = 0x0010A860u;
    constexpr uint32_t kSofdecDestroyTask = 0x0010BB00u;
    constexpr uint32_t kPtankAtomicRender = 0x00474160u;
    constexpr uint32_t kRwIm3DTransform = 0x004F1ED0u;
    constexpr uint32_t kRwIm3DEnd = 0x004F1F80u;
    constexpr uint32_t kRwIm3DRenderIndexedPrimitive = 0x004F1FD0u;
    constexpr uint32_t kRwIm3DRenderPrimitive = 0x004F2150u;
    constexpr uint32_t kRwIm3DRenderLine = 0x004F22B0u;
    constexpr uint32_t kSkyIm2DRenderLine = 0x004ECA60u;
    constexpr uint32_t kSkyIm2DRenderTriangle = 0x004ECCE0u;
    constexpr uint32_t kSkyIm2DRenderPrimitive = 0x004EDBA0u;
    constexpr uint32_t kSkyIm2DRenderIndexedPrimitive = 0x004EEB50u;
    constexpr uint32_t kRwGp = 0x007D2CF0u;
    constexpr uint32_t kRwRenderFlags = kRwGp - 20272u;
    constexpr uint32_t kRwSrcBlend = kRwGp - 20264u;
    constexpr uint32_t kRwDstBlend = kRwGp - 20260u;
    constexpr uint32_t kRwMatFxPluginOffset = kRwGp - 17848u;
    constexpr uint32_t kRwRasterPluginOffset = kRwGp - 16968u;
    constexpr uint32_t kRwClamp1 = kRwGp - 17048u;
    constexpr uint32_t kRwTex1 = kRwGp - 17040u;
    constexpr uint32_t kRwAlpha1 = kRwGp - 17032u;
    constexpr uint32_t kRwFogCol = kRwGp - 17056u;
    constexpr uint32_t kRwZbuf1 = kRwGp - 17088u;
    constexpr uint32_t kRwTest1 = kRwGp - 17072u;
    constexpr uint32_t kRwCullMode = kRwGp - 16964u;
    constexpr uint32_t kRwCurrentRaster = kRwGp - 16948u;
    constexpr uint32_t kPtankAtomicExtensionOffset = kRwGp - 17792u;
    constexpr uint64_t kGsPrimTextureEnable = 0x10u;
    constexpr uint64_t kGsPrimFogEnable = 0x20u;
    constexpr uint64_t kGsPrimAlphaBlendEnable = 0x40u;
    constexpr uint64_t kGsPrimGouraud = 0x08u;
    constexpr float kRwScreenWidth = 640.0f;
    constexpr float kRwScreenHeight = 448.0f;
    constexpr uint32_t kMaxVertices = 1u << 20;
    constexpr uint32_t kMaxTriangles = 1u << 20;
    constexpr uint32_t kIm3DVertexSize = 36u;
    constexpr uint32_t kIm3DFlagUv = 0x01u;
    constexpr uint32_t kIm3DEffectiveFields = 0x18u; // Transform ORs XYZ + RGBA into the PS2 instance flags.
    constexpr uint32_t kMatFxEffectOffset = 128u;
    constexpr uint32_t kMatFxSlotSize = 64u;
    constexpr uint32_t kMatFxSlotTypeOffset = 32u;
    constexpr uint32_t kMatFxSlotDual = 4u;
    constexpr uint32_t kMatFxSlotUv = 5u;
    constexpr uint32_t kMatFxSlotEnv = 2u;
    constexpr uint32_t kMatFxEnv = 2u;
    constexpr uint32_t kMatFxBumpEnv = 3u;
    constexpr uint32_t kMatFxDual = 4u;
    constexpr uint32_t kMatFxUvTransform = 5u;
    constexpr uint32_t kMatFxDualUvTransform = 6u;

    std::atomic<PS2Runtime::RecompiledFunction> g_originalAtomicRender{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalSectorRender{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalCameraClear{nullptr};
    // Alpha-only colour mask (GS FRAME_1.FBMSK = 0x00FFFFFF). The game brackets some fills between
    // mdlEffect_003294d0 (writes FRAME_1 with that mask) and mdlEffect_00329550 (writes it back unmasked),
    // e.g. the full-screen black fill of the battle unit pass (effMisc_00358460 from btlUnit_002891e0): on the
    // GS it only sets the framebuffer alpha, the picture keeps its RGB. The RenderWare state shadow does not
    // hold FRAME_1, so draws inside the bracket are dropped here instead of painting their RGB.
    // Guest thread only.
    constexpr uint32_t kGsColorMaskOn = 0x003294D0u;
    constexpr uint32_t kGsColorMaskOff = 0x00329550u;
    bool g_gsColorMasked = false;
    // P3FES_MASK_DIAG=1: log every masked bracket (guest caller of the "on" call, what it dropped: atomic, sector, im2d, im3d)
    uint32_t g_maskDropped[4] = {0u, 0u, 0u, 0u};
    uint32_t g_maskOnRa = 0u;
    std::atomic<PS2Runtime::RecompiledFunction> g_originalColorMaskOn{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalColorMaskOff{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalShowRaster{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalSofdecUpdateTask{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalSofdecDestroyTask{nullptr};
    std::atomic<bool> g_nativeEnabled{false};
    bool g_rwSkinPaletteNativeMath = true;
    bool g_rwSkinPaletteCompare = false;
    uint32_t g_rwSkinPaletteCompareLogs = 0u;
    uint32_t g_rwSkinPaletteCompareSeen = 0u;
    uint64_t g_rwSkinPaletteNativeCount = 0u;
    uint64_t g_rwSkinPaletteFallbackCount = 0u;
    uint64_t g_rwSkinPaletteRejectedCount = 0u;
    std::atomic<bool> g_diagEnabled{false};
    std::atomic<bool> g_diagPrinted{false};
    std::atomic<bool> g_im2dDiagPrinted{false};
    std::atomic<bool> g_im2dTextureDiagPrinted{false};
    std::atomic<bool> g_sectorDiagPrinted{false};
    std::atomic<bool> g_ptankDiagPrinted{false};
    std::atomic<bool> g_im3dDiagPrinted{false};
    std::atomic<bool> g_clearDiagPrinted{false};
    std::unordered_set<uint32_t> g_loggedAtomicPipelines;

    struct NativeIm3DState
    {
        uint32_t vertices = 0u;
        uint32_t count = 0u;
        uint32_t matrix = 0u;
        uint32_t flags = 0u;
        bool open = false;
    };
    NativeIm3DState g_nativeIm3D;

    template <typename T>
    bool readGuest(const uint8_t *rdram, uint32_t address, T &value)
    {
        if (!rdram || address > PS2_RAM_SIZE - sizeof(T))
            return false;
        std::memcpy(&value, rdram + address, sizeof(T));
        return true;
    }

    bool guestPointer(uint32_t address, uint32_t size = 1u)
    {
        // Range only: address zero is readable RAM. Optional resource fields
        // must separately reject NULL before treating it as an attribute buffer.
        return size <= PS2_RAM_SIZE && address <= PS2_RAM_SIZE - size;
    }

    struct AtomicSummary
    {
        uint32_t atomic = 0u;
        uint32_t frame = 0u;
        uint32_t geometry = 0u;
        uint32_t flags = 0u;
        uint32_t geometryFlags = 0u;
        uint32_t triangles = 0u;
        uint32_t vertices = 0u;
        uint32_t morphTargets = 0u;
        uint32_t triangleData = 0u;
        uint32_t vertexData = 0u;
        uint32_t normalData = 0u;
        uint32_t prelitData = 0u;
        uint32_t uv0Data = 0u;
    };

    bool inspectAtomic(const uint8_t *rdram, uint32_t atomic, AtomicSummary &summary)
    {
        if (!guestPointer(atomic, 0x70u))
            return false;

        uint8_t flags = 0u;
        uint32_t frame = 0u, geometry = 0u;
        if (!readGuest(rdram, atomic + kAtomicFlagsOffset, flags) ||
            !readGuest(rdram, atomic + kAtomicFrameOffset, frame) ||
            !readGuest(rdram, atomic + kAtomicGeometryOffset, geometry) ||
            !guestPointer(geometry, 0x60u))
            return false;

        int32_t triangles = 0, vertices = 0, morphTargets = 0;
        uint32_t geometryFlags = 0u, triangleData = 0u, morphData = 0u;
        uint32_t prelitData = 0u, uv0Data = 0u;
        if (!readGuest(rdram, geometry + kGeometryFlagsOffset, geometryFlags) ||
            !readGuest(rdram, geometry + kGeometryNumTrianglesOffset, triangles) ||
            !readGuest(rdram, geometry + kGeometryNumVerticesOffset, vertices) ||
            !readGuest(rdram, geometry + kGeometryNumMorphTargetsOffset, morphTargets) ||
            !readGuest(rdram, geometry + kGeometryTrianglesOffset, triangleData) ||
            !readGuest(rdram, geometry + kGeometryMorphTargetsOffset, morphData) ||
            !readGuest(rdram, geometry + kGeometryPrelitOffset, prelitData) ||
            !readGuest(rdram, geometry + kGeometryUv0Offset, uv0Data) ||
            triangles < 0 || vertices < 0 || morphTargets < 0 ||
            static_cast<uint32_t>(triangles) > kMaxTriangles || static_cast<uint32_t>(vertices) > kMaxVertices)
            return false;

        uint32_t vertexData = 0u, normalData = 0u;
        if (morphTargets > 0)
        {
            if (!guestPointer(morphData, kMorphSize) ||
                !readGuest(rdram, morphData + kMorphVerticesOffset, vertexData) ||
                !readGuest(rdram, morphData + kMorphNormalsOffset, normalData))
                return false;
        }

        const uint32_t vertexBytes = static_cast<uint32_t>(vertices) * 12u;
        const uint32_t triangleBytes = static_cast<uint32_t>(triangles) * 8u;
        if ((vertices && !guestPointer(vertexData, vertexBytes)) ||
            (triangles && !guestPointer(triangleData, triangleBytes)))
            return false;

        summary.atomic = atomic;
        summary.frame = frame;
        summary.geometry = geometry;
        summary.flags = flags;
        summary.geometryFlags = geometryFlags;
        summary.triangles = static_cast<uint32_t>(triangles);
        summary.vertices = static_cast<uint32_t>(vertices);
        summary.morphTargets = static_cast<uint32_t>(morphTargets);
        summary.triangleData = triangleData;
        summary.vertexData = vertexData;
        summary.normalData = normalData;
        summary.prelitData = prelitData;
        summary.uv0Data = uv0Data;
        return true;
    }

    uint32_t selectedAtomicPipeline(const uint8_t *rdram, const R5900Context *ctx,
                                    uint32_t atomic, bool &fromAtomicSlot)
    {
        uint32_t pipeline = 0u;
        if (readGuest(rdram, atomic + 0x6Cu, pipeline) && pipeline != 0u)
        {
            fromAtomicSlot = true;
            return pipeline;
        }
        fromAtomicSlot = false;
        if (!ctx)
            return 0u;

        // Match RpAtomicRender's null-slot branch in the game ELF:
        // v1 = *(gp - 16840), then callback = *(0x960070 + v1 + 0x3C).
        const uint32_t gp = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[28]));
        uint32_t globalOffset = 0u;
        if (!readGuest(rdram, gp - 16840u, globalOffset))
            return 0u;
        const uint64_t pipelineCell = 0x00960070ull + globalOffset + 0x3Cull;
        if (pipelineCell > UINT32_MAX || !guestPointer(static_cast<uint32_t>(pipelineCell), sizeof(pipeline)))
            return 0u;
        readGuest(rdram, static_cast<uint32_t>(pipelineCell), pipeline);
        return pipeline;
    }

    void logAtomicPipeline(const uint8_t *rdram, const R5900Context *ctx, uint32_t atomic)
    {
        if (!g_diagEnabled.load(std::memory_order_relaxed) || g_loggedAtomicPipelines.size() >= 16u)
            return;

        bool fromAtomicSlot = false;
        const uint32_t pipeline = selectedAtomicPipeline(rdram, ctx, atomic, fromAtomicSlot);
        if (!g_loggedAtomicPipelines.insert(pipeline).second)
            return;

        AtomicSummary summary;
        if (!inspectAtomic(rdram, atomic, summary))
        {
            std::fprintf(stderr,
                         "[p3fes-rw] atomic pipeline router=0x%08X selected-pipeline=0x%08X source=%s atomic=0x%08X metadata=invalid\n",
                         kRpAtomicRender, pipeline, fromAtomicSlot ? "atomic+0x6C" : "global", atomic);
            return;
        }

        int16_t morph = -1;
        readGuest(rdram, atomic + kAtomicCurrentMorphOffset, morph);
        uint32_t skinPluginOffset = 0u, skin = 0u;
        int32_t skinBones = 0;
        bool hasSkin = readGuest(rdram, kSkinGeometryExtensionOffset, skinPluginOffset) && skinPluginOffset != 0u;
        const uint64_t resolvedSkinField = static_cast<uint64_t>(summary.geometry) + skinPluginOffset;
        hasSkin = hasSkin && resolvedSkinField <= UINT32_MAX &&
                  guestPointer(static_cast<uint32_t>(resolvedSkinField), sizeof(uint32_t)) &&
                  readGuest(rdram, static_cast<uint32_t>(resolvedSkinField), skin) && skin != 0u &&
                  guestPointer(skin, sizeof(uint32_t)) && readGuest(rdram, skin, skinBones);
        std::fprintf(stderr,
                     "[p3fes-rw] atomic pipeline router=0x%08X selected-pipeline=0x%08X source=%s atomic=0x%08X geom=0x%08X flags=0x%02X gflags=0x%08X morph=%d/%u frame=0x%08X skin=%s0x%08X bones=%d\n",
                     kRpAtomicRender, pipeline, fromAtomicSlot ? "atomic+0x6C" : "global", atomic,
                     summary.geometry, summary.flags, summary.geometryFlags, static_cast<int>(morph),
                     summary.morphTargets, summary.frame, hasSkin ? "" : "none/", hasSkin ? skin : 0u,
                     hasSkin ? skinBones : 0);
    }

#if !defined(__ANDROID__)
    using p3fes::native_renderer::Camera;
    using p3fes::native_renderer::ClearColor;
    using p3fes::native_renderer::FogState;
    using p3fes::native_renderer::Matrix4;
    using p3fes::native_renderer::MeshData;
    using p3fes::native_renderer::MeshDrawState;
    using p3fes::native_renderer::MeshId;
    using p3fes::native_renderer::ScreenVertex;
    using p3fes::native_renderer::ScreenBlend;
    using p3fes::native_renderer::ScreenDrawState;
    using p3fes::native_renderer::Submesh;
    using p3fes::native_renderer::TextureData;
    using p3fes::native_renderer::Vertex;

    // What the producer remembers about a mesh it already defined to the present layer.
    struct DefinedMeshInfo
    {
        bool skinned = false;
        uint32_t boneCount = 0u;
        uint32_t usedBones[8]{}; // bit b: some vertex weights bone b
    };
    std::unordered_map<MeshId, DefinedMeshInfo> g_definedMeshes;
    struct DefinedMeshSlot
    {
        MeshId id = 0u;
        const DefinedMeshInfo *info = nullptr;
    };
    DefinedMeshSlot g_definedMeshCache[256];

    DefinedMeshInfo describeMesh(const MeshData &mesh)
    {
        DefinedMeshInfo info;
        if (!mesh.skinned || mesh.boneCount == 0u || mesh.boneCount > 256u)
            return info;
        info.skinned = true;
        info.boneCount = mesh.boneCount;
        // Same test as the renderers' skip condition (weight not > 0, or not finite, or out of range).
        for (const Vertex &vertex : mesh.vertices)
            for (unsigned j = 0u; j < 4u; ++j)
            {
                const float weight = vertex.weights[j];
                const uint32_t bone = vertex.bones[j];
                if (weight > 0.0f && std::isfinite(weight) && bone < mesh.boneCount)
                    info.usedBones[bone >> 5u] |= 1u << (bone & 31u);
            }
        return info;
    }
    struct SkinDiagnosticSample
    {
        std::array<Vertex, 3> vertices;
        uint32_t boneCount = 0u;
        uint32_t materialCount = 0u;
        uint32_t submeshCount = 0u;
        uint32_t firstSubmeshIndexCount = 0u;
        std::array<uint8_t, 4> firstMaterialColor{255u, 255u, 255u, 255u};
        bool hasPrelight = false;
        bool emitted = false;
    };
    std::unordered_map<MeshId, SkinDiagnosticSample> g_skinDiagnosticSamples;
    struct SkinFrameDiagnostic
    {
        uint64_t firstFrame = 0u;
        Matrix4 rawAtFirst{};
        Matrix4 syncedAtFirst{};
        bool dirtyAtFirst = false;
        bool firstLogged = false;
        bool followupLogged = false;
    };
    std::unordered_map<uint32_t, SkinFrameDiagnostic> g_skinFrameDiagnostics;
    struct NativeRasterResource
    {
        p3fes_native_rasters::Version version;
        std::string textureName;
    };
    std::unordered_map<uint32_t, NativeRasterResource> g_nativeRasterResources;
    // Lowest texel alpha of each decoded native texture, by texture name (the same name the draw states
    // carry in materialTextures). Filled where the texture is decoded, dropped when it is retired.
    // readNativeGlobalMeshState uses it to tell cut-out textures (alpha 0 texels) from solid ones.
    std::unordered_map<std::string, uint8_t> g_textureMinAlpha;
    // Direct-mapped front for prepareNativeRasterTexture's steady state (one lookup per material per
    // draw otherwise): raster -> its resource's texture name, valid while the raster lifecycle epoch is
    // unchanged (every version change or retire bumps it). Points at a g_nativeRasterResources node.
    struct RasterNameCacheSlot
    {
        uint32_t raster = 0u;
        uint64_t epoch = 0u;
        const std::string *name = nullptr;
    };
    RasterNameCacheSlot g_rasterNameCache[128];
    uint64_t g_texturePrepares = 0u;
    bool g_nativeFrameActive = false;
    uint64_t g_nativeFrameNumber = 0u;
    uint32_t g_nativeFrameCameraAddress = 0u;
    Camera g_nativeFrameCameraState{};
    std::unordered_set<uint32_t> g_loggedAtomicCameras;
    std::unordered_set<uint64_t> g_loggedClearCameras;
    uint64_t g_shadowBoundaryVsync = ~uint64_t{0};
    std::chrono::steady_clock::time_point g_shadowBoundaryTime{};
    uint32_t g_nativeErrors = 0u;
#if defined(PLATFORM_VITA)
    uint32_t g_nativeMovieTask = 0u;
    std::string g_nativeMoviePath;
    std::unordered_set<std::string> g_failedNativeMovies;
#endif

    bool decodeNativeRaster(const uint8_t *rdram, uint32_t raster, TextureData &texture);
    bool prepareNativeRasterTexture(const uint8_t *rdram, uint32_t raster, std::string &textureName);
    bool readMaterialUvTransform(const uint8_t *rdram, uint32_t material,
                                 MeshDrawState::UvTransform &out);
    bool readMaterialDualPass(const uint8_t *rdram, uint32_t material, MeshDrawState::DualPass &out);
    bool readMaterialEnvPass(const uint8_t *rdram, uint32_t material, MeshDrawState::EnvPass &out);
    bool readTextureSampler(const uint8_t *rdram, uint32_t texture, MeshDrawState::SamplerState &out);
    bool readMaterialMatFx(const uint8_t *rdram, uint32_t material, uint32_t &extension, uint32_t &effect);

    void readNativeFogState(const uint8_t *rdram, uint64_t renderFlags, FogState &fog)
    {
        if ((renderFlags & kGsPrimFogEnable) == 0u)
            return;
        uint32_t camera = 0u;
        uint64_t fogCol = 0u;
        float start = 0.0f, end = 0.0f;
        if (!readGuest(rdram, kRwCurrentCamera, camera) || !guestPointer(camera, kCameraFogPlaneOffset + sizeof(float)) ||
            !readGuest(rdram, camera + kCameraFogPlaneOffset, start) ||
            !readGuest(rdram, camera + kCameraFarPlaneOffset, end) ||
            !readGuest(rdram, kRwFogCol, fogCol) || !std::isfinite(start) || !std::isfinite(end) ||
            !(end > start))
            return;
        fog.enabled = true;
        fog.start = start;
        fog.end = end;
        fog.color = {static_cast<uint8_t>(fogCol & 0xFFu),
                     static_cast<uint8_t>((fogCol >> 8u) & 0xFFu),
                     static_cast<uint8_t>((fogCol >> 16u) & 0xFFu)};
    }

    void readNativeMeshDrawState(const uint8_t *rdram, uint32_t geometry, MeshDrawState &state)
    {
        uint32_t materialArray = 0u;
        int32_t materialCount = 0;
        if (!readGuest(rdram, geometry + kGeometryMatListArrayOffset, materialArray) ||
            !readGuest(rdram, geometry + kGeometryMatListCountOffset, materialCount) ||
            materialCount <= 0 || materialCount > 4096 ||
            !guestPointer(materialArray, static_cast<uint32_t>(materialCount) * sizeof(uint32_t)))
            return;
        const size_t count = static_cast<size_t>(materialCount);
        state.materialColors.assign(count, {255u, 255u, 255u, 255u});
        state.materialSamplers.resize(count);
        state.materialTextures.resize(count);
        // MatFX vectors stay empty until a material actually has that effect: the renderers treat a
        // missing entry exactly like a default (disabled) one, and most materials have none.
        for (int32_t i = 0; i < materialCount; ++i)
        {
            uint32_t material = 0u;
            if (!readGuest(rdram, materialArray + static_cast<uint32_t>(i) * 4u, material) ||
                !material || !guestPointer(material, 8u))
                continue;
            std::memcpy(state.materialColors[static_cast<size_t>(i)].data(),
                        rdram + material + kMaterialColorOffset, 4u);
            uint32_t texture = 0u;
            if (readGuest(rdram, material + kMaterialTextureOffset, texture) && texture)
            {
                readTextureSampler(rdram, texture, state.materialSamplers[static_cast<size_t>(i)]);
                uint32_t raster = 0u;
                if (readGuest(rdram, texture + kTextureRasterOffset, raster))
                    prepareNativeRasterTexture(rdram, raster, state.materialTextures[static_cast<size_t>(i)]);
            }
            uint32_t extension = 0u, effect = 0u;
            if (!readMaterialMatFx(rdram, material, extension, effect))
                continue;
            if (effect == kMatFxUvTransform || effect == kMatFxDualUvTransform)
            {
                MeshDrawState::UvTransform uv;
                if (readMaterialUvTransform(rdram, material, uv))
                {
                    state.materialUvTransforms.resize(count);
                    state.materialUvTransforms[static_cast<size_t>(i)] = uv;
                }
            }
            if (effect == kMatFxDual || effect == kMatFxDualUvTransform)
            {
                MeshDrawState::DualPass pass;
                if (readMaterialDualPass(rdram, material, pass))
                {
                    state.materialDualPasses.resize(count);
                    state.materialDualPasses[static_cast<size_t>(i)] = std::move(pass);
                }
            }
            if (effect == kMatFxEnv || effect == kMatFxBumpEnv)
            {
                MeshDrawState::EnvPass pass;
                if (readMaterialEnvPass(rdram, material, pass))
                {
                    state.materialEnvPasses.resize(count);
                    state.materialEnvPasses[static_cast<size_t>(i)] = std::move(pass);
                }
            }
        }
    }

    // True when the draw has at least one decoded texture and none of them contains an alpha-0 texel.
    bool drawTexturesAreSolid(const MeshDrawState &state)
    {
        bool any = false;
        for (const std::string &name : state.materialTextures)
        {
            if (name.empty())
                continue;
            const auto found = g_textureMinAlpha.find(name);
            if (found == g_textureMinAlpha.end() || found->second == 0u)
                return false;
            any = true;
        }
        return any;
    }

    void readNativeGlobalMeshState(const uint8_t *rdram, MeshDrawState &state)
    {
        uint64_t zbuf = 0u, test = 0u, clamp = 0u, tex1 = 0u;
        uint64_t renderFlags = 0u;
        uint32_t cull = 2u, srcBlend = 5u, dstBlend = 6u;
        if (readGuest(rdram, kRwZbuf1, zbuf))
            state.depthWrite = ((zbuf >> 32u) & 1u) == 0u; // GS ZBUF.ZMSK: 1 disables writes.
        if (readGuest(rdram, kRwTest1, test))
        {
            state.depthTest = ((test >> 17u) & 0x3u) != 1u; // RW's disabled mode uses ZTST=ALWAYS.
            state.alphaTest.enabled = (test & 0x1u) != 0u;
            state.alphaTest.function = static_cast<uint8_t>((test >> 1u) & 0x7u);
            state.alphaTest.reference = static_cast<uint8_t>((test >> 4u) & 0xFFu);
            // AFAIL = FB_ONLY (the game's normal mesh state): on the GS a pixel that fails the alpha test is
            // still drawn (blended), it only does not write Z. What the backends do about it depends
            // on the textures of the draw:
            //  * ATST = NEVER (the additive light glows) fails every pixel: all are drawn, none writes Z;
            //  * solid textures (no alpha-0 texel: the dining table's cloth, alpha 98..255): one pass,
            //    "alpha > 0". Discarding the pixels under the reference punched holes (the legs showed
            //    through), and nearly every pixel of such a texture would pass anyway;
            //  * everything else (cut-out textures: plants, lamps, foliage; soft baked shadows;
            //    untextured vertex-alpha blobs): the register's test, drawn in two passes (lighting.h,
            //    kAlphaTestFbOnlyFlag): the passing pixels with Z, then the failing ones blended with
            //    Z masked. One pass with "alpha > 0" let the bilinear fringe of a cut-out write Z and
            //    hide what is drawn behind it (black outlines on the plant); one pass with the plain
            //    test turned soft shadow decals into hard blobs.
            if (((test >> 12u) & 0x3u) == 1u && state.alphaTest.enabled)
            {
                if (state.alphaTest.function == 0u)
                {
                    state.alphaTest.enabled = false;
                    state.depthWrite = false;
                }
                else if (drawTexturesAreSolid(state))
                {
                    state.alphaTest.function = 6u; // GREATER
                    state.alphaTest.reference = 0u;
                }
                else
                    state.alphaTest.function |= p3fes::native_renderer::kAlphaTestFbOnlyFlag;
            }
        }
        {
            // P3FES_MESH_STATE_DIAG=1: each distinct raw GS state tuple seen by mesh draws, once.
            static const bool meshStateDiag = [] { const char *v = std::getenv("P3FES_MESH_STATE_DIAG"); return v && *v == '1'; }();
            if (meshStateDiag)
            {
                static std::set<std::array<uint64_t, 4>> seenTuples;
                uint64_t flags = 0u, alpha = 0u;
                readGuest(rdram, kRwRenderFlags, flags);
                readGuest(rdram, kRwAlpha1, alpha);
                if (seenTuples.insert({test, flags, alpha, zbuf}).second)
                    std::fprintf(stderr, "[p3fes-mesh-state] TEST_1=%llX flags=%llX ALPHA_1=%llX ZBUF_1=%llX\n",
                                 static_cast<unsigned long long>(test), static_cast<unsigned long long>(flags),
                                 static_cast<unsigned long long>(alpha), static_cast<unsigned long long>(zbuf));
            }
        }
        if (readGuest(rdram, kRwCullMode, cull))
        {
            switch (cull)
            {
            case 1u: state.cullMode = MeshDrawState::CullMode::None; break;
            case 3u: state.cullMode = MeshDrawState::CullMode::Front; break;
            case 2u:
            default: state.cullMode = MeshDrawState::CullMode::Back; break;
            }
        }
        if (readGuest(rdram, kRwClamp1, clamp))
        {
            const uint32_t wms = static_cast<uint32_t>(clamp & 0x3u);
            const uint32_t wmt = static_cast<uint32_t>((clamp >> 2u) & 0x3u);
            state.clampU = wms == 1u || wms == 2u;
            state.clampV = wmt == 1u || wmt == 2u;
        }
        if (readGuest(rdram, kRwTex1, tex1))
            state.linearFilter = (tex1 & (1ull << 5u)) != 0u; // GS TEX1.MMAG
        if (readGuest(rdram, kRwRenderFlags, renderFlags))
        {
            state.blendEnabled = (renderFlags & kGsPrimAlphaBlendEnable) != 0u;
            state.gouraudShading = (renderFlags & kGsPrimGouraud) != 0u;
            readNativeFogState(rdram, renderFlags, state.fog);
        }
        // Experiment (P3FES_EXPERIMENT_OPAQUE=1): Z-writing meshes drawn opaque, to test hypotheses about
        // which draws the console blends.
        static const bool experimentOpaque = [] { const char *v = std::getenv("P3FES_EXPERIMENT_OPAQUE"); return v && *v == '1'; }();
        if (experimentOpaque && state.depthWrite)
        {
            state.blendEnabled = false;
            state.alphaTest.enabled = false;
        }
        if (state.blendEnabled && readGuest(rdram, kRwSrcBlend, srcBlend) && readGuest(rdram, kRwDstBlend, dstBlend) &&
            srcBlend >= 1u && srcBlend <= 11u && dstBlend >= 1u && dstBlend <= 11u)
        {
            // What the GS runs is ALPHA_1, and in this game the RenderWare pair read above does not
            // describe it: it reads ONE/SRCCOLOR for every draw of the lounge while ALPHA_1 is 0x44.
            // Drawn as ONE/SRCCOLOR (Cs + Cd * Cs) every surface lets what is behind it show
            // through, most visibly the dining table's cloth. Use the register for the two
            // equations a factor pair can express.
            uint64_t gsAlpha = 0u;
            if (readGuest(rdram, kRwAlpha1, gsAlpha))
            {
                // Anything else falls back to plain alpha blending rather than to the bogus pair.
                srcBlend = 5u;
                dstBlend = (gsAlpha & 0xFFu) == 0x48u ? 2u : 6u; // 0x48: Cs * As + Cd, 0x44: (Cs - Cd) * As + Cd
            }
            const std::size_t materialCount = std::max<std::size_t>(state.materialColors.size(), 1u);
            MeshDrawState::BlendState blend;
            blend.enabled = true;
            blend.src = static_cast<uint8_t>(srcBlend);
            blend.dst = static_cast<uint8_t>(dstBlend);
            state.materialBlends.assign(materialCount, blend);
        }
    }

    bool readVec3(const uint8_t *rdram, uint32_t address, std::array<float, 3> &out)
    {
        return readGuest(rdram, address + 0u, out[0]) &&
               readGuest(rdram, address + 4u, out[1]) &&
               readGuest(rdram, address + 8u, out[2]);
    }

    bool readRwMatrix(const uint8_t *rdram, uint32_t m, Matrix4 &out)
    {
        if (!guestPointer(m, 64u))
            return false;
        std::array<float, 3> right{}, up{}, at{}, pos{};
        if (!readVec3(rdram, m + 0u, right) || !readVec3(rdram, m + 16u, up) ||
            !readVec3(rdram, m + 32u, at) || !readVec3(rdram, m + 48u, pos))
            return false;
        out = {right[0], right[1], right[2], 0.0f,
               up[0], up[1], up[2], 0.0f,
               at[0], at[1], at[2], 0.0f,
               pos[0], pos[1], pos[2], 1.0f};
        return true;
    }

    bool readFrameMatrix(const uint8_t *rdram, uint32_t frame, Matrix4 &out)
    {
        if (!guestPointer(frame, kFrameLtmOffset + 64u))
            return false;
        return readRwMatrix(rdram, frame + kFrameLtmOffset, out);
    }

    bool readFrameMatrixAfterSync(uint8_t *rdram, uint32_t frame, R5900Context *ctx,
                                  PS2Runtime *runtime, Matrix4 &before, Matrix4 &after,
                                  bool &syncRequested)
    {
        if (!readFrameMatrix(rdram, frame, before))
            return false;
        syncRequested = false;

        uint32_t updateNode = 0u;
        if (readGuest(rdram, frame + 0xA0u, updateNode) && guestPointer(updateNode, 4u))
            syncRequested = (rdram[updateNode + 3u] & 1u) != 0u;
        if (!syncRequested || !ctx || !runtime)
        {
            after = before;
            return true;
        }

        PS2Runtime::RecompiledFunction getLtm = runtime->lookupFunction(kRwFrameGetLtm);
        if (!getLtm)
        {
            after = before;
            return false;
        }

        // The game helper may checkpoint inside _rwFrameSyncDirty. Resume its
        // saved guest PC to the private return sentinel before using the LTM.
        const auto sync = p3fes_guest_math_helper::run(rdram, *ctx, *runtime,
                                                       kRwFrameGetLtm, frame);
        if (!sync.completed)
            return false;
        const uint32_t matrix = sync.value;
        if (matrix == 0u || !guestPointer(matrix, 64u) || !readRwMatrix(rdram, matrix, after))
        {
            after = before;
            return false;
        }
        return true;
    }

    // Bone LTM for the skin palette: the frame's matrix after _rwFrameSyncDirty, as 16 floats. The
    // padding lanes are taken as stored (the palette math never reads them). Same result as
    // readFrameMatrixAfterSync's `after`, without building two Matrix4 per bone when no sync is pending.
    // True when the frame's update node asks for a sync (what readBoneLtmAfterSync tests first).
    inline bool frameSyncRequested(const uint8_t *rdram, uint32_t frame)
    {
        uint32_t updateNode = 0u;
        return readGuest(rdram, frame + 0xA0u, updateNode) && guestPointer(updateNode, 4u) &&
               (rdram[updateNode + 3u] & 1u) != 0u;
    }

    // `out` may be null: the frame is synced (the guest-visible effect) but its matrix is not wanted.
    bool readBoneLtmAfterSync(uint8_t *rdram, uint32_t frame, R5900Context *ctx, PS2Runtime *runtime,
                              float *out)
    {
        if (!guestPointer(frame, kFrameLtmOffset + 64u))
            return false;
        uint32_t updateNode = 0u;
        const bool syncRequested = readGuest(rdram, frame + 0xA0u, updateNode) &&
                                   guestPointer(updateNode, 4u) && (rdram[updateNode + 3u] & 1u) != 0u;
        if (!syncRequested || !ctx || !runtime)
        {
            if (out)
                std::memcpy(out, rdram + frame + kFrameLtmOffset, 64u);
            return true;
        }
        Matrix4 before{}, after{};
        bool requested = false;
        if (!readFrameMatrixAfterSync(rdram, frame, ctx, runtime, before, after, requested))
            return false;
        if (out)
            std::memcpy(out, after.data(), 64u);
        return true;
    }

    void readMaterialSurface(const uint8_t *rdram, uint32_t material,
                             p3fes::native_renderer::Material &out)
    {
        float ambient = 1.0f, specular = 1.0f, diffuse = 1.0f;
        readGuest(rdram, material + kMaterialAmbientOffset, ambient);
        readGuest(rdram, material + kMaterialSpecularOffset, specular);
        readGuest(rdram, material + kMaterialDiffuseOffset, diffuse);
        if (std::isfinite(ambient))
            out.ambient = ambient;
        if (std::isfinite(specular))
            out.specular = specular;
        if (std::isfinite(diffuse))
            out.diffuse = diffuse;
    }

    bool readTextureSampler(const uint8_t *rdram, uint32_t texture, MeshDrawState::SamplerState &out)
    {
        uint32_t packed = 0u;
        if (!guestPointer(texture, kTextureFilterAddressOffset + sizeof(uint32_t)) ||
            !readGuest(rdram, texture + kTextureFilterAddressOffset, packed))
            return false;

        const uint32_t filter = packed & 0xFFu;
        const uint32_t addressU = (packed >> 8u) & 0xFu;
        const uint32_t addressV = (packed >> 12u) & 0xFu;
        if (filter < 1u || filter > 6u)
            return false;

        const auto address = [](uint32_t value)
        {
            switch (value)
            {
            case 2u: return MeshDrawState::SamplerState::Address::Mirror;
            case 3u:
            case 4u: return MeshDrawState::SamplerState::Address::Clamp;
            case 1u:
            default: return MeshDrawState::SamplerState::Address::Wrap;
            }
        };
        out.valid = true;
        out.u = address(addressU);
        out.v = address(addressV);
        // P3FES' sky driver sets TEX1.MMAG for RW modes 2/4/6 and clears it
        // for 1/3/5. Host native textures currently have one decoded level,
        // so preserve the magnification choice and ignore the mip component.
        out.linearFilter = (filter & 1u) == 0u;
        return true;
    }

    bool readMaterialMatFx(const uint8_t *rdram, uint32_t material, uint32_t &extension, uint32_t &effect)
    {
        uint32_t pluginOffset = 0u;
        if (!readGuest(rdram, kRwMatFxPluginOffset, pluginOffset) || pluginOffset == 0u ||
            !guestPointer(material + pluginOffset, sizeof(uint32_t)))
            return false;

        // RenderWare's material plugin slot stores a pointer to the 132-byte
        // MatFX record (two 64-byte slots + effect kind).
        if (!readGuest(rdram, material + pluginOffset, extension) || !extension ||
            !guestPointer(extension, kMatFxEffectOffset + sizeof(uint32_t)) ||
            !readGuest(rdram, extension + kMatFxEffectOffset, effect))
            return false;
        return true;
    }

    bool readUvMatrix(const uint8_t *rdram, uint32_t matrix, MeshDrawState::UvTransform &out)
    {
        if (!matrix || !guestPointer(matrix, 56u))
            return false; // null means the identity matrix in the stock bridge.

        float rightX = 1.0f, rightY = 0.0f, upX = 0.0f, upY = 1.0f, posX = 0.0f, posY = 0.0f;
        if (!readGuest(rdram, matrix + 0u, rightX) || !readGuest(rdram, matrix + 4u, rightY) ||
            !readGuest(rdram, matrix + 16u, upX) || !readGuest(rdram, matrix + 20u, upY) ||
            !readGuest(rdram, matrix + 48u, posX) || !readGuest(rdram, matrix + 52u, posY) ||
            !std::isfinite(rightX) || !std::isfinite(rightY) || !std::isfinite(upX) ||
            !std::isfinite(upY) || !std::isfinite(posX) || !std::isfinite(posY))
            return false;

        out.enabled = true;
        out.rightX = rightX;
        out.rightY = rightY;
        out.upX = upX;
        out.upY = upY;
        out.posX = posX;
        out.posY = posY;
        return true;
    }

    uint32_t findMatFxSlot(const uint8_t *rdram, uint32_t extension, uint32_t slotType)
    {
        for (uint32_t i = 0u; i < 2u; ++i)
        {
            const uint32_t slot = extension + i * kMatFxSlotSize;
            uint32_t type = 0u;
            if (guestPointer(slot, kMatFxSlotTypeOffset + sizeof(uint32_t)) &&
                readGuest(rdram, slot + kMatFxSlotTypeOffset, type) && type == slotType)
                return slot;
        }
        return 0u;
    }

    bool readMaterialUvTransform(const uint8_t *rdram, uint32_t material,
                                 MeshDrawState::UvTransform &out)
    {
        uint32_t extension = 0u, effect = 0u;
        if (!readMaterialMatFx(rdram, material, extension, effect) ||
            (effect != kMatFxUvTransform && effect != kMatFxDualUvTransform))
            return false;
        const uint32_t uvSlot = findMatFxSlot(rdram, extension, kMatFxSlotUv);
        if (!uvSlot)
            return false;
        uint32_t matrix = 0u;
        return readGuest(rdram, uvSlot, matrix) && readUvMatrix(rdram, matrix, out);
    }

    bool readMaterialDualPass(const uint8_t *rdram, uint32_t material, MeshDrawState::DualPass &out)
    {
        uint32_t extension = 0u, effect = 0u;
        if (!readMaterialMatFx(rdram, material, extension, effect) ||
            (effect != kMatFxDual && effect != kMatFxDualUvTransform))
            return false;

        const uint32_t dualSlot = findMatFxSlot(rdram, extension, kMatFxSlotDual);
        if (!dualSlot)
            return false;
        uint32_t texture = 0u, raster = 0u, srcBlend = 0u, dstBlend = 0u;
        if (!guestPointer(dualSlot, 12u) || !readGuest(rdram, dualSlot + 0u, texture) || !texture ||
            !guestPointer(texture, 4u) || !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster ||
            !readGuest(rdram, dualSlot + 4u, srcBlend) || !readGuest(rdram, dualSlot + 8u, dstBlend) ||
            srcBlend < 1u || srcBlend > 11u || dstBlend < 1u || dstBlend > 11u)
            return false;

        std::string textureName;
        if (!prepareNativeRasterTexture(rdram, raster, textureName))
            return false;

        out.enabled = true;
        out.textureName = std::move(textureName);
        readTextureSampler(rdram, texture, out.sampler);
        out.srcBlend = static_cast<uint8_t>(srcBlend);
        out.dstBlend = static_cast<uint8_t>(dstBlend);
        if (effect == kMatFxDualUvTransform)
        {
            const uint32_t uvSlot = findMatFxSlot(rdram, extension, kMatFxSlotUv);
            uint32_t matrix = 0u;
            if (uvSlot && readGuest(rdram, uvSlot + 4u, matrix))
                readUvMatrix(rdram, matrix, out.uvTransform);
        }
        return true;
    }

    bool readMaterialEnvPass(const uint8_t *rdram, uint32_t material, MeshDrawState::EnvPass &out)
    {
        uint32_t extension = 0u, effect = 0u;
        if (!readMaterialMatFx(rdram, material, extension, effect) ||
            (effect != kMatFxEnv && effect != kMatFxBumpEnv))
            return false;
        const uint32_t envSlot = findMatFxSlot(rdram, extension, kMatFxSlotEnv);
        if (!envSlot)
            return false;

        uint32_t frame = 0u, texture = 0u, raster = 0u, fbAlpha = 0u;
        float coefficient = 0.0f;
        if (!readGuest(rdram, envSlot + 0u, frame) ||
            !readGuest(rdram, envSlot + 4u, texture) || !texture || !guestPointer(texture, 4u) ||
            !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster ||
            !readGuest(rdram, envSlot + 8u, coefficient) || !std::isfinite(coefficient) || coefficient == 0.0f ||
            !readGuest(rdram, envSlot + 12u, fbAlpha))
            return false;

        std::string textureName;
        if (!prepareNativeRasterTexture(rdram, raster, textureName))
            return false;

        if (!frame)
        {
            uint32_t camera = 0u;
            if (!readGuest(rdram, kRwCurrentCamera, camera) || !guestPointer(camera, kCameraFrameOffset + 4u) ||
                !readGuest(rdram, camera + kCameraFrameOffset, frame))
                return false;
        }
        Matrix4 ltm{};
        if (!readFrameMatrix(rdram, frame, ltm))
            return false;

        out.enabled = true;
        out.textureName = std::move(textureName);
        readTextureSampler(rdram, texture, out.sampler);
        out.coefficient = coefficient;
        out.frameBufferAlpha = fbAlpha != 0u;
        out.frameRight = {ltm[0], ltm[1], ltm[2]};
        out.frameUp = {ltm[4], ltm[5], ltm[6]};
        return true;
    }

    bool readNativeLighting(const uint8_t *rdram, bool worldGeometry, uint32_t object, MeshDrawState &state)
    {
        uint32_t world = 0u;
        if (!readGuest(rdram, kRwCurrentWorld, world) || !guestPointer(world, kWorldGlobalLightsOffset + 8u))
            return false;

        const uint32_t sentinel = world + kWorldGlobalLightsOffset;
        uint32_t link = 0u;
        if (!readGuest(rdram, sentinel, link))
            return false;

        state.lightingEnabled = true;
        state.ambientLight = {0.0f, 0.0f, 0.0f};
        state.directionalLights.clear();
        state.localLights.clear();
        const uint8_t targetFlag = worldGeometry ? kLightAffectsWorld : kLightAffectsAtomics;
        for (uint32_t guard = 0u; link && link != sentinel && guard < 64u; ++guard)
        {
            if (link < kLightWorldLinkOffset || !guestPointer(link, 8u))
                return false;
            const uint32_t light = link - kLightWorldLinkOffset;
            if (!guestPointer(light, kLightWorldLinkOffset + 8u))
                return false;

            uint8_t type = 0u, flags = 0u;
            if (!readGuest(rdram, light + kLightTypeOffset, type) ||
                !readGuest(rdram, light + kLightFlagsOffset, flags))
                return false;
            if ((flags & targetFlag) != 0u)
            {
                std::array<float, 4> color{};
                std::memcpy(color.data(), rdram + light + kLightColorOffset, sizeof(color));
                const bool finiteColor = std::all_of(color.begin(), color.begin() + 3,
                                                     [](float v) { return std::isfinite(v); });
                if (finiteColor && type == kLightAmbient)
                {
                    for (int c = 0; c < 3; ++c)
                        state.ambientLight[c] += color[c];
                }
                else if (finiteColor && type == kLightDirectional && state.directionalLights.size() < 8u)
                {
                    uint32_t frame = 0u;
                    Matrix4 ltm{};
                    if (readGuest(rdram, light + kLightFrameOffset, frame) && readFrameMatrix(rdram, frame, ltm))
                    {
                        MeshDrawState::DirectionalLight nativeLight;
                        nativeLight.direction = {ltm[8], ltm[9], ltm[10]};
                        nativeLight.color = {color[0], color[1], color[2]};
                        state.directionalLights.push_back(nativeLight);
                    }
                }
            }
            if (!readGuest(rdram, link, link))
                return false;
        }

        // A light reachable through several sectors is added once. Only appended lights need
        // remembering: a light that was looked at and rejected is rejected again identically.
        uint32_t appendedLocalLights[8];
        const auto appendLocalLight = [&](uint32_t light) {
            const size_t appended = state.localLights.size();
            if (!light || appended >= 8u || !guestPointer(light, kLightWorldLinkOffset + 8u) ||
                std::find(appendedLocalLights, appendedLocalLights + appended, light) != appendedLocalLights + appended)
                return;

            uint8_t type = 0u, flags = 0u;
            if (!readGuest(rdram, light + kLightTypeOffset, type) ||
                !readGuest(rdram, light + kLightFlagsOffset, flags) || (flags & targetFlag) == 0u ||
                (type != kLightPoint && type != kLightSpot && type != kLightSoftSpot))
                return;

            float radius = 0.0f, minusCos = -1.0f;
            std::array<float, 4> color{};
            uint32_t frame = 0u;
            Matrix4 ltm{};
            if (!readGuest(rdram, light + kLightRadiusOffset, radius) ||
                !readGuest(rdram, light + kLightMinusCosAngleOffset, minusCos) ||
                !readGuest(rdram, light + kLightFrameOffset, frame) || !readFrameMatrix(rdram, frame, ltm))
                return;
            std::memcpy(color.data(), rdram + light + kLightColorOffset, sizeof(color));
            if (!(radius > 0.0f) || !std::isfinite(radius) || !std::isfinite(minusCos) ||
                !std::all_of(color.begin(), color.begin() + 3, [](float v) { return std::isfinite(v); }))
                return;

            MeshDrawState::LocalLight nativeLight;
            nativeLight.type = type == kLightPoint ? MeshDrawState::LocalLight::Type::Point
                : (type == kLightSpot ? MeshDrawState::LocalLight::Type::Spot
                                      : MeshDrawState::LocalLight::Type::SoftSpot);
            nativeLight.position = {ltm[12], ltm[13], ltm[14]};
            nativeLight.direction = {ltm[8], ltm[9], ltm[10]};
            nativeLight.color = {color[0], color[1], color[2]};
            nativeLight.radius = radius;
            nativeLight.minusCosAngle = minusCos;
            appendedLocalLights[appended] = light;
            state.localLights.push_back(nativeLight);
        };

        const auto walkSectorLights = [&](uint32_t sector) {
            if (!guestPointer(sector, kSectorLightTiesOffset + 8u))
                return;
            const uint32_t sectorSentinel = sector + kSectorLightTiesOffset;
            uint32_t sectorLink = 0u;
            if (!readGuest(rdram, sectorSentinel, sectorLink))
                return;
            for (uint32_t guard = 0u; sectorLink && sectorLink != sectorSentinel && guard < 64u; ++guard)
            {
                // Sector lists use the tie's sector link at +0; the light is
                // the common tie object pointer at +8.
                uint32_t light = 0u;
                if (!guestPointer(sectorLink, kTieObjectOffset + 4u) ||
                    !readGuest(rdram, sectorLink + kTieObjectOffset, light))
                    return;
                appendLocalLight(light);
                if (!readGuest(rdram, sectorLink, sectorLink))
                    return;
            }
        };

        if (worldGeometry)
        {
            walkSectorLights(object);
        }
        else if (guestPointer(object, kAtomicSectorTiesOffset + 8u))
        {
            const uint32_t atomicSentinel = object + kAtomicSectorTiesOffset;
            uint32_t atomicLink = 0u;
            if (readGuest(rdram, atomicSentinel, atomicLink))
            {
                for (uint32_t guard = 0u; atomicLink && atomicLink != atomicSentinel && guard < 64u; ++guard)
                {
                    // The atomic's list points at tie +12 (object link).
                    if (atomicLink < kTieObjectLinkOffset)
                        break;
                    const uint32_t tie = atomicLink - kTieObjectLinkOffset;
                    uint32_t sector = 0u;
                    if (guestPointer(tie, kTieSectorOffset + 4u) &&
                        readGuest(rdram, tie + kTieSectorOffset, sector))
                        walkSectorLights(sector);
                    if (!readGuest(rdram, atomicLink, atomicLink))
                        break;
                }
            }
        }
        return true;
    }

    bool readCurrentCamera(const uint8_t *rdram, Camera &out)
    {
        uint32_t camera = 0u, frame = 0u;
        if (!readGuest(rdram, kRwCurrentCamera, camera) || !guestPointer(camera, 0x70u) ||
            !readGuest(rdram, camera + kCameraFrameOffset, frame) || !guestPointer(frame, kFrameLtmOffset + 64u))
            return false;

        Matrix4 ltm{};
        if (!readFrameMatrix(rdram, frame, ltm))
            return false;
        out.position = {ltm[12], ltm[13], ltm[14]};
        out.up = {ltm[4], ltm[5], ltm[6]};
        out.target = {ltm[12] + ltm[8], ltm[13] + ltm[9], ltm[14] + ltm[10]};
        float viewWindowY = 1.0f;
        readGuest(rdram, camera + kCameraViewWindowYOffset, viewWindowY);
        if (std::isfinite(viewWindowY) && viewWindowY > 0.001f)
            out.fovy = 2.0f * std::atan(viewWindowY) * 57.29577951308232f;

        // Reuse librw-vita's RenderWare -> OpenGL camera conversion instead
        // of approximating the game camera with lookAt/fovy. RW looks down
        // positive Z, so its host GL view flips X after inverting the frame.
        int32_t projection = 0;
        float viewWindowX = 0.0f, viewOffsetX = 0.0f, viewOffsetY = 0.0f;
        float nearPlane = 0.0f, farPlane = 0.0f;
        if (readGuest(rdram, camera + kCameraProjectionOffset, projection) &&
            readGuest(rdram, camera + kCameraViewWindowXOffset, viewWindowX) &&
            readGuest(rdram, camera + kCameraViewOffsetXOffset, viewOffsetX) &&
            readGuest(rdram, camera + kCameraViewOffsetYOffset, viewOffsetY) &&
            readGuest(rdram, camera + kCameraNearPlaneOffset, nearPlane) &&
            readGuest(rdram, camera + kCameraFarPlaneOffset, farPlane) &&
            (projection == 1 || projection == 2) && std::isfinite(viewWindowX) &&
            std::isfinite(viewWindowY) && std::isfinite(viewOffsetX) && std::isfinite(viewOffsetY) &&
            std::isfinite(nearPlane) && std::isfinite(farPlane) && viewWindowX > 0.001f &&
            viewWindowY > 0.001f && nearPlane > 0.0f && farPlane > nearPlane)
        {
            const std::array<float, 3> right{ltm[0], ltm[1], ltm[2]};
            const std::array<float, 3> up{ltm[4], ltm[5], ltm[6]};
            const std::array<float, 3> at{ltm[8], ltm[9], ltm[10]};
            const std::array<float, 3> pos{ltm[12], ltm[13], ltm[14]};
            const auto dot3 = [](const auto &a, const auto &b) {
                return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
            };
            out.view = {
                -right[0], up[0], at[0], 0.0f,
                -right[1], up[1], at[1], 0.0f,
                -right[2], up[2], at[2], 0.0f,
                dot3(right, pos), -dot3(up, pos), -dot3(at, pos), 1.0f,
            };

            out.projection = p3fes_renderware_detail::makeCameraProjection(
                projection, viewWindowX, viewWindowY, viewOffsetX, viewOffsetY, nearPlane, farPlane);
            out.useMatrices = true;
        }
        return true;
    }

    bool decodeNativeSkinWeights(const uint8_t *rdram, uint32_t geometry, MeshData &mesh)
    {
        uint32_t extensionOffset = 0u;
        if (!readGuest(rdram, kSkinGeometryExtensionOffset, extensionOffset) || extensionOffset == 0u ||
            !guestPointer(geometry + extensionOffset, sizeof(uint32_t)))
            return true; // Skin plugin not installed / geometry is not skinned.

        uint32_t skin = 0u;
        if (!readGuest(rdram, geometry + extensionOffset, skin) || skin == 0u)
            return true;
        if (!guestPointer(skin, kSkinVertexWeightsOffset + sizeof(uint32_t)))
            return false;

        int32_t boneCountSigned = 0;
        uint32_t indices = 0u, weights = 0u;
        if (!readGuest(rdram, skin + kSkinNumBonesOffset, boneCountSigned) ||
            boneCountSigned <= 0 || static_cast<uint32_t>(boneCountSigned) > kMaxSkinBones ||
            !readGuest(rdram, skin + kSkinVertexIndicesOffset, indices) ||
            !readGuest(rdram, skin + kSkinVertexWeightsOffset, weights) ||
            !guestPointer(indices, static_cast<uint32_t>(mesh.vertices.size()) * 4u) ||
            !guestPointer(weights, static_cast<uint32_t>(mesh.vertices.size()) * 16u))
            return false;

        mesh.skinned = true;
        mesh.boneCount = static_cast<uint32_t>(boneCountSigned);
        for (uint32_t i = 0u; i < mesh.vertices.size(); ++i)
        {
            Vertex &vertex = mesh.vertices[i];
            std::memcpy(vertex.bones.data(), rdram + indices + i * 4u, 4u);
            for (uint32_t slot = 0u; slot < 4u; ++slot)
            {
                uint32_t bits = 0u;
                std::memcpy(&bits, rdram + weights + i * 16u + slot * 4u, sizeof(bits));
                // skinPrepareWeights (0x48A4B0) stores (bone+1)<<2 in the
                // lowest ten mantissa bits for the VU1 programs. Remove those
                // tag bits to recover the source weight; the separate index
                // array above remains the authoritative bone number.
                bits &= ~0x3FFu;
                float weight = 0.0f;
                std::memcpy(&weight, &bits, sizeof(weight));
                vertex.weights[slot] = std::isfinite(weight) && weight > 0.0f ? weight : 0.0f;
            }
        }
        return true;
    }

    bool readSkinMatrix(const uint8_t *rdram, uint32_t address, p3fes_skin_palette_math::Matrix &out)
    {
        return readRwMatrix(rdram, address, out);
    }

    struct SkinPaletteScratch
    {
        std::vector<p3fes_skin_palette_math::Matrix> inverseBind;
        std::vector<p3fes_skin_palette_math::Matrix> localMatrices;
        std::vector<p3fes_skin_palette_math::Matrix> boneLtms;
        std::vector<uint32_t> boneFrames;
    };
    SkinPaletteScratch g_skinPaletteScratch;

    uint32_t g_targetRaster = 0u; // raster of the open camera-texture bracket, 0 when none

    bool reserveSkinPaletteMathDiagnostic()
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_RESOURCE_DIAG");
            return value && *value == '1';
        }();
        static std::atomic<uint32_t> attempts{0u};
        if (!enabled)
            return false;
        return attempts.fetch_add(1u, std::memory_order_relaxed) < 8u;
    }

    bool logFirstNonordinarySkinMatrix(uint32_t atomic, const char *arrayKind,
                                       int32_t bone, const p3fes_skin_palette_math::Matrix &matrix)
    {
        for (uint32_t column = 0u; column < 4u; ++column)
        {
            for (uint32_t row = 0u; row < 3u; ++row)
            {
                const uint32_t component = column * 4u + row;
                const float value = matrix[component];
                if (p3fes_rw_matrix_math::ordinary(value))
                    continue;
                uint32_t bits = 0u;
                std::memcpy(&bits, &value, sizeof(bits));
                std::fprintf(stderr,
                             "[p3fes-skin-palette-input] atomic=%08X array=%s bone=%d component=%u value=%g bits=%08X\n",
                             atomic, arrayKind, bone, component, static_cast<double>(value), bits);
                return true;
            }
        }
        return false;
    }

    void diagnoseLocalPaletteMathFailure(
        uint32_t atomic,
        const std::vector<p3fes_skin_palette_math::Matrix> &localMatrices,
        const std::vector<p3fes_skin_palette_math::Matrix> &inverseBind)
    {
        if (!reserveSkinPaletteMathDiagnostic())
            return;
        const size_t boneCount = std::min(localMatrices.size(), inverseBind.size());
        for (size_t bone = 0u; bone < boneCount; ++bone)
        {
            if (logFirstNonordinarySkinMatrix(atomic, "inverseBind", static_cast<int32_t>(bone), inverseBind[bone]) ||
                logFirstNonordinarySkinMatrix(atomic, "localMatrices", static_cast<int32_t>(bone), localMatrices[bone]))
                return;
        }
    }

    void diagnoseFramePaletteMathFailure(
        uint32_t atomic, const p3fes_skin_palette_math::Matrix &inverseAtomicLtm,
        const std::vector<p3fes_skin_palette_math::Matrix> &boneLtms,
        const std::vector<p3fes_skin_palette_math::Matrix> &inverseBind)
    {
        if (!reserveSkinPaletteMathDiagnostic())
            return;
        const size_t boneCount = std::min(boneLtms.size(), inverseBind.size());
        for (size_t bone = 0u; bone < boneCount; ++bone)
        {
            if (logFirstNonordinarySkinMatrix(atomic, "inverseBind", static_cast<int32_t>(bone), inverseBind[bone]) ||
                logFirstNonordinarySkinMatrix(atomic, "boneLtms", static_cast<int32_t>(bone), boneLtms[bone]))
                return;
            if (logFirstNonordinarySkinMatrix(atomic, "inverseAtomicLTM", -1, inverseAtomicLtm))
                return;
        }
    }

    enum class SkinPaletteFailure : uint8_t
    {
        None,
        InvalidArguments,
        AtomicPluginField,
        HierarchyRange,
        HierarchyCount,
        InverseBindRange,
        InverseBindData,
        LocalArrayRange,
        LocalArrayData,
        FrameNodeArray,
        AtomicFrame,
        BoneFrameArray,
        AtomicLtmSync,
        AtomicMatrixFlags,
        GlobalMatrixMode,
        InverseMode,
        BoneLtmSync,
        PaletteMath,
        PaletteRange,
        Count,
    };

    const char *skinPaletteFailureName(SkinPaletteFailure reason)
    {
        static constexpr const char *names[] = {
            "none", "arguments", "atomic-plugin-field", "hierarchy-range", "hierarchy-count",
            "inverse-bind-range", "inverse-bind-data", "local-array-range", "local-array-data",
            "frame-node-array", "atomic-frame", "bone-frame-array", "atomic-ltm-sync",
            "atomic-matrix-flags", "global-matrix-mode", "inverse-mode", "bone-ltm-sync",
            "palette-math", "palette-range"};
        const size_t index = static_cast<size_t>(reason);
        return index < std::size(names) ? names[index] : "unknown";
    }

    void logSkinPaletteFailure(SkinPaletteFailure reason, uint32_t atomic, uint32_t geometry,
                               uint32_t skin, uint32_t boneCount, uint32_t detailA, uint32_t detailB)
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_RESOURCE_DIAG");
            return value && *value == '1';
        }();
        static uint8_t logged[static_cast<size_t>(SkinPaletteFailure::Count)]{};
        const size_t index = static_cast<size_t>(reason);
        if (!enabled || index >= std::size(logged) || logged[index] >= 8u)
            return;
        ++logged[index];
        std::fprintf(stderr,
                     "[p3fes-skin-palette-reject] reason=%s atomic=%08X geometry=%08X skin=%08X bones=%u detail=%08X,%08X sample=%u\n",
                     skinPaletteFailureName(reason), atomic, geometry, skin, boneCount,
                     detailA, detailB, logged[index]);
    }

    // `usedBones` (256 bits, or null for "all"): the bones some vertex of the mesh weights. The
    // renderers read palette[bone] only through those (poseSkinVertex and the GPU skin parts skip
    // weight <= 0), and a body mesh weights 50 of its 123 bones, the others a handful, so the frame
    // modes compute only those entries and leave the rest zero. Every bone's frame is still synced.
    bool prepareNativeSkinPaletteMath(uint8_t *rdram, uint32_t atomic, uint32_t skin,
                                      uint32_t boneCount, const uint32_t *usedBones,
                                      R5900Context *ctx, PS2Runtime *runtime,
                                      std::vector<p3fes_skin_palette_math::Matrix> &palette,
                                      bool &fallbackSafe, SkinPaletteFailure &failure,
                                      uint32_t &detailA, uint32_t &detailB)
    {
        using p3fes_skin_palette_math::Matrix;
        fallbackSafe = false;
        failure = SkinPaletteFailure::None;
        detailA = detailB = 0u;
        const auto reject = [&](SkinPaletteFailure reason) {
            failure = reason;
            return false;
        };
        if (!rdram || !ctx || !runtime || boneCount == 0u || boneCount > kMaxSkinBones ||
            !guestPointer(skin, kSkinVertexWeightsOffset + sizeof(uint32_t)))
            return reject(SkinPaletteFailure::InvalidArguments);
        // This helper previously drove the native renderer successfully. If
        // optional HAnim metadata doesn't match a native mode, keep that path
        // available after the caller's basic skin/context bounds are valid.
        fallbackSafe = true;

        uint32_t extensionOffset = 0u;
        if (!readGuest(rdram, kSkinAtomicExtensionOffset, extensionOffset))
            return reject(SkinPaletteFailure::AtomicPluginField);

        const uint64_t hierarchyField64 = static_cast<uint64_t>(atomic) + extensionOffset;
        if (hierarchyField64 > UINT32_MAX || !guestPointer(static_cast<uint32_t>(hierarchyField64), 4u))
            return reject(SkinPaletteFailure::AtomicPluginField);
        uint32_t hierarchy = 0u;
        if (!readGuest(rdram, static_cast<uint32_t>(hierarchyField64), hierarchy))
            return reject(SkinPaletteFailure::AtomicPluginField);
        if (hierarchy == 0u)
        {
            fallbackSafe = true;
            return p3fes_skin_palette_math::buildIdentityPalette(boneCount, palette);
        }
        if (!guestPointer(hierarchy, 12u))
            return reject(SkinPaletteFailure::HierarchyRange);

        uint32_t flags = 0u, hierarchyBoneCount = 0u, matrixOrNodeArray = 0u;
        if (!readGuest(rdram, hierarchy, flags) || !readGuest(rdram, hierarchy + 4u, hierarchyBoneCount) ||
            !readGuest(rdram, hierarchy + 8u, matrixOrNodeArray))
            return reject(SkinPaletteFailure::HierarchyRange);
        if (hierarchyBoneCount != boneCount)
        {
            detailA = flags;
            detailB = hierarchyBoneCount;
            return reject(SkinPaletteFailure::HierarchyCount);
        }

        uint32_t inverseBindAddress = 0u;
        if (!readGuest(rdram, skin + 0x0Cu, inverseBindAddress) || inverseBindAddress == 0u ||
            !guestPointer(inverseBindAddress, boneCount * 64u))
            return reject(SkinPaletteFailure::InverseBindRange);
        // The whole boneCount*64 range was validated above, so per-matrix reads cannot fail. The frame
        // palette path reads the bind matrices straight from guest memory; only the rare local-matrix
        // path and the failure diagnostics need them as vectors.
        const uint8_t *const inverseBindBase = rdram + inverseBindAddress;
        const auto loadInverseBind = [&]() -> std::vector<Matrix> & {
            auto &inverseBind = g_skinPaletteScratch.inverseBind;
            inverseBind.resize(boneCount);
            for (uint32_t i = 0u; i < boneCount; ++i)
                std::memcpy(inverseBind[i].data(), inverseBindBase + i * 64u, 64u);
            return inverseBind;
        };

        bool masked = false;
        const auto boneUsed = [usedBones](uint32_t bone) {
            return !usedBones || ((usedBones[bone >> 5u] >> (bone & 31u)) & 1u) != 0u;
        };
        const auto hierarchyMode = p3fes_skin_palette_math::hierarchyMode(flags);
        if (hierarchyMode == p3fes_skin_palette_math::HierarchyMode::FrameNodes)
        {
            uint32_t nodeArray = 0u;
            if (!readGuest(rdram, hierarchy + 0x10u, nodeArray) || nodeArray == 0u ||
                !guestPointer(nodeArray, boneCount * 16u))
                return reject(SkinPaletteFailure::FrameNodeArray);

            uint32_t atomicFrame = 0u;
            if (!readGuest(rdram, atomic + kAtomicFrameOffset, atomicFrame) || atomicFrame == 0u ||
                !guestPointer(atomicFrame, kFrameLtmOffset + 64u))
                return reject(SkinPaletteFailure::AtomicFrame);
            uint32_t boneFrames[kMaxSkinBones];
            for (uint32_t i = 0u; i < boneCount; ++i)
            {
                if (!readGuest(rdram, nodeArray + i * 16u + 0x0Cu, boneFrames[i]) || boneFrames[i] == 0u ||
                    !guestPointer(boneFrames[i], kFrameLtmOffset + 64u))
                    return reject(SkinPaletteFailure::BoneFrameArray);
                // The frames are scattered and cold; start loading the lines the loop below reads
                // (sync request word of every bone, LTM of the bones that are used) so the misses overlap.
                __builtin_prefetch(rdram + boneFrames[i] + 0xA0u);
                if (boneUsed(i))
                    __builtin_prefetch(rdram + boneFrames[i] + kFrameLtmOffset);
            }

            Matrix atomicLtm{};
            Matrix atomicBefore{};
            Matrix atomicAfter{};
            bool syncRequested = false;
            if (!readFrameMatrixAfterSync(rdram, atomicFrame, ctx, runtime,
                                          atomicBefore, atomicAfter, syncRequested))
                return reject(SkinPaletteFailure::AtomicLtmSync);
            atomicLtm = atomicAfter;
            const uint32_t atomicLtmAddress = atomicFrame + kFrameLtmOffset;
            uint32_t atomicMatrixFlags = 0u;
            if (!readGuest(rdram, atomicLtmAddress + 0x0Cu, atomicMatrixFlags))
                return reject(SkinPaletteFailure::AtomicMatrixFlags);
            const uint32_t gp = getRegU32(ctx, 28u);
            uint32_t globalOffset = 0u;
            if (!readGuest(rdram, gp - 17432u, globalOffset))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            const uint64_t matrixModeAddress64 = 0x00960074ull + globalOffset;
            if (matrixModeAddress64 > UINT32_MAX ||
                !guestPointer(static_cast<uint32_t>(matrixModeAddress64), sizeof(uint32_t)))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            uint32_t globalMatrixFlags = 0u;
            if (!readGuest(rdram, static_cast<uint32_t>(matrixModeAddress64), globalMatrixFlags))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            Matrix inverseAtomicLtm{};
            if (!p3fes_skin_palette_math::invertRwMatrix(atomicLtm, atomicMatrixFlags,
                                                         globalMatrixFlags, inverseAtomicLtm))
            {
                detailA = atomicMatrixFlags;
                detailB = globalMatrixFlags;
                return reject(SkinPaletteFailure::InverseMode);
            }

            // Entries nothing reads stay zero (clear + resize is a memset, assign(n, value) a copy loop),
            // the rest are computed in place.
            palette.clear();
            palette.resize(boneCount);
            masked = usedBones != nullptr;
            bool finite = true;
            for (uint32_t i = 0u; i < boneCount; ++i)
            {
                if (!boneUsed(i))
                {
                    if (frameSyncRequested(rdram, boneFrames[i]) &&
                        !readBoneLtmAfterSync(rdram, boneFrames[i], ctx, runtime, nullptr))
                    {
                        palette.clear();
                        return reject(SkinPaletteFailure::BoneLtmSync);
                    }
                    continue;
                }
                alignas(16) float boneLtm[16];
                alignas(16) float inverseBindMatrix[16];
                if (!readBoneLtmAfterSync(rdram, boneFrames[i], ctx, runtime, boneLtm))
                {
                    palette.clear();
                    return reject(SkinPaletteFailure::BoneLtmSync);
                }
                std::memcpy(inverseBindMatrix, inverseBindBase + i * 64u, 64u);
                finite &= p3fes_skin_palette_math::framePaletteEntry(
                    inverseAtomicLtm.data(), boneLtm, inverseBindMatrix, palette[i].data());
            }
            if (!finite)
            {
                if (reserveSkinPaletteMathDiagnostic())
                {
                    auto &boneLtms = g_skinPaletteScratch.boneLtms;
                    boneLtms.resize(boneCount);
                    for (uint32_t i = 0u; i < boneCount; ++i)
                        if (!readFrameMatrix(rdram, boneFrames[i], boneLtms[i]))
                            boneLtms[i] = p3fes_skin_palette_math::identity();
                    diagnoseFramePaletteMathFailure(atomic, inverseAtomicLtm, boneLtms, loadInverseBind());
                }
                return reject(SkinPaletteFailure::PaletteMath);
            }
        }
        else if (hierarchyMode == p3fes_skin_palette_math::HierarchyMode::LocalMatrices)
        {
            if (matrixOrNodeArray == 0u || !guestPointer(matrixOrNodeArray, boneCount * 64u))
                return reject(SkinPaletteFailure::LocalArrayRange);
            auto &localMatrices = g_skinPaletteScratch.localMatrices;
            localMatrices.resize(boneCount);
            for (uint32_t i = 0u; i < boneCount; ++i)
                if (!readSkinMatrix(rdram, matrixOrNodeArray + i * 64u, localMatrices[i]))
                    return reject(SkinPaletteFailure::LocalArrayData);
            fallbackSafe = true;
            const auto &inverseBind = loadInverseBind();
            if (!p3fes_skin_palette_math::buildLocalPalette(localMatrices, inverseBind, palette))
            {
                diagnoseLocalPaletteMathFailure(atomic, localMatrices, inverseBind);
                return reject(SkinPaletteFailure::PaletteMath);
            }
        }
        else
        {
            // With neither hierarchy flag set, +8 is a contiguous world-space
            // matrix array (64-byte stride), not the node-record array used by
            // the flags&2 branch. The ELF then applies inverseAtomicLTM.
            if (matrixOrNodeArray == 0u || !guestPointer(matrixOrNodeArray, boneCount * 64u))
                return reject(SkinPaletteFailure::LocalArrayRange);
            uint32_t atomicFrame = 0u;
            if (!readGuest(rdram, atomic + kAtomicFrameOffset, atomicFrame) || atomicFrame == 0u)
                return reject(SkinPaletteFailure::AtomicFrame);
            if (!guestPointer(atomicFrame, kFrameLtmOffset + 64u))
                return reject(SkinPaletteFailure::AtomicFrame);

            Matrix atomicLtm{};
            Matrix atomicBefore{};
            Matrix atomicAfter{};
            bool syncRequested = false;
            if (!readFrameMatrixAfterSync(rdram, atomicFrame, ctx, runtime,
                                          atomicBefore, atomicAfter, syncRequested))
                return reject(SkinPaletteFailure::AtomicLtmSync);
            atomicLtm = atomicAfter;

            uint32_t atomicLtmAddress = atomicFrame + kFrameLtmOffset;
            uint32_t atomicMatrixFlags = 0u;
            if (!readGuest(rdram, atomicLtmAddress + 0x0Cu, atomicMatrixFlags))
                return reject(SkinPaletteFailure::AtomicMatrixFlags);
            const uint32_t gp = getRegU32(ctx, 28u);
            uint32_t globalOffset = 0u;
            if (!readGuest(rdram, gp - 17432u, globalOffset))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            const uint64_t matrixModeAddress64 = 0x00960074ull + globalOffset;
            if (matrixModeAddress64 > UINT32_MAX ||
                !guestPointer(static_cast<uint32_t>(matrixModeAddress64), sizeof(uint32_t)))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            uint32_t globalMatrixFlags = 0u;
            if (!readGuest(rdram, static_cast<uint32_t>(matrixModeAddress64), globalMatrixFlags))
                return reject(SkinPaletteFailure::GlobalMatrixMode);
            Matrix inverseAtomicLtm{};
            if (!p3fes_skin_palette_math::invertRwMatrix(atomicLtm, atomicMatrixFlags,
                                                         globalMatrixFlags, inverseAtomicLtm))
            {
                detailA = atomicMatrixFlags;
                detailB = globalMatrixFlags;
                return reject(SkinPaletteFailure::InverseMode);
            }

            // matrixOrNodeArray..+boneCount*64 was range-checked above, so the reads below cannot fail.
            const uint8_t *const boneLtmBase = rdram + matrixOrNodeArray;
            palette.clear();
            palette.resize(boneCount);
            masked = usedBones != nullptr;
            bool finite = true;
            for (uint32_t i = 0u; i < boneCount; ++i)
            {
                if (!boneUsed(i))
                    continue;
                alignas(16) float boneLtm[16];
                alignas(16) float inverseBindMatrix[16];
                std::memcpy(boneLtm, boneLtmBase + i * 64u, 64u);
                std::memcpy(inverseBindMatrix, inverseBindBase + i * 64u, 64u);
                finite &= p3fes_skin_palette_math::framePaletteEntry(
                    inverseAtomicLtm.data(), boneLtm, inverseBindMatrix, palette[i].data());
            }
            if (!finite)
            {
                if (reserveSkinPaletteMathDiagnostic())
                {
                    auto &boneLtms = g_skinPaletteScratch.boneLtms;
                    boneLtms.resize(boneCount);
                    for (uint32_t i = 0u; i < boneCount; ++i)
                        std::memcpy(boneLtms[i].data(), boneLtmBase + i * 64u, 64u);
                    diagnoseFramePaletteMathFailure(atomic, inverseAtomicLtm, boneLtms, loadInverseBind());
                }
                return reject(SkinPaletteFailure::PaletteMath);
            }
        }

        for (uint32_t i = 0u; i < palette.size(); ++i)
            if ((!masked || boneUsed(i)) && !p3fes_rw_matrix_math::allNormalOrZeroXyz(palette[i].data()))
                return reject(SkinPaletteFailure::PaletteRange);
        return true;
    }

    bool prepareOriginalSkinPalette(uint8_t *rdram, uint32_t atomic, uint32_t skin,
                                    uint32_t boneCount, R5900Context *ctx, PS2Runtime *runtime,
                                    std::vector<p3fes_skin_palette_math::Matrix> &palette)
    {
        if (!ctx || !runtime)
            return false;
        const PS2Runtime::RecompiledFunction update = runtime->lookupFunction(kSkinMatrixPaletteUpdate);
        if (!update)
            return false;
        const auto result = p3fes_guest_math_helper::run(
            rdram, *ctx, *runtime, kSkinMatrixPaletteUpdate, atomic, skin);
        if (!result.completed || result.value == 0u || !guestPointer(result.value, boneCount * 64u))
            return false;
        palette.resize(boneCount);
        for (uint32_t i = 0u; i < boneCount; ++i)
            std::memcpy(palette[i].data(), rdram + result.value + i * 64u, 64u);
        return true;
    }

    bool compareSkinPaletteWithGuest(uint8_t *rdram, uint32_t atomic, uint32_t skin,
                                     uint32_t boneCount, R5900Context *ctx, PS2Runtime *runtime,
                                     const std::vector<p3fes_skin_palette_math::Matrix> &nativePalette)
    {
        if (!g_rwSkinPaletteCompare || g_rwSkinPaletteCompareLogs >= 32u)
            return true;
        // Spread the bounded comparison across animation and scene placement,
        // rather than spending all samples on the initial bind pose.
        if ((g_rwSkinPaletteCompareSeen++ % 16u) != 0u)
            return true;
        std::vector<p3fes_skin_palette_math::Matrix> guestPalette;
        if (!prepareOriginalSkinPalette(rdram, atomic, skin, boneCount, ctx, runtime, guestPalette))
        {
            std::fprintf(stderr, "[p3fes-skin] native palette compare unavailable atomic=%08X\n", atomic);
            ++g_rwSkinPaletteCompareLogs;
            return false;
        }
        float maxError = 0.0f;
        float maxRelativeError = 0.0f;
        uint32_t maxBone = 0u;
        uint32_t maxComponent = 0u;
        bool matches = true;
        for (uint32_t bone = 0u; bone < boneCount; ++bone)
            for (uint32_t component = 0u; component < 16u; ++component)
            {
                if (component % 4u == 3u)
                    continue;
                const float nativeValue = nativePalette[bone][component];
                const float guestValue = guestPalette[bone][component];
                const float error = std::fabs(nativeValue - guestValue);
                const float relativeError = error / std::max(std::fabs(guestValue), 1.0e-6f);
                if (error > maxError)
                {
                    maxError = error;
                    maxBone = bone;
                    maxComponent = component;
                }
                maxRelativeError = std::max(maxRelativeError, relativeError);
                if (!std::isfinite(nativeValue) || !std::isfinite(guestValue) ||
                    error > 1.0e-4f * std::max(1.0f, std::fabs(guestValue)))
                    matches = false;
            }
        std::fprintf(stderr,
                     "[p3fes-skin] native palette compare atomic=%08X bones=%u max-abs=%g max-rel=%g bone=%u component=%u result=%s\n",
                     atomic, boneCount, static_cast<double>(maxError),
                     static_cast<double>(maxRelativeError), maxBone, maxComponent,
                     matches ? "match" : "mismatch");
        ++g_rwSkinPaletteCompareLogs;
        return matches;
    }

    bool prepareNativeSkinPalette(uint8_t *rdram, uint32_t atomic, uint32_t geometry,
                                  R5900Context *ctx, PS2Runtime *runtime, MeshDrawState &state,
                                  const DefinedMeshInfo *definedMesh)
    {
        if (!ctx || !runtime)
            return false;
        uint32_t extensionOffset = 0u, skin = 0u;
        if (!readGuest(rdram, kSkinGeometryExtensionOffset, extensionOffset) || extensionOffset == 0u ||
            !guestPointer(geometry + extensionOffset, sizeof(uint32_t)) ||
            !readGuest(rdram, geometry + extensionOffset, skin) || skin == 0u)
            return true; // Not a skinned geometry.

        int32_t boneCountSigned = 0;
        if (!guestPointer(skin, kSkinVertexWeightsOffset + sizeof(uint32_t)) ||
            !readGuest(rdram, skin + kSkinNumBonesOffset, boneCountSigned) || boneCountSigned <= 0 ||
            static_cast<uint32_t>(boneCountSigned) > kMaxSkinBones)
            return false;
        const uint32_t boneCount = static_cast<uint32_t>(boneCountSigned);
        std::vector<p3fes_skin_palette_math::Matrix> &palette = state.skinPalette;
        bool prepared = false;
        bool fallbackSafe = false;
        SkinPaletteFailure failure = SkinPaletteFailure::None;
        uint32_t failureDetailA = 0u, failureDetailB = 0u;
        if (g_rwSkinPaletteNativeMath)
        {
            // The guest comparison (P3FES_RW_MATH_COMPARE) wants every entry.
            const uint32_t *usedBones = definedMesh && definedMesh->skinned &&
                                        definedMesh->boneCount == boneCount && !g_rwSkinPaletteCompare
                ? definedMesh->usedBones : nullptr;
            prepared = prepareNativeSkinPaletteMath(rdram, atomic, skin, boneCount, usedBones,
                                                    ctx, runtime, palette, fallbackSafe,
                                                    failure, failureDetailA, failureDetailB);
            if (!prepared)
                logSkinPaletteFailure(failure, atomic, geometry, skin, boneCount,
                                      failureDetailA, failureDetailB);
            if (prepared && !compareSkinPaletteWithGuest(rdram, atomic, skin, boneCount,
                                                         ctx, runtime, palette))
            {
                // A detected discrepancy must not become enabled again once
                // the diagnostic sample budget has been consumed.
                g_rwSkinPaletteNativeMath = false;
                prepared = false;
            }
        }
        if (prepared)
            ++g_rwSkinPaletteNativeCount;
        if (!prepared && (!g_rwSkinPaletteNativeMath || fallbackSafe))
        {
            ++g_rwSkinPaletteFallbackCount;
            prepared = prepareOriginalSkinPalette(rdram, atomic, skin, boneCount, ctx, runtime, palette);
        }
        if (!prepared)
        {
            ++g_rwSkinPaletteRejectedCount;
            return false;
        }
        return true;
    }

    bool decodeNativeMesh(const uint8_t *rdram, const AtomicSummary &summary, MeshData &mesh, MeshId &meshId)
    {
        if (summary.vertices == 0u || summary.triangles == 0u || summary.morphTargets == 0u)
            return false;

        int16_t morph = 0;
        readGuest(rdram, summary.atomic + kAtomicCurrentMorphOffset, morph);
        uint32_t morphIndex = morph >= 0 ? static_cast<uint32_t>(morph) : 0u;
        if (morphIndex >= summary.morphTargets)
            morphIndex = 0u;

        uint32_t morphBase = 0u;
        if (!readGuest(rdram, summary.geometry + kGeometryMorphTargetsOffset, morphBase) ||
            !guestPointer(morphBase, (morphIndex + 1u) * kMorphSize))
            return false;
        morphBase += morphIndex * kMorphSize;
        uint32_t positions = 0u, normals = 0u;
        if (!readGuest(rdram, morphBase + kMorphVerticesOffset, positions) ||
            !readGuest(rdram, morphBase + kMorphNormalsOffset, normals) ||
            !guestPointer(positions, summary.vertices * 12u))
            return false;

        meshId = (static_cast<MeshId>(summary.geometry) << 16u) | morphIndex;
        mesh.vertices.resize(summary.vertices);
        const bool hasNormals = normals != 0u && guestPointer(normals, summary.vertices * 12u);
        const bool hasPrelight = (summary.geometryFlags & kGeometryPrelitFlag) != 0u && summary.prelitData != 0u &&
                                 guestPointer(summary.prelitData, summary.vertices * 4u);
        const bool hasUv = summary.uv0Data != 0u && guestPointer(summary.uv0Data, summary.vertices * 8u);
        mesh.hasNormals = hasNormals;
        mesh.hasPrelight = hasPrelight;
        mesh.lighting = (summary.geometryFlags & kGeometryLightingFlag) != 0u;
        for (uint32_t i = 0; i < summary.vertices; ++i)
        {
            Vertex &v = mesh.vertices[i];
            if (!readVec3(rdram, positions + i * 12u, v.position))
                return false;
            if (hasNormals)
                readVec3(rdram, normals + i * 12u, v.normal);
            if (hasUv)
            {
                readGuest(rdram, summary.uv0Data + i * 8u + 0u, v.texCoord[0]);
                readGuest(rdram, summary.uv0Data + i * 8u + 4u, v.texCoord[1]);
            }
            if (hasPrelight)
                std::memcpy(v.color.data(), rdram + summary.prelitData + i * 4u, 4u);
        }
        if (!decodeNativeSkinWeights(rdram, summary.geometry, mesh))
            return false;
        // The second texture coordinate set belongs to the MatFX dual pass (RenderWare textures the
        // second pass with set 1 when the geometry has one). Vertex has no field for it and static
        // meshes never use `weights`, so it travels there: (u1, v1, 1, 0). The weights[2] == 1 marker
        // is what the renderers test; skinned meshes keep their real weights and use set 0.
        if (!mesh.skinned && (summary.geometryFlags & kGeometryTextured2Flag) != 0u)
        {
            uint32_t uv1Data = 0u;
            if (readGuest(rdram, summary.geometry + kGeometryUv1Offset, uv1Data) && uv1Data &&
                guestPointer(uv1Data, summary.vertices * 8u))
                for (uint32_t i = 0; i < summary.vertices; ++i)
                {
                    Vertex &v = mesh.vertices[i];
                    readGuest(rdram, uv1Data + i * 8u + 0u, v.weights[0]);
                    readGuest(rdram, uv1Data + i * 8u + 4u, v.weights[1]);
                    v.weights[2] = 1.0f;
                    v.weights[3] = 0.0f;
                }
        }

        uint32_t materialArray = 0u;
        int32_t materialCountSigned = 0;
        const bool hasMaterialList = readGuest(rdram, summary.geometry + kGeometryMatListArrayOffset, materialArray) &&
                                     readGuest(rdram, summary.geometry + kGeometryMatListCountOffset, materialCountSigned) &&
                                     materialCountSigned > 0 && materialCountSigned <= 4096 &&
                                     guestPointer(materialArray, static_cast<uint32_t>(materialCountSigned) * sizeof(uint32_t));
        const uint32_t materialCount = hasMaterialList ? static_cast<uint32_t>(materialCountSigned) : 1u;
        std::vector<std::vector<uint32_t>> materialIndices(materialCount);
        for (uint32_t i = 0; i < summary.triangles; ++i)
        {
            const uint32_t tri = summary.triangleData + i * 8u;
            uint16_t a = 0u, b = 0u, c = 0u, material = 0u;
            if (!readGuest(rdram, tri + 0u, a) || !readGuest(rdram, tri + 2u, b) || !readGuest(rdram, tri + 4u, c) ||
                a >= summary.vertices || b >= summary.vertices || c >= summary.vertices)
                return false;
            readGuest(rdram, tri + kTriangleMaterialOffset, material);
            const uint32_t materialIndex = material < materialCount ? material : 0u;
            auto &indices = materialIndices[materialIndex];
            indices.push_back(a);
            indices.push_back(b);
            indices.push_back(c);
        }

        // P3FES_MESH_TRI_DIAG=<hex mesh id>: triangle counts and local bounds per material of that atomic mesh.
        {
            static const unsigned long long diagId = [] {
                const char *v = std::getenv("P3FES_MESH_TRI_DIAG");
                return v ? std::strtoull(v, nullptr, 16) : 0ull;
            }();
            if (diagId && diagId == meshId)
            {
                std::fprintf(stderr, "[p3fes-tri] mesh %llX vertices=%u triangles=%u materials=%u geomFlags=%08X\n",
                             static_cast<unsigned long long>(meshId), summary.vertices, summary.triangles,
                             materialCount, summary.geometryFlags);
                for (uint32_t word = 0x08u; word < 0x60u; word += 4u)
                {
                    uint32_t value = 0u;
                    readGuest(rdram, summary.geometry + word, value);
                    std::fprintf(stderr, "[p3fes-tri] geometry+%02X = %08X\n", word, value);
                }
                for (uint32_t m = 0u; m < materialCount; ++m)
                {
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                    for (uint32_t index : materialIndices[m])
                        for (int c = 0; c < 3; ++c)
                        {
                            lo[c] = std::min(lo[c], mesh.vertices[index].position[c]);
                            hi[c] = std::max(hi[c], mesh.vertices[index].position[c]);
                        }
                    uint32_t material = 0u;
                    uint8_t col[4] = {};
                    if (hasMaterialList && readGuest(rdram, materialArray + m * 4u, material) && material && guestPointer(material, 28u))
                        std::memcpy(col, rdram + material + kMaterialColorOffset, 4u);
                    p3fes::native_renderer::Material surface;
                    if (material)
                        readMaterialSurface(rdram, material, surface);
                    uint32_t dbgTexture = 0u, dbgRaster = 0u;
                    if (material && readGuest(rdram, material + kMaterialTextureOffset, dbgTexture) && dbgTexture && guestPointer(dbgTexture, 4u))
                        readGuest(rdram, dbgTexture + kTextureRasterOffset, dbgRaster);
                    p3fes::native_renderer::MeshDrawState::SamplerState dbgSampler;
                    const bool dbgSamplerOk = dbgTexture && readTextureSampler(rdram, dbgTexture, dbgSampler);
                    uint32_t dbgPacked = 0u;
                    if (dbgTexture) readGuest(rdram, dbgTexture + kTextureFilterAddressOffset, dbgPacked);
                    std::fprintf(stderr, "[p3fes-tri]   mat %u material=%08X texture=%08X raster=%08X packed=%08X samplerOk=%d u=%d v=%d linear=%d\n", m, material, dbgTexture, dbgRaster,
                                 dbgPacked, dbgSamplerOk ? 1 : 0, static_cast<int>(dbgSampler.u), static_cast<int>(dbgSampler.v), dbgSampler.linearFilter ? 1 : 0);
                    double pre[3] = {}, nrm[3] = {};
                    for (uint32_t index : materialIndices[m])
                        for (int c = 0; c < 3; ++c)
                        {
                            pre[c] += mesh.vertices[index].color[c];
                            nrm[c] += mesh.vertices[index].normal[c];
                        }
                    float uvLo[2] = {1e30f, 1e30f}, uvHi[2] = {-1e30f, -1e30f};
                    for (uint32_t index : materialIndices[m])
                        for (int c = 0; c < 2; ++c)
                        {
                            uvLo[c] = std::min(uvLo[c], mesh.vertices[index].texCoord[c]);
                            uvHi[c] = std::max(uvHi[c], mesh.vertices[index].texCoord[c]);
                        }
                    if (std::getenv("P3FES_MESH_TRI_VERTS") && m == static_cast<uint32_t>(std::atoi(std::getenv("P3FES_MESH_TRI_VERTS"))))
                        for (uint32_t index : materialIndices[m])
                        {
                            const Vertex &v = mesh.vertices[index];
                            std::fprintf(stderr, "[p3fes-tri]     v%u pos=(%.1f,%.1f,%.1f) n=(%.3f,%.3f,%.3f) rgba=%u,%u,%u,%u uv=(%.3f,%.3f)\n",
                                         index, static_cast<double>(v.position[0]), static_cast<double>(v.position[1]),
                                         static_cast<double>(v.position[2]), static_cast<double>(v.normal[0]),
                                         static_cast<double>(v.normal[1]), static_cast<double>(v.normal[2]),
                                         v.color[0], v.color[1], v.color[2], v.color[3],
                                         static_cast<double>(v.texCoord[0]), static_cast<double>(v.texCoord[1]));
                        }
                    unsigned alphaLo = 255u, alphaHi = 0u;
                    for (uint32_t index : materialIndices[m])
                    {
                        alphaLo = std::min<unsigned>(alphaLo, mesh.vertices[index].color[3]);
                        alphaHi = std::max<unsigned>(alphaHi, mesh.vertices[index].color[3]);
                    }
                    std::fprintf(stderr, "[p3fes-tri]   mat %u prelit alpha=%u..%u\n", m, alphaLo, alphaHi);
                    std::fprintf(stderr, "[p3fes-tri]   mat %u uv=(%.3f,%.3f)-(%.3f,%.3f)\n", m,
                                 static_cast<double>(uvLo[0]), static_cast<double>(uvLo[1]),
                                 static_cast<double>(uvHi[0]), static_cast<double>(uvHi[1]));
                    const double n = std::max<size_t>(materialIndices[m].size(), 1u);
                    std::fprintf(stderr, "[p3fes-tri]   mat %u tris=%zu color=%u,%u,%u,%u amb=%.2f diff=%.2f prelit=%s(%.0f,%.0f,%.0f) meanN=(%.2f,%.2f,%.2f) bounds=(%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f)\n",
                                 m, materialIndices[m].size() / 3u, col[0], col[1], col[2], col[3],
                                 static_cast<double>(surface.ambient), static_cast<double>(surface.diffuse),
                                 hasPrelight ? "y" : "n", pre[0] / n, pre[1] / n, pre[2] / n,
                                 nrm[0] / n, nrm[1] / n, nrm[2] / n,
                                 lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                }
            }
        }

        mesh.materials.resize(materialCount);
        for (uint32_t materialIndex = 0u; materialIndex < materialCount; ++materialIndex)
        {
            const auto &indices = materialIndices[materialIndex];
            if (!indices.empty())
            {
                const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
                mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
                mesh.submeshes.push_back(Submesh{firstIndex, static_cast<uint32_t>(indices.size()), materialIndex});
            }
            if (!hasMaterialList)
                continue;

            uint32_t material = 0u;
            if (!readGuest(rdram, materialArray + materialIndex * 4u, material) || !material || !guestPointer(material, 28u))
                continue;
            std::memcpy(mesh.materials[materialIndex].color.data(), rdram + material + kMaterialColorOffset, 4u);
            readMaterialSurface(rdram, material, mesh.materials[materialIndex]);

            uint32_t texture = 0u, raster = 0u;
            if (!readGuest(rdram, material + kMaterialTextureOffset, texture) || !texture || !guestPointer(texture, 4u) ||
                !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster)
                continue;
            std::string textureName;
            if (!prepareNativeRasterTexture(rdram, raster, textureName))
                continue;
            mesh.materials[materialIndex].textureName = std::move(textureName);
        }
        return true;
    }

    bool decodeNativeSector(const uint8_t *rdram, uint32_t sector, MeshData &mesh, MeshId &meshId,
                            MeshDrawState &drawState)
    {
        uint16_t triangleCount = 0u, materialBase = 0u;
        uint32_t triangles = 0u, positions = 0u, normals = 0u, uv0 = 0u, prelit = 0u;
        uint32_t world = 0u, worldFlags = 0u, materialArray = 0u;
        int32_t worldMaterialCountSigned = 0;
        if (!guestPointer(sector, kSectorNumTrianglesOffset + sizeof(uint16_t)) ||
            !readGuest(rdram, sector + kSectorNumTrianglesOffset, triangleCount) || triangleCount == 0u ||
            !readGuest(rdram, sector + kSectorMaterialBaseOffset, materialBase) ||
            !readGuest(rdram, sector + kSectorTrianglesOffset, triangles) ||
            !readGuest(rdram, sector + kSectorVerticesOffset, positions) ||
            !readGuest(rdram, sector + kSectorNormalsOffset, normals) ||
            !readGuest(rdram, sector + kSectorUv0Offset, uv0) ||
            !readGuest(rdram, sector + kSectorPrelitOffset, prelit) ||
            !readGuest(rdram, kRwCurrentWorld, world) || !guestPointer(world, kWorldNumMaterialsOffset + 4u) ||
            !readGuest(rdram, world + kWorldFlagsOffset, worldFlags) ||
            !readGuest(rdram, world + kWorldMaterialsOffset, materialArray) ||
            !readGuest(rdram, world + kWorldNumMaterialsOffset, worldMaterialCountSigned) ||
            worldMaterialCountSigned <= 0 || worldMaterialCountSigned > 65535 ||
            materialBase >= static_cast<uint32_t>(worldMaterialCountSigned) ||
            !guestPointer(materialArray, static_cast<uint32_t>(worldMaterialCountSigned) * sizeof(uint32_t)) ||
            !guestPointer(triangles, static_cast<uint32_t>(triangleCount) * 8u))
        {
            return false;
        }

        uint32_t vertexCount = 0u, maxLocalMaterial = 0u;
        std::vector<std::array<uint16_t, 4>> sourceTriangles(triangleCount);
        for (uint32_t i = 0u; i < triangleCount; ++i)
        {
            auto &tri = sourceTriangles[i];
            const uint32_t address = triangles + i * 8u;
            if (!readGuest(rdram, address + 0u, tri[0]) || !readGuest(rdram, address + 2u, tri[1]) ||
                !readGuest(rdram, address + 4u, tri[2]) || !readGuest(rdram, address + 6u, tri[3]))
                return false;
            vertexCount = std::max(vertexCount, static_cast<uint32_t>(std::max({tri[0], tri[1], tri[2]})) + 1u);
            maxLocalMaterial = std::max(maxLocalMaterial, static_cast<uint32_t>(tri[3]));
        }
        if (vertexCount == 0u || vertexCount > kMaxVertices || !guestPointer(positions, vertexCount * 12u))
        {
            return false;
        }

        const bool hasNormals = normals != 0u && guestPointer(normals, vertexCount * 4u);
        const bool hasUv = uv0 != 0u && guestPointer(uv0, vertexCount * 8u);
        const bool hasPrelight = (worldFlags & kGeometryPrelitFlag) != 0u && prelit != 0u &&
                                 guestPointer(prelit, vertexCount * 4u);
        mesh.vertices.resize(vertexCount);
        mesh.hasNormals = hasNormals;
        mesh.hasPrelight = hasPrelight;
        mesh.lighting = (worldFlags & kGeometryLightingFlag) != 0u;
        for (uint32_t i = 0u; i < vertexCount; ++i)
        {
            Vertex &v = mesh.vertices[i];
            if (!readVec3(rdram, positions + i * 12u, v.position))
                return false;
            if (hasNormals)
            {
                int8_t packed[4]{};
                std::memcpy(packed, rdram + normals + i * 4u, sizeof(packed));
                v.normal = {packed[0] / 127.0f, packed[1] / 127.0f, packed[2] / 127.0f};
            }
            if (hasUv)
            {
                readGuest(rdram, uv0 + i * 8u + 0u, v.texCoord[0]);
                readGuest(rdram, uv0 + i * 8u + 4u, v.texCoord[1]);
            }
            if (hasPrelight)
                std::memcpy(v.color.data(), rdram + prelit + i * 4u, 4u);
        }

        const uint32_t availableMaterials = static_cast<uint32_t>(worldMaterialCountSigned) - materialBase;
        if (maxLocalMaterial >= availableMaterials)
        {
            return false;
        }
        const uint32_t localMaterialCount = maxLocalMaterial + 1u;
        std::vector<std::vector<uint32_t>> materialIndices(localMaterialCount);
        for (const auto &tri : sourceTriangles)
        {
            const uint32_t localMaterial = tri[3] < localMaterialCount ? tri[3] : 0u;
            auto &indices = materialIndices[localMaterial];
            indices.push_back(tri[0]);
            indices.push_back(tri[1]);
            indices.push_back(tri[2]);
        }

        mesh.materials.resize(localMaterialCount);
        drawState.materialColors.assign(localMaterialCount, {255u, 255u, 255u, 255u});
        drawState.materialSamplers.resize(localMaterialCount);
        drawState.materialTextures.resize(localMaterialCount);
        drawState.materialUvTransforms.resize(localMaterialCount);
        drawState.materialDualPasses.resize(localMaterialCount);
        drawState.materialEnvPasses.resize(localMaterialCount);
        for (uint32_t localMaterial = 0u; localMaterial < localMaterialCount; ++localMaterial)
        {
            const auto &indices = materialIndices[localMaterial];
            if (!indices.empty())
            {
                const uint32_t firstIndex = static_cast<uint32_t>(mesh.indices.size());
                mesh.indices.insert(mesh.indices.end(), indices.begin(), indices.end());
                mesh.submeshes.push_back(Submesh{firstIndex, static_cast<uint32_t>(indices.size()), localMaterial});
            }

            uint32_t material = 0u;
            const uint32_t worldMaterial = materialBase + localMaterial;
            if (!readGuest(rdram, materialArray + worldMaterial * 4u, material) || !material || !guestPointer(material, 28u))
                continue;
            std::memcpy(mesh.materials[localMaterial].color.data(), rdram + material + kMaterialColorOffset, 4u);
            readMaterialSurface(rdram, material, mesh.materials[localMaterial]);
            drawState.materialColors[localMaterial] = mesh.materials[localMaterial].color;
            readMaterialUvTransform(rdram, material, drawState.materialUvTransforms[localMaterial]);
            readMaterialDualPass(rdram, material, drawState.materialDualPasses[localMaterial]);
            readMaterialEnvPass(rdram, material, drawState.materialEnvPasses[localMaterial]);

            uint32_t texture = 0u, raster = 0u;
            if (!readGuest(rdram, material + kMaterialTextureOffset, texture) || !texture || !guestPointer(texture, 4u) ||
                !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster)
                continue;
            readTextureSampler(rdram, texture, drawState.materialSamplers[localMaterial]);
            std::string textureName;
            if (!prepareNativeRasterTexture(rdram, raster, textureName))
                continue;
            drawState.materialTextures[localMaterial] = textureName;
            mesh.materials[localMaterial].textureName = std::move(textureName);
        }
        meshId = (1ull << 63u) | sector;
        return !mesh.indices.empty();
    }

    // ---- Camera-texture passes (character shadow map) -----------------------------------------
    // A RenderWare camera whose raster is a type-5 camera texture renders into a host render target
    // named after that raster; draws that later bind the raster sample the target. The bracket opens
    // at the camera's RwCameraClear (which precedes its BeginUpdate in the shadow task) or at its
    // first draw, and closes at the first recording call made outside the texture camera.
    // P3FES_SHADOWS=0 restores the previous behaviour (pass not recorded, transparent texture).
    bool cameraTexturesEnabled()
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_SHADOWS");
            return !value || *value != '0';
        }();
        return enabled;
    }

    std::string cameraTextureName(uint32_t raster)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "rt.%X", raster);
        return name;
    }

    // False while the open frame holds nothing but camera-texture passes. Such a frame is not
    // presented on its own: it stays open so the passes travel with the display frame that follows
    // (the shadow task runs before the main camera's clear, which is a frame boundary).
    bool g_frameHasDisplayContent = false;
    uint64_t g_targetPasses = 0u, g_targetClears = 0u;

    // `anyCamera`: open the frame even when no RenderWare camera is current (a camera-texture clear
    // can be the first recording call of a frame); every world draw records its own camera anyway.
    // P3FES_SHADOW_DIAG=1: GS/RenderWare blend state of the draws of a camera-texture pass and of the
    // draws that sample one (a few lines each).
    bool shadowDiag(const uint8_t *rdram, const char *what)
    {
        static const bool enabled = [] {
            const char *value = std::getenv("P3FES_SHADOW_DIAG");
            return value && *value == '1';
        }();
        static unsigned lines = 0u;
        if (!enabled || lines >= 60u)
            return false;
        ++lines;
        uint64_t flags = 0u, alpha = 0u, test = 0u, zbuf = 0u;
        uint32_t src = 0u, dst = 0u;
        readGuest(rdram, kRwRenderFlags, flags);
        readGuest(rdram, kRwAlpha1, alpha);
        readGuest(rdram, kRwTest1, test);
        readGuest(rdram, kRwZbuf1, zbuf);
        readGuest(rdram, kRwSrcBlend, src);
        readGuest(rdram, kRwDstBlend, dst);
        std::fprintf(stderr, "[p3fes-shadow] %s flags=%llX alpha1=%llX test=%llX zbuf=%llX rw-blend=%u/%u\n", what,
                     static_cast<unsigned long long>(flags), static_cast<unsigned long long>(alpha),
                     static_cast<unsigned long long>(test), static_cast<unsigned long long>(zbuf), src, dst);
        return true;
    }

    bool openNativeFrame(const uint8_t *rdram, bool anyCamera = false)
    {
        if (g_nativeFrameActive)
            return true;
        Camera camera;
        if (!readCurrentCamera(rdram, camera) && !anyCamera)
            return false;
        p3fes_native_present::beginFrame(camera, ClearColor{});
        g_nativeFrameCameraState = camera;
        readGuest(rdram, kRwCurrentCamera, g_nativeFrameCameraAddress);
        ++g_nativeFrameNumber;
        g_nativeFrameActive = true;
        return true;
    }

    void closeCameraTexture()
    {
        if (!g_targetRaster)
            return;
        p3fes_native_present::endTextureTarget();
        g_targetRaster = 0u;
    }

    bool openCameraTexture(const uint8_t *rdram, uint32_t raster)
    {
        if (!cameraTexturesEnabled() || !raster || !guestPointer(raster, 52u))
            return false;
        if (g_targetRaster == raster && p3fes_native_present::inTextureTarget())
            return true;
        closeCameraTexture();
        int32_t width = 0, height = 0;
        if (!readGuest(rdram, raster + 12u, width) || !readGuest(rdram, raster + 16u, height) ||
            width <= 0 || height <= 0 || width > 1024 || height > 1024)
            return false;
        if (!openNativeFrame(rdram, true))
            return false;
        if (!p3fes_native_present::beginTextureTarget(cameraTextureName(raster), static_cast<uint32_t>(width),
                                                      static_cast<uint32_t>(height)))
            return false;
        g_targetRaster = raster;
        ++g_targetPasses;
        if (g_diagEnabled.load(std::memory_order_relaxed) && g_targetPasses <= 4u)
            std::fprintf(stderr, "[p3fes-camera-texture] pass raster=%08X %dx%d frame=%llu\n", raster, width, height,
                         static_cast<unsigned long long>(g_nativeFrameNumber));
        return true;
    }

    // ---- Widescreen ---------------------------------------------------------------------------
    // The game keeps its display aspect ratio as a float at 0x7CAD1C (4/3; the PCSX2 widescreen
    // patch for SLUS-21621 writes 16/9 there, "X-Fov"). Writing the host aspect instead widens the
    // RenderWare view window, so both the picture and the game's own frustum culling cover the
    // whole screen; the host renderer then draws the world full-screen instead of in a 4:3 area.
    // P3FES_WIDESCREEN: 0 off, 1 2-D stretched with the picture, 2 (default) 2-D with square pixels,
    // each HUD layer pinned to the screen edge it touches (see renderHostFrame). The patch's other entries rewrite HUD code and do not apply
    // to recompiled code.
    constexpr uint32_t kAspectRatioAddress = 0x007CAD1Cu;
    constexpr uint32_t kAspectRatioNarrow = 0x3FAAAAABu; // 4/3
    constexpr float kHostAspect = 960.0f / 544.0f;

    int widescreenMode()
    {
        static const int mode = [] {
            const char *value = std::getenv("P3FES_WIDESCREEN");
            const int parsed = value && *value ? std::atoi(value) : 2;
            return parsed < 0 || parsed > 2 ? 2 : parsed;
        }();
        return mode;
    }

    void applyWidescreen(uint8_t *rdram)
    {
        const int mode = widescreenMode();
        if (mode == 0)
            return;
        uint32_t aspect = 0u;
        if (readGuest(rdram, kAspectRatioAddress, aspect) && aspect == kAspectRatioNarrow)
        {
            std::memcpy(rdram + kAspectRatioAddress, &kHostAspect, sizeof(kHostAspect));
            p3fes_native_present::setWidescreen(mode);
            std::fprintf(stderr, "[p3fes-native] widescreen mode %d: aspect %.4f written at %08X\n", mode, kHostAspect,
                         kAspectRatioAddress);
        }
    }

    bool ensureNativeFrame(uint8_t *rdram)
    {
        applyWidescreen(rdram);
        if (p3fes_native_camera::inTextureCamera())
        {
            // Draws of a camera-texture pass go to that raster's render target. Without one the
            // pass stays on the original guest path (a no-op while GS rasterization is suppressed).
            uint32_t camera = 0u, raster = 0u;
            return readGuest(rdram, kRwCurrentCamera, camera) && guestPointer(camera, 0x64u) &&
                   readGuest(rdram, camera + 0x60u, raster) && openCameraTexture(rdram, raster);
        }
        closeCameraTexture();
        if (!openNativeFrame(rdram))
            return false;
        g_frameHasDisplayContent = true;
        return true;
    }

    // Clear can open a frame before RwCameraBeginUpdate selects its camera.
    // Snapshot the actual camera at 3-D submission boundaries, while leaving
    // screen-space Im2D glyphs independent of this state.
    //
    // This runs for every draw and the camera hardly ever changes between two of them, so the Camera
    // built from the guest is kept together with the guest words it was built from (these are all
    // that readCurrentCamera reads): while they are unchanged the same Camera comes back.
    void captureNativeDrawCamera(const uint8_t *rdram)
    {
        constexpr size_t kKeyWords = 21u;
        struct CameraCache
        {
            bool valid = false;
            bool ok = false;
            uint32_t words[kKeyWords]{};
            Camera camera;
        };
        static CameraCache cache;
        uint32_t cameraAddress = 0u, frame = 0u;
        if (!readGuest(rdram, kRwCurrentCamera, cameraAddress) ||
            !guestPointer(cameraAddress, kCameraFarPlaneOffset + sizeof(float)) ||
            !readGuest(rdram, cameraAddress + kCameraFrameOffset, frame) ||
            !guestPointer(frame, kFrameLtmOffset + 64u))
        {
            Camera camera;
            if (readCurrentCamera(rdram, camera))
                p3fes_native_present::setCamera(camera);
            return;
        }
        uint32_t words[kKeyWords];
        words[0] = cameraAddress;
        words[1] = frame;
        const uint8_t *ltm = rdram + frame + kFrameLtmOffset;
        for (uint32_t row = 0u; row < 4u; ++row)
            std::memcpy(&words[2u + row * 3u], ltm + row * 16u, 12u); // the 12 lanes readFrameMatrix uses
        const uint8_t *camera = rdram + cameraAddress;
        std::memcpy(&words[14], camera + kCameraProjectionOffset, 4u);
        std::memcpy(&words[15], camera + kCameraViewWindowXOffset, 4u);
        std::memcpy(&words[16], camera + kCameraViewWindowYOffset, 4u);
        std::memcpy(&words[17], camera + kCameraViewOffsetXOffset, 4u);
        std::memcpy(&words[18], camera + kCameraViewOffsetYOffset, 4u);
        std::memcpy(&words[19], camera + kCameraNearPlaneOffset, 4u);
        std::memcpy(&words[20], camera + kCameraFarPlaneOffset, 4u);
        if (!cache.valid || std::memcmp(cache.words, words, sizeof(words)) != 0)
        {
            cache.camera = Camera{};
            cache.ok = readCurrentCamera(rdram, cache.camera);
            std::memcpy(cache.words, words, sizeof(words));
            cache.valid = true;
        }
        if (cache.ok)
            p3fes_native_present::setCamera(cache.camera);
    }

    bool drawNativeAtomic(uint8_t *rdram, uint32_t atomic, R5900Context *ctx, PS2Runtime *runtime,
                          const MeshDrawState::BlendState *blendOverride = nullptr)
    {
        // Diagnostics are fixed at start-up: one load here instead of one per block below.
        if (g_gsColorMasked)
        {
            ++g_maskDropped[0];
            return true;
        }
        const bool diag = g_diagEnabled.load(std::memory_order_relaxed);
        AtomicSummary summary;
        if (!inspectAtomic(rdram, atomic, summary) || (summary.flags & kAtomicRenderFlag) == 0u)
            return false;
        if (!ensureNativeFrame(rdram))
            return false;
        captureNativeDrawCamera(rdram);

        if (diag &&
            g_loggedAtomicCameras.size() < 16u && g_loggedAtomicCameras.insert(atomic).second)
        {
            uint32_t currentAddress = 0u;
            int32_t projection = 0;
            Camera current{};
            readGuest(rdram, kRwCurrentCamera, currentAddress);
            readGuest(rdram, currentAddress + kCameraProjectionOffset, projection);
            float viewX = 0.0f, viewY = 0.0f, nearPlane = 0.0f, farPlane = 0.0f;
            readGuest(rdram, currentAddress + kCameraViewWindowXOffset, viewX);
            readGuest(rdram, currentAddress + kCameraViewWindowYOffset, viewY);
            readGuest(rdram, currentAddress + kCameraNearPlaneOffset, nearPlane);
            readGuest(rdram, currentAddress + kCameraFarPlaneOffset, farPlane);
            std::fprintf(stderr,
                "[p3fes-camera-fields] camera=%08X projection=%d view=%.9g,%.9g near=%.9g far=%.9g\n",
                currentAddress, projection, viewX, viewY, nearPlane, farPlane);
            if (readCurrentCamera(rdram, current))
                std::fprintf(stderr,
                    "[p3fes-camera-frame] frame=%llu atomic=%08X saved-camera=%08X current-camera=%08X projection=%d "
                    "saved-pos=%.3f,%.3f,%.3f current-pos=%.3f,%.3f,%.3f "
                    "saved-z=%.6f,%.6f,%.6f current-z=%.6f,%.6f,%.6f\n",
                    static_cast<unsigned long long>(g_nativeFrameNumber), atomic,
                    g_nativeFrameCameraAddress, currentAddress, projection,
                    g_nativeFrameCameraState.position[0], g_nativeFrameCameraState.position[1], g_nativeFrameCameraState.position[2],
                    current.position[0], current.position[1], current.position[2],
                    g_nativeFrameCameraState.projection[10], g_nativeFrameCameraState.projection[11], g_nativeFrameCameraState.projection[14],
                    current.projection[10], current.projection[11], current.projection[14]);
        }

        MeshId meshId = 0u;
        const int16_t morph = [&] { int16_t value = 0; readGuest(rdram, atomic + kAtomicCurrentMorphOffset, value); return value; }();
        const uint32_t morphIndex = morph >= 0 && static_cast<uint32_t>(morph) < summary.morphTargets ? static_cast<uint32_t>(morph) : 0u;
        meshId = (static_cast<MeshId>(summary.geometry) << 16u) | morphIndex;
        // Direct-mapped front for g_definedMeshes (nodes are stable, nothing is ever erased).
        DefinedMeshSlot &definedSlot = g_definedMeshCache[((meshId * 0x9E3779B97F4A7C15ull) >> 56) & 255u];
        const DefinedMeshInfo *definedMesh = definedSlot.info && definedSlot.id == meshId ? definedSlot.info : nullptr;
        if (!definedMesh)
        {
            const auto found = g_definedMeshes.find(meshId);
            if (found != g_definedMeshes.end())
                definedMesh = &found->second;
        }
        if (!definedMesh)
        {
            MeshData mesh;
            MeshId decodedId = 0u;
            if (!decodeNativeMesh(rdram, summary, mesh, decodedId) || decodedId != meshId)
            {
                if (g_nativeErrors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] mesh 0x%08X decode failed\n", summary.geometry);
                return false;
            }
            if (diag && mesh.skinned &&
                !mesh.vertices.empty() && g_skinDiagnosticSamples.size() < 8u)
            {
                SkinDiagnosticSample sample;
                sample.vertices = {mesh.vertices.front(), mesh.vertices[mesh.vertices.size() / 2u], mesh.vertices.back()};
                sample.boneCount = mesh.boneCount;
                sample.hasPrelight = mesh.hasPrelight;
                sample.materialCount = static_cast<uint32_t>(mesh.materials.size());
                sample.submeshCount = static_cast<uint32_t>(mesh.submeshes.size());
                if (!mesh.materials.empty())
                    sample.firstMaterialColor = mesh.materials.front().color;
                if (!mesh.submeshes.empty())
                    sample.firstSubmeshIndexCount = mesh.submeshes.front().indexCount;
                g_skinDiagnosticSamples[meshId] = sample;
                uint32_t invalidBones = 0u, emptyWeights = 0u;
                float minWeightSum = 4.0f, maxWeightSum = 0.0f;
                auto low = mesh.vertices.front().position, high = low;
                for (const Vertex &vertex : mesh.vertices)
                {
                    float sum = 0.0f;
                    for (unsigned j = 0u; j < 4u; ++j)
                        if (vertex.weights[j] > 0.0f)
                        {
                            sum += vertex.weights[j];
                            invalidBones += vertex.bones[j] >= mesh.boneCount;
                        }
                    emptyWeights += !(sum > 0.0f);
                    minWeightSum = std::min(minWeightSum, sum);
                    maxWeightSum = std::max(maxWeightSum, sum);
                    for (unsigned j = 0u; j < 3u; ++j)
                    {
                        low[j] = std::min(low[j], vertex.position[j]);
                        high[j] = std::max(high[j], vertex.position[j]);
                    }
                }
                std::fprintf(stderr,
                    "[p3fes-skin-source] mesh=%llX atomic=%08X vertices=%u bones=%u invalid-bones=%u empty-weights=%u "
                    "weight-sum=%.9g..%.9g bounds=%.4g,%.4g,%.4g..%.4g,%.4g,%.4g\n",
                    static_cast<unsigned long long>(meshId), atomic, static_cast<unsigned>(mesh.vertices.size()),
                    mesh.boneCount, invalidBones, emptyWeights, minWeightSum, maxWeightSum,
                    low[0], low[1], low[2], high[0], high[1], high[2]);
                const SkinDiagnosticSample &saved = g_skinDiagnosticSamples[meshId];
                std::fprintf(stderr,
                    "[p3fes-skin-source-color] mesh=%llX prelight=%u vertex-rgba="
                    "%u,%u,%u,%u/%u,%u,%u,%u/%u,%u,%u,%u material-count=%u first-material-rgba=%u,%u,%u,%u "
                    "submeshes=%u first-indices=%u\n",
                    static_cast<unsigned long long>(meshId), saved.hasPrelight ? 1u : 0u,
                    saved.vertices[0].color[0], saved.vertices[0].color[1], saved.vertices[0].color[2], saved.vertices[0].color[3],
                    saved.vertices[1].color[0], saved.vertices[1].color[1], saved.vertices[1].color[2], saved.vertices[1].color[3],
                    saved.vertices[2].color[0], saved.vertices[2].color[1], saved.vertices[2].color[2], saved.vertices[2].color[3],
                    saved.materialCount, saved.firstMaterialColor[0], saved.firstMaterialColor[1],
                    saved.firstMaterialColor[2], saved.firstMaterialColor[3], saved.submeshCount,
                    saved.firstSubmeshIndexCount);
            }
            p3fes_native_present::defineMesh(meshId, mesh);
            definedMesh = &g_definedMeshes.emplace(meshId, describeMesh(mesh)).first->second;
        }
        definedSlot = {meshId, definedMesh};

        Matrix4 model{};
        Matrix4 rawModel{};
        bool frameSyncRequested = false;
        if (!readFrameMatrixAfterSync(rdram, summary.frame, ctx, runtime, rawModel, model, frameSyncRequested))
            return false;
        if (diag)
        {
            const auto sample = g_skinDiagnosticSamples.find(meshId);
            if (sample != g_skinDiagnosticSamples.end())
            {
                auto stateIt = g_skinFrameDiagnostics.find(atomic);
                if (stateIt == g_skinFrameDiagnostics.end() && g_skinFrameDiagnostics.size() < 5u)
                {
                    SkinFrameDiagnostic state;
                    state.firstFrame = g_nativeFrameNumber;
                    state.rawAtFirst = rawModel;
                    state.syncedAtFirst = model;
                    state.dirtyAtFirst = frameSyncRequested;
                    state.firstLogged = true;
                    stateIt = g_skinFrameDiagnostics.emplace(atomic, state).first;
                    std::fprintf(stderr,
                        "[p3fes-skin-frame] stage=first frame=%llu atomic=%08X object-frame=%08X dirty=%u "
                        "raw-pos=%.6g,%.6g,%.6g synced-pos=%.6g,%.6g,%.6g\n",
                        static_cast<unsigned long long>(g_nativeFrameNumber), atomic, summary.frame,
                        frameSyncRequested ? 1u : 0u, rawModel[12], rawModel[13], rawModel[14],
                        model[12], model[13], model[14]);
                }
                else if (stateIt != g_skinFrameDiagnostics.end() && !stateIt->second.followupLogged &&
                         g_nativeFrameNumber >= stateIt->second.firstFrame + 120u)
                {
                    stateIt->second.followupLogged = true;
                    std::fprintf(stderr,
                        "[p3fes-skin-frame] stage=followup frame=%llu first-frame=%llu atomic=%08X object-frame=%08X dirty=%u "
                        "first-raw=%.6g,%.6g,%.6g first-synced=%.6g,%.6g,%.6g "
                        "raw-pos=%.6g,%.6g,%.6g synced-pos=%.6g,%.6g,%.6g\n",
                        static_cast<unsigned long long>(g_nativeFrameNumber),
                        static_cast<unsigned long long>(stateIt->second.firstFrame), atomic, summary.frame,
                        frameSyncRequested ? 1u : 0u, stateIt->second.rawAtFirst[12],
                        stateIt->second.rawAtFirst[13], stateIt->second.rawAtFirst[14],
                        stateIt->second.syncedAtFirst[12], stateIt->second.syncedAtFirst[13],
                        stateIt->second.syncedAtFirst[14], rawModel[12], rawModel[13], rawModel[14],
                        model[12], model[13], model[14]);
                }
            }
        }
        MeshDrawState drawState = p3fes_native_present::acquireDrawState();
        readNativeMeshDrawState(rdram, summary.geometry, drawState);
        readNativeGlobalMeshState(rdram, drawState);
        if (blendOverride && blendOverride->enabled)
        {
            drawState.blendEnabled = true;
            const std::size_t materialCount = std::max<std::size_t>(drawState.materialColors.size(), 1u);
            drawState.materialBlends.assign(materialCount, *blendOverride);
        }
        if ((summary.geometryFlags & kGeometryLightingFlag) != 0u)
            readNativeLighting(rdram, false, atomic, drawState);
        if (!prepareNativeSkinPalette(rdram, atomic, summary.geometry, ctx, runtime, drawState, definedMesh))
        {
            if (g_nativeErrors++ < 8u)
                std::fprintf(stderr, "[p3fes-native] skin palette failed for atomic 0x%08X geometry 0x%08X\n",
                             atomic, summary.geometry);
        }
        if (diag)
        {
            auto found = g_skinDiagnosticSamples.find(meshId);
            Camera camera;
            if (found != g_skinDiagnosticSamples.end() && !found->second.emitted &&
                drawState.skinPalette.size() == found->second.boneCount &&
                readCurrentCamera(rdram, camera) && camera.useMatrices)
            {
                found->second.emitted = true;
                std::fprintf(stderr,
                    "[p3fes-skin-pose] mesh=%llX atomic=%08X palette=%u model-pos=%.6g,%.6g,%.6g "
                    "cull=%u alpha=%u/%u/%u depth=%u/%u material0-alpha=%u prelight=%u\n",
                    static_cast<unsigned long long>(meshId), atomic, static_cast<unsigned>(drawState.skinPalette.size()),
                    model[12], model[13], model[14], static_cast<unsigned>(drawState.cullMode),
                    drawState.alphaTest.enabled ? 1u : 0u, drawState.alphaTest.function, drawState.alphaTest.reference,
                    drawState.depthTest ? 1u : 0u, drawState.depthWrite ? 1u : 0u,
                    found->second.firstMaterialColor[3], found->second.hasPrelight ? 1u : 0u);
                for (unsigned j = 0u; j < 3u; ++j)
                {
                    const Vertex &source = found->second.vertices[j];
                    const Vertex posed = p3fes::native_renderer::poseSkinVertex(source, drawState.skinPalette);
                    const auto world = p3fes::native_renderer::skinTransformPoint(model, posed.position);
                    const auto view = p3fes::native_renderer::skinTransformPoint(camera.view, world);
                    const Matrix4 &p = camera.projection;
                    const float clipX = p[0] * view[0] + p[4] * view[1] + p[8] * view[2] + p[12];
                    const float clipY = p[1] * view[0] + p[5] * view[1] + p[9] * view[2] + p[13];
                    const float clipZ = p[2] * view[0] + p[6] * view[1] + p[10] * view[2] + p[14];
                    const float clipW = p[3] * view[0] + p[7] * view[1] + p[11] * view[2] + p[15];
                    std::fprintf(stderr,
                        "[p3fes-skin-vertex] sample=%u source=%.6g,%.6g,%.6g bones=%u,%u,%u,%u weights=%.9g,%.9g,%.9g,%.9g "
                        "posed=%.6g,%.6g,%.6g world=%.6g,%.6g,%.6g clip=%.6g,%.6g,%.6g,%.6g\n",
                        j, source.position[0], source.position[1], source.position[2],
                        source.bones[0], source.bones[1], source.bones[2], source.bones[3],
                        source.weights[0], source.weights[1], source.weights[2], source.weights[3],
                        posed.position[0], posed.position[1], posed.position[2], world[0], world[1], world[2],
                        clipX, clipY, clipZ, clipW);
                    if (source.bones[0] < drawState.skinPalette.size())
                    {
                        const auto &bone = drawState.skinPalette[source.bones[0]];
                        std::fprintf(stderr,
                            "[p3fes-skin-bone] bone=%u diag=%.6g,%.6g,%.6g translation=%.6g,%.6g,%.6g\n",
                            source.bones[0], bone[0], bone[5], bone[10], bone[12], bone[13], bone[14]);
                    }
                }
            }
        }
        // P3FES_MESH_DIAG=1: GS-side state of untextured, unlit atomics (masks, light volumes...).
        {
            static const bool meshDiag = [] { const char *v = std::getenv("P3FES_MESH_DIAG"); return v && *v == '1'; }();
            static unsigned meshDiagLines = 0u;
            if (meshDiag && meshDiagLines < 12u && !drawState.lightingEnabled && g_nativeFrameNumber > 600u)
            {
                bool textured = false;
                for (const auto &name : drawState.materialTextures)
                    textured |= !name.empty();
                if (!textured)
                {
                    ++meshDiagLines;
                    std::fprintf(stderr, "[p3fes-mesh-diag] mesh=%llX frame=%llu colors:", static_cast<unsigned long long>(meshId),
                                 static_cast<unsigned long long>(g_nativeFrameNumber));
                    for (const auto &c : drawState.materialColors)
                        std::fprintf(stderr, " %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
                    std::fprintf(stderr, " | gp words:");
                    for (int offset = -17104; offset <= -17008; offset += 8)
                    {
                        uint64_t word = 0u;
                        readGuest(rdram, static_cast<uint32_t>(static_cast<int>(kRwGp) + offset), word);
                        std::fprintf(stderr, " %d=%llX", offset, static_cast<unsigned long long>(word));
                    }
                    uint64_t flags = 0u;
                    readGuest(rdram, kRwRenderFlags, flags);
                    uint32_t pipeline = 0u, prelit0 = 0u, prelitN = 0u;
                    readGuest(rdram, atomic + 0x6Cu, pipeline);
                    if (summary.prelitData)
                    {
                        readGuest(rdram, summary.prelitData, prelit0);
                        readGuest(rdram, summary.prelitData + (summary.vertices - 1u) * 4u, prelitN);
                    }
                    std::fprintf(stderr, " flags=%llX pipeline=%08X gflags=%X verts=%u prelit=%08X first=%08X last=%08X atomic-flags=%X\n",
                                 static_cast<unsigned long long>(flags), pipeline, summary.geometryFlags, summary.vertices,
                                 summary.prelitData, prelit0, prelitN, summary.flags);
                }
            }
        }
        if (g_targetRaster && shadowDiag(rdram, "pass-mesh"))
        {
            std::fprintf(stderr, "[p3fes-shadow]   lit=%d ambient=%.3f,%.3f,%.3f colors:", drawState.lightingEnabled ? 1 : 0,
                         drawState.ambientLight[0], drawState.ambientLight[1], drawState.ambientLight[2]);
            for (const auto &c : drawState.materialColors)
                std::fprintf(stderr, " %u,%u,%u,%u", c[0], c[1], c[2], c[3]);
            std::fprintf(stderr, "\n");
        }
        p3fes_native_present::drawMesh(meshId, model, std::move(drawState));
        return true;
    }

    bool drawNativeSector(uint8_t *rdram, uint32_t sector)
    {
        if (g_gsColorMasked)
        {
            ++g_maskDropped[1];
            return true;
        }
        if (!ensureNativeFrame(rdram))
            return false;
        captureNativeDrawCamera(rdram);
        const MeshId meshId = (1ull << 63u) | sector;
        MeshDrawState drawState;
        if (g_definedMeshes.find(meshId) == g_definedMeshes.end())
        {
            // An empty leaf sector (no triangles, no geometry pointers) is a legal part of a world: the original
            // default callback (0x499B90) returns without drawing it too. Not a decode failure.
            uint16_t sectorTriangles = 1u;
            if (guestPointer(sector, kSectorNumTrianglesOffset + sizeof(uint16_t)) &&
                readGuest(rdram, sector + kSectorNumTrianglesOffset, sectorTriangles) && sectorTriangles == 0u)
                return true;
            MeshData mesh;
            MeshId decodedId = 0u;
            if (!decodeNativeSector(rdram, sector, mesh, decodedId, drawState) || decodedId != meshId)
            {
                if (g_nativeErrors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] sector 0x%08X decode failed\n", sector);
                return false;
            }
            p3fes_native_present::defineMesh(meshId, mesh);
            g_definedMeshes.emplace(meshId, DefinedMeshInfo{});
            if (!g_sectorDiagPrinted.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr, "[p3fes-native] first world sector: 0x%08X vertices=%zu triangles=%zu materials=%zu\n",
                             sector, mesh.vertices.size(), mesh.indices.size() / 3u, mesh.materials.size());
        }
        else
        {
            // Sector material colours can still be animated; rebuild only the
            // small live draw-state array, not the immutable geometry.
            uint32_t world = 0u, materialArray = 0u;
            int32_t worldMaterialCount = 0;
            uint16_t materialBase = 0u, triangleCount = 0u;
            uint32_t triangles = 0u;
            if (readGuest(rdram, kRwCurrentWorld, world) && guestPointer(world, kWorldNumMaterialsOffset + 4u) &&
                readGuest(rdram, world + kWorldMaterialsOffset, materialArray) &&
                readGuest(rdram, world + kWorldNumMaterialsOffset, worldMaterialCount) && worldMaterialCount > 0 &&
                readGuest(rdram, sector + kSectorMaterialBaseOffset, materialBase) && materialBase < worldMaterialCount &&
                readGuest(rdram, sector + kSectorNumTrianglesOffset, triangleCount) &&
                readGuest(rdram, sector + kSectorTrianglesOffset, triangles) &&
                guestPointer(triangles, static_cast<uint32_t>(triangleCount) * 8u))
            {
                uint32_t maxLocalMaterial = 0u;
                for (uint32_t i = 0u; i < triangleCount; ++i)
                {
                    uint16_t localMaterial = 0u;
                    readGuest(rdram, triangles + i * 8u + kTriangleMaterialOffset, localMaterial);
                    maxLocalMaterial = std::max(maxLocalMaterial, static_cast<uint32_t>(localMaterial));
                }
                const uint32_t available = static_cast<uint32_t>(worldMaterialCount) - materialBase;
                const uint32_t materialCount = std::min(maxLocalMaterial + 1u, available);
                drawState.materialColors.assign(materialCount, {255u, 255u, 255u, 255u});
                drawState.materialSamplers.resize(materialCount);
                drawState.materialTextures.resize(materialCount);
                drawState.materialUvTransforms.resize(materialCount);
                drawState.materialDualPasses.resize(materialCount);
                drawState.materialEnvPasses.resize(materialCount);
                for (uint32_t localMaterial = 0u; localMaterial < materialCount; ++localMaterial)
                {
                    uint32_t material = 0u;
                    const uint32_t worldMaterial = materialBase + localMaterial;
                    if (readGuest(rdram, materialArray + worldMaterial * 4u, material) && material && guestPointer(material, 8u))
                    {
                        std::memcpy(drawState.materialColors[localMaterial].data(),
                                    rdram + material + kMaterialColorOffset, 4u);
                        uint32_t texture = 0u;
                        if (readGuest(rdram, material + kMaterialTextureOffset, texture) && texture)
                        {
                            readTextureSampler(rdram, texture, drawState.materialSamplers[localMaterial]);
                            uint32_t raster = 0u;
                            if (readGuest(rdram, texture + kTextureRasterOffset, raster))
                                prepareNativeRasterTexture(rdram, raster, drawState.materialTextures[localMaterial]);
                        }
                        readMaterialUvTransform(rdram, material, drawState.materialUvTransforms[localMaterial]);
                        readMaterialDualPass(rdram, material, drawState.materialDualPasses[localMaterial]);
                        readMaterialEnvPass(rdram, material, drawState.materialEnvPasses[localMaterial]);
                    }
                }
            }
        }
        readNativeGlobalMeshState(rdram, drawState);
        readNativeLighting(rdram, true, sector, drawState);
        p3fes_native_present::drawMesh(meshId, p3fes::native_renderer::kIdentityMatrix, std::move(drawState));
        return true;
    }

    struct RwIm2DVertex
    {
        float x, y, z, cameraZ;
        float u, v, recipZ, pad;
        // P3FES supplies floating-point channels at the Sky Im2D entry point,
        // before the PS2 pipeline converts them for GIF. Unlike librw's public
        // PS2 vertex, these are not uint32 channels: e.g. blue UI (14,139,236)
        // is stored as 41600000,430B0000,436C0000. Treating the bits as integers
        // saturates every nonzero channel to white and destroys fade alpha.
        float r, g, b, a;
        uint32_t extra[4];
    };
    static_assert(sizeof(RwIm2DVertex) == 64u);

    uint32_t reg32(const R5900Context *ctx, uint32_t reg)
    {
        return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[reg]));
    }

    // 32-bit result sign-extended into the 64-bit GPR, as the recompiled code writes it.
    void setReg32(R5900Context *ctx, uint32_t reg, uint32_t value)
    {
        const int32_t v = static_cast<int32_t>(value);
        ctx->r[reg] = _mm_set_epi32(0, 0, v < 0 ? -1 : 0, v);
    }

    uint8_t colorByte(float value)
    {
        if (!std::isfinite(value))
            return 0u;
        // Same as lround(clamp(value, 0, 255)) (halves round up) without the libm call: the
        // fractional part of a value in [0, 255] is exact.
        const float clamped = std::clamp(value, 0.0f, 255.0f);
        const int whole = static_cast<int>(clamped);
        return static_cast<uint8_t>(whole + (clamped - static_cast<float>(whole) >= 0.5f ? 1 : 0));
    }

    bool readIm2DVertex(const uint8_t *rdram, uint32_t base, uint32_t index, ScreenVertex &out)
    {
        const uint64_t address = static_cast<uint64_t>(base) + static_cast<uint64_t>(index) * sizeof(RwIm2DVertex);
        if (address + sizeof(RwIm2DVertex) > 0x02000000ull)
            return false;
        RwIm2DVertex in{};
        std::memcpy(&in, rdram + static_cast<uint32_t>(address), sizeof(in));
        if (!std::isfinite(in.x) || !std::isfinite(in.y) || !std::isfinite(in.z))
            return false;
        out.x = in.x;
        out.y = in.y;
        out.z = in.z;
        out.u = std::isfinite(in.u) ? in.u : 0.0f;
        out.v = std::isfinite(in.v) ? in.v : 0.0f;
        out.color = {colorByte(in.r), colorByte(in.g), colorByte(in.b), colorByte(in.a)};
        return true;
    }

    bool nativeIm2DTextured(const uint8_t *rdram)
    {
        uint64_t flags = 0u;
        return readGuest(rdram, kRwRenderFlags, flags) && (flags & kGsPrimTextureEnable) != 0u;
    }

    ScreenDrawState nativeIm2DState(const uint8_t *rdram)
    {
#if defined(PS2X_P3FES_FRAME_TRACE)
        p3fes_frame_trace::NativeScope traceScope(p3fes_frame_trace::Stage::StateDecode);
#endif
        ScreenDrawState state;
        uint64_t flags = 0u, alpha = 0u, clamp = 0u, tex1 = 0u, test = 0u, zbuf = 0u;
        readGuest(rdram, kRwRenderFlags, flags);
        readGuest(rdram, kRwAlpha1, alpha);
        readGuest(rdram, kRwClamp1, clamp);
        readGuest(rdram, kRwTex1, tex1);
        state.gouraudShading = (flags & kGsPrimGouraud) != 0u;
        if (readGuest(rdram, kRwTest1, test))
        {
            state.alphaTest.enabled = (test & 0x1u) != 0u;
            state.alphaTest.function = static_cast<uint8_t>((test >> 1u) & 0x7u);
            state.alphaTest.reference = static_cast<uint8_t>((test >> 4u) & 0xFFu);
            state.depthTest = (test & (1u << 16u)) != 0u;
            state.depthFunction = static_cast<p3fes::native_renderer::ScreenDepthFunction>((test >> 17u) & 3u);
            state.alphaFailAction = static_cast<p3fes::native_renderer::ScreenAlphaFailAction>((test >> 12u) & 3u);
            state.destinationAlphaTest = ((test >> 14u) & 3u) == 3u; // DATE with DATM = 1
        }
        if (readGuest(rdram, kRwZbuf1, zbuf)) state.depthWrite = ((zbuf >> 32u) & 1u) == 0u;

        if ((flags & kGsPrimAlphaBlendEnable) == 0u)
            state.blend = ScreenBlend::Opaque;
        else
        {
            switch (alpha & 0xFFu)
            {
            case 0x48u: state.blend = ScreenBlend::Additive; break;   // Cs * As + Cd
            case 0x42u: state.blend = ScreenBlend::Subtractive; break; // Cd - Cs * As
            case 0x6Au: state.blend = ScreenBlend::DestinationOnly; break; // Cd
            case 0x58u: state.blend = ScreenBlend::DestinationAlphaAdditive; break; // Cs * Ad + Cd
            default: state.blend = ScreenBlend::Alpha; break;
            }
        }
        const uint32_t wms = static_cast<uint32_t>(clamp & 0x3u);
        const uint32_t wmt = static_cast<uint32_t>((clamp >> 2u) & 0x3u);
        state.clampU = wms == 1u || wms == 2u;
        state.clampV = wmt == 1u || wmt == 2u;
        state.linearFilter = (tex1 & (1ull << 5u)) != 0u; // GS TEX1.MMAG
        readNativeFogState(rdram, flags, state.fog);
        return state;
    }

    struct RasterSummary
    {
        uint32_t raster = 0u;
        uint32_t pixels = 0u;
        uint32_t palette = 0u;
        int32_t width = 0;
        int32_t height = 0;
        int32_t depth = 0;
        int32_t stride = 0;
        uint8_t type = 0u;
        uint8_t cache = 0u;
        uint8_t lock = 0u;
        uint8_t format = 0u;
    };

    bool inspectRaster(const uint8_t *rdram, uint32_t raster, RasterSummary &summary)
    {
        if (!guestPointer(raster, 52u))
            return false;
        summary.raster = raster;
        return readGuest(rdram, raster + 4u, summary.pixels) &&
               readGuest(rdram, raster + 8u, summary.palette) &&
               readGuest(rdram, raster + 12u, summary.width) &&
               readGuest(rdram, raster + 16u, summary.height) &&
               readGuest(rdram, raster + 20u, summary.depth) &&
               readGuest(rdram, raster + 24u, summary.stride) &&
               readGuest(rdram, raster + 32u, summary.type) &&
               readGuest(rdram, raster + 33u, summary.cache) &&
               readGuest(rdram, raster + 34u, summary.lock) &&
               readGuest(rdram, raster + 35u, summary.format);
    }

    bool isCameraTextureRaster(const uint8_t *rdram, uint32_t raster)
    {
        // Same range test as inspectRaster, which then reads the type byte at +32.
        return guestPointer(raster, 52u) && (rdram[raster + 32u] & 0x07u) == 5u;
    }

    std::string rasterTextureName(uint32_t raster)
    {
        char name[24];
        std::snprintf(name, sizeof(name), "rw-raster-%08X", raster);
        return name;
    }

    std::array<uint8_t, 4> decodeRasterColor16(uint16_t value, uint8_t format)
    {
        switch (format & 0x0Fu)
        {
        case 2u: // rwRASTERFORMAT565
            return {static_cast<uint8_t>(((value >> 0u) & 31u) * 255u / 31u),
                    static_cast<uint8_t>(((value >> 5u) & 63u) * 255u / 63u),
                    static_cast<uint8_t>(((value >> 11u) & 31u) * 255u / 31u), 255u};
        case 3u: // rwRASTERFORMAT4444
            return {static_cast<uint8_t>(((value >> 0u) & 15u) * 17u),
                    static_cast<uint8_t>(((value >> 4u) & 15u) * 17u),
                    static_cast<uint8_t>(((value >> 8u) & 15u) * 17u),
                    static_cast<uint8_t>(((value >> 12u) & 15u) * 17u)};
        default: // rwRASTERFORMAT1555 (the title's ordinary 16-bit direct colour)
            return {static_cast<uint8_t>(((value >> 0u) & 31u) * 255u / 31u),
                    static_cast<uint8_t>(((value >> 5u) & 31u) * 255u / 31u),
                    static_cast<uint8_t>(((value >> 10u) & 31u) * 255u / 31u),
                    static_cast<uint8_t>((value & 0x8000u) ? 255u : 0u)};
        }
    }

    bool decodeGsResidentRaster(PS2Runtime *runtime, const uint8_t *rdram, uint32_t raster,
                                TextureData &texture)
    {
        if (!runtime)
            return false;

        RasterSummary info;
        if (!inspectRaster(rdram, raster, info) || info.width <= 0 || info.height <= 0 ||
            info.width > 4096 || info.height > 4096)
            return false;
        const uint32_t type = info.type & 0x07u;
        if ((type != 4u && type != 5u) || info.pixels != 0u)
            return false;

        uint32_t pluginOffset = 0u, tex0 = 0u;
        if (!readGuest(rdram, kRwRasterPluginOffset, pluginOffset) || !pluginOffset ||
            !guestPointer(raster + pluginOffset, 12u) ||
            !readGuest(rdram, raster + pluginOffset + 8u, tex0))
            return false;

        const uint32_t tbp0 = tex0 & 0x3FFFu;
        const uint32_t tbw = (tex0 >> 14u) & 0x3Fu;
        const uint32_t psm = (tex0 >> 20u) & 0x3Fu;
        if (tbw == 0u || (psm != GS_PSM_CT32 && psm != GS_PSM_CT24 &&
                          psm != GS_PSM_CT16 && psm != GS_PSM_CT16S))
            return false;

        // Sofdec's RenderWare raster is GPU-resident: its public pixels/stride
        // fields stay null while the sky raster plugin keeps the GS TEX0. Reuse
        // the GS backend's synchronized snapshot and the runtime's existing GS
        // swizzle readers instead of adding a second MPEG/video path.
#if defined(PLATFORM_VITA)
        // Vita uses the synchronous CPU GS backend by default, so its external
        // 4 MiB store is already authoritative here on the EE thread. Reading
        // it in place avoids copying all of VRAM for every movie frame.
        const uint8_t *vram = runtime->memory().getGSVRAM();
        if (!vram)
            return false;
#else
        runtime->gs().refreshDisplaySnapshot();
        uint32_t vramSize = 0u;
        const uint8_t *vram = runtime->gs().lockDisplaySnapshot(vramSize);
        if (!vram || vramSize < GSMem::MEMORY_SIZE)
        {
            runtime->gs().unlockDisplaySnapshot();
            return false;
        }
#endif

        texture.name = rasterTextureName(raster);
        texture.width = static_cast<uint32_t>(info.width);
        texture.height = static_cast<uint32_t>(info.height);
        texture.rgba.resize(static_cast<size_t>(info.width) * static_cast<size_t>(info.height) * 4u);
        uint8_t *mutableVram = const_cast<uint8_t *>(vram);
        for (int32_t y = 0; y < info.height; ++y)
        {
            for (int32_t x = 0; x < info.width; ++x)
            {
                uint32_t packed = 0u;
                switch (psm)
                {
                case GS_PSM_CT32:
                    packed = GSMem::ReadCT32(mutableVram, tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    break;
                case GS_PSM_CT24:
                    packed = GSMem::ReadCT24(mutableVram, tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    break;
                case GS_PSM_CT16:
                    packed = GSMem::ReadCT16(mutableVram, tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    break;
                case GS_PSM_CT16S:
                    packed = GSMem::ReadCT16S(mutableVram, tbp0, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                    break;
                }

                std::array<uint8_t, 4> color{};
                if (psm == GS_PSM_CT32)
                {
                    color = {static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8u),
                             static_cast<uint8_t>(packed >> 16u),
                             static_cast<uint8_t>(std::min(255u, ((packed >> 24u) & 0xFFu) * 2u))};
                }
                else if (psm == GS_PSM_CT24)
                    color = {static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8u),
                             static_cast<uint8_t>(packed >> 16u), 255u};
                else
                    color = decodeRasterColor16(static_cast<uint16_t>(packed), info.format);
                std::memcpy(texture.rgba.data() +
                                (static_cast<size_t>(y) * static_cast<size_t>(info.width) + static_cast<size_t>(x)) * 4u,
                            color.data(), 4u);
            }
        }
#if !defined(PLATFORM_VITA)
        runtime->gs().unlockDisplaySnapshot();
#endif
        return true;
    }

    bool queueSofdecRaster(uint8_t *rdram, PS2Runtime *runtime, uint32_t raster)
    {
        ps2_perf::Scope profile(ps2_perf::Bucket::Gs);
        static TextureData texture;
        static uint64_t revision = 0u;
        if (!decodeGsResidentRaster(runtime, rdram, raster, texture))
            return false;
        ++revision;
        if (revision == 0u)
            revision = 1u;

#if defined(PLATFORM_VITA)
        return ps2x_vita_present_software_frame_rgba(texture.rgba.data(), texture.width, texture.height) >= 0;
#else

        if (!g_nativeFrameActive)
        {
            Camera camera;
            if (!readCurrentCamera(rdram, camera))
                camera = {};
            p3fes_native_present::beginFrame(camera, ClearColor{});
            g_nativeFrameActive = true;
        }
        g_frameHasDisplayContent = true;

        const std::string textureName = texture.name;
        p3fes_native_present::defineTexture(texture, revision);

        std::vector<ScreenVertex> vertices(6u);
        const float width = static_cast<float>(texture.width);
        const float height = static_cast<float>(texture.height);
        const std::array<uint8_t, 4> white{255u, 255u, 255u, 255u};
        vertices[0] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, white};
        vertices[1] = {width, 0.0f, 0.0f, 1.0f, 0.0f, white};
        vertices[2] = {width, height, 0.0f, 1.0f, 1.0f, white};
        vertices[3] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, white};
        vertices[4] = {width, height, 0.0f, 1.0f, 1.0f, white};
        vertices[5] = {0.0f, height, 0.0f, 0.0f, 1.0f, white};
        ScreenDrawState state;
        state.blend = ScreenBlend::Opaque;
        state.clampU = true;
        state.clampV = true;
        state.linearFilter = true;
        p3fes_native_present::drawScreenTriangles(std::move(vertices), width, height, textureName, state);
        return true;
#endif
    }

    // RenderWare's PS2 driver does not store PAL4/PAL8 CPU backing data in GS
    // local-memory page order. Its SWIZZLED4/SWIZZLED8 representation uses this
    // four-row permutation so the upload can be submitted as PSMCT16/PSMCT32
    // at half dimensions. Adapted from aap/librw src/ps2/ps2raster.cpp
    // (unswizzleRaster, commit 18532c2e13efbc1aa43f0b00b4459831f9414de0).
    uint32_t rwPs2SwizzledIndex(uint32_t x, uint32_t y, uint32_t logWidth)
    {
        const uint32_t y1 = (y >> 1u) & 1u, y2 = (y >> 2u) & 1u;
        x ^= (y1 ^ y2) << 2u;
        const uint32_t nx = (x & 7u) | ((x >> 1u) & ~7u);
        const uint32_t ny = (y & 1u) | ((y >> 1u) & ~1u);
        return (y1 | (((x >> 3u) & 1u) << 1u)) | (nx << 2u) | (ny << (logWidth + 1u));
    }

    bool decodeNativeRaster(const uint8_t *rdram, uint32_t raster, TextureData &texture)
    {
        RasterSummary info;
        if (!inspectRaster(rdram, raster, info) || info.width <= 0 || info.height <= 0 ||
            info.width > 4096 || info.height > 4096 || info.stride <= 0)
            return false;

        // Type 5 (camera texture) lives in GS memory; its guest RAM backing is
        // never written, so decoding it would bind junk. Plain textures only.
        const uint32_t type = info.type & 0x07u;
        if (type != 4u)
            return false;

        uint32_t pluginOffset = 0u;
        uint8_t cacheFlags = 0u;
        if (readGuest(rdram, kRwRasterPluginOffset, pluginOffset) && pluginOffset &&
            guestPointer(raster + pluginOffset, 55u))
        {
            readGuest(rdram, raster + pluginOffset + 54u, cacheFlags);
        }
        const bool swizzled8 = (cacheFlags & 0x02u) != 0u;
        const bool swizzled4 = (cacheFlags & 0x04u) != 0u;
        if ((swizzled8 && info.depth != 8) || (swizzled4 && info.depth != 4) || (swizzled8 && swizzled4))
            return false;

        size_t rowBytes = 0u;
        switch (info.depth)
        {
        case 4: rowBytes = (static_cast<size_t>(info.width) + 1u) / 2u; break;
        case 8: rowBytes = static_cast<size_t>(info.width); break;
        case 16: rowBytes = static_cast<size_t>(info.width) * 2u; break;
        case 24: rowBytes = static_cast<size_t>(info.width) * 3u; break;
        case 32: rowBytes = static_cast<size_t>(info.width) * 4u; break;
        default: return false;
        }
        if (!guestPointer(info.pixels, 1u))
            return false;
        if (!swizzled8 && !swizzled4)
        {
            if (static_cast<size_t>(info.stride) < rowBytes)
                return false;
            const uint64_t pixelEnd = static_cast<uint64_t>(info.pixels) +
                                      static_cast<uint64_t>(info.stride) * static_cast<uint64_t>(info.height - 1) + rowBytes;
            if (pixelEnd > PS2_RAM_SIZE)
                return false;
        }

        const uint32_t paletteEntries = info.depth == 4 ? 16u : (info.depth == 8 ? 256u : 0u);
        const uint32_t paletteEntryBytes = (info.format & 0x0Fu) == 1u ? 2u : 4u;
        if (paletteEntries && !guestPointer(info.palette, paletteEntries * paletteEntryBytes))
            return false;

        texture.name = rasterTextureName(raster);
        texture.width = static_cast<uint32_t>(info.width);
        texture.height = static_cast<uint32_t>(info.height);
        texture.rgba.resize(static_cast<size_t>(info.width) * static_cast<size_t>(info.height) * 4u);

        uint32_t swizzledWidth = static_cast<uint32_t>(info.width);
        if (swizzled8)
            swizzledWidth = std::max(swizzledWidth, 16u);
        else if (swizzled4)
            swizzledWidth = std::max(swizzledWidth, 32u);
        uint32_t swizzledLogWidth = 0u;
        for (uint32_t w = 1u; w < swizzledWidth; w <<= 1u)
            ++swizzledLogWidth;
        const uint32_t swizzledMask = (1u << (swizzledLogWidth + 2u)) - 1u;
        for (int32_t y = 0; y < info.height; ++y)
        {
            const uint64_t rowOffset = static_cast<uint64_t>(static_cast<uint32_t>(y)) *
                                       static_cast<uint32_t>(info.stride);
            const uint8_t *src = (!swizzled8 && !swizzled4)
                ? rdram + info.pixels + static_cast<uint32_t>(rowOffset)
                : nullptr;
            for (int32_t x = 0; x < info.width; ++x)
            {
                std::array<uint8_t, 4> color{};
                if (info.depth == 4 || info.depth == 8)
                {
                    uint32_t index = 0u;
                    if (swizzled8 || swizzled4)
                    {
                        const uint32_t groupY = static_cast<uint32_t>(y) & ~3u;
                        const uint32_t swizzled = rwPs2SwizzledIndex(static_cast<uint32_t>(x),
                                                                     static_cast<uint32_t>(y),
                                                                     swizzledLogWidth) & swizzledMask;
                        const uint64_t address = static_cast<uint64_t>(info.pixels) +
                                                 (swizzled8
                                                      ? static_cast<uint64_t>(groupY) * swizzledWidth + swizzled
                                                      : (static_cast<uint64_t>(groupY) * swizzledWidth + swizzled) / 2u);
                        if (address >= PS2_RAM_SIZE)
                            return false;
                        const uint8_t packed = rdram[static_cast<uint32_t>(address)];
                        index = swizzled8 ? packed
                                          : ((swizzled & 1u) ? (packed >> 4u) : (packed & 0x0Fu));
                    }
                    else
                        index = info.depth == 8 ? src[x]
                            : ((x & 1) ? (src[x >> 1] >> 4u) : (src[x >> 1] & 0x0Fu));
                    // PAL8 palettes are stored in CSM1 order by the PS2 driver;
                    // PAL4 palettes stay linear. This is the same 0x08/0x10
                    // bit exchange used by RenderWare's convertPalette().
                    if (info.depth == 8)
                        index = (index & ~0x18u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
                    if (paletteEntryBytes == 2u)
                    {
                        uint16_t packed = 0u;
                        std::memcpy(&packed, rdram + info.palette + index * 2u, sizeof(packed));
                        color = decodeRasterColor16(packed, 1u);
                    }
                    else
                    {
                        std::memcpy(color.data(), rdram + info.palette + index * 4u, 4u);
                        // skyRasterWritePalette stores GS alpha (0..0x80), not
                        // RenderWare's host alpha (0..0xFF).
                        color[3] = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(color[3]) * 2u));
                    }
                }
                else if (info.depth == 16)
                {
                    uint16_t packed = 0u;
                    std::memcpy(&packed, src + x * 2, sizeof(packed));
                    color = decodeRasterColor16(packed, info.format);
                }
                else if (info.depth == 24)
                    color = {src[x * 3u + 0u], src[x * 3u + 1u], src[x * 3u + 2u], 255u};
                else
                {
                    std::memcpy(color.data(), src + x * 4u, 4u);
                    // Direct RGBA8888 rasters are stored in GS colour form too.
                    color[3] = static_cast<uint8_t>(std::min(255u, static_cast<uint32_t>(color[3]) * 2u));
                }
                std::memcpy(texture.rgba.data() + (static_cast<size_t>(y) * info.width + x) * 4u,
                            color.data(), 4u);
            }
        }
        static const bool textureDiag = [] {
            const char *value = std::getenv("P3FES_TEXTURE_DIAG");
            return value && *value == '1';
        }();
        if (textureDiag)
        {
            unsigned lo = 255u, hi = 0u;
            for (size_t i = 3; i < texture.rgba.size(); i += 4)
            {
                lo = std::min<unsigned>(lo, texture.rgba[i]);
                hi = std::max<unsigned>(hi, texture.rgba[i]);
            }
            uint32_t rasterFormat = 0u;
            readGuest(rdram, raster + 32u, rasterFormat);
            uint32_t diagPluginOffset = 0u;
            uint64_t diagTex0 = 0u;
            if (readGuest(rdram, kRwRasterPluginOffset, diagPluginOffset))
                readGuest(rdram, raster + diagPluginOffset + 8u, diagTex0);
            std::fprintf(stderr, "[p3fes-texture] raster=%X fmt=%02X word32=%08X %dx%d depth=%d alpha=%u..%u tcc=%u tfx=%u tex0=%llX\n", raster,
                         info.format, rasterFormat, info.width, info.height, info.depth, lo, hi,
                         static_cast<unsigned>((diagTex0 >> 34u) & 1u), static_cast<unsigned>((diagTex0 >> 35u) & 3u),
                         static_cast<unsigned long long>(diagTex0));
        }
        if (!g_im2dTextureDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[p3fes-native] first native texture: raster=0x%08X type=%u fmt=0x%02X %dx%d depth=%d swizzle=%u\n",
                         raster, type, info.format, info.width, info.height, info.depth,
                         swizzled8 ? 8u : (swizzled4 ? 4u : 0u));
        return true;
    }

    void retireNativeRaster(uint32_t raster, uint64_t generation)
    {
        p3fes_native_texture_access::forgetRaster(raster);
        const auto found = g_nativeRasterResources.find(raster);
        if (found == g_nativeRasterResources.end() || found->second.version.generation != generation)
            return;
        p3fes_native_present::retireTexture(found->second.textureName);
        g_textureMinAlpha.erase(found->second.textureName);
        g_nativeRasterResources.erase(found);
    }

    bool prepareNativeRasterTexture(const uint8_t *rdram, uint32_t raster, std::string &textureName)
    {
        if (!raster) return false;
        const uint64_t lifecycleEpoch = p3fes_native_rasters::epoch();
        RasterNameCacheSlot &cached = g_rasterNameCache[(raster >> 4u) & 127u];
        if (cached.raster == raster && cached.epoch == lifecycleEpoch && cached.name)
        {
            textureName = *cached.name;
            return true;
        }
        // An empty texture name means "untextured, vertex colour only", which
        // would paint solid blobs where the GS-resident shadow map should be
        // sampled. Bind a fully transparent 1x1 texture instead.
        if (isCameraTextureRaster(rdram, raster) && cameraTexturesEnabled())
        {
            // The pixels come from the raster's render-target pass. The definition below is only the
            // name's placeholder (fully transparent, shown until the first pass has run).
            TextureData placeholder;
            placeholder.name = cameraTextureName(raster);
            placeholder.width = 1u;
            placeholder.height = 1u;
            placeholder.rgba.assign(4u, 0u);
            p3fes_native_present::defineTexture(placeholder, 1u);
            textureName = std::move(placeholder.name);
            return true;
        }
        if (isCameraTextureRaster(rdram, raster))
        {
            static TextureData transparent = [] {
                TextureData t;
                t.name = "rw-camera-texture-transparent";
                t.width = 1u;
                t.height = 1u;
                t.rgba.assign(4u, 0u);
                return t;
            }();
            // defineTexture is a no-op until the present layer is active and
            // is idempotent for an unchanged revision, so call it every time.
            p3fes_native_present::defineTexture(transparent, 1u);
            textureName = transparent.name;
            return true;
        }
        const auto version = p3fes_native_rasters::version(raster);
        const auto found = g_nativeRasterResources.find(raster);
        if (found != g_nativeRasterResources.end() &&
            found->second.version.generation == version.generation && found->second.version.revision == version.revision)
        {
            textureName = found->second.textureName;
            cached = {raster, lifecycleEpoch, &found->second.textureName};
            return true;
        }

        // Resource preparation happens once at the first bind after creation
        // or an explicit image/palette update. Draws only look up its version;
        // they never inspect or hash the texture's source bytes.
        TextureData texture;
        if (!decodeNativeRaster(rdram, raster, texture)) return false;
        if (found != g_nativeRasterResources.end())
        {
            p3fes_native_present::retireTexture(found->second.textureName);
            g_textureMinAlpha.erase(found->second.textureName);
        }
        // Short, so the name fits the std::string small buffer (15 chars on libstdc++): every draw
        // copies these names and a heap allocation per copy is costly on the Vita's CPU. The raster
        // address (< 32 MiB), generation and revision keep it unique.
        char name[40];
        std::snprintf(name, sizeof(name), "%X.%llX.%llX", raster,
                      static_cast<unsigned long long>(version.generation),
                      static_cast<unsigned long long>(version.revision));
        texture.name = name;
        textureName = texture.name;
        static const bool dumpTextures = [] {
            const char *value = std::getenv("P3FES_TEXTURE_DUMP");
            return value && *value == '1';
        }();
        if (dumpTextures)
        {
            char dumpName[80];
            std::snprintf(dumpName, sizeof(dumpName), "rw-raster-%08X-g%llu-r%llu", raster,
                          static_cast<unsigned long long>(version.generation),
                          static_cast<unsigned long long>(version.revision));
            char path[192];
#if defined(PLATFORM_VITA)
            std::snprintf(path, sizeof(path), "ux0:data/p3fes/texture-dump/%s.rgba", dumpName);
#else
            std::snprintf(path, sizeof(path), "texture-dump/%s.rgba", dumpName);
#endif
            if (FILE *file = std::fopen(path, "wb"))
            {
                std::fwrite(texture.rgba.data(), 1, texture.rgba.size(), file);
                std::fclose(file);
                std::fprintf(stderr, "[p3fes-texture-data] %s %ux%u\n", dumpName, texture.width, texture.height);
            }
        }
        {
            uint8_t lowest = 255u;
            for (size_t i = 3u; i < texture.rgba.size() && lowest != 0u; i += 4u)
                lowest = std::min(lowest, texture.rgba[i]);
            g_textureMinAlpha[textureName] = lowest;
        }
        p3fes_native_present::defineTexture(texture, version.revision);
        NativeRasterResource &resource = g_nativeRasterResources[raster];
        resource = {version, textureName};
        cached = {raster, lifecycleEpoch, &resource.textureName};
        ++g_texturePrepares;
        return true;
    }

    bool prepareNativeIm2DTexture(const uint8_t *rdram, std::string &textureName)
    {
#if defined(PS2X_P3FES_FRAME_TRACE)
        p3fes_frame_trace::NativeScope traceScope(p3fes_frame_trace::Stage::TextureResolve);
#endif
        uint32_t raster = 0u;
        if (!readGuest(rdram, kRwCurrentRaster, raster) || !raster)
            return false;
        return prepareNativeRasterTexture(rdram, raster, textureName);
    }

    bool drawNativeIm2DTriangles(uint8_t *rdram, uint32_t vertices, uint32_t vertexCount,
                                 const std::vector<uint16_t> &triangles)
    {
        if (g_gsColorMasked)
        {
            ++g_maskDropped[2];
            return true;
        }
        if (vertexCount == 0u || vertexCount > 65535u || triangles.empty())
            return false;
        std::string textureName;
        if (nativeIm2DTextured(rdram) && !prepareNativeIm2DTexture(rdram, textureName))
            return false;
        const auto drawState = nativeIm2DState(rdram);
        uint64_t zbuf = 0;
        readGuest(rdram, kRwZbuf1, zbuf);
        const float inverseDepth = p3fes_im2d::inverseDepthRange(zbuf);
        std::vector<ScreenVertex> out;
        out.reserve(triangles.size());
        // P3FES_DEBUG_GLOW_ALPHA=<0..255>: pin the vertex alpha of additive, never-passing alpha-test draws
        // (the date HUD's sun glow pulses; this makes frames comparable).
        static const int debugGlowAlpha = [] { const char *v = std::getenv("P3FES_DEBUG_GLOW_ALPHA"); return v ? std::atoi(v) : -1; }();
        const bool pinGlowAlpha = debugGlowAlpha >= 0 && drawState.blend == ScreenBlend::Additive &&
                                  drawState.alphaTest.enabled && drawState.alphaTest.function == 0u;
        {
#if defined(PS2X_P3FES_FRAME_TRACE)
            p3fes_frame_trace::NativeScope vertexScope(p3fes_frame_trace::Stage::VertexDecode);
#endif
            for (uint16_t index : triangles)
            {
                if (index >= vertexCount)
                    return false;
                ScreenVertex v;
                if (!readIm2DVertex(rdram, vertices, index, v))
                    return false;
                v.z = p3fes_im2d::normalizedDepth(v.z, inverseDepth);
                if (pinGlowAlpha)
                    v.color[3] = static_cast<uint8_t>(std::min(debugGlowAlpha, 255));
                out.push_back(v);
            }
        }
        if (!ensureNativeFrame(rdram))
            return false;
        // P3FES_SCREEN_STATE_DIAG=1: raw GS state words of Im2D draws that touch the date HUD's sun/moon
        // icon (640x448 x 575..640, y 50..110).
        {
            static const bool screenStateDiag = [] { const char *v = std::getenv("P3FES_SCREEN_STATE_DIAG"); return v && *v == '1'; }();
            static unsigned screenStateLogs = 0u;
            if (screenStateDiag && screenStateLogs < 2000000u)
            {
                float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
                for (const ScreenVertex &v : out)
                {
                    minX = std::min(minX, v.x); maxX = std::max(maxX, v.x);
                    minY = std::min(minY, v.y); maxY = std::max(maxY, v.y);
                }
                if (maxX > 575.0f && minX < 640.0f && maxY > 50.0f && minY < 110.0f)
                {
                    uint64_t flags = 0u, alpha = 0u, test = 0u, tex1 = 0u, clamp = 0u;
                    readGuest(rdram, kRwRenderFlags, flags);
                    readGuest(rdram, kRwAlpha1, alpha);
                    readGuest(rdram, kRwTest1, test);
                    readGuest(rdram, kRwTex1, tex1);
                    readGuest(rdram, kRwClamp1, clamp);
                    uint32_t srcBlend = 0u, dstBlend = 0u;
                    readGuest(rdram, kRwSrcBlend, srcBlend);
                    readGuest(rdram, kRwDstBlend, dstBlend);
                    const ScreenVertex &v0 = out.front();
                    std::fprintf(stderr,
                                 "[p3fes-screen-state] #%u tex=%s xy=%.0f,%.0f..%.0f,%.0f flags=%llX ALPHA_1=%016llX TEST_1=%016llX ZBUF_1=%016llX TEX1_1=%llX CLAMP_1=%llX rwBlend=%u/%u rgba=%u,%u,%u,%u\n",
                                 screenStateLogs++, textureName.c_str(), static_cast<double>(minX), static_cast<double>(minY),
                                 static_cast<double>(maxX), static_cast<double>(maxY),
                                 static_cast<unsigned long long>(flags), static_cast<unsigned long long>(alpha),
                                 static_cast<unsigned long long>(test), static_cast<unsigned long long>(zbuf),
                                 static_cast<unsigned long long>(tex1), static_cast<unsigned long long>(clamp),
                                 srcBlend, dstBlend, v0.color[0], v0.color[1], v0.color[2], v0.color[3]);
                }
            }
        }
        if (g_diagEnabled.load(std::memory_order_relaxed))
        {
            static std::unordered_set<std::string> seen;
            static unsigned largeUntextured = 0;
            const bool traceFan = textureName.empty() && out.size() > 12u && largeUntextured++ < 8u;
            if ((seen.size() < 256u && seen.insert(textureName).second) || traceFan)
            {
                uint64_t flags = 0u, alpha = 0u, test = 0u, tex0 = 0u;
                readGuest(rdram, kRwRenderFlags, flags);
                readGuest(rdram, kRwAlpha1, alpha);
                readGuest(rdram, kRwTest1, test);
                uint32_t raster = 0u, pluginOffset = 0u;
                if (readGuest(rdram, kRwCurrentRaster, raster) && raster &&
                    readGuest(rdram, kRwRasterPluginOffset, pluginOffset) && pluginOffset)
                    readGuest(rdram, raster + pluginOffset + 8u, tex0);
                const auto &v = out.front();
                RwIm2DVertex raw{};
                std::memcpy(&raw, rdram + vertices + static_cast<uint32_t>(triangles.front()) * sizeof(raw), sizeof(raw));
                std::fprintf(stderr, "[p3fes-rw] Im2D texture=%s flags=%llX alpha=%llX rgba=%u,%u,%u,%u uv=%.3f,%.3f\n",
                             textureName.c_str(), static_cast<unsigned long long>(flags),
                             static_cast<unsigned long long>(alpha),
                             v.color[0], v.color[1], v.color[2], v.color[3], v.u, v.v);
                std::fprintf(stderr, "[p3fes-rw] Im2D source rgba=%.2f,%.2f,%.2f,%.2f\n", raw.r, raw.g, raw.b, raw.a);
                uint32_t colorBits[4];
                std::memcpy(colorBits, &raw.r, sizeof(colorBits));
                std::fprintf(stderr, "[p3fes-rw] Im2D color bits=%08X,%08X,%08X,%08X\n",
                             colorBits[0], colorBits[1], colorBits[2], colorBits[3]);
                float minZ = out.front().z, maxZ = minZ;
                for (const auto &vertex : out) { minZ = std::min(minZ, vertex.z); maxZ = std::max(maxZ, vertex.z); }
                std::fprintf(stderr, "[p3fes-rw] Im2D vertices=%u z=%.6f..%.6f TEST=%llX TEX0=%llX TFX=%u TCC=%u\n",
                    static_cast<unsigned>(out.size()), minZ, maxZ,
                    static_cast<unsigned long long>(test), static_cast<unsigned long long>(tex0),
                    static_cast<unsigned>((tex0 >> 35u) & 3u), static_cast<unsigned>((tex0 >> 34u) & 1u));
            }
        }
        if (g_targetRaster && shadowDiag(rdram, "pass-im2d"))
            for (const ScreenVertex &v : out)
                std::fprintf(stderr, "[p3fes-shadow]   xy=%.1f,%.1f z=%.4f rgba=%u,%u,%u,%u\n", v.x, v.y, v.z, v.color[0],
                             v.color[1], v.color[2], v.color[3]);
        p3fes_native_present::drawScreenTriangles(std::move(out), kRwScreenWidth, kRwScreenHeight,
                                                  std::move(textureName), drawState);
        return true;
    }

    bool drawNativeIm2DLines(uint8_t *rdram, uint32_t vertices, uint32_t vertexCount,
                             const std::vector<uint16_t> &lines)
    {
        if (g_gsColorMasked)
            return true;
        if (vertexCount == 0u || vertexCount > 65535u || lines.empty())
            return false;
        std::string textureName;
        if (nativeIm2DTextured(rdram) && !prepareNativeIm2DTexture(rdram, textureName))
            return false;
        uint64_t lineZbuf = 0;
        readGuest(rdram, kRwZbuf1, lineZbuf);
        const float lineInverseDepth = p3fes_im2d::inverseDepthRange(lineZbuf);
        std::vector<ScreenVertex> out;
        out.reserve(lines.size());
        {
#if defined(PS2X_P3FES_FRAME_TRACE)
            p3fes_frame_trace::NativeScope vertexScope(p3fes_frame_trace::Stage::VertexDecode);
#endif
            for (uint16_t index : lines)
            {
                if (index >= vertexCount)
                    return false;
                ScreenVertex v;
                if (!readIm2DVertex(rdram, vertices, index, v))
                    return false;
                v.z = p3fes_im2d::normalizedDepth(v.z, lineInverseDepth);
                out.push_back(v);
            }
        }
        if (!ensureNativeFrame(rdram))
            return false;
        p3fes_native_present::drawScreenLines(std::move(out), kRwScreenWidth, kRwScreenHeight,
                                             std::move(textureName), nativeIm2DState(rdram));
        return true;
    }

    // The expanded index lists live in scratch storage reused across calls (the game thread is the only
    // caller and the result is consumed before the next call), so steady state allocates nothing.
    const std::vector<uint16_t> &im2dLineList(uint32_t primitive, uint32_t count,
                                              const uint16_t *indices = nullptr)
    {
        static std::vector<uint16_t> out;
        out.clear();
        auto at = [&](uint32_t i) -> uint16_t { return indices ? indices[i] : static_cast<uint16_t>(i); };
        if (count > 65535u)
            return out;
        if (primitive == 1u) // rwPRIMTYPELINELIST
        {
            out.reserve((count / 2u) * 2u);
            for (uint32_t i = 0u; i + 1u < count; i += 2u)
            {
                out.push_back(at(i));
                out.push_back(at(i + 1u));
            }
        }
        else if (primitive == 2u) // rwPRIMTYPEPOLYLINE
        {
            out.reserve(count > 1u ? (count - 1u) * 2u : 0u);
            for (uint32_t i = 0u; i + 1u < count; ++i)
            {
                out.push_back(at(i));
                out.push_back(at(i + 1u));
            }
        }
        return out;
    }

    const std::vector<uint16_t> &im2dTriangleList(uint32_t primitive, uint32_t count,
                                                  const uint16_t *indices = nullptr)
    {
#if defined(PS2X_P3FES_FRAME_TRACE)
        p3fes_frame_trace::NativeScope traceScope(p3fes_frame_trace::Stage::PrimitiveExpand);
#endif
        static std::vector<uint16_t> out;
        out.clear();
        auto at = [&](uint32_t i) -> uint16_t { return indices ? indices[i] : static_cast<uint16_t>(i); };
        if (count > 65535u)
            return out;
        switch (primitive)
        {
        case 3u: // rwPRIMTYPETRILIST
            out.reserve((count / 3u) * 3u);
            for (uint32_t i = 0u; i + 2u < count; i += 3u)
            {
                out.push_back(at(i));
                out.push_back(at(i + 1u));
                out.push_back(at(i + 2u));
            }
            break;
        case 4u: // rwPRIMTYPETRISTRIP
            out.reserve(count > 2u ? (count - 2u) * 3u : 0u);
            for (uint32_t i = 0u; i + 2u < count; ++i)
            {
                if (i & 1u)
                {
                    out.push_back(at(i + 1u));
                    out.push_back(at(i));
                }
                else
                {
                    out.push_back(at(i));
                    out.push_back(at(i + 1u));
                }
                out.push_back(at(i + 2u));
            }
            break;
        case 5u: // rwPRIMTYPETRIFAN
            out.reserve(count > 2u ? (count - 2u) * 3u : 0u);
            for (uint32_t i = 1u; i + 1u < count; ++i)
            {
                out.push_back(at(0u));
                out.push_back(at(i));
                out.push_back(at(i + 1u));
            }
            break;
        default:
            break;
        }
        return out;
    }

    bool readIm3DVertex(const uint8_t *rdram, uint32_t base, uint32_t index, uint32_t flags, Vertex &out)
    {
        const uint64_t address = static_cast<uint64_t>(base) + static_cast<uint64_t>(index) * kIm3DVertexSize;
        if (address + kIm3DVertexSize > PS2_RAM_SIZE)
            return false;
        const uint32_t v = static_cast<uint32_t>(address);
        if (!readVec3(rdram, v, out.position) ||
            !std::all_of(out.position.begin(), out.position.end(), [](float x) { return std::isfinite(x); }))
            return false;
        std::memcpy(out.color.data(), rdram + v + 12u, 4u);
        if ((flags & kIm3DFlagUv) != 0u)
        {
            if (!readGuest(rdram, v + 28u, out.texCoord[0]) || !readGuest(rdram, v + 32u, out.texCoord[1]) ||
                !std::isfinite(out.texCoord[0]) || !std::isfinite(out.texCoord[1]))
                return false;
        }
        return true;
    }

    bool nativeIm3DModel(const uint8_t *rdram, Matrix4 &model)
    {
        model = p3fes::native_renderer::kIdentityMatrix;
        return g_nativeIm3D.matrix == 0u || readRwMatrix(rdram, g_nativeIm3D.matrix, model);
    }

    bool nativeIm3DSourceIndices(const uint8_t *rdram, bool indexed, uint32_t indicesAddress,
                                 uint32_t count, std::vector<uint16_t> &indices)
    {
        if (!indexed)
            return count <= g_nativeIm3D.count;
        if (count > (1u << 20u) || !guestPointer(indicesAddress, count * sizeof(uint16_t)))
            return false;
        indices.resize(count);
        if (count)
            std::memcpy(indices.data(), rdram + indicesAddress, count * sizeof(uint16_t));
        return true;
    }

    bool submitNativeIm3D(uint8_t *rdram, uint32_t primitive, bool indexed,
                          uint32_t indicesAddress, uint32_t indexCount)
    {
        if (g_gsColorMasked)
        {
            ++g_maskDropped[3];
            return true;
        }
        if (!g_nativeIm3D.open || g_nativeIm3D.count == 0u || g_nativeIm3D.count > 65536u)
            return false;

        std::vector<Vertex> vertices(g_nativeIm3D.count);
        for (uint32_t i = 0u; i < g_nativeIm3D.count; ++i)
            if (!readIm3DVertex(rdram, g_nativeIm3D.vertices, i, g_nativeIm3D.flags, vertices[i]))
                return false;

        std::vector<uint16_t> sourceIndices;
        const uint32_t sourceCount = indexed ? indexCount : g_nativeIm3D.count;
        if (!nativeIm3DSourceIndices(rdram, indexed, indicesAddress, sourceCount, sourceIndices))
            return false;
        auto at = [&](uint32_t i) -> uint32_t {
            return indexed ? static_cast<uint32_t>(sourceIndices[i]) : i;
        };
        auto valid = [&](uint32_t i) { return i < g_nativeIm3D.count; };

        std::string textureName;
        if (nativeIm2DTextured(rdram) && !prepareNativeIm2DTexture(rdram, textureName))
            return false;
        MeshDrawState drawState;
        readNativeGlobalMeshState(rdram, drawState);
        Matrix4 model{};
        if (!nativeIm3DModel(rdram, model) || !ensureNativeFrame(rdram))
            return false;
        captureNativeDrawCamera(rdram);

        bool queued = false;
        if (primitive == 1u || primitive == 2u)
        {
            std::vector<Vertex> lines;
            const uint32_t pairs = primitive == 1u ? sourceCount / 2u : (sourceCount > 1u ? sourceCount - 1u : 0u);
            lines.reserve(static_cast<size_t>(pairs) * 2u);
            if (primitive == 1u)
            {
                for (uint32_t i = 0u; i + 1u < sourceCount; i += 2u)
                {
                    const uint32_t a = at(i), b = at(i + 1u);
                    if (!valid(a) || !valid(b)) return false;
                    lines.push_back(vertices[a]); lines.push_back(vertices[b]);
                }
            }
            else
            {
                for (uint32_t i = 0u; i + 1u < sourceCount; ++i)
                {
                    const uint32_t a = at(i), b = at(i + 1u);
                    if (!valid(a) || !valid(b)) return false;
                    lines.push_back(vertices[a]); lines.push_back(vertices[b]);
                }
            }
            if (!lines.empty())
            {
                p3fes_native_present::drawWorldLines(std::move(lines), model, std::move(textureName), std::move(drawState));
                queued = true;
            }
        }
        else if (primitive == 3u || primitive == 4u || primitive == 5u)
        {
            MeshData mesh;
            mesh.vertices = std::move(vertices);
            mesh.hasPrelight = true;
            mesh.materials.resize(1u);
            mesh.materials[0].textureName = std::move(textureName);
            if (primitive == 3u)
            {
                mesh.indices.reserve((sourceCount / 3u) * 3u);
                for (uint32_t i = 0u; i + 2u < sourceCount; i += 3u)
                {
                    const uint32_t a = at(i), b = at(i + 1u), c = at(i + 2u);
                    if (!valid(a) || !valid(b) || !valid(c)) return false;
                    mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
                }
            }
            else if (primitive == 4u)
            {
                mesh.indices.reserve(sourceCount > 2u ? (sourceCount - 2u) * 3u : 0u);
                for (uint32_t i = 0u; i + 2u < sourceCount; ++i)
                {
                    uint32_t a = at(i), b = at(i + 1u), c = at(i + 2u);
                    if (!valid(a) || !valid(b) || !valid(c)) return false;
                    if (a == b || b == c || a == c) continue;
                    if (i & 1u) std::swap(a, b);
                    mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
                }
            }
            else
            {
                mesh.indices.reserve(sourceCount > 2u ? (sourceCount - 2u) * 3u : 0u);
                for (uint32_t i = 1u; i + 1u < sourceCount; ++i)
                {
                    const uint32_t a = at(0u), b = at(i), c = at(i + 1u);
                    if (!valid(a) || !valid(b) || !valid(c)) return false;
                    mesh.indices.push_back(a); mesh.indices.push_back(b); mesh.indices.push_back(c);
                }
            }
            if (!mesh.indices.empty())
            {
                mesh.submeshes.push_back(Submesh{0u, static_cast<uint32_t>(mesh.indices.size()), 0u});
                if (mesh.materials[0].textureName.rfind("rt.", 0) == 0)
                {
                    // Receiver of a camera texture (the floor triangle that shows the shadow map).
                    // Two things the GS does that the generic mesh state does not model:
                    //  * the RenderWare blend pair it asks for (ONE/SRCCOLOR) is not expressible on
                    //    the GS; ALPHA_1 holds 0x44, plain (Cs - Cd) * As + Cd;
                    //  * its alpha test has AFAIL = FB_ONLY with Z writes masked, so failing pixels
                    //    are drawn all the same. Treated as a discard it cuts the map's fade at
                    //    half alpha.
                    // Applied to this draw only: the same rules applied to every mesh broke other
                    // materials (the lounge chandelier), so the generic state is left as it was.
                    uint64_t gsAlpha = 0u, gsTest = 0u;
                    if (readGuest(rdram, kRwAlpha1, gsAlpha) && (gsAlpha & 0xFFu) == 0x44u)
                    {
                        MeshDrawState::BlendState blend;
                        blend.enabled = true;
                        drawState.materialBlends.assign(1u, blend); // SRCALPHA / INVSRCALPHA
                    }
                    if (readGuest(rdram, kRwTest1, gsTest) && ((gsTest >> 12u) & 0x3u) == 1u && !drawState.depthWrite)
                        drawState.alphaTest.enabled = false;
                }
                if (mesh.materials[0].textureName.rfind("rt.", 0) == 0 && shadowDiag(rdram, "receiver-im3d"))
                    for (const Vertex &v : mesh.vertices)
                        std::fprintf(stderr, "[p3fes-shadow]   pos=%.2f,%.2f,%.2f uv=%.4f,%.4f rgba=%u,%u,%u,%u\n",
                                     v.position[0], v.position[1], v.position[2], v.texCoord[0], v.texCoord[1],
                                     v.color[0], v.color[1], v.color[2], v.color[3]);
                p3fes_native_present::drawTransientMesh(std::move(mesh), model, std::move(drawState));
                queued = true;
            }
        }

        if (!g_im3dDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[p3fes-native] first Im3D: primitive=%u vertices=%u indices=%u indexed=%u textured=%u native=%u\n",
                         primitive, g_nativeIm3D.count, sourceCount, indexed ? 1u : 0u,
                         nativeIm2DTextured(rdram) ? 1u : 0u, queued ? 1u : 0u);
        return queued;
    }

    void p3fesRwIm3DTransform(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t vertices = reg32(ctx, 4u), count = reg32(ctx, 5u), matrix = reg32(ctx, 6u), flags = reg32(ctx, 7u);
        const uint64_t bytes = static_cast<uint64_t>(count) * kIm3DVertexSize;
        const bool valid = count <= 65536u && bytes <= PS2_RAM_SIZE &&
                           guestPointer(vertices, static_cast<uint32_t>(bytes)) &&
                           (matrix == 0u || guestPointer(matrix, 64u));
        if (valid)
            g_nativeIm3D = NativeIm3DState{vertices, count, matrix, flags | kIm3DEffectiveFields, true};
        else
            g_nativeIm3D = {};
        ctx->r[2] = _mm_cvtsi32_si128(valid ? static_cast<int32_t>(vertices) : 0);
        ctx->pc = reg32(ctx, 31u);
    }

    void p3fesRwIm3DRenderPrimitive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t primitive = reg32(ctx, 4u);
        const bool valid = g_nativeIm3D.open && primitive >= 1u && primitive <= 5u;
        if (valid)
            submitNativeIm3D(rdram, primitive, false, 0u, g_nativeIm3D.count);
        ctx->r[2] = _mm_cvtsi32_si128(valid ? 1 : 0);
        ctx->pc = reg32(ctx, 31u);
    }

    void p3fesRwIm3DRenderIndexedPrimitive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t primitive = reg32(ctx, 4u), indices = reg32(ctx, 5u), count = reg32(ctx, 6u);
        const uint64_t bytes = static_cast<uint64_t>(count) * sizeof(uint16_t);
        const bool valid = g_nativeIm3D.open && primitive >= 1u && primitive <= 5u &&
                           count <= (1u << 20u) && bytes <= PS2_RAM_SIZE &&
                           guestPointer(indices, static_cast<uint32_t>(bytes));
        if (valid)
            submitNativeIm3D(rdram, primitive, true, indices, count);
        ctx->r[2] = _mm_cvtsi32_si128(valid ? 1 : 0);
        ctx->pc = reg32(ctx, 31u);
    }

    void p3fesRwIm3DRenderLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t a = reg32(ctx, 4u), b = reg32(ctx, 5u);
        const bool valid = g_nativeIm3D.open && a < g_nativeIm3D.count && b < g_nativeIm3D.count;
        if (valid)
        {
            // RenderLine is just an indexed two-vertex line-list in the stock
            // implementation; use a tiny guest-independent index buffer here.
            std::vector<Vertex> vertices(2u);
            if (readIm3DVertex(rdram, g_nativeIm3D.vertices, a, g_nativeIm3D.flags, vertices[0]) &&
                readIm3DVertex(rdram, g_nativeIm3D.vertices, b, g_nativeIm3D.flags, vertices[1]))
            {
                Matrix4 model{};
                MeshDrawState state;
                std::string textureName;
                if (nativeIm3DModel(rdram, model) && ensureNativeFrame(rdram))
                {
                    captureNativeDrawCamera(rdram);
                    readNativeGlobalMeshState(rdram, state);
                    if (!nativeIm2DTextured(rdram) || prepareNativeIm2DTexture(rdram, textureName))
                        p3fes_native_present::drawWorldLines(std::move(vertices), model, std::move(textureName), std::move(state));
                }
            }
        }
        ctx->r[2] = _mm_cvtsi32_si128(valid ? 1 : 0);
        ctx->pc = reg32(ctx, 31u);
    }

    void p3fesRwIm3DEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const bool wasOpen = g_nativeIm3D.open;
        g_nativeIm3D = {};
        ctx->r[2] = _mm_cvtsi32_si128(wasOpen ? 1 : 0);
        ctx->pc = reg32(ctx, 31u);
    }

    void nativeIm2DResult(R5900Context *ctx)
    {
        ctx->r[2] = _mm_cvtsi32_si128(1); // every SkyIm2DRender* entry returns 1
        // A preemption can resume directly at this native leaf. Returning only
        // to the C++ caller leaves the guest PC here and repeats the same draw.
        ctx->pc = reg32(ctx, 31u);
    }

    void p3fesSkyIm2DRenderLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t verts = reg32(ctx, 4u), count = reg32(ctx, 5u);
        const uint32_t i0 = reg32(ctx, 6u), i1 = reg32(ctx, 7u);
        if (!g_im2dDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[p3fes-native] first Im2D call: line textured=%u\n", nativeIm2DTextured(rdram) ? 1u : 0u);
        if (i0 <= 65535u && i1 <= 65535u)
            drawNativeIm2DLines(rdram, verts, count,
                                {static_cast<uint16_t>(i0), static_cast<uint16_t>(i1)});
        nativeIm2DResult(ctx);
    }

    void p3fesSkyIm2DRenderTriangle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t verts = reg32(ctx, 4u), count = reg32(ctx, 5u);
        const uint32_t i0 = reg32(ctx, 6u), i1 = reg32(ctx, 7u), i2 = reg32(ctx, 8u);
        if (!g_im2dDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[p3fes-native] first Im2D call: triangle vertices=%u textured=%u\n",
                         count, nativeIm2DTextured(rdram) ? 1u : 0u);
        if (i0 <= 65535u && i1 <= 65535u && i2 <= 65535u)
            drawNativeIm2DTriangles(rdram, verts, count,
                                    {static_cast<uint16_t>(i0), static_cast<uint16_t>(i1), static_cast<uint16_t>(i2)});
        nativeIm2DResult(ctx);
    }

    void p3fesSkyIm2DRenderPrimitive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t primitive = reg32(ctx, 4u), verts = reg32(ctx, 5u), count = reg32(ctx, 6u);
        if (!g_im2dDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[p3fes-native] first Im2D call: primitive=%u vertices=%u textured=%u\n",
                         primitive, count, nativeIm2DTextured(rdram) ? 1u : 0u);
        if (primitive == 1u || primitive == 2u)
            drawNativeIm2DLines(rdram, verts, count, im2dLineList(primitive, count));
        else
            drawNativeIm2DTriangles(rdram, verts, count, im2dTriangleList(primitive, count));
        nativeIm2DResult(ctx);
    }

    void p3fesSkyIm2DRenderIndexedPrimitive(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const uint32_t primitive = reg32(ctx, 4u), verts = reg32(ctx, 5u), vertexCount = reg32(ctx, 6u);
        const uint32_t indicesAddress = reg32(ctx, 7u), indexCount = reg32(ctx, 8u);
        if (!g_im2dDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[p3fes-native] first Im2D call: indexed primitive=%u vertices=%u indices=%u textured=%u\n",
                         primitive, vertexCount, indexCount, nativeIm2DTextured(rdram) ? 1u : 0u);
        if (indexCount <= 65535u && guestPointer(indicesAddress, indexCount * sizeof(uint16_t)))
        {
            static std::vector<uint16_t> indices;
            indices.resize(indexCount);
            std::memcpy(indices.data(), rdram + indicesAddress, indexCount * sizeof(uint16_t));
            if (primitive == 1u || primitive == 2u)
                drawNativeIm2DLines(rdram, verts, vertexCount, im2dLineList(primitive, indexCount, indices.data()));
            else
                drawNativeIm2DTriangles(rdram, verts, vertexCount, im2dTriangleList(primitive, indexCount, indices.data()));
        }
        nativeIm2DResult(ctx);
    }

    bool installNativeOnlyHook(PS2Runtime &runtime, uint32_t address, PS2Runtime::RecompiledFunction hook)
    {
        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(address);
        return original && original != hook && runtime.replaceFunction(address, hook);
    }

    void finishNativeFrame(PS2Runtime *runtime)
    {
        if (!g_nativeFrameActive)
            return;
        if (!g_frameHasDisplayContent && cameraTexturesEnabled())
        {
            closeCameraTexture();
            return;
        }
        g_frameHasDisplayContent = false;
        g_gsColorMasked = false;
        g_targetRaster = 0u; // endFrame closes an open bracket
        p3fes_native_present::endFrame();
        g_nativeFrameActive = false;
        static const bool resourceDiag = [] {
            const char *value = std::getenv("P3FES_RESOURCE_DIAG");
            return value && *value == '1';
        }();
        if (resourceDiag)
        {
            static auto previous = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - previous >= std::chrono::seconds(5))
            {
                previous = now;
                const uint64_t fields = runtime ? runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed) : 0u;
                std::fprintf(stderr, "[p3fes-textures] prepared=%llu live=%u fields=%llu\n",
                             static_cast<unsigned long long>(g_texturePrepares), static_cast<unsigned>(g_nativeRasterResources.size()),
                             static_cast<unsigned long long>(fields));
                p3fes_native_rasters::logCounters();
                p3fes_native_texture_access::logCounters();
                p3fes_native_camera::logCounters();
                p3fes_native_rw_math::logCounters();
                p3fes_native_hanim::logCounters();
                std::fprintf(stderr, "[p3fes-native-skin-palette] native=%llu fallback=%llu rejected=%llu\n",
                             static_cast<unsigned long long>(g_rwSkinPaletteNativeCount),
                             static_cast<unsigned long long>(g_rwSkinPaletteFallbackCount),
                             static_cast<unsigned long long>(g_rwSkinPaletteRejectedCount));
                p3fes_native_bgm_hooks::logCounters();
            }
        }
        (void)runtime;
    }
#endif

    void p3fesAtomicDefaultRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx && ctx->pc == kRpAtomicRender)
        {
            const uint32_t atomic = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            logAtomicPipeline(rdram, ctx, atomic);
            AtomicSummary summary;
            if (inspectAtomic(rdram, atomic, summary) && g_diagEnabled.load(std::memory_order_relaxed) &&
                !g_diagPrinted.exchange(true, std::memory_order_relaxed))
            {
                std::fprintf(stderr,
                             "[p3fes-rw] RpAtomicRender atomic=0x%08X frame=0x%08X geometry=0x%08X flags=0x%02X "
                             "gflags=0x%08X vertices=%u triangles=%u morphs=%u verts=0x%08X tris=0x%08X\n",
                             summary.atomic, summary.frame, summary.geometry, summary.flags, summary.geometryFlags,
                             summary.vertices, summary.triangles, summary.morphTargets, summary.vertexData, summary.triangleData);
            }
#if !defined(__ANDROID__)
            if (g_nativeEnabled.load(std::memory_order_relaxed))
            {
                // Native mode is intentionally strict: once selected, an
                // unsupported atomic is missing rather than silently falling
                // back to the interlaced GS path.
                drawNativeAtomic(rdram, atomic, ctx, runtime);
                // RpAtomicRender returns the atomic on success.
                // Native submission itself is the success path here, and
                // callers do use $v0 as the RenderWare callback result.
                ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
                ctx->r[2] = ctx->r[4];
                ctx->pc = reg32(ctx, 31u);
                return;
            }
#endif
        }

        if (PS2Runtime::RecompiledFunction original = g_originalAtomicRender.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesSectorDefaultRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx && ctx->pc == kSectorDefaultRenderCallBack)
        {
            const uint32_t sector = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
#if !defined(__ANDROID__)
            if (g_nativeEnabled.load(std::memory_order_relaxed))
            {
                // World sectors are the field environment path. In strict
                // native mode consume them here, before PS2All/VIF/VU1/GS.
                drawNativeSector(rdram, sector);
                ctx->r[2] = ctx->r[4];
                ctx->pc = reg32(ctx, 31u);
                return;
            }
#endif
        }
        if (PS2Runtime::RecompiledFunction original = g_originalSectorRender.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

#if !defined(__ANDROID__)
    // ---- Particle tanks (RpPTank), the "Spr3dGen" kind --------------------------------------------
    // A tank atomic has an empty RpGeometry (no vertices, no triangles): the PS2 pipeline expands the
    // particles into camera-facing quads on VU0/VU1 from the tank's attribute arrays, so the generic
    // atomic decoder rejects it ("mesh decode failed"). The expansion, read from the recompiled
    // callbacks (rpPTankSkyInsSetup_Spr3dGen 0x479170, rpPTankSkyInsPosNCPSPR 0x47E9E0,
    // rpPTankSkyInsUV2C 0x485EF0, rpPTankSkyInsColorPP 0x4838E0), is reproduced here:
    //   tank+0x00 capacity, +0x04 live particle count, +0x24/+0x28/+0x2C/+0x30 the instance callbacks
    //   (setup, position, constant UV, colour), +0x50 the lock table of {data, stride} pairs at +4+8k:
    //   k0 position (3 floats), k3 size (w, h floats), k4 colour (RGBA8, 0x80 = 1.0), k6 rotation
    //   (radians), and the constant UV window at table+0x90 (u0, v0, u1, v1);
    //   corner i of a particle is P + s_i * (R) + t_i * (U) with R = (right*cos - up*sin) * w/2,
    //   U = (up*cos + right*sin) * h/2, right/up the camera axes as the host view sees them.
    constexpr uint32_t kPtankSetupSpr3d = 0x00479170u;
    constexpr uint32_t kPtankPosNCPSPR = 0x0047E9E0u;
    constexpr uint32_t kPtankColorPP = 0x004838E0u;
    constexpr uint32_t kPtankMaxParticles = 4096u;

    bool drawNativePtankSprites(uint8_t *rdram, uint32_t atomic, uint32_t tank, int32_t countSigned,
                                const MeshDrawState::BlendState *blendOverride, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool disabled = [] { const char *v = std::getenv("P3FES_PTANK_SPRITES"); return v && *v == '0'; }();
        if (disabled)
            return false;
        uint32_t setup = 0u, position = 0u, color = 0u;
        if (!guestPointer(tank, 0x100u) || !readGuest(rdram, tank + 0x24u, setup) ||
            !readGuest(rdram, tank + 0x28u, position) || !readGuest(rdram, tank + 0x30u, color) ||
            setup != kPtankSetupSpr3d || position != kPtankPosNCPSPR || color != kPtankColorPP)
            return false;
        if (countSigned <= 0 || static_cast<uint32_t>(countSigned) > kPtankMaxParticles)
            return false;
        const uint32_t count = static_cast<uint32_t>(countSigned);

        const uint32_t table = tank + 0x50u;
        uint32_t posData = 0u, posStride = 0u, sizeData = 0u, sizeStride = 0u, colorData = 0u, colorStride = 0u;
        uint32_t rotData = 0u, rotStride = 0u;
        if (!readGuest(rdram, table + 0x04u, posData) || !readGuest(rdram, table + 0x08u, posStride) ||
            !readGuest(rdram, table + 0x1Cu, sizeData) || !readGuest(rdram, table + 0x20u, sizeStride) ||
            !readGuest(rdram, table + 0x24u, colorData) || !readGuest(rdram, table + 0x28u, colorStride) ||
            !readGuest(rdram, table + 0x34u, rotData) || !readGuest(rdram, table + 0x38u, rotStride) ||
            !posData || !sizeData || !colorData)
            return false;
        // Every array is an interleaved column of one particle record.
        const auto fits = [&](uint32_t data, uint32_t stride, uint32_t bytes) {
            return stride >= bytes && stride <= 0x400u &&
                   guestPointer(data, (count - 1u) * stride + bytes);
        };
        if (!fits(posData, posStride, 12u) || !fits(sizeData, sizeStride, 8u) || !fits(colorData, colorStride, 4u) ||
            (rotData && !fits(rotData, rotStride, 4u)))
            return false;
        float uv[4] = {0.0f, 0.0f, 1.0f, 1.0f}; // u0, v0, u1, v1
        for (uint32_t i = 0u; i < 4u; ++i)
        {
            float value = 0.0f;
            if (readGuest(rdram, table + 0x90u + i * 4u, value) && std::isfinite(value))
                uv[i] = value;
        }

        AtomicSummary summary;
        if (!inspectAtomic(rdram, atomic, summary) || (summary.flags & kAtomicRenderFlag) == 0u)
            return false;
        if (!ensureNativeFrame(rdram))
            return false;
        captureNativeDrawCamera(rdram);
        Camera camera;
        if (!readCurrentCamera(rdram, camera) || !camera.useMatrices)
            return false;
        Matrix4 rawModel{}, model{};
        bool syncRequested = false;
        if (!readFrameMatrixAfterSync(rdram, summary.frame, ctx, runtime, rawModel, model, syncRequested))
            return false;
        const std::array<float, 3> right{camera.view[0], camera.view[4], camera.view[8]};
        const std::array<float, 3> up{camera.view[1], camera.view[5], camera.view[9]};

        MeshDrawState drawState = p3fes_native_present::acquireDrawState();
        readNativeMeshDrawState(rdram, summary.geometry, drawState);
        readNativeGlobalMeshState(rdram, drawState);
        if (blendOverride && blendOverride->enabled)
        {
            drawState.blendEnabled = true;
            drawState.materialBlends.assign(std::max<std::size_t>(drawState.materialColors.size(), 1u), *blendOverride);
        }
        drawState.cullMode = MeshDrawState::CullMode::None;
        drawState.materialColors.assign(1u, {255u, 255u, 255u, 255u});

        MeshData mesh;
        mesh.vertices.resize(static_cast<std::size_t>(count) * 4u);
        mesh.indices.reserve(static_cast<std::size_t>(count) * 6u);
        mesh.hasPrelight = true;
        mesh.materials.resize(1u);
        if (!drawState.materialTextures.empty())
            mesh.materials[0].textureName = drawState.materialTextures[0];
        const float cornerS[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        const float cornerT[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        const float cornerU[4] = {uv[0], uv[2], uv[2], uv[0]};
        const float cornerV[4] = {uv[3], uv[3], uv[1], uv[1]};
        uint32_t quads = 0u;
        for (uint32_t i = 0u; i < count; ++i)
        {
            float p[3] = {}, size[2] = {}, angle = 0.0f;
            std::array<uint8_t, 4> rgba{};
            readGuest(rdram, posData + i * posStride + 0u, p[0]);
            readGuest(rdram, posData + i * posStride + 4u, p[1]);
            readGuest(rdram, posData + i * posStride + 8u, p[2]);
            readGuest(rdram, sizeData + i * sizeStride + 0u, size[0]);
            readGuest(rdram, sizeData + i * sizeStride + 4u, size[1]);
            if (rotData)
                readGuest(rdram, rotData + i * rotStride, angle);
            std::memcpy(rgba.data(), rdram + colorData + i * colorStride, 4u);
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !std::isfinite(size[0]) ||
                !std::isfinite(size[1]) || !std::isfinite(angle))
                continue;
            const float c = std::cos(angle), sn = std::sin(angle);
            const float hw = 0.5f * size[0], hh = 0.5f * size[1];
            std::array<float, 3> axisR, axisU, centre;
            for (int k = 0; k < 3; ++k)
            {
                axisR[k] = (right[k] * c - up[k] * sn) * hw;
                axisU[k] = (up[k] * c + right[k] * sn) * hh;
                centre[k] = model[k] * p[0] + model[4 + k] * p[1] + model[8 + k] * p[2] + model[12 + k];
            }
            const uint32_t base = quads * 4u;
            for (uint32_t corner = 0u; corner < 4u; ++corner)
            {
                Vertex &v = mesh.vertices[base + corner];
                for (int k = 0; k < 3; ++k)
                    v.position[k] = centre[k] + cornerS[corner] * axisR[k] + cornerT[corner] * axisU[k];
                v.texCoord = {cornerU[corner], cornerV[corner]};
                v.color = rgba;
            }
            mesh.indices.insert(mesh.indices.end(), {base, base + 1u, base + 2u, base, base + 2u, base + 3u});
            ++quads;
        }
        if (quads == 0u)
            return true; // nothing live: the original draws nothing either
        mesh.vertices.resize(static_cast<std::size_t>(quads) * 4u);
        mesh.submeshes.push_back(Submesh{0u, static_cast<uint32_t>(mesh.indices.size()), 0u});
        p3fes_native_present::drawTransientMesh(std::move(mesh), p3fes::native_renderer::kIdentityMatrix, std::move(drawState));
        return true;
    }
#endif

    // Replacement for rpPTankAtomicRenderCB (0x474160). It keeps the original's control flow, frame and register
    // use, because both tank callbacks are guest code and can be interrupted: at an EE checkpoint the callee
    // (or something it calls) unwinds the host stack with ctx->pc set to where it must resume, and the scheduler
    // later re-enters by PC. The two return addresses 0x4741AC (after the +0x1C callback) and 0x4741CC (after the
    // +0x20 callback) are therefore also hooked, as resume entries of this function: the original frame holds
    // ra/s0-s2, s1 = atomic, s2 = tank and s0 = the instance result live in the guest registers across the calls.
    // Calling the callbacks with a plain host call and carrying on, as this hook once did, treated an interrupted
    // callee as finished and left the guest with a half-run callback (corrupt s1, a callback slot read from the
    // wrong struct, a jump to a heap address).
    void p3fesPtankAtomicRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if !defined(__ANDROID__)
        if (!ctx || !runtime)
            return;
        constexpr uint32_t kAfterLock = kPtankAtomicRender + 0x4Cu;     // 0x4741AC
        constexpr uint32_t kAfterInstance = kPtankAtomicRender + 0x6Cu; // 0x4741CC
        constexpr uint32_t kFrame = 0x40u;
        const uint32_t entryPc = ctx->pc;
        const bool resumeLock = entryPc == kAfterLock;
        const bool resumeInstance = entryPc == kAfterInstance;
        uint32_t atomic = 0u, tank = 0u;
        int32_t count = 0;
        uint32_t sp = reg32(ctx, 29u);
        bool lockCalled = resumeLock;

        if (!resumeLock && !resumeInstance)
        {
            atomic = reg32(ctx, 4u);
            uint32_t pluginOffset = 0u;
            const auto atomicExtension = [&]() -> uint32_t {
                const uint64_t address = static_cast<uint64_t>(atomic) + pluginOffset;
                return address <= 0xFFFFFFFFull ? static_cast<uint32_t>(address) : 0xFFFFFFFFu;
            };
            if (!(readGuest(rdram, kPtankAtomicExtensionOffset, pluginOffset) && pluginOffset != 0u &&
                  guestPointer(atomicExtension(), sizeof(uint32_t)) &&
                  readGuest(rdram, atomicExtension(), tank) && tank && guestPointer(tank, 104u) &&
                  readGuest(rdram, tank + 4u, count) && count > 0))
            {
                // Nothing to run (the original skips to its epilogue for count <= 0).
                setReg32(ctx, 2u, atomic);
                setReg32(ctx, 4u, atomic);
                ctx->pc = reg32(ctx, 31u);
                return;
            }
            // Prologue: the original's 0x40-byte frame (ra at 0x30, s2 0x20, s1 0x10, s0 0x0).
            sp -= kFrame;
            if (!guestPointer(sp, kFrame))
                return;
            std::memcpy(rdram + sp + 0x30u, &ctx->r[31], sizeof(uint64_t));
            std::memcpy(rdram + sp + 0x20u, &ctx->r[18], 16u);
            std::memcpy(rdram + sp + 0x10u, &ctx->r[17], 16u);
            std::memcpy(rdram + sp + 0x00u, &ctx->r[16], 16u);
            setReg32(ctx, 29u, sp);
            setReg32(ctx, 17u, atomic);
            setReg32(ctx, 18u, tank);
            setReg32(ctx, 16u, 1u);
        }
        else
        {
            atomic = reg32(ctx, 17u);
            tank = reg32(ctx, 18u);
            if (!guestPointer(tank, 104u) || !guestPointer(sp, kFrame))
                return;
        }
        readGuest(rdram, tank + 4u, count);

        if (!resumeInstance)
        {
            if (!resumeLock)
            {
                uint32_t lockCallback = 0u;
                readGuest(rdram, tank + 28u, lockCallback);
                if (lockCallback)
                {
                    uint32_t lockFlags = 0u;
                    readGuest(rdram, tank + 64u, lockFlags);
                    setReg32(ctx, 7u, lockFlags);
                    setReg32(ctx, 5u, tank + 80u);
                    setReg32(ctx, 6u, count);
                    setReg32(ctx, 31u, kAfterLock);
                    if (!runtime->dispatchGuestBranch(rdram, ctx, lockCallback, kPtankAtomicRender + 0x44u, kAfterLock,
                                                      PS2Runtime::GuestBranchKind::IndirectCall, "JALR"))
                        return; // interrupted: resumes at kAfterLock
                    lockCalled = true;
                }
            }
            if (lockCalled)
            {
                const uint32_t zero = 0u;
                std::memcpy(rdram + tank + 64u, &zero, sizeof(zero));
            }

            uint32_t instanceCallback = 0u;
            readGuest(rdram, tank + 32u, instanceCallback);
            if (instanceCallback)
            {
                setReg32(ctx, 6u, count);
                setReg32(ctx, 4u, atomic);
                setReg32(ctx, 5u, tank + 80u);
                setReg32(ctx, 31u, kAfterInstance);
                if (!runtime->dispatchGuestBranch(rdram, ctx, instanceCallback, kPtankAtomicRender + 0x64u, kAfterInstance,
                                                  PS2Runtime::GuestBranchKind::IndirectCall, "JALR"))
                    return; // interrupted: resumes at kAfterInstance
                ctx->r[16] = ctx->r[2];
            }
        }
        else
            ctx->r[16] = ctx->r[2]; // 0x4741CC: s0 = v0

        // 0x4741D0: the original calls the render callback at +0x10 when the instance result is 1; this draws natively.
        const uint32_t instanceResult = reg32(ctx, 16u);
        MeshDrawState::BlendState blend;
        uint32_t blendEnabled = 0u, srcBlend = 0u, dstBlend = 0u;
        if (readGuest(rdram, tank + 100u, blendEnabled) && blendEnabled != 0u &&
            readGuest(rdram, tank + 92u, srcBlend) && readGuest(rdram, tank + 96u, dstBlend) &&
            srcBlend >= 1u && srcBlend <= 11u && dstBlend >= 1u && dstBlend <= 11u)
        {
            blend.enabled = true;
            blend.src = static_cast<uint8_t>(srcBlend);
            blend.dst = static_cast<uint8_t>(dstBlend);
        }

        bool drawn = false;
        if (instanceResult == 1u)
        {
            drawn = drawNativePtankSprites(rdram, atomic, tank, count, blend.enabled ? &blend : nullptr, ctx, runtime);
            if (!drawn)
                drawn = drawNativeAtomic(rdram, atomic, ctx, runtime, blend.enabled ? &blend : nullptr);
        }
        if (!g_ptankDiagPrinted.exchange(true, std::memory_order_relaxed))
        {
            uint32_t lockCallback = 0u, instanceCallback = 0u;
            readGuest(rdram, tank + 28u, lockCallback);
            readGuest(rdram, tank + 32u, instanceCallback);
            std::fprintf(stderr,
                         "[p3fes-native] first PTank: atomic=0x%08X count=%d lock=0x%08X instance=0x%08X result=%u native=%u\n",
                         atomic, count, lockCallback, instanceCallback, instanceResult, drawn ? 1u : 0u);
        }

        // Epilogue (0x4741E8): v0 = atomic, restore ra/s0-s2 from the frame, drop it, return.
        uint64_t ra = 0u;
        std::memcpy(&ra, rdram + sp + 0x30u, sizeof(ra));
        std::memcpy(&ctx->r[18], rdram + sp + 0x20u, 16u);
        std::memcpy(&ctx->r[17], rdram + sp + 0x10u, 16u);
        std::memcpy(&ctx->r[16], rdram + sp + 0x00u, 16u);
        setReg32(ctx, 2u, atomic);
        setReg32(ctx, 4u, atomic);
        setReg32(ctx, 29u, sp + kFrame);
        ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(ra));
        ctx->pc = static_cast<uint32_t>(ra);
#else
        (void)rdram;
        (void)ctx;
        (void)runtime;
#endif
    }

    void p3fesGsColorMaskOn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_gsColorMasked = true;
        g_maskOnRa = getRegU32(ctx, 31);
        std::memset(g_maskDropped, 0, sizeof(g_maskDropped));
        if (PS2Runtime::RecompiledFunction original = g_originalColorMaskOn.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesGsColorMaskOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool maskDiag = [] { const char *v = std::getenv("P3FES_MASK_DIAG"); return v && *v == '1'; }();
        if (maskDiag && g_gsColorMasked)
            std::fprintf(stderr, "[p3fes-mask] bracket on-ra=%08X off-ra=%08X dropped atomic=%u sector=%u im2d=%u im3d=%u\n", g_maskOnRa,
                         getRegU32(ctx, 31), g_maskDropped[0], g_maskDropped[1], g_maskDropped[2], g_maskDropped[3]);
        g_gsColorMasked = false;
        if (PS2Runtime::RecompiledFunction original = g_originalColorMaskOff.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesRwCameraShowRaster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if !defined(__ANDROID__)
        if (g_nativeEnabled.load(std::memory_order_relaxed))
            finishNativeFrame(runtime);
#endif
        // Keep the guest's flip/timing bookkeeping for now. Host presentation
        // is suppressed in native mode, so this is not a visual GS fallback.
        if (PS2Runtime::RecompiledFunction original = g_originalShowRaster.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesSofdecUpdateTask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if defined(PLATFORM_VITA)
        if (ctx && g_nativeEnabled.load(std::memory_order_relaxed))
        {
            const uint32_t task = reg32(ctx, 4u);
            uint32_t state = 0u;
            int16_t phase = -1;
            if (readGuest(rdram, task + 60u, state) && state && readGuest(rdram, state + 484u, phase))
            {
                if (g_nativeMovieTask == task)
                {
                    // +0x1FC is the guest's deferred-resource cleanup flag,
                    // which is already set before phase 2, not a cancel flag.
                    if (phase == 0)
                    {
                        std::fprintf(stderr, "[native-movie] guest idle task=%08X state=%08X ra=%08X\n", task, state, reg32(ctx, 31u));
                        p3fes_native_movie::cancel();
                    }
                    const auto status = p3fes_native_movie::status();
                    if (status == p3fes_native_movie::Status::Pending || status == p3fes_native_movie::Status::Playing)
                    {
                        // The task scheduler enters with v0 == -1. Returning
                        // it would destroy this task before the first frame.
                        ctx->r[2] = _mm_setzero_si128();
                        ctx->pc = reg32(ctx, 31u);
                        return;
                    }
                    // Script-owned tasks wait in phase zero for MOVIE_SYNC;
                    // standalone opening tasks return -1 so the scheduler
                    // destroys them and op_root can advance, as at 0x10B7F0.
                    const int16_t next = status == p3fes_native_movie::Status::Failed ? 2 : 0;
                    std::memcpy(rdram + state + 484u, &next, sizeof(next));
                    if (status == p3fes_native_movie::Status::Failed)
                        g_failedNativeMovies.insert(g_nativeMoviePath);
                    g_nativeMovieTask = 0u;
                    p3fes_native_movie::reset();
                    p3fes_native_present::setHostPresentationPaused(false);
                    if (next == 0)
                    {
                        uint32_t standalone = 0u;
                        readGuest(rdram, state + 500u, standalone);
                        ctx->r[2] = standalone ? _mm_set1_epi32(-1) : _mm_setzero_si128();
                        ctx->pc = reg32(ctx, 31u);
                        return;
                    }
                }
                else if (phase == 2)
                {
                    const char *enabled = std::getenv("P3FES_NATIVE_MOVIES");
                    int16_t movie = -1;
                    uint32_t name = 0u;
                    if (!(enabled && *enabled == '0') && readGuest(rdram, state + 490u, movie) &&
                        movie >= 0 && movie < 128 &&
                        readGuest(rdram, 0x005D4B74u + static_cast<uint32_t>(movie) * 40u, name))
                    {
                        std::string source;
                        for (uint32_t i = 0; i < 128u; ++i)
                        {
                            uint8_t ch = 0u;
                            if (!readGuest(rdram, name + i, ch) || !ch)
                                break;
                            source.push_back(static_cast<char>(ch));
                        }
                        const size_t slash = source.find_last_of("/\\");
                        std::string stem = source.substr(slash == std::string::npos ? 0 : slash + 1);
                        if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos)
                            stem.resize(dot);
                        for (char &ch : stem)
                            if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
                        const std::string path = "ux0:data/p3fes/native_movies/" + stem + ".mp4";
                        std::fprintf(stderr, "[native-movie] request id=%d source=%s task=%08X state=%08X ra=%08X\n", movie, source.c_str(), task, state, reg32(ctx, 31u));
                        if (!stem.empty() && !g_failedNativeMovies.count(path) && p3fes_native_movie::request(path))
                        {
                            g_nativeMovieTask = task;
                            g_nativeMoviePath = path;
                            const int16_t playing = 3;
                            std::memcpy(rdram + state + 484u, &playing, sizeof(playing));
                            finishNativeFrame(runtime);
                            p3fes_native_present::setHostPresentationPaused(true);
                            ctx->r[2] = _mm_setzero_si128();
                            ctx->pc = reg32(ctx, 31u);
                            return;
                        }
                    }
                }
            }
        }
#endif
#if !defined(__ANDROID__)
        if (ctx && g_nativeEnabled.load(std::memory_order_relaxed))
        {
            const uint32_t task = reg32(ctx, 4u);
            uint32_t state = 0u, raster = 0u;
            int16_t phase = -1;
            if (guestPointer(task + 60u, sizeof(uint32_t)) && readGuest(rdram, task + 60u, state) && state)
            {
                readGuest(rdram, state + 208u, raster);
                readGuest(rdram, state + 484u, phase);
            }

            const bool activeMovie = phase == 3 || phase == 4;
            if (g_diagEnabled.load(std::memory_order_relaxed))
            {
                static int16_t lastPhase = -2;
                if (phase != lastPhase)
                {
                    std::fprintf(stderr, "[p3fes-native] Sofdec phase=%d raster=0x%08X\n", phase, raster);
                    lastPhase = phase;
                }
            }
            p3fes_native_present::setHostPresentationPaused(activeMovie);
            if (activeMovie)
            {
                if (raster)
                    queueSofdecRaster(rdram, runtime, raster);
                // Close the producer frame on every Sofdec tick even when Vita
                // presents the movie through sceDisplaySetFrameBuf. The host
                // renderer is paused, so this cannot emit a GXM swap; endFrame
                // merely replaces the single pending frame and prevents the
                // command vector from growing for the duration of the movie.
                finishNativeFrame(runtime);
            }
        }
#endif
        if (PS2Runtime::RecompiledFunction original = g_originalSofdecUpdateTask.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesSofdecDestroyTask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if defined(PLATFORM_VITA)
        if (ctx && g_nativeMovieTask && reg32(ctx, 4u) == g_nativeMovieTask)
        {
            std::fprintf(stderr, "[native-movie] guest destroy task=%08X ra=%08X\n", g_nativeMovieTask, reg32(ctx, 31u));
            p3fes_native_movie::cancel();
            g_nativeMovieTask = 0u;
        }
#endif
#if !defined(__ANDROID__)
        p3fes_native_present::setHostPresentationPaused(false);
#endif
        if (PS2Runtime::RecompiledFunction original = g_originalSofdecDestroyTask.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    // RwCameraClear runs before RwCameraBeginUpdate in the shadow task, so the
    // begin/end flag does not cover it: read the raster type of the camera
    // being cleared, the same way the camera hooks do (camera+0x60 -> raster,
    // raster+0x20 & 7 -> type).
    bool isTextureCameraArgument(const uint8_t *rdram, uint32_t camera)
    {
        uint32_t raster = 0u;
        uint8_t type = 0u;
        return guestPointer(camera, 0x64u) && readGuest(rdram, camera + 0x60u, raster) && raster &&
               guestPointer(raster, 0x21u) && readGuest(rdram, raster + 0x20u, type) && (type & 0x07u) > 4u;
    }

    void p3fesRwCameraClear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if !defined(__ANDROID__)
        if (ctx && g_nativeEnabled.load(std::memory_order_relaxed) && cameraTexturesEnabled() &&
            isTextureCameraArgument(rdram, reg32(ctx, 4u)))
        {
            // Clear of a camera texture: open its render-target pass and clear that. The guest clear
            // still runs below for its RenderWare bookkeeping (its GS packets are not rasterized).
            const uint32_t camera = reg32(ctx, 4u);
            const uint32_t colorAddress = reg32(ctx, 5u);
            const uint32_t flags = reg32(ctx, 6u);
            uint32_t raster = 0u;
            if (readGuest(rdram, camera + 0x60u, raster) && openCameraTexture(rdram, raster))
            {
                ClearColor clear{};
                if ((flags & 1u) != 0u && guestPointer(colorAddress, 4u))
                    std::memcpy(&clear, rdram + colorAddress, 4u);
                p3fes_native_present::clear(clear, (flags & 1u) != 0u, (flags & 2u) != 0u);
                if (g_diagEnabled.load(std::memory_order_relaxed) && g_targetClears++ < 4u)
                    std::fprintf(stderr, "[p3fes-camera-texture] clear camera=%08X raster=%08X flags=%X rgba=%u,%u,%u,%u ra=%08X\n",
                                 camera, raster, flags, clear.r, clear.g, clear.b, clear.a, reg32(ctx, 31u));
            }
        }
        else if (ctx && g_nativeEnabled.load(std::memory_order_relaxed) &&
            !isTextureCameraArgument(rdram, reg32(ctx, 4u)))
        {
            if (runtime && reg32(ctx, 31u) == kShadowNodeClearReturn)
            {
                const uint64_t vsync = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
                const auto now = std::chrono::steady_clock::now();
                const bool tickAdvanced = vsync != g_shadowBoundaryVsync;
                const bool hostDeadline = g_shadowBoundaryTime.time_since_epoch().count() != 0 &&
                                          now - g_shadowBoundaryTime >= kShadowFallbackPeriod;
                if (tickAdvanced || hostDeadline)
                {
                    finishNativeFrame(runtime);
                    g_shadowBoundaryVsync = vsync;
                    g_shadowBoundaryTime = now;
                }
            }
            const uint32_t camera = reg32(ctx, 4u);
            const uint32_t colorAddress = reg32(ctx, 5u);
            const uint32_t flags = reg32(ctx, 6u);
            if (g_diagEnabled.load(std::memory_order_relaxed))
            {
                uint32_t currentCamera = 0u;
                readGuest(rdram, kRwCurrentCamera, currentCamera);
                const uint64_t pair = (static_cast<uint64_t>(camera) << 32u) | currentCamera;
                if (g_loggedClearCameras.size() < 16u && g_loggedClearCameras.insert(pair).second)
                    std::fprintf(stderr,
                        "[p3fes-camera-clear] frame=%llu camera=%08X current-camera=%08X saved-camera=%08X flags=%X ra=%08X\n",
                        static_cast<unsigned long long>(g_nativeFrameNumber), camera, currentCamera,
                        g_nativeFrameCameraAddress, flags, reg32(ctx, 31u));
            }
            if (ensureNativeFrame(rdram))
            {
                ClearColor clear{};
                if ((flags & 1u) != 0u && guestPointer(colorAddress, 4u))
                    std::memcpy(&clear, rdram + colorAddress, 4u);
                if (g_diagEnabled.load(std::memory_order_relaxed) &&
                    !g_clearDiagPrinted.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr, "[p3fes-rw] first native camera clear: flags=0x%X rgba=%u,%u,%u,%u\n",
                                 flags, clear.r, clear.g, clear.b, clear.a);
                p3fes_native_present::clear(clear, (flags & 1u) != 0u, (flags & 2u) != 0u);
                ctx->r[2] = _mm_cvtsi32_si128(static_cast<int32_t>(camera));
                ctx->pc = reg32(ctx, 31u);
                return;
            }
        }
#endif
        if (PS2Runtime::RecompiledFunction original = g_originalCameraClear.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void applyP3fesRenderWare(PS2Runtime &runtime)
    {
        const char *native = std::getenv("P3FES_NATIVE_RW");
        const char *diag = std::getenv("P3FES_RW_HOOK_DIAG");
#if defined(PLATFORM_VITA)
        // Vita is the native-rendering target. Environment variables are not a
        // practical launch-time switch on hardware (or when Vita3K starts an
        // installed title), so make the progressive RenderWare path the
        // platform default. Keeping an explicit "0" escape hatch is useful for
        // development builds launched by tools that do provide an environment.
        const bool nativeEnabled = !(native && *native == '0');
#else
        const bool nativeEnabled = native && *native && *native != '0';
#endif
        const bool diagEnabled = diag && *diag && *diag != '0';
        if (!nativeEnabled && !diagEnabled)
        {
#if !defined(__ANDROID__)
            // GS reference runs (P3FES_NATIVE_RW=0): the debug warp harness is inert unless a P3FES_WARP* variable
            // is set, so a warped scene can be rendered by the game's own GS path for comparison.
            p3fes_debug_warp::install(runtime);
#endif
            return;
        }

        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kRpAtomicRender);
        if (!original || original == &p3fesAtomicDefaultRender)
            return;

        if (nativeEnabled)
        {
            p3fes_native_rasters::reset();
            g_nativeRasterResources.clear();
            g_textureMinAlpha.clear();
            for (auto &slot : g_rasterNameCache)
                slot = {};
            g_texturePrepares = 0u;
        }
        g_diagEnabled.store(diagEnabled, std::memory_order_relaxed);
        g_nativeEnabled.store(nativeEnabled, std::memory_order_relaxed);
        const char *rwSkinMath = std::getenv("P3FES_NATIVE_RW_MATH");
        g_rwSkinPaletteNativeMath = !rwSkinMath || *rwSkinMath != '0';
        const char *rwSkinMathCompare = std::getenv("P3FES_RW_MATH_COMPARE");
        g_rwSkinPaletteCompare = rwSkinMathCompare && *rwSkinMathCompare && *rwSkinMathCompare != '0';
        g_rwSkinPaletteCompareLogs = 0u;
        g_rwSkinPaletteCompareSeen = 0u;
        g_rwSkinPaletteNativeCount = g_rwSkinPaletteFallbackCount = g_rwSkinPaletteRejectedCount = 0u;
        g_diagPrinted.store(false, std::memory_order_relaxed);
        g_skinDiagnosticSamples.clear();
        g_skinFrameDiagnostics.clear();
        g_im2dDiagPrinted.store(false, std::memory_order_relaxed);
        g_im2dTextureDiagPrinted.store(false, std::memory_order_relaxed);
        g_sectorDiagPrinted.store(false, std::memory_order_relaxed);
        g_ptankDiagPrinted.store(false, std::memory_order_relaxed);
        g_im3dDiagPrinted.store(false, std::memory_order_relaxed);
        g_clearDiagPrinted.store(false, std::memory_order_relaxed);
        g_loggedAtomicPipelines.clear();
        g_skinDiagnosticSamples.clear();
        g_loggedAtomicCameras.clear();
        g_loggedClearCameras.clear();
        g_nativeFrameNumber = 0u;
        g_nativeFrameCameraAddress = 0u;
        g_nativeFrameCameraState = {};
        g_shadowBoundaryVsync = ~uint64_t{0};
        g_shadowBoundaryTime = {};
        g_nativeIm3D = {};
        g_originalAtomicRender.store(original, std::memory_order_release);
        const bool replaced = runtime.replaceFunction(kRpAtomicRender, &p3fesAtomicDefaultRender);
        if (diagEnabled)
            std::fprintf(stderr, "[p3fes-rw] RpAtomicRender hook %s at 0x%08X\n",
                         replaced ? "installed" : "failed", kRpAtomicRender);

#if !defined(__ANDROID__)
        if (nativeEnabled)
        {
            const bool rasterLifecycle = p3fes_native_rasters::install(runtime, &retireNativeRaster);
            p3fes_native_texture_access::resetCounters();
            const bool textureAccess = p3fes_native_texture_access::install(runtime, &prepareNativeRasterTexture);
            const char *nativeCameraOption = std::getenv("P3FES_NATIVE_CAMERA");
            const bool nativeCamera = (!nativeCameraOption || *nativeCameraOption != '0') &&
                                      p3fes_native_camera::install(runtime);
            std::fprintf(stderr, "[p3fes-native] raster lifecycle hooks=%u; content scans disabled\n", rasterLifecycle ? 1u : 0u);
            std::fprintf(stderr, "[p3fes-native] native texture access hooks=%u\n", textureAccess ? 1u : 0u);
            std::fprintf(stderr, "[p3fes-native] native camera begin hook=%u\n", nativeCamera ? 1u : 0u);
            const char *nativeMathOption = std::getenv("P3FES_NATIVE_MATH");
            p3fes_native_math::resetCounters();
            const bool nativeMath = p3fes_native_math::install(runtime, !nativeMathOption || *nativeMathOption != '0');
            std::fprintf(stderr, "[p3fes-native] native libm hooks=%u\n", nativeMath ? 1u : 0u);
            const char *rwMathOption = std::getenv("P3FES_NATIVE_RW_MATH");
            p3fes_native_rw_math::resetCounters();
            const bool rwMath = p3fes_native_rw_math::install(runtime, !rwMathOption || *rwMathOption != '0');
            std::fprintf(stderr, "[p3fes-native] native RW math hooks=%u\n", rwMath ? 1u : 0u);
            p3fes_native_hanim::resetCounters();
            const bool nativeHanim = p3fes_native_hanim::install(runtime, !rwMathOption || *rwMathOption != '0');
            std::fprintf(stderr, "[p3fes-native] native HAnim hooks=%u\n", nativeHanim ? 1u : 0u);
            p3fes_native_frame_cuts::install(runtime);
            p3fes_native_ymisc::install(runtime);
            p3fes_debug_warp::install(runtime);
#if defined(PLATFORM_VITA)
            const char *nativeBgmOption = std::getenv("P3FES_NATIVE_BGM");
            p3fes_native_bgm_hooks::resetCounters();
            const bool nativeBgm = (!nativeBgmOption || *nativeBgmOption != '0') &&
                                   p3fes_native_bgm_hooks::install(runtime);
            std::fprintf(stderr, "[p3fes-native] native BGM hooks=%u\n", nativeBgm ? 1u : 0u);
#else
            // Desktop bench: opt-in only, output goes to a wall-clock paced null sink.
            const char *nativeBgmOption = std::getenv("P3FES_NATIVE_BGM");
            p3fes_native_bgm_hooks::resetCounters();
            const bool nativeBgm = nativeBgmOption && nativeBgmOption[0] == '1' && nativeBgmOption[1] == '\0' &&
                                   p3fes_native_bgm_hooks::install(runtime);
            if (nativeBgmOption && nativeBgmOption[0] == '1')
                std::fprintf(stderr, "[p3fes-native] native BGM hooks=%u (null sink)\n", nativeBgm ? 1u : 0u);
#endif
            PS2Runtime::RecompiledFunction sector = runtime.lookupFunction(kSectorDefaultRenderCallBack);
            const bool sectorHook = sector && sector != &p3fesSectorDefaultRender &&
                                    runtime.replaceFunction(kSectorDefaultRenderCallBack, &p3fesSectorDefaultRender);
            if (sectorHook)
                g_originalSectorRender.store(sector, std::memory_order_release);
            const bool ptankHook = installNativeOnlyHook(runtime, kPtankAtomicRender, &p3fesPtankAtomicRender);
            // Resume entries of the same function (see p3fesPtankAtomicRender).
            const bool ptankResume = installNativeOnlyHook(runtime, kPtankAtomicRender + 0x4Cu, &p3fesPtankAtomicRender) &&
                                     installNativeOnlyHook(runtime, kPtankAtomicRender + 0x6Cu, &p3fesPtankAtomicRender);
            if (ptankHook && !ptankResume)
                std::fprintf(stderr, "[p3fes-native] PTank resume entries not installed\n");
            const bool im3dTransform = installNativeOnlyHook(runtime, kRwIm3DTransform, &p3fesRwIm3DTransform);
            const bool im3dEnd = installNativeOnlyHook(runtime, kRwIm3DEnd, &p3fesRwIm3DEnd);
            const bool im3dIndexed = installNativeOnlyHook(runtime, kRwIm3DRenderIndexedPrimitive, &p3fesRwIm3DRenderIndexedPrimitive);
            const bool im3dPrim = installNativeOnlyHook(runtime, kRwIm3DRenderPrimitive, &p3fesRwIm3DRenderPrimitive);
            const bool im3dLine = installNativeOnlyHook(runtime, kRwIm3DRenderLine, &p3fesRwIm3DRenderLine);
            g_gsColorMasked = false;
            PS2Runtime::RecompiledFunction maskOn = runtime.lookupFunction(kGsColorMaskOn);
            PS2Runtime::RecompiledFunction maskOff = runtime.lookupFunction(kGsColorMaskOff);
            // P3FES_GS_COLOR_MASK=0 leaves the bracket unhooked (A/B against the old behaviour).
            const char *colorMaskOption = std::getenv("P3FES_GS_COLOR_MASK");
            if ((!colorMaskOption || *colorMaskOption != '0') && maskOn && maskOff &&
                maskOn != &p3fesGsColorMaskOn && maskOff != &p3fesGsColorMaskOff)
            {
                g_originalColorMaskOn.store(maskOn, std::memory_order_release);
                g_originalColorMaskOff.store(maskOff, std::memory_order_release);
                runtime.replaceFunction(kGsColorMaskOn, &p3fesGsColorMaskOn);
                runtime.replaceFunction(kGsColorMaskOff, &p3fesGsColorMaskOff);
            }
            PS2Runtime::RecompiledFunction clear = runtime.lookupFunction(kRwCameraClear);
            const bool clearHook = clear && clear != &p3fesRwCameraClear &&
                                   runtime.replaceFunction(kRwCameraClear, &p3fesRwCameraClear);
            if (clearHook)
                g_originalCameraClear.store(clear, std::memory_order_release);
            PS2Runtime::RecompiledFunction sofdecUpdate = runtime.lookupFunction(kSofdecUpdateTask);
            if (sofdecUpdate && sofdecUpdate != &p3fesSofdecUpdateTask)
            {
                g_originalSofdecUpdateTask.store(sofdecUpdate, std::memory_order_release);
                runtime.replaceFunction(kSofdecUpdateTask, &p3fesSofdecUpdateTask);
            }
            PS2Runtime::RecompiledFunction sofdecDestroy = runtime.lookupFunction(kSofdecDestroyTask);
            if (sofdecDestroy && sofdecDestroy != &p3fesSofdecDestroyTask)
            {
                g_originalSofdecDestroyTask.store(sofdecDestroy, std::memory_order_release);
                runtime.replaceFunction(kSofdecDestroyTask, &p3fesSofdecDestroyTask);
            }
            PS2Runtime::RecompiledFunction show = runtime.lookupFunction(kRwCameraShowRaster);
            if (show && show != &p3fesRwCameraShowRaster)
            {
                g_originalShowRaster.store(show, std::memory_order_release);
                runtime.replaceFunction(kRwCameraShowRaster, &p3fesRwCameraShowRaster);
            }
            const bool im2dLine = installNativeOnlyHook(runtime, kSkyIm2DRenderLine, &p3fesSkyIm2DRenderLine);
            const bool im2dTri = installNativeOnlyHook(runtime, kSkyIm2DRenderTriangle, &p3fesSkyIm2DRenderTriangle);
            const bool im2dPrim = installNativeOnlyHook(runtime, kSkyIm2DRenderPrimitive, &p3fesSkyIm2DRenderPrimitive);
            const bool im2dIndexed = installNativeOnlyHook(runtime, kSkyIm2DRenderIndexedPrimitive, &p3fesSkyIm2DRenderIndexedPrimitive);
            // Strict native mode: never let the host GS consumer paint an
            // interlaced fallback over the HLE framebuffer, even before the
            // first supported 3-D atomic appears.
            p3fes_native_present::setActive(true);
            std::fprintf(stderr, "[p3fes-native] progressive native-only presentation enabled; sector=%u ptank=%u clear=%u Im3D=%u%u%u%u%u Im2D=%u%u%u%u\n",
                         sectorHook ? 1u : 0u, ptankHook ? 1u : 0u, clearHook ? 1u : 0u,
                         im3dTransform ? 1u : 0u, im3dEnd ? 1u : 0u, im3dIndexed ? 1u : 0u,
                         im3dPrim ? 1u : 0u, im3dLine ? 1u : 0u,
                         im2dLine ? 1u : 0u, im2dTri ? 1u : 0u,
                         im2dPrim ? 1u : 0u, im2dIndexed ? 1u : 0u);
        }
        else
            p3fes_debug_warp::install(runtime); // P3FES_RW_HOOK_DIAG without native rendering
#else
        (void)nativeEnabled;
#endif
    }
}

// CRC32 is for the USA v1.00 SLUS_216.21 used by this port (SHA-1
// 3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb).
PS2_REGISTER_GAME_OVERRIDE("p3fes-renderware",
                           "SLUS_216.21",
                           0x00100008u,
                           0x9B2AF129u,
                           applyP3fesRenderWare)

namespace ps2_game_overrides
{
    void forceLinkP3fesRenderWare() {}
}
