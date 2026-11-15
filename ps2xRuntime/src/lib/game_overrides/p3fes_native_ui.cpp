// 2-D user-interface path: probes, and (below) the native sprite/panel drawer. See p3fes_native_ui.h.
// Local builds include this file from p3fes_native_frame_cuts.cpp (unity style) until it has its own CMake line.
#include "p3fes_native_ui.h"
#include "p3fes_native_frame_cuts.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace p3fes_native_ui
{
    bool profInit()
    {
        const char *v = std::getenv("P3FES_UI_PROF");
        g_prof.mode = v && *v >= '1' && *v <= '2' ? *v - '0' : 0;
        if (g_prof.mode)
        {
            uint64_t total = 0u;
            for (int i = 0; i < 200; ++i)
            {
                const uint32_t a = stamp(), b = stamp();
                total += static_cast<uint32_t>(b - a);
            }
            g_prof.stampCost = static_cast<uint32_t>(total / 200u);
            std::fprintf(stderr, "[p3fes-ui] probes on, unit=%s stamp cost=%u\n", g_prof.mode == 2 ? "instr" : "ns", g_prof.stampCost);
        }
        return g_prof.mode != 0;
    }

    void profReport(uint64_t frames)
    {
        if (!g_prof.mode)
            return;
        static const char *ctxNames[CtxCount] = {"other", "maestro", "glyph"};
        static const char *catNames[CatCount] = {"rs-gated", "rs-orig", "im2d", "im2d-tex", "im2d-state", "im2d-vert", "im2d-submit", "rpset", "TOTAL"};
        const double f = frames ? static_cast<double>(frames) : 1.0;
        for (int which = 0; which < 2; ++which)
        {
            auto &m = which ? g_prof.callerIm2d : g_prof.callerRs;
            std::vector<std::pair<uint64_t, uint32_t>> top;
            for (const auto &e : m)
                top.push_back({e.second.sum, e.first});
            std::sort(top.rbegin(), top.rend());
            std::fprintf(stderr, "[p3fes-ui] %s calls outside the drawer/glyph path by return address (per frame calls x instr):", which ? "Im2D" : "render-state");
            for (size_t i = 0; i < top.size() && i < 14u; ++i)
            {
                const Stat &st = m[top[i].second];
                std::fprintf(stderr, " %08X:%.1fx%.0f", top[i].second, st.calls / f, st.calls ? static_cast<double>(st.sum) / st.calls : 0.0);
            }
            std::fprintf(stderr, "\n");
            m.clear();
        }
        {
            std::fprintf(stderr, "[p3fes-ui] render-state by state per frame (calls x instr; orig | gated):");
            for (int i = 0; i < 64; ++i)
            {
                Stat &o = g_prof.rsState[i], &g = g_prof.rsGate[i];
                if (!o.calls && !g.calls)
                    continue;
                std::fprintf(stderr, " %d:%.1fx%.0f|%.1fx%.0f", i, o.calls / f, o.calls ? static_cast<double>(o.sum) / o.calls : 0.0,
                             g.calls / f, g.calls ? static_cast<double>(g.sum) / g.calls : 0.0);
                o = {};
                g = {};
            }
            std::fprintf(stderr, " | rpset:");
            for (int i = 0; i < 8; ++i)
            {
                Stat &o = g_prof.rpSet[i];
                if (!o.calls)
                    continue;
                std::fprintf(stderr, " %d:%.1fx%.0f", i, o.calls / f, static_cast<double>(o.sum) / o.calls);
                o = {};
            }
            std::fprintf(stderr, "\n");
        }
        for (int c = 0; c < CtxCount; ++c)
        {
            std::fprintf(stderr, "[p3fes-ui] %s per frame:", ctxNames[c]);
            for (int k = 0; k < CatCount; ++k)
            {
                Stat &s = g_prof.stat[c][k];
                if (!s.calls)
                    continue;
                // the cost of the probe's own second stamp is inside every measured interval
                const double net = static_cast<double>(s.sum) - static_cast<double>(s.calls) * g_prof.stampCost;
                std::fprintf(stderr, " %s=%.1f calls x %.0f = %.0fK", catNames[k], s.calls / f, s.calls ? net / s.calls : 0.0, net / f / 1e3);
                s = {};
            }
            std::fprintf(stderr, "\n");
        }
    }

    // =================================================================================================================
    // Native h_maestro_001127d0 (0x1127D0): the generic 2-D sprite / panel drawer.
    //
    //   void maestro_draw(Obj *s1 /*a0*/, int setStates /*a1*/)
    //
    // The guest function is 1176 MIPS instructions; one call costs ~17 K host instructions on the Vita, three quarters
    // of it the function's own code and the dispatch of its ~17 indirect calls (RenderWare's RenderStateSet and
    // RwIm2DRenderPrimitive through the function pointers at 0x960090 / 0x9600A0). This port runs the same sequence
    // of calls (same arguments, same order, so the render-state shadow, the GS packet buffer and the draw stream come
    // out identical) but does the arithmetic in C++ and calls the two targets without the dispatcher.
    //
    // What it does (obj fields: +08 parent, +0C layout index, +10/+14 x/y float, +18/+19 u8 alpha terms, +1C/+1E s16
    // extra piece, +20 float angle, +24/+26 s16 origin, +28/+2A u16 scale (1/4096), +2C float z bias, +30..32 u8 colour
    // factors; layout record L = [parent+0x184] + (idx << 7): +14 texture slot, +18 flags (1 mirror u, 2 mirror v,
    // 8 untextured), +2C blend flags, +34/+38/+3C/+40 edge widths, +44/+48 offset, +54/+58/+5C/+60 uv rect, +64/+68/+6C/
    // +70 corner colours, +74/+76 s16 size override):
    //   1. if setStates: RenderState(6,1) (7,2) (8,1) (9,2) (12,1) (11,6) (10,5) (2,4) (14,0)
    //   2. uv rect -> 4 corner uv pairs, divided by the texture's width/height
    //   3. if setStates: RpSkyRenderStateSet(2, 0x44), (3, 0x717FB), and by L+0x2C bit 0 / bit 1 (2, 0x48 / 0x42), (3, 0x71801)
    //   4. mirror flags swap the uv pairs
    //   5. size = uv rect size (or override) * scale; corner offsets; four vertices (colour = corner colour * factor,
    //      gamma-ish table unless flag 8, alpha reduced by +19 / +18)
    //   6. RenderState(TEXTURERASTER, texture or 0), RwIm2DRenderPrimitive(TRISTRIP, v, 4): the main quad
    //   7. the first non-zero of L+0x34 / L+0x38 / L+0x3C / L+0x40 / obj+0x1E / obj+0x1C adds one edge strip, drawn the
    //      same way (RenderState + Im2D)
    // A rotated sprite (angle != 0, polynomial sin/cos, a matrix transform) and a missing texture slot are left to the
    // guest function: those conditions are decided from reads alone, before anything has been changed.
    // =================================================================================================================
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;
        constexpr uint32_t kMaestro = 0x001127D0u;
        constexpr uint32_t kRsFnPtr = 0x00960090u, kIm2dFnPtr = 0x009600A0u, kRasterZ = 0x00960088u;
        constexpr uint32_t kRsEntry = 0x004D6B90u, kIm2dEntry = 0x004EDBA0u, kRpSkySet = 0x004D7F60u;
        constexpr uint32_t kSentinel = 0xFFFFFFFCu;

        enum class MaestroMode : uint8_t { Off, On, Verify };
        MaestroMode g_maestroMode = MaestroMode::Off;
        GuestFunction g_maestroOriginal = nullptr;
        int g_maestroVerifyEvery = 61;
        uint64_t g_maestroTick = 0u, g_maestroAltTick = 0u;

        struct MaestroCounters
        {
            uint64_t native = 0u, fallback = 0u, rotated = 0u, notReady = 0u, pieces = 0u;
            uint64_t verifyRuns = 0u, verifyOk = 0u, verifyBad = 0u, verifyUnwound = 0u, im2dFallback = 0u;
        } g_mc;

        inline uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4u); return v; }
        inline int16_t rd16s(const uint8_t *rdram, uint32_t a) { int16_t v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 2u); return v; }
        inline uint16_t rd16u(const uint8_t *rdram, uint32_t a) { uint16_t v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 2u); return v; }
        inline uint8_t rd8(const uint8_t *rdram, uint32_t a) { return rdram[a & PS2_RAM_MASK]; }
        inline float rdf(const uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + (a & PS2_RAM_MASK), 4u); return v; }
        inline bool inRam(uint32_t a, uint32_t size) { return a >= 0x100000u && static_cast<uint64_t>(a) + size <= PS2_RAM_SIZE; }

        // ---- verify: event log shared by the native run and the original run -------------------------------------
        std::vector<uint32_t> g_events;
        bool g_logEvents = false;
        inline void logEvent(uint32_t kind, uint32_t a, uint32_t b)
        {
            g_events.push_back(kind);
            g_events.push_back(a);
            g_events.push_back(b);
        }
        inline void logQuad(const Im2DVertexF (&v)[4])
        {
            g_events.push_back(3u);
            for (const Im2DVertexF &p : v)
            {
                uint32_t bits[9];
                std::memcpy(bits, &p, sizeof(bits));
                g_events.insert(g_events.end(), bits, bits + 9);
            }
        }

        // Runs the guest function at `entry` to completion on the caller's context (registers a0/a1 given); the
        // native path never suspends: a function that asks to be resumed is resumed here.
        bool callGuest(uint8_t *rdram, PS2Runtime *runtime, R5900Context *ctx, uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2 = 0u)
        {
            GuestFunction fn = runtime->lookupFunction(entry);
            if (!fn)
                return false;
            ctx->pc = entry;
            ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
            ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a2)));
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(kSentinel)));
            fn(rdram, ctx, runtime);
            for (uint32_t resume = 0u; resume < 100000u && ctx->pc != kSentinel; ++resume)
            {
                if (runtime->isStopRequested() || ctx->pc == 0u || !runtime->hasFunction(ctx->pc))
                    break;
                runtime->lookupFunction(ctx->pc)(rdram, ctx, runtime);
            }
            return ctx->pc == kSentinel;
        }

        inline float unsignedToFloat(uint64_t reg64)
        {
            const uint32_t low = static_cast<uint32_t>(reg64);
            if (static_cast<int64_t>(reg64) < 0)
            {
                const float h = FPU_CVT_S_W(static_cast<int32_t>((low >> 1) | (low & 1u)));
                return FPU_ADD_S(h, h);
            }
            return FPU_CVT_S_W(static_cast<int32_t>(low));
        }

        // The context the drawer is entered with: what it must leave alone
        struct MaestroEntry
        {
            uint32_t obj = 0u, parent = 0u, layoutBase = 0u, layout = 0u, texSlot = 0u, tex = 0u, cam = 0u;
            bool setStates = false;
        };

        // Everything the guest function decides from reads alone before it changes anything: false = leave the call to
        // the guest function (rotated sprite, no texture, anything that is not the plain layout).
        bool maestroPrecheck(const uint8_t *rdram, R5900Context *ctx, MaestroEntry &e)
        {
            e.obj = getRegU32(ctx, 4);
            e.setStates = GPR_U64(ctx, 5) != 0u;
            if (!inRam(e.obj, 0x34u))
                return false;
            e.parent = rd32(rdram, e.obj + 8u);
            if (!inRam(e.parent, 0x188u))
                return false;
            e.layoutBase = rd32(rdram, e.parent + 0x184u);
            e.layout = e.layoutBase + (rd32(rdram, e.obj + 0xCu) << 7);
            if (!inRam(e.layout, 0x80u))
                return false;
            const uint32_t slotIndex = rd32(rdram, e.layout + 0x14u);
            e.texSlot = e.parent + 0x104u + (slotIndex << 2);
            if (!inRam(e.texSlot, 4u))
                return false;
            e.tex = rd32(rdram, e.texSlot);
            if (!e.tex)
            {
                ++g_mc.fallback;
                return false;
            }
            if (!inRam(e.tex, 0x14u))
                return false;
            if ((rd32(rdram, e.obj + 0x20u) & 0x7FFFFFFFu) != 0u)
            {
                ++g_mc.rotated;
                return false;
            }
            if (rd32(rdram, kRsFnPtr) != kRsEntry || rd32(rdram, kIm2dFnPtr) != kIm2dEntry)
                return false;
            e.cam = rd32(rdram, getRegU32(ctx, 28) - 0x4C30u);
            if (!inRam(e.cam, 0x84u))
                return false;
            return true;
        }

        inline void rsCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t state, uint32_t value)
        {
            if (g_logEvents)
                logEvent(1u, state, value);
            g_sinks.renderState(rdram, ctx, runtime, state, value);
        }

        // RpSkyRenderStateSet(2 = ALPHA, v) / (3 = TEST low half, v): the guest function asks for packet room, stores the
        // value in its shadow word ([gp-0x4288] / [gp-0x42B0]) and emits one packet. The sprite drawer calls it with the
        // same constants around every sprite, so the shadow usually holds the value already: the call then only emits a
        // packet (dead in the native renderer). Same rule as the render-state repeat gate; P3FES_CUT_MAESTRO_RPSET=0
        // runs the guest function every time, P3FES_CUT_MAESTRO=verify runs it and checks the claim.
        constexpr uint32_t kGpAddress = 0x007D2CF0u;
        constexpr uint32_t kShadowAlpha = kGpAddress - 0x4288u, kShadowTest = kGpAddress - 0x42B0u;
        constexpr uint32_t kBlockStart = kGpAddress - 20304u, kBlockSize = 3404u;
        bool g_rpSetDrop = true;
        uint64_t g_rpSetDropped = 0u, g_rpSetChecked = 0u, g_rpSetBad = 0u;
        inline bool rpSetNoop(const uint8_t *rdram, uint32_t a, uint32_t b)
        {
            if (a == 2u)
            {
                uint64_t alpha;
                std::memcpy(&alpha, rdram + kShadowAlpha, sizeof(alpha));
                return alpha == (static_cast<uint64_t>(b & 0xFFu) | (static_cast<uint64_t>(b & 0xFF000000u) << 8));
            }
            if (a == 3u)
            {
                uint64_t test;
                std::memcpy(&test, rdram + kShadowTest, sizeof(test));
                return (test & 0xFFFFu) == (b & 0xFFFFu);
            }
            return false;
        }

        inline void rpSet(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a, uint32_t b)
        {
            if (g_logEvents)
                logEvent(2u, a, b);
            if (g_rpSetDrop && g_cutsOn && rpSetNoop(rdram, a, b))
            {
                ++g_rpSetDropped;
                return;
            }
            const uint32_t begin = g_prof.mode ? stamp() : 0u;
            callGuest(rdram, runtime, ctx, kRpSkySet, a, b);
            if (g_prof.mode)
            {
                note(RpSet, begin);
                Stat &st = g_prof.rpSet[a & 7u];
                ++st.calls;
                st.sum += static_cast<uint32_t>(stamp() - begin) - g_prof.stampCost;
            }
        }

        // One draw: RenderState(TEXTURERASTER, raster) then the quad. `sp` is where the guest function's vertex array
        // sits (sp + 0x100): used only when the native Im2D path declines.
        inline void drawPiece(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t raster, const Im2DVertexF (&v)[4],
                              uint32_t scratch)
        {
            rsCall(rdram, ctx, runtime, 1u, raster);
            if (g_logEvents)
                logQuad(v);
            if (g_sinks.im2dQuad(rdram, v))
                return;
            // Declined (diagnostics on, ...): the hook reads the vertices from guest memory like the original call
            ++g_mc.im2dFallback;
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                float f[16] = {};
                f[0] = v[i].x; f[1] = v[i].y; f[2] = v[i].z; f[4] = v[i].u; f[5] = v[i].v;
                f[8] = v[i].r; f[9] = v[i].g; f[10] = v[i].b; f[11] = v[i].a;
                std::memcpy(rdram + (scratch + i * 64u), f, 64u);
            }
            callGuest(rdram, runtime, ctx, kIm2dEntry, 4u, scratch, 4u);
        }

        // The body, after maestroPrecheck said yes. Writes nothing the guest function does not write (its own dead
        // stack aside) and leaves every register the guest function preserves as it found it.
        void maestroNative(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const MaestroEntry &e)
        {
            const uint32_t entrySp = getRegU32(ctx, 29);
            const uint64_t entryRa = GPR_U64(ctx, 31);
            const uint32_t obj = e.obj;
            const uint32_t gp = getRegU32(ctx, 28);
            (void)gp;
            float S[0x240 / 4] = {};
#define SF(off) S[(off) >> 2]
            const uint32_t scratch = (entrySp - 0x220u) + 0x100u; // where the guest function builds its vertices (dead stack)

            // f22 = 1 / camera.farZ-ish word at [camera + 0x80]
            // c.eq.s 0.0, angle (angle == 0 here) leaves the FPU condition bit set
            ctx->fcr31 |= 0x00800000u;
            // f22 = 1 / [camera + 0x80]: only the FPU flags of the division are visible (the vertices' w is not read)
            const float recipCam = ps2FpuDivide(ctx->fcr31, 1.0f, rdf(rdram, e.cam + 0x80u));
            uint32_t charge = 0u;

            // ---- 1. state prologue
            if (e.setStates)
            {
                static const uint8_t prologue[9][2] = {{6, 1}, {7, 2}, {8, 1}, {9, 2}, {0xC, 1}, {0xB, 6}, {0xA, 5}, {2, 4}, {0xE, 0}};
                if (g_sinks.renderStatesDropped && g_sinks.renderStatesDropped(rdram, prologue, 9u))
                {
                    if (g_logEvents)
                        for (const auto &p : prologue)
                            logEvent(1u, p[0], p[1]);
                }
                else
                    for (const auto &p : prologue)
                        rsCall(rdram, ctx, runtime, p[0], p[1]);
                charge += 9u * EeScheduler::kGuestDispatchCycles;
            }

            // ---- 2. uv rect
            const uint32_t L = e.layout;
            const float texW = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, e.tex + 0xCu)));  // lwc1 f1 / cvt.s.w
            const float texH = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, e.tex + 0x10u))); // lwc1 f0 / cvt.s.w
            {
                const float rectX = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x54u)));
                const float u0 = ps2FpuDivide(ctx->fcr31, rectX, texW);
                const float rectY = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x58u)));
                const float v0 = ps2FpuDivide(ctx->fcr31, rectY, texH);
                const float rectR = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x5Cu) - 1u));
                const float u1 = ps2FpuDivide(ctx->fcr31, rectR, texW);
                const float rectB = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x60u) - 1u));
                const float v1 = ps2FpuDivide(ctx->fcr31, rectB, texH);
                SF(0x80) = u0; SF(0x84) = v0; SF(0x88) = u1; SF(0x8C) = v0;
                SF(0x90) = u0; SF(0x94) = v1; SF(0x98) = u1; SF(0x9C) = v1;
            }

            // ---- 3. raw GS alpha / test registers
            if (e.setStates)
            {
                const uint32_t blendFlags = rd32(rdram, L + 0x2Cu);
                rpSet(rdram, ctx, runtime, 2u, 0x44u);
                rpSet(rdram, ctx, runtime, 3u, 0x717FBu);
                if ((blendFlags & 1u) != 0u)
                {
                    rpSet(rdram, ctx, runtime, 2u, 0x48u);
                    rpSet(rdram, ctx, runtime, 3u, 0x71801u);
                }
                if ((blendFlags & 2u) != 0u)
                {
                    rpSet(rdram, ctx, runtime, 2u, 0x42u);
                    rpSet(rdram, ctx, runtime, 3u, 0x71801u);
                }
                charge += 4u * EeScheduler::kGuestDispatchCycles;
            }

            // ---- 4. mirroring
            const uint32_t flags = rd32(rdram, L + 0x18u);
            auto swapPair = [&](uint32_t a, uint32_t b)
            {
                std::swap(SF(a), SF(b));
                std::swap(SF(a + 4), SF(b + 4));
            };
            if (flags & 2u)
            {
                swapPair(0x80, 0x90);
                swapPair(0x88, 0x98);
            }
            if (flags & 1u)
            {
                swapPair(0x80, 0x88);
                swapPair(0x90, 0x98);
            }

            // ---- 5. size, corners, vertices
            const int32_t originX = rd16s(rdram, obj + 0x24u), originY = rd16s(rdram, obj + 0x26u);
            SF(0xD0) = FPU_CVT_S_W(0 - originX);
            SF(0xD4) = FPU_CVT_S_W(0 - originY);
            auto extent = [&](uint32_t lo, uint32_t hi, uint32_t overrideAt, uint32_t scaleAt, int32_t origin) -> float
            {
                int64_t a2 = static_cast<int64_t>(static_cast<int32_t>(rd32(rdram, L + hi) - rd32(rdram, L + lo)));
                const int64_t over = rd16s(rdram, L + overrideAt);
                if (over != 0)
                    a2 = over;
                const uint32_t scale = rd16u(rdram, obj + scaleAt);
                if (scale != 0u)
                {
                    const uint32_t product = static_cast<uint32_t>(a2) * scale; // mult: low 32 bits, sign-extended
                    a2 = static_cast<int64_t>(static_cast<int32_t>(product >> 12));
                }
                float f1 = unsignedToFloat(static_cast<uint64_t>(a2)); // bltz a2: unsigned fix-up for negatives
                return FPU_SUB_S(f1, FPU_CVT_S_W(origin));
            };
            SF(0xF4) = extent(0x54u, 0x5Cu, 0x74u, 0x28u, originX);
            SF(0xF8) = extent(0x58u, 0x60u, 0x76u, 0x2Au, originY);
            SF(0xDC) = SF(0xF4);
            SF(0xE0) = SF(0xD4);
            SF(0xE8) = SF(0xD0);
            SF(0xEC) = SF(0xF8);
            {
                const float f6 = rdf(rdram, obj + 0x10u), f5 = rdf(rdram, obj + 0x14u);
                const float f4 = FPU_CVT_S_W(originX);
                const float f3 = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x44u)));
                const float f2 = FPU_CVT_S_W(originY);
                const float f1 = FPU_CVT_S_W(static_cast<int32_t>(rd32(rdram, L + 0x48u)));
                static const uint16_t corner[4] = {0xD0, 0xDC, 0xE8, 0xF4};
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    float x = FPU_ADD_S(f4, SF(corner[i]));
                    x = FPU_ADD_S(f6, x);
                    x = FPU_ADD_S(x, f3);
                    SF(0x60 + 8u * i) = x;
                    float y = FPU_ADD_S(f2, SF(corner[i] + 4u));
                    y = FPU_ADD_S(f5, y);
                    y = FPU_ADD_S(y, f1);
                    SF(0x64 + 8u * i) = y;
                }
            }
            Im2DVertexF V[4];
            {
                const float zBase = rdf(rdram, kRasterZ);
                const float zBias = rdf(rdram, obj + 0x2Cu);
                const uint32_t f18 = rd8(rdram, obj + 0x18u), f19 = rd8(rdram, obj + 0x19u);
                const uint32_t m0 = rd8(rdram, obj + 0x30u), m1 = rd8(rdram, obj + 0x31u), m2 = rd8(rdram, obj + 0x32u);
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    Im2DVertexF &p = V[i];
                    p.z = FPU_SUB_S(zBase, zBias);
                    p.u = SF(0x80 + 8u * i);
                    p.v = SF(0x84 + 8u * i);
                    const uint32_t word = i == 2u ? rd32(rdram, L + 0x70u) : (i == 3u ? rd32(rdram, L + 0x6Cu) : rd32(rdram, L + 0x64u + i * 4u));
                    uint32_t cr = (word >> 24) & 0xFFu, cg = (word >> 16) & 0xFFu, cb = (word >> 8) & 0xFFu, ca = word & 0xFFu;
                    cr = ((cr * m0) / 255u) & 0xFFu;
                    cg = ((cg * m1) / 255u) & 0xFFu;
                    cb = ((cb * m2) / 255u) & 0xFFu;
                    if ((flags & 8u) == 0u)
                    {
                        auto gamma = [](uint32_t c) -> uint32_t { return c < 0x81u ? (((c * 255u) >> 7) & 0xFFu) : 0xFFu; };
                        cr = gamma(cr);
                        cg = gamma(cg);
                        cb = gamma(cb);
                        ca = gamma(ca);
                    }
                    ca = ((ca * (255u - f19)) / 255u) & 0xFFu;
                    ca = f18 < ca ? ((ca - f18) & 0xFFu) : 0u;
                    p.r = FPU_CVT_S_W(static_cast<int32_t>(cr));
                    p.g = FPU_CVT_S_W(static_cast<int32_t>(cg));
                    p.b = FPU_CVT_S_W(static_cast<int32_t>(cb));
                    p.a = FPU_CVT_S_W(static_cast<int32_t>(ca));
                    p.x = SF(0x60 + 8u * i);
                    p.y = SF(0x64 + 8u * i);
                }
                charge += 4u * 32u; // the vertex loop's four back edges
            }

            // ---- 6. main quad
            const auto textureArg = [&](uint32_t layout) -> uint32_t
            {
                return (rd32(rdram, layout + 0x18u) & 8u) != 0u ? 0u : rd32(rdram, e.texSlot);
            };
            drawPiece(rdram, ctx, runtime, textureArg(L), V, scratch);
            charge += 2u * EeScheduler::kGuestDispatchCycles;
            ++g_mc.pieces;

            // ---- 7. one edge strip
            auto setPos = [&](Im2DVertexF &p, float x, float y) { p.x = x; p.y = y; };
            auto setUv = [&](Im2DVertexF &p, uint32_t at) { p.u = SF(at); p.v = SF(at + 4); };
            auto strip = [&](uint32_t which, int32_t a1) -> bool
            {
                const float a1f = FPU_CVT_S_W(a1);
                switch (which)
                {
                case 0: // top: grows upwards by L+0x34
                    setPos(V[0], SF(0x60), FPU_SUB_S(SF(0x64), a1f));
                    setPos(V[1], SF(0x68), FPU_SUB_S(SF(0x6C), a1f));
                    setPos(V[2], SF(0x60), SF(0x64));
                    setPos(V[3], SF(0x68), SF(0x6C));
                    setUv(V[0], 0x80); setUv(V[1], 0x88); setUv(V[2], 0x80); setUv(V[3], 0x88);
                    break;
                case 1: // bottom: grows downwards by L+0x38 / obj+0x1E
                    setPos(V[0], SF(0x70), SF(0x74));
                    setPos(V[1], SF(0x78), SF(0x7C));
                    setPos(V[2], SF(0x70), FPU_ADD_S(SF(0x74), a1f));
                    setPos(V[3], SF(0x78), FPU_ADD_S(SF(0x7C), a1f));
                    setUv(V[0], 0x90); setUv(V[1], 0x98); setUv(V[2], 0x90); setUv(V[3], 0x98);
                    break;
                case 2: // left: grows to the left by L+0x3C
                    setPos(V[0], FPU_CVT_S_W(FPU_CVT_W_S(SF(0x60)) - a1), SF(0x64));
                    setPos(V[1], SF(0x60), SF(0x64));
                    setPos(V[2], FPU_CVT_S_W(FPU_CVT_W_S(SF(0x70)) - a1), SF(0x74));
                    setPos(V[3], SF(0x70), SF(0x74));
                    setUv(V[0], 0x80); setUv(V[1], 0x80); setUv(V[2], 0x90); setUv(V[3], 0x90);
                    break;
                default: // right: grows to the right by L+0x40 / obj+0x1C
                    setPos(V[0], SF(0x68), SF(0x6C));
                    setPos(V[1], FPU_CVT_S_W(FPU_CVT_W_S(SF(0x68)) + a1), SF(0x6C));
                    setPos(V[2], SF(0x78), SF(0x7C));
                    setPos(V[3], FPU_CVT_S_W(FPU_CVT_W_S(SF(0x78)) + a1), SF(0x7C));
                    setUv(V[0], 0x88); setUv(V[1], 0x88); setUv(V[2], 0x98); setUv(V[3], 0x98);
                    break;
                }
                drawPiece(rdram, ctx, runtime, textureArg(L), V, scratch);
                charge += 2u * EeScheduler::kGuestDispatchCycles;
                ++g_mc.pieces;
                return true;
            };
            int32_t edge;
            if ((edge = static_cast<int32_t>(rd32(rdram, L + 0x34u))) != 0)
                strip(0, edge);
            else if ((edge = static_cast<int32_t>(rd32(rdram, L + 0x38u))) != 0)
                strip(1, edge);
            else if ((edge = static_cast<int32_t>(rd32(rdram, L + 0x3Cu))) != 0)
                strip(2, edge);
            else if ((edge = static_cast<int32_t>(rd32(rdram, L + 0x40u))) != 0)
                strip(3, edge);
            else if ((edge = rd16s(rdram, obj + 0x1Eu)) != 0)
                strip(1, edge);
            else if ((edge = rd16s(rdram, obj + 0x1Cu)) != 0)
                strip(3, edge);
#undef SF
            (void)recipCam;

            // emulated EE time the original spends (dispatches, loop back edges); never enters the scheduler
            for (uint32_t left = charge; left != 0u;)
            {
                const uint32_t step = std::min(left, 64u);
                if (!runtime->eeTryChargeCall(step))
                    break;
                left -= step;
            }
            // what the guest function leaves: v0 = Im2D's 1, sp / ra as entered, pc = return address
            ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(entrySp)));
            ctx->r[31] = _mm_set_epi64x(0, static_cast<int64_t>(entryRa));
            setReturnS32(ctx, 1);
            ctx->pc = static_cast<uint32_t>(entryRa);
        }
    }

    namespace
    {
        // ---- verify -------------------------------------------------------------------------------------------
        GuestFunction g_vRsInner = nullptr, g_vSetInner = nullptr;
        void vRsLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kRsEntry)
                logEvent(1u, getRegU32(ctx, 4), getRegU32(ctx, 5));
            g_vRsInner(rdram, ctx, runtime);
        }
        void vSetLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kRpSkySet)
            {
                g_vSetInner(rdram, ctx, runtime);
                return;
            }
            const uint32_t a = getRegU32(ctx, 4), b = getRegU32(ctx, 5);
            logEvent(2u, a, b);
            // the claim of rpSetNoop: a call it would drop changes nothing in the driver's state block but the packet
            // bookkeeping (gp-17216 .. gp-17120)
            const bool noop = rpSetNoop(rdram, a, b);
            static uint8_t before[kBlockSize];
            if (noop)
                std::memcpy(before, rdram + kBlockStart, kBlockSize);
            g_vSetInner(rdram, ctx, runtime);
            if (noop)
            {
                const uint32_t first = kGpAddress - 17216u - kBlockStart, last = kGpAddress - 17120u - kBlockStart;
                std::memcpy(before + first, rdram + kBlockStart + first, last - first);
                ++g_rpSetChecked;
                if (std::memcmp(before, rdram + kBlockStart, kBlockSize) != 0 && g_rpSetBad++ < 8u)
                    std::fprintf(stderr, "[p3fes-ui] RpSkyRenderStateSet(%u, %08X) predicted a no-op but changed the state block\n", a, b);
            }
        }
        void vIm2dLog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t prim = getRegU32(ctx, 4), verts = getRegU32(ctx, 5), count = getRegU32(ctx, 6);
            if (prim == 4u && count == 4u && inRam(verts, 4u * 64u))
            {
                Im2DVertexF q[4];
                for (uint32_t i = 0u; i < 4u; ++i)
                {
                    const uint32_t a = verts + i * 64u;
                    q[i] = {rdf(rdram, a), rdf(rdram, a + 4u), rdf(rdram, a + 8u), rdf(rdram, a + 0x10u), rdf(rdram, a + 0x14u),
                            rdf(rdram, a + 0x20u), rdf(rdram, a + 0x24u), rdf(rdram, a + 0x28u), rdf(rdram, a + 0x2Cu)};
                }
                logQuad(q);
            }
            else
                logEvent(0xEEu, prim, count);
            ctx->r[2] = _mm_cvtsi32_si128(1);
            ctx->pc = getRegU32(ctx, 31);
        }

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

        void maestroVerify(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const MaestroEntry &e)
        {
            static std::vector<uint8_t> before, afterNative;
            before.resize(PS2_RAM_SIZE);
            afterNative.resize(PS2_RAM_SIZE);
            const uint32_t ra = getRegU32(ctx, 31), sp = getRegU32(ctx, 29);
            const R5900Context ctxBefore = *ctx;
            uint8_t cacheBefore[p3fes_native_frame_cuts::kRenderStateCacheBytes];
            uint8_t cacheNative[p3fes_native_frame_cuts::kRenderStateCacheBytes];
            p3fes_native_frame_cuts::saveRenderStateCache(cacheBefore);
            std::memcpy(before.data(), rdram, PS2_RAM_SIZE);
            // 1. native, for real (the RpSkyRenderStateSet drops are checked on their own, see rpSetCheck)
            g_events.clear();
            g_logEvents = true;
            const bool dropWas = g_rpSetDrop;
            g_rpSetDrop = false;
            maestroNative(rdram, ctx, runtime, e);
            g_rpSetDrop = dropWas;
            g_logEvents = false;
            std::vector<uint32_t> nativeEvents = g_events;
            const R5900Context ctxNative = *ctx;
            p3fes_native_frame_cuts::saveRenderStateCache(cacheNative);
            std::memcpy(afterNative.data(), rdram, PS2_RAM_SIZE);
            // 2. the guest function on the restored state; its draws are only recorded
            std::memcpy(rdram, before.data(), PS2_RAM_SIZE);
            p3fes_native_frame_cuts::restoreRenderStateCache(cacheBefore);
            *ctx = ctxBefore;
            g_vRsInner = runtime->lookupFunction(kRsEntry);
            g_vSetInner = runtime->lookupFunction(kRpSkySet);
            GuestFunction im2dInner = runtime->lookupFunction(kIm2dEntry);
            runtime->replaceFunction(kRsEntry, &vRsLog);
            runtime->replaceFunction(kRpSkySet, &vSetLog);
            runtime->replaceFunction(kIm2dEntry, &vIm2dLog);
            g_events.clear();
            g_maestroOriginal(rdram, ctx, runtime);
            runtime->replaceFunction(kRsEntry, g_vRsInner);
            runtime->replaceFunction(kRpSkySet, g_vSetInner);
            runtime->replaceFunction(kIm2dEntry, im2dInner);
            ++g_mc.verifyRuns;
            if (ctx->pc != ra)
            {
                ++g_mc.verifyUnwound;
                return;
            }
            uint8_t cacheOriginal[p3fes_native_frame_cuts::kRenderStateCacheBytes];
            p3fes_native_frame_cuts::saveRenderStateCache(cacheOriginal);
            const bool ctxOk = sameCtx(*ctx, ctxNative);
            const bool eventsOk = g_events == nativeEvents;
            const bool cacheOk = std::memcmp(cacheOriginal, cacheNative, sizeof(cacheNative)) == 0;
            const uint32_t deadLo = sp - 0x1000u, deadHi = sp;
            uint32_t badAddr = 0u;
            bool ramOk = true;
            for (uint32_t a = 0u; a < PS2_RAM_SIZE; a += 16u)
            {
                if (a >= deadLo && a < deadHi)
                    continue;
                if (std::memcmp(rdram + a, afterNative.data() + a, 16u) != 0)
                {
                    ramOk = false;
                    badAddr = a;
                    break;
                }
            }
            if (ctxOk && eventsOk && cacheOk && ramOk)
            {
                ++g_mc.verifyOk;
                return;
            }
            if (g_mc.verifyBad < 16u)
            {
                std::fprintf(stderr, "[p3fes-ui] maestro verify MISMATCH n=%llu ctx=%d events=%d(%u/%u words) cache=%d ram=%d obj=%08X layout=%08X setStates=%d",
                             static_cast<unsigned long long>(g_mc.verifyRuns), ctxOk ? 1 : 0, eventsOk ? 1 : 0,
                             static_cast<unsigned>(g_events.size()), static_cast<unsigned>(nativeEvents.size()), cacheOk ? 1 : 0,
                             ramOk ? 1 : 0, e.obj, e.layout, e.setStates ? 1 : 0);
                if (!eventsOk)
                {
                    size_t k = 0u;
                    while (k < g_events.size() && k < nativeEvents.size() && g_events[k] == nativeEvents[k]) ++k;
                    std::fprintf(stderr, " first-diff-word=%u orig=%08X native=%08X", static_cast<unsigned>(k),
                                 k < g_events.size() ? g_events[k] : 0u, k < nativeEvents.size() ? nativeEvents[k] : 0u);
                }
                if (!ramOk)
                {
                    uint32_t o, n, b;
                    std::memcpy(&o, rdram + badAddr, 4);
                    std::memcpy(&n, afterNative.data() + badAddr, 4);
                    std::memcpy(&b, before.data() + badAddr, 4);
                    std::fprintf(stderr, " ram[%06X was=%08X orig=%08X native=%08X]", badAddr, b, o, n);
                }
                if (!ctxOk)
                    std::fprintf(stderr, " pc=%08X/%08X fcr31=%08X/%08X v0=%08X/%08X", ctx->pc, ctxNative.pc, ctx->fcr31, ctxNative.fcr31,
                                 getRegU32(ctx, 2), getRegU32(&ctxNative, 2));
                std::fprintf(stderr, "\n");
            }
            ++g_mc.verifyBad;
        }

        void maestroHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const int alternate = [] { const char *v = std::getenv("P3FES_UI_ALTERNATE"); return v ? std::atoi(v) : 0; }();
            if (alternate > 0 && (++g_maestroAltTick & 63u) == 0u)
            {
                static const auto start = std::chrono::steady_clock::now();
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
                const bool on = ((seconds / alternate) & 1) == 0;
                if (on != g_cutsOn)
                {
                    g_cutsOn = on;
                    std::fprintf(stderr, "[p3fes-altcuts] %s at frame 0 (%llds)\n", on ? "on" : "off", static_cast<long long>(seconds));
                }
            }
            // resumed in the middle of the guest function (after a checkpoint): not ours
            if (ctx->pc != kMaestro || !g_cutsOn || !g_sinks.renderState || !g_sinks.im2dQuad)
            {
                if (ctx->pc == kMaestro)
                    ++g_mc.notReady;
                g_maestroOriginal(rdram, ctx, runtime);
                return;
            }
            MaestroEntry e;
            if (!maestroPrecheck(rdram, ctx, e))
            {
                const uint32_t begin = g_prof.mode ? stamp() : 0u;
                g_maestroOriginal(rdram, ctx, runtime);
                if (g_prof.mode)
                {
                    Stat &st = g_prof.rpSet[7]; // reported as "rpset 7": the calls left to the guest function
                    ++st.calls;
                    st.sum += static_cast<uint32_t>(stamp() - begin);
                }
                return;
            }
            ++g_mc.native;
            if (g_maestroMode == MaestroMode::Verify && (++g_maestroTick % static_cast<uint64_t>(g_maestroVerifyEvery)) == 0u)
            {
                maestroVerify(rdram, ctx, runtime, e);
                return;
            }
            maestroNative(rdram, ctx, runtime, e);
        }
    }

    bool installMaestro(PS2Runtime &runtime)
    {
        const char *v = std::getenv("P3FES_CUT_MAESTRO");
        g_maestroMode = !v || !*v ? MaestroMode::On : (std::strcmp(v, "verify") == 0 ? MaestroMode::Verify : (v[0] == '0' ? MaestroMode::Off : MaestroMode::On));
        if (const char *r = std::getenv("P3FES_CUT_MAESTRO_RPSET"); r && r[0] == '0' && !r[1])
            g_rpSetDrop = false;
        if (const char *n = std::getenv("P3FES_CUT_MAESTRO_VERIFY_EVERY"); n && *n)
            g_maestroVerifyEvery = std::max(1, std::atoi(n));
        if (g_maestroMode == MaestroMode::Off || !runtime.hasFunction(kMaestro))
            return false;
        GuestFunction current = runtime.lookupFunction(kMaestro);
        if (!current || current == &maestroHook)
            return false;
        g_maestroOriginal = current;
        const bool ok = runtime.replaceFunction(kMaestro, &maestroHook);
        std::fprintf(stderr, "[p3fes-native] cut sprite/panel drawer hook=%u mode=%s\n", ok ? 1u : 0u,
                     g_maestroMode == MaestroMode::Verify ? "verify" : "on");
        return ok;
    }

    void logMaestroCounters()
    {
        if (g_maestroMode == MaestroMode::Off)
            return;
        std::fprintf(stderr, "[p3fes-ui] maestro native=%llu pieces=%llu left-to-guest: rotated=%llu no-texture=%llu not-ready=%llu im2d-declined=%llu | verify runs=%llu ok=%llu bad=%llu unwound=%llu\n",
                     static_cast<unsigned long long>(g_mc.native), static_cast<unsigned long long>(g_mc.pieces),
                     static_cast<unsigned long long>(g_mc.rotated), static_cast<unsigned long long>(g_mc.fallback),
                     static_cast<unsigned long long>(g_mc.notReady), static_cast<unsigned long long>(g_mc.im2dFallback),
                     static_cast<unsigned long long>(g_mc.verifyRuns), static_cast<unsigned long long>(g_mc.verifyOk),
                     static_cast<unsigned long long>(g_mc.verifyBad), static_cast<unsigned long long>(g_mc.verifyUnwound));
        std::fprintf(stderr, "[p3fes-ui] RpSkyRenderStateSet dropped=%llu verify: no-op claims checked=%llu wrong=%llu\n",
                     static_cast<unsigned long long>(g_rpSetDropped), static_cast<unsigned long long>(g_rpSetChecked),
                     static_cast<unsigned long long>(g_rpSetBad));
    }
}
