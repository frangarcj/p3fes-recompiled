#include "runtime/ps2_memory.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
    constexpr uint32_t kTimerBase = 0x10000000u;
    constexpr uint32_t kCount = kTimerBase;
    constexpr uint32_t kMode = kTimerBase + 0x10u;
    constexpr uint32_t kCompare = kTimerBase + 0x20u;
    constexpr uint32_t kCue = 1u << 7;
    constexpr uint32_t kZeroReturn = 1u << 6;
    constexpr uint32_t kCompareEnable = 1u << 8;
    constexpr uint64_t kEeClockHz = 294912000ull;
    constexpr std::array<uint64_t, 4> kClockHz = {147456000ull, 9216000ull, 576000ull, 15734ull};

    void require(bool condition, const char *message, uint32_t clockSource, uint64_t value)
    {
        if (condition)
            return;
        std::fprintf(stderr, "EE timer arithmetic test failed: %s (clock=%u value=%llu)\n",
                     message, clockSource, static_cast<unsigned long long>(value));
        std::abort();
    }

    void checkAdvance(uint32_t clockSource)
    {
        PS2Memory memory;
        memory.resetEeTimers();
        memory.writeIORegister(kCount, 0u);
        memory.writeIORegister(kMode, kCue | clockSource);

        constexpr std::array<uint64_t, 7> deltas = {
            1u, 2u, 31u, 512u, 1234567u, kEeClockHz - 1u, kEeClockHz * 3u + 12345u};
        uint32_t expectedCount = 0u;
        uint64_t remainder = 0u;
        for (const uint64_t cycles : deltas)
        {
            const uint64_t scaled = cycles * kClockHz[clockSource] + remainder;
            const uint64_t expectedTicks = scaled / kEeClockHz;
            remainder = scaled % kEeClockHz;
            expectedCount = static_cast<uint32_t>(expectedCount + expectedTicks) & 0xFFFFu;

            const uint32_t raised = memory.advanceEeTimers(cycles);
            require(raised == 0u, "unexpected timer interrupt", clockSource, cycles);
            require(memory.readIORegister(kCount) == expectedCount,
                    "advance produced an inexact timer count", clockSource, cycles);
        }
    }

    void checkDeadline(uint32_t clockSource, uint64_t elapsedCycles)
    {
        PS2Memory memory;
        memory.resetEeTimers();
        memory.writeIORegister(kCount, 0u);
        memory.writeIORegister(kMode, kCue | clockSource);
        memory.advanceEeTimers(elapsedCycles);

        const uint64_t remainder = (elapsedCycles * kClockHz[clockSource]) % kEeClockHz;
        const uint32_t count = memory.readIORegister(kCount);
        const uint32_t compare = (count + 101u) & 0xFFFFu;
        memory.writeIORegister(kCompare, compare);
        memory.writeIORegister(kMode, kCue | kCompareEnable | clockSource);

        uint32_t eventTicks = (compare - count) & 0xFFFFu;
        if (eventTicks == 0u)
            eventTicks = 0x10000u;
        const uint64_t numerator = static_cast<uint64_t>(eventTicks) * kEeClockHz - remainder;
        const uint64_t expectedCycles = (numerator + kClockHz[clockSource] - 1u) / kClockHz[clockSource];
        const uint64_t actualCycles = memory.cyclesUntilNextEeTimerInterrupt();
        require(actualCycles == expectedCycles, "next timer deadline is inexact", clockSource, elapsedCycles);
    }

    void checkZeroReturn(uint32_t clockSource, uint64_t cycles)
    {
        PS2Memory memory;
        memory.resetEeTimers();
        constexpr uint32_t initialCount = 1234u;
        constexpr uint32_t compare = 5432u;
        memory.writeIORegister(kCount, initialCount);
        memory.writeIORegister(kCompare, compare);
        memory.writeIORegister(kMode, kCue | kZeroReturn | clockSource);

        const uint64_t wholeSeconds = cycles / kEeClockHz;
        const uint64_t remainingCycles = cycles % kEeClockHz;
        const uint64_t scaled = remainingCycles * kClockHz[clockSource];
        const uint64_t ticks = wholeSeconds * kClockHz[clockSource] + scaled / kEeClockHz;
        const uint32_t distance = (compare - initialCount) & 0xFFFFu;
        const uint32_t expectedCount = ticks >= distance
                                           ? static_cast<uint32_t>((ticks - distance) % compare)
                                           : static_cast<uint32_t>(initialCount + ticks) & 0xFFFFu;

        const uint32_t raised = memory.advanceEeTimers(cycles);
        require(raised == 0u, "zero-return raised an unexpected timer interrupt", clockSource, cycles);
        require(memory.readIORegister(kCount) == expectedCount,
                "zero-return timer count is inexact", clockSource, cycles);
    }
}

int main()
{
    for (uint32_t clockSource = 0u; clockSource < kClockHz.size(); ++clockSource)
    {
        checkAdvance(clockSource);
        checkDeadline(clockSource, 1u);
        checkDeadline(clockSource, 1234567u);
        checkDeadline(clockSource, kEeClockHz - 1u);
        checkZeroReturn(clockSource, kEeClockHz - 1u);
        checkZeroReturn(clockSource, kEeClockHz * 100u + 42u);
    }
    std::puts("EE timer arithmetic tests passed");
    return 0;
}
