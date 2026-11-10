// Persona 3 FES RenderWare integration point.
//
// Guest offsets below are cross-checked against C-GBL/P3FES-Decompilation
// (render_view.h/render_rw5.h/render_rw6.h, commit 78bb5776). In particular,
// the game's normal model path does not go through RpClumpRender: every atomic
// reaches AtomicDefaultRenderCallBack, which is the useful native-renderer seam.
// The hook is opt-in and still calls the original until the native path can
// submit the corresponding geometry itself.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/gs/gs_host_present.h"
#include "runtime/ps2_memory.h"
#if !defined(__ANDROID__)
#include "native_renderer/renderer.h"
#endif

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr uint32_t kAtomicDefaultRenderCallBack = 0x00490860u;
    constexpr uint32_t kAtomicFlagsOffset = 0x02u;
    constexpr uint32_t kAtomicFrameOffset = 0x04u;
    constexpr uint32_t kAtomicGeometryOffset = 0x18u;
    constexpr uint32_t kAtomicCurrentMorphOffset = 0x50u;
    constexpr uint8_t kAtomicRenderFlag = 0x04u;

    constexpr uint32_t kGeometryFlagsOffset = 0x08u;
    constexpr uint32_t kGeometryNumTrianglesOffset = 0x10u;
    constexpr uint32_t kGeometryNumVerticesOffset = 0x14u;
    constexpr uint32_t kGeometryNumMorphTargetsOffset = 0x18u;
    constexpr uint32_t kGeometryTrianglesOffset = 0x2Cu;
    constexpr uint32_t kGeometryPrelitOffset = 0x30u;
    constexpr uint32_t kGeometryUv0Offset = 0x34u;
    constexpr uint32_t kGeometryMorphTargetsOffset = 0x5Cu;
    constexpr uint32_t kMorphVerticesOffset = 0x14u;
    constexpr uint32_t kMorphNormalsOffset = 0x18u;
    constexpr uint32_t kMorphSize = 0x1Cu;
    constexpr uint32_t kFrameLtmOffset = 0x50u;
    constexpr uint32_t kRwCurrentCamera = 0x00960070u;
    constexpr uint32_t kCameraFrameOffset = 0x04u;
    constexpr uint32_t kCameraViewWindowYOffset = 0x6Cu;
    constexpr uint32_t kRwCameraShowRaster = 0x004C9ED0u;
    constexpr uint32_t kMaxVertices = 1u << 20;
    constexpr uint32_t kMaxTriangles = 1u << 20;

    std::atomic<PS2Runtime::RecompiledFunction> g_originalAtomicRender{nullptr};
    std::atomic<PS2Runtime::RecompiledFunction> g_originalShowRaster{nullptr};
    std::atomic<bool> g_nativeEnabled{false};
    std::atomic<bool> g_diagEnabled{false};
    std::atomic<bool> g_diagPrinted{false};

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
    using p3fes::native_renderer::MeshId;
    using p3fes::native_renderer::Renderer;
    using p3fes::native_renderer::Submesh;
    using p3fes::native_renderer::Vertex;

    std::unique_ptr<Renderer> g_nativeRenderer;
    std::unordered_set<MeshId> g_uploadedMeshes;
    bool g_nativeFrameActive = false;
    uint32_t g_nativeErrors = 0u;

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

        mesh.indices.reserve(static_cast<size_t>(summary.triangles) * 3u);
        for (uint32_t i = 0; i < summary.triangles; ++i)
        {
            const uint32_t tri = summary.triangleData + i * 8u;
            uint16_t a = 0u, b = 0u, c = 0u;
            if (!readGuest(rdram, tri + 0u, a) || !readGuest(rdram, tri + 2u, b) || !readGuest(rdram, tri + 4u, c) ||
                a >= summary.vertices || b >= summary.vertices || c >= summary.vertices)
                return false;
            mesh.indices.push_back(a);
            mesh.indices.push_back(b);
            mesh.indices.push_back(c);
        }
        mesh.submeshes.push_back(Submesh{0u, static_cast<uint32_t>(mesh.indices.size()), 0u});
        mesh.materials.push_back({{255u, 255u, 255u, 255u}, {}});
        return true;
    }

    bool ensureNativeRenderer()
    {
        if (!g_nativeRenderer)
            g_nativeRenderer = p3fes::native_renderer::createPlatformRenderer();
        if (!g_nativeRenderer)
            return false;
        std::string error;
        if (!g_nativeRenderer->open(960, 544, "Persona 3 FES native", error))
        {
            if (g_nativeErrors++ < 4u)
                std::fprintf(stderr, "[p3fes-native] renderer open failed: %s\n", error.c_str());
            return false;
        }
        return true;
    }

    bool drawNativeAtomic(uint8_t *rdram, uint32_t atomic)
    {
        AtomicSummary summary;
        if (!inspectAtomic(rdram, atomic, summary) || (summary.flags & kAtomicRenderFlag) == 0u || !ensureNativeRenderer())
            return false;

        if (!g_nativeFrameActive)
        {
            Camera camera;
            if (!readCurrentCamera(rdram, camera))
                return false;
            g_nativeRenderer->beginFrame(camera, ClearColor{});
            g_nativeFrameActive = true;
        }

        MeshId meshId = 0u;
        const int16_t morph = [&] { int16_t value = 0; readGuest(rdram, atomic + kAtomicCurrentMorphOffset, value); return value; }();
        const uint32_t morphIndex = morph >= 0 && static_cast<uint32_t>(morph) < summary.morphTargets ? static_cast<uint32_t>(morph) : 0u;
        meshId = (static_cast<MeshId>(summary.geometry) << 16u) | morphIndex;
        if (g_uploadedMeshes.find(meshId) == g_uploadedMeshes.end())
        {
            MeshData mesh;
            MeshId decodedId = 0u;
            std::string error;
            if (!decodeNativeMesh(rdram, summary, mesh, decodedId) || decodedId != meshId ||
                !g_nativeRenderer->uploadMesh(meshId, mesh, error))
            {
                if (g_nativeErrors++ < 8u)
                    std::fprintf(stderr, "[p3fes-native] mesh 0x%08X decode/upload failed%s%s\n",
                                 summary.geometry, error.empty() ? "" : ": ", error.c_str());
                return false;
            }
            g_uploadedMeshes.insert(meshId);
        }

        Matrix4 model{};
        if (!readFrameMatrix(rdram, summary.frame, model))
            return false;
        g_nativeRenderer->drawMesh(meshId, model);
        return true;
    }

    void finishNativeFrame(PS2Runtime *runtime)
    {
        if (!g_nativeFrameActive || !g_nativeRenderer)
            return;
        g_nativeRenderer->endFrame();
        g_nativeFrameActive = false;
        (void)runtime;
        ps2xSetExternalPresentationActive(true);
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
                return;
            }
#endif
        }

        if (PS2Runtime::RecompiledFunction original = g_originalAtomicRender.load(std::memory_order_acquire))
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
        g_originalAtomicRender.store(original, std::memory_order_release);
        const bool replaced = runtime.replaceFunction(kAtomicDefaultRenderCallBack, &p3fesAtomicDefaultRender);
        if (diagEnabled)
            std::fprintf(stderr, "[p3fes-rw] AtomicDefaultRender hook %s at 0x%08X\n",
                         replaced ? "installed" : "failed", kAtomicDefaultRenderCallBack);

#if !defined(__ANDROID__)
        if (nativeEnabled)
        {
            PS2Runtime::RecompiledFunction show = runtime.lookupFunction(kRwCameraShowRaster);
            if (show && show != &p3fesRwCameraShowRaster)
            {
                g_originalShowRaster.store(show, std::memory_order_release);
                runtime.replaceFunction(kRwCameraShowRaster, &p3fesRwCameraShowRaster);
            }
            // Strict native mode: never let the host GS consumer paint an
            // interlaced fallback over the HLE framebuffer, even before the
            // first supported 3-D atomic appears.
            ps2xSetExternalPresentationActive(true);
            std::fprintf(stderr, "[p3fes-native] progressive native-only presentation enabled\n");
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
