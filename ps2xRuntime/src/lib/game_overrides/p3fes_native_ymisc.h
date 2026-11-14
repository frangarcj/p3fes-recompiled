#pragma once

#include <cstdint>

class PS2Runtime;

// y_misc_004281f0 (0x4281F0..0x429C60): the per-frame update of the Tartarus floor-tile task (see p3fes_native_ymisc.cpp).
//   P3FES_NATIVE_YMISC   unset / 1  whole function native (steady phase) + native "empty cell" fast path of its five helpers
//                        helpers    only the helper fast paths (the guest function itself runs)
//                        0          original code
//                        verify     whole-function verify: native on a snapshot, restore, original; compares all RAM,
//                                   scratchpad and callee-saved registers (use P3FES_CUT_RS=0 with it: the render-state
//                                   repeat filter of p3fes_native_frame_cuts keeps host state that a double run would skew)
//                        verify-helpers  per-helper verify (callee call counts + sampled RAM compare)
//                        log        counters and helper timing only
//   P3FES_YMISC_VERIFY_EVERY=n  verify every n-th call (default 4 whole function / 128 helpers)
//   P3FES_YMISC_TIME=1  time every call of the function (log line every 5 s with P3FES_RESOURCE_DIAG=1), =2 also segments
namespace p3fes_native_ymisc
{
    bool install(PS2Runtime &runtime);
    void logCounters();
}
