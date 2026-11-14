#include "game_overrides/p3fes_skin_palette_math.h"
#include "ps2_runtime.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

void MatrixInvertGeneric_0x4c2860(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

namespace
{
    constexpr uint32_t kInverseEntry = 0x004C2860u;
    constexpr uint32_t kSourceAddress = 0x00100000u;
    constexpr uint32_t kDestinationAddress = 0x00100100u;
    constexpr uint32_t kStackPointer = 0x001FF000u;
    constexpr uint32_t kReturnAddress = 0x00123450u;
    using Matrix = p3fes_skin_palette_math::Matrix;

    void require(bool condition, const char *message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "native skin palette math test failed: %s\n", message);
        std::exit(1);
    }

    bool close(float a, float b, float tolerance = 1.0e-5f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    void requireAffineNear(const Matrix &actual, const Matrix &expected, float tolerance,
                           const char *label, uint32_t trial = 0u)
    {
        for (unsigned component : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u, 12u, 13u, 14u})
        {
            const float error = std::fabs(actual[component] - expected[component]);
            const float limit = tolerance * std::max(1.0f, std::fabs(expected[component]));
            if (std::isfinite(actual[component]) && std::isfinite(expected[component]) && error <= limit)
                continue;
            std::fprintf(stderr,
                         "native skin inverse mismatch: %s trial=%u component=%u actual=%.9g expected=%.9g abs=%.9g limit=%.9g\n",
                         label, trial, component, static_cast<double>(actual[component]),
                         static_cast<double>(expected[component]), static_cast<double>(error),
                         static_cast<double>(limit));
            std::exit(1);
        }
    }

    Matrix identity()
    {
        return p3fes_skin_palette_math::identity();
    }

    Matrix translation(float x, float y, float z)
    {
        Matrix result = identity();
        result[12] = x;
        result[13] = y;
        result[14] = z;
        return result;
    }

    Matrix rotationZ90()
    {
        Matrix result = identity();
        result[0] = 0.0f;
        result[1] = 1.0f;
        result[4] = -1.0f;
        result[5] = 0.0f;
        return result;
    }

    void setArgument(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    Matrix generatedInverse(PS2Runtime &runtime, std::vector<uint8_t> &rdram, const Matrix &source,
                            uint32_t *determinantBits = nullptr)
    {
        std::memcpy(rdram.data() + kSourceAddress, source.data(), sizeof(Matrix));
        Matrix destination = identity();
        std::memcpy(rdram.data() + kDestinationAddress, destination.data(), sizeof(Matrix));
        R5900Context ctx;
        ctx.pc = kInverseEntry;
        setArgument(ctx, 4u, kDestinationAddress);
        setArgument(ctx, 5u, kSourceAddress);
        setArgument(ctx, 29u, kStackPointer);
        setArgument(ctx, 31u, kReturnAddress);
        const auto function = runtime.lookupFunction(kInverseEntry);
        require(function == &MatrixInvertGeneric_0x4c2860,
                "test function table did not expose generated 0x4C2860");
        function(rdram.data(), &ctx, &runtime);
        require(ctx.pc == kReturnAddress && getRegU32(&ctx, 29u) == kStackPointer,
                "generated inverse did not return with its stack restored");
        std::memcpy(destination.data(), rdram.data() + kDestinationAddress, sizeof(Matrix));
        if (determinantBits)
            std::memcpy(determinantBits, rdram.data() + kStackPointer - 4u, sizeof(uint32_t));
        return destination;
    }
}

int main()
{
    PS2Runtime::RecompiledFunction generated = &MatrixInvertGeneric_0x4c2860;
    ps2SetRecompiledFunctionTable({&generated, kInverseEntry, kInverseEntry + 4u, 1u});
    PS2Runtime runtime;
    std::vector<uint8_t> rdram(PS2_RAM_SIZE);

    {
        Matrix source = rotationZ90();
        source[12] = 5.0f;
        source[13] = 7.0f;
        source[14] = -3.0f;
        Matrix inverse{};
        require(p3fes_skin_palette_math::invertOrthonormal(source, inverse),
                "orthonormal matrix inversion rejected ordinary values");
        require(close(inverse[0], 0.0f) && close(inverse[1], -1.0f) &&
                    close(inverse[4], 1.0f) && close(inverse[5], 0.0f),
                "inverse basis was not transposed");
        require(close(inverse[12], -7.0f) && close(inverse[13], 5.0f) && close(inverse[14], 3.0f),
                "inverse translation did not use negative R-transpose times translation");
        Matrix product = identity();
        require(p3fes_rw_matrix_math::multiply(inverse, source, product),
                "inverse/source product failed");
        for (unsigned i : {0u, 5u, 10u, 15u})
            require(close(product[i], 1.0f), "inverse product diagonal was not identity");
        for (unsigned i : {1u, 2u, 4u, 6u, 8u, 9u, 12u, 13u, 14u})
            require(close(product[i], 0.0f), "inverse product off-diagonal/translation was not zero");
    }

    {
        require(p3fes_skin_palette_math::hierarchyMode(0x2u) ==
                    p3fes_skin_palette_math::HierarchyMode::FrameNodes,
                "flags&2 did not select frame-node hierarchy mode");
        require(p3fes_skin_palette_math::hierarchyMode(0x4000u) ==
                    p3fes_skin_palette_math::HierarchyMode::LocalMatrices,
                "flags&0x4000 did not select local-matrix mode");
        require(p3fes_skin_palette_math::hierarchyMode(0u) ==
                    p3fes_skin_palette_math::HierarchyMode::WorldMatrices,
                "unflagged hierarchy did not select contiguous world-matrix mode");
        require(p3fes_skin_palette_math::hierarchyMode(0x4002u) ==
                    p3fes_skin_palette_math::HierarchyMode::FrameNodes,
                "flags&2 branch did not take precedence when both bits are present");

        Matrix stored = translation(4.0f, -2.0f, 7.0f);
        Matrix copied{};
        require(p3fes_skin_palette_math::invertRwMatrix(stored, 0x20003u, 0x20000u, copied),
                "RwMatrixInvert special copy path was rejected");
        require(close(copied[12], 4.0f) && close(copied[13], -2.0f) && close(copied[14], 7.0f),
                "RwMatrixInvert special path inverted instead of copying");
    }

    {
        // Rotation mixed with nonuniform scale and shear follows the generic
        // 0x4C2860 path (source flags=0, global flags=0x20000, so no copy).
        Matrix source{};
        source[0] = 1.8f;  source[1] = 0.35f; source[2] = -0.2f;
        source[4] = -0.55f; source[5] = 2.7f; source[6] = 0.45f;
        source[8] = 0.3f; source[9] = -0.4f; source[10] = 1.35f;
        source[12] = 8.0f; source[13] = -3.5f; source[14] = 12.0f; source[15] = 1.0f;

        Matrix native{};
        require(p3fes_skin_palette_math::invertRwMatrix(source, 0u, 0x20000u, native),
                "generic affine inverse rejected rotation/scale/shear input");
        const Matrix guest = generatedInverse(runtime, rdram, source);
        requireAffineNear(native, guest, 2.0e-6f, "rotation-scale-shear");
        Matrix product = identity();
        require(p3fes_rw_matrix_math::multiply(native, source, product),
                "generic inverse/source product failed");
        for (unsigned i : {0u, 5u, 10u, 15u})
            require(close(product[i], 1.0f, 2.0e-5f), "generic inverse product diagonal was not identity");
        for (unsigned i : {1u, 2u, 4u, 6u, 8u, 9u, 12u, 13u, 14u})
            require(close(product[i], 0.0f, 2.0e-5f),
                    "generic inverse product off-diagonal/translation was not zero");

        Matrix singular = identity();
        singular[10] = 0.0f;
        Matrix singularNative{};
        require(p3fes_skin_palette_math::invertRwMatrix(singular, 0u, 0x20000u, singularNative),
                "guest-compatible singular inverse fallback was rejected");
        const Matrix singularGenerated = generatedInverse(runtime, rdram, singular);
        requireAffineNear(singularNative, singularGenerated, 1.0e-6f, "singular");

        Matrix unsafe = identity();
        unsafe[0] = std::numeric_limits<float>::infinity();
        Matrix ignored{};
        require(!p3fes_skin_palette_math::invertRwMatrix(unsafe, 0u, 0x20000u, ignored),
                "nonfinite generic inverse input bypassed the guest-helper fallback");

        std::mt19937 random(0x4C2860u);
        std::uniform_real_distribution<float> coefficient(-1.75f, 1.75f);
        std::uniform_real_distribution<float> position(-30.0f, 30.0f);
        uint32_t matched = 0u;
        for (uint32_t attempt = 0u; attempt < 4096u && matched < 512u; ++attempt)
        {
            Matrix varied{};
            for (unsigned i : {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u})
                varied[i] = coefficient(random);
            for (unsigned i : {12u, 13u, 14u})
                varied[i] = position(random);
            varied[15] = 1.0f;
            Matrix nativeVaried{};
            if (!p3fes_skin_palette_math::invertRwMatrix(varied, 0u, 0x20000u, nativeVaried))
                continue;
            const Matrix guestVaried = generatedInverse(runtime, rdram, varied);
            requireAffineNear(nativeVaried, guestVaried, 3.0e-6f,
                              "randomized-generated-0x4C2860", matched);
            ++matched;
        }
        require(matched == 512u, "could not produce enough safe randomized inverse fixtures");

        // Probe sign combinations of zero coefficients through the real
        // generated callee. If its determinant scratch is -0, the native
        // inverse must delegate that exceptional division to the guest.
        bool negativeZeroFixture = false;
        const unsigned linearIndices[] = {0u, 1u, 2u, 4u, 5u, 6u, 8u, 9u, 10u};
        for (uint32_t mask = 0u; mask < (1u << 9u) && !negativeZeroFixture; ++mask)
        {
            Matrix zeroMatrix{};
            zeroMatrix[15] = 1.0f;
            for (unsigned bit = 0u; bit < 9u; ++bit)
                if ((mask & (1u << bit)) != 0u)
                {
                    const uint32_t negativeZero = 0x80000000u;
                    std::memcpy(&zeroMatrix[linearIndices[bit]], &negativeZero, sizeof(negativeZero));
                }
            uint32_t determinantBits = 0u;
            (void)generatedInverse(runtime, rdram, zeroMatrix, &determinantBits);
            if (determinantBits == 0x80000000u)
            {
                Matrix ignored{};
                require(!p3fes_skin_palette_math::invertRwMatrix(zeroMatrix, 0u, 0u, ignored),
                        "native inverse consumed negative-zero determinant instead of falling back");
                negativeZeroFixture = true;
            }
        }
    }

    {
        Matrix local = rotationZ90();
        Matrix bind = translation(2.0f, 0.0f, 0.0f);
        std::vector<Matrix> output;
        require(p3fes_skin_palette_math::buildLocalPalette({local}, {bind}, output),
                "local-space palette construction failed");
        require(output.size() == 1u && close(output[0][12], 0.0f) && close(output[0][13], 2.0f),
                "local palette used inverseBind * local instead of local * inverseBind");
    }

    {
        Matrix atomicInverse = translation(-10.0f, 0.0f, 0.0f);
        Matrix bone = translation(10.0f, 3.0f, 0.0f);
        Matrix bind = translation(0.0f, 2.0f, 0.0f);
        std::vector<Matrix> output;
        require(p3fes_skin_palette_math::buildFramePalette(atomicInverse, {bone}, {bind}, output),
                "frame-derived palette construction failed");
        require(output.size() == 1u && close(output[0][12], 0.0f) && close(output[0][13], 5.0f),
                "frame palette did not compute inverseAtomic * (boneLTM * inverseBind)");
    }

    {
        std::vector<Matrix> output;
        require(p3fes_skin_palette_math::buildIdentityPalette(3u, output) && output.size() == 3u,
                "missing-hierarchy identity palette failed");
        require(close(output[2][0], 1.0f) && close(output[2][5], 1.0f) &&
                    close(output[2][10], 1.0f) && close(output[2][15], 1.0f),
                "identity palette contents were incorrect");
        require(!p3fes_skin_palette_math::buildLocalPalette({identity()}, {}, output),
                "local hierarchy count mismatch was accepted");
        require(!p3fes_skin_palette_math::buildFramePalette(identity(), {identity(), identity()},
                                                            {identity()}, output),
                "frame hierarchy count mismatch was accepted");
    }

    std::puts("native skin palette math test passed");
    return 0;
}
