#pragma once

#include <array>
#include <cstdint>
#include <cstring>

#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
#include <arm_neon.h>
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define P3FES_RW_MATRIX_NO_FP_CONTRACT __attribute__((optimize("fp-contract=off")))
#else
#define P3FES_RW_MATRIX_NO_FP_CONTRACT
#endif

namespace p3fes_rw_matrix_math
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

    // Branch-free test that the 12 operand floats of each source (the xyz lanes of
    // its four columns) are all "ordinary" (see ordinary()). The fourth lanes are
    // flags/padding and are never inspected.
    P3FES_RW_MATRIX_NO_FP_CONTRACT inline bool allOrdinaryOperands(
        const float *first, const float *second)
    {
#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
        // Eight 4-lane loads; the padding lane of every column is forced to "ordinary".
        const uint32x4_t absMask = vdupq_n_u32(0x7FFFFFFFu);
        const uint32x4_t low = vdupq_n_u32(0x27800000u);
        const uint32x4_t span = vdupq_n_u32(0x28000000u);
        const uint32x4_t zero = vdupq_n_u32(0u);
        const uint32x4_t padLane = {0u, 0u, 0u, 0xFFFFFFFFu};
        uint32x4_t all = vdupq_n_u32(0xFFFFFFFFu);
        for (unsigned column = 0; column < 4; ++column)
        {
            const uint32x4_t a = vandq_u32(vreinterpretq_u32_f32(vld1q_f32(first + column * 4)), absMask);
            const uint32x4_t b = vandq_u32(vreinterpretq_u32_f32(vld1q_f32(second + column * 4)), absMask);
            const uint32x4_t okA = vorrq_u32(vceqq_u32(a, zero), vcleq_u32(vsubq_u32(a, low), span));
            const uint32x4_t okB = vorrq_u32(vceqq_u32(b, zero), vcleq_u32(vsubq_u32(b, low), span));
            all = vandq_u32(all, vorrq_u32(vandq_u32(okA, okB), padLane));
        }
        const uint32x2_t folded = vand_u32(vget_low_u32(all), vget_high_u32(all));
        return (vget_lane_u32(folded, 0) & vget_lane_u32(folded, 1)) == 0xFFFFFFFFu;
#else
        uint32_t ok = 1u;
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                uint32_t a, b;
                std::memcpy(&a, first + column * 4 + component, sizeof(a));
                std::memcpy(&b, second + column * 4 + component, sizeof(b));
                a &= 0x7FFFFFFFu;
                b &= 0x7FFFFFFFu;
                // bits == 0, or 0x27800000 <= bits <= 0x4F800000 (same set as ordinary()).
                ok &= static_cast<uint32_t>((a == 0u) | ((a - 0x27800000u) <= 0x28000000u)) &
                      static_cast<uint32_t>((b == 0u) | ((b - 0x27800000u) <= 0x28000000u));
            }
        return ok != 0u;
#endif
    }

    // Reference implementation with per-element operand classification. Used as the
    // slow path of multiply() for operands outside the ordinary range.
    // RwMatrixMultiply(out, first, second): column-vector equivalent second*first.
    // RW's fourth lanes are flags/padding, not homogeneous float coordinates.
    // Preserve those lanes in out; read both sources fully before any write.
    P3FES_RW_MATRIX_NO_FP_CONTRACT inline bool multiplyChecked(
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


    // Same contract and results as multiplyChecked(). The common case (every operand
    // ordinary) takes one branch-free pre-check and then the plain same-order
    // arithmetic with contraction off; anything else goes to multiplyChecked().
    // Lanes 3 of out are left untouched. All reads happen before any write.
    P3FES_RW_MATRIX_NO_FP_CONTRACT inline bool multiply(
        const Matrix &first, const Matrix &second, Matrix &out)
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
        if (!allOrdinaryOperands(first.data(), second.data()))
            return multiplyChecked(first, second, out);
#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
        // Lane-wise over (x,y,z,pad): the pad lane is computed but never stored.
        // vmul/vadd are not fused, so every lane rounds exactly like the scalar form.
        const float32x4_t s0 = vld1q_f32(second.data());
        const float32x4_t s1 = vld1q_f32(second.data() + 4);
        const float32x4_t s2 = vld1q_f32(second.data() + 8);
        const float32x4_t s3 = vld1q_f32(second.data() + 12);
        const uint32x4_t keepPad = {0u, 0u, 0u, 0xFFFFFFFFu};
        float32x4_t col[4];
        for (unsigned column = 0; column < 4; ++column)
        {
            const float *f = first.data() + column * 4;
            float32x4_t xy = vaddq_f32(vmulq_n_f32(s0, f[0]), vmulq_n_f32(s1, f[1]));
            float32x4_t xyz = vaddq_f32(xy, vmulq_n_f32(s2, f[2]));
            col[column] = column == 3 ? vaddq_f32(xyz, s3) : xyz;
        }
        float *o = out.data();
        for (unsigned column = 0; column < 4; ++column)
            vst1q_f32(o + column * 4, vbslq_f32(keepPad, vld1q_f32(o + column * 4), col[column]));
        return true;
#else
        float result[12];
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                const float x = second[component] * first[column * 4];
                const float y = second[4 + component] * first[column * 4 + 1];
                const float z = second[8 + component] * first[column * 4 + 2];
                const float xy = x + y;
                const float xyz = xy + z;
                result[column * 3 + component] = column == 3 ? xyz + second[12 + component] : xyz;
            }
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
                out[column * 4 + component] = result[column * 3 + component];
        return true;
#endif
    }


    // True when every one of the 12 xyz-lane floats of a matrix (four columns of
    // x,y,z; the fourth lane of each column is padding) is finite. Branch-free.
    inline bool allFiniteXyz(const float *m)
    {
#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
        const uint32x4_t exponent = vdupq_n_u32(0x7F800000u);
        const uint32x4_t padLane = {0u, 0u, 0u, 0xFFFFFFFFu};
        uint32x4_t all = vdupq_n_u32(0xFFFFFFFFu);
        for (unsigned column = 0; column < 4; ++column)
        {
            const uint32x4_t bits = vreinterpretq_u32_f32(vld1q_f32(m + column * 4));
            const uint32x4_t notFinite = vceqq_u32(vandq_u32(bits, exponent), exponent);
            all = vandq_u32(all, vorrq_u32(vmvnq_u32(notFinite), padLane));
        }
        const uint32x2_t folded = vand_u32(vget_low_u32(all), vget_high_u32(all));
        return (vget_lane_u32(folded, 0) & vget_lane_u32(folded, 1)) == 0xFFFFFFFFu;
#else
        uint32_t bad = 0u;
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                uint32_t bits;
                std::memcpy(&bits, m + column * 4 + component, sizeof(bits));
                bad |= static_cast<uint32_t>((bits & 0x7F800000u) == 0x7F800000u);
            }
        return bad == 0u;
#endif
    }

    // Same set as normalOrZero() over the 12 xyz-lane floats of a matrix, branch-free.
    inline bool allNormalOrZeroXyz(const float *m)
    {
#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
        const uint32x4_t absMask = vdupq_n_u32(0x7FFFFFFFu);
        const uint32x4_t low = vdupq_n_u32(0x00800000u);
        const uint32x4_t span = vdupq_n_u32(0x7EFFFFFFu);
        const uint32x4_t zero = vdupq_n_u32(0u);
        const uint32x4_t padLane = {0u, 0u, 0u, 0xFFFFFFFFu};
        uint32x4_t all = vdupq_n_u32(0xFFFFFFFFu);
        for (unsigned column = 0; column < 4; ++column)
        {
            const uint32x4_t bits = vandq_u32(vreinterpretq_u32_f32(vld1q_f32(m + column * 4)), absMask);
            const uint32x4_t ok = vorrq_u32(vceqq_u32(bits, zero), vcleq_u32(vsubq_u32(bits, low), span));
            all = vandq_u32(all, vorrq_u32(ok, padLane));
        }
        const uint32x2_t folded = vand_u32(vget_low_u32(all), vget_high_u32(all));
        return (vget_lane_u32(folded, 0) & vget_lane_u32(folded, 1)) == 0xFFFFFFFFu;
#else
        uint32_t ok = 1u;
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned component = 0; component < 3; ++component)
            {
                uint32_t bits;
                std::memcpy(&bits, m + column * 4 + component, sizeof(bits));
                bits &= 0x7FFFFFFFu;
                ok &= static_cast<uint32_t>((bits == 0u) | ((bits - 0x00800000u) <= 0x7EFFFFFFu));
            }
        return ok != 0u;
#endif
    }

    // RwMatrixMultiply arithmetic (second*first, plain same-order mul/add, no FMA, no
    // operand classification) for callers that only feed a renderer. Writes all 16
    // floats of out: xyz lanes from the product, padding lanes as the identity's
    // (0,0,0 / 0,0,0 / 0,0,0 / 1). out must not overlap first or second.
    P3FES_RW_MATRIX_NO_FP_CONTRACT inline void multiplyPlainAffine(
        const float *first, const float *second, float *out)
    {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#if defined(__ARM_NEON) && !defined(P3FES_RW_MATRIX_FORCE_SCALAR)
        const float32x4_t s0 = vld1q_f32(second);
        const float32x4_t s1 = vld1q_f32(second + 4);
        const float32x4_t s2 = vld1q_f32(second + 8);
        const float32x4_t s3 = vld1q_f32(second + 12);
        for (unsigned column = 0; column < 4; ++column)
        {
            const float *f = first + column * 4;
            const float32x4_t xy = vaddq_f32(vmulq_n_f32(s0, f[0]), vmulq_n_f32(s1, f[1]));
            float32x4_t xyz = vaddq_f32(xy, vmulq_n_f32(s2, f[2]));
            if (column == 3)
                xyz = vaddq_f32(xyz, s3);
            vst1q_f32(out + column * 4, vsetq_lane_f32(column == 3 ? 1.0f : 0.0f, xyz, 3));
        }
#else
        for (unsigned column = 0; column < 4; ++column)
        {
            for (unsigned component = 0; component < 3; ++component)
            {
                const float x = second[component] * first[column * 4];
                const float y = second[4 + component] * first[column * 4 + 1];
                const float z = second[8 + component] * first[column * 4 + 2];
                const float xy = x + y;
                const float xyz = xy + z;
                out[column * 4 + component] = column == 3 ? xyz + second[12 + component] : xyz;
            }
            out[column * 4 + 3] = column == 3 ? 1.0f : 0.0f;
        }
#endif
    }

}

#undef P3FES_RW_MATRIX_NO_FP_CONTRACT
