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
    constexpr uint32_t kTriangleMaterialOffset = 0x06u;
    constexpr uint32_t kMaterialTextureOffset = 0x00u;
    constexpr uint32_t kMaterialColorOffset = 0x04u;
    constexpr uint32_t kTextureRasterOffset = 0x00u;
    constexpr uint32_t kSectorTrianglesOffset = 0x04u;
    constexpr uint32_t kSectorVerticesOffset = 0x08u;
    constexpr uint32_t kSectorNormalsOffset = 0x0Cu;
    constexpr uint32_t kSectorUv0Offset = 0x10u;
    constexpr uint32_t kSectorPrelitOffset = 0x30u;
    constexpr uint32_t kSectorMaterialBaseOffset = 0x80u;
    constexpr uint32_t kSectorNumTrianglesOffset = 0x84u;
    constexpr uint32_t kWorldMaterialsOffset = 0x10u;
    constexpr uint32_t kWorldNumMaterialsOffset = 0x14u;
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
    constexpr uint32_t kRwCameraShowRaster = 0x004C9ED0u;
    constexpr uint32_t kSkyIm2DRenderLine = 0x004ECA60u;
    constexpr uint32_t kSkyIm2DRenderTriangle = 0x004ECCE0u;
    constexpr uint32_t kSkyIm2DRenderPrimitive = 0x004EDBA0u;
    constexpr uint32_t kSkyIm2DRenderIndexedPrimitive = 0x004EEB50u;
    constexpr uint32_t kRwGp = 0x007D2CF0u;
    constexpr uint32_t kRwRenderFlags = kRwGp - 20272u;
    constexpr uint32_t kRwRasterPluginOffset = kRwGp - 16968u;
    constexpr uint32_t kRwClamp1 = kRwGp - 17048u;
    constexpr uint32_t kRwTex1 = kRwGp - 17040u;
    constexpr uint32_t kRwAlpha1 = kRwGp - 17032u;
    constexpr uint32_t kRwCurrentRaster = kRwGp - 16948u;
    constexpr uint64_t kGsPrimTextureEnable = 0x10u;
    constexpr uint64_t kGsPrimAlphaBlendEnable = 0x40u;
    constexpr float kRwScreenWidth = 640.0f;
    constexpr float kRwScreenHeight = 448.0f;
    constexpr uint32_t kMaxVertices = 1u << 20;
    constexpr uint32_t kMaxTriangles = 1u << 20;

    std::atomic<PS2Runtime::RecompiledFunction> g_originalAtomicRender{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalSectorRender{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalShowRaster{nullptr};
    std::atomic<bool> g_nativeEnabled{false};
    std::atomic<bool> g_diagEnabled{false};
    std::atomic<bool> g_diagPrinted{false};
    std::atomic<bool> g_im2dDiagPrinted{false};
    std::atomic<bool> g_im2dTextureDiagPrinted{false};
    std::atomic<bool> g_sectorDiagPrinted{false};

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
    bool g_nativeFrameActive = false;
    uint32_t g_nativeErrors = 0u;

    bool decodeNativeRaster(const uint8_t *rdram, uint32_t raster, TextureData &texture, uint64_t &revision);

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
        for (int32_t i = 0; i < materialCount; ++i)
        {
            uint32_t material = 0u;
            if (!readGuest(rdram, materialArray + static_cast<uint32_t>(i) * 4u, material) ||
                !material || !guestPointer(material, 8u))
                continue;
            std::memcpy(state.materialColors[static_cast<size_t>(i)].data(),
                        rdram + material + kMaterialColorOffset, 4u);
        }
    }

    bool readVec3(const uint8_t *rdram, uint32_t address, std::array<float, 3> &out)
    {
        return readGuest(rdram, address + 0u, out[0]) &&
               readGuest(rdram, address + 4u, out[1]) &&
               readGuest(rdram, address + 8u, out[2]);
    }

    bool readFrameMatrix(const uint8_t *rdram, uint32_t frame, Matrix4 &out)
    {
        if (!guestPointer(frame, kFrameLtmOffset + 64u))
            return false;
        const uint32_t m = frame + kFrameLtmOffset;
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

            uint32_t texture = 0u, raster = 0u;
            if (!readGuest(rdram, material + kMaterialTextureOffset, texture) || !texture || !guestPointer(texture, 4u) ||
                !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster)
                continue;
            TextureData nativeTexture;
            uint64_t revision = 0u;
            if (!decodeNativeRaster(rdram, raster, nativeTexture, revision))
                continue;
            mesh.materials[materialIndex].textureName = nativeTexture.name;
            const auto known = g_rasterRevisions.find(raster);
            if (known == g_rasterRevisions.end() || known->second != revision)
            {
                p3fes_native_present::defineTexture(nativeTexture, revision);
                g_rasterRevisions[raster] = revision;
            }
        }
        return true;
    }

    bool decodeNativeSector(const uint8_t *rdram, uint32_t sector, MeshData &mesh, MeshId &meshId,
                            MeshDrawState &drawState)
    {
        uint16_t triangleCount = 0u, materialBase = 0u;
        uint32_t triangles = 0u, positions = 0u, normals = 0u, uv0 = 0u, prelit = 0u;
        uint32_t world = 0u, materialArray = 0u;
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
            drawState.materialColors[localMaterial] = mesh.materials[localMaterial].color;

            uint32_t texture = 0u, raster = 0u;
            if (!readGuest(rdram, material + kMaterialTextureOffset, texture) || !texture || !guestPointer(texture, 4u) ||
                !readGuest(rdram, texture + kTextureRasterOffset, raster) || !raster)
                continue;
            TextureData nativeTexture;
            uint64_t revision = 0u;
            if (!decodeNativeRaster(rdram, raster, nativeTexture, revision))
                continue;
            mesh.materials[localMaterial].textureName = nativeTexture.name;
            const auto known = g_rasterRevisions.find(raster);
            if (known == g_rasterRevisions.end() || known->second != revision)
            {
                p3fes_native_present::defineTexture(nativeTexture, revision);
                g_rasterRevisions[raster] = revision;
            }
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

    bool drawNativeAtomic(uint8_t *rdram, uint32_t atomic)
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
                for (uint32_t localMaterial = 0u; localMaterial < materialCount; ++localMaterial)
                {
                    uint32_t material = 0u;
                    const uint32_t worldMaterial = materialBase + localMaterial;
                    if (readGuest(rdram, materialArray + worldMaterial * 4u, material) && material && guestPointer(material, 8u))
                        std::memcpy(drawState.materialColors[localMaterial].data(),
                                    rdram + material + kMaterialColorOffset, 4u);
                }
            }
        }
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
        uint64_t flags = 0u, alpha = 0u, clamp = 0u, tex1 = 0u;
        readGuest(rdram, kRwRenderFlags, flags);
        readGuest(rdram, kRwAlpha1, alpha);
        readGuest(rdram, kRwClamp1, clamp);
        readGuest(rdram, kRwTex1, tex1);

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

    bool prepareNativeIm2DTexture(const uint8_t *rdram, std::string &textureName)
    {
        uint32_t raster = 0u;
        if (!readGuest(rdram, kRwCurrentRaster, raster) || !raster)
            return false;
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
        return true;
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
                drawNativeAtomic(rdram, atomic);
                // AtomicDefaultRenderCallBack returns the atomic on success.
                // Native submission itself is the success path here, and
                // callers do use $v0 as the RenderWare callback result.
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
            std::fprintf(stderr, "[p3fes-native] progressive native-only presentation enabled; sector=%u Im2D=%u%u%u%u\n",
                         sectorHook ? 1u : 0u, im2dLine ? 1u : 0u, im2dTri ? 1u : 0u,
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
