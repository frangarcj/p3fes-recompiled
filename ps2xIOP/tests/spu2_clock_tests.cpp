#include "emulator/core/spu2.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using ps2x::iop::detail::Spu2;

namespace
{
    void configureStream(Spu2 &spu, const std::vector<uint8_t> &pcm)
    {
        for (uint32_t core = 0; core < 2; ++core)
            for (uint32_t side = 0; side < 2; ++side)
                spu.write16(Spu2::RegBase + Spu2::RegMvolBase + core * Spu2::RegMvolStride + side * 2, 0x3FFF);
        spu.write16(Spu2::RegBase + Spu2::RegMvolBase + 0x0C, 0x3FFF);
        spu.write16(Spu2::RegBase + Spu2::RegMvolBase + 0x0E, 0x3FFF);
        spu.write16(Spu2::RegBase + Spu2::RegAdmas, 1);
        assert(spu.dmaWrite(0, pcm.data(), pcm.size()));
    }

    std::vector<int16_t> drain(Spu2 &spu)
    {
        std::vector<int16_t> pcm(spu.pendingFrames() * 2);
        assert(spu.drainOutput(pcm.data(), pcm.size() / 2) == pcm.size() / 2);
        return pcm;
    }
}

int main()
{
    Spu2 spu;
    assert(spu.canSkipServiceAt(0));
    for (uint64_t cycle = 0; cycle < Spu2::CyclesPerSample; ++cycle)
    {
        spu.advanceTo(cycle);
        assert(spu.pendingFrames() == 0);
    }
    spu.advanceTo(768);
    assert(spu.canSkipServiceAt(768));
    assert(!spu.canSkipServiceAt(1536));
    assert(spu.pendingFrames() == 1);
    spu.advanceTo(768);
    spu.advanceTo(767);
    spu.advanceTo(1535);
    assert(spu.pendingFrames() == 1);
    spu.advanceTo(1536);
    assert(spu.pendingFrames() == 2);
    spu.advanceTo(10 * 768 + 511);
    assert(spu.pendingFrames() == 10);
    spu.advanceTo(11 * 768 - 1);
    assert(spu.pendingFrames() == 10);
    spu.advanceTo(11 * 768);
    assert(spu.pendingFrames() == 11);

    // An MMIO/RAM write can raise IRQ without advancing a sample. The clock's
    // no-work path must leave that event available to the IOP service.
    spu.write16(Spu2::RegBase + Spu2::RegAttr, 0x40);
    spu.write16(Spu2::RegBase + Spu2::RegIrqa + 2, 0x123);
    spu.writeRam(0x123, 0x4567);
    assert(!spu.canSkipServiceAt(11 * 768 + 1));
    spu.advanceTo(11 * 768 + 1);
    assert(spu.takeInterrupt());
    assert(!spu.takeInterrupt());

    // Compare fragmented scheduler advances against an independent sample-by-
    // sample mixer reference, including the queued third ADMA block and its
    // completion notification as the input ring gains space.
    std::vector<uint8_t> input(3 * 1024);
    for (size_t i = 0; i < input.size() / 2; ++i)
    {
        const int16_t value = static_cast<int32_t>((i * 31) % 20001) - 10000;
        input[2 * i] = static_cast<uint8_t>(value);
        input[2 * i + 1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
    }
    Spu2 clocked, reference;
    configureStream(clocked, input);
    configureStream(reference, input);
    assert(clocked.takeAdmaDone(0) == reference.takeAdmaDone(0));
    uint64_t expectedFrames = 0;
    uint32_t random = 0xF35;
    uint64_t cycle = 0;
    while (cycle < 900 * 768)
    {
        random = random * 1664525u + 1013904223u;
        cycle = std::min<uint64_t>(900 * 768, cycle + random % 1537 + 1);
        clocked.advanceTo(cycle);
        while (expectedFrames < cycle / 768)
        {
            reference.step();
            ++expectedFrames;
        }
        assert(clocked.pendingFrames() == expectedFrames);
        const bool couldSkipService = clocked.canSkipServiceAt(cycle);
        const bool clockedDone = clocked.takeAdmaDone(0);
        assert(clockedDone == reference.takeAdmaDone(0));
        if (clockedDone)
            assert(!couldSkipService);
        assert(clocked.takeInterrupt() == reference.takeInterrupt());
    }
    const auto actual = drain(clocked), expected = drain(reference);
    assert(actual == expected);
    assert(std::any_of(actual.begin(), actual.end(), [](int16_t value) { return value != 0; }));
    clocked.reset();
    clocked.advanceTo(767);
    assert(clocked.pendingFrames() == 0);
    clocked.advanceTo(768);
    assert(clocked.pendingFrames() == 1);
    std::puts("SPU2 clock boundaries, remainder, PCM, ADMA completions and immediate IRQ passed");
}
