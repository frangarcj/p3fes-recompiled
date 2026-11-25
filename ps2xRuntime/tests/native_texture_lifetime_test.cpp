#include "../src/lib/game_overrides/p3fes_native_present.h"

#include <cassert>
#include <memory>
#include <string>
#include <unordered_map>

namespace nr = p3fes::native_renderer;
namespace present = p3fes_native_present;

namespace {
struct Observations {
    unsigned uploads = 0, draws = 0, deletes = 0;
    unsigned begins = 0, cameraChanges = 0, clears = 0, ends = 0;
    std::vector<nr::Camera> cameras;
    std::unordered_map<std::string, unsigned> colors;
} observed;

class Renderer final : public nr::Renderer {
public:
    bool open(int, int, const char *, std::string &) override { return true; }
    void close() override { observed.colors.clear(); }
    bool shouldClose() const override { return false; }
    void setTargetFps(int) override {}
    bool uploadTexture(const nr::TextureData &texture, std::string &) override {
        ++observed.uploads;
        observed.colors[texture.name] = texture.rgba.front();
        return true;
    }
    void removeTexture(std::string_view name) override {
        observed.deletes += observed.colors.erase(std::string(name));
    }
    bool uploadMesh(nr::MeshId, const nr::MeshData &, std::string &) override { return true; }
    void removeMesh(nr::MeshId) override {}
    void beginFrame(const nr::Camera &camera, nr::ClearColor) override {
        ++observed.begins;
        observed.cameras.push_back(camera);
    }
    void setCamera(const nr::Camera &camera) override {
        ++observed.cameraChanges;
        observed.cameras.push_back(camera);
    }
    void clear(nr::ClearColor, bool, bool) override { ++observed.clears; }
    void drawMesh(nr::MeshId, const nr::Matrix4 &, const nr::MeshDrawState &state) override {
        for (const auto &name : state.materialTextures) assert(observed.colors.contains(name));
        ++observed.draws;
    }
    void drawWorldLines(const std::vector<nr::Vertex> &, const nr::Matrix4 &, std::string_view,
                        const nr::MeshDrawState &) override {}
    void drawScreenTriangles(const std::vector<nr::ScreenVertex> &, float, float,
                             std::string_view name, const nr::ScreenDrawState &) override {
        assert(observed.colors.at(std::string(name)) == 17);
        ++observed.draws;
    }
    void drawScreenLines(const std::vector<nr::ScreenVertex> &, float, float,
                         std::string_view, const nr::ScreenDrawState &) override {}
    void endFrame(void (*)()) override { ++observed.ends; }
};

nr::TextureData texture(std::string name, unsigned value) {
    return {std::move(name), 1, 1, {static_cast<unsigned char>(value), 0, 0, 255}};
}
}

namespace p3fes::native_renderer {
std::unique_ptr<Renderer> createPlatformRenderer() { return std::make_unique<::Renderer>(); }
}

int main() {
    present::setActive(true);
    present::defineTexture(texture("old-generation", 17), 1);
    present::beginFrame({});
    present::drawScreenTriangles(std::vector<nr::ScreenVertex>(3), 640, 448, "old-generation");
    // The guest frees/reuses its raster before the host consumes the frame.
    present::retireTexture("old-generation");
    present::defineTexture(texture("new-generation", 99), 1);
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.uploads == 1 && observed.draws == 1 && observed.deletes == 1);
    assert(!observed.colors.contains("old-generation"));

    // Dropping a superseded frame releases its resource, without uploading it.
    present::beginFrame({});
    present::drawScreenTriangles(std::vector<nr::ScreenVertex>(3), 640, 448, "new-generation");
    present::endFrame();
    present::retireTexture("new-generation");
    present::defineTexture(texture("current-generation", 17), 2);
    present::beginFrame({});
    present::drawScreenTriangles(std::vector<nr::ScreenVertex>(3), 640, 448, "current-generation");
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.uploads == 2 && observed.draws == 2);
    assert(!observed.colors.contains("new-generation"));

    // Cached geometry obtains its live texture binding from the draw state.
    present::defineMesh(1, {});
    present::beginFrame({});
    nr::MeshDrawState state;
    state.materialTextures = {"current-generation"};
    present::drawMesh(1, nr::kIdentityMatrix, state);
    present::retireTexture("current-generation");
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.draws == 3 && observed.deletes == 2);

    // Camera changes are ordered inside one frame. Repeating the current
    // camera is coalesced, and changing it emits no implicit clear or frame.
    nr::Camera firstCamera{};
    firstCamera.position = {1.0f, 2.0f, 3.0f};
    nr::Camera secondCamera{};
    secondCamera.position = {4.0f, 5.0f, 6.0f};
    const unsigned beginsBefore = observed.begins;
    const unsigned changesBefore = observed.cameraChanges;
    const unsigned clearsBefore = observed.clears;
    const unsigned endsBefore = observed.ends;
    const unsigned drawsBefore = observed.draws;
    const std::size_t camerasBefore = observed.cameras.size();
    present::beginFrame(firstCamera);
    present::setCamera(firstCamera);
    present::drawMesh(1, nr::kIdentityMatrix);
    present::setCamera(secondCamera);
    present::setCamera(secondCamera);
    present::drawMesh(1, nr::kIdentityMatrix);
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.begins == beginsBefore + 1);
    assert(observed.cameraChanges == changesBefore + 1);
    assert(observed.clears == clearsBefore);
    assert(observed.ends == endsBefore + 1);
    assert(observed.draws == drawsBefore + 2);
    assert(observed.cameras.size() == camerasBefore + 2);
    assert(observed.cameras[camerasBefore] == firstCamera);
    assert(observed.cameras[camerasBefore + 1] == secondCamera);
    present::shutdownHost();
    assert(observed.colors.empty());
}
