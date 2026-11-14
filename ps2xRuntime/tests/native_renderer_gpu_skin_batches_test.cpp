#include "gpu_skin_batches.h"
#include "skinning.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace p3fes::native_renderer;
using namespace p3fes::native_renderer::detail;

namespace
{
void assertPoseClose(const Vertex &actual, const Vertex &expected, float epsilon = 1.0e-6f)
{
    for (std::size_t axis = 0u; axis < 3u; ++axis)
    {
        assert(std::abs(actual.position[axis] - expected.position[axis]) < epsilon);
        assert(std::abs(actual.normal[axis] - expected.normal[axis]) < epsilon);
    }
    assert(actual.texCoord == expected.texCoord);
    assert(actual.color == expected.color);
}

void assertVertexUnchanged(const Vertex &actual, const Vertex &expected)
{
    assert(actual.position == expected.position);
    assert(actual.normal == expected.normal);
    assert(actual.texCoord == expected.texCoord);
    assert(actual.color == expected.color);
}

void setZRotationTranslation(Matrix4 &matrix, float angle, float tx, float ty, float tz)
{
    matrix = kIdentityMatrix;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    matrix[0] = cosine;
    matrix[1] = sine;
    matrix[4] = -sine;
    matrix[5] = cosine;
    matrix[12] = tx;
    matrix[13] = ty;
    matrix[14] = tz;
}

void testTriangleBatchOrderAndRotatedNormals()
{
    std::vector<Vertex> vertices(30u * 3u);
    std::vector<std::uint16_t> indices;
    indices.reserve(vertices.size());
    for (std::uint32_t triangle = 0; triangle < 30u; ++triangle)
    {
        for (std::uint32_t corner = 0; corner < 3u; ++corner)
        {
            Vertex &v = vertices[triangle * 3u + corner];
            v.position = {static_cast<float>(corner) + triangle * 0.125f, 1.0f, 2.0f};
            v.normal = {0.2f + corner * 0.1f, 0.3f, 0.9f};
            v.texCoord = {triangle * 0.25f + corner * 0.0625f, corner * 0.125f};
            v.color = {static_cast<std::uint8_t>(triangle), static_cast<std::uint8_t>(corner), 137u, 255u};
            v.bones = {static_cast<std::uint8_t>(triangle), 0u, 0u, 0u};
            v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
            indices.push_back(static_cast<std::uint16_t>(triangle * 3u + corner));
        }
    }

    std::vector<GpuSkinBatch> batches;
    assert(buildGpuSkinBatches(vertices, indices, 30u, 24u, batches) == SkinBatchResult::Success);
    assert(batches.size() == 2u);
    assert(batches[0].paletteBones.size() == 24u && batches[1].paletteBones.size() == 6u);
    assert(batches[0].indices.size() == 72u && batches[1].indices.size() == 18u);
    for (std::size_t i = 0u; i < batches[0].paletteBones.size(); ++i)
        assert(batches[0].paletteBones[i] == i);
    for (std::size_t i = 0u; i < batches[1].paletteBones.size(); ++i)
        assert(batches[1].paletteBones[i] == i + 24u);

    // Distinct rotations and translations make both local palette remapping and
    // transformed normals observable. The source triangle/corner order must be
    // preserved across the batch split.
    std::vector<Matrix4> fullPalette(30u, kIdentityMatrix);
    for (std::size_t bone = 0u; bone < fullPalette.size(); ++bone)
    {
        const float angle = static_cast<float>(bone + 1u) * 0.03125f;
        setZRotationTranslation(fullPalette[bone], angle,
                                static_cast<float>(bone) * 0.25f,
                                static_cast<float>(bone) * -0.125f,
                                static_cast<float>(bone) * 0.0625f);
    }

    std::size_t streamOffset = 0u;
    for (const GpuSkinBatch &batch : batches)
    {
        std::vector<Matrix4> localPalette;
        for (std::uint8_t bone : batch.paletteBones)
            localPalette.push_back(fullPalette[bone]);
        assert(batch.indices.size() % 3u == 0u);
        for (std::size_t i = 0u; i < batch.indices.size(); ++i)
        {
            const std::uint16_t sourceIndex = indices[streamOffset + i];
            const Vertex &source = vertices[sourceIndex];
            const Vertex &remapped = batch.vertices[batch.indices[i]];
            assert(remapped.position == source.position);
            assert(remapped.texCoord == source.texCoord);
            assert(remapped.color == source.color);
            assert(remapped.weights == source.weights);
            assert(remapped.bones[0] < batch.paletteBones.size());
            assert(batch.paletteBones[remapped.bones[0]] == source.bones[0]);

            const Vertex actual = poseSkinVertex(remapped, localPalette);
            const Vertex expected = poseSkinVertex(source, fullPalette);
            assertPoseClose(actual, expected);
        }
        streamOffset += batch.indices.size();
    }
    assert(streamOffset == indices.size());
}

void testExactUint16VertexLimitAndRepeatedTailIndices()
{
    // Cover every representable uint16 vertex index, then repeat indices 0/1
    // to complete the final triangle. The batch must retain 65,536 vertices
    // and remap index 65,535 without wrapping or splitting.
    constexpr std::size_t kVertexCount = 65536u;
    std::vector<Vertex> vertices(kVertexCount);
    std::vector<std::uint16_t> indices;
    indices.reserve(kVertexCount + 2u);
    for (std::size_t i = 0u; i < kVertexCount; ++i)
    {
        Vertex &v = vertices[i];
        v.position = {static_cast<float>(i), static_cast<float>(i % 101u), -static_cast<float>(i % 37u)};
        v.normal = {0.0f, 1.0f, 0.0f};
        v.texCoord = {static_cast<float>(i % 257u) / 256.0f, static_cast<float>(i % 127u) / 126.0f};
        v.color = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8u), 37u, 255u};
        v.bones = {0u, 0u, 0u, 0u};
        v.weights = {1.0f, 0.0f, 0.0f, 0.0f};
        indices.push_back(static_cast<std::uint16_t>(i));
    }
    indices.push_back(0u);
    indices.push_back(1u);
    assert(indices.size() % 3u == 0u);

    std::vector<GpuSkinBatch> batches;
    assert(buildGpuSkinBatches(vertices, indices, 1u, 1u, batches) == SkinBatchResult::Success);
    assert(batches.size() == 1u);
    const GpuSkinBatch &batch = batches[0];
    assert(batch.vertices.size() == kVertexCount);
    assert(batch.indices.size() == kVertexCount + 2u);
    assert(batch.paletteBones.size() == 1u && batch.paletteBones[0] == 0u);
    for (std::size_t i = 0u; i < kVertexCount; ++i)
    {
        assert(batch.indices[i] == i);
        const Vertex &actual = batch.vertices[batch.indices[i]];
        const Vertex &expected = vertices[i];
        assertVertexUnchanged(actual, expected);
        assert(actual.bones == expected.bones);
        assert(actual.weights == expected.weights);
    }
    assert(batch.indices[kVertexCount - 1u] == std::numeric_limits<std::uint16_t>::max());
    assert(batch.indices[kVertexCount] == 0u);
    assert(batch.indices[kVertexCount + 1u] == 1u);
}

void testInvalidAndNearZeroWeightsReturnSourcePose()
{
    const std::vector<std::uint16_t> oneTriangle{0u, 1u, 2u};
    std::vector<GpuSkinBatch> batches;
    std::vector<Matrix4> fullPalette(3u, kIdentityMatrix);
    setZRotationTranslation(fullPalette[0], 0.75f, 100.0f, 20.0f, -5.0f);
    setZRotationTranslation(fullPalette[1], -0.5f, -80.0f, 3.0f, 12.0f);
    setZRotationTranslation(fullPalette[2], 0.25f, 40.0f, -10.0f, 8.0f);

    Vertex invalid{};
    invalid.position = {4.0f, -3.0f, 2.0f};
    invalid.normal = {0.2f, 0.7f, 0.4f};
    invalid.texCoord = {0.25f, 0.75f};
    invalid.color = {17u, 83u, 191u, 255u};
    invalid.bones = {255u, 1u, 0u, 2u};
    invalid.weights = {0.25f, std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(), -0.5f};
    const std::vector<Vertex> invalidVertices{invalid, invalid, invalid};
    assert(buildGpuSkinBatches(invalidVertices, oneTriangle, 3u, 24u, batches) == SkinBatchResult::Success);
    assert(batches.size() == 1u && batches[0].paletteBones.empty());
    for (std::size_t i = 0u; i < 3u; ++i)
    {
        const Vertex &remapped = batches[0].vertices[batches[0].indices[i]];
        for (std::size_t influence = 0u; influence < 4u; ++influence)
            assert(remapped.weights[influence] == 0.0f);
        const Vertex actual = poseSkinVertex(remapped, fullPalette);
        const Vertex expected = poseSkinVertex(invalidVertices[i], fullPalette);
        assertVertexUnchanged(actual, expected);
    }

    // Positive but tiny valid weights still fail the poseSkinVertex weight-sum
    // threshold and must preserve the source pose, even with a non-empty palette.
    Vertex nearZero = invalid;
    nearZero.bones = {0u, 1u, 2u, 0u};
    nearZero.weights = {4.0e-9f, 5.0e-9f, 0.0f, 0.0f};
    const std::vector<Vertex> nearZeroVertices{nearZero, nearZero, nearZero};
    assert(buildGpuSkinBatches(nearZeroVertices, oneTriangle, 3u, 24u, batches) == SkinBatchResult::Success);
    assert(batches.size() == 1u && batches[0].paletteBones.size() == 2u);
    for (std::size_t i = 0u; i < 3u; ++i)
    {
        const Vertex &remapped = batches[0].vertices[batches[0].indices[i]];
        std::vector<Matrix4> localPalette;
        for (std::uint8_t bone : batches[0].paletteBones)
            localPalette.push_back(fullPalette[bone]);
        const Vertex actual = poseSkinVertex(remapped, localPalette);
        const Vertex expected = poseSkinVertex(nearZeroVertices[i], fullPalette);
        assertVertexUnchanged(actual, expected);
    }

    Vertex atThreshold = invalid;
    atThreshold.bones = {2u, 0u, 0u, 0u};
    atThreshold.weights = {1.0e-8f, 0.0f, 0.0f, 0.0f};
    const std::vector<Vertex> thresholdVertices{atThreshold, atThreshold, atThreshold};
    assert(buildGpuSkinBatches(thresholdVertices, oneTriangle, 3u, 24u, batches) == SkinBatchResult::Success);
    assert(batches.size() == 1u && batches[0].paletteBones.size() == 1u);
    const Vertex &thresholdRemapped = batches[0].vertices[batches[0].indices[0]];
    std::vector<Matrix4> thresholdPalette{fullPalette[batches[0].paletteBones[0]]};
    assertVertexUnchanged(poseSkinVertex(thresholdRemapped, thresholdPalette),
                          poseSkinVertex(thresholdVertices[0], fullPalette));
}

void testTriangleThatExceedsBoneBudgetIsRejected()
{
    const std::vector<std::uint16_t> oneTriangle{0u, 1u, 2u};
    std::vector<Vertex> tooMany(3u);
    for (std::size_t i = 0u; i < 3u; ++i)
    {
        tooMany[i].bones = {static_cast<std::uint8_t>(i * 2u),
                            static_cast<std::uint8_t>(i * 2u + 1u), 0u, 0u};
        tooMany[i].weights = {0.5f, 0.5f, 0.0f, 0.0f};
    }
    std::vector<GpuSkinBatch> batches;
    assert(buildGpuSkinBatches(tooMany, oneTriangle, 6u, 5u, batches) ==
           SkinBatchResult::TooManyBonesInTriangle);
    assert(batches.empty());
}
} // namespace

int main()
{
    testTriangleBatchOrderAndRotatedNormals();
    testExactUint16VertexLimitAndRepeatedTailIndices();
    testInvalidAndNearZeroWeightsReturnSourcePose();
    testTriangleThatExceedsBoneBudgetIsRejected();
}
