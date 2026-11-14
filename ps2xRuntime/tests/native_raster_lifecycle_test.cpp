#include "game_overrides/p3fes_native_raster_lifecycle.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x004C0000u;
    constexpr uint32_t kTableEnd = 0x00500000u;
    constexpr uint32_t kRwRasterCreate = 0x004CE0F0u;
    constexpr uint32_t kSkyRasterCreate = 0x004DB350u;
    constexpr uint32_t kRwRasterUnlock = 0x004CDE00u;
    constexpr uint32_t kRwRasterDestroy = 0x004CDE90u;
    constexpr uint32_t kSkyRasterDestroy = 0x004DD2E0u;
    constexpr uint32_t kSyntheticReturn = 0x004C0100u;
    constexpr uint32_t kUnlockContinuation = kRwRasterUnlock + 4u;

    uint32_t g_nextCreateRaster = 0u;
    bool g_yieldNextUnlock = false;
    std::vector<std::pair<uint32_t, uint64_t>> g_retired;

    uint32_t regU32(const R5900Context *ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[index]));
    }

    void setRegU32(R5900Context *ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx->r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void fakeReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = regU32(ctx, 31);
    }

    void fakeRasterCreate(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setRegU32(ctx, 2, g_nextCreateRaster);
        ctx->pc = regU32(ctx, 31);
    }

    void fakeSkyRasterCreate(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setRegU32(ctx, 4, 0xDEADBEEFu); // The callback need not preserve a0.
        setRegU32(ctx, 2, 1u); // RwBool, not a raster address.
        ctx->pc = regU32(ctx, 31);
    }

    void fakeRasterUnlock(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (g_yieldNextUnlock)
        {
            g_yieldNextUnlock = false;
            ctx->pc = kUnlockContinuation;
            return;
        }
        ctx->pc = regU32(ctx, 31);
    }

    void fakeRasterDestroy(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = regU32(ctx, 31);
    }

    void fakeSkyRasterDestroy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t outerReturn = regU32(ctx, 31);
        const uint32_t raster = regU32(ctx, 4);
        setRegU32(ctx, 31, kSyntheticReturn);
        ctx->pc = kRwRasterDestroy;
        runtime->lookupFunction(kRwRasterDestroy)(rdram, ctx, runtime);
        if (ctx->pc != kSyntheticReturn)
        {
            std::fprintf(stderr, "inner raster destroy did not return to synthetic site\n");
            std::abort();
        }
        setRegU32(ctx, 4, raster);
        setRegU32(ctx, 31, outerReturn);
        ctx->pc = outerReturn;
    }

    void onRetire(uint32_t raster, uint64_t generation)
    {
        g_retired.emplace_back(raster, generation);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "native raster lifecycle test failed: %s\n", message);
            std::abort();
        }
    }

    void invoke(PS2Runtime &runtime, uint32_t address, uint32_t raster,
                uint32_t returnAddress = kSyntheticReturn)
    {
        R5900Context ctx{};
        setRegU32(&ctx, 4, raster);
        setRegU32(&ctx, 31, returnAddress);
        ctx.pc = address;
        runtime.lookupFunction(address)(nullptr, &ctx, &runtime);
        require(ctx.pc == returnAddress, "guest wrapper did not complete at the expected return PC");
    }

    void createRaster(PS2Runtime &runtime, uint32_t raster)
    {
        g_nextCreateRaster = raster;
        invoke(runtime, kRwRasterCreate, 0u);
    }
}

int main()
{
    constexpr uint32_t slotCount = (kTableEnd - kTableBase) / 4u;
    std::vector<PS2Runtime::RecompiledFunction> functions(slotCount, &fakeReturn);
    functions[(kRwRasterCreate - kTableBase) / 4u] = &fakeRasterCreate;
    functions[(kSkyRasterCreate - kTableBase) / 4u] = &fakeSkyRasterCreate;
    functions[(kRwRasterUnlock - kTableBase) / 4u] = &fakeRasterUnlock;
    functions[(kRwRasterDestroy - kTableBase) / 4u] = &fakeRasterDestroy;
    functions[(kSkyRasterDestroy - kTableBase) / 4u] = &fakeSkyRasterDestroy;
    ps2SetRecompiledFunctionTable({functions.data(), kTableBase, kTableEnd, slotCount});

    PS2Runtime runtime;
    p3fes_native_rasters::reset();
    g_retired.clear();
    require(p3fes_native_rasters::install(runtime, &onRetire), "failed to install lifecycle wrappers");

    constexpr uint32_t rasterA = 0x00120000u;
    createRaster(runtime, rasterA);
    const auto created = p3fes_native_rasters::version(rasterA);
    require(created.generation != 0u && created.revision == 1u, "create did not register revision 1");

    invoke(runtime, kRwRasterUnlock, rasterA);
    const auto afterUnlock = p3fes_native_rasters::version(rasterA);
    require(afterUnlock.generation == created.generation && afterUnlock.revision == created.revision + 1u,
            "unlock did not dirty the existing raster generation");

    g_yieldNextUnlock = true;
    R5900Context yielded{};
    setRegU32(&yielded, 4, rasterA);
    setRegU32(&yielded, 31, kSyntheticReturn);
    yielded.pc = kRwRasterUnlock;
    runtime.lookupFunction(kRwRasterUnlock)(nullptr, &yielded, &runtime);
    require(yielded.pc == kUnlockContinuation, "fake unlock did not yield at the continuation PC");
    const auto afterYield = p3fes_native_rasters::version(rasterA);
    require(afterYield.generation == created.generation && afterYield.revision == afterUnlock.revision + 1u,
            "pre-invalidation was lost when unlock yielded");
    runtime.lookupFunction(kUnlockContinuation)(nullptr, &yielded, &runtime);
    require(yielded.pc == kSyntheticReturn, "resumed unlock continuation did not return");
    require(p3fes_native_rasters::version(rasterA).revision == afterYield.revision,
            "resuming an interior PC invalidated a second time");

    invoke(runtime, kSkyRasterDestroy, rasterA);
    require(g_retired.size() == 1u && g_retired[0] == std::make_pair(rasterA, created.generation),
            "nested Sky/RW destroy retired more than once or lost generation");

    createRaster(runtime, rasterA);
    const auto recreated = p3fes_native_rasters::version(rasterA);
    require(recreated.generation != created.generation && recreated.revision == 1u,
            "reused guest address did not receive a new generation");

    constexpr uint32_t rasterB = 0x00120100u;
    createRaster(runtime, rasterB);
    const auto second = p3fes_native_rasters::version(rasterB);
    p3fes_native_rasters::reset();
    require(g_retired.size() == 3u, "reset did not retire every remaining host resource");
    require(std::find(g_retired.begin() + 1, g_retired.end(), std::make_pair(rasterA, recreated.generation)) != g_retired.end() &&
            std::find(g_retired.begin() + 1, g_retired.end(), std::make_pair(rasterB, second.generation)) != g_retired.end(),
            "reset retired an unexpected generation");

    g_retired.clear();
    constexpr uint32_t rasterC = 0x00120200u;
    invoke(runtime, kSkyRasterCreate, rasterC);
    p3fes_native_rasters::reset();
    require(g_retired.size() == 1u && g_retired[0].first == rasterC,
            "Sky create registered its boolean result instead of its raster input");

    std::puts("native raster lifecycle tests passed");
    return 0;
}
