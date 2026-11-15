#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

class PS2Runtime;
struct R5900Context;

// The 2-D user-interface path: render-state calls, Im2D draws, glyphs and the generic sprite/panel drawer
// h_maestro_001127d0. This header holds the measurement probes shared by the hooks of that path (switch
// P3FES_UI_PROF=1: nanoseconds on the Mac bench, =2: game-thread instructions from the PMU on the Vita with
// PS2X_VITA_PMU=1), and the interface of the native drawer (p3fes_native_ui.cpp).
//
// The probes cost one predictable branch per hook when off.
namespace p3fes_native_ui
{
#if defined(PLATFORM_VITA)
    extern "C" __attribute__((weak)) int ps2x_vita_pmu_read_fast(uint32_t *instr, uint32_t *cycles);
#endif

    // Which top-level UI function the nested hooks (render state, Im2D) are running for.
    enum Ctx : uint8_t { CtxOther = 0, CtxMaestro = 1, CtxGlyph = 2, CtxCount = 3 };
    enum Cat : uint8_t
    {
        RsGated,    // render-state call answered by the repeat gate
        RsOriginal, // render-state call that ran the guest function
        Im2dTotal,  // SkyIm2DRenderPrimitive hook, whole
        Im2dTexture,
        Im2dState,
        Im2dVertices,
        Im2dSubmit,
        RpSet,      // RpSkyRenderStateSet (0x4D7F60) called by the native sprite drawer
        CtxTotal,   // the top-level function itself (inclusive)
        CatCount
    };

    struct Stat
    {
        uint64_t calls = 0u, sum = 0u;
    };
    struct Prof
    {
        int mode = 0; // 0 off, 1 ns, 2 PMU instructions
        Stat stat[CtxCount][CatCount];
        uint8_t ctx = CtxOther;
        uint32_t stampCost = 0u;
        Stat rsState[64];   // render-state calls that ran the guest function, by state
        Stat rsGate[64];    // ... and the ones the gate answered
        Stat rpSet[8];      // RpSkyRenderStateSet by its first argument
        // calls made outside the drawer and the glyph path, by return address (render state, Im2D)
        std::map<uint32_t, Stat> callerRs, callerIm2d;
    };
    inline Prof g_prof;

    inline uint32_t stamp()
    {
#if defined(PLATFORM_VITA)
        if (g_prof.mode == 2)
        {
            uint32_t instr = 0u, cycles = 0u;
            if (ps2x_vita_pmu_read_fast && ps2x_vita_pmu_read_fast(&instr, &cycles))
                return instr;
            return 0u;
        }
#endif
        return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    inline void note(Cat cat, uint32_t begin)
    {
        Stat &s = g_prof.stat[g_prof.ctx][cat];
        ++s.calls;
        s.sum += static_cast<uint32_t>(stamp() - begin);
    }

    // RAII top-level context: nested hooks are attributed to it, and its own inclusive total is recorded.
    struct CtxScope
    {
        uint8_t previous;
        uint8_t id;
        uint32_t begin;
        explicit CtxScope(Ctx c) : previous(g_prof.ctx), id(c), begin(g_prof.mode ? stamp() : 0u)
        {
            g_prof.ctx = c;
        }
        ~CtxScope()
        {
            if (g_prof.mode)
            {
                Stat &s = g_prof.stat[id][CtxTotal];
                ++s.calls;
                s.sum += static_cast<uint32_t>(stamp() - begin);
            }
            g_prof.ctx = previous;
        }
    };

    // ---- native sprite/panel drawer (h_maestro_001127d0) ------------------------------------------------------------
    // One Im2D vertex as the drawer produces it (the fields the Im2D hook reads: x y z u v r g b a).
    struct Im2DVertexF
    {
        float x, y, z, u, v, r, g, b, a;
    };
    // What the drawer needs from the modules that own the render-state hook and the Im2D hook. Each owner fills its
    // member when its hook is installed; the drawer stays off while one is missing.
    struct Sinks
    {
        // The call the guest makes through the function pointer at 0x960090 (RwRenderStateSet), without the dispatch.
        // The caller's context is used to run the guest function when the repeat gate lets the call through.
        void (*renderState)(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t state, uint32_t value) = nullptr;
        // RwIm2DRenderPrimitive(TRISTRIP, v, 4) for a quad (vertices in strip order). False: not handled, nothing drawn;
        // the caller then writes the vertices to guest memory and runs the hook normally.
        bool (*im2dQuad)(uint8_t *rdram, const Im2DVertexF (&vertices)[4]) = nullptr;
        // True when every (state, value) pair of the list would be dropped by the render-state gate right now (and then
        // accounts for them as dropped); false: nothing was changed, make the calls one by one.
        bool (*renderStatesDropped)(const uint8_t *rdram, const uint8_t (*pairs)[2], uint32_t count) = nullptr;
    };
    inline Sinks g_sinks;
    // Measurement only: P3FES_UI_ALTERNATE=<seconds> turns the native paths of this module off and on every <seconds>
    // (starting on) so one run holds both sides of an A/B; logs "[p3fes-altcuts] on|off at frame 0" like the other
    // alternation switches (altstat2.py reads them).
    inline bool g_cutsOn = true;

    // Installs the native drawer over h_maestro_001127d0 (switch P3FES_CUT_MAESTRO: unset/1 = on, 0 = off,
    // verify = every Nth call also runs the original on a copy of RAM and compares). True when hooked.
    bool installMaestro(PS2Runtime &runtime);
    void logMaestroCounters();

    // Reads P3FES_UI_PROF once; true when the probes are on.
    bool profInit();
    // Prints (and clears) the per-game-frame table; called with the resource diagnostics.
    void profReport(uint64_t frames);
}
