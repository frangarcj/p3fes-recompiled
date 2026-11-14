#include "iop_compat_test_support.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <cstdlib>
#endif

namespace
{
    using iop_test::Host;
    using ps2x::iop::IopSubsystem;

    void setEnv(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }

    struct RunResult
    {
        uint64_t cycles = 0u;
        uint64_t instructions = 0u;
        std::vector<int16_t> pcm;
        size_t pendingAudio = 0u;
        size_t audioCommands = 0u;
        size_t guestWrites = 0u;
        size_t memoryCardCalls = 0u;
        std::vector<std::string> logs;
    };

    struct DelayedRunResult
    {
        std::vector<uint64_t> cycles;
        std::vector<uint64_t> instructions;
        std::vector<uint32_t> marks;
        std::vector<size_t> pendingAudio;
    };

    DelayedRunResult runDelayedThread(bool fast)
    {
        setEnv("PS2X_IOP_IDLE_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_WORK_DIAG", "0");
        Host host;
        iop_test::Irx irx(0x10000u, 0x600u);
        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t load = 0x10000u;
        constexpr uint32_t worker = load + 0x100u;
        constexpr uint32_t thbase = load + 0x180u;
        constexpr uint32_t createStub = thbase + 20u;
        constexpr uint32_t startStub = createStub + 8u;
        constexpr uint32_t delayStub = startStub + 8u;

        irx.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu,                         // save ra
            0x3C040001u, 0x34840400u, jal(createStub), 0u,  // CreateThread(&descriptor)
            0x00404021u, 0x01002021u, 0x00002821u,          // id -> a0, args = 0
            jal(startStub), 0u,                             // StartThread(id, 0)
            0x8FBF001Cu, 0x27BD0020u, 0x03E00008u, 0u      // return from module start
        });
        irx.words(0x100u, {
            0x27BDFFF0u, 0xAFBF000Cu,                      // save ra
            0x24040001u, jal(delayStub), 0u,              // DelayThread(1 us)
            0x3C080001u, 0x35080500u,                     // t0 = marker address
            0x3C09C0DEu, 0x3529F00Du, 0xAD090000u,        // write marker 0xC0DEF00D
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0u     // return from thread
        });
        irx.words(0x180u, {
            0x41E00000u, 0u, 0x00000101u, 0x61626874u, 0x00006573u, // thbase
            0x03E00008u, 0x24000004u, // CreateThread
            0x03E00008u, 0x24000006u, // StartThread
            0x03E00008u, 0x24000021u, // DelayThread
            0u, 0u
        });
        irx.words(0x400u, {0u, 0u, worker, 0x400u, 10u});
        irx.install(host);

        IopSubsystem iop(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        iop_test::require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic delay IRX failed to start");

        DelayedRunResult result;
        for (uint32_t step = 0u; step < 40u; ++step)
        {
            iop.runEeCycles(8u * 4u); // Four IOP cycles per fragment, crossing the 1 us wake boundary gradually.
            const auto snapshot = iop.debugSnapshot();
            uint32_t marker = 0u;
            iop_test::require(iop.readMemory(0x10500u, &marker, sizeof(marker)), "could not read delay marker");
            result.cycles.push_back(snapshot.emulatorCycles);
            result.instructions.push_back(snapshot.emulatorInstructions);
            result.marks.push_back(marker);
            result.pendingAudio.push_back(iop.pendingAudioFrames());
            if (marker == 0xC0DEF00Du)
                break;
        }
        iop_test::require(!result.marks.empty() && result.marks.back() == 0xC0DEF00Du,
                          "delayed IOP thread did not write its marker");
        return result;
    }

    DelayedRunResult runGuestCallback(bool fast)
    {
        setEnv("PS2X_IOP_IDLE_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_WORK_DIAG", "0");
        Host host;
        iop_test::Irx irx(0x10000u, 0x600u);
        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t load = 0x10000u;
        constexpr uint32_t callback = load + 0x100u;
        constexpr uint32_t sifman = load + 0x200u;
        constexpr uint32_t dmaIntrStub = sifman + 20u;
        constexpr uint32_t descriptor = load + 0x300u;
        constexpr uint32_t payload = load + 0x320u;

        irx.words(0x00u, {
            0x27BDFFF0u, 0xAFBF000Cu,
            0x3C040001u, 0x34840300u, 0x24050001u,
            0x3C060001u, 0x34C60100u, 0x24070055u,
            jal(dmaIntrStub), 0u,
            0x00001021u,                                 // start routine returns zero, not the DMA id
            0x8FBF000Cu, 0x27BD0010u, 0x03E00008u, 0u
        });
        irx.words(0x100u, {
            0x3C080001u, 0x35080500u, 0x3C09ABCDu, 0x35291234u,
            0xAD090000u, 0x03E00008u, 0u
        });
        irx.words(0x200u, {
            0x41E00000u, 0u, 0x00000101u, 0x6D666973u, 0x00006E61u, // sifman
            0x03E00008u, 0x24000020u, 0u, 0u, // sceSifSetDmaIntr
            0u, 0u
        });
        irx.words(0x300u, {payload, 0xC00u, sizeof(uint32_t), 0u});
        irx.words(0x320u, {0x53494621u});
        irx.install(host);

        IopSubsystem iop(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        iop_test::require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic SIF callback IRX failed to start");
        const uint32_t copied = host.word(0xC00u);
        iop_test::require(copied == 0x53494621u, "synthetic SIF DMA did not copy its payload");

        DelayedRunResult result;
        for (const uint32_t fragment : {1000u, 500u, 600u})
        {
            iop.runEeCycles(static_cast<uint64_t>(fragment) * 8u);
            const auto snapshot = iop.debugSnapshot();
            uint32_t marker = 0u;
            iop_test::require(iop.readMemory(0x10500u, &marker, sizeof(marker)), "could not read callback marker");
            result.cycles.push_back(snapshot.emulatorCycles);
            result.instructions.push_back(snapshot.emulatorInstructions);
            result.marks.push_back(marker);
            result.pendingAudio.push_back(iop.pendingAudioFrames());
        }
        iop_test::require(result.marks[0] == 0u && result.marks[1] == 0u && result.marks[2] == 0xABCD1234u,
                          "SIF completion callback did not run at its scheduled cycle");
        return result;
    }

    DelayedRunResult runTimerCallback(bool fast)
    {
        setEnv("PS2X_IOP_IDLE_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_WORK_DIAG", "0");
        Host host;
        iop_test::Irx irx(0x10000u, 0x600u);
        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t load = 0x10000u;
        constexpr uint32_t callback = load + 0x100u;
        constexpr uint32_t timrman = load + 0x200u;
        constexpr uint32_t allocStub = timrman + 20u;
        constexpr uint32_t handlerStub = allocStub + 8u;
        constexpr uint32_t setupStub = handlerStub + 8u;
        constexpr uint32_t startStub = setupStub + 8u;

        irx.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu, 0xAFB00018u,       // save ra/s0
            0x24040001u, 0x24050020u, 0x24060001u,      // AllocHardTimer(SYSCLK, 32, prescale 1)
            jal(allocStub), 0u, 0x00408021u,            // s0 = timer id
            0x02002021u, 0x24050064u,                  // a0=id, compare=100
            0x3C060001u, 0x34C60100u, 0x00003821u,     // callback, common=0
            jal(handlerStub), 0u,
            0x02002021u, 0x24050001u, 0x00003021u, 0x24070001u,
            jal(setupStub), 0u,                        // SetupHardTimer(id, source=1, mode=0, prescale=1)
            0x02002021u, jal(startStub), 0u,           // StartHardTimer(id)
            0x8FB00018u, 0x8FBF001Cu, 0x27BD0020u, 0x03E00008u, 0u
        });
        irx.words(0x100u, {
            0x3C080001u, 0x35080500u, 0x3C0971AEu, 0x3529BEEFu,
            0xAD090000u, 0x03E00008u, 0u
        });
        irx.words(0x200u, {
            0x41E00000u, 0u, 0x00000101u, 0x726D6974u, 0x006E616Du, // timrman
            0x03E00008u, 0x24000004u, // AllocHardTimer
            0x03E00008u, 0x24000014u, // SetTimerHandler
            0x03E00008u, 0x24000016u, // SetupHardTimer
            0x03E00008u, 0x24000017u, // StartHardTimer
            0u, 0u
        });
        irx.install(host);

        IopSubsystem iop(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        iop_test::require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic timer IRX failed to start");

        DelayedRunResult result;
        for (const uint32_t fragment : {50u, 30u, 30u})
        {
            iop.runEeCycles(static_cast<uint64_t>(fragment) * 8u);
            const auto snapshot = iop.debugSnapshot();
            uint32_t marker = 0u;
            iop_test::require(iop.readMemory(0x10500u, &marker, sizeof(marker)), "could not read timer marker");
            result.cycles.push_back(snapshot.emulatorCycles);
            result.instructions.push_back(snapshot.emulatorInstructions);
            result.marks.push_back(marker);
            result.pendingAudio.push_back(iop.pendingAudioFrames());
        }
        iop_test::require(result.marks[0] == 0u && result.marks[1] == 0u && result.marks[2] == 0x71AEBEEFu,
                          "timrman callback did not run at its scheduled cycle");
        return result;
    }

    DelayedRunResult runDmaInterrupt(bool fast)
    {
        setEnv("PS2X_IOP_IDLE_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_WORK_DIAG", "0");
        Host host;
        iop_test::Irx irx(0x10000u, 0x600u);
        const auto jal = [](uint32_t target) { return 0x0C000000u | ((target >> 2u) & 0x03FFFFFFu); };
        constexpr uint32_t load = 0x10000u;
        constexpr uint32_t handler = load + 0x100u;
        constexpr uint32_t intrman = load + 0x200u;
        constexpr uint32_t registerStub = intrman + 20u;
        constexpr uint32_t enableStub = registerStub + 8u;

        irx.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x24040024u, 0x00002821u, 0x3C060001u, 0x34C60100u, 0x00003821u,
            jal(registerStub), 0u,                         // RegisterIntrHandler(0x24, 0, handler, 0)
            0x24040024u, jal(enableStub), 0u,              // EnableIntr(0x24)
            0x3C081F80u, 0x350810C0u,                     // DMA SPU0 register block
            0x3C090001u, 0x35290580u, 0xAD090000u,       // MADR = 0x10580
            0x24090001u, 0xAD090004u,                     // BCR = one word
            0x3C090100u, 0x35290001u, 0xAD090008u,       // CHCR = start, memory-to-SPU
            0x8FBF001Cu, 0x27BD0020u, 0x03E00008u, 0u
        });
        irx.words(0x100u, {
            0x3C080001u, 0x35080500u, 0x3C091234u, 0x35295678u,
            0xAD090000u, 0x03E00008u, 0u
        });
        irx.words(0x200u, {
            0x41E00000u, 0u, 0x00000101u, 0x72746E69u, 0x006E616Du, // intrman
            0x03E00008u, 0x24000004u, // RegisterIntrHandler
            0x03E00008u, 0x24000006u, // EnableIntr
            0u, 0u
        });
        irx.words(0x580u, {0x12345678u});
        irx.install(host);

        IopSubsystem iop(host);
        const auto loaded = iop.loadModuleBuffer(0x1000u);
        iop_test::require(loaded.moduleId > 0 && loaded.startResult == 0, "synthetic DMA IRQ IRX failed to start");

        DelayedRunResult result;
        for (const uint32_t fragment : {24u, 24u, 24u})
        {
            iop.runEeCycles(static_cast<uint64_t>(fragment) * 8u);
            const auto snapshot = iop.debugSnapshot();
            uint32_t marker = 0u;
            iop_test::require(iop.readMemory(0x10500u, &marker, sizeof(marker)), "could not read DMA marker");
            result.cycles.push_back(snapshot.emulatorCycles);
            result.instructions.push_back(snapshot.emulatorInstructions);
            result.marks.push_back(marker);
            result.pendingAudio.push_back(iop.pendingAudioFrames());
        }
        iop_test::require(result.marks[0] == 0u && result.marks[1] == 0u && result.marks[2] == 0x12345678u,
                          "SPU DMA interrupt was not delivered at its deadline");
        return result;
    }

    RunResult run(bool fast, bool diagnostics)
    {
        setEnv("PS2X_IOP_IDLE_FAST", fast ? "1" : "0");
        setEnv("PS2X_IOP_WORK_DIAG", diagnostics ? "1" : "0");
        Host host;
        IopSubsystem iop(host);

        // The first call populates the cached idle state. Then many one-cycle
        // fragments exercise the guarded fast path; the final call completes
        // exactly six emulated seconds total.
        constexpr uint64_t kThreeSecondsIop = 3ull * 48000ull * 768ull;
        iop.runEeCycles(kThreeSecondsIop * 8ull);
        constexpr uint64_t kOneCycleFragments = 1000u;
        for (uint64_t cycle = 0u; cycle < kOneCycleFragments; ++cycle)
            iop.runEeCycles(8u);
        iop.runEeCycles((kThreeSecondsIop - kOneCycleFragments) * 8ull);

        RunResult result;
        const auto snapshot = iop.debugSnapshot();
        result.cycles = snapshot.emulatorCycles;
        result.instructions = snapshot.emulatorInstructions;
        result.pendingAudio = iop.pendingAudioFrames();
        result.pcm.resize(result.pendingAudio * 2u);
        if (!result.pcm.empty())
            (void)iop.drainAudio(result.pcm.data(), result.pendingAudio);
        result.audioCommands = host.audioCalls;
        result.guestWrites = host.guestWrites;
        result.memoryCardCalls = host.cardCalls.size();
        result.logs = host.logs;
        return result;
    }
}

int main()
{
    {
        ps2x::iop::detail::IopMemory memory;
        ps2x::iop::detail::IopKernel kernel(memory);
        ps2x::iop::detail::IopCpuState importCpu{};
        iop_test::require(kernel.beginNextReady(0u) == nullptr, "empty kernel should cache idle");
        iop_test::require(kernel.cachedIdleCovers(0u, 100u), "idle cache should cover target before any wake");

        constexpr uint32_t descriptor = 0x100u;
        memory.write32(descriptor + 8u, 0x200u);
        memory.write32(descriptor + 12u, 0x400u);
        memory.write32(descriptor + 16u, 32u);
        importCpu.gpr[4] = descriptor;
        iop_test::require(kernel.dispatchThreadImport(4u, importCpu, 0u), "CreateThread import not handled");
        iop_test::require(!kernel.cachedIdleCovers(0u, 100u), "thread creation must invalidate idle cache");
        importCpu.gpr[4] = importCpu.gpr[2];
        iop_test::require(kernel.dispatchThreadImport(6u, importCpu, 0u), "StartThread import not handled");
        auto *thread = kernel.beginNextReady(0u);
        iop_test::require(thread != nullptr, "started thread must be ready");
        ps2x::iop::detail::IopCpuState suspended{};
        kernel.delayCurrentUntil(10u, suspended);
        kernel.endTimeslice(*thread, 0xFFFFFFFFu);
        iop_test::require(kernel.beginNextReady(0u) == nullptr, "delayed thread should leave kernel idle");
        iop_test::require(kernel.cachedIdleCovers(0u, 10u), "idle cache should include exact wake boundary");
        iop_test::require(!kernel.cachedIdleCovers(0u, 11u), "idle cache must reject targets past wake boundary");
        iop_test::require(!kernel.cachedIdleCovers(10u, 10u), "idle cache is invalid at the wake cycle itself");
    }

    constexpr bool kDiagnosticsBuild =
#if defined(PS2X_IOP_WORK_DIAGNOSTICS)
        true;
#else
        false;
#endif

    const RunResult baseline = run(false, kDiagnosticsBuild);
    const RunResult fast = run(true, kDiagnosticsBuild);
    if (baseline.cycles != fast.cycles || baseline.instructions != fast.instructions ||
        baseline.pendingAudio != fast.pendingAudio || baseline.pcm != fast.pcm ||
        baseline.audioCommands != fast.audioCommands || baseline.guestWrites != fast.guestWrites ||
        baseline.memoryCardCalls != fast.memoryCardCalls)
        return 1;

    const DelayedRunResult delayedBaseline = runDelayedThread(false);
    const DelayedRunResult delayedFast = runDelayedThread(true);
    if (delayedBaseline.cycles != delayedFast.cycles ||
        delayedBaseline.instructions != delayedFast.instructions ||
        delayedBaseline.marks != delayedFast.marks ||
        delayedBaseline.pendingAudio != delayedFast.pendingAudio)
        return 3;

    const DelayedRunResult callbackBaseline = runGuestCallback(false);
    const DelayedRunResult callbackFast = runGuestCallback(true);
    if (callbackBaseline.cycles != callbackFast.cycles ||
        callbackBaseline.instructions != callbackFast.instructions ||
        callbackBaseline.marks != callbackFast.marks ||
        callbackBaseline.pendingAudio != callbackFast.pendingAudio)
        return 4;

    const DelayedRunResult timerBaseline = runTimerCallback(false);
    const DelayedRunResult timerFast = runTimerCallback(true);
    if (timerBaseline.cycles != timerFast.cycles ||
        timerBaseline.instructions != timerFast.instructions ||
        timerBaseline.marks != timerFast.marks ||
        timerBaseline.pendingAudio != timerFast.pendingAudio)
        return 5;

    const DelayedRunResult dmaBaseline = runDmaInterrupt(false);
    const DelayedRunResult dmaFast = runDmaInterrupt(true);
    if (dmaBaseline.cycles != dmaFast.cycles ||
        dmaBaseline.instructions != dmaFast.instructions ||
        dmaBaseline.marks != dmaFast.marks ||
        dmaBaseline.pendingAudio != dmaFast.pendingAudio)
        return 6;

    if (kDiagnosticsBuild)
    {
        const auto hasFastCount = [](const RunResult &result, bool expectedPositive)
        {
            const auto it = std::find_if(result.logs.begin(), result.logs.end(), [](const std::string &line)
            {
                return line.find("[IOP:work]") != std::string::npos;
            });
            if (it == result.logs.end())
                return false;
            const size_t field = it->find("idle_fast=");
            if (field == std::string::npos)
                return false;
            const size_t valueStart = field + sizeof("idle_fast=") - 1u;
            const bool positive = valueStart < it->size() && (*it)[valueStart] >= '1' && (*it)[valueStart] <= '9';
            return positive == expectedPositive;
        };
        if (!hasFastCount(baseline, false) || !hasFastCount(fast, true))
            return 2;
    }

    return 0;
}
