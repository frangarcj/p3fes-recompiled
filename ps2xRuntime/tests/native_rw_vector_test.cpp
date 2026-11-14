#include "game_overrides/p3fes_native_rw_math.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

void vectorASMMultPoints_0x4e3f20(uint8_t *, R5900Context *, PS2Runtime *);

namespace
{
    constexpr uint32_t kEntry = 0x004E3F20u;
    constexpr uint32_t kReturn = 0x00123450u;
    constexpr uint32_t kSource = 0x00100000u;
    constexpr uint32_t kOutput = 0x00110000u;
    constexpr uint32_t kMatrix = 0x00120000u;
    using Point = std::array<float, 3>;
    using Matrix = std::array<float, 16>;

    void setArg(R5900Context &ctx, unsigned reg, uint64_t value)
    {
        SET_GPR_U64(&ctx, reg, value);
    }

    R5900Context makeContext(uint32_t output, uint32_t source, int32_t count)
    {
        R5900Context ctx;
        ctx.pc = kEntry;
        setArg(ctx, 4, output);
        setArg(ctx, 5, source);
        setArg(ctx, 6, static_cast<uint64_t>(static_cast<int64_t>(count)));
        setArg(ctx, 7, kMatrix);
        setArg(ctx, 31, kReturn);
        // Give preserved W lanes and ACC state nontrivial values.
        ctx.vu0_acc = _mm_setr_ps(31.0f, -7.0f, 5.0f, 19.0f);
        ctx.vu0_vf[6] = _mm_setr_ps(3.0f, 4.0f, 5.0f, -11.0f);
        return ctx;
    }

    bool compareCall(const std::vector<uint8_t> &initial, uint32_t output,
                     uint32_t source, int32_t count, const char *label)
    {
        std::vector<uint8_t> guestRam = initial;
        std::vector<uint8_t> nativeRam = initial;
        auto guestContext = makeContext(output, source, count);
        auto nativeContext = guestContext;
        PS2Runtime runtime;
        vectorASMMultPoints_0x4e3f20(guestRam.data(), &guestContext, &runtime);
        const auto hook = runtime.lookupFunction(kEntry);
        if (!hook || hook == &vectorASMMultPoints_0x4e3f20)
            return false;
        hook(nativeRam.data(), &nativeContext, &runtime);
        if (guestRam != nativeRam || std::memcmp(&guestContext, &nativeContext, sizeof(R5900Context)) != 0)
        {
            std::fprintf(stderr, "RW vector result/context differs from generated guest: %s\n", label);
            return false;
        }
        return true;
    }

    void storeMatrix(std::vector<uint8_t> &ram, const Matrix &matrix)
    {
        std::memcpy(ram.data() + kMatrix, matrix.data(), sizeof(matrix));
    }

    void storePoints(std::vector<uint8_t> &ram, uint32_t address, const std::vector<Point> &points)
    {
        for (size_t i = 0; i < points.size(); ++i)
            std::memcpy(ram.data() + address + i * 12u, points[i].data(), 12u);
    }
}

int main()
{
    setenv("P3FES_NATIVE_RW_VECTOR", "1", 1);
    PS2Runtime::RecompiledFunction original = &vectorASMMultPoints_0x4e3f20;
    ps2SetRecompiledFunctionTable({&original, kEntry, kEntry + 4u, 1u});
    PS2Runtime runtime;
    assert(p3fes_native_rw_math::install(runtime));
    assert(p3fes_native_rw_math::install(runtime));
    assert(runtime.lookupFunction(kEntry) != &vectorASMMultPoints_0x4e3f20);

    std::vector<uint8_t> ram(PS2_RAM_SIZE);
    Matrix matrix = {1.25f, -0.5f, 0.75f, 0.0f,
                     -0.25f, 1.5f, 0.125f, 0.0f,
                     0.5f, 0.25f, -1.0f, 0.0f,
                     3.0f, -2.0f, 1.0f, 0.0f};
    storeMatrix(ram, matrix);

    std::mt19937 rng(0x4E3F20u);
    std::uniform_real_distribution<float> values(-20.0f, 20.0f);
    for (unsigned trial = 0; trial < 128; ++trial)
    {
        const int32_t count = static_cast<int32_t>(1u + rng() % 32u);
        std::vector<Point> points(static_cast<size_t>(count) + 1u);
        for (auto &point : points)
            point = {values(rng), values(rng), values(rng)};
        std::vector<uint8_t> initial = ram;
        storePoints(initial, kSource, points);
        std::memset(initial.data() + kOutput, 0xA5, static_cast<size_t>(count) * 12u);
        assert(compareCall(initial, kOutput, kSource, count, "random-disjoint"));
    }

    // Exact in-place operation is safe because each next source point is
    // prefetched before the current output triple is stored.
    {
        std::vector<Point> points = {{1.0f, 2.0f, 3.0f}, {-4.0f, 0.5f, 8.0f},
                                     {2.0f, -3.0f, 7.0f}, {9.0f, 1.0f, -2.0f}};
        std::vector<uint8_t> initial = ram;
        storePoints(initial, kSource, points);
        assert(compareCall(initial, kSource, kSource, 3, "exact-in-place"));
    }

    // A one-point partial overlap is deliberately delegated to generated code.
    {
        std::vector<Point> points = {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f},
                                     {7.0f, 8.0f, 9.0f}, {10.0f, 11.0f, 12.0f}};
        std::vector<uint8_t> initial = ram;
        storePoints(initial, kSource, points);
        assert(compareCall(initial, kSource + 12u, kSource, 3, "partial-overlap-fallback"));
    }

    // Nonpositive counts and special floats remain on the original path.
    {
        std::vector<uint8_t> initial = ram;
        storePoints(initial, kSource, {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}});
        assert(compareCall(initial, kOutput, kSource, 0, "zero-count-fallback"));
        assert(compareCall(initial, kOutput, kSource, -1, "negative-count-fallback"));

        Point special = {std::numeric_limits<float>::infinity(), 2.0f, 3.0f};
        std::memcpy(initial.data() + kSource, special.data(), sizeof(special));
        assert(compareCall(initial, kOutput, kSource, 1, "infinity-input-fallback"));

        initial = ram;
        storePoints(initial, kSource, {{1.0f, 2.0f, 3.0f}, {4.0f, 5.0f, 6.0f}});
        const float subnormal = std::numeric_limits<float>::denorm_min();
        std::memcpy(initial.data() + kMatrix, &subnormal, sizeof(subnormal));
        assert(compareCall(initial, kOutput, kSource, 1, "subnormal-matrix-fallback"));
    }

    uint64_t nativeCount = 0, fallbackCount = 0;
    p3fes_native_rw_math::pointsCounters(nativeCount, fallbackCount);
    assert(nativeCount > 0u && fallbackCount >= 4u);
    return 0;
}
