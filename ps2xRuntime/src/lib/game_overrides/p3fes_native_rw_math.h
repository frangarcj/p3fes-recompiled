#pragma once

#include <cstdint>

class PS2Runtime;

namespace p3fes_native_rw_math
{
    bool install(PS2Runtime &runtime, bool enabled = true);
    void resetCounters();
    void logCounters();
    void pointsCounters(uint64_t &native, uint64_t &fallback);
}
