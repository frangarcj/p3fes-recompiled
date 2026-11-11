// GPU-neutral Persona 3 mesh data. The layout follows the native-renderer
// boundary used by Celosia and Coney: game/asset decoding stays independent
// from the host graphics API.
#pragma once

#include <array>
#include <cstdint>
#include <string>
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
    std::string textureName;
    // RenderWare surface properties. The native runtime currently uses the
    // ambient/diffuse terms; specular is retained for the next lighting step.
    float ambient = 1.0f;
    float specular = 1.0f;
    float diffuse = 1.0f;
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
    bool lighting = false;
    bool skinned = false;
    std::uint32_t boneCount = 0;
};

struct TextureData
{
    std::string name;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};
} // namespace p3fes::native_renderer
