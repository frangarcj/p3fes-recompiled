// Host test: p3fes_rw_matrix_math::multiply (fast pre-check path) must return the same bool and
// leave bit-identical output as the original exact implementation, which is kept below verbatim
// as the reference (namespace reference).
//
// Build (from the repository root); run once per variant, both must pass:
//   clang++ -std=c++17 -O2 -Wall -Wextra -ffp-contract=off -I ps2xRuntime/src/lib/game_overrides \
//     ps2xRuntime/src/lib/game_overrides/tests/p3fes_rw_matrix_math_test.cpp -o rw_matrix_test && ./rw_matrix_test
//   clang++ ... -DP3FES_RW_MATRIX_FORCE_SCALAR ...   (scalar fallback instead of NEON on ARM hosts)

#include "p3fes_rw_matrix_math.h"
#include "p3fes_skin_palette_math.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#if defined(__GNUC__) && !defined(__clang__)
#define P3FES_RW_MATRIX_NO_FP_CONTRACT __attribute__((optimize("fp-contract=off")))
#else
#define P3FES_RW_MATRIX_NO_FP_CONTRACT
#endif

namespace reference
{
    using Matrix = std::array<float, 16>;
    inline bool ordinary(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        bits &= 0x7FFFFFFFu;
        // Conservative legacy fast-path bounds. Finite normal values outside
        // this range use checked scalar operations below.
        return bits == 0u || (bits >= 0x27800000u && bits <= 0x4F800000u);
    }

    inline bool normalOrZero(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        bits &= 0x7FFFFFFFu;
        // Includes finite normal values across the IEEE range and signed zero;
        // excludes subnormals, infinities and NaNs.
        return bits == 0u || (bits >= 0x00800000u && bits <= 0x7F7FFFFFu);
    }

    inline bool safeMultiply(float a, float b, float &result)
    {
        if (!normalOrZero(a) || !normalOrZero(b))
            return false;
        result = a * b;
        if (!normalOrZero(result))
            return false;
        // A nonzero product rounded all the way to zero is an underflow path
        // whose guest behavior may clamp differently.
        return a == 0.0f || b == 0.0f || result != 0.0f;
    }

    inline bool safeSumResult(float a, float b, float result)
    {
        if (!normalOrZero(a) || !normalOrZero(b))
            return false;
        if (!normalOrZero(result))
            return false;
        // ARMv7 may flush a subnormal sum to zero. Only accept zero when the
        // inputs really cancel (or both inputs are signed zero).
        return result != 0.0f || (a == 0.0f && b == 0.0f) || a == -b;
    }

    inline bool safeAdd(float a, float b, float &result)
    {
        if (!normalOrZero(a) || !normalOrZero(b))
            return false;
        result = a + b;
        return safeSumResult(a, b, result);
    }

    // RwMatrixMultiply(out, first, second): column-vector equivalent second*first.
    // RW's fourth lanes are flags/padding, not homogeneous float coordinates.
    // Preserve those lanes in out; read both sources fully before any write.
    P3FES_RW_MATRIX_NO_FP_CONTRACT inline bool multiply(
        const Matrix &first, const Matrix &second, Matrix &out)
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma STDC FP_CONTRACT OFF
#endif
        bool useConservativeFastPath = true;
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                const float a = first[column * 4 + component];
                const float b = second[column * 4 + component];
                if (!normalOrZero(a) || !normalOrZero(b))
                    return false;
                useConservativeFastPath = useConservativeFastPath && ordinary(a) && ordinary(b);
            }

        Matrix result = out;
        if (!useConservativeFastPath)
        {
            for (unsigned column = 0; column < 4; ++column)
                for (unsigned component = 0; component < 3; ++component)
                {
                    float x, y, z, xy, xyz, value;
                    if (!safeMultiply(second[component], first[column * 4], x) ||
                        !safeMultiply(second[4 + component], first[column * 4 + 1], y) ||
                        !safeMultiply(second[8 + component], first[column * 4 + 2], z) ||
                        !safeAdd(x, y, xy) || !safeAdd(xy, z, xyz))
                        return false;
                    if (column == 3)
                    {
                        if (!safeAdd(xyz, second[12 + component], value))
                            return false;
                    }
                    else
                        value = xyz;
                    result[column * 4 + component] = value;
                }
            out = result;
            return true;
        }

        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                const float x = second[component] * first[column * 4];
                const float y = second[4 + component] * first[column * 4 + 1];
                const float z = second[8 + component] * first[column * 4 + 2];
                const float xy = x + y;
                const float xyz = xy + z;
                result[column * 4 + component] = column == 3 ? xyz + second[12 + component] : xyz;
            }
        out = result;
        return true;
    }

}

namespace
{
    using p3fes_rw_matrix_math::Matrix;

    uint64_t g_cases = 0, g_failures = 0, g_trueCount = 0, g_falseCount = 0, g_fastCount = 0;

    float fromBits(uint32_t bits)
    {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    bool sameBits(const Matrix &a, const Matrix &b)
    {
        return std::memcmp(a.data(), b.data(), sizeof(float) * 16) == 0;
    }

    void check(const Matrix &first, const Matrix &second, const Matrix &outInit, const char *label)
    {
        Matrix refOut = outInit, newOut = outInit;
        const bool refOk = reference::multiply(first, second, refOut);
        const bool newOk = p3fes_rw_matrix_math::multiply(first, second, newOut);
        // Aliased destination (out == first) must also match the reference's aliased result.
        Matrix refAlias = first, newAlias = first;
        const bool refAliasOk = reference::multiply(refAlias, second, refAlias);
        const bool newAliasOk = p3fes_rw_matrix_math::multiply(newAlias, second, newAlias);
        ++g_cases;
        (newOk ? g_trueCount : g_falseCount)++;
        if (newOk && p3fes_rw_matrix_math::allOrdinaryOperands(first.data(), second.data()))
            ++g_fastCount;
        if (refOk != newOk || !sameBits(refOut, newOut) || refAliasOk != newAliasOk ||
            !sameBits(refAlias, newAlias))
        {
            if (++g_failures <= 10)
                std::printf("MISMATCH (%s): ref=%d new=%d aliasRef=%d aliasNew=%d\n", label, refOk, newOk,
                            refAliasOk, newAliasOk);
        }
    }

    // Value pool covering every operand class: zeros, ordinary, borderline of the ordinary range,
    // normal but outside it, denormals, infinities, NaNs, extremes.
    float poolValue(std::mt19937 &rng, int cls)
    {
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        std::uniform_int_distribution<uint32_t> any(0u, 0xFFFFFFFFu);
        const uint32_t sign = (rng() & 1u) << 31;
        switch (cls)
        {
        case 0: return unit(rng) * 10.0f;                              // ordinary
        case 1: return (rng() & 1u) ? 0.0f : -0.0f;                    // zeros
        case 2: return fromBits(sign | (any(rng) & 0x007FFFFFu));      // denormals (incl. zero payload)
        case 3: return fromBits(sign | 0x7F800000u);                   // infinities
        case 4: return fromBits(sign | 0x7F800000u | (any(rng) & 0x007FFFFFu) | 1u); // NaNs
        case 5: return fromBits(sign | (0x7F000000u + (any(rng) & 0x007FFFFFu)));     // huge
        case 6: return fromBits(sign | (0x00800000u + (any(rng) & 0x007FFFFFu)));     // tiny normal
        case 7: return fromBits(sign | (0x27800000u - 2u + (any(rng) % 5u)));         // lower ordinary border
        case 8: return fromBits(sign | (0x4F800000u - 2u + (any(rng) % 5u)));         // upper ordinary border
        case 9: return fromBits(sign | (0x27800000u + (any(rng) % 0x28000001u)));     // anywhere in ordinary range
        case 10: return fromBits(sign | (0x00800000u + (any(rng) % 0x7F000000u)));    // any normal
        default: return fromBits(any(rng));                                           // totally random
        }
    }

    Matrix randomMatrix(std::mt19937 &rng, int mode)
    {
        Matrix m;
        for (unsigned i = 0; i < 16; ++i)
        {
            int cls = 0;
            switch (mode)
            {
            case 0: cls = 0; break;                               // ordinary only
            case 1: cls = 9; break;                               // whole ordinary range (big products)
            case 2: cls = (rng() % 4u == 0u) ? 1 : 0; break;      // ordinary with zeros
            case 3: cls = static_cast<int>(rng() % 12u); break;   // everything
            case 4: cls = (rng() % 8u == 0u) ? static_cast<int>(rng() % 12u) : 9; break; // mostly ordinary, sparse exceptions
            default: cls = (rng() % 3u == 0u) ? static_cast<int>(7 + rng() % 2u) : 9; break; // borders
            }
            m[i] = poolValue(rng, cls);
        }
        return m;
    }
}

namespace
{
    // The skin palette's previous per-bone formula (multiplyFast + acc finite check), verbatim.
    void referenceMultiplyFast(const Matrix &first, const Matrix &second, Matrix &out, float &acc)
    {
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                float value = second[component] * first[column * 4] +
                              second[4 + component] * first[column * 4 + 1] +
                              second[8 + component] * first[column * 4 + 2];
                if (column == 3)
                    value += second[12 + component];
                out[column * 4 + component] = value;
                acc += value * 0.0f;
            }
    }

    uint64_t g_paletteCases = 0, g_paletteFailures = 0;

    void checkPaletteEntry(const Matrix &atomicInverse, const Matrix &bone, const Matrix &bind)
    {
        Matrix refTemp = p3fes_skin_palette_math::identity();
        Matrix refOut = p3fes_skin_palette_math::identity();
        float acc = 0.0f;
        referenceMultiplyFast(bind, bone, refTemp, acc);
        referenceMultiplyFast(refTemp, atomicInverse, refOut, acc);
        const bool refFinite = acc == 0.0f;

        Matrix out;
        out.fill(fromBits(0xDEADBEEFu));
        const bool finite = p3fes_skin_palette_math::framePaletteEntry(atomicInverse.data(), bone.data(),
                                                                       bind.data(), out.data());
        ++g_paletteCases;
        // Results must match bit for bit whenever the reference says finite; both must agree on finiteness.
        // (A NaN payload may differ between the two evaluation orders of the finite check only.)
        if (finite != refFinite || (refFinite && !sameBits(out, refOut)))
        {
            if (++g_paletteFailures <= 10)
                std::printf("PALETTE MISMATCH finite ref=%d new=%d\n", refFinite, finite);
        }
    }
}

int main()
{
    {
        // Vector predicates against their per-element definitions (padding lanes ignored).
        std::mt19937 prng(777u);
        uint64_t predicateFailures = 0;
        for (int mode = 0; mode < 6; ++mode)
            for (int i = 0; i < 300000; ++i)
            {
                const Matrix m = randomMatrix(prng, mode);
                bool normal = true, finite = true;
                for (unsigned column = 0; column < 4; ++column)
                    for (unsigned c = 0; c < 3; ++c)
                    {
                        const float v = m[column * 4 + c];
                        normal = normal && p3fes_rw_matrix_math::normalOrZero(v);
                        finite = finite && std::isfinite(v);
                    }
                predicateFailures += p3fes_rw_matrix_math::allNormalOrZeroXyz(m.data()) != normal;
                predicateFailures += p3fes_rw_matrix_math::allFiniteXyz(m.data()) != finite;
            }
        std::printf("predicate failures=%llu\n", (unsigned long long)predicateFailures);
        g_failures += predicateFailures;
    }
    {
        std::mt19937 prng(12345u);
        for (int mode = 0; mode < 6; ++mode)
            for (int i = 0; i < 200000; ++i)
            {
                Matrix a = randomMatrix(prng, mode), b = randomMatrix(prng, mode), c = randomMatrix(prng, mode);
                checkPaletteEntry(a, b, c);
            }
    }
    std::mt19937 rng(0xC0FFEEu);
    const char *modeNames[] = {"ordinary", "ordinary-range", "ordinary+zeros", "all-classes", "sparse-exceptions", "borders"};
    for (int mode = 0; mode < 6; ++mode)
        for (int i = 0; i < 400000; ++i)
        {
            const Matrix a = randomMatrix(rng, mode);
            const Matrix b = randomMatrix(rng, mode);
            Matrix out = randomMatrix(rng, 3); // junk incl. NaN lanes: lanes 3 must be preserved bit-exactly
            check(a, b, out, modeNames[mode]);
        }

    // Exhaustive single-element sweeps: one operand slot takes each class value, the rest ordinary.
    for (int slot = 0; slot < 32; ++slot)
        for (int cls = 0; cls < 12; ++cls)
            for (int rep = 0; rep < 2000; ++rep)
            {
                Matrix a = randomMatrix(rng, 0), b = randomMatrix(rng, 0);
                (slot < 16 ? a[slot] : b[slot - 16]) = poolValue(rng, cls);
                check(a, b, randomMatrix(rng, 3), "single-slot");
            }

    // Fixed edge matrices.
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, fromBits(1u), fromBits(0x007FFFFFu), fromBits(0x00800000u),
                              fromBits(0x27800000u), fromBits(0x277FFFFFu), fromBits(0x4F800000u), fromBits(0x4F800001u),
                              fromBits(0x7F7FFFFFu), fromBits(0x7F800000u), fromBits(0xFF800000u), fromBits(0x7FC00000u),
                              fromBits(0xFFC00001u), 1e-30f, 1e30f};
    const int n = sizeof(specials) / sizeof(specials[0]);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            Matrix a, b;
            a.fill(specials[i]);
            b.fill(specials[j]);
            check(a, b, randomMatrix(rng, 3), "uniform-special");
            Matrix id{};
            id[0] = id[5] = id[10] = id[15] = 1.0f;
            Matrix c = id;
            c[12] = specials[i];
            c[3] = specials[j];
            check(c, id, a, "identity-special");
            check(id, c, a, "identity-special2");
        }

    std::printf("cases=%llu ok-true=%llu ok-false=%llu fast-path=%llu failures=%llu\n",
                (unsigned long long)g_cases, (unsigned long long)g_trueCount, (unsigned long long)g_falseCount,
                (unsigned long long)g_fastCount, (unsigned long long)g_failures);
    std::printf("palette cases=%llu failures=%llu\n", (unsigned long long)g_paletteCases,
                (unsigned long long)g_paletteFailures);
    g_failures += g_paletteFailures;
    std::printf(g_failures == 0 ? "PASS\n" : "FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
