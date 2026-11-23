// Proves that the optimised Spu2 (voice bitmask, idle bulk path, ring-buffer output) is observably identical to the
// frozen pre-optimisation implementation in tests/reference (Spu2Reference).
//
// Both objects are driven with the same randomised script (register writes, ADPCM blocks in RAM, key-on/off, DMA and
// ADMA, PCM streams, advanceTo in irregular increments, drains of random size, status reads) and every observable is
// compared after every operation: output samples, pending frames, ENDX/STATX and the whole register file, every voice
// state, the interrupt line, ADMA completion and streamConsumed.
//
// Every seed runs twice: once with the optimised Spu2 serviced like the original (advanceTo), and once in lazy mode
// (setLazy(true), PS2X_SPU2_LAZY=1): there the harness calls serviceTo() where the original is advanced, only settles
// (renders what is owed) at a random half of the comparison points, and never before takeInterrupt / takeAdmaDone /
// canSkipServiceAt: those must already agree, which is the guarantee the lazy mode makes (events are never deferred).
// Register, RAM, DMA and stream accesses render by themselves; any missing catch-up shows up as a mismatch.
//
// Usage: spu2_equivalence_tests [--seeds N] [--ops N] [--first-seed N] [--eager-only | --lazy-only] [--bench]
//   --bench  after the equivalence run (or alone with --seeds 0) prints samples/second, reference vs optimised.

#include "emulator/core/spu2.h"
#include "spu2_reference.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using ps2x::iop::detail::Spu2;
using ps2x::iop::detail::Spu2Reference;

extern "C" void ps2xSpu2Counters(uint64_t *out); // spu2.cpp: [..., services (6), deferred (7)]

namespace
{
    static_assert(Spu2::RegBase == Spu2Reference::RegBase);
    static_assert(Spu2::CyclesPerSample == Spu2Reference::CyclesPerSample);

    struct Rng
    {
        uint64_t state;
        explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) { next(); next(); }
        uint64_t next()
        {
            // splitmix64
            uint64_t z = (state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            return z ^ (z >> 31);
        }
        uint32_t below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>((next() >> 11) % n); }
        uint32_t range(uint32_t lo, uint32_t hi) { return lo + below(hi - lo + 1u); } // inclusive
        bool pct(uint32_t p) { return below(100) < p; }
        uint16_t u16() { return static_cast<uint16_t>(next() >> 20); }
    };

    struct Failure
    {
        std::string what;
    };

    [[noreturn]] void fail(uint64_t seed, size_t op, const char *kind, const std::string &detail)
    {
        std::fprintf(stderr, "MISMATCH seed=%llu op=%zu (%s): %s\n", static_cast<unsigned long long>(seed), op, kind,
                     detail.c_str());
        std::exit(1);
    }

    constexpr uint32_t kSampleRegion = 0x1000u; // halfword address of the ADPCM block area used by the script
    constexpr uint32_t kSampleBlocks = 96u;

    // One ADPCM block (8 halfwords): valid shift/filter, loop/end flags chosen so voices loop, stop and restart.
    void makeBlock(Rng &rng, uint16_t out[8], bool garbage)
    {
        uint32_t shift = rng.below(13);
        uint32_t filter = rng.below(5);
        uint32_t flags;
        const uint32_t f = rng.below(100);
        if (f < 55) flags = 0;
        else if (f < 65) flags = 4;
        else if (f < 80) flags = 3;
        else if (f < 90) flags = 1;
        else flags = 7;
        uint16_t header = static_cast<uint16_t>(shift | (filter << 4) | (flags << 8));
        if (garbage)
            header = rng.u16(); // shift > 12, filter > 4 and arbitrary flag bits
        out[0] = header;
        for (int i = 1; i < 8; ++i)
            out[i] = rng.u16();
    }

    struct Harness
    {
        uint64_t seed;
        size_t opIndex = 0;
        const char *opName = "";
        std::unique_ptr<Spu2> a = std::make_unique<Spu2>();
        std::unique_ptr<Spu2Reference> b = std::make_unique<Spu2Reference>();
        Rng rng;
        uint64_t cycle = 0;
        uint64_t samplesAdvanced = 0;
        uint64_t drainedFrames = 0;
        uint64_t interrupts = 0;
        uint64_t admaDone = 0;
        uint64_t streamFramesConsumed = 0;
        // Coverage statistics (new implementation's view; the comparison guarantees the reference agrees)
        uint64_t activeVoiceSum = 0, compares = 0, endxNonZero = 0, nonSilentFrames = 0, overflows = 0, streamFrames = 0;
        uint64_t sawIrqLine = 0;
        bool lazy = false;

        Harness(uint64_t s, bool lazyMode = false) : seed(s), rng(s), lazy(lazyMode)
        {
            a->setLazy(lazy); // eager runs must not pick up the PS2X_SPU2_LAZY default
        }

        template <typename F>
        void both(F &&fn)
        {
            fn(*a);
            fn(*b);
        }

        template <typename T>
        void expectEq(const T &x, const T &y, const char *label)
        {
            if (!(x == y))
                fail(seed, opIndex, opName, std::string(label) + ": new=" + std::to_string(static_cast<long long>(x)) +
                                                " reference=" + std::to_string(static_cast<long long>(y)));
        }

        void write16(uint32_t offset, uint16_t value)
        {
            both([&](auto &s) { s.write16(Spu2::RegBase + offset, value); });
        }

        void write32(uint32_t offset, uint32_t value) // high halfword at +0, low at +2 (voice / core address pairs)
        {
            write16(offset, static_cast<uint16_t>(value >> 16));
            write16(offset + 2u, static_cast<uint16_t>(value));
        }

        void compareVoice(uint32_t core, uint32_t i)
        {
            const auto &x = a->voice(core, i);
            const auto &y = b->voice(core, i);
            auto eq = [&](auto p, auto q, const char *f)
            {
                if (static_cast<long long>(p) != static_cast<long long>(q))
                    fail(seed, opIndex, opName,
                         "voice " + std::to_string(core) + "." + std::to_string(i) + " " + f + ": new=" +
                             std::to_string(static_cast<long long>(p)) + " reference=" + std::to_string(static_cast<long long>(q)));
            };
            eq(x.active, y.active, "active");
            eq(x.nax, y.nax, "nax");
            eq(x.lsa, y.lsa, "lsa");
            eq(x.lsaFromRegister, y.lsaFromRegister, "lsaFromRegister");
            eq(x.pitchCounter, y.pitchCounter, "pitchCounter");
            for (int k = 0; k < 28; ++k)
                eq(x.decoded[k], y.decoded[k], "decoded");
            eq(x.decodedIndex, y.decodedIndex, "decodedIndex");
            eq(x.hist1, y.hist1, "hist1");
            eq(x.hist2, y.hist2, "hist2");
            for (int k = 0; k < 3; ++k)
                eq(x.prev[k], y.prev[k], "prev");
            eq(x.blockFlags, y.blockFlags, "blockFlags");
            eq(static_cast<int>(x.phase), static_cast<int>(y.phase), "phase");
            eq(x.envelope, y.envelope, "envelope");
            eq(x.envelopeCounter, y.envelopeCounter, "envelopeCounter");
            eq(x.lastOutput, y.lastOutput, "lastOutput");
        }

        // Everything observable that does not consume state.
        void compareCheap()
        {
            if (lazy)
            {
                // Events are never deferred: the interrupt line and the ADMA-done counters (what canSkipServiceAt shows)
                // must agree before anything is settled.
                for (uint64_t probe : {cycle, cycle + Spu2::CyclesPerSample})
                    expectEq(a->canSkipServiceAt(probe), b->canSkipServiceAt(probe), "canSkipServiceAt before settle");
                if (rng.pct(45))
                    return; // let owed samples pile up across operations
                a->settle();
            }
            a->flushOutput(); // PS2X_SPU2_BATCH stages frames; the consumer-side view is compared after publishing them
            expectEq(a->pendingFrames(), b->pendingFrames(), "pendingFrames");
            for (uint32_t core = 0; core < 2; ++core)
            {
                const uint32_t base = core * 0x400u;
                for (uint32_t off : {Spu2::RegEndx, Spu2::RegEndx + 2u, Spu2::RegStatx, Spu2::RegAttr, Spu2::RegAdmas})
                    expectEq(a->read16(Spu2::RegBase + base + off), b->read16(Spu2::RegBase + base + off), "reg");
                expectEq(a->admaActive(core), b->admaActive(core), "admaActive");
            }
            expectEq(a->read16(Spu2::RegBase + 0x7C2u), b->read16(Spu2::RegBase + 0x7C2u), "SPDIF_IRQINFO");
            for (uint32_t s = 0; s < Spu2::StreamCount; ++s)
                expectEq(a->streamConsumed(s), b->streamConsumed(s), "streamConsumed");
            // canSkipServiceAt exposes the interrupt line and the ADMA-done counters without consuming them.
            for (uint64_t probe : {cycle, cycle + Spu2::CyclesPerSample, cycle + 5u * Spu2::CyclesPerSample})
                expectEq(a->canSkipServiceAt(probe), b->canSkipServiceAt(probe), "canSkipServiceAt");
            for (uint32_t core = 0; core < 2; ++core)
                for (uint32_t i = 0; i < Spu2::VoicesPerCore; ++i)
                {
                    compareVoice(core, i);
                    activeVoiceSum += a->voice(core, i).active ? 1u : 0u;
                }
            ++compares;
            endxNonZero += (a->read16(Spu2::RegBase + Spu2::RegEndx) | a->read16(Spu2::RegBase + 0x400u + Spu2::RegEndx)) != 0 ? 1u : 0u;
        }

        void compareAllRegisters()
        {
            for (uint32_t off = 0; off < 0x800u; off += 2u)
                expectEq(a->read16(Spu2::RegBase + off), b->read16(Spu2::RegBase + off), ("read16 +" + std::to_string(off)).c_str());
        }

        void compareRam(uint32_t from, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                expectEq(a->ram(from + i), b->ram(from + i), "ram");
        }

        void drain(size_t maxFrames, size_t maxLatency)
        {
            std::vector<int16_t> bufA(maxFrames * 2u + 8u, 0x5A5A), bufB(maxFrames * 2u + 8u, 0x5A5A);
            a->settle(); // no-op unless lazy: the consumer sees only what the producer has rendered
            a->flushOutput();
            const size_t na = a->drainOutput(bufA.data(), maxFrames, maxLatency);
            const size_t nb = b->drainOutput(bufB.data(), maxFrames, maxLatency);
            expectEq(na, nb, "drain frames");
            if (bufA != bufB)
            {
                size_t k = 0;
                while (bufA[k] == bufB[k]) ++k;
                fail(seed, opIndex, opName, "drained sample " + std::to_string(k) + " differs: new=" + std::to_string(bufA[k]) +
                                                " reference=" + std::to_string(bufB[k]));
            }
            drainedFrames += na;
            for (size_t k = 0; k < na; ++k)
                nonSilentFrames += (bufA[k * 2u] != 0 || bufA[k * 2u + 1u] != 0) ? 1u : 0u;
        }

        void advanceCycles(uint64_t target)
        {
            if (target > cycle)
                samplesAdvanced += (target - cycle) / Spu2::CyclesPerSample;
            if (lazy)
            {
                // the new object only counts the samples (unless an event could be raised); the reference renders them
                a->serviceTo(target);
                b->advanceTo(target);
                if (target > cycle)
                    cycle = target;
                return;
            }
            a->flushOutput();
            const size_t before = a->pendingFrames();
            const uint64_t due = target > cycle ? (target - cycle) / Spu2::CyclesPerSample : 0u;
            both([&](auto &s) { s.advanceTo(target); });
            a->flushOutput();
            if (a->pendingFrames() < before + due)
                ++overflows; // fewer frames than produced: the drop-oldest-half policy ran
            if (target > cycle)
                cycle = target;
        }

        // ---- script building blocks -------------------------------------------------------------------------

        void writeBlockRun(uint32_t firstBlock, uint32_t blocks, bool garbage)
        {
            for (uint32_t blk = 0; blk < blocks; ++blk)
            {
                uint16_t words[8];
                makeBlock(rng, words, garbage);
                const uint32_t addr = kSampleRegion + ((firstBlock + blk) % kSampleBlocks) * 8u;
                for (uint32_t w = 0; w < 8; ++w)
                    both([&](auto &s) { s.writeRam(addr + w, words[w]); });
            }
        }

        uint32_t sampleAddress()
        {
            if (rng.pct(90))
                return kSampleRegion + rng.below(kSampleBlocks) * 8u;
            return rng.below(1u << 20); // anywhere, including the wrap-around end of RAM
        }

        void setupVoice(uint32_t core, uint32_t v, bool keyOn)
        {
            const uint32_t base = core * 0x400u;
            write32(base + Spu2::RegVoiceAddr + v * 0xCu, sampleAddress());
            if (rng.pct(30))
                write32(base + Spu2::RegVoiceAddr + v * 0xCu + 4u, sampleAddress()); // LSAX override
            write16(base + v * 0x10u + 0u, rng.pct(70) ? static_cast<uint16_t>(rng.range(0x800, 0x3FFF)) : rng.u16());
            write16(base + v * 0x10u + 2u, rng.pct(70) ? static_cast<uint16_t>(rng.range(0x800, 0x3FFF)) : rng.u16());
            write16(base + v * 0x10u + 4u, rng.pct(80) ? static_cast<uint16_t>(rng.range(0x200, 0x2800)) : rng.u16());
            // ADSR: favour fast attack/decay and quick release so envelopes complete within a test run
            const uint16_t adsr1 = rng.pct(60) ? static_cast<uint16_t>((rng.range(0x40, 0x7F) << 8) | (rng.range(0, 0xF) << 4) | rng.below(16))
                                               : rng.u16();
            const uint16_t adsr2 = rng.pct(60) ? static_cast<uint16_t>((rng.range(0, 0x7F) << 6) | rng.below(8))
                                               : rng.u16();
            write16(base + v * 0x10u + 6u, adsr1);
            write16(base + v * 0x10u + 8u, adsr2);
            // route to the dry mix
            const uint32_t bit = 1u << v;
            const uint32_t vmixL = (static_cast<uint32_t>(a->read16(Spu2::RegBase + base + Spu2::RegVmixL + 2u)) << 16 |
                                    a->read16(Spu2::RegBase + base + Spu2::RegVmixL)) | (rng.pct(85) ? bit : 0u);
            const uint32_t vmixR = (static_cast<uint32_t>(a->read16(Spu2::RegBase + base + Spu2::RegVmixR + 2u)) << 16 |
                                    a->read16(Spu2::RegBase + base + Spu2::RegVmixR)) | (rng.pct(85) ? bit : 0u);
            // VMIX registers are stored as (low, high) pairs: low halfword at +0, high at +2
            write16(base + Spu2::RegVmixL, static_cast<uint16_t>(vmixL));
            write16(base + Spu2::RegVmixL + 2u, static_cast<uint16_t>(vmixL >> 16));
            write16(base + Spu2::RegVmixR, static_cast<uint16_t>(vmixR));
            write16(base + Spu2::RegVmixR + 2u, static_cast<uint16_t>(vmixR >> 16));
            if (keyOn)
                keyOnMask(core, bit);
        }

        void keyOnMask(uint32_t core, uint32_t mask)
        {
            const uint32_t base = core * 0x400u;
            if ((mask & 0xFFFFu) != 0 || rng.pct(20))
                write16(base + Spu2::RegKon, static_cast<uint16_t>(mask));
            if (((mask >> 16) & 0xFFu) != 0 || rng.pct(20))
                write16(base + Spu2::RegKon + 2u, static_cast<uint16_t>(mask >> 16));
        }

        void keyOffMask(uint32_t core, uint32_t mask)
        {
            const uint32_t base = core * 0x400u;
            write16(base + Spu2::RegKoff, static_cast<uint16_t>(mask));
            write16(base + Spu2::RegKoff + 2u, static_cast<uint16_t>(mask >> 16));
        }

        // Releases every voice quickly and removes all other sources, so the SPU2 goes fully idle.
        void quiet()
        {
            for (uint32_t core = 0; core < 2; ++core)
            {
                const uint32_t base = core * 0x400u;
                for (uint32_t v = 0; v < Spu2::VoicesPerCore; ++v)
                    write16(base + v * 0x10u + 8u, 0x0000); // release rate 0: linear, 1 step of -16384 per sample
                keyOffMask(core, 0xFFFFFFu);
                write16(base + Spu2::RegAdmas, 0);
            }
            for (uint32_t s = 0; s < Spu2::StreamCount; ++s)
                both([&](auto &sp) { sp.streamReset(s); });
        }

        void advanceRandom()
        {
            const uint32_t kind = rng.below(100);
            uint64_t target = cycle;
            if (lazy && rng.pct(50))
                target += static_cast<uint64_t>(rng.range(1, 40)) * Spu2::CyclesPerSample + rng.below(768); // stay under the bound
            else if (kind < 28)
                target += rng.range(1, 767);
            else if (kind < 53)
                target += static_cast<uint64_t>(rng.range(1, 20)) * Spu2::CyclesPerSample + rng.below(768);
            else if (kind < 78)
                target += static_cast<uint64_t>(rng.range(20, 500)) * Spu2::CyclesPerSample + rng.below(768);
            else if (kind < 92)
                target += static_cast<uint64_t>(rng.range(500, 5000)) * Spu2::CyclesPerSample + rng.below(768);
            else if (kind < 96)
                target += static_cast<uint64_t>(rng.range(5000, 40000)) * Spu2::CyclesPerSample;
            else if (kind < 98)
                target += static_cast<uint64_t>(rng.range(97000, 140000)) * Spu2::CyclesPerSample; // overflows the buffer
            else
                target = cycle > 5000 ? cycle - rng.range(1, 5000) : cycle; // backwards: must be ignored
            advanceCycles(target);
        }

        void doDrain(bool bigBias)
        {
            const uint32_t kind = rng.below(100);
            size_t maxFrames;
            if (kind < 20) maxFrames = 0;
            else if (kind < 50) maxFrames = rng.range(1, 64);
            else if (kind < 85) maxFrames = rng.range(64, 3000);
            else maxFrames = rng.range(3000, bigBias ? 120000 : 20000);
            size_t latency = SIZE_MAX;
            if (rng.pct(35))
                latency = rng.pct(30) ? 0 : rng.below(4000);
            drain(maxFrames, latency);
        }

        void randomVoiceRegWrite()
        {
            const uint32_t core = rng.below(2), v = rng.below(Spu2::VoicesPerCore);
            const uint32_t base = core * 0x400u;
            const uint32_t field = rng.below(5) * 2u; // 0 volL, 2 volR, 4 pitch, 6 adsr1, 8 adsr2
            if (rng.pct(5))
                write16(base + v * 0x10u + 0xAu, rng.u16()); // ENVX: read-only in effect, must not matter
            else
                write16(base + v * 0x10u + field, rng.pct(60) ? rng.u16() : static_cast<uint16_t>(rng.range(0, 0x3FFF)));
            if (rng.pct(25))
            {
                const uint32_t addrField = rng.below(6) * 2u; // SSA hi/lo, LSAX hi/lo, NAX hi/lo
                write16(base + Spu2::RegVoiceAddr + v * 0xCu + addrField,
                        addrField < 2u || (addrField >= 4u && addrField < 6u) ? static_cast<uint16_t>(rng.below(16))
                                                                              : rng.u16());
            }
        }

        void randomCoreRegWrite()
        {
            const uint32_t core = rng.below(2);
            const uint32_t base = core * 0x400u;
            switch (rng.below(12))
            {
            case 0: write16(base + Spu2::RegVmixL + rng.below(2) * 2u, rng.u16()); break;
            case 1: write16(base + Spu2::RegVmixR + rng.below(2) * 2u, rng.u16()); break;
            case 2: write16(base + Spu2::RegMmix, rng.u16()); break;
            case 3: write16(base + Spu2::RegAttr, rng.pct(50) ? static_cast<uint16_t>(rng.u16() | 0x40u) : rng.u16()); break;
            case 4: // IRQA: usually inside the sample region so voices hit it
                write32(base + Spu2::RegIrqa, rng.pct(80) ? kSampleRegion + rng.below(kSampleBlocks * 8u) : rng.below(1u << 20));
                break;
            case 5:
            {
                const uint32_t m = Spu2::RegMvolBase + core * Spu2::RegMvolStride + rng.below(8) * 2u;
                write16(m, rng.pct(90) ? static_cast<uint16_t>(rng.range(0x1000, 0x3FFF)) : rng.u16());
                break;
            }
            case 6: write32(base + Spu2::RegTsa, rng.pct(70) ? kSampleRegion + rng.below(kSampleBlocks * 8u) : rng.below(1u << 20)); break;
            case 7: write16(base + Spu2::RegPmon + rng.below(2) * 2u, rng.u16()); break;
            case 8: write16(base + Spu2::RegNon + rng.below(2) * 2u, rng.u16()); break;
            case 9: write16(base + Spu2::RegStatx, rng.u16()); break;
            case 10:
            {
                const uint16_t v = static_cast<uint16_t>(rng.below(4)); // ADMAS: 1 = core 0, 2 = core 1
                write16(base + Spu2::RegAdmas, v);
                break;
            }
            default: write16(base + rng.range(0x1B2, 0x1BE) / 2u * 2u, rng.u16()); break; // misc registers
            }
        }

        void randomDma()
        {
            const uint32_t core = rng.below(2);
            const uint32_t kind = rng.below(100);
            if (kind < 25 && rng.pct(60))
            {
                // toggle ADMA on for this core (the stream tests need it often)
                write16(core * 0x400u + Spu2::RegAdmas, static_cast<uint16_t>(rng.pct(85) ? core + 1u : rng.below(4)));
            }
            if (kind < 70)
            {
                static const uint32_t sizes[] = {2, 6, 30, 64, 500, 1022, 1024, 1026, 2048, 3000, 4096, 6144, 8192, 1};
                size_t bytes = sizes[rng.below(sizeof(sizes) / sizeof(sizes[0]))];
                if (rng.pct(20))
                    bytes = rng.range(0, 5000);
                std::vector<uint8_t> data(bytes);
                const bool quiet = rng.pct(50);
                for (auto &d : data)
                    d = static_cast<uint8_t>(quiet ? rng.below(8) * 16 : rng.below(256));
                bool ra = false, rb = false;
                ra = a->dmaWrite(core, data.data(), data.size());
                rb = b->dmaWrite(core, data.data(), data.size());
                expectEq(ra, rb, "dmaWrite result");
            }
            else
            {
                const size_t bytes = rng.range(0, 600);
                std::vector<uint8_t> da(bytes, 0xA5), db(bytes, 0xA5);
                const bool ra = a->dmaRead(core, da.data(), bytes);
                const bool rb = b->dmaRead(core, db.data(), bytes);
                expectEq(ra, rb, "dmaRead result");
                if (da != db)
                    fail(seed, opIndex, opName, "dmaRead data differs");
            }
            if (rng.pct(30))
                both([&](auto &s) { s.markDmaReady(core); });
        }

        void randomStreamOp()
        {
            const uint32_t kind = rng.below(100);
            const uint32_t stream = rng.pct(90) ? rng.below(Spu2::StreamCount) : rng.range(0, 6); // sometimes invalid
            if (kind < 70)
            {
                static const uint32_t rates[] = {0, 8000, 11025, 22050, 32000, 44100, 48000, 48000, 96000, 24000};
                const uint32_t rate = rates[rng.below(sizeof(rates) / sizeof(rates[0]))];
                const size_t frames = rng.pct(10) ? 0 : rng.range(1, rng.pct(10) ? 20000 : 400);
                std::vector<int16_t> pcm(frames * 2u);
                const int amp = rng.pct(30) ? 32767 : 6000;
                for (auto &p : pcm)
                    p = static_cast<int16_t>(static_cast<int>(rng.below(2 * amp + 1)) - amp);
                both([&](auto &s) { s.streamPush(stream, rate, pcm.data(), frames); });
            }
            else if (kind < 80)
            {
                both([&](auto &s) { s.streamReset(stream); });
            }
            else
            {
                expectEq(a->streamConsumed(stream), b->streamConsumed(stream), "streamConsumed");
            }
        }

        void runOp()
        {
            // lazy mode: leave samples owed in front of most operations, so each access has something to render first
            if (lazy && rng.pct(60))
                advanceCycles(cycle + static_cast<uint64_t>(rng.range(1, 30)) * Spu2::CyclesPerSample + rng.below(768));
            const uint32_t roll = rng.below(100);
            if (roll < 15) { opName = "voice reg"; randomVoiceRegWrite(); }
            else if (roll < 21) { opName = "core reg"; randomCoreRegWrite(); }
            else if (roll < 30)
            {
                opName = "setup voice + key on";
                const uint32_t n = rng.pct(70) ? 1 : rng.range(2, 8);
                for (uint32_t i = 0; i < n; ++i)
                    setupVoice(rng.below(2), rng.below(Spu2::VoicesPerCore), true);
            }
            else if (roll < 34)
            {
                opName = "key on mask";
                keyOnMask(rng.below(2), rng.pct(10) ? 0xFFFFFFu : (1u << rng.below(24)) | (rng.pct(50) ? 1u << rng.below(24) : 0u));
            }
            else if (roll < 38)
            {
                opName = "key off mask";
                keyOffMask(rng.below(2), rng.pct(15) ? 0xFFFFFFu : (1u << rng.below(24)) | (rng.pct(50) ? 1u << rng.below(24) : 0u));
            }
            else if (roll < 42) { opName = "ram blocks"; writeBlockRun(rng.below(kSampleBlocks), rng.range(1, 24), rng.pct(15)); }
            else if (roll < 44)
            {
                opName = "ram garbage";
                const uint32_t addr = rng.pct(80) ? kSampleRegion + rng.below(kSampleBlocks * 8u) : rng.below(1u << 20);
                const uint16_t v = rng.u16();
                both([&](auto &s) { s.writeRam(addr, v); });
            }
            else if (roll < 47)
            {
                opName = "RegData";
                const uint32_t core = rng.below(2);
                const uint16_t v = rng.u16();
                write16(core * 0x400u + Spu2::RegData, v);
            }
            else if (roll < 55) { opName = "dma"; randomDma(); }
            else if (roll < 60) { opName = "stream"; randomStreamOp(); }
            else if (roll < 84) { opName = "advanceTo"; advanceRandom(); }
            else if (roll < 90) { opName = "drain"; doDrain(rng.pct(10)); }
            else if (roll < 93)
            {
                opName = "takeInterrupt";
                const bool ia = a->takeInterrupt(), ib = b->takeInterrupt();
                expectEq(ia, ib, "takeInterrupt");
                interrupts += ia ? 1 : 0;
            }
            else if (roll < 95)
            {
                opName = "takeAdmaDone";
                const uint32_t core = rng.below(2);
                const bool da = a->takeAdmaDone(core), db = b->takeAdmaDone(core);
                expectEq(da, db, "takeAdmaDone");
                admaDone += da ? 1 : 0;
            }
            else if (roll < 96)
            {
                opName = "ENDX clear";
                const uint32_t core = rng.below(2);
                write16(core * 0x400u + Spu2::RegEndx + rng.below(2) * 2u, 0);
            }
            else if (roll < 97)
            {
                opName = "step()";
                for (uint32_t n = rng.range(1, 3); n > 0; --n)
                    both([&](auto &s) { s.step(); });
            }
            else if (roll < 99)
            {
                opName = "quiet";
                quiet();
                advanceCycles(cycle + static_cast<uint64_t>(rng.range(1, 600)) * Spu2::CyclesPerSample);
                if (rng.pct(70))
                    advanceCycles(cycle + static_cast<uint64_t>(rng.range(2000, 20000)) * Spu2::CyclesPerSample); // idle stretch
            }
            else if (rng.pct(15))
            {
                opName = "reset";
                both([&](auto &s) { s.reset(); });
                cycle = 0;
                preamble(); // reset zeroes every volume register; restore them so the mixer is audible again
            }
            else
            {
                opName = "step() x many";
                for (uint32_t n = rng.range(1, 600); n > 0; --n)
                    both([&](auto &s) { s.step(); });
            }

            compareCheap();
            if (opIndex % 40u == 0u)
                compareAllRegisters();
        }

        // Audible master volumes (MVOL and BVOL) and valid ADPCM data in RAM.
        void preamble()
        {
            const char *saved = opName;
            opName = "preamble";
            for (uint32_t core = 0; core < 2; ++core)
                for (uint32_t i = 0; i < 8; ++i)
                    write16(Spu2::RegMvolBase + core * Spu2::RegMvolStride + i * 2u, rng.pct(80) ? 0x3FFF : rng.u16());
            writeBlockRun(0, kSampleBlocks, false);
            opName = saved;
        }

        void run(size_t ops)
        {
            preamble();
            for (opIndex = 1; opIndex <= ops; ++opIndex)
            {
                if (opIndex % 700u == 0u)
                {
                    opName = "periodic quiet";
                    quiet();
                    advanceCycles(cycle + 3000u * Spu2::CyclesPerSample);
                    compareCheap();
                }
                runOp();
            }
            opName = "final";
            compareAllRegisters();
            compareRam(kSampleRegion, kSampleBlocks * 8u);
            for (int i = 0; i < 4096; ++i)
                compareRam(rng.below(1u << 20), 1);
            doDrain(true);
            drain(1u << 18, SIZE_MAX);
            if (rng.pct(100))
            {
                bool x = a->takeInterrupt(), y = b->takeInterrupt();
                expectEq(x, y, "final takeInterrupt");
            }
        }
    };

    // ---- benchmark ----------------------------------------------------------------------------------------

    template <typename T>
    void setupBench(T &spu, uint32_t voices)
    {
        auto w = [&](uint32_t off, uint16_t v) { spu.write16(T::RegBase + off, v); };
        // A looping block (loop start + end + repeat) so voices never stop on their own.
        const uint32_t block = 0x1000u;
        spu.writeRam(block, 0x0000u | (7u << 8) | 0x08u);
        for (uint32_t i = 1; i < 8; ++i)
            spu.writeRam(block + i, static_cast<uint16_t>(0x1357u * i + 0x2468u));
        for (uint32_t core = 0; core < 2; ++core)
        {
            const uint32_t base = core * 0x400u;
            for (uint32_t i = 0; i < 8; ++i)
                w(T::RegMvolBase + core * T::RegMvolStride + i * 2u, 0x3FFF);
            w(base + T::RegVmixL, 0xFFFF);
            w(base + T::RegVmixL + 2u, 0x00FF);
            w(base + T::RegVmixR, 0xFFFF);
            w(base + T::RegVmixR + 2u, 0x00FF);
        }
        for (uint32_t n = 0; n < voices; ++n)
        {
            const uint32_t core = n % 2u, v = n / 2u;
            const uint32_t base = core * 0x400u;
            w(base + T::RegVoiceAddr + v * 0xCu, 0);
            w(base + T::RegVoiceAddr + v * 0xCu + 2u, static_cast<uint16_t>(block));
            w(base + v * 0x10u + 0u, 0x3000);
            w(base + v * 0x10u + 2u, 0x3000);
            w(base + v * 0x10u + 4u, static_cast<uint16_t>(0x0C00u + n * 0x40u));
            w(base + v * 0x10u + 6u, 0x7FFF); // fastest attack, fast decay, highest sustain level
            w(base + v * 0x10u + 8u, 0x1FC0); // sustain rate: slow increase
            w(base + T::RegKon + (v >= 16u ? 2u : 0u), static_cast<uint16_t>(1u << (v % 16u)));
        }
    }

    template <typename T>
    double benchRate(uint32_t voices, uint64_t totalSamples, uint32_t batchSamples)
    {
        auto spu = std::make_unique<T>();
        setupBench(*spu, voices);
        std::vector<int16_t> out(8192 * 2u);
        uint64_t cycle = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t done = 0; done < totalSamples; done += batchSamples)
        {
            cycle += static_cast<uint64_t>(batchSamples) * T::CyclesPerSample;
            spu->advanceTo(cycle);
            spu->drainOutput(out.data(), out.size() / 2u);
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (voices != 0)
        {
            uint32_t active = 0;
            for (uint32_t c = 0; c < 2; ++c)
                for (uint32_t i = 0; i < 24; ++i)
                    active += spu->voice(c, i).active ? 1u : 0u;
            if (active != voices)
            {
                std::fprintf(stderr, "bench setup error: %u voices active, expected %u\n", active, voices);
                std::exit(1);
            }
        }
        return static_cast<double>(totalSamples) / seconds;
    }

    void runBench()
    {
        struct Case { const char *name; uint32_t voices; uint64_t samples; };
        const Case cases[] = {{"idle", 0, 4000000}, {"4 voices", 4, 600000}, {"24 voices", 24, 200000}};
        std::printf("%-12s %16s %16s %9s\n", "scenario", "reference smp/s", "optimised smp/s", "speedup");
        for (const Case &c : cases)
        {
            // Best of several interleaved runs: the host is shared with other work, so single runs are noisy.
            double ref = 0.0, opt = 0.0;
            for (int rep = 0; rep < 5; ++rep)
            {
                ref = std::max(ref, benchRate<Spu2Reference>(c.voices, c.samples, 40));
                opt = std::max(opt, benchRate<Spu2>(c.voices, c.samples, 40));
            }
            std::printf("%-12s %16.0f %16.0f %8.1fx\n", c.name, ref, opt, opt / ref);
        }
        std::printf("(48000 samples/s is real time; batches of 40 samples, drained each batch)\n");
    }
}

int main(int argc, char **argv)
{
    size_t seeds = 30, ops = 2500;
    uint64_t firstSeed = 1;
    bool bench = false, eager = true, lazyRun = true;
    for (int i = 1; i < argc; ++i)
    {
        if (!std::strcmp(argv[i], "--eager-only")) { lazyRun = false; continue; }
        if (!std::strcmp(argv[i], "--lazy-only")) { eager = false; continue; }
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc) seeds = static_cast<size_t>(std::atoll(argv[++i]));
        else if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops = static_cast<size_t>(std::atoll(argv[++i]));
        else if (!std::strcmp(argv[i], "--first-seed") && i + 1 < argc) firstSeed = static_cast<uint64_t>(std::atoll(argv[++i]));
        else if (!std::strcmp(argv[i], "--bench")) bench = true;
        else { std::fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }

    for (int mode = 0; mode < 2; ++mode)
    {
        const bool lazyMode = mode == 1;
        if (lazyMode ? !lazyRun : !eager)
            continue;
        uint64_t totalOps = 0, totalSamples = 0, totalBulk = 0, totalDrained = 0, totalIrq = 0, totalAdma = 0;
        uint64_t totalActive = 0, totalCompares = 0, totalEndx = 0, totalNonSilent = 0, totalOverflows = 0;
        uint64_t spuBefore[16] = {}, spuAfter[16] = {};
        ps2xSpu2Counters(spuBefore);
        for (size_t n = 0; n < seeds; ++n)
        {
            Harness h(firstSeed + n, lazyMode);
            h.run(ops);
            totalOps += ops;
            totalSamples += h.samplesAdvanced;
            totalBulk += h.a->bulkSamplesForTest();
            totalDrained += h.drainedFrames;
            totalIrq += h.interrupts;
            totalAdma += h.admaDone;
            totalActive += h.activeVoiceSum;
            totalCompares += h.compares;
            totalEndx += h.endxNonZero;
            totalNonSilent += h.nonSilentFrames;
            totalOverflows += h.overflows;
        }
        ps2xSpu2Counters(spuAfter);
        if (seeds != 0)
        {
            std::printf("spu2 equivalence (%s service): %zu seeds, %llu operations, %llu samples advanced, %llu via the idle bulk path, "
                        "%llu frames drained, %llu interrupts taken, %llu ADMA completions: identical to the reference\n",
                        lazyMode ? "lazy" : "eager", seeds, static_cast<unsigned long long>(totalOps),
                        static_cast<unsigned long long>(totalSamples), static_cast<unsigned long long>(totalBulk),
                        static_cast<unsigned long long>(totalDrained), static_cast<unsigned long long>(totalIrq),
                        static_cast<unsigned long long>(totalAdma));
            std::printf("coverage: avg %.2f active voices per comparison, ENDX non-zero in %llu of %llu comparisons, "
                        "%llu non-silent frames drained, %llu buffer overflows; service points %llu, left for later %llu\n",
                        totalCompares ? static_cast<double>(totalActive) / static_cast<double>(totalCompares) : 0.0,
                        static_cast<unsigned long long>(totalEndx), static_cast<unsigned long long>(totalCompares),
                        static_cast<unsigned long long>(totalNonSilent), static_cast<unsigned long long>(totalOverflows),
                        static_cast<unsigned long long>(spuAfter[6] - spuBefore[6]), static_cast<unsigned long long>(spuAfter[7] - spuBefore[7]));
            // The comparison is only meaningful if the interesting paths were really taken.
            const uint64_t deferred = spuAfter[7] - spuBefore[7];
            const bool overflowsExpected = !lazyMode; // the lazy harness does not count them
            if (seeds >= 10 && (totalBulk == 0 || totalNonSilent == 0 || totalEndx == 0 || (overflowsExpected && totalOverflows == 0) ||
                                totalIrq == 0 || totalAdma == 0 || totalDrained == 0 || (lazyMode && deferred == 0)))
            {
                std::fprintf(stderr, "coverage check failed (%s): bulk=%llu irq=%llu adma=%llu drained=%llu deferred=%llu\n",
                             lazyMode ? "lazy" : "eager", static_cast<unsigned long long>(totalBulk),
                             static_cast<unsigned long long>(totalIrq), static_cast<unsigned long long>(totalAdma),
                             static_cast<unsigned long long>(totalDrained), static_cast<unsigned long long>(deferred));
                return 1;
            }
        }
    }
    if (bench)
        runBench();
    return 0;
}
