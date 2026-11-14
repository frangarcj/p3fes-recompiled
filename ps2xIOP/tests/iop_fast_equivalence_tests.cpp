// Differential test of the PS2X_IOP_FAST burst interpreter (IopCpuCore::runBurst) against the ordinary
// IopCpuCore::executeFetchedInstruction: random instruction streams (ALU, mult/div, loads/stores with
// misaligned/out-of-RAM/KSEG addresses, LWL/LWR/SWL/SWR, branches, branches in delay slots, load-delay hazards, COP0,
// syscalls, overflowing adds, jr/jalr to junk) run on two identical memories; the full CPU state is compared after
// every burst and RAM/ownership flags every few hundred bursts.
#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_imports.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace
{
    using namespace ps2x::iop::detail;

    constexpr uint32_t kCode = 0x10000u;
    constexpr uint32_t kCodeWords = 0x4000u; // 64 KB of code
    constexpr uint32_t kData = 0x40000u;

    struct Rng
    {
        std::mt19937_64 gen;
        explicit Rng(uint64_t seed) : gen(seed) {}
        uint32_t next(uint32_t n) { return static_cast<uint32_t>(gen() % n); }
        bool chance(uint32_t percent) { return next(100) < percent; }
    };

    uint32_t rtype(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct)
    {
        return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
    }
    uint32_t itype(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm)
    {
        return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu);
    }

    uint32_t randomInstruction(Rng &rng, uint32_t index)
    {
        // small register pool for plenty of hazards; r1..r3 hold data pointers, r4..r9 are the work registers
        auto work = [&]() { return 4u + rng.next(6); };
        auto any = [&]() { return rng.chance(85) ? work() : rng.next(32); };
        auto base = [&]() { return rng.chance(95) ? 1u + rng.next(3) : any(); };
        auto offset = [&]() { return rng.chance(90) ? rng.next(0x3000) : rng.next(0x10000); };
        const uint32_t kind = rng.next(100);
        if (kind < 22) // ALU register
        {
            static const uint32_t functs[] = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B,
                                              0x20, 0x22};
            return rtype(any(), any(), any(), rng.next(32), functs[rng.next(sizeof(functs) / sizeof(functs[0]))]);
        }
        if (kind < 42) // ALU immediate
        {
            static const uint32_t ops[] = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x09, 0x09, 0x08};
            return itype(ops[rng.next(sizeof(ops) / sizeof(ops[0]))], any(), any(), rng.chance(50) ? rng.next(16) : rng.next(0x10000));
        }
        if (kind < 62) // loads and stores
        {
            static const uint32_t ops[] = {0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x23, 0x2B, 0x23};
            return itype(ops[rng.next(sizeof(ops) / sizeof(ops[0]))], base(), any(), offset());
        }
        if (kind < 74) // branches within the code region
        {
            const int32_t target = static_cast<int32_t>(rng.next(48)) - 24;
            const uint32_t off = static_cast<uint32_t>(target) & 0xFFFFu;
            switch (rng.next(7))
            {
            case 0: return itype(0x04, any(), any(), off);
            case 1: return itype(0x05, any(), any(), off);
            case 2: return itype(0x06, any(), 0, off);
            case 3: return itype(0x07, any(), 0, off);
            case 4: return itype(0x01, any(), rng.next(2), off);
            case 5: return itype(0x01, rng.chance(30) ? 31u : any(), 0x10 + rng.next(2), off);
            default: return itype(0x05, any(), 0, off);
            }
        }
        if (kind < 80) // j / jal into the code region
        {
            const uint32_t target = kCode + 4u * rng.next(kCodeWords - 64u);
            return ((rng.chance(50) ? 0x02u : 0x03u) << 26) | ((target >> 2) & 0x03FFFFFFu);
        }
        if (kind < 84) // jr / jalr through a register (usually junk)
        {
            if (rng.chance(50))
                return rtype(any(), 0, 0, 0, 0x08);
            const uint32_t source = rng.chance(60) ? work() : (rng.chance(50) ? 31u : rng.next(32));
            return rtype(source, 0, rng.chance(40) ? source : any(), 0, 0x09); // often rd == rs
        }
        if (kind < 90) // mult/div/hi/lo
        {
            static const uint32_t functs[] = {0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B};
            return rtype(any(), any(), any(), 0, functs[rng.next(8)]);
        }
        if (kind < 93) // COP0: mfc0/mtc0/rfe/others
        {
            switch (rng.next(4))
            {
            case 0: return (0x10u << 26) | (0u << 21) | (work() << 16) | ((12u + rng.next(3)) << 11);
            case 1: return (0x10u << 26) | (4u << 21) | (work() << 16) | (rng.chance(50) ? 14u : 12u) << 11;
            case 2: return 0x42000010u;
            default: return (0x10u << 26) | (rng.next(32) << 21) | (rng.next(1u << 21));
            }
        }
        if (kind < 95)
            return rtype(0, 0, 0, rng.next(1u << 10), rng.chance(50) ? 0x0Cu : 0x0Du); // syscall / break
        if (kind < 96)
            return 0x03E00008u; // jr ra (import stub candidate when followed by addiu zero)
        if (kind < 97)
            return 0x24000000u | rng.next(0x400u); // addiu zero, zero, ordinal
        if (kind < 98)
            return static_cast<uint32_t>(rng.gen()); // junk
        (void)index;
        return 0u; // nop
    }

    struct Machine
    {
        IopMemory memory;
        IopCpuCore core{memory};
        IopCpuState cpu;
    };

    void initMachine(Machine &m, uint64_t seed)
    {
        Rng rng(seed);
        std::vector<uint32_t> code(kCodeWords);
        for (uint32_t i = 0; i < kCodeWords; ++i)
            code[i] = randomInstruction(rng, i);
        std::vector<uint32_t> data(0x4000u);
        for (auto &w : data)
            w = static_cast<uint32_t>(rng.gen());
        (void)m.memory.writeRam(kCode, code.data(), code.size() * 4u);
        (void)m.memory.writeRam(kData, data.data(), data.size() * 4u);
        // exception vector (RAM at 0x80): back into the code region
        const uint32_t vec[] = {0x08000000u | ((kCode >> 2) & 0x03FFFFFFu), 0u};
        (void)m.memory.writeRam(0x80u, vec, sizeof(vec));
        m.cpu = IopCpuState{};
        m.cpu.pc = kCode;
        m.cpu.gpr[1] = kData + 0x4000u;
        m.cpu.gpr[2] = kData + 0x8001u; // misaligned base
        m.cpu.gpr[3] = 0x80000000u | (kData + 0x2000u); // KSEG0 alias
        for (uint32_t r = 4; r < 10; ++r)
            m.cpu.gpr[r] = static_cast<uint32_t>(rng.gen());
        m.cpu.gpr[29] = kData + 0xC000u;
        m.cpu.gpr[31] = kCode + 0x100u;
        m.cpu.cop0[12] = 0u;
    }

    bool sameState(const IopCpuState &a, const IopCpuState &b, std::string &why)
    {
        auto fail = [&](const char *what) { why = what; return false; };
        if (a.gpr != b.gpr) return fail("gpr");
        if (a.hi != b.hi) return fail("hi");
        if (a.lo != b.lo) return fail("lo");
        if (a.pc != b.pc) return fail("pc");
        if (a.cop0 != b.cop0) return fail("cop0");
        if (a.pendingLoad != b.pendingLoad) return fail("pendingLoad");
        if (a.pendingLoadReg != b.pendingLoadReg) return fail("pendingLoadReg");
        if (a.pendingLoadValue != b.pendingLoadValue) return fail("pendingLoadValue");
        if (a.branchPending != b.branchPending) return fail("branchPending");
        if (a.branchTarget != b.branchTarget) return fail("branchTarget");
        if (a.stopped != b.stopped) return fail("stopped");
        if (a.yielded != b.yielded) return fail("yielded");
        if (a.exception != b.exception) return fail("exception");
        return true;
    }

    // The ordinary per-instruction path with the checks step() makes that matter here (RAM-only fetch; interrupts
    // are never pending in this test).
    bool fixPc(Machine &m)
    {
        if ((m.cpu.pc & 0x1FFFFFFFu) >= IopMemory::RamSize)
        {
            m.cpu.pc = kCode;
            m.cpu.branchPending = false;
            return true;
        }
        return false;
    }

    int fuzz(uint64_t seed, uint64_t bursts, uint64_t &executed, uint64_t &burstInstr)
    {
        Machine ref, fast;
        initMachine(ref, seed);
        initMachine(fast, seed);
        Rng rng(seed * 7919u + 1u);
        std::string why;
        for (uint64_t it = 0; it < bursts; ++it)
        {
            const uint32_t limit = 1u + (rng.chance(10) ? rng.next(2000) : rng.next(40));
            bool bailed = false;
            uint32_t n = 0;
            // random walks collapse into short loops: re-seed pc and registers now and then, identically on both
            if (rng.chance(35))
            {
                const uint32_t pc = kCode + 4u * rng.next(kCodeWords - 64u);
                const bool delaySlot = rng.chance(15);
                const uint32_t target = kCode + 4u * rng.next(kCodeWords - 64u);
                const bool load = rng.chance(20);
                const uint32_t loadReg = 1u + rng.next(31);
                const uint32_t loadValue = static_cast<uint32_t>(rng.gen());
                uint32_t regs[6];
                for (auto &v : regs) v = static_cast<uint32_t>(rng.gen());
                const bool newRegs = rng.chance(50);
                for (Machine *m : {&ref, &fast})
                {
                    m->cpu.pc = pc;
                    m->cpu.branchPending = delaySlot;
                    m->cpu.branchTarget = target;
                    m->cpu.pendingLoad = load;
                    m->cpu.pendingLoadReg = loadReg;
                    m->cpu.pendingLoadValue = loadValue;
                    if (newRegs)
                        for (uint32_t i = 0; i < 6; ++i) m->cpu.gpr[4 + i] = regs[i];
                }
            }
            // pc sanity as step() does before any instruction: outside RAM / unaligned fetch -> both reset
            if (fixPc(fast)) (void)fixPc(ref);
            n = fast.core.runBurst(fast.cpu, limit, bailed);
            burstInstr += n;
            if (n > limit)
            {
                std::printf("seed %llu burst %llu: runBurst executed %u instructions with a limit of %u\n", (unsigned long long)seed, (unsigned long long)it, n, limit);
                return 1;
            }
            for (uint32_t i = 0; i < n; ++i)
                (void)ref.core.executeFetchedInstruction(ref.cpu, ref.memory.fetch32(ref.cpu.pc));
            executed += n;
            if (!sameState(ref.cpu, fast.cpu, why))
            {
                std::printf("seed %llu burst %llu (n=%u limit=%u bailed=%d): state mismatch in %s, pc ref=%08x fast=%08x\n",
                            (unsigned long long)seed, (unsigned long long)it, n, limit, bailed, why.c_str(), ref.cpu.pc, fast.cpu.pc);
                return 1;
            }
            if (bailed || n == 0)
            {
                // the bailed instruction runs the ordinary way on both
                const bool fixedFast = fixPc(fast);
                (void)fixPc(ref);
                if (!fixedFast)
                {
                    (void)fast.core.executeFetchedInstruction(fast.cpu, fast.memory.fetch32(fast.cpu.pc));
                    (void)ref.core.executeFetchedInstruction(ref.cpu, ref.memory.fetch32(ref.cpu.pc));
                    ++executed;
                }
                if (!sameState(ref.cpu, fast.cpu, why))
                {
                    std::printf("seed %llu burst %llu: slow-path mismatch in %s\n", (unsigned long long)seed, (unsigned long long)it, why.c_str());
                    return 1;
                }
            }
            if ((it & 255u) == 0u)
            {
                if (std::memcmp(ref.memory.ramData(), fast.memory.ramData(), IopMemory::RamSize) != 0 ||
                    std::memcmp(ref.memory.ownedData(), fast.memory.ownedData(), IopMemory::RamSize) != 0)
                {
                    std::printf("seed %llu burst %llu: RAM/owned mismatch\n", (unsigned long long)seed, (unsigned long long)it);
                    return 1;
                }
            }
        }
        if (std::memcmp(ref.memory.ramData(), fast.memory.ramData(), IopMemory::RamSize) != 0 ||
            std::memcmp(ref.memory.ownedData(), fast.memory.ownedData(), IopMemory::RamSize) != 0)
        {
            std::printf("seed %llu: final RAM/owned mismatch\n", (unsigned long long)seed);
            return 1;
        }
        return 0;
    }

    // resolve() with the memo must answer exactly as without it while export libraries come and go.
    int importCacheTest()
    {
        IopMemory memory;
        Rng rng(99);
        uint64_t checks = 0;
        // resolve(): same answers with and without the memo while libraries come and go
        auto exportTable = [&](uint32_t address, const char *name, uint16_t version, uint32_t base) {
            memory.write32(address, 0x41C00000u);
            memory.write16(address + 8u, version);
            for (uint32_t i = 0; i < 8; ++i) memory.write8(address + 12u + i, i < std::strlen(name) ? static_cast<uint8_t>(name[i]) : 0u);
            for (uint32_t k = 0; k < 6; ++k) memory.write32(address + 20u + 4u * k, base + k);
            memory.write32(address + 20u + 24u, 0u);
        };
        IopImportRegistry cached(memory), plain(memory);
        cached.setResolveCache(true);
        const char *libs[] = {"modhsyn", "MODHSYN", "sksound", "cri_adxi"};
        for (uint32_t round = 0; round < 4000; ++round)
        {
            const uint32_t slot = 0x40000u + 0x100u * rng.next(6);
            switch (rng.next(6))
            {
            case 0:
                exportTable(slot, libs[rng.next(4)], static_cast<uint16_t>(0x0100u + rng.next(3) + (rng.next(2) << 8)), 0x1000u * (round + 1));
                (void)cached.registerExportTable(slot);
                (void)plain.registerExportTable(slot);
                break;
            case 1:
                (void)cached.releaseExportTable(slot);
                (void)plain.releaseExportTable(slot);
                break;
            case 2:
                cached.eraseRange(0x40000u, 0x200u);
                plain.eraseRange(0x40000u, 0x200u);
                break;
            default:
                break;
            }
            const char *library = libs[rng.next(4)];
            const uint16_t ordinal = static_cast<uint16_t>(rng.next(8));
            const std::optional<uint16_t> version = rng.chance(50) ? std::optional<uint16_t>(static_cast<uint16_t>(0x0100u + rng.next(3) + (rng.next(2) << 8))) : std::nullopt;
            if (cached.resolve(library, ordinal, version) != plain.resolve(library, ordinal, version))
            {
                std::printf("resolve cache mismatch round %u\n", round);
                return 1;
            }
            ++checks;
        }
        std::printf("import resolve cache: %llu checks OK\n", static_cast<unsigned long long>(checks));
        return 0;
    }

    // Straight-line random code (no control flow, valid addresses) for timing: the dispatch sees an unpredictable
    // opcode sequence like real guest code does, unlike a tiny loop.
    uint32_t benchInstruction(Rng &rng)
    {
        auto work = [&]() { return 4u + rng.next(6); };
        const uint32_t kind = rng.next(100);
        if (kind < 28)
        {
            static const uint32_t functs[] = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x21, 0x21};
            return rtype(work(), work(), work(), rng.next(32), functs[rng.next(16)]);
        }
        if (kind < 52)
        {
            static const uint32_t ops[] = {0x09, 0x09, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
            return itype(ops[rng.next(9)], work(), work(), rng.next(0x10000));
        }
        if (kind < 90)
        {
            static const uint32_t ops[] = {0x23, 0x23, 0x23, 0x2B, 0x2B, 0x20, 0x24, 0x21, 0x25, 0x28, 0x29, 0x22, 0x26};
            const uint32_t op = ops[rng.next(13)];
            uint32_t off = rng.next(0x3000);
            if (op == 0x23 || op == 0x2B) off &= ~3u;
            if (op == 0x21 || op == 0x25 || op == 0x29) off &= ~1u;
            return itype(op, 1u + rng.next(3), work(), off);
        }
        static const uint32_t functs[] = {0x10, 0x11, 0x12, 0x13, 0x18, 0x19};
        return rtype(work(), work(), work(), 0, functs[rng.next(6)]);
    }

    uint64_t hostInstructions()
    {
#if defined(__APPLE__)
        rusage_info_v4 usage{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&usage)) == 0)
            return usage.ri_instructions;
#endif
        return 0;
    }

    void benchRun()
    {
        constexpr uint32_t words = 300000u;
        Machine ref, fast;
        for (Machine *m : {&ref, &fast})
        {
            Rng rng(5);
            std::vector<uint32_t> code(words + 2);
            for (uint32_t i = 0; i < words; ++i) code[i] = benchInstruction(rng);
            code[words] = 0x08000000u | ((kCode >> 2) & 0x03FFFFFFu); // j start
            code[words + 1] = 0u;
            (void)m->memory.writeRam(kCode, code.data(), code.size() * 4u);
            m->cpu = IopCpuState{};
            m->cpu.pc = kCode;
            m->cpu.gpr[1] = kData + 0x4000u;
            m->cpu.gpr[2] = kData + 0x8000u;
            m->cpu.gpr[3] = 0x80000000u | (kData + 0x2000u);
        }
        const uint64_t total = 60'000'000ull;
        auto t0 = std::chrono::steady_clock::now();
        uint64_t h0 = hostInstructions();
        for (uint64_t n = 0; n < total; ++n)
            (void)ref.core.executeFetchedInstruction(ref.cpu, ref.memory.fetch32(ref.cpu.pc));
        const double slow = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t slowHost = hostInstructions() - h0;
        t0 = std::chrono::steady_clock::now();
        h0 = hostInstructions();
        uint64_t done = 0;
        bool bailed = false;
        while (done < total)
        {
            const uint32_t n = fast.core.runBurst(fast.cpu, static_cast<uint32_t>(std::min<uint64_t>(256, total - done)), bailed);
            done += n;
            if (bailed || n == 0)
            {
                (void)fast.core.executeFetchedInstruction(fast.cpu, fast.memory.fetch32(fast.cpu.pc));
                ++done;
            }
        }
        const double fastTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t fastHost = hostInstructions() - h0;
        std::printf("host instructions per guest instruction: executeFetchedInstruction %.1f, runBurst %.1f\n", double(slowHost) / total, double(fastHost) / done);
        std::printf("random straight-line code, %llu instr: executeFetchedInstruction %.2f ns/instr, runBurst %.2f ns/instr (x%.2f)\n",
                    (unsigned long long)total, slow * 1e9 / total, fastTime * 1e9 / done, slow * done / (fastTime * total));
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "--bench")
    {
        benchRun();
        return 0;
    }
    uint64_t seeds = 40, bursts = 60000;
    if (argc > 1) seeds = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) bursts = std::strtoull(argv[2], nullptr, 10);
    if (importCacheTest() != 0)
        return 1;
    uint64_t executed = 0, burstInstr = 0;
    for (uint64_t seed = 1; seed <= seeds; ++seed)
        if (fuzz(seed, bursts, executed, burstInstr) != 0)
            return 1;
    std::printf("iop fast equivalence: %llu seeds, %llu instructions compared (%llu through the burst path): OK\n",
                (unsigned long long)seeds, (unsigned long long)executed, (unsigned long long)burstInstr);
    return 0;
}
