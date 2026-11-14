#pragma once

#include "../../../../p3fes/native_renderer/renderer.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PS2Runtime;

// Diagnostic builds only. All call sites are guarded by PS2X_P3FES_FRAME_TRACE.
// Producer state belongs to the EE executor; a sealed trace travels with Frame.
namespace p3fes_frame_trace
{
    struct Frame;
    using Handle = std::shared_ptr<Frame>;

    enum class Stage : uint32_t { TextureResolve = 1, VertexDecode, StateDecode, PrimitiveExpand, CommandQueue };
    struct Origin { uint32_t target = 0, source = 0; };

    class GuestScope
    {
    public:
        GuestScope(uint32_t target, uint32_t source, bool resumed = false) noexcept;
        ~GuestScope();
        GuestScope(const GuestScope &) = delete;
        GuestScope &operator=(const GuestScope &) = delete;
    private:
        friend class NativeScope;
        friend Origin origin() noexcept;
        friend void noteCheckpoint(uint32_t) noexcept;
        Handle m_frame;
        GuestScope *m_previous = nullptr;
        uint64_t m_started = 0, m_children = 0, m_checkpoints = 0;
        uint32_t m_route = 0;
    };
    class NativeScope : public GuestScope
    {
    public:
        explicit NativeScope(Stage stage) noexcept;
    };

    void setRuntime(PS2Runtime *runtime) noexcept;
    Handle beginFrame(uint64_t sequence) noexcept;
    void endProducer(const Handle &, uint32_t draws, uint32_t meshes, uint32_t textures) noexcept;
    void noteCheckpoint(uint32_t chargedCycles) noexcept;
    Origin origin() noexcept;

    struct DrawInfo
    {
        uint32_t index = 0;
        std::string kind;
        Origin origin;
        std::string texture;
        uint32_t textureWidth = 0, textureHeight = 0;
        uint64_t textureRevision = 0;
        uint32_t vertexCount = 0;
        float sourceWidth = 640.0f, sourceHeight = 448.0f;
        std::vector<p3fes::native_renderer::ScreenVertex> vertices;
        p3fes::native_renderer::ScreenDrawState state{};
        uint64_t ensureNs = 0, submitNs = 0;
    };
    // Host side only: record calls around renderer submission (wall time, not GPU time).
    bool rendering(const Handle &) noexcept;
    uint64_t nowNs() noexcept;
    void beginRender(const Handle &) noexcept;
    void setFrameState(const Handle &, const p3fes::native_renderer::Camera &,
                       p3fes::native_renderer::ClearColor) noexcept;
    void texturePreload(const Handle &, uint64_t elapsedNs, uint64_t uploads) noexcept;
    void resourceActivity(const Handle &, uint64_t uploads, uint64_t deletes) noexcept;
    void beginDraw(const Handle &, DrawInfo info) noexcept;
    void endDraw(const Handle &, uint64_t ensureNs, uint64_t submitNs) noexcept;
    void gpuPass(uint32_t primitive, uint32_t vertices, uint32_t bytes) noexcept;
    void endRender(const Handle &, uint64_t beginNs, uint64_t endNs) noexcept;
    bool captureScreenshot(const Handle &) noexcept;
}
