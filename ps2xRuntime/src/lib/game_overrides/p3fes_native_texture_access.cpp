#include "p3fes_native_texture_access.h"
#include "p3fes_native_texture_access_internal.h"
#include "p3fes_native_raster_lifecycle.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace p3fes_native_texture_access
{
    namespace
    {
        constexpr uint32_t kRwGp = 0x007D2CF0u;
        constexpr uint32_t kRwCurrentRaster = kRwGp - 16948u;
        constexpr uint32_t kRwCurrentRasterFlags = kRwGp - 16944u;
        // RpSkyTexCache's own current binding, used by Release/Restore in the
        // original MIPS implementation (absolute gp-independent globals).
        constexpr uint32_t kSkyCurrentRaster = 0x0077F224u;
        constexpr uint32_t kSkyCurrentRasterFlags = 0x0077F228u;

        constexpr uint32_t kRpSkyTexCacheAccessRaster = 0x004F1020u;
        constexpr uint32_t kRpSkyTexCacheAccessSpeculate = 0x004F0F30u;
        constexpr uint32_t kRpSkyTexCacheReleaseRaster = 0x004F1540u;
        constexpr uint32_t kRpSkyTexCacheRasterLock = 0x004F1780u;
        constexpr uint32_t kRpSkyTexCacheRestore = 0x004F1B30u;

        using GuestFunction = PS2Runtime::RecompiledFunction;

        PrepareRaster g_prepareRaster = nullptr;
        std::unordered_map<uint32_t, GuestFunction> g_originals;
        detail::RasterSet g_nativeRasters;
        // Hooks, lifecycle operations and diagnostics all run on the EE worker.
        Counters g_counters{};
        // accessRasterNative's memo of the last raster it prepared (see there)
        uint32_t g_memoRaster = 0u;
        uint64_t g_memoEpoch = 0u;

        bool readGuestU32(const uint8_t *rdram, uint32_t address, uint32_t &value)
        {
            return detail::readU32(rdram, PS2_RAM_SIZE, address, value);
        }

        bool writeGuestU32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (!rdram || address > PS2_RAM_SIZE - sizeof(value))
                return false;
            std::memcpy(rdram + address, &value, sizeof(value));
            return true;
        }

        GuestFunction originalFor(uint32_t address)
        {
            const auto found = g_originals.find(address);
            return found == g_originals.end() ? nullptr : found->second;
        }

        bool prepareCpuRaster(uint8_t *rdram, uint32_t raster)
        {
            if (!detail::nativeTextureDescriptor(rdram, PS2_RAM_SIZE, raster))
            {
                ++g_counters.invalidRaster;
                return false;
            }

            std::string textureName;
            if (!g_prepareRaster || !g_prepareRaster(rdram, raster, textureName))
            {
                ++g_counters.prepareFailed;
                return false;
            }
            return true;
        }

        void invokeOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GuestFunction original = originalFor(address))
                original(rdram, ctx, runtime);
        }

        void accessRasterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t raster = GPR_U32(ctx, 4);
            const uint32_t accessFlags = GPR_U32(ctx, 5);
            if (prepareCpuRaster(rdram, raster))
            {
                g_nativeRasters.mark(raster);
                // The original's native-visible state is the currently bound
                // raster and its access flags. It also rewrites GS TEX1/CLAMP
                // and emits GIF packets; the host renderer consumes the raster
                // plus existing filter/address state without a GS transfer.
                writeGuestU32(rdram, kRwCurrentRaster, raster);
                writeGuestU32(rdram, kRwCurrentRasterFlags, accessFlags);
                writeGuestU32(rdram, kSkyCurrentRaster, raster);
                writeGuestU32(rdram, kSkyCurrentRasterFlags, accessFlags);
                SET_GPR_U32(ctx, 2, 1u); // MIPS v0: success.
                ++g_counters.accessNative;
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }

            g_nativeRasters.forget(raster);
            g_memoRaster = 0u;
            ++g_counters.accessFallback;
            invokeOriginal(kRpSkyTexCacheAccessRaster, rdram, ctx, runtime);
        }

        void accessSpeculateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t raster = GPR_U32(ctx, 4);
            if (prepareCpuRaster(rdram, raster))
            {
                // This path only prewarms the host texture. Preserve the
                // RenderWare and Sky current-raster globals for the real bind.
                g_nativeRasters.mark(raster);
                ++g_counters.speculateNative;
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }

            g_nativeRasters.forget(raster);
            g_memoRaster = 0u;
            ++g_counters.speculateFallback;
            invokeOriginal(kRpSkyTexCacheAccessSpeculate, rdram, ctx, runtime);
        }

        void releaseRasterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t raster = GPR_U32(ctx, 4);
            if (g_nativeRasters.contains(raster) &&
                detail::nativeTextureDescriptor(rdram, PS2_RAM_SIZE, raster))
            {
                // No GS texture-cache slot was acquired for a native texture.
                // The raster lifecycle wrapper separately retires its host
                // resource when the guest destroys the raster.
                ++g_counters.releaseNative;
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }

            ++g_counters.releaseFallback;
            invokeOriginal(kRpSkyTexCacheReleaseRaster, rdram, ctx, runtime);
        }

        void rasterLockHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            // The Sky cache lock takes a pointer to the raster slot in a0, not
            // RwRasterLock's raster+flags pair. Its result is v0 = 1/0.
            uint32_t raster = 0u;
            const uint32_t rasterSlot = GPR_U32(ctx, 4);
            if (readGuestU32(rdram, rasterSlot, raster) && g_nativeRasters.contains(raster) &&
                detail::nativeTextureDescriptor(rdram, PS2_RAM_SIZE, raster))
            {
                SET_GPR_U32(ctx, 2, 1u);
                ++g_counters.lockNative;
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }

            ++g_counters.lockFallback;
            invokeOriginal(kRpSkyTexCacheRasterLock, rdram, ctx, runtime);
        }

        void restoreHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            uint32_t raster = 0u;
            if (readGuestU32(rdram, kSkyCurrentRaster, raster) && g_nativeRasters.contains(raster) &&
                detail::nativeTextureDescriptor(rdram, PS2_RAM_SIZE, raster))
            {
                // Restore only rebuilds the GS-side setup for the bound raster;
                // host texture binding is performed from the RenderWare state.
                ++g_counters.restoreNative;
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }

            ++g_counters.restoreFallback;
            invokeOriginal(kRpSkyTexCacheRestore, rdram, ctx, runtime);
        }

        struct HookSpec
        {
            uint32_t address;
            GuestFunction wrapper;
        };

        constexpr HookSpec kHooks[] = {
            {kRpSkyTexCacheAccessSpeculate, &accessSpeculateHook},
            {kRpSkyTexCacheAccessRaster, &accessRasterHook},
            {kRpSkyTexCacheReleaseRaster, &releaseRasterHook},
            {kRpSkyTexCacheRasterLock, &rasterLockHook},
            {kRpSkyTexCacheRestore, &restoreHook},
        };
    }

    bool install(PS2Runtime &runtime, PrepareRaster prepareRaster)
    {
        if (!prepareRaster)
            return false;
        g_prepareRaster = prepareRaster;

        bool allInstalled = true;
        for (const HookSpec &spec : kHooks)
        {
            GuestFunction current = runtime.lookupFunction(spec.address);
            if (!current)
            {
                allInstalled = false;
                continue;
            }

            if (current == spec.wrapper)
            {
                if (g_originals.find(spec.address) == g_originals.end())
                    allInstalled = false;
                continue;
            }

            g_originals[spec.address] = current;
            if (!runtime.replaceFunction(spec.address, spec.wrapper))
                allInstalled = false;
        }
        return allInstalled;
    }

    bool accessRasterNative(uint8_t *rdram, uint32_t raster, uint32_t accessFlags)
    {
        // The last raster prepared and marked, and the raster lifecycle epoch it was valid at: preparing it again
        // (descriptor check, host texture lookup, marking) can only repeat the same answer while no raster has been
        // modified, retired or reset (epoch(), the same rule the draw path's name cache relies on).
        uint32_t &memoRaster = g_memoRaster;
        uint64_t &memoEpoch = g_memoEpoch;
        const uint64_t epoch = p3fes_native_rasters::epoch();
        if (raster != memoRaster || epoch != memoEpoch)
        {
            if (!prepareCpuRaster(rdram, raster))
            {
                g_nativeRasters.forget(raster);
                memoRaster = 0u;
                return false;
            }
            g_nativeRasters.mark(raster);
            memoRaster = raster;
            memoEpoch = epoch;
        }
        writeGuestU32(rdram, kRwCurrentRaster, raster);
        writeGuestU32(rdram, kRwCurrentRasterFlags, accessFlags);
        writeGuestU32(rdram, kSkyCurrentRaster, raster);
        writeGuestU32(rdram, kSkyCurrentRasterFlags, accessFlags);
        ++g_counters.speculateNative;
        ++g_counters.accessNative;
        return true;
    }

    void forgetRaster(uint32_t raster)
    {
        if (raster == g_memoRaster)
            g_memoRaster = 0u;
        g_nativeRasters.forget(raster);
    }

    bool isNativeRaster(uint32_t raster)
    {
        return g_nativeRasters.contains(raster);
    }

    Counters counters()
    {
        return g_counters;
    }

    void resetCounters()
    {
        g_counters = {};
    }

    void logCounters()
    {
        const Counters value = counters();
        std::fprintf(stderr,
                     "[p3fes-native-texture-access] speculate=%llu/%llu access=%llu/%llu release=%llu/%llu lock=%llu/%llu restore=%llu/%llu invalid=%llu prepare-failed=%llu\n",
                     static_cast<unsigned long long>(value.speculateNative),
                     static_cast<unsigned long long>(value.speculateFallback),
                     static_cast<unsigned long long>(value.accessNative),
                     static_cast<unsigned long long>(value.accessFallback),
                     static_cast<unsigned long long>(value.releaseNative),
                     static_cast<unsigned long long>(value.releaseFallback),
                     static_cast<unsigned long long>(value.lockNative),
                     static_cast<unsigned long long>(value.lockFallback),
                     static_cast<unsigned long long>(value.restoreNative),
                     static_cast<unsigned long long>(value.restoreFallback),
                     static_cast<unsigned long long>(value.invalidRaster),
                     static_cast<unsigned long long>(value.prepareFailed));
    }
}
