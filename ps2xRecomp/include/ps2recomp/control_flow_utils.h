#ifndef PS2RECOMP_CONTROL_FLOW_UTILS_H
#define PS2RECOMP_CONTROL_FLOW_UTILS_H

#include "ps2recomp/instructions.h"
#include "ps2recomp/types.h"

#include <cstdint>
#include <string_view>

namespace ps2recomp
{
    inline uint32_t buildAbsoluteJumpTarget(uint32_t address, uint32_t target) noexcept
    {
        return ((address + 4u) & 0xF0000000u) | (target << 2);
    }

    // True when the generated code for an instruction can reach the runtime
    // (syscalls, traps, exceptions, MMIO handlers, VU0 microprograms) and so
    // needs the guest PC and branch-delay state stored in the context first.
    // Plain RAM accesses are not listed: their out-of-line path stores the PC
    // itself through PS2_SYNC_PC.
    inline bool translationObservesGuestPc(std::string_view code) noexcept
    {
        static constexpr std::string_view markers[] = {
            "runtime->", "runtime)", "ctx->pc", "throw ", "ps2_syscalls::", "ps2_stubs::",
            "SignalException", "handleTrap", "handleBreak", "handleSyscall"};
        for (std::string_view marker : markers)
        {
            if (code.find(marker) != std::string_view::npos)
            {
                return true;
            }
        }
        return false;
    }

    inline bool isGuestNop(const Instruction &inst) noexcept
    {
        const bool sllZero =
            inst.opcode == OPCODE_SPECIAL &&
            inst.function == SPECIAL_SLL &&
            inst.rd == 0u &&
            inst.rt == 0u &&
            inst.sa == 0u;

        // Some tests and decoded streams represent a no-op as addiu $zero, $zero, 0.
        const bool addiuZero =
            inst.opcode == OPCODE_ADDIU &&
            inst.rs == 0u &&
            inst.rt == 0u &&
            static_cast<int16_t>(inst.simmediate) == 0;

        return sllZero || addiuZero;
    }
}

#endif // PS2RECOMP_CONTROL_FLOW_UTILS_H
