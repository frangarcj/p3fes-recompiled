#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace p3fes::native_renderer::detail
{
enum class MeshCompactionResult
{
    Success,
    IndexOutOfRange,
    TooManyVertices,
};

template <typename Vertex, typename Index>
MeshCompactionResult compactIndexedVertices(std::span<const Vertex> sourceVertices,
                                            std::span<const Index> sourceIndices,
                                            std::vector<Vertex> &compactVertices,
                                            std::vector<std::uint16_t> &compactIndices)
{
    constexpr std::size_t kIndexableVertexCount =
        static_cast<std::size_t>(UINT16_MAX) + 1u;
    constexpr std::uint32_t kUnmapped = std::numeric_limits<std::uint32_t>::max();

    compactVertices.clear();
    compactIndices.clear();
    compactIndices.reserve(sourceIndices.size());
    std::vector<std::uint32_t> remap(sourceVertices.size(), kUnmapped);

    for (const Index rawIndex : sourceIndices)
    {
        const std::uint64_t sourceIndex = static_cast<std::uint64_t>(rawIndex);
        if (sourceIndex >= sourceVertices.size())
        {
            compactVertices.clear();
            compactIndices.clear();
            return MeshCompactionResult::IndexOutOfRange;
        }

        std::uint32_t &compactIndex = remap[static_cast<std::size_t>(sourceIndex)];
        if (compactIndex != kUnmapped)
        {
            compactIndices.push_back(static_cast<std::uint16_t>(compactIndex));
            continue;
        }

        if (compactVertices.size() >= kIndexableVertexCount)
        {
            compactVertices.clear();
            compactIndices.clear();
            return MeshCompactionResult::TooManyVertices;
        }

        compactIndex = static_cast<std::uint32_t>(compactVertices.size());
        compactVertices.push_back(sourceVertices[static_cast<std::size_t>(sourceIndex)]);
        compactIndices.push_back(static_cast<std::uint16_t>(compactIndex));
    }

    return MeshCompactionResult::Success;
}

template <typename Vertex, typename Index>
MeshCompactionResult compactIndexedVertices(const std::vector<Vertex> &sourceVertices,
                                            const std::vector<Index> &sourceIndices,
                                            std::vector<Vertex> &compactVertices,
                                            std::vector<std::uint16_t> &compactIndices)
{
    return compactIndexedVertices<Vertex, Index>(
        std::span<const Vertex>(sourceVertices.data(), sourceVertices.size()),
        std::span<const Index>(sourceIndices.data(), sourceIndices.size()),
        compactVertices,
        compactIndices);
}
} // namespace p3fes::native_renderer::detail
