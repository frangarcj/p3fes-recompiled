#include "native_model.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>

namespace p3fes::native_renderer
{
namespace
{
    constexpr std::array<char, 8> kMagic{'P', '3', 'N', 'M', 'D', 'L', '1', '\0'};
    constexpr std::uint32_t kVersion = 1u;
    constexpr std::size_t kNameBytes = 32u;
    class Reader
    {
    public:
        explicit Reader(std::vector<std::uint8_t> bytes) : m_bytes(std::move(bytes)) {}

        template <typename T>
        bool pod(T &value)
        {
            if (remaining() < sizeof(T))
                return false;
            std::memcpy(&value, m_bytes.data() + m_pos, sizeof(T));
            m_pos += sizeof(T);
            return true;
        }

        bool raw(void *dst, std::size_t size)
        {
            if (remaining() < size)
                return false;
            std::memcpy(dst, m_bytes.data() + m_pos, size);
            m_pos += size;
            return true;
        }

        std::size_t remaining() const { return m_bytes.size() - m_pos; }

    private:
        std::vector<std::uint8_t> m_bytes;
        std::size_t m_pos = 0;
    };

    bool reasonableCount(std::uint32_t value, std::uint32_t limit)
    {
        return value <= limit;
    }

    std::string readName(const char (&raw)[kNameBytes])
    {
        const void *end = std::memchr(raw, '\0', kNameBytes);
        const std::size_t size = end ? static_cast<const char *>(end) - raw : kNameBytes;
        return std::string(raw, size);
    }
}

bool loadNativeModel(const std::string &path, NativeModel &model, std::string &error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        error = "could not open native model pack";
        return false;
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Reader reader(std::move(bytes));

    std::array<char, 8> magic{};
    std::uint32_t version = 0u, meshCount = 0u, textureCount = 0u, flags = 0u;
    if (!reader.raw(magic.data(), magic.size()) || !reader.pod(version) || !reader.pod(meshCount) ||
        !reader.pod(textureCount) || !reader.pod(flags) || magic != kMagic || version != kVersion)
    {
        error = "invalid or unsupported native model header";
        return false;
    }
    if (!reasonableCount(meshCount, 4096u) || !reasonableCount(textureCount, 4096u))
    {
        error = "native model count is unreasonable";
        return false;
    }

    NativeModel loaded;
    loaded.meshes.reserve(meshCount);
    for (std::uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
    {
        std::uint32_t meshFlags = 0u, vertexCount = 0u, indexCount = 0u, submeshCount = 0u;
        std::uint32_t materialCount = 0u, boneCount = 0u, reserved = 0u;
        if (!reader.pod(meshFlags) || !reader.pod(vertexCount) || !reader.pod(indexCount) ||
            !reader.pod(submeshCount) || !reader.pod(materialCount) || !reader.pod(boneCount) || !reader.pod(reserved))
        {
            error = "truncated native mesh header";
            return false;
        }
        if (!reasonableCount(vertexCount, 1u << 20) || !reasonableCount(indexCount, 1u << 23) ||
            !reasonableCount(submeshCount, 65536u) || !reasonableCount(materialCount, 65536u) || boneCount > 1024u)
        {
            error = "native mesh count is unreasonable";
            return false;
        }

        MeshData mesh;
        mesh.hasNormals = (meshFlags & 1u) != 0u;
        mesh.hasPrelight = (meshFlags & 2u) != 0u;
        mesh.skinned = (meshFlags & 4u) != 0u;
        mesh.boneCount = boneCount;
        mesh.vertices.resize(vertexCount);
        for (Vertex &vertex : mesh.vertices)
        {
            if (!reader.raw(vertex.position.data(), sizeof(float) * 3) ||
                !reader.raw(vertex.normal.data(), sizeof(float) * 3) ||
                !reader.raw(vertex.texCoord.data(), sizeof(float) * 2) ||
                !reader.raw(vertex.color.data(), 4) || !reader.raw(vertex.bones.data(), 4) ||
                !reader.raw(vertex.weights.data(), sizeof(float) * 4))
            {
                error = "truncated native vertex data";
                return false;
            }
        }

        mesh.indices.resize(indexCount);
        if (indexCount != 0u && !reader.raw(mesh.indices.data(), sizeof(std::uint32_t) * indexCount))
        {
            error = "truncated native index data";
            return false;
        }
        mesh.submeshes.resize(submeshCount);
        for (Submesh &submesh : mesh.submeshes)
        {
            if (!reader.pod(submesh.firstIndex) || !reader.pod(submesh.indexCount) || !reader.pod(submesh.materialIndex))
            {
                error = "truncated native submesh data";
                return false;
            }
            if (submesh.firstIndex > mesh.indices.size() || submesh.indexCount > mesh.indices.size() - submesh.firstIndex ||
                submesh.materialIndex >= materialCount)
            {
                error = "native submesh references invalid data";
                return false;
            }
        }

        mesh.materials.resize(materialCount);
        for (Material &material : mesh.materials)
        {
            char name[kNameBytes]{};
            if (!reader.raw(material.color.data(), 4) || !reader.raw(name, sizeof(name)))
            {
                error = "truncated native material data";
                return false;
            }
            material.textureName = readName(name);
        }
        loaded.meshes.push_back(std::move(mesh));
    }

    loaded.textures.reserve(textureCount);
    for (std::uint32_t textureIndex = 0; textureIndex < textureCount; ++textureIndex)
    {
        char name[kNameBytes]{};
        std::uint32_t width = 0u, height = 0u, size = 0u;
        if (!reader.raw(name, sizeof(name)) || !reader.pod(width) || !reader.pod(height) || !reader.pod(size))
        {
            error = "truncated native texture header";
            return false;
        }
        if (width == 0u || height == 0u || width > 8192u || height > 8192u ||
            static_cast<std::uint64_t>(width) * height * 4u != size || size > reader.remaining())
        {
            error = "invalid native texture dimensions";
            return false;
        }
        TextureData texture;
        texture.name = readName(name);
        texture.width = width;
        texture.height = height;
        texture.rgba.resize(size);
        if (!reader.raw(texture.rgba.data(), size))
        {
            error = "truncated native texture pixels";
            return false;
        }
        loaded.textures.push_back(std::move(texture));
    }

    if (reader.remaining() != 0u)
    {
        error = "native model has trailing bytes";
        return false;
    }
    model = std::move(loaded);
    return true;
}
} // namespace p3fes::native_renderer
