#include "p3fes_native_dialogue.h"
#include "p3fes_native_frame_cuts.h"

// What a dialogue scene costs on top of the plain lounge (Mac, deterministic probes, P3FES_DIALOGUE_DIAG=1, per game
// frame at 30 fps): the message-window update itfMesManager_003a84c0 (0x3A84C0, one call per frame, 9 windows) takes
// ~125 us in the dialogue scene (portrait + name/text box + 3-choice box) against ~2 us in the lounge, on a game thread
// that costs ~1.0 ms per frame there: +8..10 %, which is the whole +8..9 points of core the console shows.
//   text   frFont_1360 (0x3B1360, 4 runs: name, line, 3 choices... 85 glyphs per frame): ~62 us
//          per glyph: frFontRaster (0x3B38F0, ~290 MIPS instructions of float setup, a 256-byte memset, four
//          64-byte RwIm2DVertex, then RwRenderStateSet(TEXTURERASTER, raster) and RwIm2DRenderPrimitive(TRISTRIP, v, 4)
//          through function pointers) ~0.6 us. Every glyph has its own raster (a 28x28 bitmap of the font cache), so
//          consecutive glyphs cannot share a draw or a texture state; batching was tried and abandoned.
//   boxes  the translucent frames and the portrait: itfMesManager_003a90c0 -> 003aaae0 / 003aa720 -> h_maestro_001127d0
//          (0x1127D0, a 2D sprite primitive that sets ~30 render states through the pointer at 0x960090): 25 more calls
//          per frame than the lounge, ~1.7 us each, three quarters of it the dispatch of those state calls.
// The cut below removes the guest execution of the rasteriser (~19 us of the 125); see the report for what is left.
//

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <ctime>
#include <vector>
#include <utility>

namespace p3fes_native_dialogue
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;

        // ---- diagnostic: inclusive time / calls of the message-window functions --------------------------
        struct Probe
        {
            uint32_t address;
            const char *name;
        };
        constexpr Probe kProbes[] = {
            {0x00194100u, "kwlnTaskUpdateAll"}, {0x003A84C0u, "itf_3a84c0"}, {0x003A7E90u, "itf_3a7e90"},
            {0x003A8080u, "itf_3a8080"},        {0x003A90C0u, "itf_3a90c0"}, {0x003AAAE0u, "itf_3aaae0"},
            {0x003AA720u, "itf_3aa720"},        {0x003A8170u, "itf_3a8170"}, {0x003A8260u, "itf_3a8260"},
            {0x003A8350u, "itf_3a8350"},        {0x003A6380u, "itf_3a6380"}, {0x003B1300u, "frFont_1300"},
            {0x003B1330u, "frFont_1330"},       {0x003B1360u, "frFont_1360"}, {0x003B38F0u, "frFontRaster"},
            {0x001127D0u, "maestro_1127d0"},      {0x004D6B90u, "SkySetRS"},
            {0x004D7F60u, "RpSkyRSSet"},
        };
        constexpr size_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);
        GuestFunction g_probeOriginal[kProbeCount] = {};
        uint64_t g_probeCalls[kProbeCount] = {};
        uint64_t g_probeNs[kProbeCount] = {};
        std::chrono::steady_clock::time_point g_lastReport{};
        uint64_t g_lastThreadNs = 0u;

        uint64_t g_im2dCalls = 0u, g_im2dNs = 0u;

        uint64_t threadCpuNs()
        {
#if defined(CLOCK_THREAD_CPUTIME_ID)
            timespec ts{};
            clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
#else
            return 0u;
#endif
        }

        void report()
        {
            const auto now = std::chrono::steady_clock::now();
            const double wall = std::chrono::duration<double>(now - g_lastReport).count();
            const uint64_t cpu = threadCpuNs();
            std::fprintf(stderr, "[p3fes-dlg] window %.2fs thread-cpu %.1f ms/s |", wall,
                         static_cast<double>(cpu - g_lastThreadNs) / 1e6 / wall);
            const uint64_t frames = g_probeCalls[0] ? g_probeCalls[0] : 1u;
            for (size_t i = 1; i < kProbeCount; ++i)
                if (g_probeCalls[i])
                    std::fprintf(stderr, " %s=%.2f/frame %.0fus/frame", kProbes[i].name,
                                 static_cast<double>(g_probeCalls[i]) / static_cast<double>(frames),
                                 static_cast<double>(g_probeNs[i]) / 1e3 / static_cast<double>(frames));
            std::fprintf(stderr, " Im2D=%.2f/frame %.0fus/frame", static_cast<double>(g_im2dCalls) / static_cast<double>(frames),
                         static_cast<double>(g_im2dNs) / 1e3 / static_cast<double>(frames));
            g_im2dCalls = g_im2dNs = 0u;
            std::fprintf(stderr, " | frames=%llu\n", static_cast<unsigned long long>(g_probeCalls[0]));
            std::memset(g_probeCalls, 0, sizeof(g_probeCalls));
            std::memset(g_probeNs, 0, sizeof(g_probeNs));
            g_lastReport = now;
            g_lastThreadNs = cpu;
        }

        bool g_lateDone = false;
        GuestFunction g_im2dProbeOriginal = nullptr;
        void im2dProbe(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto t0 = std::chrono::steady_clock::now();
            g_im2dProbeOriginal(rdram, ctx, runtime);
            g_im2dNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
            ++g_im2dCalls;
        }

        template <size_t Index>
        void probeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const auto t0 = std::chrono::steady_clock::now();
            g_probeOriginal[Index](rdram, ctx, runtime);
            const auto t1 = std::chrono::steady_clock::now();
            ++g_probeCalls[Index];
            g_probeNs[Index] += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            if (Index == 0u && t1 - g_lastReport > std::chrono::seconds(5))
                report();
            if (Index == 0u && !g_lateDone)
            {
                g_lateDone = true;
                // hooks of other modules are installed after this one: wrap them at the first frame
                static const uint32_t late[] = {0x004EDBA0u};
                (void)late;
                if (runtime->hasFunction(0x004EDBA0u))
                {
                    g_im2dProbeOriginal = runtime->lookupFunction(0x004EDBA0u);
                    runtime->replaceFunction(0x004EDBA0u, &im2dProbe);
                }
            }
        }

        bool installOne(PS2Runtime &runtime, uint32_t address, GuestFunction hook, GuestFunction &original)
        {
            if (!runtime.hasFunction(address))
                return false;
            GuestFunction current = runtime.lookupFunction(address);
            if (!current || current == hook)
                return false;
            original = current;
            return runtime.replaceFunction(address, hook);
        }

        // ---- cut 1: glyph rasteriser frFontRaster (0x3B38F0), see the header comment ------------------------
        enum class FontMode : uint8_t { Off, On, Verify };
        FontMode g_fontMode = FontMode::Off;
        constexpr uint32_t kFontRaster = 0x003B38F0u;
        constexpr uint32_t kRsFnPtr = 0x00960090u;     // RwRenderStateSet function pointer
        constexpr uint32_t kIm2dFnPtr = 0x009600A0u;   // RwIm2DRenderPrimitive function pointer
        constexpr uint32_t kRasterZGlobal = 0x00960088u;
        constexpr uint32_t kUnitUvTable = 0x006A28D0u; // 4 x (u, v)
        constexpr uint32_t kVertexBytes = 64u;
        constexpr uint32_t kSentinel = 0xFFFFFFFCu;

        GuestFunction g_rasterOriginal = nullptr;

        struct FontCounters
        {
            uint64_t fast = 0u, alpha0 = 0u, fallback = 0u, fbPage = 0u, fbRange = 0u;
            uint64_t verifyRuns = 0u, verifyOk = 0u, verifyBad = 0u, verifyUnwound = 0u;
        } g_fc;

        std::vector<uint8_t> g_record; // verify: Im2D calls (expanded to triangle lists)
        int g_verifyEvery = 61;
        uint64_t g_verifyTick = 0u;

        inline uint32_t r32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4u);
            return v;
        }
        inline int16_t r16s(const uint8_t *rdram, uint32_t a)
        {
            int16_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 2u);
            return v;
        }
        inline float rf(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4u);
            return v;
        }
        inline void w32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, 4u); }
        inline bool inRam(uint32_t a, uint32_t size) { return a >= 0x100000u && static_cast<uint64_t>(a) + size <= PS2_RAM_SIZE; }

        // Runs a guest function (here: a pointer target of the renderer, normally a native hook) to completion on the
        // caller's context. The registers it changes (arguments, v0, ra, pc) are restored by the caller.
        bool callGuest(uint8_t *rdram, PS2Runtime *runtime, R5900Context *ctx, uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2)
        {
            if (!runtime->hasFunction(entry))
                return false;
            ctx->pc = entry;
            ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a2)));
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(kSentinel)));
            for (uint32_t resume = 0u; resume < 100000u; ++resume)
            {
                if (runtime->isStopRequested() || ctx->pc == 0u || ctx->pc == kSentinel || !runtime->hasFunction(ctx->pc))
                    break;
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            }
            return ctx->pc == kSentinel;
        }

        // The original charges its loop back edges (about 1100 cycles of the 32-cycle checkpoint kind, 120 more
        // when it draws); keep the emulated EE clock close to that. eeTryChargeCall never enters the scheduler.
        void chargeCycles(PS2Runtime *runtime, uint32_t cycles)
        {
            for (uint32_t n = 0u; n < cycles / 32u; ++n)
                if (!runtime->eeTryChargeCall(32u))
                    break;
        }

        // Returns false when the glyph must go through the original function (nothing has been changed then).
        // Everything the original does outside its own dead stack frame is reproduced in the same order:
        //   float setup (x/16, y/8, 1/camera.z with the FCR31 flags), [page+0] |= 1, nothing more when the alpha is 0,
        //   the four vertices, RwRenderStateSet(TEXTURERASTER, raster), RwIm2DRenderPrimitive(TRISTRIP, v, 4), v0 = 0.
        bool rasterFast(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t glyph = getRegU32(ctx, 6);
            const uint64_t page = GPR_U64(ctx, 7);
            if (!inRam(glyph, 0x20u))
            {
                ++g_fc.fbRange;
                return false;
            }
            const uint32_t pageRef = r32(rdram, glyph + 0x1Cu);
            if (!inRam(pageRef, 0x10u))
            {
                ++g_fc.fbRange;
                return false;
            }
            const uint32_t desc = r32(rdram, pageRef + 0xCu);
            if (!inRam(desc, 0x18u))
            {
                ++g_fc.fbRange;
                return false;
            }
            if (static_cast<int64_t>(r16s(rdram, desc + 8u)) != static_cast<int64_t>(page))
            {
                ++g_fc.fbPage; // another font page: the original loads it (0x3B0430, 0x3B4580)
                return false;
            }
            const uint32_t gp = getRegU32(ctx, 28);
            const uint32_t camera = r32(rdram, gp - 0x4C30u);
            const uint32_t rsFn = r32(rdram, kRsFnPtr), im2dFn = r32(rdram, kIm2dFnPtr);
            const uint32_t sp = getRegU32(ctx, 29);
            if (!inRam(camera, 0x84u) || !runtime->hasFunction(rsFn) || !runtime->hasFunction(im2dFn) || sp < 0x200000u)
            {
                ++g_fc.fbRange;
                return false;
            }
            const uint32_t a0 = getRegU32(ctx, 4), a1 = getRegU32(ctx, 5);
            const uint64_t t0 = GPR_U64(ctx, 8), t1 = GPR_U64(ctx, 9);
            const uint32_t color = getRegU32(ctx, 10);
            const float zArg = ctx->f[12];
            const uint32_t ra = getRegU32(ctx, 31);
            // ---- from here on there is no way back ----
            const float x = ps2FpuDivide(ctx->fcr31, FPU_CVT_S_W(a0), 16.0f);
            const float y = ps2FpuDivide(ctx->fcr31, FPU_CVT_S_W(a1), 8.0f);
            const float recipZ = ps2FpuDivide(ctx->fcr31, 1.0f, rf(rdram, camera + 0x80u));
            w32(rdram, desc, r32(rdram, desc) | 1u);
            chargeCycles(runtime, 1096u);
            ++g_fc.fast;
            if ((color & 0xFFu) == 0u)
            {
                ++g_fc.alpha0;
                setReturnS32(ctx, 0);
                ctx->pc = ra;
                return true;
            }
            auto unsignedToFloat = [](uint64_t reg64) -> float
            {
                const uint32_t low = static_cast<uint32_t>(reg64);
                if (static_cast<int64_t>(reg64) < 0)
                {
                    const float h = FPU_CVT_S_W(static_cast<int32_t>((low >> 1) | (low & 1u)));
                    return FPU_ADD_S(h, h);
                }
                return FPU_CVT_S_W(static_cast<int32_t>(low));
            };
            const float x1 = FPU_ADD_S(x, unsignedToFloat(t0));
            const float y1 = FPU_ADD_S(y, unsignedToFloat(t1));
            const float z = FPU_SUB_S(rf(rdram, kRasterZGlobal), zArg);
            const float rgba[4] = {FPU_CVT_S_W(static_cast<int32_t>((color >> 24) & 0xFFu)), FPU_CVT_S_W(static_cast<int32_t>((color >> 16) & 0xFFu)),
                                   FPU_CVT_S_W(static_cast<int32_t>((color >> 8) & 0xFFu)), FPU_CVT_S_W(static_cast<int32_t>(color & 0xFFu))};
            const uint32_t raster = r32(rdram, desc + 0x14u);
            // the original builds the vertices at (its own sp = entry sp - 0x1C0) + 0xC0: dead stack
            const uint32_t verts = sp - 0x1C0u + 0xC0u;
            const float px[4] = {x, x1, x, x1}, py[4] = {y, y, y1, y1};
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                uint8_t *v = rdram + verts + i * kVertexBytes;
                std::memset(v, 0, kVertexBytes);
                float f[12] = {};
                f[0] = px[i];
                f[1] = py[i];
                f[2] = z;
                f[4] = rf(rdram, kUnitUvTable + i * 8u);
                f[5] = rf(rdram, kUnitUvTable + i * 8u + 4u);
                f[6] = recipZ;
                f[8] = rgba[0];
                f[9] = rgba[1];
                f[10] = rgba[2];
                f[11] = rgba[3];
                std::memcpy(v, f, sizeof(f));
            }
            chargeCycles(runtime, 120u);
            // Both pointer targets run on this context; the original calls them through jalr in the same order.
            const __m128i savedSp = ctx->r[29];
            ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(sp - 0x1C0u))); // the original's own frame
            callGuest(rdram, runtime, ctx, rsFn, 1u, raster, 0u);
            callGuest(rdram, runtime, ctx, im2dFn, 4u, verts, 4u);
            ctx->r[29] = savedSp;
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(ra)));
            setReturnS32(ctx, 0);
            ctx->pc = ra;
            return true;
        }

        // verify: the Im2D entry records the call (expanded to a triangle list); the native run also forwards it
        GuestFunction g_im2dForward = nullptr;
        bool g_recForward = false;
        void recordIm2D(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t prim = getRegU32(ctx, 4), verts = getRegU32(ctx, 5), count = getRegU32(ctx, 6);
            auto push = [&](uint32_t i)
            {
                const uint8_t *src = rdram + ((verts + i * kVertexBytes) & PS2_RAM_MASK);
                g_record.insert(g_record.end(), src, src + kVertexBytes);
            };
            if (prim == 3u)
                for (uint32_t i = 0u; i + 2u < count; i += 3u) { push(i); push(i + 1u); push(i + 2u); }
            else if (prim == 4u)
                for (uint32_t i = 0u; i + 2u < count; ++i)
                {
                    if (i & 1u) { push(i + 1u); push(i); } else { push(i); push(i + 1u); }
                    push(i + 2u);
                }
            else
                g_record.push_back(0xEE); // unexpected primitive: forces a mismatch
            if (g_recForward && g_im2dForward)
            {
                g_im2dForward(rdram, ctx, runtime);
                return;
            }
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = getRegU32(ctx, 31);
        }

        // compares what the caller can see of the context
        bool sameCtx(const R5900Context &a, const R5900Context &b)
        {
            if (a.pc != b.pc || a.fcr31 != b.fcr31 || getRegU32(&a, 2) != getRegU32(&b, 2))
                return false;
            static const int regs[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
            for (int r : regs)
                if (GPR_U64(&a, r) != GPR_U64(&b, r))
                    return false;
            for (int f = 20; f <= 23; ++f)
                if (std::memcmp(&a.f[f], &b.f[f], sizeof(float)) != 0)
                    return false;
            return true;
        }

        void rasterVerify(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static std::vector<uint8_t> before, afterNative, nativeStream;
            before.resize(PS2_RAM_SIZE);
            afterNative.resize(PS2_RAM_SIZE);
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            const R5900Context ctxBefore = *ctx;
            const uint32_t im2dTarget = r32(rdram, kIm2dFnPtr);
            if (!runtime->hasFunction(im2dTarget))
            {
                g_rasterOriginal(rdram, ctx, runtime);
                return;
            }
            // the render-state cache of the frame cuts is C++ state outside RAM: restore it between the two runs
            uint8_t rsCache[p3fes_native_frame_cuts::kRenderStateCacheBytes];
            p3fes_native_frame_cuts::saveRenderStateCache(rsCache);
            std::memcpy(before.data(), rdram, PS2_RAM_SIZE);
            GuestFunction saved = runtime->lookupFunction(im2dTarget);
            g_im2dForward = saved;
            // 1. native, draws for real (through the recorder)
            g_record.clear();
            g_recForward = true;
            runtime->replaceFunction(im2dTarget, &recordIm2D);
            const bool used = rasterFast(rdram, ctx, runtime);
            runtime->replaceFunction(im2dTarget, saved);
            if (!used)
            {
                g_rasterOriginal(rdram, ctx, runtime); // not a fast-path glyph (the counters of rasterFast already counted it)
                return;
            }
            ++g_fc.verifyRuns;
            if (ctx->pc != ra)
            {
                ++g_fc.verifyUnwound;
                return;
            }
            const R5900Context ctxNative = *ctx;
            nativeStream = g_record;
            std::memcpy(afterNative.data(), rdram, PS2_RAM_SIZE);
            // 2. original on the restored state, Im2D recorded only
            std::memcpy(rdram, before.data(), PS2_RAM_SIZE);
            p3fes_native_frame_cuts::restoreRenderStateCache(rsCache);
            *ctx = ctxBefore;
            g_record.clear();
            g_recForward = false;
            runtime->replaceFunction(im2dTarget, &recordIm2D);
            g_rasterOriginal(rdram, ctx, runtime);
            runtime->replaceFunction(im2dTarget, saved);
            if (ctx->pc != ra)
            {
                ++g_fc.verifyUnwound;
                return;
            }
            const bool ctxOk = sameCtx(*ctx, ctxNative);
            const bool streamOk = g_record == nativeStream;
            const uint32_t deadLo = sp - 0x400u, deadHi = sp;
            uint32_t badAddr = 0u;
            bool ramOk = true;
            for (uint32_t a = 0u; a < PS2_RAM_SIZE && ramOk; a += 16u)
            {
                if (a >= deadLo && a < deadHi)
                    continue;
                if (std::memcmp(rdram + a, afterNative.data() + a, 16u) != 0)
                {
                    ramOk = false;
                    badAddr = a;
                }
            }
            if (ctxOk && streamOk && ramOk)
            {
                ++g_fc.verifyOk;
                return;
            }
            if (g_fc.verifyBad < 16u)
            {
                std::fprintf(stderr, "[p3fes-dlg-font] verify MISMATCH n=%llu ctx=%d stream=%d(%llu/%llu bytes) ram=%d",
                             static_cast<unsigned long long>(g_fc.verifyRuns), ctxOk ? 1 : 0, streamOk ? 1 : 0, static_cast<unsigned long long>(g_record.size()),
                             static_cast<unsigned long long>(nativeStream.size()), ramOk ? 1 : 0);
                if (!ramOk)
                {
                    uint32_t shown = 0u;
                    for (uint32_t a = 0u; a < PS2_RAM_SIZE && shown < 8u; a += 4u)
                    {
                        if (a >= deadLo && a < deadHi)
                            continue;
                        if (std::memcmp(rdram + a, afterNative.data() + a, 4u) != 0)
                        {
                            uint32_t o, n, b;
                            std::memcpy(&o, rdram + a, 4);
                            std::memcpy(&n, afterNative.data() + a, 4);
                            std::memcpy(&b, before.data() + a, 4);
                            std::fprintf(stderr, " [%06X was=%08X orig=%08X native=%08X]", a, b, o, n);
                            ++shown;
                        }
                    }
                }
                if (!streamOk)
                {
                    size_t k = 0;
                    while (k < g_record.size() && k < nativeStream.size() && g_record[k] == nativeStream[k]) ++k;
                    std::fprintf(stderr, " first-diff-byte=%u (vertex %u field %u)", static_cast<unsigned>(k), static_cast<unsigned>(k / kVertexBytes),
                                 static_cast<unsigned>((k % kVertexBytes) / 4u));
                    if (k < g_record.size() && k < nativeStream.size())
                    {
                        const size_t base = (k / 4u) * 4u;
                        uint32_t o, n;
                        std::memcpy(&o, g_record.data() + base, 4);
                        std::memcpy(&n, nativeStream.data() + base, 4);
                        std::fprintf(stderr, " orig=%08X native=%08X table:", o, n);
                        for (uint32_t i = 0; i < 8; ++i)
                            std::fprintf(stderr, " %08X", r32(before.data(), kUnitUvTable + i * 4u));
                    }
                }
                if (!ctxOk)
                    std::fprintf(stderr, " pc=%08X/%08X v0=%08X/%08X fcr31=%08X/%08X", ctx->pc, ctxNative.pc, getRegU32(ctx, 2),
                                 getRegU32(&ctxNative, 2), ctx->fcr31, ctxNative.fcr31);
                std::fprintf(stderr, "\n");
            }
            ++g_fc.verifyBad;
        }

        void rasterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_fontMode == FontMode::Verify && (++g_verifyTick % static_cast<uint64_t>(g_verifyEvery)) == 0u)
            {
                rasterVerify(rdram, ctx, runtime);
                return;
            }
            if (rasterFast(rdram, ctx, runtime))
                return;
            ++g_fc.fallback;
            g_rasterOriginal(rdram, ctx, runtime);
        }

        template <size_t... Index>
        bool installProbes(PS2Runtime &runtime, std::index_sequence<Index...>)
        {
            return (installOne(runtime, kProbes[Index].address, &probeHook<Index>, g_probeOriginal[Index]) | ...);
        }
    }

    bool install(PS2Runtime &runtime)
    {
        bool any = false;
        if (const char *v = std::getenv("P3FES_DIALOGUE_DIAG"); v && *v == '1')
        {
            g_lastReport = std::chrono::steady_clock::now();
            g_lastThreadNs = threadCpuNs();
            any |= installProbes(runtime, std::make_index_sequence<kProbeCount>{});
        }
        {
            const char *v = std::getenv("P3FES_CUT_FONT");
            g_fontMode = !v || !*v ? FontMode::On : (std::strcmp(v, "verify") == 0 ? FontMode::Verify : (v[0] == '0' ? FontMode::Off : FontMode::On));
            if (const char *e = std::getenv("P3FES_CUT_FONT_VERIFY_EVERY"); e && *e)
                g_verifyEvery = std::max(1, std::atoi(e));
            if (g_fontMode != FontMode::Off)
            {
                const bool ok = installOne(runtime, kFontRaster, &rasterHook, g_rasterOriginal);
                std::fprintf(stderr, "[p3fes-native] cut font glyph rasteriser hook=%u mode=%s\n", ok ? 1u : 0u,
                             g_fontMode == FontMode::Verify ? "verify" : "on");
                if (!ok)
                    g_fontMode = FontMode::Off;
                any |= ok;
            }
        }
        return any;
    }

    void logCounters()
    {
        if (g_fontMode == FontMode::Off)
            return;
        const FontCounters &c = g_fc;
        std::fprintf(stderr, "[p3fes-dlg-font] fast=%llu (alpha0=%llu) fallback=%llu (page=%llu range=%llu) | verify runs=%llu ok=%llu bad=%llu unwound=%llu\n",
                     static_cast<unsigned long long>(c.fast), static_cast<unsigned long long>(c.alpha0),
                     static_cast<unsigned long long>(c.fallback), static_cast<unsigned long long>(c.fbPage),
                     static_cast<unsigned long long>(c.fbRange), static_cast<unsigned long long>(c.verifyRuns),
                     static_cast<unsigned long long>(c.verifyOk), static_cast<unsigned long long>(c.verifyBad),
                     static_cast<unsigned long long>(c.verifyUnwound));
    }
}
