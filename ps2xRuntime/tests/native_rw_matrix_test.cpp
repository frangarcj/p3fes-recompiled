#include "game_overrides/p3fes_native_rw_math.h"
#include "game_overrides/p3fes_rw_matrix_math.h"
#include "ps2_runtime.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

void RwMatrixMultiply_0x4c2f30(uint8_t *, R5900Context *, PS2Runtime *);

namespace
{
    constexpr uint32_t kEntry = 0x004C2F30u, kReturn = 0x00123450u;
    constexpr uint32_t kFirst = 0x00100000u, kSecond = 0x00100100u, kOut = 0x00100200u;
    using Matrix = std::array<float, 16>;

    void argument(R5900Context &ctx, unsigned reg, uint32_t value)
    {
        ctx.r[reg] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    R5900Context context(uint32_t out)
    {
        R5900Context ctx;
        ctx.pc = kEntry;
        argument(ctx, 4, out); argument(ctx, 5, kFirst); argument(ctx, 6, kSecond);
        argument(ctx, 31, kReturn);
        ctx.vu0_vf[0] = _mm_set_ps(1.0f, 0.0f, 0.0f, 0.0f);
        for (unsigned i = 9; i <= 12; ++i)
            ctx.vu0_vf[i] = _mm_set_ps(float(i) + 0.25f, 7.0f, 8.0f, 9.0f);
        return ctx;
    }
}

int main()
{
    PS2Runtime::RecompiledFunction original = &RwMatrixMultiply_0x4c2f30;
    ps2SetRecompiledFunctionTable({&original, kEntry, kEntry + 4u, 1u});
    PS2Runtime runtime;
    assert(!p3fes_native_rw_math::install(runtime, false));
    assert(runtime.lookupFunction(kEntry) == &RwMatrixMultiply_0x4c2f30);
    assert(p3fes_native_rw_math::install(runtime));
    assert(p3fes_native_rw_math::install(runtime));
    const auto native = runtime.lookupFunction(kEntry);
    assert(native && native != &RwMatrixMultiply_0x4c2f30);

    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    std::mt19937 random(0x3F35u);
    std::uniform_real_distribution<float> values(-4.0f, 4.0f);
    for (unsigned trial = 0; trial < 1024; ++trial)
    {
        Matrix first{}, second{};
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                first[column * 4 + component] = values(random);
                second[column * 4 + component] = values(random);
            }
        const uint32_t flagsA = random(), flagsB = random();
        std::memcpy(first.data() + 3, &flagsA, 4u);
        std::memcpy(second.data() + 3, &flagsB, 4u);
        for (uint32_t output : {kOut, kFirst, kSecond})
        {
            const auto reset = [&] {
                std::memcpy(ram.data() + kFirst, first.data(), 64u);
                std::memcpy(ram.data() + kSecond, second.data(), 64u);
            };
            reset();
            auto reference = context(output);
            RwMatrixMultiply_0x4c2f30(ram.data(), &reference, &runtime);
            std::array<uint8_t, 64> expected;
            std::memcpy(expected.data(), ram.data() + output, 64u);
            reset();
            auto actual = context(output);
            native(ram.data(), &actual, &runtime);
            if (std::memcmp(expected.data(), ram.data() + output, 64u) != 0)
            {
                std::fprintf(stderr, "RW multiply differs from original: trial=%u alias=%08X\n", trial, output);
                return 1;
            }
            assert(actual.pc == kReturn && getRegU32(&actual, 2) == output);
            assert(getRegU32(&actual, 4) == output && getRegU32(&actual, 31) == kReturn);
        }
    }

    const auto compareWithGuest = [&](const Matrix &first, const Matrix &second, const char *label) {
        Matrix initialOut{};
        initialOut.fill(123.25f);
        const uint32_t outMetadata[4] = {0xA5A55A5Au, 0x12345678u, 0xCAFEBABEu, 0x87654321u};
        for (unsigned column = 0; column < 4; ++column)
            std::memcpy(&initialOut[column * 4 + 3], &outMetadata[column], sizeof(uint32_t));
        Matrix nativeResult = initialOut;
        const bool nativeOk = p3fes_rw_matrix_math::multiply(first, second, nativeResult);
        if (!nativeOk && std::memcmp(nativeResult.data(), initialOut.data(), 64u) != 0)
        {
            std::fprintf(stderr, "failed RW matrix kernel modified output: %s\n", label);
            return false;
        }
        const auto reset = [&] {
            std::memcpy(ram.data() + kFirst, first.data(), 64u);
            std::memcpy(ram.data() + kSecond, second.data(), 64u);
        };
        reset();
        auto guestContext = context(kOut);
        RwMatrixMultiply_0x4c2f30(ram.data(), &guestContext, &runtime);
        std::array<uint8_t, 64> guestBytes;
        std::memcpy(guestBytes.data(), ram.data() + kOut, guestBytes.size());
        reset();
        auto nativeContext = context(kOut);
        native(ram.data(), &nativeContext, &runtime);
        if (std::memcmp(guestBytes.data(), ram.data() + kOut, guestBytes.size()) != 0)
        {
            std::fprintf(stderr, "RW matrix extended-range result differs from guest: %s\n", label);
            return false;
        }
        return nativeContext.pc == kReturn && getRegU32(&nativeContext, 4) == kOut;
    };

    {
        Matrix first{};
        Matrix second{};
        first[0] = 1.0e-18f;
        first[5] = 1.0e-24f;
        first[10] = 1.0f;
        first[15] = 1.0f;
        second[0] = second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(p3fes_rw_matrix_math::multiply(first, second, output));
        assert(output[0] == first[0] && output[5] == first[5]);
        assert(compareWithGuest(first, second, "small-normal-1e-18-1e-24"));
    }

    {
        const float left = 1.0f;
        const float right = -std::nextafter(left, 0.0f);
        const float rounded = left + right;
        assert(p3fes_rw_matrix_math::safeSumResult(left, right, rounded));
        // Simulate ARMv7 FTZ for the subnormal result without mutating FPCR.
        const float nearMin = 2.0f * std::numeric_limits<float>::min();
        const float nextBelow = -std::nextafter(nearMin, 0.0f);
        assert(!p3fes_rw_matrix_math::safeSumResult(nearMin, nextBelow, 0.0f));
        assert(p3fes_rw_matrix_math::safeSumResult(1.0f, -1.0f, 0.0f));
        assert(p3fes_rw_matrix_math::safeSumResult(0.0f, -0.0f, 0.0f));

        Matrix first{};
        Matrix second{};
        first[0] = left;
        first[1] = right;
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = second[4] = second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(p3fes_rw_matrix_math::multiply(first, second, output));
        assert(output[0] == rounded);
        assert(compareWithGuest(first, second, "close-opposite-normal-add"));
    }

    // Wide finite normal inputs exercise the checked scalar path against the
    // generated guest implementation.
    {
        std::mt19937 wideRandom(0xF35A11u);
        std::uniform_real_distribution<float> mantissa(-1.0f, 1.0f);
        std::uniform_int_distribution<int> exponent(-120, 120);
        for (unsigned trial = 0; trial < 512; ++trial)
        {
            Matrix first{}, second{};
            for (unsigned column = 0; column < 4; ++column)
                for (unsigned component = 0; component < 3; ++component)
                {
                    const auto sample = [&] {
                        float value = std::ldexp(mantissa(wideRandom), exponent(wideRandom));
                        if (value == 0.0f)
                            value = std::numeric_limits<float>::min();
                        return value;
                    };
                    first[column * 4 + component] = sample();
                    second[column * 4 + component] = sample();
                }
            // Keep arbitrary metadata in all fourth lanes; they are not float
            // operands and the generated guest must preserve output padding.
            const uint32_t flagsA = wideRandom(), flagsB = wideRandom();
            std::memcpy(first.data() + 3, &flagsA, 4u);
            std::memcpy(second.data() + 3, &flagsB, 4u);
            assert(compareWithGuest(first, second, "wide-normal-random"));
        }
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = 1.0e35f;
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(p3fes_rw_matrix_math::multiply(first, second, output));
        assert(output[0] == first[0]);
        assert(compareWithGuest(first, second, "large-normal-1e35"));
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = first[5] = first[10] = first[15] = 1.0f;
        second[0] = second[5] = second[10] = second[15] = 1.0f;
        first[0] = 1.0e-30f;
        second[0] = 1.0e-20f; // Nonzero normals multiply to zero by underflow.
        Matrix output{};
        assert(!p3fes_rw_matrix_math::multiply(first, second, output));
        assert(compareWithGuest(first, second, "nonzero-product-underflow-to-zero"));
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = 1.0e-30f;
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = 1.0e-10f; // Nonzero product is subnormal.
        second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(!p3fes_rw_matrix_math::multiply(first, second, output));
        assert(compareWithGuest(first, second, "product-subnormal"));
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = 1.0e30f;
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = 1.0e30f;
        second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(!p3fes_rw_matrix_math::multiply(first, second, output));
        assert(compareWithGuest(first, second, "product-overflow"));
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = 2.0e38f;
        first[1] = 2.0e38f;
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = second[4] = second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(!p3fes_rw_matrix_math::multiply(first, second, output));
        assert(compareWithGuest(first, second, "sum-overflow"));
    }

    {
        Matrix first{};
        Matrix second{};
        first[0] = 2.0f * std::numeric_limits<float>::min();
        first[1] = -std::nextafter(first[0], 0.0f);
        first[5] = first[10] = first[15] = 1.0f;
        second[0] = second[4] = second[5] = second[10] = second[15] = 1.0f;
        Matrix output{};
        assert(!p3fes_rw_matrix_math::multiply(first, second, output));
        assert(compareWithGuest(first, second, "cancellation-to-subnormal"));
    }
    // Special inputs must use the original VU arithmetic, including its clamps.
    Matrix special{};
    special[0] = INFINITY;
    std::memcpy(ram.data() + kFirst, special.data(), 64u);
    std::memcpy(ram.data() + kSecond, special.data(), 64u);
    auto reference = context(kOut);
    RwMatrixMultiply_0x4c2f30(ram.data(), &reference, &runtime);
    std::array<uint8_t, 64> expected;
    std::memcpy(expected.data(), ram.data() + kOut, 64u);
    auto actual = context(kOut);
    native(ram.data(), &actual, &runtime);
    assert(std::memcmp(expected.data(), ram.data() + kOut, 64u) == 0);
    assert(actual.pc == kReturn);
    // A damaged/noncanonical VF0 must retain the actual guest behavior too.
    special[0] = special[5] = special[10] = 1.0f;
    special[12] = 7.0f; special[13] = 8.0f; special[14] = 9.0f;
    std::memcpy(ram.data() + kFirst, special.data(), 64u);
    std::memcpy(ram.data() + kSecond, special.data(), 64u);
    reference = context(kOut);
    reference.vu0_vf[0] = _mm_set_ps(0.5f, 0.0f, 0.0f, 0.0f);
    RwMatrixMultiply_0x4c2f30(ram.data(), &reference, &runtime);
    std::memcpy(expected.data(), ram.data() + kOut, 64u);
    actual = context(kOut);
    actual.vu0_vf[0] = reference.vu0_vf[0];
    native(ram.data(), &actual, &runtime);
    assert(std::memcmp(expected.data(), ram.data() + kOut, 64u) == 0);
    std::puts("RW native multiply matches original in 3072 affine/alias cases, extended normals, and fallback cases");
}
