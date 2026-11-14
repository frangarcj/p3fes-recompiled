#include "Kernel/ee_clock_duration.h"

#include <cassert>
#include <cstdint>
#include <limits>
#include <initializer_list>

int main()
{
    uint64_t seed = 0x63E99A84u;
    for (uint32_t i = 0; i < 100000u; ++i)
    {
        seed = seed * 6364136223846793005ull + 1u;
        const uint64_t cycles = seed % (ps2x::ee_clock::kFastMaxCycles + 1u);
        uint64_t ns = 0u;
        assert(ps2x::ee_clock::tryNanoseconds(cycles, ns));
        const auto exact = (static_cast<unsigned __int128>(cycles) * 1000000000u + 294911999u) / 294912000u;
        assert(ns == exact);
    }
    for (uint64_t cycles : {uint64_t{0}, uint64_t{1}, uint64_t{4920115}, ps2x::ee_clock::kFastMaxCycles})
    {
        uint64_t ns = 0u;
        assert(ps2x::ee_clock::tryNanoseconds(cycles, ns));
        assert(ns == (cycles * 1000000000u + 294911999u) / 294912000u);
    }
    uint64_t untouched = 42u;
    assert(!ps2x::ee_clock::tryNanoseconds(ps2x::ee_clock::kFastMaxCycles + 1u, untouched));
    assert(untouched == 42u);
    assert(!ps2x::ee_clock::tryNanoseconds(std::numeric_limits<uint64_t>::max(), untouched));
}
