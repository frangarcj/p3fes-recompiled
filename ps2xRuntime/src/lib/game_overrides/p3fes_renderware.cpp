// Persona 3 FES RenderWare integration point.
//
// Guest offsets below are cross-checked against C-GBL/P3FES-Decompilation
// (render_view.h/render_rw5.h/render_rw6.h, commit 78bb5776). In particular,
// the game's normal model path does not go through RpClumpRender: every atomic
// reaches AtomicDefaultRenderCallBack, which is the useful native-renderer seam.
// The hook is opt-in and still calls the original until the native path can
// submit the corresponding geometry itself.
// RenderWare camera -> OpenGL matrix conversion is adapted from
// Rinnegatamante/librw-vita @ 5e14b3f8123f8b950917ae46748dd655c723b09f,
// src/gl/gl3device.cpp (MIT), for the same positive-Z RenderWare camera model;
// see LICENSE.librw-vita.txt beside this file.
#include "game_overrides.h"
#include "p3fes_native_present.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#if !defined(__ANDROID__)
#include "native_renderer/renderer.h"
#include "runtime/gs/ps2_gs_psmt4.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr uint32_t kAtomicDefaultRenderCallBack = 0x00490860u;
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
    constexpr uint32_t kRwCameraShowRaster = 0x004C9ED0u;
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
    std::atomic<PS2Runtime::RecompiledFunction> g_originalShowRaster{nullptr};
    std::atomic<bool> g_nativeEnabled{false};
    std::atomic<bool> g_diagEnabled{false};
    std::atomic<bool> g_diagPrinted{false};
    std::atomic<bool> g_im2dDiagPrinted{false};
    std::atomic<bool> g_im2dTextureDiagPrinted{false};
    std::atomic<bool> g_sectorDiagPrinted{false};
    std::atomic<bool> g_ptankDiagPrinted{false};
    std::atomic<bool> g_im3dDiagPrinted{false};

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

    std::unordered_set<MeshId> g_definedMeshes;
    std::unordered_map<uint32_t, uint64_t> g_rasterRevisions;
    std::unordered_map<uint32_t, std::string> g_frameRasterTextures;
    bool g_nativeFrameActive = false;
    uint32_t g_nativeErrors = 0u;

    bool decodeNativeRaster(const uint8_t *rdram, uint32_t raster, TextureData &texture, uint64_t &revision);
    bool prepareNativeRasterTexture(const uint8_t *rdram, uint32_t raster, std::string &textureName);
    bool readMaterialUvTransform(const uint8_t *rdram, uint32_t material,
                                 MeshDrawState::UvTransform &out);
    bool readMaterialDualPass(const uint8_t *rdram, uint32_t material, MeshDrawState::DualPass &out);
    bool readMaterialEnvPass(const uint8_t *rdram, uint32_t material, MeshDrawState::EnvPass &out);
    bool readTextureSampler(const uint8_t *rdram, uint32_t texture, MeshDrawState::SamplerState &out);

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
        state.materialColors.assign(static_cast<size_t>(materialCount), {255u, 255u, 255u, 255u});
        state.materialSamplers.resize(static_cast<size_t>(materialCount));
        state.materialUvTransforms.resize(static_cast<size_t>(materialCount));
        state.materialDualPasses.resize(static_cast<size_t>(materialCount));
        state.materialEnvPasses.resize(static_cast<size_t>(materialCount));
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
                readTextureSampler(rdram, texture, state.materialSamplers[static_cast<size_t>(i)]);
            readMaterialUvTransform(rdram, material, state.materialUvTransforms[static_cast<size_t>(i)]);
            readMaterialDualPass(rdram, material, state.materialDualPasses[static_cast<size_t>(i)]);
            readMaterialEnvPass(rdram, material, state.materialEnvPasses[static_cast<size_t>(i)]);
        }
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
        if (state.blendEnabled && readGuest(rdram, kRwSrcBlend, srcBlend) && readGuest(rdram, kRwDstBlend, dstBlend) &&
            srcBlend >= 1u && srcBlend <= 11u && dstBlend >= 1u && dstBlend <= 11u)
        {
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

        std::unordered_set<uint32_t> seenLocalLights;
        const auto appendLocalLight = [&](uint32_t light) {
            if (!light || !seenLocalLights.insert(light).second || state.localLights.size() >= 8u ||
                !guestPointer(light, kLightWorldLinkOffset + 8u))
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

            const float invwx = 1.0f / viewWindowX;
            const float invwy = 1.0f / viewWindowY;
            const float invz = 1.0f / (farPlane - nearPlane);
            const float ox = viewOffsetX * invwx;
            const float oy = viewOffsetY * invwy;
            if (projection == 1) // rwPERSPECTIVE
            {
                out.projection = {
                    invwx, 0.0f, 0.0f, 0.0f,
                    0.0f, invwy, 0.0f, 0.0f,
                    ox, oy, (farPlane + nearPlane) * invz, 1.0f,
                    -ox, -oy, -2.0f * nearPlane * farPlane * invz, 0.0f,
                };
            }
            else // rwPARALLEL
            {
                out.projection = {
                    invwx, 0.0f, 0.0f, 0.0f,
                    0.0f, invwy, 0.0f, 0.0f,
                    ox, oy, -(farPlane + nearPlane) * invz, 0.0f,
                    -ox, -oy, 2.0f * invz, 1.0f,
                };
            }
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

    bool prepareNativeSkinPalette(uint8_t *rdram, uint32_t atomic, uint32_t geometry,
                                  R5900Context *ctx, PS2Runtime *runtime, MeshDrawState &state)
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

        PS2Runtime::RecompiledFunction update = runtime->lookupFunction(kSkinMatrixPaletteUpdate);
        if (!update)
            return false;

        // Reuse RenderWare's own palette builder. This preserves all of its
        // hierarchy flag/inverse-bind/inverse-atomic-LTM rules and avoids a
        // second subtly different matrix implementation. The original skin
        // render path calls this same helper before VU1 submission.
        ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
        ctx->r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(skin));
        update(rdram, ctx, runtime);
        const uint32_t palette = getRegU32(ctx, 2);
        const uint32_t boneCount = static_cast<uint32_t>(boneCountSigned);
        if (!guestPointer(palette, boneCount * 64u))
            return false;

        state.skinPalette.resize(boneCount);
        for (uint32_t bone = 0u; bone < boneCount; ++bone)
            std::memcpy(state.skinPalette[bone].data(), rdram + palette + bone * 64u, 64u);
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
        const bool hasNormals = guestPointer(normals, summary.vertices * 12u);
        const bool hasPrelight = guestPointer(summary.prelitData, summary.vertices * 4u);
        const bool hasUv = guestPointer(summary.uv0Data, summary.vertices * 8u);
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
            return false;

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
            return false;

        const bool hasNormals = guestPointer(normals, vertexCount * 4u);
        const bool hasUv = guestPointer(uv0, vertexCount * 8u);
        const bool hasPrelight = guestPointer(prelit, vertexCount * 4u);
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
            return false;
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
            mesh.materials[localMaterial].textureName = std::move(textureName);
        }
        meshId = (1ull << 63u) | sector;
        return !mesh.indices.empty();
    }

    bool ensureNativeFrame(uint8_t *rdram)
    {
        if (g_nativeFrameActive)
            return true;
        Camera camera;
        if (!readCurrentCamera(rdram, camera))
            return false;
        p3fes_native_present::beginFrame(camera, ClearColor{});
        g_nativeFrameActive = true;
        return true;
    }

    bool drawNativeAtomic(uint8_t *rdram, uint32_t atomic, R5900Context *ctx, PS2Runtime *runtime,
                          const MeshDrawState::BlendState *blendOverride = nullptr)
    {
        AtomicSummary summary;
        if (!inspectAtomic(rdram, atomic, summary) || (summary.flags & kAtomicRenderFlag) == 0u)
            return false;

        if (!ensureNativeFrame(rdram))
            return false;

        MeshId meshId = 0u;
        const int16_t morph = [&] { int16_t value = 0; readGuest(rdram, atomic + kAtomicCurrentMorphOffset, value); return value; }();
        const uint32_t morphIndex = morph >= 0 && static_cast<uint32_t>(morph) < summary.morphTargets ? static_cast<uint32_t>(morph) : 0u;
        meshId = (static_cast<MeshId>(summary.geometry) << 16u) | morphIndex;
        if (g_definedMeshes.find(meshId) == g_definedMeshes.end())
        {
            MeshData mesh;
            MeshId decodedId = 0u;
            if (!decodeNativeMesh(rdram, summary, mesh, decodedId) || decodedId != meshId)
            {
                if (g_nativeErrors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] mesh 0x%08X decode failed\n", summary.geometry);
                return false;
            }
            p3fes_native_present::defineMesh(meshId, mesh);
            g_definedMeshes.insert(meshId);
        }

        Matrix4 model{};
        if (!readFrameMatrix(rdram, summary.frame, model))
            return false;
        MeshDrawState drawState;
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
        if (!prepareNativeSkinPalette(rdram, atomic, summary.geometry, ctx, runtime, drawState))
        {
            if (g_nativeErrors++ < 8u)
                std::fprintf(stderr, "[p3fes-native] skin palette failed for atomic 0x%08X geometry 0x%08X\n",
                             atomic, summary.geometry);
        }
        p3fes_native_present::drawMesh(meshId, model, std::move(drawState));
        return true;
    }

    bool drawNativeSector(uint8_t *rdram, uint32_t sector)
    {
        if (!ensureNativeFrame(rdram))
            return false;
        const MeshId meshId = (1ull << 63u) | sector;
        MeshDrawState drawState;
        if (g_definedMeshes.find(meshId) == g_definedMeshes.end())
        {
            MeshData mesh;
            MeshId decodedId = 0u;
            if (!decodeNativeSector(rdram, sector, mesh, decodedId, drawState) || decodedId != meshId)
            {
                if (g_nativeErrors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] sector 0x%08X decode failed\n", sector);
                return false;
            }
            p3fes_native_present::defineMesh(meshId, mesh);
            g_definedMeshes.insert(meshId);
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
                            readTextureSampler(rdram, texture, drawState.materialSamplers[localMaterial]);
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
        float r, g, b, a;
        float extra[4];
    };
    static_assert(sizeof(RwIm2DVertex) == 64u);

    uint32_t reg32(const R5900Context *ctx, uint32_t reg)
    {
        return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[reg]));
    }

    uint8_t colorByte(float v)
    {
        if (!std::isfinite(v))
            return 0u;
        return static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255));
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
        ScreenDrawState state;
        uint64_t flags = 0u, alpha = 0u, clamp = 0u, tex1 = 0u, test = 0u;
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
        }

        if ((flags & kGsPrimAlphaBlendEnable) == 0u)
            state.blend = ScreenBlend::Opaque;
        else
        {
            switch (alpha & 0xFFu)
            {
            case 0x48u: state.blend = ScreenBlend::Additive; break;   // Cs * As + Cd
            case 0x42u: state.blend = ScreenBlend::Subtractive; break; // Cd - Cs * As
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

    uint64_t hashBytes(uint64_t hash, const uint8_t *bytes, size_t size)
    {
        constexpr uint64_t kPrime = 1099511628211ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= bytes[i];
            hash *= kPrime;
        }
        return hash;
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

    bool decodeNativeRaster(const uint8_t *rdram, uint32_t raster, TextureData &texture, uint64_t &revision)
    {
        RasterSummary info;
        if (!inspectRaster(rdram, raster, info) || info.width <= 0 || info.height <= 0 ||
            info.width > 4096 || info.height > 4096 || info.stride <= 0)
            return false;

        const uint32_t type = info.type & 0x07u;
        if (type != 4u && type != 5u) // texture / camera-texture only
            return false;

        uint32_t pluginOffset = 0u;
        uint8_t cacheFlags = 0u;
        uint32_t tex0 = 0u;
        if (readGuest(rdram, kRwRasterPluginOffset, pluginOffset) && pluginOffset &&
            guestPointer(raster + pluginOffset, 55u))
        {
            readGuest(rdram, raster + pluginOffset + 54u, cacheFlags);
            readGuest(rdram, raster + pluginOffset + 8u, tex0);
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

        const uint32_t tex0Tbw = (tex0 >> 14u) & 0x3Fu;
        const uint32_t tbw = tex0Tbw != 0u ? tex0Tbw : std::max(1u, (static_cast<uint32_t>(info.width) + 63u) / 64u);
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
                    if (swizzled8)
                    {
                        const uint32_t byteOffset = GSPSMT8::addrPSMT8(0u, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                        const uint64_t address = static_cast<uint64_t>(info.pixels) + byteOffset;
                        if (address >= PS2_RAM_SIZE)
                            return false;
                        index = rdram[static_cast<uint32_t>(address)];
                    }
                    else if (swizzled4)
                    {
                        const uint32_t nibbleOffset = GSPSMT4::addrPSMT4(0u, tbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                        const uint32_t byteOffset = nibbleOffset >> 1u;
                        const uint64_t address = static_cast<uint64_t>(info.pixels) + byteOffset;
                        if (address >= PS2_RAM_SIZE)
                            return false;
                        const uint8_t packed = rdram[static_cast<uint32_t>(address)];
                        index = (nibbleOffset & 1u) ? (packed >> 4u) : (packed & 0x0Fu);
                    }
                    else
                        index = info.depth == 8 ? src[x]
                            : ((x & 1) ? (src[x >> 1] >> 4u) : (src[x >> 1] & 0x0Fu));
                    // The sky driver keeps 256-entry CLUTs in GS CSM1 order
                    // (middle index bits swapped); 16-entry CLUTs stay linear.
                    if (info.depth == 8)
                        index = (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
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
        constexpr uint64_t kFnvOffset = 1469598103934665603ull;
        uint64_t hash = hashBytes(kFnvOffset, texture.rgba.data(), texture.rgba.size());
        hash ^= (static_cast<uint64_t>(static_cast<uint32_t>(info.width)) << 32u) |
                static_cast<uint32_t>(info.height);
        revision = hash ? hash : 1u;

        if (!g_im2dTextureDiagPrinted.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[p3fes-native] first native texture: raster=0x%08X type=%u fmt=0x%02X %dx%d depth=%d swizzle=%u\n",
                         raster, type, info.format, info.width, info.height, info.depth,
                         swizzled8 ? 8u : (swizzled4 ? 4u : 0u));
        return true;
    }

    bool prepareNativeRasterTexture(const uint8_t *rdram, uint32_t raster, std::string &textureName)
    {
        if (!raster)
            return false;
        // RenderWare commonly submits the same raster many times in one frame
        // (fonts, movie quads, layered UI). Decoding and hashing the CPU raster
        // for every primitive dominated the game thread during the intro. A
        // raster can be considered stable across one CameraShowRaster frame;
        // dynamic rasters are rechecked on the next frame, preserving the
        // existing content-revision upload path without repeated per-draw work.
        const auto frameCached = g_frameRasterTextures.find(raster);
        if (frameCached != g_frameRasterTextures.end())
        {
            textureName = frameCached->second;
            return true;
        }
        TextureData texture;
        uint64_t revision = 0u;
        if (!decodeNativeRaster(rdram, raster, texture, revision))
            return false;
        textureName = texture.name;
        const auto known = g_rasterRevisions.find(raster);
        if (known == g_rasterRevisions.end() || known->second != revision)
        {
            p3fes_native_present::defineTexture(texture, revision);
            g_rasterRevisions[raster] = revision;
        }
        g_frameRasterTextures.emplace(raster, textureName);
        return true;
    }

    bool prepareNativeIm2DTexture(const uint8_t *rdram, std::string &textureName)
    {
        uint32_t raster = 0u;
        if (!readGuest(rdram, kRwCurrentRaster, raster) || !raster)
            return false;
        return prepareNativeRasterTexture(rdram, raster, textureName);
    }

    bool drawNativeIm2DTriangles(uint8_t *rdram, uint32_t vertices, uint32_t vertexCount,
                                 const std::vector<uint16_t> &triangles)
    {
        if (vertexCount == 0u || vertexCount > 65535u || triangles.empty())
            return false;
        std::string textureName;
        if (nativeIm2DTextured(rdram) && !prepareNativeIm2DTexture(rdram, textureName))
            return false;
        std::vector<ScreenVertex> out;
        out.reserve(triangles.size());
        for (uint16_t index : triangles)
        {
            if (index >= vertexCount)
                return false;
            ScreenVertex v;
            if (!readIm2DVertex(rdram, vertices, index, v))
                return false;
            out.push_back(v);
        }
        if (!ensureNativeFrame(rdram))
            return false;
        p3fes_native_present::drawScreenTriangles(std::move(out), kRwScreenWidth, kRwScreenHeight,
                                                  std::move(textureName), nativeIm2DState(rdram));
        return true;
    }

    bool drawNativeIm2DLines(uint8_t *rdram, uint32_t vertices, uint32_t vertexCount,
                             const std::vector<uint16_t> &lines)
    {
        if (vertexCount == 0u || vertexCount > 65535u || lines.empty())
            return false;
        std::string textureName;
        if (nativeIm2DTextured(rdram) && !prepareNativeIm2DTexture(rdram, textureName))
            return false;
        std::vector<ScreenVertex> out;
        out.reserve(lines.size());
        for (uint16_t index : lines)
        {
            if (index >= vertexCount)
                return false;
            ScreenVertex v;
            if (!readIm2DVertex(rdram, vertices, index, v))
                return false;
            out.push_back(v);
        }
        if (!ensureNativeFrame(rdram))
            return false;
        p3fes_native_present::drawScreenLines(std::move(out), kRwScreenWidth, kRwScreenHeight,
                                             std::move(textureName), nativeIm2DState(rdram));
        return true;
    }

    std::vector<uint16_t> im2dLineList(uint32_t primitive, uint32_t count,
                                      const uint16_t *indices = nullptr)
    {
        std::vector<uint16_t> out;
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

    std::vector<uint16_t> im2dTriangleList(uint32_t primitive, uint32_t count,
                                          const uint16_t *indices = nullptr)
    {
        std::vector<uint16_t> out;
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
                    readNativeGlobalMeshState(rdram, state);
                    if (!nativeIm2DTextured(rdram) || prepareNativeIm2DTexture(rdram, textureName))
                        p3fes_native_present::drawWorldLines(std::move(vertices), model, std::move(textureName), std::move(state));
                }
            }
        }
        ctx->r[2] = _mm_cvtsi32_si128(valid ? 1 : 0);
    }

    void p3fesRwIm3DEnd(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (!ctx)
            return;
        const bool wasOpen = g_nativeIm3D.open;
        g_nativeIm3D = {};
        ctx->r[2] = _mm_cvtsi32_si128(wasOpen ? 1 : 0);
    }

    void nativeIm2DResult(R5900Context *ctx)
    {
        ctx->r[2] = _mm_cvtsi32_si128(1); // every SkyIm2DRender* entry returns 1
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
            std::vector<uint16_t> indices(indexCount);
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
        p3fes_native_present::endFrame();
        g_nativeFrameActive = false;
        g_frameRasterTextures.clear();
        (void)runtime;
    }
#endif

    void p3fesAtomicDefaultRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx && ctx->pc == kAtomicDefaultRenderCallBack)
        {
            const uint32_t atomic = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            AtomicSummary summary;
            if (inspectAtomic(rdram, atomic, summary) && g_diagEnabled.load(std::memory_order_relaxed) &&
                !g_diagPrinted.exchange(true, std::memory_order_relaxed))
            {
                std::fprintf(stderr,
                             "[p3fes-rw] AtomicDefaultRender atomic=0x%08X frame=0x%08X geometry=0x%08X flags=0x%02X "
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
                // AtomicDefaultRenderCallBack returns the atomic on success.
                // Native submission itself is the success path here, and
                // callers do use $v0 as the RenderWare callback result.
                ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
                ctx->r[2] = ctx->r[4];
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
                return;
            }
#endif
        }
        if (PS2Runtime::RecompiledFunction original = g_originalSectorRender.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void p3fesPtankAtomicRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
#if !defined(__ANDROID__)
        if (!ctx || !runtime)
            return;
        const uint32_t atomic = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        uint32_t pluginOffset = 0u, tank = 0u;
        int32_t count = 0;
        const auto atomicExtension = [&]() -> uint32_t {
            const uint64_t address = static_cast<uint64_t>(atomic) + pluginOffset;
            return address <= 0xFFFFFFFFull ? static_cast<uint32_t>(address) : 0xFFFFFFFFu;
        };
        if (readGuest(rdram, kPtankAtomicExtensionOffset, pluginOffset) && pluginOffset != 0u &&
            guestPointer(atomicExtension(), sizeof(uint32_t)) &&
            readGuest(rdram, atomicExtension(), tank) && tank && guestPointer(tank, 104u) &&
            readGuest(rdram, tank + 4u, count) && count > 0)
        {
            uint32_t lockCallback = 0u, instanceCallback = 0u;
            readGuest(rdram, tank + 28u, lockCallback);
            readGuest(rdram, tank + 32u, instanceCallback);

            if (lockCallback)
            {
                uint32_t lockArg = 0u;
                readGuest(rdram, tank + 64u, lockArg);
                // Original sub_474160 clears this in the jalr delay slot, even
                // if the callback itself later fails. Preserve that state
                // transition before dispatching the dynamic callback.
                const uint32_t zero = 0u;
                std::memcpy(rdram + tank + 64u, &zero, sizeof(zero));
                if (PS2Runtime::RecompiledFunction lock = runtime->lookupFunction(lockCallback))
                {
                    ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
                    ctx->r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(tank + 80u));
                    ctx->r[6] = _mm_cvtsi32_si128(count);
                    ctx->r[7] = _mm_cvtsi32_si128(static_cast<int32_t>(lockArg));
                    lock(rdram, ctx, runtime);
                }
            }

            uint32_t instanceResult = 1u;
            if (instanceCallback)
            {
                if (PS2Runtime::RecompiledFunction instance = runtime->lookupFunction(instanceCallback))
                {
                    ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
                    ctx->r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(tank + 80u));
                    ctx->r[6] = _mm_cvtsi32_si128(count);
                    instance(rdram, ctx, runtime);
                    instanceResult = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[2]));
                }
                else
                    instanceResult = 0u;
            }

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
                drawn = drawNativeAtomic(rdram, atomic, ctx, runtime, blend.enabled ? &blend : nullptr);
            if (!g_ptankDiagPrinted.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[p3fes-native] first PTank: atomic=0x%08X count=%d lock=0x%08X instance=0x%08X result=%u native=%u\n",
                             atomic, count, lockCallback, instanceCallback, instanceResult, drawn ? 1u : 0u);
        }
        ctx->r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(atomic));
        ctx->r[2] = ctx->r[4];
#else
        (void)rdram;
        (void)ctx;
        (void)runtime;
#endif
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

    void applyP3fesRenderWare(PS2Runtime &runtime)
    {
        const char *native = std::getenv("P3FES_NATIVE_RW");
        const char *diag = std::getenv("P3FES_RW_HOOK_DIAG");
        const bool nativeEnabled = native && *native && *native != '0';
        const bool diagEnabled = diag && *diag && *diag != '0';
        if (!nativeEnabled && !diagEnabled)
            return;

        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kAtomicDefaultRenderCallBack);
        if (!original || original == &p3fesAtomicDefaultRender)
            return;

        g_diagEnabled.store(diagEnabled, std::memory_order_relaxed);
        g_nativeEnabled.store(nativeEnabled, std::memory_order_relaxed);
        g_diagPrinted.store(false, std::memory_order_relaxed);
        g_im2dDiagPrinted.store(false, std::memory_order_relaxed);
        g_im2dTextureDiagPrinted.store(false, std::memory_order_relaxed);
        g_sectorDiagPrinted.store(false, std::memory_order_relaxed);
        g_ptankDiagPrinted.store(false, std::memory_order_relaxed);
        g_im3dDiagPrinted.store(false, std::memory_order_relaxed);
        g_nativeIm3D = {};
        g_originalAtomicRender.store(original, std::memory_order_release);
        const bool replaced = runtime.replaceFunction(kAtomicDefaultRenderCallBack, &p3fesAtomicDefaultRender);
        if (diagEnabled)
            std::fprintf(stderr, "[p3fes-rw] AtomicDefaultRender hook %s at 0x%08X\n",
                         replaced ? "installed" : "failed", kAtomicDefaultRenderCallBack);

#if !defined(__ANDROID__)
        if (nativeEnabled)
        {
            PS2Runtime::RecompiledFunction sector = runtime.lookupFunction(kSectorDefaultRenderCallBack);
            const bool sectorHook = sector && sector != &p3fesSectorDefaultRender &&
                                    runtime.replaceFunction(kSectorDefaultRenderCallBack, &p3fesSectorDefaultRender);
            if (sectorHook)
                g_originalSectorRender.store(sector, std::memory_order_release);
            const bool ptankHook = installNativeOnlyHook(runtime, kPtankAtomicRender, &p3fesPtankAtomicRender);
            const bool im3dTransform = installNativeOnlyHook(runtime, kRwIm3DTransform, &p3fesRwIm3DTransform);
            const bool im3dEnd = installNativeOnlyHook(runtime, kRwIm3DEnd, &p3fesRwIm3DEnd);
            const bool im3dIndexed = installNativeOnlyHook(runtime, kRwIm3DRenderIndexedPrimitive, &p3fesRwIm3DRenderIndexedPrimitive);
            const bool im3dPrim = installNativeOnlyHook(runtime, kRwIm3DRenderPrimitive, &p3fesRwIm3DRenderPrimitive);
            const bool im3dLine = installNativeOnlyHook(runtime, kRwIm3DRenderLine, &p3fesRwIm3DRenderLine);
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
            std::fprintf(stderr, "[p3fes-native] progressive native-only presentation enabled; sector=%u ptank=%u Im3D=%u%u%u%u%u Im2D=%u%u%u%u\n",
                         sectorHook ? 1u : 0u, ptankHook ? 1u : 0u,
                         im3dTransform ? 1u : 0u, im3dEnd ? 1u : 0u, im3dIndexed ? 1u : 0u,
                         im3dPrim ? 1u : 0u, im3dLine ? 1u : 0u,
                         im2dLine ? 1u : 0u, im2dTri ? 1u : 0u,
                         im2dPrim ? 1u : 0u, im2dIndexed ? 1u : 0u);
        }
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
