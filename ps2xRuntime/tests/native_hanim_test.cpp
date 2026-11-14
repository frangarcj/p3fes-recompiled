#include "game_overrides/p3fes_native_hanim.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cfloat>
#include <random>
#include <vector>

void RpHAnimCompressedKeyFrameInterpolate_0x4f7560(uint8_t *, R5900Context *, PS2Runtime *);

namespace
{
constexpr uint32_t kAddress = 0x004F7560u;
constexpr uint32_t kTableBase = 0x004F7000u;
constexpr uint32_t kTableEnd = 0x004F8000u;
constexpr uint32_t kOutput = 0x1000u;
constexpr uint32_t kKey0 = 0x2000u;
constexpr uint32_t kKey1 = 0x2040u;
constexpr uint32_t kTransform = 0x2100u;
constexpr uint32_t kStack = 0x3000u;

float decode(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t mag = h & 0x7FFFu;
    if (!mag)
    {
        float f;
        std::memcpy(&f, &sign, 4);
        return f;
    }
    const uint32_t e = mag & 0x7800u;
    const uint32_t bits = sign | 0x38000000u | (e << 12u) |
                          ((mag & 0x07FFu) << 12u);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t packNearest(float x)
{
    uint16_t best = 0;
    float bestError = INFINITY;
    for (uint32_t h = 0; h < 65536u; ++h)
    {
        const float candidate = decode(static_cast<uint16_t>(h));
        const float error = std::fabs(candidate - x);
        if (error < bestError)
        {
            bestError = error;
            best = static_cast<uint16_t>(h);
        }
    }
    return best;
}

void setFloat(uint8_t *memory, uint32_t address, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    Ps2FastWrite32(memory, address, bits);
}

void setHalf(uint8_t *memory, uint32_t address, float value)
{
    Ps2FastWrite16(memory, address, packNearest(value));
}

void initializeMemory(uint8_t *memory, const std::array<float, 4> &q0,
                     const std::array<float, 4> &q1,
                     const std::array<float, 3> &p0,
                     const std::array<float, 3> &p1)
{
    std::fill(memory, memory + PS2_RAM_SIZE, 0u);
    setFloat(memory, kKey0 + 4, 0.0f);
    setFloat(memory, kKey1 + 4, 1.0f);
    for (uint32_t i = 0; i < 4; ++i)
    {
        setHalf(memory, kKey0 + 8 + i * 2, q0[i]);
        setHalf(memory, kKey1 + 8 + i * 2, q1[i]);
    }
    for (uint32_t i = 0; i < 3; ++i)
    {
        setHalf(memory, kKey0 + 16 + i * 2, p0[i]);
        setHalf(memory, kKey1 + 16 + i * 2, p1[i]);
        setFloat(memory, kTransform + i * 4, 1.25f + 0.5f * i);
        setFloat(memory, kTransform + 12 + i * 4, 0.75f + 0.25f * i);
    }
}

R5900Context makeContext(float time)
{
    R5900Context ctx{};
    ctx.r[4] = _mm_set1_epi32(kOutput);
    ctx.r[5] = _mm_set1_epi32(kKey0);
    ctx.r[6] = _mm_set1_epi32(kKey1);
    ctx.r[7] = _mm_set1_epi32(kTransform);
    ctx.r[29] = _mm_set1_epi32(kStack + 0x80u);
    ctx.r[2] = _mm_set1_epi32(0x76543210u);
    ctx.r[31] = _mm_set1_epi32(0x12345678u);
    ctx.f[12] = time;
    ctx.f[20] = 20.25f;
    ctx.f[21] = -21.5f;
    ctx.f[22] = 22.75f;
    ctx.pc = kAddress;
    return ctx;
}

float getFloat(const uint8_t *memory, uint32_t address)
{
    const uint32_t bits = Ps2FastRead32(memory, address);
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

void runCase(PS2Runtime &runtime, const std::array<float, 4> &q0,
             const std::array<float, 4> &q1, float time)
{
    std::vector<uint8_t> guest(PS2_RAM_SIZE), native(PS2_RAM_SIZE);
    const std::array<float, 3> p0{2.25f, -3.5f, 1.125f};
    const std::array<float, 3> p1{-4.0f, 7.25f, 9.5f};
    initializeMemory(guest.data(), q0, q1, p0, p1);
    native = guest;
    R5900Context guestCtx = makeContext(time);
    R5900Context nativeCtx = guestCtx;

    RpHAnimCompressedKeyFrameInterpolate_0x4f7560(guest.data(), &guestCtx, &runtime);
    auto replacement = runtime.lookupFunction(kAddress);
    assert(replacement);
    const auto before = p3fes_native_hanim::counters();
    replacement(native.data(), &nativeCtx, &runtime);
    const auto after = p3fes_native_hanim::counters();
    assert(after.native == before.native + 1u && after.guestFallbacks == before.guestFallbacks);

    // The game uses the resulting quaternion/translation directly. 3e-5 is
    // far below one packed-key quantization step, while allowing std libm and
    // the EE polynomial approximation a small arithmetic-order difference.
    for (uint32_t i = 0; i < 7; ++i)
    {
        const uint32_t offset = i < 4 ? 8u + i * 4u : 24u + (i - 4u) * 4u;
        const float expected = getFloat(guest.data(), kOutput + offset);
        const float actual = getFloat(native.data(), kOutput + offset);
        assert(std::isfinite(expected) && std::isfinite(actual));
        if (!(std::fabs(actual - expected) <= 3e-5f * std::max(1.0f, std::fabs(expected))))
        {
            std::fprintf(stderr, "time=%g offset=%u expected=%g actual=%g diff=%g\\n",
                         time, offset, expected, actual, actual - expected);
            assert(false);
        }
    }
    assert(nativeCtx.pc == 0x12345678u);
    assert(getRegU32(&nativeCtx, 2) == 0x76543210u);
    assert(nativeCtx.f[20] == 20.25f && nativeCtx.f[21] == -21.5f && nativeCtx.f[22] == 22.75f);
}

void runDegenerateInterval(PS2Runtime &runtime)
{
    std::vector<uint8_t> memory(PS2_RAM_SIZE);
    initializeMemory(memory.data(), {0.0f, 0.0f, 0.0f, 1.0f},
                     {0.0f, 0.0f, 0.2f, 0.98f}, {0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f});
    setFloat(memory.data(), kKey1 + 4, 0.0f);
    R5900Context ctx = makeContext(0.0f);
    const auto replacement = runtime.lookupFunction(kAddress);
    replacement(memory.data(), &ctx, &runtime);
    assert(ctx.pc == 0x12345678u);
    assert(getRegU32(&ctx, 2) == 0x76543210u);
}

void runOverflowScaleFallback(PS2Runtime &runtime)
{
    std::vector<uint8_t> guest(PS2_RAM_SIZE), native(PS2_RAM_SIZE);
    initializeMemory(guest.data(), {0.0f, 0.0f, 0.0f, 1.0f},
                     {0.0f, 0.0f, 0.3f, 0.95f}, {2.25f, -3.5f, 1.125f}, {-4.0f, 7.25f, 9.5f});
    setFloat(guest.data(), kTransform + 12, FLT_MAX);
    native = guest;
    R5900Context guestCtx = makeContext(0.0f);
    R5900Context nativeCtx = guestCtx;
    RpHAnimCompressedKeyFrameInterpolate_0x4f7560(guest.data(), &guestCtx, &runtime);
    runtime.lookupFunction(kAddress)(native.data(), &nativeCtx, &runtime);
    assert(!std::isfinite(getFloat(guest.data(), kOutput + 24)));
    for (uint32_t i = 0; i < 7; ++i)
    {
        const uint32_t offset = i < 4 ? 8u + i * 4u : 24u + (i - 4u) * 4u;
        assert(Ps2FastRead32(guest.data(), kOutput + offset) ==
               Ps2FastRead32(native.data(), kOutput + offset));
    }
    assert(nativeCtx.pc == guestCtx.pc && nativeCtx.pc == 0x12345678u);
}
}

int main()
{
    std::vector<PS2Runtime::RecompiledFunction> table((kTableEnd - kTableBase) / 4u, nullptr);
    table[(kAddress - kTableBase) / 4u] = &RpHAnimCompressedKeyFrameInterpolate_0x4f7560;
    ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableEnd,
                                   static_cast<uint32_t>(table.size())});
    PS2Runtime runtime;
    assert(p3fes_native_hanim::install(runtime));
    assert(p3fes_native_hanim::install(runtime));

    const std::array<float, 4> identity{0.0f, 0.0f, 0.0f, 1.0f};
    const float angle = 0.65f;
    const std::array<float, 4> turnZ{0.0f, 0.0f, std::sin(angle * 0.5f), std::cos(angle * 0.5f)};
    runCase(runtime, identity, turnZ, 0.0f);   // start endpoint
    runCase(runtime, identity, turnZ, 0.5f);   // midpoint
    runCase(runtime, identity, turnZ, 0.5001f); // near midpoint
    runCase(runtime, identity, turnZ, 1.0f);   // end endpoint
    runCase(runtime, identity, turnZ, -0.2f);  // extrapolate before the key interval
    runCase(runtime, identity, turnZ, 1.2f);   // extrapolate past the key interval
    const float smallAngle = 0.001f;
    const std::array<float, 4> tinyTurn{0.0f, 0.0f, std::sin(smallAngle * 0.5f), std::cos(smallAngle * 0.5f)};
    runCase(runtime, identity, tinyTurn, 0.5f); // near-parallel linear branch
    const float broadAngle = 2.2f;
    const std::array<float, 4> broadTurn{0.0f, 0.0f, std::sin(broadAngle * 0.5f), std::cos(broadAngle * 0.5f)};
    runCase(runtime, identity, broadTurn, 0.5f); // low-dot polynomial branch
    const std::array<float, 4> opposite{-turnZ[0], -turnZ[1], -turnZ[2], -turnZ[3]};
    runCase(runtime, identity, opposite, 0.5f); // negative dot, same rotation hemisphere
    const std::array<float, 4> oblique0{0.2f, 0.3f, 0.1f, std::sqrt(0.86f)};
    const std::array<float, 4> oblique1{-0.15f, 0.4f, 0.25f, std::sqrt(0.755f)};
    runCase(runtime, oblique0, oblique1, 0.35f); // non-axis-aligned compressed quaternions

    // The compressed mantissa step near 1.0 is about 1/2048; choose a target
    // above the half-step so packing does not round back to exactly 1.0.
    const float slightlyNonUnit = decode(packNearest(std::sqrt(1.001f)));
    assert(slightlyNonUnit * slightlyNonUnit > 1.0001f);
    const std::array<float, 4> dotOverGuard{0.0f, 0.0f, 0.0f, slightlyNonUnit};
    runCase(runtime, dotOverGuard, dotOverGuard, 0.4f); // packed self-dot > 1.0001
    const float dot101Scalar = std::sqrt(1.01f);
    const std::array<float, 4> dot101{0.0f, 0.0f, 0.0f, dot101Scalar};
    runCase(runtime, dot101, dot101, 0.4f); // self-dot approximately 1.01
    const float dot150Scalar = std::sqrt(1.5f);
    const std::array<float, 4> dot150{0.0f, 0.0f, 0.0f, dot150Scalar};
    runCase(runtime, dot150, dot150, 0.4f); // self-dot approximately 1.5

    for (unsigned i = 1; i <= 16; ++i)
    {
        const float sampleAngle = 0.18f * static_cast<float>(i);
        const std::array<float, 4> sampleTurn{
            0.0f, 0.0f, std::sin(sampleAngle * 0.5f), std::cos(sampleAngle * 0.5f)};
        runCase(runtime, identity, sampleTurn, 0.37f);
    }

    std::mt19937 random(0x4F7560u);
    std::uniform_real_distribution<float> component(-1.0f, 1.0f);
    for (unsigned sample = 0; sample < 100u; ++sample)
    {
        std::array<float, 4> q0{}, q1{};
        for (float &value : q0)
            value = component(random);
        for (float &value : q1)
            value = component(random);
        const auto normalize = [](std::array<float, 4> &q) {
            float lengthSquared = 0.0f;
            for (float value : q)
                lengthSquared += value * value;
            const float inverseLength = 1.0f / std::sqrt(lengthSquared);
            for (float &value : q)
                value *= inverseLength;
        };
        normalize(q0);
        normalize(q1);
        runCase(runtime, q0, q1, component(random));
    }

    runDegenerateInterval(runtime); // equal timestamps use the original guest routine
    runOverflowScaleFallback(runtime);
    const auto stats = p3fes_native_hanim::counters();
    assert(stats.native == 129u && stats.guestFallbacks == 2u && stats.installFailures == 0u);
    p3fes_native_hanim::resetCounters();
    const auto cleared = p3fes_native_hanim::counters();
    assert(cleared.native == 0u && cleared.guestFallbacks == 0u && cleared.installFailures == 0u);
}
