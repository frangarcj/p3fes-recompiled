// Adapted from Ryubara/coney's RenderWare-to-MeshData conversion (GPL-3.0-or-later),
// commit eb07fc0f9d69e62c7aeee6307be5c2ef162f4ee6.
#include "mesh_convert.h"

#include <rw.h>

#include <algorithm>
#include <cstring>

namespace p3fes::native_rw
{
std::optional<native_renderer::MeshData> convertGeometry(rw::Geometry &geometry, std::string &error)
{
    if ((geometry.flags & rw::Geometry::NATIVE) != 0)
    {
        error = "geometry is still platform-native";
        return std::nullopt;
    }
    if (geometry.numMorphTargets <= 0 || geometry.morphTargets[0].vertices == nullptr)
    {
        error = "geometry has no positions";
        return std::nullopt;
    }
    if (geometry.meshHeader == nullptr)
        geometry.buildMeshes();
    if (geometry.meshHeader == nullptr)
    {
        error = "geometry could not build a mesh header";
        return std::nullopt;
    }

    const rw::MorphTarget &target = geometry.morphTargets[0];
    const rw::Skin *skin = rw::Skin::get(&geometry);
    native_renderer::MeshData out;
    out.hasNormals = target.normals != nullptr;
    out.hasPrelight = geometry.colors != nullptr;
    out.skinned = skin != nullptr;
    out.boneCount = skin ? static_cast<std::uint32_t>(skin->numBones) : 0u;
    out.triangleStrip = (geometry.meshHeader->flags & rw::MeshHeader::TRISTRIP) != 0;
    out.vertices.resize(static_cast<std::size_t>(geometry.numVertices));

    for (std::size_t i = 0; i < out.vertices.size(); ++i)
    {
        native_renderer::Vertex &vertex = out.vertices[i];
        const rw::V3d &position = target.vertices[i];
        vertex.position = {position.x, position.y, position.z};
        if (out.hasNormals)
        {
            const rw::V3d &normal = target.normals[i];
            vertex.normal = {normal.x, normal.y, normal.z};
        }
        if (out.hasPrelight)
        {
            const rw::RGBA &color = geometry.colors[i];
            vertex.color = {color.red, color.green, color.blue, color.alpha};
        }
        if (geometry.numTexCoordSets > 0 && geometry.texCoords[0] != nullptr)
        {
            const rw::TexCoords &uv = geometry.texCoords[0][i];
            vertex.texCoord = {uv.u, uv.v};
        }
        if (skin != nullptr)
        {
            std::memcpy(vertex.bones.data(), skin->indices + i * 4, 4);
            std::memcpy(vertex.weights.data(), skin->weights + i * 4, sizeof(float) * 4);
        }
    }

    out.materials.reserve(static_cast<std::size_t>(geometry.matList.numMaterials));
    for (rw::int32 i = 0; i < geometry.matList.numMaterials; ++i)
    {
        const rw::Material *material = geometry.matList.materials[i];
        const rw::RGBA &color = material->color;
        native_renderer::Material converted;
        converted.color = {color.red, color.green, color.blue, color.alpha};
        if (material->texture && material->texture->name[0] != '\0')
            converted.textureName = material->texture->name;
        out.materials.push_back(std::move(converted));
    }

    const rw::Mesh *meshes = geometry.meshHeader->getMeshes();
    out.indices.reserve(geometry.meshHeader->totalIndices);
    out.submeshes.reserve(geometry.meshHeader->numMeshes);
    for (rw::uint16 i = 0; i < geometry.meshHeader->numMeshes; ++i)
    {
        const rw::Mesh &mesh = meshes[i];
        rw::Material *const *begin = geometry.matList.materials;
        rw::Material *const *end = begin + geometry.matList.numMaterials;
        rw::Material *const *found = std::find(begin, end, mesh.material);
        if (found == end)
        {
            error = "mesh material is not present in the geometry material list";
            return std::nullopt;
        }

        out.submeshes.push_back({
            static_cast<std::uint32_t>(out.indices.size()),
            static_cast<std::uint32_t>(mesh.numIndices),
            static_cast<std::uint32_t>(found - begin),
        });
        for (rw::uint32 n = 0; n < mesh.numIndices; ++n)
            out.indices.push_back(mesh.indices[n]);
    }

    return out;
}
} // namespace p3fes::native_rw
