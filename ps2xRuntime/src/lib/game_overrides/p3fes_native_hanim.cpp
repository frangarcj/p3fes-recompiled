#include "p3fes_native_hanim.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <unordered_map>

namespace p3fes_native_hanim
{
namespace
{
constexpr uint32_t kInterpolateAddress = 0x004F7560u;
using GuestFunction = PS2Runtime::RecompiledFunction;
std::unordered_map<uint32_t, GuestFunction> g_originals;
uint64_t g_native = 0u;
uint64_t g_guestFallbacks = 0u;
uint64_t g_installFailures = 0u;

constexpr unsigned kSlerpTermBits = 8u;
struct SlerpTerm
{
    uint32_t dotBits = 0u;
    float angle = 0.0f;
    float reciprocalSin = 0.0f;
    bool valid = false;
};
SlerpTerm g_slerpTerms[1u << kSlerpTermBits];

float unpackCompressed16(uint16_t h)
{
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16u;
    const uint32_t magnitude = h & 0x7FFFu;
    if (magnitude == 0u)
    {
        float zero;
        const uint32_t bits = sign;
        std::memcpy(&zero, &bits, sizeof(zero));
        return zero;
    }

    const uint32_t exponent = magnitude & 0x7800u;
    const uint32_t fraction = magnitude & 0x07FFu;
    const uint32_t ieee = sign | 0x38000000u | (exponent << 12u) |
                          (fraction << 12u);
    float result;
    std::memcpy(&result, &ieee, sizeof(result));
    return result;
}

uint16_t read16(const uint8_t *rdram, uint32_t address)
{
    return Ps2FastRead16(rdram, address);
}

float readFloat(const uint8_t *rdram, uint32_t address)
{
    const uint32_t bits = Ps2FastRead32(rdram, address);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeFloat(uint8_t *rdram, uint32_t address, float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    Ps2FastWrite32(rdram, address, bits);
}

bool finite(float x) { return std::isfinite(x); }

bool canHandle(const uint8_t *rdram, const R5900Context *ctx)
{
    if (!rdram || !ctx)
        return false;
    const uint32_t out = getRegU32(ctx, 4);
    const uint32_t key0 = getRegU32(ctx, 5);
    const uint32_t key1 = getRegU32(ctx, 6);
    const uint32_t transform = getRegU32(ctx, 7);
    const auto inRange = [](uint32_t address, uint32_t size) {
        return address != 0u && address <= PS2_RAM_SIZE - size;
    };
    if (!inRange(out, 36u) || !inRange(key0, 22u) || !inRange(key1, 22u) ||
        !inRange(transform, 24u))
        return false;
    const float t = ctx->f[12];
    const float t0 = readFloat(rdram, key0 + 4u);
    const float t1 = readFloat(rdram, key1 + 4u);
    if (!finite(t) || !finite(t0) || !finite(t1) || t1 == t0)
        return false;
    for (uint32_t i = 0; i < 6u; ++i)
        if (!finite(readFloat(rdram, transform + i * 4u)))
            return false;
    return true;
}

void originalHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (!ctx)
        return;
    if (!interpolate(rdram, ctx))
    {
        ++g_guestFallbacks;
        const auto found = g_originals.find(kInterpolateAddress);
        if (found != g_originals.end() && found->second)
            found->second(rdram, ctx, runtime);
    }
}
}

bool interpolate(uint8_t *rdram, R5900Context *ctx)
{
    if (!canHandle(rdram, ctx))
        return false;

    const uint32_t out = getRegU32(ctx, 4);
    const uint32_t key0 = getRegU32(ctx, 5);
    const uint32_t key1 = getRegU32(ctx, 6);
    const uint32_t transform = getRegU32(ctx, 7);
    const float t0 = readFloat(rdram, key0 + 4u);
    const float t1 = readFloat(rdram, key1 + 4u);
    const float t = ctx->f[12];
    const float u = (t - t0) / (t1 - t0);

    float q0[4], q1[4];
    float p0[3], p1[3];
    for (uint32_t i = 0; i < 4u; ++i)
    {
        q0[i] = unpackCompressed16(read16(rdram, key0 + 8u + i * 2u));
        q1[i] = unpackCompressed16(read16(rdram, key1 + 8u + i * 2u));
    }
    for (uint32_t i = 0; i < 3u; ++i)
    {
        p0[i] = unpackCompressed16(read16(rdram, key0 + 16u + i * 2u));
        p1[i] = unpackCompressed16(read16(rdram, key1 + 16u + i * 2u));
    }

    float dot = 0.0f;
    for (uint32_t i = 0; i < 4u; ++i)
        dot += q0[i] * q1[i];
    if (dot < 0.0f)
    {
        dot = -dot;
        for (float &v : q1)
            v = -v;
    }
    if (!finite(dot) || dot < 0.0f || !finite(u))
        return false;

    float weight0, weight1;
    if (dot >= 0.999f)
    {
        // The guest routine has a dedicated near-parallel linear branch.
        weight0 = 1.0f - u;
        weight1 = u;
    }
    else
    {
        // angle and 1/sin(angle) are pure functions of dot, and the same key pair is interpolated
        // for many consecutive frames, so memoise them (exactly: bit-identical to recomputing).
        uint32_t dotBits;
        std::memcpy(&dotBits, &dot, sizeof(dotBits));
        SlerpTerm &term = g_slerpTerms[(dotBits * 0x9E3779B1u) >> (32u - kSlerpTermBits)];
        if (!term.valid || term.dotBits != dotBits)
        {
            term.dotBits = dotBits;
            term.angle = std::acos(dot);
            term.reciprocalSin = 1.0f / std::sin(term.angle);
            term.valid = true;
        }
        const float angle = term.angle;
        const float reciprocalSin = term.reciprocalSin;
        weight0 = std::sin((1.0f - u) * angle) * reciprocalSin;
        weight1 = std::sin(u * angle) * reciprocalSin;
    }
    std::array<float, 7> results{};
    for (uint32_t i = 0; i < 4u; ++i)
    {
        results[i] = q0[i] * weight0 + q1[i] * weight1;
        if (!finite(results[i]))
            return false;
    }

    for (uint32_t i = 0; i < 3u; ++i)
    {
        const float scaled = (p0[i] + (p1[i] - p0[i]) * u) *
                             readFloat(rdram, transform + 12u + i * 4u);
        results[4u + i] = scaled + readFloat(rdram, transform + i * 4u);
        if (!finite(results[4u + i]))
            return false;
    }

    for (uint32_t i = 0; i < 4u; ++i)
        writeFloat(rdram, out + 8u + i * 4u, results[i]);
    for (uint32_t i = 0; i < 3u; ++i)
        writeFloat(rdram, out + 24u + i * 4u, results[4u + i]);

    ctx->pc = getRegU32(ctx, 31);
    ++g_native;
    return true;
}

bool install(PS2Runtime &runtime, bool enabled)
{
    if (!enabled)
        return false;
    if (!runtime.hasFunction(kInterpolateAddress))
    {
        ++g_installFailures;
        return false;
    }
    const GuestFunction current = runtime.lookupFunction(kInterpolateAddress);
    if (!current)
    {
        ++g_installFailures;
        return false;
    }
    if (current == &originalHook)
        return g_originals.find(kInterpolateAddress) != g_originals.end();
    g_originals[kInterpolateAddress] = current;
    if (!runtime.replaceFunction(kInterpolateAddress, &originalHook))
    {
        ++g_installFailures;
        return false;
    }
    return true;
}

Counters counters()
{
    return {g_native, g_guestFallbacks, g_installFailures};
}

void reset()
{
    g_native = g_guestFallbacks = g_installFailures = 0u;
}

void resetCounters() { reset(); }

void logCounters()
{
    std::fprintf(stderr, "[p3fes-native-hanim] native=%llu guestFallbacks=%llu installFailures=%llu\n",
                 static_cast<unsigned long long>(g_native),
                 static_cast<unsigned long long>(g_guestFallbacks),
                 static_cast<unsigned long long>(g_installFailures));
}
}
