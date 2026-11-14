#include "p3fes_native_rw_math.h"
#include "p3fes_rw_matrix_math.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace p3fes_native_rw_math
{
    namespace
    {
        constexpr uint32_t kMultiply = 0x004C2F30u;
        constexpr uint32_t kMultPoints = 0x004E3F20u;
        PS2Runtime::RecompiledFunction g_original = nullptr;
        PS2Runtime::RecompiledFunction g_originalPoints = nullptr;
        uint64_t g_native = 0u, g_fallback = 0u;
        uint64_t g_pointsNative = 0u, g_pointsFallback = 0u;

        bool matrixRange(uint32_t address)
        {
            return address != 0u && (address & 15u) == 0u && address <= PS2_RAM_SIZE - 64u;
        }

        bool ramRange(uint32_t address, uint64_t bytes)
        {
            return address != 0u && bytes <= PS2_RAM_SIZE &&
                   static_cast<uint64_t>(address) + bytes <= PS2_RAM_SIZE;
        }

        uint32_t loadWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        }

        __m128 pointVector(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t x = loadWord(rdram, address);
            const uint32_t y = loadWord(rdram, address + 4u);
            const uint32_t z = loadWord(rdram, address + 8u);
            return _mm_castsi128_ps(_mm_set_epi32(0, static_cast<int>(z),
                                                   static_cast<int>(y), static_cast<int>(x)));
        }

        bool vectorNormalOrZero(__m128 value, unsigned lanes)
        {
            alignas(16) float lane[4];
            _mm_store_ps(lane, value);
            for (unsigned i = 0; i < lanes; ++i)
                if (!p3fes_rw_matrix_math::normalOrZero(lane[i]))
                    return false;
            return true;
        }

        void transformPoint(R5900Context *ctx, __m128 point)
        {
            const __m128 xyz = _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1));
            const __m128 x = _mm_shuffle_ps(point, point, _MM_SHUFFLE(0, 0, 0, 0));
            const __m128 y = _mm_shuffle_ps(point, point, _MM_SHUFFLE(1, 1, 1, 1));
            const __m128 z = _mm_shuffle_ps(point, point, _MM_SHUFFLE(2, 2, 2, 2));
            __m128 acc = PS2_VMUL(ctx->vu0_vf[1], x);
            acc = _mm_blendv_ps(ctx->vu0_acc, acc, xyz);
            __m128 mul = PS2_VMUL(ctx->vu0_vf[2], y);
            __m128 sum = PS2_VADD(acc, mul);
            acc = _mm_blendv_ps(acc, sum, xyz);
            mul = PS2_VMUL(ctx->vu0_vf[3], z);
            sum = PS2_VADD(acc, mul);
            acc = _mm_blendv_ps(acc, sum, xyz);
            ctx->vu0_acc = acc;
            mul = PS2_VMUL(ctx->vu0_vf[4], _mm_set1_ps(_mm_cvtss_f32(
                _mm_shuffle_ps(ctx->vu0_vf[0], ctx->vu0_vf[0], _MM_SHUFFLE(3, 3, 3, 3)))));
            sum = PS2_VADD(ctx->vu0_acc, mul);
            const __m128 result = _mm_blendv_ps(ctx->vu0_vf[6], sum, xyz);
            PS2_VU0_VF_WRITE(ctx, 6, result);
        }

        void pointsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t outAddress = getRegU32(ctx, 4);
            const uint32_t sourceAddress = getRegU32(ctx, 5);
            const int64_t signedCount = GPR_S64(ctx, 6);
            const uint32_t matrixAddress = getRegU32(ctx, 7);
            bool eligible = rdram && runtime && ctx->pc == kMultPoints && signedCount > 0 &&
                signedCount <= INT32_MAX && (GPR_U64(ctx, 6) ==
                    static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(signedCount)))) &&
                GPR_U64(ctx, 0) == 0u && matrixRange(matrixAddress);
            const uint64_t count = eligible ? static_cast<uint64_t>(signedCount) : 0u;
            const uint64_t sourceBytes = (count + 1u) * 12u;
            const uint64_t outputBytes = count * 12u;
            eligible = eligible && ramRange(sourceAddress, sourceBytes) &&
                ramRange(outAddress, outputBytes) && (sourceAddress & 3u) == 0u &&
                (outAddress & 3u) == 0u &&
                (outAddress == sourceAddress ||
                 static_cast<uint64_t>(outAddress) + outputBytes <= sourceAddress ||
                 static_cast<uint64_t>(sourceAddress) + sourceBytes <= outAddress);

            alignas(16) __m128 matrix[4];
            if (eligible)
            {
                for (unsigned i = 0; i < 4; ++i)
                    matrix[i] = _mm_castsi128_ps(_mm_load_si128(
                        reinterpret_cast<const __m128i *>(rdram + matrixAddress + i * 16u)));
                for (const __m128 value : matrix)
                    eligible = eligible && vectorNormalOrZero(value, 3u);
                eligible = eligible && p3fes_rw_matrix_math::normalOrZero(_mm_cvtss_f32(
                    _mm_shuffle_ps(ctx->vu0_vf[0], ctx->vu0_vf[0], _MM_SHUFFLE(3, 3, 3, 3))));
                for (uint64_t i = 0; eligible && i < count; ++i)
                    eligible = vectorNormalOrZero(pointVector(rdram,
                        sourceAddress + static_cast<uint32_t>(i * 12u)), 3u);
            }

            if (!eligible)
            {
                ++g_pointsFallback;
                if (g_originalPoints)
                    g_originalPoints(rdram, ctx, runtime);
                return;
            }

            ++g_pointsNative;
            SET_GPR_U64(ctx, 8, static_cast<uint64_t>(outAddress) + GPR_U64(ctx, 0));
            for (unsigned i = 0; i < 4; ++i)
                ctx->vu0_vf[i + 1u] = matrix[i];

            __m128 currentPoint = pointVector(rdram, sourceAddress);
            SET_GPR_ZE32(ctx, 13, loadWord(rdram, sourceAddress));
            SET_GPR_ZE32(ctx, 11, loadWord(rdram, sourceAddress + 4u));
            SET_GPR_ZE32(ctx, 12, loadWord(rdram, sourceAddress + 8u));
            SET_GPR_U64(ctx, 11, (GPR_U64(ctx, 11) << 32) | GPR_U64(ctx, 13));
            SET_GPR_VEC(ctx, 12, PS2_PCPYLD(GPR_VEC(ctx, 12), GPR_VEC(ctx, 11)));
            PS2_VU0_VF_WRITE(ctx, 5, _mm_castsi128_ps(GPR_VEC(ctx, 12)));

            uint32_t currentOut = outAddress;
            uint32_t currentSource = sourceAddress;
            for (uint64_t i = 0; i < count; ++i)
            {
                currentSource += 12u; // branch delay slot at 0x4e3f54
                SET_GPR_S32(ctx, 5, static_cast<int32_t>(currentSource));
                transformPoint(ctx, currentPoint);
                const __m128 nextPoint = pointVector(rdram, currentSource);
                SET_GPR_ZE32(ctx, 13, loadWord(rdram, currentSource));
                SET_GPR_ZE32(ctx, 11, loadWord(rdram, currentSource + 4u));
                SET_GPR_ZE32(ctx, 12, loadWord(rdram, currentSource + 8u));
                SET_GPR_S32(ctx, 6, static_cast<int32_t>(GPR_U32(ctx, 6) - 1u));
                SET_GPR_U64(ctx, 11, (GPR_U64(ctx, 11) << 32) | GPR_U64(ctx, 13));
                SET_GPR_VEC(ctx, 12, PS2_PCPYLD(GPR_VEC(ctx, 12), GPR_VEC(ctx, 11)));
                PS2_VU0_VF_WRITE(ctx, 5, _mm_castsi128_ps(GPR_VEC(ctx, 12)));
                SET_GPR_VEC(ctx, 25, _mm_castps_si128(ctx->vu0_vf[6]));
                SET_GPR_U64(ctx, 2, GPR_U64(ctx, 25) >> 32);
                SET_GPR_VEC(ctx, 3, _mm_unpackhi_epi64(GPR_VEC(ctx, 25), GPR_VEC(ctx, 25)));
                const uint32_t x = GPR_U32(ctx, 25);
                const uint32_t y = GPR_U32(ctx, 2);
                const uint32_t z = GPR_U32(ctx, 3);
                std::memcpy(rdram + currentOut, &x, 4u);
                std::memcpy(rdram + currentOut + 4u, &y, 4u);
                std::memcpy(rdram + currentOut + 8u, &z, 4u);
                currentOut += 12u; // branch delay slot at 0x4e3fa4
                SET_GPR_S32(ctx, 4, static_cast<int32_t>(currentOut));
                ctx->pc = 0x4E3F50u;
                ctx->in_delay_slot = false;
                ctx->branch_pc = 0x4E3FA0u;
                if (runtime->eeCheckpointDue())
                    return;
                currentPoint = nextPoint;
            }

            // Final taken blez branch executes its a1 += 12 delay slot.
            currentSource += 12u;
            SET_GPR_S32(ctx, 5, static_cast<int32_t>(currentSource));
            SET_GPR_S32(ctx, 4, static_cast<int32_t>(currentOut));
            SET_GPR_U64(ctx, 2, GPR_U64(ctx, 8) + GPR_U64(ctx, 0));
            ctx->pc = GPR_U32(ctx, 31);
            ctx->in_delay_slot = false;
            ctx->branch_pc = 0x4E3F50u;
        }

        void multiplyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t outAddress = getRegU32(ctx, 4);
            const uint32_t firstAddress = getRegU32(ctx, 5);
            const uint32_t secondAddress = getRegU32(ctx, 6);
            const float homogeneousOne = _mm_cvtss_f32(_mm_shuffle_ps(
                ctx->vu0_vf[0], ctx->vu0_vf[0], _MM_SHUFFLE(3, 3, 3, 3)));
            if (rdram && ctx->pc == kMultiply && matrixRange(outAddress) &&
                matrixRange(firstAddress) && matrixRange(secondAddress) && homogeneousOne == 1.0f)
            {
                p3fes_rw_matrix_math::Matrix first, second, result{};
                std::memcpy(first.data(), rdram + firstAddress, 64u);
                std::memcpy(second.data(), rdram + secondAddress, 64u);
                if (p3fes_rw_matrix_math::multiply(first, second, result))
                {
                    // The original VU routine stores these unused fourth lanes
                    // from its volatile registers; preserve their memory bytes.
                    for (unsigned column = 1; column < 4; ++column)
                    {
                        alignas(16) float value[4];
                        _mm_storeu_ps(value, ctx->vu0_vf[9 + column]);
                        result[column * 4 + 3] = value[3];
                    }
                    uint32_t firstFlags, secondFlags;
                    std::memcpy(&firstFlags, first.data() + 3, 4u);
                    std::memcpy(&secondFlags, second.data() + 3, 4u);
                    const uint32_t flags = firstFlags & secondFlags;
                    std::memcpy(result.data() + 3, &flags, 4u);
                    std::memcpy(rdram + outAddress, result.data(), 64u);
                    ctx->r[2] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[2]),
                                                          _mm_castsi128_pd(ctx->r[4])));
                    ctx->pc = getRegU32(ctx, 31);
                    ++g_native;
                    return;
                }
            }
            ++g_fallback;
            if (g_original)
                g_original(rdram, ctx, runtime);
        }
    }

    bool install(PS2Runtime &runtime, bool enabled)
    {
        if (!enabled)
            return false;
        bool installed = false;
        if (runtime.hasFunction(kMultiply))
        {
            const auto current = runtime.lookupFunction(kMultiply);
            if (current == &multiplyHook)
                installed = g_original != nullptr;
            else
            {
                g_original = current;
                installed = runtime.replaceFunction(kMultiply, &multiplyHook);
            }
        }
        // Keep the new vector kernel opt-in until its differential and frame
        // validation is complete. The existing multiply path stays unchanged.
        const char *pointsEnabled = std::getenv("P3FES_NATIVE_RW_VECTOR");
        if (pointsEnabled && *pointsEnabled == '1' && runtime.hasFunction(kMultPoints))
        {
            const auto current = runtime.lookupFunction(kMultPoints);
            if (current == &pointsHook)
                installed = (g_originalPoints != nullptr) || installed;
            else
            {
                g_originalPoints = current;
                installed = runtime.replaceFunction(kMultPoints, &pointsHook) || installed;
            }
        }
        return installed;
    }

    void resetCounters() { g_native = g_fallback = g_pointsNative = g_pointsFallback = 0u; }

    void pointsCounters(uint64_t &native, uint64_t &fallback)
    {
        native = g_pointsNative;
        fallback = g_pointsFallback;
    }

    void logCounters()
    {
        std::fprintf(stderr, "[p3fes-native-rw-math] multiply=%llu fallback=%llu\n",
                     static_cast<unsigned long long>(g_native), static_cast<unsigned long long>(g_fallback));
        std::fprintf(stderr, "[p3fes-native-rw-math] points=%llu fallback=%llu\n",
                     static_cast<unsigned long long>(g_pointsNative),
                     static_cast<unsigned long long>(g_pointsFallback));
    }
}
