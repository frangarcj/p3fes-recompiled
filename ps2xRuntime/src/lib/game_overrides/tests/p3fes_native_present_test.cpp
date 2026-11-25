// Host test for p3fes_native_present: texture retention, state recycling and the lock-free producer path.
// Build (from the repository root; add -fsanitize=thread to check the producer/host split):
//   clang++ -std=gnu++20 -O1 -g -Wall -I ps2xRuntime/include -I ps2xRuntime/src/lib \
//     -I ps2xRuntime/src/lib/game_overrides -I p3fes -I ps2xRuntime/src/lib/gs \
//     ps2xRuntime/src/lib/game_overrides/p3fes_native_present.cpp \
//     ps2xRuntime/src/lib/game_overrides/tests/p3fes_native_present_test.cpp -o present_test
// (p3fes_native_movie::shutdownHost is stubbed below. -DP3FES_PRESENT_TEST_NO_RECYCLE skips the state
// recycling checks, so the remaining ones can also be run against the previous implementation.)

#include "p3fes_native_present.h"
#include "p3fes_native_movie.h"
#include "p3fes_scene_forward.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace p3fes_native_movie { void shutdownHost() {} }
extern "C" int ps2xFrameHistEnabled() { return 0; }
extern "C" void ps2xFrameHistAddLock(uint64_t) {}

namespace nr = p3fes::native_renderer;
namespace present = p3fes_native_present;

namespace
{
struct Observations
{
    unsigned uploads = 0, draws = 0, missing = 0, wrongValue = 0;
    std::unordered_map<std::string, unsigned> colors; // name -> first texel
    std::vector<unsigned> drawnValues;                // value of the first material texture per mesh draw
} observed;

unsigned valueOf(const std::string &name) // names look like "<tag>-<value>" or "<tag>"
{
    const auto dash = name.rfind('-');
    return dash == std::string::npos ? 0u : static_cast<unsigned>(std::stoul(name.substr(dash + 1)));
}

class Renderer final : public nr::Renderer
{
public:
    bool open(int, int, const char *, std::string &) override { return true; }
    void close() override { observed.colors.clear(); }
    bool shouldClose() const override { return false; }
    void setTargetFps(int) override {}
    bool uploadTexture(const nr::TextureData &texture, std::string &) override
    {
        ++observed.uploads;
        observed.colors[texture.name] = texture.rgba.front();
        return true;
    }
    void removeTexture(std::string_view name) override { observed.colors.erase(std::string(name)); }
    bool uploadMesh(nr::MeshId, const nr::MeshData &, std::string &) override { return true; }
    void removeMesh(nr::MeshId) override {}
    void beginFrame(const nr::Camera &, nr::ClearColor) override {}
    void setCamera(const nr::Camera &) override {}
    void beginTextureTarget(std::string_view, int, int) override {}
    void endTextureTarget() override {}
    void setWidescreen(int) override {}
    void setScreenPlacement(ScreenPlacement) override {}
    void clear(nr::ClearColor, bool, bool) override {}
    void drawMesh(nr::MeshId, const nr::Matrix4 &, const nr::MeshDrawState &state) override
    {
        ++observed.draws;
        bool first = true;
        for (const auto &name : state.materialTextures)
        {
            if (name.empty()) continue;
            const auto found = observed.colors.find(name);
            if (found == observed.colors.end())
                ++observed.missing;
            else
            {
                if (found->second != valueOf(name)) ++observed.wrongValue;
                if (first) observed.drawnValues.push_back(found->second);
            }
            first = false;
        }
    }
    void drawWorldLines(const std::vector<nr::Vertex> &, const nr::Matrix4 &, std::string_view,
                        const nr::MeshDrawState &) override {}
    void drawScreenTriangles(const std::vector<nr::ScreenVertex> &, float, float, std::string_view name,
                             const nr::ScreenDrawState &) override
    {
        ++observed.draws;
        if (!name.empty() && !observed.colors.contains(std::string(name))) ++observed.missing;
    }
    void drawScreenLines(const std::vector<nr::ScreenVertex> &, float, float, std::string_view,
                         const nr::ScreenDrawState &) override {}
    void endFrame(void (*)()) override {}
};

nr::TextureData texture(std::string name, unsigned value)
{
    return {std::move(name), 1, 1, {static_cast<unsigned char>(value), 0, 0, 255}};
}
} // namespace

namespace p3fes::native_renderer {
std::unique_ptr<Renderer> createPlatformRenderer() { return std::make_unique<::Renderer>(); }
}

int main()
{
    present::setActive(true);

    // Holding the scene control adds one batched VHS draw. Releasing it removes
    // that draw on the next frame, without leaving a texture bound.
    for (bool held : {false, true, false})
    {
        p3fes_scene_forward::active.store(held);
        present::beginFrame({});
        present::drawScreenTriangles(std::vector<nr::ScreenVertex>(3), 640, 448, {});
        present::endFrame();
        observed = {};
        assert(present::renderHostFrame());
        assert(observed.draws == (held ? 2u : 1u) && observed.missing == 0);
    }
    observed = {};

    // A texture redefined under the same name after a draw already retained it: the frame uses the
    // latest definition for every draw (what the per-draw map assignment used to give).
    present::defineMesh(1, {});
    present::defineTexture(texture("a-10", 10), 1);
    present::beginFrame({});
    {
        nr::MeshDrawState state;
        state.materialTextures = {"a-10"};
        present::drawMesh(1, nr::kIdentityMatrix, state);
        present::defineTexture(texture("a-10", 10), 2); // new revision, same name and content
        present::drawMesh(1, nr::kIdentityMatrix, state);
    }
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.draws == 2 && observed.missing == 0 && observed.uploads == 1);

    // Retire after use, then redefine the same name within the same frame: both the retained old
    // definition and the new one must not disturb the frame (no missing texture, no crash).
    observed = {};
    present::defineTexture(texture("b-20", 20), 1);
    present::beginFrame({});
    {
        nr::MeshDrawState state;
        state.materialTextures = {"b-20"};
        present::drawMesh(1, nr::kIdentityMatrix, state);
        present::retireTexture("b-20");
        present::defineTexture(texture("b-20", 20), 1);
        present::drawMesh(1, nr::kIdentityMatrix, state);
    }
    present::endFrame();
    assert(present::renderHostFrame());
    assert(observed.draws == 2 && observed.missing == 0 && observed.wrongValue == 0);

    // A retired texture's slot is reclaimed at the next frame; the name can be reused afterwards.
    present::retireTexture("b-20");
    present::defineTexture(texture("b-20", 20), 1);
    present::beginFrame({});
    {
        nr::MeshDrawState state;
        state.materialTextures = {"b-20"};
        present::drawMesh(1, nr::kIdentityMatrix, state);
    }
    present::endFrame();
    observed = {};
    assert(present::renderHostFrame());
    assert(observed.draws == 1 && observed.missing == 0);

    // Long (heap) names behave the same as short ones.
    const std::string longName = "a-very-long-texture-name-that-does-not-fit-a-small-string-buffer-30";
    present::defineTexture(texture(longName, 30), 1);
    present::beginFrame({});
    {
        nr::MeshDrawState state;
        state.materialTextures = {longName, "unknown-1"};
        present::drawMesh(1, nr::kIdentityMatrix, state);
    }
    present::endFrame();
    observed = {};
    assert(present::renderHostFrame());
    assert(observed.draws == 1 && observed.wrongValue == 0);

    // Drawn states come back through acquireDrawState with defaults but their storage kept.
#ifndef P3FES_PRESENT_TEST_NO_RECYCLE
    {
        present::beginFrame({});
        nr::MeshDrawState state;
        state.materialColors.assign(16, {1, 2, 3, 4});
        state.skinPalette.resize(40);
        state.depthWrite = false;
        state.cullMode = nr::MeshDrawState::CullMode::None;
        present::drawMesh(1, nr::kIdentityMatrix, std::move(state));
        present::endFrame();
        assert(present::renderHostFrame());
        present::beginFrame({}); // picks the recycled states up
        nr::MeshDrawState recycled = present::acquireDrawState();
        assert(recycled.materialColors.empty() && recycled.skinPalette.empty());
        assert(recycled.materialColors.capacity() >= 16 && recycled.skinPalette.capacity() >= 40);
        assert(recycled.depthWrite && recycled.cullMode == nr::MeshDrawState::CullMode::Back);
        present::endFrame();
    }
#endif

    // Producer and presentation threads running concurrently (use -fsanitize=thread): every drawn
    // texture must still be defined when its frame is rendered, however the game retires it.
    observed = {};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> rendered{0};
    std::thread host([&] {
        while (!stop.load())
            if (present::renderHostFrame()) ++rendered;
    });
    for (unsigned frame = 0; frame < 3000; ++frame)
    {
        std::vector<std::string> names;
        for (unsigned i = 0; i < 6; ++i)
        {
            names.push_back("t" + std::to_string(i) + "-" + std::to_string((frame + i) % 200));
            present::defineTexture(texture(names.back(), (frame + i) % 200), 1);
        }
        present::beginFrame({});
        for (unsigned draw = 0; draw < 12; ++draw)
        {
#ifndef P3FES_PRESENT_TEST_NO_RECYCLE
            nr::MeshDrawState state = present::acquireDrawState();
#else
            nr::MeshDrawState state;
#endif
            state.materialTextures = {names[draw % 6], names[(draw + 1) % 6]};
            present::drawMesh(1, nr::kIdentityMatrix, std::move(state));
            if (draw % 3 == 0)
                present::drawScreenTriangles(std::vector<nr::ScreenVertex>(3), 640, 448, names[draw % 6]);
        }
        present::endFrame();
        for (const auto &name : names) present::retireTexture(name); // the game destroys them right after
    }
    stop = true;
    host.join();
    present::shutdownHost();
    std::printf("frames rendered=%u draws=%u missing=%u wrongValue=%u\n", rendered.load(), observed.draws,
                observed.missing, observed.wrongValue);
    assert(observed.missing == 0 && observed.wrongValue == 0 && rendered.load() > 0);
    std::printf("PASS\n");
    return 0;
}
