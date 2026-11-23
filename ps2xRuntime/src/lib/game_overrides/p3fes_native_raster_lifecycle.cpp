#include "p3fes_native_raster_lifecycle.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace p3fes_native_rasters
{
    namespace
    {
        constexpr uint32_t kRwRasterSetFromImage = 0x004CDCE0u;
        constexpr uint32_t kRwRasterUnlock = 0x004CDE00u;
        constexpr uint32_t kRwRasterUnlockPalette = 0x004CDE40u;
        constexpr uint32_t kRwRasterDestroy = 0x004CDE90u;
        constexpr uint32_t kRwRasterCreate = 0x004CE0F0u;
        constexpr uint32_t kSkyRasterDestroy = 0x004DD2E0u;
        constexpr uint32_t kSkyRasterUnlock8To32 = 0x004DDAA0u;
        constexpr uint32_t kSkyRasterUnlock4To16 = 0x004DDD30u;
        constexpr uint32_t kSkyRasterUnlock = 0x004DECE0u;
        constexpr uint32_t kSkyRasterSetImagePalettised = 0x004EB8C0u;
        constexpr uint32_t kSkyRasterSetImageNormal = 0x004EBB40u;
        constexpr uint32_t kSkyRasterSetImage = 0x004EBFB0u;
        constexpr uint32_t kSkyRasterCreate = 0x004DB350u;
        constexpr size_t kHookCount = 13u;

        struct Entry
        {
            uint64_t generation = 0u;
            uint64_t revision = 1u;
        };

        struct Counters
        {
            uint64_t created = 0u;
            uint64_t lazyRegistered = 0u;
            uint64_t invalidated = 0u;
            uint64_t retired = 0u;
            uint64_t createYielded = 0u;
            uint64_t installFailures = 0u;
        };

        using GuestFunction = PS2Runtime::RecompiledFunction;

        struct HookSpec
        {
            uint32_t address;
            GuestFunction wrapper;
        };

        std::unordered_map<uint32_t, Entry> g_entries;
        std::unordered_map<uint32_t, GuestFunction> g_originals;
        uint64_t g_nextGeneration = 1u;
        uint64_t g_epoch = 1u;
        RetireCallback g_onRetire = nullptr;
        Counters g_counters{};

        void createdRaster(uint32_t raster)
        {
            if (!raster)
                return;

            const auto existing = g_entries.find(raster);
            if (existing != g_entries.end())
                return; // Create wrappers may nest; one guest allocation is one generation.

            Entry entry;
            entry.generation = g_nextGeneration++;
            if (g_nextGeneration == 0u)
                g_nextGeneration = 1u;
            g_entries.emplace(raster, entry);
            ++g_counters.created;
        }

        GuestFunction originalFor(uint32_t address)
        {
            const auto found = g_originals.find(address);
            return found == g_originals.end() ? nullptr : found->second;
        }

        // Mutations are invalidated before invoking guest code. Generated
        // functions can yield at EE checkpoints and resume at an interior PC,
        // bypassing an entry wrapper's post-call code. Pre-invalidation ensures
        // the next native bind observes a new revision even in that case.
        void rasterMutationHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t address = ctx->pc;
            const uint32_t raster = GPR_U32(ctx, 4);
            const GuestFunction original = originalFor(address);
            static const bool diag = [] { const char *v = std::getenv("P3FES_RASTER_DIAG"); return v && *v == '1'; }();
            if (diag)
            {
                uint32_t w = 0u, h = 0u, d = 0u, f = 0u, px = 0u;
                if (raster && raster < PS2_RAM_SIZE - 64u)
                {
                    std::memcpy(&px, rdram + raster + 4u, 4u); std::memcpy(&w, rdram + raster + 12u, 4u);
                    std::memcpy(&h, rdram + raster + 16u, 4u); std::memcpy(&d, rdram + raster + 20u, 4u);
                    std::memcpy(&f, rdram + raster + 32u, 4u);
                }
                std::fprintf(stderr, "[raster-inval] hook=%X a0=%X a1=%X ra=%X %ux%u d=%u f=%X px=%X\n", address, raster, GPR_U32(ctx, 5),
                             GPR_U32(ctx, 31), w, h, d, f, px);
            }
            invalidate(raster);
            if (original)
                original(rdram, ctx, runtime);
        }

        void rasterCreateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t address = ctx->pc;
            const uint32_t returnAddress = GPR_U32(ctx, 31);
            // The device callback returns RwBool; only RwRasterCreate returns
            // the allocated pointer. Keep its input before guest calls change a0.
            const uint32_t deviceRaster = GPR_U32(ctx, 4);
            const GuestFunction original = originalFor(address);
            if (!original)
                return;

            original(rdram, ctx, runtime);
            if (ctx->pc != returnAddress)
            {
                ++g_counters.createYielded;
                return;
            }
            const uint32_t result = GPR_U32(ctx, 2);
            if (result)
                createdRaster(address == kSkyRasterCreate ? deviceRaster : result);
        }

        void rasterDestroyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            const uint32_t address = ctx->pc;
            const uint32_t raster = GPR_U32(ctx, 4);
            const GuestFunction original = originalFor(address);
            retire(raster);
            if (original)
                original(rdram, ctx, runtime);
        }

        const HookSpec kHooks[kHookCount] = {
            {kRwRasterCreate, &rasterCreateHook},
            {kSkyRasterCreate, &rasterCreateHook},
            {kRwRasterSetFromImage, &rasterMutationHook},
            {kSkyRasterSetImage, &rasterMutationHook},
            {kSkyRasterSetImageNormal, &rasterMutationHook},
            {kSkyRasterSetImagePalettised, &rasterMutationHook},
            {kRwRasterUnlock, &rasterMutationHook},
            {kRwRasterUnlockPalette, &rasterMutationHook},
            {kSkyRasterUnlock, &rasterMutationHook},
            {kSkyRasterUnlock8To32, &rasterMutationHook},
            {kSkyRasterUnlock4To16, &rasterMutationHook},
            {kRwRasterDestroy, &rasterDestroyHook},
            {kSkyRasterDestroy, &rasterDestroyHook},
        };
    }

    void reset()
    {
        if (g_onRetire)
        {
            for (const auto &[raster, entry] : g_entries)
            {
                g_onRetire(raster, entry.generation);
                ++g_counters.retired;
            }
        }
        g_entries.clear();
        g_counters = {};
        ++g_epoch;
    }

    bool install(PS2Runtime &runtime, RetireCallback onRetire)
    {
        g_onRetire = onRetire;
        bool allInstalled = true;
        for (const HookSpec &spec : kHooks)
        {
            GuestFunction current = runtime.lookupFunction(spec.address);
            if (!current)
            {
                ++g_counters.installFailures;
                allInstalled = false;
                continue;
            }

            if (current != spec.wrapper)
                g_originals[spec.address] = current;
            else if (g_originals.find(spec.address) == g_originals.end())
            {
                ++g_counters.installFailures;
                allInstalled = false;
                continue;
            }

            if (current != spec.wrapper && !runtime.replaceFunction(spec.address, spec.wrapper))
            {
                ++g_counters.installFailures;
                allInstalled = false;
            }
        }
        return allInstalled;
    }

    uint64_t epoch() { return g_epoch; }

    Version version(uint32_t raster)
    {
        if (!raster)
            return {};
        auto found = g_entries.find(raster);
        if (found == g_entries.end())
        {
            Entry entry;
            entry.generation = g_nextGeneration++;
            if (g_nextGeneration == 0u)
                g_nextGeneration = 1u;
            found = g_entries.emplace(raster, entry).first;
            ++g_counters.lazyRegistered;
        }
        return {found->second.generation, found->second.revision};
    }

    void invalidate(uint32_t raster)
    {
        if (!raster)
            return;
        (void)version(raster);
        Entry &entry = g_entries.find(raster)->second;
        ++g_epoch;
        ++entry.revision;
        if (entry.revision == 0u)
            entry.revision = 1u;
        ++g_counters.invalidated;
    }

    void retire(uint32_t raster)
    {
        if (!raster)
            return;
        const auto found = g_entries.find(raster);
        if (found == g_entries.end())
            return;
        const uint64_t generation = found->second.generation;
        g_entries.erase(found);
        ++g_epoch;
        ++g_counters.retired;
        if (g_onRetire)
            g_onRetire(raster, generation);
    }

    void logCounters()
    {
        std::fprintf(stderr,
                     "[p3fes-raster] created=%llu lazy=%llu invalidated=%llu retired=%llu create-yield=%llu install-fail=%llu live=%u\n",
                     static_cast<unsigned long long>(g_counters.created),
                     static_cast<unsigned long long>(g_counters.lazyRegistered),
                     static_cast<unsigned long long>(g_counters.invalidated),
                     static_cast<unsigned long long>(g_counters.retired),
                     static_cast<unsigned long long>(g_counters.createYielded),
                     static_cast<unsigned long long>(g_counters.installFailures),
                     static_cast<unsigned>(g_entries.size()));
    }
}
