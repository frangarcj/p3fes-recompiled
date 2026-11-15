#pragma once

#include <cstdint>

class PS2Runtime;

// Measurement only: per-function instruction and cycle accounting of the game thread.
//   P3FES_FN_PROF=addr,addr,...   (hex, up to 80; at most 80 wrappers are installed, in this order, after every other hook)
//     Each listed guest function gets a pass-through wrapper that reads the PMU of the game thread (Vita with
//     PS2X_VITA_PMU=1 or =2; elsewhere nanoseconds) before and after the call. Per function it reports calls per frame,
//     inclusive and exclusive (inclusive minus the inclusive cost of the wrapped functions it called) instructions and
//     cycles per frame. A frame is one call of the function at P3FES_FN_PROF_FRAME (default 0x1983A0, kwlnUpdate).
//     The wrapper's own cost (a stamp pair) is subtracted per call; the dispatch a wrapped function costs its callers
//     (they no longer reach it by a direct call) stays in the parent's exclusive figure.
//   P3FES_FN_PROF_EVERY=n         frames per report line group (default 150)
//   P3FES_FN_PROF_TOP=n           lines per report (default 40, sorted by exclusive instructions)
// Output: "[fnprof] f=<frames> ..." lines on stderr (the runtime log).
namespace p3fes_native_fnprof
{
    bool install(PS2Runtime &runtime);
}
