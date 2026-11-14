#include "game_overrides/p3fes_native_bgm.h"
#include "game_overrides/p3fes_native_bgm_hooks.h"
#include "ps2_runtime.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x00100000u;
    constexpr uint32_t kTableEnd = 0x00550000u;
    constexpr uint32_t kReturnPc = 0x000FF000u;
    constexpr uint32_t kPlay = 0x001099D0u;
    constexpr uint32_t kStop = 0x00108F70u;
    constexpr uint32_t kStopFade = 0x00108FD0u;
    constexpr uint32_t kGetCurrent = 0x00108E50u;
    constexpr uint32_t kGetBgmStatus = 0x001099A0u;
    constexpr uint32_t kAdxStop = 0x0054D100u;
    constexpr uint32_t kAdxSetPause = 0x0054D118u;
    constexpr uint32_t kAdxGetPause = 0x0054D130u;
    constexpr uint32_t kAdxGetStatus = 0x0054D148u;
    constexpr uint32_t kAdxSetVolume = 0x0054D1A8u;
    constexpr uint32_t kBgmActive = 0x007E4430u;
    constexpr uint32_t kBgmFadeActive = 0x007E4434u;
    constexpr uint32_t kBgmCurrentId = 0x007E4438u;
    constexpr uint32_t kBgmQueued = 0x007E443Cu;
    constexpr uint32_t kBgmHandle = 0x007E4440u;
    constexpr uint32_t kBgmTransition = 0x007E4444u;
    constexpr uint32_t kBgmPreviousId = 0x007E4560u;
    constexpr uint32_t kNameTable = 0x005D4014u;
    constexpr uint32_t kNameAddress = 0x005F0000u;
    constexpr uint32_t kHandleValue = 0x00123450u;
    constexpr uint32_t kOtherHandle = 0x00123460u;
    constexpr uint32_t kOriginalMarker = 0x0BADC0DEu;

    std::vector<uint8_t> g_rdram(0x02000000u, 0u);
    uint32_t g_originalCalls[5]{};
    bool g_startSucceeds = true;
    bool g_backendPlaying = false;
    bool g_backendPaused = false;
    float g_backendVolume = 1.0f;
    float g_startVolumeSeen = 1.0f;
    float g_fadeSeconds = -1.0f;
    std::string g_startedPath;
    uint32_t g_startCalls = 0u;
    uint32_t g_stopCalls = 0u;
    uint32_t g_pauseCalls = 0u;
    uint32_t g_volumeCalls = 0u;

    void putU32(uint32_t address, uint32_t value)
    {
        std::memcpy(g_rdram.data() + address, &value, sizeof(value));
    }

    uint32_t getU32(uint32_t address)
    {
        uint32_t value = 0u;
        std::memcpy(&value, g_rdram.data() + address, sizeof(value));
        return value;
    }

    void putS16(uint32_t address, int16_t value)
    {
        std::memcpy(g_rdram.data() + address, &value, sizeof(value));
    }

    int16_t getS16(uint32_t address)
    {
        int16_t value = 0;
        std::memcpy(&value, g_rdram.data() + address, sizeof(value));
        return value;
    }

    uint32_t reg(const R5900Context &ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[index]));
    }

    void setReg(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void fakeOriginal(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (ctx->pc == kPlay) ++g_originalCalls[0];
        else if (ctx->pc == kStop) ++g_originalCalls[1];
        else if (ctx->pc == kStopFade) ++g_originalCalls[2];
        else if (ctx->pc == kAdxSetPause) ++g_originalCalls[3];
        else if (ctx->pc == kAdxSetVolume) ++g_originalCalls[4];
        setReg(*ctx, 2u, kOriginalMarker);
        ctx->pc = reg(*ctx, 31u);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
        {
            std::fprintf(stderr, "native BGM hooks integration test failed: %s\n", message);
            std::abort();
        }
    }

    R5900Context invoke(PS2Runtime &runtime, uint32_t address, uint32_t arg0 = 0u,
                        uint32_t arg1 = 0u)
    {
        R5900Context ctx{};
        setReg(ctx, 4u, arg0);
        setReg(ctx, 5u, arg1);
        setReg(ctx, 31u, kReturnPc);
        ctx.pc = address;
        runtime.lookupFunction(address)(g_rdram.data(), &ctx, &runtime);
        require(ctx.pc == kReturnPc, "hook/original did not return to caller");
        return ctx;
    }
}

namespace p3fes_native_bgm
{
    bool start(const std::string &path)
    {
        ++g_startCalls;
        if (!g_startSucceeds)
            return false;
        g_startedPath = path;
        g_startVolumeSeen = g_backendVolume;
        g_backendPlaying = true;
        g_backendPaused = false;
        return true;
    }

    void stop()
    {
        ++g_stopCalls;
        g_backendPlaying = false;
    }

    void fadeOut(float seconds)
    {
        g_fadeSeconds = seconds;
    }

    bool playing()
    {
        return g_backendPlaying;
    }

    void setVolume(float volume)
    {
        ++g_volumeCalls;
        g_backendVolume = volume;
    }

    void pause(bool paused)
    {
        ++g_pauseCalls;
        g_backendPaused = paused;
    }
}

int main()
{
    const uint32_t slotCount = (kTableEnd - kTableBase) / 4u;
    std::vector<PS2Runtime::RecompiledFunction> functions(slotCount, &fakeOriginal);
    ps2SetRecompiledFunctionTable({functions.data(), kTableBase, kTableEnd, slotCount});

    PS2Runtime runtime;
    const int16_t bgmId = 7;
    const uint32_t row = kNameTable + static_cast<uint32_t>(bgmId) * 12u;
    putU32(row, kNameAddress);
    std::memcpy(g_rdram.data() + kNameAddress, "music/bgmone.adx", sizeof("music/bgmone.adx"));
    putU32(kBgmHandle, kHandleValue);

    require(p3fes_native_bgm_hooks::install(runtime), "failed to install all hooks");

    // The guest may configure/mute the BGM handle before any native track is
    // active. Capture only this handle's output volume while still delegating
    // the original guest operation, then seed the backend before its start.
    invoke(runtime, kAdxSetVolume, kHandleValue, static_cast<uint32_t>(-1200));
    require(g_originalCalls[4] == 1u && g_volumeCalls == 0u,
            "pre-play BGM volume did not preserve guest handling");

    invoke(runtime, kPlay, bgmId);
    require(g_startCalls == 1u && g_startedPath == "ux0:data/p3fes/native_bgm/BGMONE.wav",
            "PlayBgm ID did not resolve to the expected native path");
    require(std::fabs(g_startVolumeSeen - std::pow(10.0f, -1200.0f / 2000.0f)) < 0.00001f &&
            g_volumeCalls == 1u,
            "pre-play BGM output volume was not applied to the backend");
    require(getU32(kBgmActive) == 1u && getU32(kBgmFadeActive) == 0u &&
            getS16(kBgmCurrentId) == bgmId && getS16(kBgmQueued) == 1 &&
            getS16(kBgmTransition) == 0 && getS16(kBgmPreviousId) == -1,
            "native PlayBgm did not mirror the guest state fields");
    invoke(runtime, kPlay, bgmId);
    require(g_startCalls == 1u, "same active BGM ID restarted the backend");
    require(reg(invoke(runtime, kGetCurrent), 2u) == static_cast<uint32_t>(bgmId) &&
            reg(invoke(runtime, kGetBgmStatus), 2u) == static_cast<uint32_t>(bgmId),
            "native current/status query did not report the active ID");

    invoke(runtime, kStop);
    require(g_stopCalls == 1u && !g_backendPlaying && getU32(kBgmActive) == 0u &&
            getS16(kBgmQueued) == 0 && getS16(kBgmCurrentId) == -1,
            "native StopBgm did not stop and clear guest state");

    invoke(runtime, kPlay, bgmId);
    require(g_startCalls == 2u, "BGM did not restart after explicit stop");
    invoke(runtime, kAdxSetPause, kHandleValue, 1u);
    require(g_backendPaused && g_pauseCalls == 1u && reg(invoke(runtime, kAdxGetPause, kHandleValue), 2u) == 1u,
            "ADX pause/status hooks did not use the native backend for the BGM handle");
    invoke(runtime, kAdxSetVolume, kHandleValue, static_cast<uint32_t>(-6000));
    require(g_volumeCalls == 3u && std::fabs(g_backendVolume - 0.001f) < 0.00001f,
            "ADX millibel output volume did not convert to linear gain");
    require(getU32(kHandleValue + 0xBCu) == static_cast<uint32_t>(-6000),
            "native output volume did not update the public ACSSND context");
    const uint32_t beforeSfxPause = g_originalCalls[3];
    invoke(runtime, kAdxSetPause, kOtherHandle, 1u);
    const uint32_t beforeSfxVolume = g_originalCalls[4];
    invoke(runtime, kAdxSetVolume, kOtherHandle, static_cast<uint32_t>(-6000));
    require(g_originalCalls[3] == beforeSfxPause + 1u && g_originalCalls[4] == beforeSfxVolume + 1u,
            "ADX hooks intercepted an SFX handle instead of delegating");
    require(reg(invoke(runtime, kAdxGetStatus, kOtherHandle), 2u) == kOriginalMarker,
            "ADX status hook intercepted an SFX handle instead of delegating");
    require(reg(invoke(runtime, kAdxGetStatus, kHandleValue), 2u) == 1u,
            "native playback status did not report the active state");

    invoke(runtime, kStopFade, 60u);
    require(std::fabs(g_fadeSeconds - 1.0f) < 0.00001f && getU32(kBgmActive) == 0u &&
            getS16(kBgmQueued) == 0 && getS16(kBgmCurrentId) == -1 &&
            reg(invoke(runtime, kGetCurrent), 2u) == 0xFFFFFFFFu,
            "StopBgmFade did not convert 60 ticks or preserve immediate guest stop state");
    require(reg(invoke(runtime, kAdxGetStatus, kHandleValue), 2u) == 1u,
            "native route did not remain active while the backend fade was playing");
    g_backendPlaying = false;
    require(reg(invoke(runtime, kAdxGetStatus, kHandleValue), 2u) == 3u &&
            g_originalCalls[3] == beforeSfxPause + 1u,
            "native EOF did not report stopped through the retained native route");

    invoke(runtime, kPlay, bgmId);
    invoke(runtime, kStopFade, 0x0000FFFFu); // Guest code sign-extends the low 16 bits.
    require(g_fadeSeconds == 0.0f,
            "StopBgmFade did not sign-extend its 16-bit fade argument");
    invoke(runtime, kPlay, bgmId);

    // After EOF, native routing remains until a new guest fallback is needed.
    // A missing replacement must stop the currently native-owned track before
    // handing playback back to the original guest path.
    invoke(runtime, kPlay, bgmId);
    const int16_t missingId = 8;
    const uint32_t missingRow = kNameTable + static_cast<uint32_t>(missingId) * 12u;
    const uint32_t missingNameAddress = 0x005F0100u;
    putU32(missingRow, missingNameAddress);
    std::memcpy(g_rdram.data() + missingNameAddress, "music/missing.adx", sizeof("music/missing.adx"));
    g_startSucceeds = false;
    invoke(runtime, kPlay, missingId);
    require(g_originalCalls[0] == 1u && !g_backendPlaying && getU32(kBgmActive) == 0u &&
            getS16(kBgmQueued) == 0 && getS16(kBgmCurrentId) == -1,
            "missing WAV did not cleanly fall back to the original guest play path");
    const uint32_t beforeGuestOwned = g_startCalls;
    putU32(kBgmActive, 1u);
    putS16(kBgmCurrentId, 11);
    invoke(runtime, kPlay, bgmId);
    require(g_startCalls == beforeGuestOwned && g_originalCalls[0] == 2u,
            "native backend displaced an already-active guest-owned BGM");
}
