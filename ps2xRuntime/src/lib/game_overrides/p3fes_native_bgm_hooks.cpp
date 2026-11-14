#include "p3fes_native_bgm_hooks.h"

#include "p3fes_native_bgm.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace p3fes_native_bgm_hooks
{
    namespace
    {
        constexpr uint32_t kBgmActive = 0x007E4430u;
        constexpr uint32_t kBgmFadeActive = 0x007E4434u;
        constexpr uint32_t kBgmCurrentId = 0x007E4438u;
        constexpr uint32_t kBgmQueued = 0x007E443Cu;
        constexpr uint32_t kBgmPreviousId = 0x007E4560u;
        constexpr uint32_t kBgmTransition = 0x007E4444u;
        constexpr uint32_t kBgmHandle = 0x007E4440u;
        constexpr uint32_t kBgmNameTable = 0x005D4014u;
        constexpr uint32_t kMaxBgmIds = 256u;

        constexpr uint32_t kPlayBgm = 0x001099D0u;
        constexpr uint32_t kStopBgm = 0x00108F70u;
        constexpr uint32_t kStopBgmFade = 0x00108FD0u;
        constexpr uint32_t kGetCurrentBgm = 0x00108E50u;
        constexpr uint32_t kGetBgmStatus = 0x001099A0u;

        // These ADX wrappers are called by the BGM state/fade routines and
        // also by SFX. They are intercepted only for the real BGM handle.
        constexpr uint32_t kAdxStop = 0x0054D100u;
        constexpr uint32_t kAdxSetPause = 0x0054D118u;
        constexpr uint32_t kAdxGetPause = 0x0054D130u;
        constexpr uint32_t kAdxGetStatus = 0x0054D148u;
        constexpr uint32_t kAdxSetOutputVolume = 0x0054D1A8u;

        using GuestFunction = PS2Runtime::RecompiledFunction;

        std::unordered_map<uint32_t, GuestFunction> g_originals;
        uint64_t g_nativeStarts = 0u;
        uint64_t g_guestStartFallbacks = 0u;
        uint64_t g_nativeStops = 0u;
        uint64_t g_nativeFades = 0u;
        uint64_t g_nativeVolumeUpdates = 0u;
        uint64_t g_nativePauseUpdates = 0u;

        // All guest state changes happen on the EE worker. Backend playback
        // runs independently, but ownership changes are made only by hooks.
        bool g_nativeOwned = false;
        // Keep routing ADX calls for the BGM handle away from CRI after native
        // EOF or StopFade. Unsupported guest fallback explicitly clears it.
        bool g_nativeRoute = false;
        bool g_nativePaused = false;
        int16_t g_nativeId = -1;
        bool g_nativeVolumeKnown = false;
        float g_nativeVolume = 1.0f;

        bool readGuestU32(const uint8_t *rdram, uint32_t address, uint32_t &value)
        {
            if (!rdram || address > PS2_RAM_SIZE - sizeof(value))
                return false;
            std::memcpy(&value, rdram + address, sizeof(value));
            return true;
        }

        bool writeGuestU32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (!rdram || address > PS2_RAM_SIZE - sizeof(value))
                return false;
            std::memcpy(rdram + address, &value, sizeof(value));
            return true;
        }

        bool writeGuestS16(uint8_t *rdram, uint32_t address, int16_t value)
        {
            if (!rdram || address > PS2_RAM_SIZE - sizeof(value))
                return false;
            std::memcpy(rdram + address, &value, sizeof(value));
            return true;
        }

        int16_t readGuestS16(const uint8_t *rdram, uint32_t address, int16_t fallback)
        {
            int16_t value = fallback;
            if (!rdram || address > PS2_RAM_SIZE - sizeof(value))
                return fallback;
            std::memcpy(&value, rdram + address, sizeof(value));
            return value;
        }

        std::string readGuestString(const uint8_t *rdram, uint32_t address)
        {
            std::string value;
            if (!rdram || address >= PS2_RAM_SIZE)
                return value;
            for (uint32_t i = 0u; i < 128u && address + i < PS2_RAM_SIZE; ++i)
            {
                const char ch = static_cast<char>(rdram[address + i]);
                if (ch == '\0')
                    break;
                if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) > 0x7Eu)
                    return {};
                value.push_back(ch);
            }
            return value;
        }

        std::string bgmSourceName(const uint8_t *rdram, int16_t id)
        {
            if (id < 0 || static_cast<uint32_t>(id) >= kMaxBgmIds)
                return {};

            const uint32_t row = kBgmNameTable + static_cast<uint32_t>(id) * 12u;
            uint32_t nameAddress = 0u;
            if (!readGuestU32(rdram, row, nameAddress))
                return {};
            return readGuestString(rdram, nameAddress);
        }

        std::string nativePathForName(const std::string &sourceName, bool fromCvm)
        {
            const size_t slash = sourceName.find_last_of("/\\");
            std::string stem = sourceName.substr(slash == std::string::npos ? 0u : slash + 1u);
            if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos)
                stem.resize(dot);

            for (char &ch : stem)
            {
                if (ch >= 'a' && ch <= 'z')
                    ch = static_cast<char>(ch - ('a' - 'A'));
                else if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
                    ch = '_';
            }

            if (stem.empty())
                return {};
            // Preferred source: the ADX inside the game's own BGM.CVM, decoded natively. A prepared WAV is the fallback.
#if defined(PLATFORM_VITA)
            if (fromCvm)
                return "ux0:data/p3fes/BGM.CVM|" + stem + ".ADX";
            return "ux0:data/p3fes/native_bgm/" + stem + ".wav";
#else
            if (fromCvm)
                return "BGM.CVM|" + stem + ".ADX";
            return "native_bgm/" + stem + ".wav";
#endif
        }

        void setNativeGuestState(uint8_t *rdram, int16_t id)
        {
            uint32_t wasActive = 0u;
            readGuestU32(rdram, kBgmActive, wasActive);
            const int16_t previous = wasActive ? readGuestS16(rdram, kBgmCurrentId, -1) : -1;

            writeGuestU32(rdram, kBgmFadeActive, 0u);
            writeGuestU32(rdram, kBgmActive, 1u);
            writeGuestS16(rdram, kBgmQueued, 1);
            writeGuestS16(rdram, kBgmPreviousId, previous);
            writeGuestS16(rdram, kBgmCurrentId, id);
            writeGuestS16(rdram, kBgmTransition, 0);
        }

        void clearNativeGuestState(uint8_t *rdram)
        {
            writeGuestU32(rdram, kBgmActive, 0u);
            writeGuestS16(rdram, kBgmQueued, 0);
            writeGuestS16(rdram, kBgmCurrentId, -1);
        }

        // If a track reaches EOF naturally, release native ownership and keep
        // the guest-facing state consistent with the status callers.
        bool nativeIsPlaying(uint8_t *rdram)
        {
            if (!g_nativeOwned)
                return false;
            if (p3fes_native_bgm::playing())
                return true;

            g_nativeOwned = false;
            g_nativePaused = false;
            g_nativeId = -1;
            clearNativeGuestState(rdram);
            return false;
        }

        GuestFunction originalFor(uint32_t address)
        {
            const auto found = g_originals.find(address);
            return found == g_originals.end() ? nullptr : found->second;
        }

        void callOriginal(uint32_t address, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GuestFunction original = originalFor(address))
                original(rdram, ctx, runtime);
        }

        void returnToCaller(R5900Context *ctx)
        {
            ctx->pc = GPR_U32(ctx, 31);
        }

        void playBgmHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;

            const int16_t id = static_cast<int16_t>(GPR_U32(ctx, 4));
            const std::string sourceName = bgmSourceName(rdram, id);
            std::fprintf(stderr, "[native-bgm] request id=%d source=%s ra=%08X\n",
                         static_cast<int>(id), sourceName.empty() ? "<unknown>" : sourceName.c_str(),
                         GPR_U32(ctx, 31));

            uint32_t guestActive = 0u;
            readGuestU32(rdram, kBgmActive, guestActive);
            const int16_t guestId = readGuestS16(rdram, kBgmCurrentId, -1);

            // A guest-owned track stays guest-owned until its own Stop call.
            // This avoids abruptly switching audio backends during transitions.
            if (!g_nativeOwned && guestActive != 0u)
            {
                ++g_guestStartFallbacks;
                callOriginal(kPlayBgm, rdram, ctx, runtime);
                return;
            }

            if (g_nativeOwned && guestActive != 0u && guestId == id && nativeIsPlaying(rdram))
            {
                returnToCaller(ctx);
                return;
            }

            std::string path = nativePathForName(sourceName, true);
            if (!path.empty())
            {
                if (g_nativeVolumeKnown)
                    p3fes_native_bgm::setVolume(g_nativeVolume);
            }
            bool nativeStarted = !path.empty() && p3fes_native_bgm::start(path);
            if (!nativeStarted && !path.empty())
            {
                path = nativePathForName(sourceName, false);
                nativeStarted = p3fes_native_bgm::start(path);
            }
            if (nativeStarted)
            {
                g_nativeOwned = true;
                g_nativeRoute = true;
                g_nativePaused = false;
                g_nativeId = id;
                setNativeGuestState(rdram, id);
                ++g_nativeStarts;
                std::fprintf(stderr, "[native-bgm] started id=%d path=%s volume=%.5f\n",
                             static_cast<int>(id), path.c_str(), g_nativeVolume);
                returnToCaller(ctx);
                return;
            }

            std::fprintf(stderr, "[native-bgm] unavailable id=%d path=%s; using guest audio\n",
                         static_cast<int>(id), path.empty() ? "<invalid>" : path.c_str());
            if (g_nativeRoute)
            {
                // A failed replacement must not leave the old native stream
                // playing underneath the guest fallback.
                p3fes_native_bgm::stop();
                g_nativeOwned = false;
                g_nativeRoute = false;
                g_nativePaused = false;
                g_nativeId = -1;
                clearNativeGuestState(rdram);
            }
            ++g_guestStartFallbacks;
            callOriginal(kPlayBgm, rdram, ctx, runtime);
        }

        void stopBgmHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_nativeRoute)
            {
                if (g_nativeOwned)
                    p3fes_native_bgm::stop();
                g_nativeOwned = false;
                g_nativePaused = false;
                g_nativeId = -1;
                clearNativeGuestState(rdram);
                ++g_nativeStops;
                returnToCaller(ctx);
                return;
            }
            callOriginal(kStopBgm, rdram, ctx, runtime);
        }

        void stopBgmFadeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_nativeRoute)
            {
                // Current mapping assumes the guest fade value is in 60 Hz
                // update ticks; generated callers use values such as 10, 30,
                // 60 and 90. The guest-side +0xA8 consumer/frequency still
                // needs confirmation. Preserve the visible stop state now
                // while the native stream fades asynchronously.
                const int32_t ticks = static_cast<int16_t>(GPR_U32(ctx, 4));
                const float seconds = std::max(0.0f, static_cast<float>(ticks) / 60.0f);
                if (g_nativeOwned)
                    p3fes_native_bgm::fadeOut(seconds);
                clearNativeGuestState(rdram);
                ++g_nativeFades;
                returnToCaller(ctx);
                return;
            }
            callOriginal(kStopBgmFade, rdram, ctx, runtime);
        }

        void getCurrentBgmHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_nativeRoute)
            {
                const bool playing = nativeIsPlaying(rdram);
                uint32_t active = 0u;
                readGuestU32(rdram, kBgmActive, active);
                SET_GPR_S32(ctx, 2, playing && active ? g_nativeId : -1);
                returnToCaller(ctx);
                return;
            }
            callOriginal(kGetCurrentBgm, rdram, ctx, runtime);
        }

        void getBgmStatusHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (g_nativeRoute)
            {
                const bool playing = nativeIsPlaying(rdram);
                uint32_t active = 0u;
                readGuestU32(rdram, kBgmActive, active);
                SET_GPR_S32(ctx, 2, playing && active ? g_nativeId : -1);
                returnToCaller(ctx);
                return;
            }
            callOriginal(kGetBgmStatus, rdram, ctx, runtime);
        }

        bool isNativeBgmHandle(const uint8_t *rdram, const R5900Context *ctx)
        {
            uint32_t handle = 0u;
            return g_nativeRoute && ctx && readGuestU32(rdram, kBgmHandle, handle) &&
                   handle != 0u && GPR_U32(ctx, 4) == handle;
        }

        void adxStopHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (isNativeBgmHandle(rdram, ctx))
            {
                if (g_nativeOwned)
                    p3fes_native_bgm::stop();
                g_nativeOwned = false;
                g_nativePaused = false;
                g_nativeId = -1;
                clearNativeGuestState(rdram);
                ++g_nativeStops;
                returnToCaller(ctx);
                return;
            }
            callOriginal(kAdxStop, rdram, ctx, runtime);
        }

        void adxSetPauseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (isNativeBgmHandle(rdram, ctx))
            {
                g_nativePaused = GPR_U32(ctx, 5) != 0u;
                if (g_nativeOwned)
                    p3fes_native_bgm::pause(g_nativePaused);
                ++g_nativePauseUpdates;
                returnToCaller(ctx);
                return;
            }
            callOriginal(kAdxSetPause, rdram, ctx, runtime);
        }

        void adxGetPauseHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (isNativeBgmHandle(rdram, ctx))
            {
                SET_GPR_S32(ctx, 2, g_nativePaused ? 1 : 0);
                returnToCaller(ctx);
                return;
            }
            callOriginal(kAdxGetPause, rdram, ctx, runtime);
        }

        void adxGetStatusHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            if (isNativeBgmHandle(rdram, ctx))
            {
                // ACSSND status 3 is the terminal/stopped state checked by
                // H_Snd_00109180; 1 is the active state.
                SET_GPR_S32(ctx, 2, nativeIsPlaying(rdram) ? 1 : 3);
                returnToCaller(ctx);
                return;
            }
            callOriginal(kAdxGetStatus, rdram, ctx, runtime);
        }

        void adxSetOutputVolumeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;
            uint32_t bgmHandle = 0u;
            const bool isBgmHandle = readGuestU32(rdram, kBgmHandle, bgmHandle) &&
                                     bgmHandle != 0u && GPR_U32(ctx, 4) == bgmHandle;
            if (isBgmHandle)
            {
                // ACSSND output volume is in hundredths of a decibel (mB).
                const float millibels = static_cast<float>(GPR_S32(ctx, 5));
                const float linear = std::clamp(std::pow(10.0f, millibels / 2000.0f), 0.0f, 1.0f);
                g_nativeVolume = linear;
                g_nativeVolumeKnown = true;
                if (g_nativeRoute)
                {
                    p3fes_native_bgm::setVolume(linear);
                // ACSSND_SetOutVol stores the requested mB in handle+0xBC
                // before submitting its driver command. Preserve that public
                // context state without sending another IOP audio command.
                const uint32_t handle = GPR_U32(ctx, 4);
                if (handle <= PS2_RAM_SIZE - 0xC0u)
                    writeGuestU32(rdram, handle + 0xBCu, GPR_U32(ctx, 5));
                    ++g_nativeVolumeUpdates;
                    returnToCaller(ctx);
                    return;
                }
            }
            callOriginal(kAdxSetOutputVolume, rdram, ctx, runtime);
        }

        struct HookSpec
        {
            uint32_t address;
            GuestFunction wrapper;
        };

        constexpr HookSpec kHooks[] = {
            {kPlayBgm, &playBgmHook},
            {kStopBgm, &stopBgmHook},
            {kStopBgmFade, &stopBgmFadeHook},
            {kGetCurrentBgm, &getCurrentBgmHook},
            {kGetBgmStatus, &getBgmStatusHook},
            {kAdxStop, &adxStopHook},
            {kAdxSetPause, &adxSetPauseHook},
            {kAdxGetPause, &adxGetPauseHook},
            {kAdxGetStatus, &adxGetStatusHook},
            {kAdxSetOutputVolume, &adxSetOutputVolumeHook},
        };
    }

    bool install(PS2Runtime &runtime)
    {
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

    Counters counters()
    {
        return {
            g_nativeStarts,
            g_guestStartFallbacks,
            g_nativeStops,
            g_nativeFades,
            g_nativeVolumeUpdates,
            g_nativePauseUpdates,
        };
    }

    void resetCounters()
    {
        g_nativeStarts = 0u;
        g_guestStartFallbacks = 0u;
        g_nativeStops = 0u;
        g_nativeFades = 0u;
        g_nativeVolumeUpdates = 0u;
        g_nativePauseUpdates = 0u;
    }

    void logCounters()
    {
        const Counters value = counters();
        std::fprintf(stderr,
                     "[native-bgm] starts=%llu guest-fallbacks=%llu stops=%llu fades=%llu volume=%llu pause=%llu owner=%u id=%d paused=%u\n",
                     static_cast<unsigned long long>(value.nativeStarts),
                     static_cast<unsigned long long>(value.guestStartFallbacks),
                     static_cast<unsigned long long>(value.nativeStops),
                     static_cast<unsigned long long>(value.nativeFades),
                     static_cast<unsigned long long>(value.nativeVolumeUpdates),
                     static_cast<unsigned long long>(value.nativePauseUpdates),
                     g_nativeOwned ? 1u : 0u, static_cast<int>(g_nativeId),
                     g_nativePaused ? 1u : 0u);
    }
}
