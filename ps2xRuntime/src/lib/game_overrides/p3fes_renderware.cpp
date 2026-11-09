// Persona 3 FES RenderWare integration point.
//
// The guest layouts and RpClumpRender walk match RenderWare 3.7 as recovered
// by Raikaru/Persona4-Decompilation. This wrapper is opt-in and always calls
// the original recompiled function, so the existing GS remains the
// correctness fallback while native drawing is brought up incrementally.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint32_t kRpClumpRender = 0x00491630u;
    constexpr uint32_t kAtomicListOffset = 0x08u;
    constexpr uint32_t kAtomicLinkOffset = 0x40u;
    constexpr uint32_t kAtomicFlagsOffset = 0x02u;
    constexpr uint32_t kAtomicFrameOffset = 0x04u;
    constexpr uint32_t kAtomicGeometryOffset = 0x0Cu;
    constexpr uint8_t kAtomicRenderFlag = 0x04u;
    constexpr uint32_t kMaxAtomicsPerClump = 4096u;

    std::atomic<PS2Runtime::RecompiledFunction> g_originalRpClumpRender{nullptr};
    std::atomic<bool> g_diagEnabled{false};
    std::atomic<bool> g_diagPrinted{false};

    template <typename T>
    bool readGuest(const uint8_t *rdram, uint32_t address, T &value)
    {
        if (!rdram || address > PS2_RAM_SIZE - sizeof(T))
            return false;
        std::memcpy(&value, rdram + address, sizeof(T));
        return true;
    }

    bool guestPointer(uint32_t address, uint32_t size = 1u)
    {
        return size <= PS2_RAM_SIZE && address <= PS2_RAM_SIZE - size;
    }

    struct ClumpSummary
    {
        uint32_t address = 0u;
        uint32_t atomics = 0u;
        uint32_t renderable = 0u;
        uint32_t frames = 0u;
        uint32_t geometries = 0u;
    };

    bool inspectClump(const uint8_t *rdram, uint32_t clumpAddress, ClumpSummary &summary)
    {
        if (!guestPointer(clumpAddress, kAtomicListOffset + 8u))
            return false;

        const uint32_t sentinel = clumpAddress + kAtomicListOffset;
        uint32_t link = 0u;
        if (!readGuest(rdram, sentinel, link))
            return false;

        summary.address = clumpAddress;
        while (link != sentinel)
        {
            if (++summary.atomics > kMaxAtomicsPerClump || link < kAtomicLinkOffset || !guestPointer(link, 8u))
                return false;

            const uint32_t atomic = link - kAtomicLinkOffset;
            uint8_t flags = 0u;
            uint32_t frame = 0u;
            uint32_t geometry = 0u;
            uint32_t next = 0u;
            if (!readGuest(rdram, atomic + kAtomicFlagsOffset, flags) ||
                !readGuest(rdram, atomic + kAtomicFrameOffset, frame) ||
                !readGuest(rdram, atomic + kAtomicGeometryOffset, geometry) ||
                !readGuest(rdram, link, next))
                return false;

            if ((flags & kAtomicRenderFlag) != 0u)
                ++summary.renderable;
            if (guestPointer(frame, 1u))
                ++summary.frames;
            if (guestPointer(geometry, 1u))
                ++summary.geometries;
            link = next;
        }
        return true;
    }

    void p3fesRpClumpRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx && ctx->pc == kRpClumpRender)
        {
            const uint32_t clump = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
            ClumpSummary summary;
            if (inspectClump(rdram, clump, summary) && g_diagEnabled.load(std::memory_order_relaxed) &&
                !g_diagPrinted.exchange(true, std::memory_order_relaxed))
            {
                std::fprintf(stderr,
                             "[p3fes-rw] RpClumpRender clump=0x%08X atomics=%u renderable=%u frames=%u geometries=%u\n",
                             summary.address, summary.atomics, summary.renderable, summary.frames, summary.geometries);
            }
        }

        if (PS2Runtime::RecompiledFunction original = g_originalRpClumpRender.load(std::memory_order_acquire))
            original(rdram, ctx, runtime);
    }

    void applyP3fesRenderWare(PS2Runtime &runtime)
    {
        const char *native = std::getenv("P3FES_NATIVE_RW");
        const char *diag = std::getenv("P3FES_RW_HOOK_DIAG");
        const bool nativeEnabled = native && *native && *native != '0';
        const bool diagEnabled = diag && *diag && *diag != '0';
        if (!nativeEnabled && !diagEnabled)
            return;

        PS2Runtime::RecompiledFunction original = runtime.lookupFunction(kRpClumpRender);
        if (!original || original == &p3fesRpClumpRender)
            return;

        g_diagEnabled.store(diagEnabled, std::memory_order_relaxed);
        g_diagPrinted.store(false, std::memory_order_relaxed);
        g_originalRpClumpRender.store(original, std::memory_order_release);
        const bool replaced = runtime.replaceFunction(kRpClumpRender, &p3fesRpClumpRender);
        if (diagEnabled)
            std::fprintf(stderr, "[p3fes-rw] RpClumpRender hook %s at 0x%08X\n",
                         replaced ? "installed" : "failed", kRpClumpRender);
    }
}

// CRC32 is for the USA v1.00 SLUS_216.21 used by this port (SHA-1
// 3929cd7c02be944f25ec6b924e5f1eab9bc5e9cb).
PS2_REGISTER_GAME_OVERRIDE("p3fes-renderware",
                           "SLUS_216.21",
                           0x00100008u,
                           0x9B2AF129u,
                           applyP3fesRenderWare)
