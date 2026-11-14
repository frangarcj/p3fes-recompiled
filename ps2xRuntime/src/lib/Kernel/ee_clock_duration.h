#pragma once

#include <cstdint>
#include <limits>

namespace ps2x::ee_clock
{
// Most scheduler deadlines are shorter than one field. Reduce 1e9 / 294912000
// to 15625 / 4608, then divide by 512 before the constant 32-bit division by 9.
// This keeps the conversion exact on ARMv7 without its 64-bit division helper.
inline constexpr uint64_t kFastMaxCycles = uint64_t{1} << 26u;
static_assert(((kFastMaxCycles * 15625u + 4607u) >> 9u) <= std::numeric_limits<uint32_t>::max());

inline bool tryNanoseconds(uint64_t cycles, uint64_t &nanoseconds)
{
    if (cycles > kFastMaxCycles)
        return false;
    const uint64_t scaled = cycles * 15625u + 4607u;
    nanoseconds = static_cast<uint32_t>(scaled >> 9u) / 9u;
    return true;
}
}
