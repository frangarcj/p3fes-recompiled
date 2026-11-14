// A/B test of PS2X_IOP_FAST at the emulator level: the same synthetic IRX (a guest thread running a compute loop
// that uses RAM, SPU2 register stores, DelayThread and the import stubs) runs with the switch off and on; cycle and
// instruction counters after every runEeCycles call, guest memory and the log (including the PS2X_IOP_PC_EVERY
// samples) must be identical. With --bench it times both modes on a pure compute loop.
#include "iop_compat_test_support.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    using iop_test::Host;
    using ps2x::iop::IopSubsystem;

    void setEnv(const char *name, const char *value) { setenv(name, value, 1); }

    struct Asm
    {
        std::vector<uint32_t> w;
        uint32_t base;
        explicit Asm(uint32_t b) : base(b) {}
        uint32_t here() const { return base + 4u * static_cast<uint32_t>(w.size()); }
        void emit(uint32_t v) { w.push_back(v); }
        static uint32_t r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t f) { return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | f; }
        static uint32_t i(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFFu); }
        void lui(uint32_t rt, uint32_t v) { emit(i(0x0F, 0, rt, v)); }
        void ori(uint32_t rt, uint32_t rs, uint32_t v) { emit(i(0x0D, rs, rt, v)); }
        void addiu(uint32_t rt, uint32_t rs, int32_t v) { emit(i(0x09, rs, rt, static_cast<uint32_t>(v))); }
        void li(uint32_t rt, uint32_t v) { lui(rt, v >> 16); ori(rt, rt, v & 0xFFFF); }
        void addu(uint32_t rd, uint32_t rs, uint32_t rt) { emit(r(rs, rt, rd, 0, 0x21)); }
        void xorr(uint32_t rd, uint32_t rs, uint32_t rt) { emit(r(rs, rt, rd, 0, 0x26)); }
        void slt(uint32_t rd, uint32_t rs, uint32_t rt) { emit(r(rs, rt, rd, 0, 0x2A)); }
        void sll(uint32_t rd, uint32_t rt, uint32_t sa) { emit(r(0, rt, rd, sa, 0x00)); }
        void srl(uint32_t rd, uint32_t rt, uint32_t sa) { emit(r(0, rt, rd, sa, 0x02)); }
        void andi(uint32_t rt, uint32_t rs, uint32_t v) { emit(i(0x0C, rs, rt, v)); }
        void mult(uint32_t rs, uint32_t rt) { emit(r(rs, rt, 0, 0, 0x18)); }
        void divu(uint32_t rs, uint32_t rt) { emit(r(rs, rt, 0, 0, 0x1B)); }
        void mflo(uint32_t rd) { emit(r(0, 0, rd, 0, 0x12)); }
        void mfhi(uint32_t rd) { emit(r(0, 0, rd, 0, 0x10)); }
        void lw(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x23, rs, rt, static_cast<uint32_t>(o))); }
        void sw(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x2B, rs, rt, static_cast<uint32_t>(o))); }
        void lwl(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x22, rs, rt, static_cast<uint32_t>(o))); }
        void lwr(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x26, rs, rt, static_cast<uint32_t>(o))); }
        void sh(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x29, rs, rt, static_cast<uint32_t>(o))); }
        void sb(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x28, rs, rt, static_cast<uint32_t>(o))); }
        void lbu(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x24, rs, rt, static_cast<uint32_t>(o))); }
        void lh(uint32_t rt, uint32_t rs, int32_t o) { emit(i(0x21, rs, rt, static_cast<uint32_t>(o))); }
        void nop() { emit(0u); }
        void jal(uint32_t target) { emit(0x0C000000u | ((target >> 2) & 0x03FFFFFFu)); }
        void jr(uint32_t rs) { emit(r(rs, 0, 0, 0, 0x08)); }
        // branch to an absolute target
        void bne(uint32_t rs, uint32_t rt, uint32_t target) { emit(i(0x05, rs, rt, static_cast<uint32_t>((static_cast<int32_t>(target) - static_cast<int32_t>(here() + 4u)) >> 2))); }
        void beq(uint32_t rs, uint32_t rt, uint32_t target) { emit(i(0x04, rs, rt, static_cast<uint32_t>((static_cast<int32_t>(target) - static_cast<int32_t>(here() + 4u)) >> 2))); }
        // forward branch fixup
        size_t bnePlaceholder(uint32_t rs, uint32_t rt) { emit(i(0x05, rs, rt, 0)); return w.size() - 1; }
        void fix(size_t at) { w[at] |= static_cast<uint32_t>((static_cast<int32_t>(here()) - static_cast<int32_t>(base + 4u * at + 4u)) >> 2) & 0xFFFFu; }
    };

    struct Result
    {
        std::vector<uint64_t> cycles, instructions;
        std::vector<uint32_t> memory;
        std::vector<std::string> logs;
        double seconds = 0.0;
    };

    // computeOnly: a pure register/RAM loop, no imports, for timing; otherwise the mixed loop.
    Result run(bool fast, bool computeOnly, uint64_t eeChunk, uint32_t chunks, bool idle = false)
    {
        setEnv("PS2X_IOP_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_PC_EVERY", (computeOnly || idle) ? "0" : "997");
        setEnv("PS2X_IOP_WORK_DIAG", "0");
        Host host;
        iop_test::Irx irx(0x10000u, 0x1000u);
        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t load = 0x10000u;
        constexpr uint32_t worker = load + 0x100u;
        constexpr uint32_t thbase = load + 0x800u;
        constexpr uint32_t createStub = thbase + 20u;
        constexpr uint32_t startStub = createStub + 8u;
        constexpr uint32_t delayStub = startStub + 8u;
        constexpr uint32_t buffer = 0x12000u;

        irx.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840A00u, jal(createStub), 0u,
            0x00404021u, 0x01002021u, 0x00002821u,
            jal(startStub), 0u,
            0x8FBF001Cu, 0x27BD0020u, 0x03E00008u, 0u
        });
        irx.words(0x800u, {
            0x41E00000u, 0u, 0x00000101u, 0x61626874u, 0x00006573u,
            0x03E00008u, 0x24000004u, 0x03E00008u, 0x24000006u, 0x03E00008u, 0x24000021u, 0u, 0u
        });
        irx.words(0xA00u, {0u, 0u, worker, 0x400u, 10u});

        Asm a(worker);
        enum { s0 = 16, s1, s2, s3, t0 = 8, t1, t2, t3, t4, t5, t6, t7 };
        a.li(s0, buffer);
        a.addiu(s1, 0, 0);
        a.li(s2, 0x12345678u);
        a.li(s3, 0x1F900180u);
        a.li(t7, 1103515245u);
        const uint32_t loop = a.here();
        a.mult(s2, t7);                       // lcg
        a.mflo(s2);
        a.addiu(s2, s2, 12345);
        a.andi(t1, s1, 0xFF);
        a.sll(t1, t1, 2);
        a.addu(t2, s0, t1);
        a.lw(t3, t2, 0);
        a.xorr(t3, t3, s2);
        a.sw(t3, t2, 0);
        a.lwl(t4, t2, 1);                     // load-delay + unaligned merge hazards
        a.lwr(t4, t2, 4);
        a.sh(t4, t2, 2);
        a.sb(t4, t2, 5);
        a.lbu(t5, t2, 5);
        a.lh(t6, t2, 2);
        a.slt(t0, t3, s2);
        const size_t skip = a.bnePlaceholder(t0, 0);
        a.addu(t5, t5, t6);                   // delay slot
        a.srl(t5, t5, 3);
        a.divu(s2, t5);
        a.mfhi(t6);
        a.sw(t6, t2, 8);
        a.fix(skip);
        a.addiu(s1, s1, 1);
        if (!computeOnly)
        {
            a.andi(t0, s1, 7);
            const size_t noSpu = a.bnePlaceholder(t0, 0);
            a.nop();
            a.sh(s2, s3, 0);                  // SPU2 register store: leaves the burst path
            a.lw(t6, s3, 4);                  // and a hardware load
            a.fix(noSpu);
            a.andi(t0, s1, idle ? 63u : 31u);
            const size_t noDelay = a.bnePlaceholder(t0, 0);
            a.nop();
            a.li(4, idle ? 4166u : 1u);
            a.jal(delayStub);                 // DelayThread(1): import + thread switch
            a.nop();
            a.fix(noDelay);
        }
        a.beq(0, 0, loop);
        a.nop();
        for (uint32_t k = 0; k < a.w.size(); ++k)
            irx.words(0x100u + 4u * k, {a.w[k]});
        irx.install(host);

        IopSubsystem iop(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        iop_test::require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic IRX failed to start");
        Result result;
        const auto t0c = std::chrono::steady_clock::now();
        for (uint32_t c = 0; c < chunks; ++c)
        {
            iop.runEeCycles(eeChunk + ((computeOnly || idle) ? 0u : (c % 7u) * 13u));
            if (!computeOnly && !idle)
            {
                const auto snapshot = iop.debugSnapshot();
                result.cycles.push_back(snapshot.emulatorCycles);
                result.instructions.push_back(snapshot.emulatorInstructions);
            }
        }
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0c).count();
        const auto snapshot = iop.debugSnapshot();
        result.cycles.push_back(snapshot.emulatorCycles);
        result.instructions.push_back(snapshot.emulatorInstructions);
        result.memory.resize(0x400u);
        iop_test::require(iop.readMemory(buffer, result.memory.data(), result.memory.size() * 4u), "read buffer");
        // [IOP:idle] uses a process-wide static counter, so it only fires in the first run of the process
        for (const auto &line : host.logs)
            if (line.find("[IOP:idle]") == std::string::npos)
                result.logs.push_back(line);
        return result;
    }

    bool same(const Result &x, const Result &y, const char *what)
    {
        if (x.cycles != y.cycles) { std::cout << what << ": cycle counters differ\n"; return false; }
        if (x.instructions != y.instructions) { std::cout << what << ": instruction counters differ\n"; return false; }
        if (x.memory != y.memory) { std::cout << what << ": guest memory differs\n"; return false; }
        if (x.logs != y.logs)
        {
            std::cout << what << ": logs differ (" << x.logs.size() << " vs " << y.logs.size() << ")\n";
            for (size_t k = 0; k < std::max(x.logs.size(), y.logs.size()); ++k)
            {
                const std::string a = k < x.logs.size() ? x.logs[k] : "<none>", b = k < y.logs.size() ? y.logs[k] : "<none>";
                if (a != b) std::cout << "  [" << k << "] off: " << a << "\n  [" << k << "] on : " << b << "\n";
            }
            return false;
        }
        return true;
    }
}

int main(int argc, char **argv)
{
    try
    {
        bool ok = true;
        // several EE chunk sizes (the real EE feeds the IOP in small, irregular pieces)
        for (uint64_t chunk : {8ull * 7ull, 8ull * 40ull, 8ull * 700ull, 8ull * 5000ull})
        {
            const Result slow = run(false, false, chunk, 600);
            const Result fast = run(true, false, chunk, 600);
            std::cout << "chunk " << chunk << ": " << slow.instructions.back() << " instructions, " << slow.logs.size() << " log lines\n";
            bool pcLines = false;
            for (const auto &l : slow.logs) pcLines |= l.find("[IOP:pc]") != std::string::npos;
            iop_test::require(pcLines && slow.instructions.back() > 1000u, "test program did not run");
            if (!same(slow, fast, "mixed loop"))
                ok = false;
        }
        if (argc > 1 && std::string(argv[1]) == "--bench")
        {
            const uint32_t chunks = 20000;
            const uint64_t chunk = 8ull * 2000ull;
            for (int rep = 0; rep < 3; ++rep)
            {
                const Result slow = run(false, true, chunk, chunks);
                const Result fast = run(true, true, chunk, chunks);
                const double instr = static_cast<double>(slow.instructions.back());
                std::cout << "bench: " << instr / 1e6 << " M instructions; off " << slow.seconds * 1e9 / instr << " ns/instr, on "
                          << fast.seconds * 1e9 / instr << " ns/instr (x" << slow.seconds / fast.seconds << ")\n";
                iop_test::require(slow.instructions == fast.instructions, "bench: instruction counts differ");
            }
        }
        if (argc > 1 && std::string(argv[1]) == "--bench-idle")
        {
            // like the game: a driver thread that works a little, then sleeps 4.2 ms (240 Hz); the EE feeds the
            // IOP ~2047 IOP cycles at a time. Cost per runEeCycles call, warm caches.
            const uint32_t chunks = 200000;
            const uint64_t chunk = 8ull * 2047ull;
            for (int rep = 0; rep < 3; ++rep)
            {
                const Result slow = run(false, false, chunk, chunks, true);
                const Result fast = run(true, false, chunk, chunks, true);
                std::cout << "idle bench: " << slow.instructions.back() << " instr; off " << slow.seconds * 1e9 / chunks << " ns/call, on "
                          << fast.seconds * 1e9 / chunks << " ns/call; cycles " << (slow.cycles.back() == fast.cycles.back() ? "same" : "DIFFER") << "\n";
            }
        }
        std::cout << (ok ? "iop fast integration: OK\n" : "iop fast integration: FAILED\n");
        return ok ? 0 : 1;
    }
    catch (const std::exception &error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
