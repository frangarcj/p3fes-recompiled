#include "game_overrides/p3fes_native_texture_access.h"

#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x004C0000u;
    constexpr uint32_t kTableEnd = 0x00500000u;
    constexpr uint32_t kSpeculate = 0x004F0F30u;
    constexpr uint32_t kAccess = 0x004F1020u;
    constexpr uint32_t kRelease = 0x004F1540u;
    constexpr uint32_t kRasterLock = 0x004F1780u;
    constexpr uint32_t kRestore = 0x004F1B30u;
    constexpr uint32_t kReturnPc = 0x004C0100u;
    constexpr uint32_t kRasterA = 0x00100000u;
    constexpr uint32_t kRasterB = 0x00100100u;
    constexpr uint32_t kRasterC = 0x00100200u;
    constexpr uint32_t kRasterD = 0x00100300u;
    constexpr uint32_t kPixels = 0x00110000u;
    constexpr uint32_t kRasterSlot = 0x00002000u;
    constexpr uint32_t kRwCurrentRaster = 0x007CEABCu;
    constexpr uint32_t kRwCurrentRasterFlags = 0x007CEAC0u;
    constexpr uint32_t kSkyCurrentRaster = 0x0077F224u;
    constexpr uint32_t kSkyCurrentRasterFlags = 0x0077F228u;
    constexpr uint32_t kOriginalMarker = 0x0BADCAFEu;

    uint32_t g_prepareCalls = 0u;
    uint32_t g_speculateOriginalCalls = 0u;
    uint32_t g_accessOriginalCalls = 0u;
    uint32_t g_releaseOriginalCalls = 0u;
    uint32_t g_lockOriginalCalls = 0u;
    uint32_t g_restoreOriginalCalls = 0u;
    bool g_prepareResult = true;
    std::vector<uint8_t> *g_rdram = nullptr;

    uint32_t regU32(const R5900Context *ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[index]));
    }

    void setRegU32(R5900Context *ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx->r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void putU32(std::vector<uint8_t> &ram, uint32_t address, uint32_t value)
    {
        std::memcpy(ram.data() + address, &value, sizeof(value));
    }

    uint32_t getU32(const std::vector<uint8_t> &ram, uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, ram.data() + address, sizeof(value));
        return value;
    }

    void fakeReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setRegU32(ctx, 2, kOriginalMarker);
        ctx->pc = regU32(ctx, 31);
    }

    void fakeAccess(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_accessOriginalCalls;
        fakeReturn(nullptr, ctx, nullptr);
    }

    void fakeSpeculate(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_speculateOriginalCalls;
        fakeReturn(nullptr, ctx, nullptr);
    }

    void fakeRelease(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_releaseOriginalCalls;
        fakeReturn(nullptr, ctx, nullptr);
    }

    void fakeLock(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_lockOriginalCalls;
        fakeReturn(nullptr, ctx, nullptr);
    }

    void fakeRestore(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_restoreOriginalCalls;
        fakeReturn(nullptr, ctx, nullptr);
    }

    bool prepareRaster(const uint8_t *rdram, uint32_t raster, std::string &textureName)
    {
        ++g_prepareCalls;
        if (!rdram || (raster != kRasterA && raster != kRasterD))
            return false;
        textureName = "test-native-texture";
        return g_prepareResult;
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "native texture access integration test failed: %s\n", message);
            std::abort();
        }
    }

    R5900Context invoke(PS2Runtime &runtime, uint32_t address, uint32_t arg0,
                        uint32_t arg1 = 0u, uint32_t returnPc = kReturnPc)
    {
        R5900Context ctx{};
        setRegU32(&ctx, 4, arg0);
        setRegU32(&ctx, 5, arg1);
        setRegU32(&ctx, 31, returnPc);
        ctx.pc = address;
        runtime.lookupFunction(address)(g_rdram ? g_rdram->data() : nullptr, &ctx, &runtime);
        require(ctx.pc == returnPc, "hook/original did not return to the caller PC");
        return ctx;
    }

    void initRaster(std::vector<uint8_t> &ram, uint32_t raster, uint32_t pixels,
                    uint32_t type = 4u)
    {
        putU32(ram, raster + 4u, pixels);
        putU32(ram, raster + 12u, 64u);
        putU32(ram, raster + 16u, 32u);
        putU32(ram, raster + 20u, 32u);
        putU32(ram, raster + 24u, 256u);
        putU32(ram, raster + 32u, type);
    }
}

int main()
{
    constexpr uint32_t slotCount = (kTableEnd - kTableBase) / 4u;
    std::vector<PS2Runtime::RecompiledFunction> functions(slotCount, &fakeReturn);
    functions[(kSpeculate - kTableBase) / 4u] = &fakeSpeculate;
    functions[(kAccess - kTableBase) / 4u] = &fakeAccess;
    functions[(kRelease - kTableBase) / 4u] = &fakeRelease;
    functions[(kRasterLock - kTableBase) / 4u] = &fakeLock;
    functions[(kRestore - kTableBase) / 4u] = &fakeRestore;
    ps2SetRecompiledFunctionTable({functions.data(), kTableBase, kTableEnd, slotCount});

    PS2Runtime runtime;
    std::vector<uint8_t> ram(0x02000000u, 0u);
    g_rdram = &ram;
    initRaster(ram, kRasterA, kPixels);
    initRaster(ram, kRasterB, kPixels, 5u); // Camera/render target: unsupported.
    initRaster(ram, kRasterC, 0u);          // CPU pixels absent: unsupported.
    initRaster(ram, kRasterD, kPixels);
    putU32(ram, kRasterSlot, kRasterA);

    require(p3fes_native_texture_access::install(runtime, &prepareRaster), "failed to install texture access hooks");

    // Speculative access prepares and marks a supported raster, but leaves
    // both RenderWare and Sky current-raster state untouched until real access.
    R5900Context speculate = invoke(runtime, kSpeculate, kRasterD, 1u);
    require(regU32(&speculate, 2) == 0u && g_prepareCalls == 1u && g_speculateOriginalCalls == 0u &&
            p3fes_native_texture_access::isNativeRaster(kRasterD),
            "supported speculate did not prepare/mark without changing the void return value");
    require(getU32(ram, kRwCurrentRaster) == 0u && getU32(ram, kRwCurrentRasterFlags) == 0u &&
            getU32(ram, kSkyCurrentRaster) == 0u && getU32(ram, kSkyCurrentRasterFlags) == 0u,
            "speculate changed current-raster binding globals");

    R5900Context speculateUnsupported = invoke(runtime, kSpeculate, kRasterB, 1u);
    require(regU32(&speculateUnsupported, 2) == kOriginalMarker && g_speculateOriginalCalls == 1u &&
            g_prepareCalls == 1u && !p3fes_native_texture_access::isNativeRaster(kRasterB),
            "unsupported speculate did not delegate to the original");

    // A supported CPU-backed texture prepares once and binds both RW and Sky
    // globals, with the access flags retained for the native draw path.
    constexpr uint32_t accessFlags = 0x30u;
    R5900Context access = invoke(runtime, kAccess, kRasterA, accessFlags);
    require(regU32(&access, 2) == 1u, "native access did not return success in v0");
    require(g_prepareCalls == 2u && g_accessOriginalCalls == 0u,
            "valid raster did not use exactly one native prepare callback");
    require(getU32(ram, kRwCurrentRaster) == kRasterA &&
            getU32(ram, kRwCurrentRasterFlags) == accessFlags &&
            getU32(ram, kSkyCurrentRaster) == kRasterA &&
            getU32(ram, kSkyCurrentRasterFlags) == accessFlags,
            "native access did not publish current raster and flags");
    require(p3fes_native_texture_access::isNativeRaster(kRasterA), "successful raster was not marked native");

    // Release, the pointer-to-raster cache lock, and restore are metadata-only
    // for a marked native raster and must not repeat preparation or call GS code.
    R5900Context release = invoke(runtime, kRelease, kRasterA);
    require(g_releaseOriginalCalls == 0u && g_prepareCalls == 2u,
            "native release called the guest fallback or prepared again");
    (void)release;

    R5900Context lock = invoke(runtime, kRasterLock, kRasterSlot);
    require(regU32(&lock, 2) == 1u && g_lockOriginalCalls == 0u && g_prepareCalls == 2u,
            "native raster lock did not return success without preparation");

    R5900Context restore = invoke(runtime, kRestore, 0u);
    require(g_restoreOriginalCalls == 0u && g_prepareCalls == 2u,
            "native restore called the guest fallback or prepared again");
    (void)restore;

    // Unsupported descriptors and host-prepare failure must delegate to the
    // originals; they must never be marked native.
    const uint32_t prepareBeforeFallback = g_prepareCalls;
    R5900Context type5 = invoke(runtime, kAccess, kRasterB);
    require(regU32(&type5, 2) == kOriginalMarker && g_accessOriginalCalls == 1u &&
            g_prepareCalls == prepareBeforeFallback && !p3fes_native_texture_access::isNativeRaster(kRasterB),
            "type-5 raster did not fall back before native preparation");

    R5900Context missingPixels = invoke(runtime, kAccess, kRasterC);
    require(regU32(&missingPixels, 2) == kOriginalMarker && g_accessOriginalCalls == 2u &&
            g_prepareCalls == prepareBeforeFallback && !p3fes_native_texture_access::isNativeRaster(kRasterC),
            "raster without CPU pixels did not fall back");

    g_prepareResult = false;
    R5900Context prepareFailed = invoke(runtime, kAccess, kRasterD);
    require(regU32(&prepareFailed, 2) == kOriginalMarker && g_accessOriginalCalls == 3u &&
            g_prepareCalls == prepareBeforeFallback + 1u &&
            !p3fes_native_texture_access::isNativeRaster(kRasterD),
            "failed host preparation did not fall back");
    g_prepareResult = true;

    // Destroy lifecycle calls forget the marker separately. After forgetting,
    // Release must delegate without attempting to re-create native state.
    p3fes_native_texture_access::forgetRaster(kRasterA);
    require(!p3fes_native_texture_access::isNativeRaster(kRasterA), "forgetRaster retained native marker");
    invoke(runtime, kRelease, kRasterA);
    require(g_releaseOriginalCalls == 1u && g_prepareCalls == prepareBeforeFallback + 1u,
            "release after forget recreated state instead of delegating");

    const auto counters = p3fes_native_texture_access::counters();
    require(counters.speculateNative == 1u && counters.speculateFallback == 1u,
            "speculate native/fallback counters are incorrect");

    std::puts("native texture access integration tests passed");
    return 0;
}
