#include <rw.h>

#include "mesh_convert.h"
#include "../native_renderer/native_model.h"
#include "../native_renderer/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace
{
struct Options
{
    const char *modelPath = nullptr;
    int frames = 0;
};

std::uint32_t readLe32(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::size_t findRwOffset(const std::vector<std::uint8_t> &data)
{
    const std::size_t scanEnd = std::min<std::size_t>(data.size(), 0x100);
    for (std::size_t offset = 0; offset + 12 <= scanEnd; ++offset)
    {
        const std::uint32_t id = readLe32(data.data() + offset);
        const std::uint32_t size = readLe32(data.data() + offset + 4);
        if ((id == rw::ID_TEXDICTIONARY || id == rw::ID_CLUMP) &&
            size <= data.size() - offset - 12)
            return offset;
    }
    return data.size();
}

std::vector<std::uint8_t> readFile(const char *path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

rw::Clump *readClump(std::uint8_t *data, std::size_t size)
{
    rw::StreamMemory stream;
    stream.open(data, static_cast<rw::uint32>(size));
    rw::Clump *clump = nullptr;
    if (rw::findChunk(&stream, rw::ID_CLUMP, nullptr, nullptr))
        clump = rw::Clump::streamRead(&stream);
    stream.close();
    return clump;
}

bool parseInt(const char *text, int &value)
{
    char *end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (!text[0] || (end && *end) || parsed < 0 || parsed > 1000000)
        return false;
    value = static_cast<int>(parsed);
    return true;
}

bool parseArgs(int argc, char **argv, Options &out)
{
    if (argc < 2)
        return false;
    out.modelPath = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
        {
            if (!parseInt(argv[++i], out.frames))
                return false;
        }
        else
        {
            return false;
        }
    }
    return true;
}

bool hasSuffix(const char *path, const char *suffix)
{
    const std::string_view value(path ? path : "");
    const std::string_view ending(suffix);
    return value.size() >= ending.size() && value.substr(value.size() - ending.size()) == ending;
}

struct Bounds
{
    std::array<float, 3> min{
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    std::array<float, 3> max{
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
    };
};

void include(Bounds &bounds, const p3fes::native_renderer::MeshData &mesh)
{
    for (const auto &vertex : mesh.vertices)
    {
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            bounds.min[axis] = std::min(bounds.min[axis], vertex.position[axis]);
            bounds.max[axis] = std::max(bounds.max[axis], vertex.position[axis]);
        }
    }
}
} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (!parseArgs(argc, argv, options))
    {
        std::fprintf(stderr, "Usage: %s MODEL.RMD|MODEL.p3n [--frames N]\n", argv[0]);
        return 2;
    }

    std::vector<p3fes::native_renderer::MeshData> neutralMeshes;
    std::vector<p3fes::native_renderer::TextureData> preparedTextures;
    if (hasSuffix(options.modelPath, ".p3n"))
    {
        p3fes::native_renderer::NativeModel model;
        std::string error;
        if (!p3fes::native_renderer::loadNativeModel(options.modelPath, model, error))
        {
            std::fprintf(stderr, "Could not load native model pack: %s\n", error.c_str());
            return 1;
        }
        preparedTextures = std::move(model.textures);
        neutralMeshes = std::move(model.meshes);
    }
    else
    {
        std::vector<std::uint8_t> file = readFile(options.modelPath);
        const std::size_t rwOffset = findRwOffset(file);
        if (file.empty() || rwOffset == file.size())
        {
            std::fprintf(stderr, "Could not find a RenderWare stream in %s\n", options.modelPath);
            return 1;
        }

        rw::Engine::init();
        rw::registerHAnimPlugin();
        rw::registerSkinPlugin();
        rw::registerMatFXPlugin();
        rw::ps2::registerPlatformPlugins();
        rw::ps2::registerNativeDataPlugin();
        rw::ps2::registerADCPlugin();
        if (!rw::Engine::open(nullptr) || !rw::Engine::start())
            return 1;

        rw::Clump *clump = readClump(file.data() + rwOffset, file.size() - rwOffset);
        if (clump == nullptr)
            return 1;

        FORLIST(lnk, clump->atomics)
        {
            rw::Atomic *atomic = rw::Atomic::fromClump(lnk);
            if (atomic->geometry == nullptr)
                continue;
            std::string error;
            auto mesh = p3fes::native_rw::convertGeometry(*atomic->geometry, error);
            if (!mesh)
            {
                std::fprintf(stderr, "Could not convert an atomic: %s\n", error.c_str());
                continue;
            }
            neutralMeshes.push_back(std::move(*mesh));
        }
        clump->destroy();
        rw::Engine::stop();
        rw::Engine::close();
        rw::Engine::term();
    }

    if (neutralMeshes.empty())
        return 1;

    Bounds bounds;
    for (const auto &mesh : neutralMeshes)
        include(bounds, mesh);

    std::unique_ptr<p3fes::native_renderer::Renderer> renderer =
        p3fes::native_renderer::createDesktopRenderer();
    std::string renderError;
    if (!renderer->open(960, 544, "Persona 3 FES direct RenderWare viewer", renderError))
    {
        std::fprintf(stderr, "Could not open native renderer: %s\n", renderError.c_str());
        return 1;
    }
    for (const auto &texture : preparedTextures)
    {
        if (!renderer->uploadTexture(texture, renderError))
        {
            std::fprintf(stderr, "Could not upload texture %s: %s\n", texture.name.c_str(), renderError.c_str());
            return 1;
        }
    }
    for (std::size_t i = 0; i < neutralMeshes.size(); ++i)
    {
        if (!renderer->uploadMesh(static_cast<p3fes::native_renderer::MeshId>(i + 1), neutralMeshes[i], renderError))
        {
            std::fprintf(stderr, "Could not upload atomic %zu: %s\n", i, renderError.c_str());
            return 1;
        }
    }

    const std::array<float, 3> center{
        (bounds.min[0] + bounds.max[0]) * 0.5f,
        (bounds.min[1] + bounds.max[1]) * 0.5f,
        (bounds.min[2] + bounds.max[2]) * 0.5f,
    };
    const float dx = bounds.max[0] - bounds.min[0];
    const float dy = bounds.max[1] - bounds.min[1];
    const float dz = bounds.max[2] - bounds.min[2];
    const float radius = std::max({dx, dy, dz, 1.0f}) * 0.75f;

    p3fes::native_renderer::Camera camera{};
    camera.target = center;
    camera.position = {center[0] + radius * 1.6f, center[1] + radius * 0.8f, center[2] + radius * 1.6f};
    camera.fovy = 45.0f;
    renderer->setTargetFps(60);

    int frame = 0;
    while (!renderer->shouldClose() && (options.frames <= 0 || frame < options.frames))
    {
        const float phase = static_cast<float>(frame) * 0.008f;
        camera.position[0] = center[0] + std::cos(phase) * radius * 2.2f;
        camera.position[2] = center[2] + std::sin(phase) * radius * 2.2f;

        renderer->beginFrame(camera, {});
        for (std::size_t i = 0; i < neutralMeshes.size(); ++i)
            renderer->drawMesh(static_cast<p3fes::native_renderer::MeshId>(i + 1));
        renderer->endFrame();
        ++frame;
    }

    renderer->close();
    return 0;
}
