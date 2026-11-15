#pragma once

#include <cstdint>

class PS2Runtime;

// Native versions of hot guest functions of the effect system (see p3fes_native_effects.cpp for what each one does and
// why the result is the same as the recompiled code):
//   0x32AF30  mdlEffect_0032af30   particle emitter update (effect kind 0): inlines 0x357E30, 0x32A770, 0x32A120, 0x32A540,
//                                  0x32A890, 0x329D60 and the effect random generator
//   0x3275D0  mdlEffect_003275d0   draw variant 1 without local space (particles -> the sky pipeline's vertex streams):
//                                  inlines 0x477050, 0x474640, 0x4747F0, 0x323860 / 0x321140
//   0x4C31B0  RwMatrixRotate       combine op 0 (replace)
//   0x4CB7F0  RwFrameTransform     combine op 0
//   0x4916D0  RpClumpForAllAtomics only with the callback 0x34D0D0 (material colours of an effect sprite)
//   0x4C33D0  RwMatrixScale, 0x4C35D0 RwMatrixTranslate    all combine ops
// A call that has a shape the native code does not implement (kind 2 emitters, other draw variants, pointers outside RAM,
// overlapping operands, another callback, another combine op) is detected before the first write and runs the original.
//
// Environment:
//   P3FES_NATIVE_EFFECTS   unset / 1   native paths on
//                          0           original code (nothing is hooked)
//                          verify      every n-th call (P3FES_EFFECTS_VERIFY_EVERY, default 2): the native path runs on a
//                                      journaled RAM, is rolled back, then the original runs and what the original left is
//                                      what the game continues with. Compared: every byte the native path wrote, the
//                                      emitter's regions (E, T, particles with their trails, per-particle data, rand state),
//                                      the whole of RAM on every m-th check (P3FES_EFFECTS_VERIFY_FULL, default 8 / first 64),
//                                      the scratchpad, the callee-saved registers and f20..f31 (counters in the
//                                      [p3fes-effects] lines printed with P3FES_RESOURCE_DIAG=1)
//                          log         hooks pass through to the original and time it (P3FES_EFFECTS_TIME)
//   P3FES_EFFECTS_FUZZ=n   with verify: n synthetic calls per verified call (random emitters: kinds 0/1, trails, bursts,
//                          durations, curve kinds, special floats; random matrices and clumps for the helpers)
//   P3FES_EFFECTS_FUNCS=a,b  subset: af30, draw, rotate, frame, forall, scale, translate (default: all of them)
//   P3FES_EFFECTS_CACHE=0  no memoization of the colour/alpha/size curves and of the 0x357E30 rotation
//   P3FES_EFFECTS_TIME=1   time the emitter hook (ns; log line every 5 s with P3FES_RESOURCE_DIAG=1)
//                      =3  game-thread instructions from the PMU (Vita, PS2X_VITA_PMU=1) per call of each hook: run it with
//                          P3FES_NATIVE_EFFECTS=log for the original code and without for the native path
//                      =2  as 3, plus the segments of the emitter (the extra stamps add ~100 instructions each)
//   P3FES_EFFECTS_TIMEFN=a,b,c,d  with TIME=2/3: time the original functions at those addresses instead (pass-through hooks)
//   P3FES_EFFECTS_PROBE=a,b       measurement: stubs that return at once, swapped with ALTERNATE (state-free functions only)
//   P3FES_EFFECTS_ALTERNATE=s  measurement: a host thread swaps the hooks for the original functions and back every s
//                          seconds (prints the [p3fes-altcuts] lines that scratchpad/meshcut/altstat2.py reads)
//   P3FES_EFFECTS_TRACE=1  prints the first return addresses of 0x32AF30
namespace p3fes_native_effects
{
    bool install(PS2Runtime &runtime);
    void logCounters();
}
