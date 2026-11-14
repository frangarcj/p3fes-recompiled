#include "game_overrides/p3fes_native_math.h"
#include "ps2_runtime.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x0052E000u;
    constexpr uint32_t kTableEnd = 0x0052F000u;
    constexpr uint32_t kAtanAddress = 0x0052E408u;
    constexpr uint32_t kSinAddress = 0x0052E878u;
    constexpr uint32_t kCosAddress = 0x0052E6D8u;
    uint32_t g_fallbackCalls[3]{};

    void atanFallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_fallbackCalls[0];
        ctx->f[0] = 101.0f;
        ctx->f[7] = 707.0f;
        ctx->pc = getRegU32(ctx, 31);
    }

    void sinFallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_fallbackCalls[1];
        ctx->f[0] = 102.0f;
        ctx->f[7] = 707.0f;
        ctx->pc = getRegU32(ctx, 31);
    }

    void cosFallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_fallbackCalls[2];
        ctx->f[0] = 103.0f;
        ctx->f[7] = 707.0f;
        ctx->pc = getRegU32(ctx, 31);
    }

    void assertNear(float actual, float expected, float tolerance = 1e-6f)
    {
        assert(std::fabs(actual - expected) <= tolerance);
    }
}

int main()
{
    std::vector<PS2Runtime::RecompiledFunction> table((kTableEnd - kTableBase) / 4u, nullptr);
    table[(kAtanAddress - kTableBase) / 4u] = &atanFallback;
    table[(kSinAddress - kTableBase) / 4u] = &sinFallback;
    table[(kCosAddress - kTableBase) / 4u] = &cosFallback;
    ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableEnd,
                                   static_cast<uint32_t>(table.size())});

    PS2Runtime runtime;
    assert(p3fes_native_math::install(runtime));
    assert(p3fes_native_math::install(runtime)); // Reinstall is safe.
    const auto atan = runtime.lookupFunction(kAtanAddress);
    const auto sin = runtime.lookupFunction(kSinAddress);
    const auto cos = runtime.lookupFunction(kCosAddress);
    assert(atan && sin && cos);

    uint8_t rdram[1]{};
    R5900Context ctx;
    ctx.r[31] = _mm_set1_epi32(0x12345678u);
    ctx.f[1] = 11.0f;
    ctx.f[7] = 77.0f;
    ctx.f[13] = 13.0f;
    ctx.fcr31 = 0xABCDEF01u;

    ctx.f[12] = 0.0f;
    atan(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 0.0f);
    assert(ctx.pc == 0x12345678u);
    assert(ctx.f[1] == 11.0f && ctx.f[7] == 77.0f && ctx.f[13] == 13.0f);
    assert(ctx.fcr31 == 0xABCDEF01u);

    ctx.f[12] = 1.0f;
    atan(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 0.7853981852531433f);
    ctx.f[12] = -0.0f;
    sin(rdram, &ctx, &runtime);
    assert(ctx.f[0] == 0.0f && std::signbit(ctx.f[0]));
    ctx.f[12] = 0.0f;
    cos(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 1.0f);
    ctx.f[12] = 1.5707963705062866f;
    sin(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 1.0f);
    ctx.f[12] = 1.5707963705062866f;
    cos(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 0.0f);
    ctx.f[12] = 8192.0f;
    atan(rdram, &ctx, &runtime);
    assertNear(ctx.f[0], 1.5706748962402344f, 2e-6f);

    const float specialInputs[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::denorm_min(),
        8192.5f,
    };
    for (float input : specialInputs)
    {
        ctx.f[12] = input;
        atan(rdram, &ctx, &runtime);
        assert(ctx.f[0] == 101.0f && ctx.f[7] == 707.0f);
        assert(ctx.pc == 0x12345678u);
    }

    assert(g_fallbackCalls[0] == 4u && g_fallbackCalls[1] == 0u && g_fallbackCalls[2] == 0u);
    const auto stats = p3fes_native_math::counters();
    assert(stats.nativeAtan == 3u && stats.nativeSin == 2u && stats.nativeCos == 2u);
    assert(stats.guestFallbacks == 4u && stats.installFailures == 0u);
}
