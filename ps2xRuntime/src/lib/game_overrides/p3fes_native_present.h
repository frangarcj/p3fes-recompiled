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
void beginFrame(const nr::Camera &camera, nr::ClearColor clear = {});
void clear(nr::ClearColor clear, bool colorBuffer, bool depthBuffer);
void defineMesh(nr::MeshId id, const nr::MeshData &mesh);
void defineTexture(const nr::TextureData &texture, uint64_t revision);
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
