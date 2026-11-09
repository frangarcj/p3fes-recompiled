// GPU-neutral Persona 3 mesh data. The layout follows the native-renderer
// boundary used by Celosia and Coney: game/asset decoding stays independent
// from the host graphics API.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace p3fes::native_renderer
{
struct Vertex
{
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> texCoord{};
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
    std::array<std::uint8_t, 4> bones{};
    std::array<float, 4> weights{};
};

struct Material
{
    std::array<std::uint8_t, 4> color{255, 255, 255, 255};
};

struct Submesh
{
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t materialIndex = 0;
};

struct MeshData
{
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Submesh> submeshes;
    std::vector<Material> materials;
    bool triangleStrip = false;
    bool hasNormals = false;
    bool hasPrelight = false;
    bool skinned = false;
};
} // namespace p3fes::native_renderer
