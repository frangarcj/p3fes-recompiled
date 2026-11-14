#pragma once

#if defined(__ANDROID__)

namespace p3fes_native_present
{
inline bool active() { return false; }
inline bool renderHostFrame() { return false; }
inline void shutdownHost() {}
}

#else

#include "native_renderer/renderer.h"

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
void beginFrame(const nr::Camera &camera, nr::ClearColor clear = {});
void setCamera(const nr::Camera &camera);
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
void endFrame();

// Consumer side (host presentation thread): owns every graphics API call.
bool renderHostFrame();
void shutdownHost();
}

#endif
