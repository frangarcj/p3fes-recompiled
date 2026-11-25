#pragma once

#if defined(__ANDROID__)

namespace p3fes_native_present
{
inline bool active() { return false; }
inline bool renderHostFrame(void (*)() = nullptr) { return false; }
inline void shutdownHost() {}
}

#else

#include "native_renderer/renderer.h"

#include <memory>
#include <string>
#include <vector>

namespace p3fes_native_present
{
namespace nr = p3fes::native_renderer;

// Producer side (guest GameThread): CPU-only command recording.
void setActive(bool active);
bool active();
void setHostPresentationPaused(bool paused);
// A default-initialised draw state that may reuse storage returned by the presentation thread.
// Producer thread only; falls back to a fresh state when nothing is available.
nr::MeshDrawState acquireDrawState();
// In-place variant of acquireDrawState + drawMesh (P3FES_CUT_HANDOFF): stockDrawState() returns a clean state that
// the caller fills where it lies (the back of the stock, or a scratch state when the stock is empty), and
// drawMeshFromStock() then moves it into the frame once. Instead of drawMeshFromStock, a caller that moved the state
// elsewhere (a transient draw) or abandons it calls dropStockDrawState(). Exactly one of the three must follow each
// stockDrawState(); nothing else may record draws or take states in between.
nr::MeshDrawState &stockDrawState();
void drawMeshFromStock(nr::MeshId id, const nr::Matrix4 &model);
void dropStockDrawState();
// Draws mesh `id` with a retained, immutable state that the caller keeps and re-submits frame after frame (the
// static-atomic cache in p3fes_renderware.cpp). The frame holds a reference; the host reads it and never mutates or
// recycles it. Equivalent to drawMesh(id, model, copy of *state), without the copy.
// `colors` (colorCount <= 8, or null): the draw's own material colours, one RGBA word per material, replacing the
// state's materialColors for this draw only (the retained state is never modified).
// Copies `state` into a retained record for drawMeshShared (the only way to make one: it carries the producer's cached
// texture lookups next to the state).
std::shared_ptr<const nr::MeshDrawState> makeRetainedState(const nr::MeshDrawState &state);
void drawMeshShared(nr::MeshId id, const nr::Matrix4 &model, const std::shared_ptr<const nr::MeshDrawState> &state,
                    const uint32_t *colors = nullptr, uint32_t colorCount = 0);
void beginFrame(const nr::Camera &camera, nr::ClearColor clear = {});
void setCamera(const nr::Camera &camera);
// Producer side: bumped whenever the camera recorded in the frame under construction can have changed (frame begin
// and end, a recorded setCamera, a render-target bracket, a producer reset). While it is unchanged, calling setCamera
// again with the camera passed last time records nothing, so the caller can skip the call.
uint64_t cameraEpoch();
// Measurement only (P3FES_CUT_ALTERNATE in p3fes_renderware.cpp): turns the producer-side cuts of this file off and
// back on at run time, so one run can compare both settings in the same scene.
void setCutsEnabled(bool enabled);
// Verification of that statement: true when setCamera(camera) would record nothing right now.
bool setCameraWouldBeNoop(const nr::Camera &camera);
void clear(nr::ClearColor clear, bool colorBuffer, bool depthBuffer);
// Render-to-texture bracket inside the frame being recorded: world draws, setCamera and clear up to
// endTextureTarget go to a width x height target that any draw of the same frame can sample under
// `name` (the host renders brackets before the rest of the frame). Returns false when no frame is
// open or a bracket already is.
bool beginTextureTarget(const std::string &name, uint32_t width, uint32_t height);
void endTextureTarget();
bool inTextureTarget();
// Widescreen mode of the host renderer (see Renderer::setWidescreen).
void setWidescreen(int mode);
void defineMesh(nr::MeshId id, const nr::MeshData &mesh);
void defineTexture(const nr::TextureData &texture, uint64_t revision);
// Marks the texture `name` as the display frame buffer itself (a camera raster at GS address 0): before the first draw that
// samples it in a frame the host copies the picture drawn so far into it (Renderer::snapshotFramebuffer into a
// texWidth x texHeight texture holding the srcWidth x srcHeight picture). Producer side; idempotent.
void markFramebufferTexture(const std::string &name, uint32_t texWidth, uint32_t texHeight, uint32_t srcWidth, uint32_t srcHeight);
// Release the owner's reference; queued frames retain their own definitions.
// The host deletes GPU storage after the last referencing frame is consumed.
void retireTexture(const std::string &name);
void drawMesh(nr::MeshId id, const nr::Matrix4 &model, nr::MeshDrawState state = {});
// One-frame geometry such as RenderWare Im3D. This deliberately bypasses the
// persistent mesh-definition cache: the host uploads, draws and destroys it
// while consuming the frame so transient UI/effect batches cannot accumulate.
void drawTransientMesh(nr::MeshData mesh, const nr::Matrix4 &model, nr::MeshDrawState state = {});
void drawWorldLines(std::vector<nr::Vertex> vertices, const nr::Matrix4 &model,
                    std::string textureName = {}, nr::MeshDrawState state = {});
void drawScreenTriangles(std::vector<nr::ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                         std::string textureName = {}, nr::ScreenDrawState state = {});
void drawScreenLines(std::vector<nr::ScreenVertex> vertices, float sourceWidth, float sourceHeight,
                     std::string textureName = {}, nr::ScreenDrawState state = {});
// ---- Compact 2-D records ------------------------------------------------------------------------------------------------
// The cheap way to record a screen-space draw (P3FES_IM2D_CMD in p3fes_renderware.cpp): the vertices go into one array
// for the frame, the texture name and the draw state are interned in per-frame tables, so a draw is a few words. The ids
// are valid until screenEpoch() changes. The record call
// returns where the caller writes the `vertexCount` vertices, or null when no frame is being recorded.
// Mode 0: drawScreenTriangles/Lines keep one owning record per draw (the previous path); 1: they too record compactly;
// 2: compact, and every record also noted in its old form with noteOldScreenRecord is compared with it at endFrame
// (log line "[p3fes-im2d-frame] frames= records= mismatches=").
constexpr uint32_t kNoScreenRecord = 0xFFFFFFFFu;
// Bumped when the ids above stop being valid: frame begin and end, a render-target bracket opening or closing, a producer
// reset (not by a recorded camera, unlike cameraEpoch()).
uint64_t screenEpoch();
void setScreenRecordMode(int mode);
int screenRecordMode();
uint32_t internScreenTexture(const std::string &name); // 0: untextured
uint32_t internScreenState(const nr::ScreenDrawState &state, float sourceWidth, float sourceHeight);
nr::ScreenVertex *recordScreenDraw(uint32_t texture, uint32_t state, uint32_t vertexCount, bool lines = false);
void noteOldScreenRecord(const std::vector<nr::ScreenVertex> &vertices, float sourceWidth, float sourceHeight, const std::string &textureName,
                         const nr::ScreenDrawState &state, bool lines = false);
void endFrame();

// Consumer side (host presentation thread): owns every graphics API call.
bool renderHostFrame(void (*overlay)() = nullptr);
void shutdownHost();
}

#endif
