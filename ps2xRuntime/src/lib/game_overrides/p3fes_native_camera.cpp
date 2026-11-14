#include "p3fes_native_camera.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace p3fes_native_camera
{
    namespace
    {
        constexpr uint32_t kSkyCameraBeginUpdate = 0x004D84C0u;
        constexpr uint32_t kSkyCameraEndUpdate = 0x004D8E80u;
        constexpr uint32_t kCameraRasterOffset = 0x60u;
        constexpr uint32_t kRasterTypeOffset = 0x20u;

        // Sky's non-type-5 path updates these persistent RenderWare/GIF shadow
        // globals. The native renderer reads render state from the camera and
        // RenderWare globals directly, while these stores preserve the guest
        // bookkeeping consumed by later RenderWare entry points.
        constexpr uint32_t kGpCameraReady = 0x42C8u;
        constexpr uint32_t kGpCameraBeginCount = 0x4258u;
        constexpr uint32_t kGpCameraPending = 0x425Cu;
        constexpr uint32_t kGpCachedTextureState = 0x42B8u;
        constexpr uint32_t kGpGsState = 0x42A8u;
        constexpr uint32_t kGpCacheRefresh = 0x4278u;
        constexpr uint32_t kGpGsStateEnabled = 0x4358u;
        constexpr uint32_t kGpGsStateHigh = 0x435Cu;
        constexpr uint32_t kGpGsStateLow = 0x4360u;
        constexpr uint32_t kGpGsStateAlpha = 0x4344u;

        using GuestFunction = PS2Runtime::RecompiledFunction;
        std::unordered_map<uint32_t, GuestFunction> g_originals;

        struct CameraCounters
        {
            uint64_t beginNative = 0u;
            uint64_t beginFallback = 0u;
            uint64_t endNative = 0u;
            uint64_t endFallback = 0u;
            std::array<uint64_t, 8> beginNativeByType{};
            std::array<uint64_t, 8> beginFallbackByType{};
            std::array<uint64_t, 8> endNativeByType{};
            std::array<uint64_t, 8> endFallbackByType{};
            bool haveFirstNative = false;
            uint8_t firstNativeType = 0xFFu;
            uint32_t firstNativeCamera = 0u;
            uint32_t firstNativeRaster = 0u;
            bool haveFirstFallback = false;
            uint8_t firstFallbackType = 0xFFu;
            uint32_t firstFallbackCamera = 0u;
            uint32_t firstFallbackRaster = 0u;
        };
        CameraCounters g_counters;

        // True between RwCameraBeginUpdate and RwCameraEndUpdate on a texture
        // camera (raster type > 4). Hooks run on the EE executor thread, so a
        // relaxed atomic is enough.
        std::atomic<bool> g_inTextureCamera{false};

        void recordCameraPath(bool begin, bool native, uint8_t type,
                              uint32_t camera, uint32_t raster)
        {
            if (begin)
            {
                if (native) ++g_counters.beginNative;
                else ++g_counters.beginFallback;
            }
            else
            {
                if (native) ++g_counters.endNative;
                else ++g_counters.endFallback;
            }

            if (type < 8u)
            {
                if (begin && native) ++g_counters.beginNativeByType[type];
                else if (begin) ++g_counters.beginFallbackByType[type];
                else if (native) ++g_counters.endNativeByType[type];
                else ++g_counters.endFallbackByType[type];
            }

            if (native && !g_counters.haveFirstNative)
            {
                g_counters.haveFirstNative = true;
                g_counters.firstNativeType = type;
                g_counters.firstNativeCamera = camera;
                g_counters.firstNativeRaster = raster;
            }
            if (!native && !g_counters.haveFirstFallback)
            {
                g_counters.haveFirstFallback = true;
                g_counters.firstFallbackType = type;
                g_counters.firstFallbackCamera = camera;
                g_counters.firstFallbackRaster = raster;
            }
        }

        bool guestRange(uint32_t address, uint32_t bytes)
        {
            return address <= PS2_RAM_SIZE && bytes <= PS2_RAM_SIZE - address;
        }

        bool read32(const uint8_t *rdram, uint32_t address, uint32_t &value)
        {
            if (!rdram || !guestRange(address, sizeof(value)))
                return false;
            std::memcpy(&value, rdram + address, sizeof(value));
            return true;
        }

        bool write32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (!rdram || !guestRange(address, sizeof(value)))
                return false;
            std::memcpy(rdram + address, &value, sizeof(value));
            return true;
        }

        bool read64(const uint8_t *rdram, uint32_t address, uint64_t &value)
        {
            if (!rdram || !guestRange(address, sizeof(value)))
                return false;
            std::memcpy(&value, rdram + address, sizeof(value));
            return true;
        }

        bool write64(uint8_t *rdram, uint32_t address, uint64_t value)
        {
            if (!rdram || !guestRange(address, sizeof(value)))
                return false;
            std::memcpy(rdram + address, &value, sizeof(value));
            return true;
        }

        uint32_t gpAddress(uint32_t gp, uint32_t negativeOffset)
        {
            return gp - negativeOffset;
        }

        GuestFunction originalFor(uint32_t address)
        {
            const auto found = g_originals.find(address);
            return found == g_originals.end() ? nullptr : found->second;
        }

        void skyCameraBeginUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;

            const uint32_t camera = GPR_U32(ctx, 5); // Sky callback ABI: a1 = camera.
            uint32_t raster = 0u;
            uint8_t rasterType = 0xFFu;
            if (guestRange(camera, kCameraRasterOffset + sizeof(uint32_t)) &&
                read32(rdram, camera + kCameraRasterOffset, raster) && raster &&
                guestRange(raster, kRasterTypeOffset + 1u))
                rasterType = static_cast<uint8_t>(rdram[raster + kRasterTypeOffset] & 0x7u);

            // Type 5 is the guest-managed camera/GS target path. Keep it on the
            // original callback until a host render target can replace it.
            if (!raster || rasterType > 4u)
            {
                // Native hooks must not record this pass: it renders into a GS
                // resident target the host never presents.
                g_inTextureCamera.store(true, std::memory_order_relaxed);
                recordCameraPath(true, false, rasterType, camera, raster);
                if (GuestFunction original = originalFor(kSkyCameraBeginUpdate))
                    original(rdram, ctx, runtime);
                return;
            }

            // A native display camera beginning proves any earlier texture
            // camera ended, even if its EndUpdate never reached this hook.
            g_inTextureCamera.store(false, std::memory_order_relaxed);
            if (!applyNativeBeginState(rdram, GPR_U32(ctx, 28), camera, raster))
            {
                recordCameraPath(true, false, rasterType, camera, raster);
                if (GuestFunction original = originalFor(kSkyCameraBeginUpdate))
                    original(rdram, ctx, runtime);
                return;
            }

            recordCameraPath(true, true, rasterType, camera, raster);

            // RwCameraBeginUpdate checks v0 for success after this indirect
            // callback. The original successful path returns 1.
            SET_GPR_U32(ctx, 2, 1u);
            ctx->pc = GPR_U32(ctx, 31);
        }

        void skyCameraEndUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;

            const uint32_t camera = GPR_U32(ctx, 5); // Sky callback ABI: a1 = camera.
            uint32_t raster = 0u;
            uint8_t rasterType = 0xFFu;
            if (guestRange(camera, kCameraRasterOffset + sizeof(uint32_t)) &&
                read32(rdram, camera + kCameraRasterOffset, raster) && raster &&
                guestRange(raster, kRasterTypeOffset + 1u))
                rasterType = static_cast<uint8_t>(rdram[raster + kRasterTypeOffset] & 0x7u);

            // EndUpdate's type-5 branch emits TEXFLUSH and invokes the Sky
            // raster-lock callback. Preserve that path until a host target can
            // replace both effects.
            if (!raster || rasterType > 4u)
            {
                g_inTextureCamera.store(false, std::memory_order_relaxed);
                recordCameraPath(false, false, rasterType, camera, raster);
                if (GuestFunction original = originalFor(kSkyCameraEndUpdate))
                    original(rdram, ctx, runtime);
                return;
            }

            if (!applyNativeEndState(rdram, GPR_U32(ctx, 28), camera, raster))
            {
                recordCameraPath(false, false, rasterType, camera, raster);
                if (GuestFunction original = originalFor(kSkyCameraEndUpdate))
                    original(rdram, ctx, runtime);
                return;
            }

            recordCameraPath(false, true, rasterType, camera, raster);

            // The non-type-5 path returns 1 whether or not the ready flag was
            // set. A pending flag is cleared before returning when present.
            SET_GPR_U32(ctx, 2, 1u);
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    bool inTextureCamera()
    {
        return g_inTextureCamera.load(std::memory_order_relaxed);
    }

    bool applyNativeBeginState(uint8_t *rdram, uint32_t gp, uint32_t camera,
                               uint32_t raster)
    {
        if (gp < 0x4360u || !guestRange(gp - 0x4360u, 0x140u))
            return false;
        uint32_t type = 0u;
        if (!guestRange(camera, kCameraRasterOffset + sizeof(uint32_t)) ||
            !read32(rdram, camera + kCameraRasterOffset, raster) || !raster ||
            !guestRange(raster, kRasterTypeOffset + sizeof(uint32_t)) ||
            !read32(rdram, raster + kRasterTypeOffset, type))
            return false;
        type &= 0x7u;
        if (type > 4u)
            return false;

        // Mirror the bookkeeping writes on the non-type-5 path. The GS packet
        // buffer itself (gp-0x42FC) is deliberately untouched.
        uint32_t refresh = 0u;
        if (read32(rdram, gpAddress(gp, kGpCacheRefresh), refresh))
        {
            uint64_t cachedState = 0u;
            const uint32_t source = (refresh & 1u) ? 0x009649F0u : 0x00964B60u;
            if (read64(rdram, source, cachedState))
                write64(rdram, gpAddress(gp, kGpCachedTextureState), cachedState);
        }

        uint32_t cameraReady = 0u;
        if (read32(rdram, gpAddress(gp, kGpCameraReady), cameraReady) && !cameraReady)
            write32(rdram, gpAddress(gp, kGpCameraReady), 1u);
        write32(rdram, gpAddress(gp, kGpCameraPending), 0u);

        uint64_t gsState = 0u;
        uint32_t enabled = 0u, hi = 0u, lo = 0u, alpha = 0u;
        if (read32(rdram, gpAddress(gp, kGpGsStateEnabled), enabled) && (enabled & 0xFFu) &&
            read32(rdram, gpAddress(gp, kGpGsStateHigh), hi) &&
            read32(rdram, gpAddress(gp, kGpGsStateLow), lo) &&
            read32(rdram, gpAddress(gp, kGpGsStateAlpha), alpha))
        {
            // LBU operands are unsigned; DSLL32/DSRA32 sign-extends the
            // resulting 32-bit word, not an 8-bit channel.
            gsState = (hi ^ (lo & 1u) ^ alpha) & 0xFFu;
        }

        uint64_t stateMiddle = 0u, stateHigh = 0u;
        read64(rdram, gpAddress(gp, 0x4270u), stateMiddle);
        read64(rdram, gpAddress(gp, 0x4268u), stateHigh);
        gsState = (gsState << 35u) | (static_cast<uint64_t>(stateMiddle) << 4u) |
                  (static_cast<uint64_t>(stateHigh) << 36u);
        write64(rdram, gpAddress(gp, kGpGsState), gsState);

        uint32_t begins = 0u;
        if (read32(rdram, gpAddress(gp, kGpCameraBeginCount), begins))
            write32(rdram, gpAddress(gp, kGpCameraBeginCount), begins + 1u);
        return true;
    }

    bool install(PS2Runtime &runtime)
    {
        struct HookSpec
        {
            uint32_t address;
            GuestFunction wrapper;
        };
        constexpr HookSpec hooks[] = {
            {kSkyCameraBeginUpdate, &skyCameraBeginUpdateHook},
            {kSkyCameraEndUpdate, &skyCameraEndUpdateHook},
        };

        bool allInstalled = true;
        for (const HookSpec &hook : hooks)
        {
            GuestFunction current = runtime.lookupFunction(hook.address);
            if (!current)
            {
                allInstalled = false;
                continue;
            }
            if (current == hook.wrapper)
            {
                if (g_originals.find(hook.address) == g_originals.end())
                    allInstalled = false;
                continue;
            }
            g_originals[hook.address] = current;
            if (!runtime.replaceFunction(hook.address, hook.wrapper))
                allInstalled = false;
        }
        return allInstalled;
    }

    bool applyNativeEndState(uint8_t *rdram, uint32_t gp, uint32_t camera,
                             uint32_t raster)
    {
        if (gp < 0x4360u || !guestRange(gp - 0x4360u, 0x140u) ||
            !guestRange(camera, kCameraRasterOffset + sizeof(uint32_t)) ||
            !read32(rdram, camera + kCameraRasterOffset, raster) || !raster ||
            !guestRange(raster, kRasterTypeOffset + 1u))
            return false;

        const uint8_t type = static_cast<uint8_t>(rdram[raster + kRasterTypeOffset] & 0x7u);
        if (type > 4u)
            return false;

        // In the original non-type-5 route, ready==0 returns 1 directly;
        // ready!=0 reaches 4D8F28 and clears it before the same return.
        uint32_t ready = 0u;
        if (!read32(rdram, gpAddress(gp, kGpCameraReady), ready))
            return false;
        if (ready != 0u)
            write32(rdram, gpAddress(gp, kGpCameraReady), 0u);
        return true;
    }

    void logCounters()
    {
        std::fprintf(stderr,
                     "[p3fes-native-camera] begin native=%llu fallback=%llu; end native=%llu fallback=%llu; first native type=%u camera=%08X raster=%08X; first fallback type=%u camera=%08X raster=%08X\n",
                     static_cast<unsigned long long>(g_counters.beginNative),
                     static_cast<unsigned long long>(g_counters.beginFallback),
                     static_cast<unsigned long long>(g_counters.endNative),
                     static_cast<unsigned long long>(g_counters.endFallback),
                     g_counters.firstNativeType, g_counters.firstNativeCamera,
                     g_counters.firstNativeRaster, g_counters.firstFallbackType,
                     g_counters.firstFallbackCamera, g_counters.firstFallbackRaster);

        for (uint32_t type = 0u; type < g_counters.beginNativeByType.size(); ++type)
        {
            const uint64_t bn = g_counters.beginNativeByType[type];
            const uint64_t bf = g_counters.beginFallbackByType[type];
            const uint64_t en = g_counters.endNativeByType[type];
            const uint64_t ef = g_counters.endFallbackByType[type];
            if (bn || bf || en || ef)
                std::fprintf(stderr,
                             "[p3fes-native-camera] type=%u begin native=%llu fallback=%llu; end native=%llu fallback=%llu\n",
                             type, static_cast<unsigned long long>(bn),
                             static_cast<unsigned long long>(bf),
                             static_cast<unsigned long long>(en),
                             static_cast<unsigned long long>(ef));
        }
    }
}
