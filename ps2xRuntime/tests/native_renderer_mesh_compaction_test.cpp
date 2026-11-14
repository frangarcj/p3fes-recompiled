#include "mesh.h"
#include "mesh_compaction.h"

#include <cassert>
#include <cstdint>
#include <vector>

using p3fes::native_renderer::Vertex;
using p3fes::native_renderer::detail::MeshCompactionResult;
using p3fes::native_renderer::detail::compactIndexedVertices;

namespace
{
    Vertex makeVertex(std::uint32_t id)
    {
        Vertex vertex;
        vertex.position = {static_cast<float>(id), static_cast<float>(id + 1u), static_cast<float>(id + 2u)};
        vertex.normal = {static_cast<float>(id + 3u), static_cast<float>(id + 4u), static_cast<float>(id + 5u)};
        vertex.texCoord = {static_cast<float>(id + 6u), static_cast<float>(id + 7u)};
        vertex.color = {static_cast<std::uint8_t>(id), 2u, 3u, 4u};
        vertex.bones = {5u, 6u, 7u, static_cast<std::uint8_t>(id)};
        vertex.weights = {0.1f, 0.2f, 0.3f, 0.4f};
        return vertex;
    }
}

int main()
{
    const std::vector<Vertex> source{
        makeVertex(0u), makeVertex(1u), makeVertex(2u),
        makeVertex(3u), makeVertex(4u), makeVertex(5u),
    };
    std::vector<Vertex> compact;
    std::vector<std::uint16_t> indices;

    // Shared references are compacted once, in first-use order. Triangles stay
    // in their input order and the full vertex record (including skin data) copies.
    const std::vector<std::uint16_t> shared{0u, 2u, 1u, 2u, 1u, 3u};
    assert(compactIndexedVertices<Vertex>(source, shared, compact, indices) == MeshCompactionResult::Success);
    assert((indices == std::vector<std::uint16_t>{0u, 1u, 2u, 1u, 2u, 3u}));
    assert(compact.size() == 4u);
    assert(compact[0].position == source[0].position && compact[0].bones == source[0].bones &&
           compact[0].weights == source[0].weights && compact[0].color == source[0].color);
    assert(compact[1].position == source[2].position && compact[2].texCoord == source[1].texCoord);
    // Reconstruct every indexed vertex to check the rendered triangles keep
    // all attributes, independent of the chosen local numbering.
    for (std::size_t i = 0u; i < shared.size(); ++i)
    {
        const Vertex &before = source[shared[i]];
        const Vertex &after = compact[indices[i]];
        assert(after.position == before.position && after.normal == before.normal &&
               after.texCoord == before.texCoord && after.color == before.color &&
               after.bones == before.bones && after.weights == before.weights);
    }

    // Separate materials with disjoint source ranges compact independently.
    const std::vector<std::uint16_t> materialA{0u, 2u, 1u};
    assert(compactIndexedVertices<Vertex>(source, materialA, compact, indices) == MeshCompactionResult::Success);
    assert(compact.size() == 3u && (indices == std::vector<std::uint16_t>{0u, 1u, 2u}));
    const std::vector<std::uint16_t> materialB{3u, 5u, 4u};
    assert(compactIndexedVertices<Vertex>(source, materialB, compact, indices) == MeshCompactionResult::Success);
    assert(compact.size() == 3u && compact[0].position == source[3].position &&
           compact[1].position == source[5].position && compact[2].position == source[4].position);

    // Strip input is expected to be triangulated by triangleList first; the
    // resulting triangle sequence and shared-vertex remap must remain unchanged.
    const std::vector<std::uint16_t> triangulatedStrip{0u, 1u, 2u, 2u, 1u, 3u};
    assert(compactIndexedVertices<Vertex>(source, triangulatedStrip, compact, indices) == MeshCompactionResult::Success);
    assert((indices == std::vector<std::uint16_t>{0u, 1u, 2u, 2u, 1u, 3u}));

    const std::vector<std::uint32_t> invalid{0u, 1u, 6u};
    assert(compactIndexedVertices<Vertex>(source, invalid, compact, indices) == MeshCompactionResult::IndexOutOfRange);
    assert(compact.empty() && indices.empty());

    // All 65,536 representable indices are valid, including compact index 65535.
    std::vector<Vertex> largeSource(65537u);
    std::vector<std::uint32_t> max16Indices(65536u);
    for (std::uint32_t i = 0u; i < max16Indices.size(); ++i)
        max16Indices[i] = i;
    assert(compactIndexedVertices<Vertex>(largeSource, max16Indices, compact, indices) == MeshCompactionResult::Success);
    assert(compact.size() == 65536u && indices.size() == 65536u && indices.back() == 65535u);

    // A 65,537th distinct referenced vertex cannot be represented by 16-bit indices.
    std::vector<std::uint32_t> overflowIndices(65537u);
    for (std::uint32_t i = 0u; i < overflowIndices.size(); ++i)
        overflowIndices[i] = i;
    assert(compactIndexedVertices<Vertex>(largeSource, overflowIndices, compact, indices) == MeshCompactionResult::TooManyVertices);
    assert(compact.empty() && indices.empty());
}
