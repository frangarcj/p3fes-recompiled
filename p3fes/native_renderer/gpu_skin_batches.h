#pragma once

#include "mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace p3fes::native_renderer::detail
{
struct GpuSkinBatch
{
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    // Batch-local bone index -> index in MeshDrawState::skinPalette.
    std::vector<std::uint8_t> paletteBones;
};

enum class SkinBatchResult
{
    Success,
    InvalidIndexStream,
    TooManyBonesInTriangle,
    TooManyVertices,
};

inline SkinBatchResult buildGpuSkinBatches(const std::vector<Vertex> &source,
                                          const std::vector<std::uint16_t> &indices,
                                          std::uint32_t boneCount,
                                          std::size_t maxBones,
                                          std::vector<GpuSkinBatch> &out)
{
    out.clear();
    if (indices.empty() || (indices.size() % 3u) != 0u || maxBones == 0u || maxBones > 255u)
        return SkinBatchResult::InvalidIndexStream;

    GpuSkinBatch batch;
    std::unordered_map<std::uint32_t, std::uint8_t> boneMap;
    std::unordered_map<std::uint16_t, std::uint16_t> vertexMap;
    auto flush = [&] {
        if (!batch.indices.empty())
            out.push_back(std::move(batch));
        batch = GpuSkinBatch{};
        boneMap.clear();
        vertexMap.clear();
    };

    for (std::size_t triangle = 0; triangle < indices.size(); triangle += 3u)
    {
        std::array<std::uint32_t, 12> triangleBones{};
        std::size_t triangleBoneCount = 0u;
        for (std::size_t corner = 0; corner < 3u; ++corner)
        {
            const std::uint16_t sourceIndex = indices[triangle + corner];
            if (sourceIndex >= source.size())
                return SkinBatchResult::InvalidIndexStream;
            const Vertex &vertex = source[sourceIndex];
            for (std::size_t influence = 0; influence < 4u; ++influence)
            {
                const float weight = vertex.weights[influence];
                const std::uint32_t bone = vertex.bones[influence];
                if (!(weight > 0.0f) || !std::isfinite(weight) || bone >= boneCount)
                    continue;
                bool found = false;
                for (std::size_t i = 0; i < triangleBoneCount; ++i)
                    found = found || triangleBones[i] == bone;
                if (!found)
                    triangleBones[triangleBoneCount++] = bone;
            }
        }
        if (triangleBoneCount > maxBones)
            return SkinBatchResult::TooManyBonesInTriangle;

        std::size_t additions = 0u;
        for (std::size_t i = 0; i < triangleBoneCount; ++i)
            additions += boneMap.find(triangleBones[i]) == boneMap.end() ? 1u : 0u;
        if (!batch.indices.empty() && boneMap.size() + additions > maxBones)
            flush();

        std::size_t newVertices = 0u;
        for (std::size_t corner = 0; corner < 3u; ++corner)
        {
            const std::uint16_t index = indices[triangle + corner];
            bool appearsEarlier = false;
            for (std::size_t previous = 0; previous < corner; ++previous)
                appearsEarlier = appearsEarlier || indices[triangle + previous] == index;
            newVertices += !appearsEarlier && vertexMap.find(index) == vertexMap.end() ? 1u : 0u;
        }
        if (!batch.indices.empty() && batch.vertices.size() + newVertices > 65536u)
            flush();

        for (std::size_t i = 0; i < triangleBoneCount; ++i)
        {
            const std::uint32_t bone = triangleBones[i];
            if (boneMap.find(bone) == boneMap.end())
            {
                const auto local = static_cast<std::uint8_t>(batch.paletteBones.size());
                batch.paletteBones.push_back(static_cast<std::uint8_t>(bone));
                boneMap.emplace(bone, local);
            }
        }

        for (std::size_t corner = 0; corner < 3u; ++corner)
        {
            const std::uint16_t sourceIndex = indices[triangle + corner];
            auto foundVertex = vertexMap.find(sourceIndex);
            if (foundVertex == vertexMap.end())
            {
                if (batch.vertices.size() >= 65536u)
                    return SkinBatchResult::TooManyVertices;
                Vertex vertex = source[sourceIndex];
                for (std::size_t influence = 0; influence < 4u; ++influence)
                {
                    const float weight = vertex.weights[influence];
                    const std::uint32_t bone = vertex.bones[influence];
                    const auto foundBone = boneMap.find(bone);
                    if (!(weight > 0.0f) || !std::isfinite(weight) || bone >= boneCount || foundBone == boneMap.end())
                    {
                        vertex.bones[influence] = 0u;
                        vertex.weights[influence] = 0.0f;
                    }
                    else
                        vertex.bones[influence] = foundBone->second;
                }
                const auto localVertex = static_cast<std::uint16_t>(batch.vertices.size());
                batch.vertices.push_back(vertex);
                vertexMap.emplace(sourceIndex, localVertex);
                batch.indices.push_back(localVertex);
            }
            else
                batch.indices.push_back(foundVertex->second);
        }
    }
    flush();
    return out.empty() ? SkinBatchResult::InvalidIndexStream : SkinBatchResult::Success;
}
} // namespace p3fes::native_renderer::detail
