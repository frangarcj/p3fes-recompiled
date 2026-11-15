#pragma once

#include <cstdint>

class PS2Runtime;

// Per-frame guest work that the native P3FES port does not need. Each cut has
// its own switch read once at install time (default on, "0" restores the
// original guest function):
//   P3FES_CUT_SHADOWMAP  K_FldShadow_UpdateShadowMapTask (0x19C4B0)
//                        (default OFF unless P3FES_SHADOWS=0: the pass renders the native shadow target)
//   P3FES_CUT_ADXT_IDLE  ADXT_ExecServer idle gate incl. the empty DTX exchange (0x549EA0); "log" = hook only logs
//   P3FES_CUT_ADXT_TERM  "0" turns off one rule of that gate: a PLAYEND/stopped ADXT handle whose stream is in READEND
//                        (0x54C940 -> 0x5420A0 with both decoder callbacks [adxb+0xB4]/[adxb+0xC0] null, a pure read) counts
//                        as idle; "[p3fes-cuts] adx ... term-hit=N" counts the ticks idle only thanks to it
//   P3FES_CUT_CRI_IDLE   CRI server classes 2/4/5/6 (wrappers 0x5653F0/0x565420/0x565438/0x565450), 0x552658, 0x585BA8,
//                        the vint wake of the class-2/4 workers (0x53FFF8) and the idle worker round (0x540ED8)
//   P3FES_CUT_CRI_TRACE=1 diagnostic call counters of the CRI entry points (P3FES_RESOURCE_DIAG=1 prints them)
//   P3FES_CUT_RWALLOC    RwMalloc/RwFree/RwCalloc         (0x104140/0x104280/0x104040)
//   P3FES_CUT_DBPRT      H_Dbprt_Main                     (0x104420)
//   P3FES_CUT_RS         _rwSkySetRenderState repeat gate (0x4D6B90); "verify" checks every dropped call
//   P3FES_CUT_RS_EXACT   0 = off: the blend states (10, 11) are dropped when they would store what the driver's ALPHA
//                        shadow already holds (reads the words themselves; the repeat gate cannot tell)
//   P3FES_CUT_RS_TEX     RenderState(TEXTURERASTER, raster) answered natively when it needs no GS packet (texture path
//                        of the native renderer); "verify" (every P3FES_CUT_RS_TEX_EVERY-th call) runs both on a copy
//                        of RAM and compares
//   P3FES_CUT_MAESTRO    h_maestro_001127d0 (the generic 2-D sprite/panel drawer) in C++ (p3fes_native_ui.cpp);
//                        "verify" (every P3FES_CUT_MAESTRO_VERIFY_EVERY-th call) compares RAM, registers and the whole
//                        sequence of render-state / Im2D calls with the guest function; P3FES_CUT_MAESTRO_RPSET=0 keeps
//                        its RpSkyRenderStateSet(2|3) calls even when they would store what is already there
//   P3FES_CUT_IM2D       quad fast path of the Im2D hook (p3fes_renderware.cpp); "verify" compares its draw record with
//                        the generic path's on every call
//   P3FES_CUT_FONT       frFontRaster (p3fes_native_dialogue.cpp); P3FES_CUT_FONT_DIRECT=0: through guest vertices
//   P3FES_UI_PROF=1|2    per-category cost of the 2-D path (ns on the Mac, PMU instructions on the Vita)
//   P3FES_UI_ALTERNATE=<s>  turns the native paths of the 2-D path off and on every <s> seconds (A/B inside one run)
// "verify" is accepted by SHADOWMAP, RWALLOC, ADXT_IDLE and CRI_IDLE: the guest function runs and the
// native result is compared against it (diagnostic only).
namespace p3fes_native_frame_cuts
{
    // Returns true when at least one cut was installed.
    bool install(PS2Runtime &runtime);

    // Periodic [p3fes-cuts] counter line (called with P3FES_RESOURCE_DIAG=1).
    void logCounters();

    // The C++-side cache of the render-state repeat gate (not part of guest RAM). Verify modes that rewind guest RAM
    // (p3fes_native_dialogue.cpp) save it before the first run and restore it before the second one.
    constexpr unsigned kRenderStateCacheBytes = 64u * 4u + 64u;
    void saveRenderStateCache(void *buffer);
    void restoreRenderStateCache(const void *buffer);
}
