#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace ps2x::iop::detail
{
    class IopMemory;

    struct IopCpuState
    {
        std::array<uint32_t, 32> gpr{};
        uint32_t hi = 0;
        uint32_t lo = 0;
        uint32_t pc = 0;
        std::array<uint32_t, 32> cop0{};
        uint32_t pendingLoadReg = 0;
        uint32_t pendingLoadValue = 0;
        bool pendingLoad = false;
        bool branchPending = false;
        uint32_t branchTarget = 0;
        bool stopped = false;
        bool yielded = false;
        bool exception = false;
    };

    class IopCpuCore
    {
    public:
        explicit IopCpuCore(IopMemory &memory) noexcept;

        [[nodiscard]] bool executeInstruction(IopCpuState &cpu);
        // The emulator already fetches at cpu.pc to recognize import stubs.
        // Execute that same word after interrupt handling, without fetching twice.
        [[nodiscard]] bool executeFetchedInstruction(IopCpuState &cpu, uint32_t instruction);
        // PS2X_IOP_FAST: executes up to `limit` consecutive instructions with the PC, branch/load-delay state and
        // registers held in locals. Only instructions that cannot touch anything outside the CPU registers and RAM
        // are executed (no COP0, no exceptions, no hardware/scratchpad/unaligned accesses, no possible import stub
        // `jr ra`, no fetch outside RAM): at the first other instruction it returns WITHOUT having changed any state
        // for it (bailed = true) so the caller runs it through the ordinary path. Returns the instructions executed.
        // Interrupt/import/sentinel/DMA checks are the caller's job and must be no-ops for the whole burst.
        [[nodiscard]] uint32_t runBurst(IopCpuState &cpu, uint32_t limit, bool &bailed);
        void raiseException(IopCpuState &cpu, uint32_t code, uint32_t faultPc, bool delaySlot, std::optional<uint32_t> badAddress = std::nullopt) const;

    private:
        static void writeRegister(IopCpuState &cpu, uint32_t reg, uint32_t value, uint32_t &writtenReg);
        static void scheduleLoad(uint32_t reg, uint32_t value, bool &scheduled, uint32_t &scheduledReg, uint32_t &scheduledValue);

        IopMemory &m_memory;
    };
}
