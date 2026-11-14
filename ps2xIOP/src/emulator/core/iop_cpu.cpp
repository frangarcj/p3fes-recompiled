#include "iop_cpu.h"
#include "iop_memory.h"

#include <cstring>
#include <limits>

namespace ps2x::iop::detail
{
    IopCpuCore::IopCpuCore(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopCpuCore::writeRegister(IopCpuState &cpu, uint32_t reg, uint32_t value, uint32_t &writtenReg)
    {
        if (reg == 0u)
            return;
        cpu.gpr[reg] = value;
        writtenReg = reg;
    }

    void IopCpuCore::scheduleLoad(uint32_t reg, uint32_t value, bool &scheduled, uint32_t &scheduledReg, uint32_t &scheduledValue)
    {
        if (reg == 0u)
            return;
        scheduled = true;
        scheduledReg = reg;
        scheduledValue = value;
    }

    void IopCpuCore::raiseException(IopCpuState &cpu, uint32_t code, uint32_t faultPc, bool delaySlot, std::optional<uint32_t> badAddress) const
    {
        uint32_t cause = cpu.cop0[13] & ~0x7Cu;
        cause |= (code & 0x1Fu) << 2u;
        if (delaySlot)
        {
            cause |= 0x80000000u;
            cpu.cop0[14] = faultPc - 4u;
        }
        else
        {
            cause &= ~0x80000000u;
            cpu.cop0[14] = faultPc;
        }
        cpu.cop0[13] = cause;
        if (badAddress)
            cpu.cop0[8] = *badAddress;
        const uint32_t status = cpu.cop0[12];
        cpu.cop0[12] = (status & ~0x3Fu) | ((status << 2u) & 0x3Fu);
        cpu.pc = (status & (1u << 22u)) ? 0xBFC00180u : 0x80000080u;
        cpu.branchPending = false;
        cpu.pendingLoad = false;
        cpu.exception = true;
    }

    bool IopCpuCore::executeInstruction(IopCpuState &cpu)
    {
        return executeFetchedInstruction(cpu, m_memory.fetch32(cpu.pc));
    }

    bool IopCpuCore::executeFetchedInstruction(IopCpuState &cpu, uint32_t instruction)
    {
        const uint32_t pc = cpu.pc;
        const bool wasDelaySlot = cpu.branchPending;
        const uint32_t priorBranchTarget = cpu.branchTarget;

        cpu.branchPending = false;
        cpu.exception = false;
        cpu.yielded = false;

        const uint32_t opcode = instruction >> 26u;
        const uint32_t rs = (instruction >> 21u) & 31u;
        const uint32_t rt = (instruction >> 16u) & 31u;
        const uint32_t rd = (instruction >> 11u) & 31u;
        const uint32_t sa = (instruction >> 6u) & 31u;
        const uint32_t funct = instruction & 63u;
        const uint32_t imm = instruction & 0xFFFFu;
        const int32_t simm = static_cast<int16_t>(imm);
        const uint32_t nextPc = pc + 4u;

        uint32_t writtenReg = 0u;
        bool scheduledLoad = false;
        uint32_t scheduledReg = 0u;
        uint32_t scheduledValue = 0u;
        bool newBranch = false;
        uint32_t newBranchTarget = 0u;

        auto branch = [&](bool condition)
        {
            if (condition)
            {
                newBranch = true;
                newBranchTarget = nextPc + (static_cast<uint32_t>(simm) << 2u);
            }
        };
        auto write = [&](uint32_t reg, uint32_t value)
        {
            writeRegister(cpu, reg, value, writtenReg);
        };
        auto load = [&](uint32_t reg, uint32_t value)
        {
            scheduleLoad(reg, value, scheduledLoad, scheduledReg, scheduledValue);
        };
        auto overflowAdd = [&](int32_t lhs, int32_t rhs, uint32_t reg)
        {
            const int64_t result = static_cast<int64_t>(lhs) + rhs;
            if (result > std::numeric_limits<int32_t>::max() || result < std::numeric_limits<int32_t>::min())
                raiseException(cpu, 12u, pc, wasDelaySlot);
            else
                write(reg, static_cast<uint32_t>(static_cast<int32_t>(result)));
        };
        auto overflowSub = [&](int32_t lhs, int32_t rhs, uint32_t reg)
        {
            const int64_t result = static_cast<int64_t>(lhs) - rhs;
            if (result > std::numeric_limits<int32_t>::max() || result < std::numeric_limits<int32_t>::min())
                raiseException(cpu, 12u, pc, wasDelaySlot);
            else
                write(reg, static_cast<uint32_t>(static_cast<int32_t>(result)));
        };

        // TODO kill this magic number and make it a constant somewhere
        switch (opcode)
        {
        case 0x00:
            switch (funct)
            {
            case 0x00:
                write(rd, cpu.gpr[rt] << sa);
                break;
            case 0x02:
                write(rd, cpu.gpr[rt] >> sa);
                break;
            case 0x03:
                write(rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[rt]) >> sa));
                break;
            case 0x04:
                write(rd, cpu.gpr[rt] << (cpu.gpr[rs] & 31u));
                break;
            case 0x06:
                write(rd, cpu.gpr[rt] >> (cpu.gpr[rs] & 31u));
                break;
            case 0x07:
                write(rd, static_cast<uint32_t>(static_cast<int32_t>(cpu.gpr[rt]) >> (cpu.gpr[rs] & 31u)));
                break;
            case 0x08:
                newBranch = true;
                newBranchTarget = cpu.gpr[rs];
                break;
            case 0x09:
                write(rd ? rd : 31u, pc + 8u);
                newBranch = true;
                newBranchTarget = cpu.gpr[rs];
                break;
            case 0x0C:
                raiseException(cpu, 8u, pc, wasDelaySlot);
                break;
            case 0x0D:
                raiseException(cpu, 9u, pc, wasDelaySlot);
                break;
            case 0x10:
                write(rd, cpu.hi);
                break;
            case 0x11:
                cpu.hi = cpu.gpr[rs];
                break;
            case 0x12:
                write(rd, cpu.lo);
                break;
            case 0x13:
                cpu.lo = cpu.gpr[rs];
                break;
            case 0x18:
            {
                const int64_t result = static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[rs])) * static_cast<int64_t>(static_cast<int32_t>(cpu.gpr[rt]));
                cpu.lo = static_cast<uint32_t>(result);
                cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(result) >> 32u);
                break;
            }
            case 0x19:
            {
                const uint64_t result = static_cast<uint64_t>(cpu.gpr[rs]) * cpu.gpr[rt];
                cpu.lo = static_cast<uint32_t>(result);
                cpu.hi = static_cast<uint32_t>(result >> 32u);
                break;
            }
            case 0x1A:
            {
                const int32_t lhs = static_cast<int32_t>(cpu.gpr[rs]);
                const int32_t rhs = static_cast<int32_t>(cpu.gpr[rt]);
                if (rhs == 0)
                {
                    cpu.lo = lhs >= 0 ? 0xFFFFFFFFu : 1u;
                    cpu.hi = static_cast<uint32_t>(lhs);
                }
                else if (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)
                {
                    cpu.lo = static_cast<uint32_t>(lhs);
                    cpu.hi = 0u;
                }
                else
                {
                    cpu.lo = static_cast<uint32_t>(lhs / rhs);
                    cpu.hi = static_cast<uint32_t>(lhs % rhs);
                }
                break;
            }
            case 0x1B:
                if (cpu.gpr[rt] == 0u)
                {
                    cpu.lo = 0xFFFFFFFFu;
                    cpu.hi = cpu.gpr[rs];
                }
                else
                {
                    cpu.lo = cpu.gpr[rs] / cpu.gpr[rt];
                    cpu.hi = cpu.gpr[rs] % cpu.gpr[rt];
                }
                break;
            case 0x20:
                overflowAdd(static_cast<int32_t>(cpu.gpr[rs]), static_cast<int32_t>(cpu.gpr[rt]), rd);
                break;
            case 0x21:
                write(rd, cpu.gpr[rs] + cpu.gpr[rt]);
                break;
            case 0x22:
                overflowSub(static_cast<int32_t>(cpu.gpr[rs]), static_cast<int32_t>(cpu.gpr[rt]), rd);
                break;
            case 0x23:
                write(rd, cpu.gpr[rs] - cpu.gpr[rt]);
                break;
            case 0x24:
                write(rd, cpu.gpr[rs] & cpu.gpr[rt]);
                break;
            case 0x25:
                write(rd, cpu.gpr[rs] | cpu.gpr[rt]);
                break;
            case 0x26:
                write(rd, cpu.gpr[rs] ^ cpu.gpr[rt]);
                break;
            case 0x27:
                write(rd, ~(cpu.gpr[rs] | cpu.gpr[rt]));
                break;
            case 0x2A:
                write(rd, static_cast<int32_t>(cpu.gpr[rs]) < static_cast<int32_t>(cpu.gpr[rt]) ? 1u : 0u);
                break;
            case 0x2B:
                write(rd, cpu.gpr[rs] < cpu.gpr[rt] ? 1u : 0u);
                break;
            default:
                raiseException(cpu, 10u, pc, wasDelaySlot);
                break;
            }
            break;
        case 0x01:
            switch (rt)
            {
            case 0x00:
                branch(static_cast<int32_t>(cpu.gpr[rs]) < 0);
                break;
            case 0x01:
                branch(static_cast<int32_t>(cpu.gpr[rs]) >= 0);
                break;
            case 0x10:
                write(31u, pc + 8u);
                branch(static_cast<int32_t>(cpu.gpr[rs]) < 0);
                break;
            case 0x11:
                write(31u, pc + 8u);
                branch(static_cast<int32_t>(cpu.gpr[rs]) >= 0);
                break;
            default:
                raiseException(cpu, 10u, pc, wasDelaySlot);
                break;
            }
            break;
        case 0x02:
            newBranch = true;
            newBranchTarget = (nextPc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2u);
            break;
        case 0x03:
            write(31u, pc + 8u);
            newBranch = true;
            newBranchTarget = (nextPc & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2u);
            break;
        case 0x04:
            branch(cpu.gpr[rs] == cpu.gpr[rt]);
            break;
        case 0x05:
            branch(cpu.gpr[rs] != cpu.gpr[rt]);
            break;
        case 0x06:
            branch(static_cast<int32_t>(cpu.gpr[rs]) <= 0);
            break;
        case 0x07:
            branch(static_cast<int32_t>(cpu.gpr[rs]) > 0);
            break;
        case 0x08:
            overflowAdd(static_cast<int32_t>(cpu.gpr[rs]), simm, rt);
            break;
        case 0x09:
            write(rt, cpu.gpr[rs] + static_cast<uint32_t>(simm));
            break;
        case 0x0A:
            write(rt, static_cast<int32_t>(cpu.gpr[rs]) < simm ? 1u : 0u);
            break;
        case 0x0B:
            write(rt, cpu.gpr[rs] < static_cast<uint32_t>(simm) ? 1u : 0u);
            break;
        case 0x0C:
            write(rt, cpu.gpr[rs] & imm);
            break;
        case 0x0D:
            write(rt, cpu.gpr[rs] | imm);
            break;
        case 0x0E:
            write(rt, cpu.gpr[rs] ^ imm);
            break;
        case 0x0F:
            write(rt, imm << 16u);
            break;
        case 0x10:
        {
            const uint32_t copRs = rs;
            if (copRs == 0x00)
                load(rt, cpu.cop0[rd]);
            else if (copRs == 0x04)
                cpu.cop0[rd] = cpu.gpr[rt];
            else if (copRs == 0x10 && funct == 0x10)
            {
                const uint32_t status = cpu.cop0[12];
                cpu.cop0[12] = (status & ~0x0Fu) | ((status >> 2u) & 0x0Fu);
            }
            else
                raiseException(cpu, 10u, pc, wasDelaySlot);
            break;
        }
        case 0x20:
        case 0x24:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint8_t value = m_memory.load8(address);
            load(rt, opcode == 0x20
                         ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value)))
                         : value);
            break;
        }
        case 0x21:
        case 0x25:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 1u)
            {
                raiseException(cpu, 4u, pc, wasDelaySlot, address);
                break;
            }
            const uint16_t value = m_memory.load16(address);
            load(rt, opcode == 0x21 ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value))) : value);
            break;
        }
        case 0x22:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t memory = m_memory.load32(address & ~3u);
            // Back-to-back LWL/LWR on the same register merge with the value still in the load delay slot
            // (as on the R3000). mc2_d copies the card specs this way.
            const uint32_t old = cpu.pendingLoad && cpu.pendingLoadReg == rt ? cpu.pendingLoadValue : cpu.gpr[rt];
            static constexpr uint32_t masks[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
            static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};
            load(rt, (old & masks[address & 3u]) | (memory << shifts[address & 3u]));
            break;
        }
        case 0x23:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 3u)
            {
                raiseException(cpu, 4u, pc, wasDelaySlot, address);
                break;
            }
            load(rt, m_memory.load32(address));
            break;
        }
        case 0x26:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t memory = m_memory.load32(address & ~3u);
            const uint32_t old = cpu.pendingLoad && cpu.pendingLoadReg == rt ? cpu.pendingLoadValue : cpu.gpr[rt];
            static constexpr uint32_t masks[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
            static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};
            load(rt, (old & masks[address & 3u]) | (memory >> shifts[address & 3u]));
            break;
        }
        case 0x28:
            m_memory.store8(cpu.gpr[rs] + static_cast<uint32_t>(simm), static_cast<uint8_t>(cpu.gpr[rt]));
            break;
        case 0x29:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 1u)
            {
                raiseException(cpu, 5u, pc, wasDelaySlot, address);
                break;
            }
            m_memory.store16(address, static_cast<uint16_t>(cpu.gpr[rt]));
            break;
        }
        case 0x2A:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t aligned = address & ~3u;
            const uint32_t old = m_memory.load32(aligned);
            const uint32_t value = cpu.gpr[rt];
            uint32_t result = old;
            switch (address & 3u)
            {
            case 0u:
                result = (old & 0xFFFFFF00u) | (value >> 24u);
                break;
            case 1u:
                result = (old & 0xFFFF0000u) | (value >> 16u);
                break;
            case 2u:
                result = (old & 0xFF000000u) | (value >> 8u);
                break;
            case 3u:
                result = value;
                break;
            }
            m_memory.store32(aligned, result);
            break;
        }
        case 0x2B:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            if (address & 3u)
            {
                raiseException(cpu, 5u, pc, wasDelaySlot, address);
                break;
            }
            m_memory.store32(address, cpu.gpr[rt]);
            break;
        }
        case 0x2E:
        {
            const uint32_t address = cpu.gpr[rs] + static_cast<uint32_t>(simm);
            const uint32_t aligned = address & ~3u;
            const uint32_t old = m_memory.load32(aligned);
            const uint32_t value = cpu.gpr[rt];
            uint32_t result = old;
            switch (address & 3u)
            {
            case 0u:
                result = value;
                break;
            case 1u:
                result = (old & 0x000000FFu) | (value << 8u);
                break;
            case 2u:
                result = (old & 0x0000FFFFu) | (value << 16u);
                break;
            case 3u:
                result = (old & 0x00FFFFFFu) | (value << 24u);
                break;
            }
            m_memory.store32(aligned, result);
            break;
        }
        default:
            raiseException(cpu, 10u, pc, wasDelaySlot);
            break;
        }

        cpu.gpr[0] = 0u;
        if (cpu.exception)
            return !cpu.stopped;

        if (cpu.pendingLoad)
        {
            if (cpu.pendingLoadReg != 0u && cpu.pendingLoadReg != writtenReg)
                cpu.gpr[cpu.pendingLoadReg] = cpu.pendingLoadValue;
            cpu.pendingLoad = false;
        }
        if (scheduledLoad)
        {
            cpu.pendingLoad = true;
            cpu.pendingLoadReg = scheduledReg;
            cpu.pendingLoadValue = scheduledValue;
        }
        cpu.gpr[0] = 0u;

        if (wasDelaySlot)
        {
            cpu.pc = priorBranchTarget;
            cpu.branchPending = false;
        }
        else
        {
            cpu.pc = nextPc;
            cpu.branchPending = newBranch;
            cpu.branchTarget = newBranchTarget;
        }
        return !cpu.stopped;
    }
    uint32_t IopCpuCore::runBurst(IopCpuState &cpu, uint32_t limit, bool &bailed)
    {
        // Mirrors executeFetchedInstruction() for the instructions handled here, quirks included (a branch in a delay
        // slot is dropped, jalr/bltzal write the link register before reading rs, a load whose register is written by
        // the next instruction is lost, ...). Anything else is left, untouched, to the ordinary path.
        uint8_t *const ram = m_memory.ramData();
        uint8_t *const owned = m_memory.ownedData();
        uint32_t *const r = cpu.gpr.data();
        uint32_t pc = cpu.pc;
        uint32_t n = 0u;
        bool bail = false;
        uint32_t busy = (cpu.pendingLoad ? 1u : 0u) | (cpu.branchPending ? 1u : 0u); // a load or a branch is in flight

        constexpr uint32_t kBad = 0xFFE00003u; // outside RAM (>= 2 MB) or unaligned

        // Executes `dw` if it is a simple register/store instruction that cannot trap, branch, load or leave RAM:
        // returns false (nothing changed) otherwise. `written` receives the register it wrote (0 if none).
        // Used to run a branch's delay slot, or the instruction after a load, together with the instruction itself.
        const auto simple = [r, ram, owned](uint32_t dw, uint32_t &written) -> bool
        {
            const uint32_t drs = (dw >> 21u) & 31u;
            const uint32_t drt = (dw >> 16u) & 31u;
            const uint32_t drd = (dw >> 11u) & 31u;
            const uint32_t dsa = (dw >> 6u) & 31u;
            const uint32_t dimm = dw & 0xFFFFu;
            const uint32_t dsimm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(dimm)));
            switch (dw >> 26u)
            {
            case 0x00:
                switch (dw & 63u)
                {
                case 0x00: r[drd] = r[drt] << dsa; written = drd; return true;
                case 0x02: r[drd] = r[drt] >> dsa; written = drd; return true;
                case 0x03: r[drd] = static_cast<uint32_t>(static_cast<int32_t>(r[drt]) >> dsa); written = drd; return true;
                case 0x21: r[drd] = r[drs] + r[drt]; written = drd; return true;
                case 0x23: r[drd] = r[drs] - r[drt]; written = drd; return true;
                case 0x24: r[drd] = r[drs] & r[drt]; written = drd; return true;
                case 0x25: r[drd] = r[drs] | r[drt]; written = drd; return true;
                case 0x26: r[drd] = r[drs] ^ r[drt]; written = drd; return true;
                case 0x2A: r[drd] = static_cast<int32_t>(r[drs]) < static_cast<int32_t>(r[drt]) ? 1u : 0u; written = drd; return true;
                case 0x2B: r[drd] = r[drs] < r[drt] ? 1u : 0u; written = drd; return true;
                default: return false;
                }
            case 0x09: r[drt] = r[drs] + dsimm; written = drt; return true;
            case 0x0A: r[drt] = static_cast<int32_t>(r[drs]) < static_cast<int32_t>(dsimm) ? 1u : 0u; written = drt; return true;
            case 0x0B: r[drt] = r[drs] < dsimm ? 1u : 0u; written = drt; return true;
            case 0x0C: r[drt] = r[drs] & dimm; written = drt; return true;
            case 0x0D: r[drt] = r[drs] | dimm; written = drt; return true;
            case 0x0E: r[drt] = r[drs] ^ dimm; written = drt; return true;
            case 0x0F: r[drt] = dimm << 16u; written = drt; return true;
            case 0x2B:
            {
                const uint32_t phys = (r[drs] + dsimm) & 0x1FFFFFFFu;
                if (phys & kBad)
                    return false;
                const uint32_t value = r[drt];
                std::memcpy(ram + phys, &value, sizeof(value));
                std::memset(owned + phys, 1, sizeof(value));
                written = 0u;
                return true;
            }
            default:
                return false;
            }
        };

        while (n < limit)
        {
            const uint32_t pcPhys = pc & 0x1FFFFFFFu;
            if (pcPhys & kBad)
            {
                bail = true;
                break;
            }
            uint32_t ins;
            std::memcpy(&ins, ram + pcPhys, sizeof(ins));
            const uint32_t rs = (ins >> 21u) & 31u;
            const uint32_t rt = (ins >> 16u) & 31u;
            const uint32_t rd = (ins >> 11u) & 31u;
            const uint32_t sa = (ins >> 6u) & 31u;
            const uint32_t imm = ins & 0xFFFFu;
            const uint32_t simm = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm)));
            const uint32_t nextPc = pc + 4u;

            uint32_t wreg = 0u;
            uint32_t sReg = 0u; // scheduled load target (0 = none)
            uint32_t sVal = 0u;
            bool nb = false;
            uint32_t nbt = 0u;

// instruction that neither schedules a load nor branches: with no load/branch in flight it finishes right here
#define IOPF_PLAIN() if (!busy) { r[0] = 0u; pc = nextPc; cpu.branchTarget = 0u; ++n; continue; } break
#define IOPF_W(reg, value) do { const uint32_t w_ = (reg); r[w_] = (value); wreg = w_; } while (0)
#define IOPF_BRANCH(cond) do { if (cond) { nb = true; nbt = nextPc + (simm << 2u); } } while (0)
#define IOPF_LOAD(reg, value) do { sReg = (reg); sVal = (value); } while (0)

            switch (ins >> 26u)
            {
            case 0x00:
                switch (ins & 63u)
                {
                case 0x00: IOPF_W(rd, r[rt] << sa); IOPF_PLAIN();
                case 0x02: IOPF_W(rd, r[rt] >> sa); IOPF_PLAIN();
                case 0x03: IOPF_W(rd, static_cast<uint32_t>(static_cast<int32_t>(r[rt]) >> sa)); IOPF_PLAIN();
                case 0x04: IOPF_W(rd, r[rt] << (r[rs] & 31u)); IOPF_PLAIN();
                case 0x06: IOPF_W(rd, r[rt] >> (r[rs] & 31u)); IOPF_PLAIN();
                case 0x07: IOPF_W(rd, static_cast<uint32_t>(static_cast<int32_t>(r[rt]) >> (r[rs] & 31u))); IOPF_PLAIN();
                case 0x08:
                    if (ins == 0x03E00008u)
                    {
                        // `jr ra` followed by `addiu zero, zero, ordinal` may be an import stub: the caller decodes those.
                        if (pcPhys + 4u >= IopMemory::RamSize)
                            goto bail_out;
                        uint32_t delay;
                        std::memcpy(&delay, ram + pcPhys + 4u, sizeof(delay));
                        if ((delay & 0xFFFF0000u) == 0x24000000u)
                            goto bail_out;
                    }
                    nb = true;
                    nbt = r[rs];
                    break;
                case 0x09:
                    IOPF_W(rd ? rd : 31u, pc + 8u);
                    nb = true;
                    nbt = r[rs];
                    break;
                case 0x10: IOPF_W(rd, cpu.hi); IOPF_PLAIN();
                case 0x11: cpu.hi = r[rs]; IOPF_PLAIN();
                case 0x12: IOPF_W(rd, cpu.lo); IOPF_PLAIN();
                case 0x13: cpu.lo = r[rs]; IOPF_PLAIN();
                case 0x18:
                {
                    const int64_t result = static_cast<int64_t>(static_cast<int32_t>(r[rs])) * static_cast<int64_t>(static_cast<int32_t>(r[rt]));
                    cpu.lo = static_cast<uint32_t>(result);
                    cpu.hi = static_cast<uint32_t>(static_cast<uint64_t>(result) >> 32u);
                    IOPF_PLAIN();
                }
                case 0x19:
                {
                    const uint64_t result = static_cast<uint64_t>(r[rs]) * r[rt];
                    cpu.lo = static_cast<uint32_t>(result);
                    cpu.hi = static_cast<uint32_t>(result >> 32u);
                    IOPF_PLAIN();
                }
                case 0x1A:
                {
                    const int32_t lhs = static_cast<int32_t>(r[rs]);
                    const int32_t rhs = static_cast<int32_t>(r[rt]);
                    if (rhs == 0)
                    {
                        cpu.lo = lhs >= 0 ? 0xFFFFFFFFu : 1u;
                        cpu.hi = static_cast<uint32_t>(lhs);
                    }
                    else if (lhs == std::numeric_limits<int32_t>::min() && rhs == -1)
                    {
                        cpu.lo = static_cast<uint32_t>(lhs);
                        cpu.hi = 0u;
                    }
                    else
                    {
                        cpu.lo = static_cast<uint32_t>(lhs / rhs);
                        cpu.hi = static_cast<uint32_t>(lhs % rhs);
                    }
                    IOPF_PLAIN();
                }
                case 0x1B:
                    if (r[rt] == 0u)
                    {
                        cpu.lo = 0xFFFFFFFFu;
                        cpu.hi = r[rs];
                    }
                    else
                    {
                        cpu.lo = r[rs] / r[rt];
                        cpu.hi = r[rs] % r[rt];
                    }
                    IOPF_PLAIN();
                case 0x20:
                {
                    const int64_t result = static_cast<int64_t>(static_cast<int32_t>(r[rs])) + static_cast<int32_t>(r[rt]);
                    if (result != static_cast<int32_t>(result))
                        goto bail_out;
                    IOPF_W(rd, static_cast<uint32_t>(result));
                    IOPF_PLAIN();
                }
                case 0x21: IOPF_W(rd, r[rs] + r[rt]); IOPF_PLAIN();
                case 0x22:
                {
                    const int64_t result = static_cast<int64_t>(static_cast<int32_t>(r[rs])) - static_cast<int32_t>(r[rt]);
                    if (result != static_cast<int32_t>(result))
                        goto bail_out;
                    IOPF_W(rd, static_cast<uint32_t>(result));
                    IOPF_PLAIN();
                }
                case 0x23: IOPF_W(rd, r[rs] - r[rt]); IOPF_PLAIN();
                case 0x24: IOPF_W(rd, r[rs] & r[rt]); IOPF_PLAIN();
                case 0x25: IOPF_W(rd, r[rs] | r[rt]); IOPF_PLAIN();
                case 0x26: IOPF_W(rd, r[rs] ^ r[rt]); IOPF_PLAIN();
                case 0x27: IOPF_W(rd, ~(r[rs] | r[rt])); IOPF_PLAIN();
                case 0x2A: IOPF_W(rd, static_cast<int32_t>(r[rs]) < static_cast<int32_t>(r[rt]) ? 1u : 0u); IOPF_PLAIN();
                case 0x2B: IOPF_W(rd, r[rs] < r[rt] ? 1u : 0u); IOPF_PLAIN();
                default:
                    goto bail_out; // syscall, break, reserved
                }
                break;
            case 0x01:
                switch (rt)
                {
                case 0x00: IOPF_BRANCH(static_cast<int32_t>(r[rs]) < 0); break;
                case 0x01: IOPF_BRANCH(static_cast<int32_t>(r[rs]) >= 0); break;
                case 0x10:
                    IOPF_W(31u, pc + 8u);
                    IOPF_BRANCH(static_cast<int32_t>(r[rs]) < 0);
                    break;
                case 0x11:
                    IOPF_W(31u, pc + 8u);
                    IOPF_BRANCH(static_cast<int32_t>(r[rs]) >= 0);
                    break;
                default:
                    goto bail_out;
                }
                break;
            case 0x02:
                nb = true;
                nbt = (nextPc & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2u);
                break;
            case 0x03:
                IOPF_W(31u, pc + 8u);
                nb = true;
                nbt = (nextPc & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2u);
                break;
            case 0x04: IOPF_BRANCH(r[rs] == r[rt]); break;
            case 0x05: IOPF_BRANCH(r[rs] != r[rt]); break;
            case 0x06: IOPF_BRANCH(static_cast<int32_t>(r[rs]) <= 0); break;
            case 0x07: IOPF_BRANCH(static_cast<int32_t>(r[rs]) > 0); break;
            case 0x08:
            {
                const int64_t result = static_cast<int64_t>(static_cast<int32_t>(r[rs])) + static_cast<int32_t>(simm);
                if (result != static_cast<int32_t>(result))
                    goto bail_out;
                IOPF_W(rt, static_cast<uint32_t>(result));
                IOPF_PLAIN();
            }
            case 0x09: IOPF_W(rt, r[rs] + simm); IOPF_PLAIN();
            case 0x0A: IOPF_W(rt, static_cast<int32_t>(r[rs]) < static_cast<int32_t>(simm) ? 1u : 0u); IOPF_PLAIN();
            case 0x0B: IOPF_W(rt, r[rs] < simm ? 1u : 0u); IOPF_PLAIN();
            case 0x0C: IOPF_W(rt, r[rs] & imm); IOPF_PLAIN();
            case 0x0D: IOPF_W(rt, r[rs] | imm); IOPF_PLAIN();
            case 0x0E: IOPF_W(rt, r[rs] ^ imm); IOPF_PLAIN();
            case 0x0F: IOPF_W(rt, imm << 16u); IOPF_PLAIN();
            case 0x20:
            case 0x24:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                const uint8_t value = ram[phys];
                IOPF_LOAD(rt, (ins >> 26u) == 0x20u ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(value))) : value);
                break;
            }
            case 0x21:
            case 0x25:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys & (kBad & ~2u)) // outside RAM or odd (misaligned halfword raises an exception)
                    goto bail_out;
                uint16_t value;
                std::memcpy(&value, ram + phys, sizeof(value));
                IOPF_LOAD(rt, (ins >> 26u) == 0x21u ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value))) : value);
                break;
            }
            case 0x22:
            {
                const uint32_t address = r[rs] + simm;
                const uint32_t phys = address & 0x1FFFFFFCu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                uint32_t memory;
                std::memcpy(&memory, ram + phys, sizeof(memory));
                const uint32_t old = cpu.pendingLoad && cpu.pendingLoadReg == rt ? cpu.pendingLoadValue : r[rt];
                static constexpr uint32_t masks[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
                static constexpr uint32_t shifts[4] = {24u, 16u, 8u, 0u};
                IOPF_LOAD(rt, (old & masks[address & 3u]) | (memory << shifts[address & 3u]));
                break;
            }
            case 0x23:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys & kBad)
                    goto bail_out;
                uint32_t value;
                std::memcpy(&value, ram + phys, sizeof(value));
                IOPF_LOAD(rt, value);
                break;
            }
            case 0x26:
            {
                const uint32_t address = r[rs] + simm;
                const uint32_t phys = address & 0x1FFFFFFCu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                uint32_t memory;
                std::memcpy(&memory, ram + phys, sizeof(memory));
                const uint32_t old = cpu.pendingLoad && cpu.pendingLoadReg == rt ? cpu.pendingLoadValue : r[rt];
                static constexpr uint32_t masks[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
                static constexpr uint32_t shifts[4] = {0u, 8u, 16u, 24u};
                IOPF_LOAD(rt, (old & masks[address & 3u]) | (memory >> shifts[address & 3u]));
                break;
            }
            case 0x28:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                ram[phys] = static_cast<uint8_t>(r[rt]);
                owned[phys] = 1u;
                IOPF_PLAIN();
            }
            case 0x29:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys & (kBad & ~2u))
                    goto bail_out;
                const uint16_t value = static_cast<uint16_t>(r[rt]);
                std::memcpy(ram + phys, &value, sizeof(value));
                std::memset(owned + phys, 1, sizeof(value));
                IOPF_PLAIN();
            }
            case 0x2A:
            {
                const uint32_t address = r[rs] + simm;
                const uint32_t phys = address & 0x1FFFFFFCu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                uint32_t old;
                std::memcpy(&old, ram + phys, sizeof(old));
                const uint32_t value = r[rt];
                uint32_t result = old;
                switch (address & 3u)
                {
                case 0u: result = (old & 0xFFFFFF00u) | (value >> 24u); break;
                case 1u: result = (old & 0xFFFF0000u) | (value >> 16u); break;
                case 2u: result = (old & 0xFF000000u) | (value >> 8u); break;
                case 3u: result = value; break;
                }
                std::memcpy(ram + phys, &result, sizeof(result));
                std::memset(owned + phys, 1, sizeof(result));
                IOPF_PLAIN();
            }
            case 0x2B:
            {
                const uint32_t phys = (r[rs] + simm) & 0x1FFFFFFFu;
                if (phys & kBad)
                    goto bail_out;
                const uint32_t value = r[rt];
                std::memcpy(ram + phys, &value, sizeof(value));
                std::memset(owned + phys, 1, sizeof(value));
                IOPF_PLAIN();
            }
            case 0x2E:
            {
                const uint32_t address = r[rs] + simm;
                const uint32_t phys = address & 0x1FFFFFFCu;
                if (phys >= IopMemory::RamSize)
                    goto bail_out;
                uint32_t old;
                std::memcpy(&old, ram + phys, sizeof(old));
                const uint32_t value = r[rt];
                uint32_t result = old;
                switch (address & 3u)
                {
                case 0u: result = value; break;
                case 1u: result = (old & 0x000000FFu) | (value << 8u); break;
                case 2u: result = (old & 0x0000FFFFu) | (value << 16u); break;
                case 3u: result = (old & 0x00FFFFFFu) | (value << 24u); break;
                }
                std::memcpy(ram + phys, &result, sizeof(result));
                std::memset(owned + phys, 1, sizeof(result));
                IOPF_PLAIN();
            }
            default:
                goto bail_out; // COP0 and everything rare: ordinary path
            }

            // A taken branch or a load with nothing in flight: run the delay slot / next instruction right here when it
            // is a simple one, so no branch or load-delay state has to be carried to the next iteration.
            if (!busy && n + 2u <= limit && (nb || sReg != 0u))
            {
                const uint32_t dphys = nextPc & 0x1FFFFFFFu;
                uint32_t dw;
                uint32_t dwritten = 0u;
                if (!(dphys & kBad) && (std::memcpy(&dw, ram + dphys, sizeof(dw)), simple(dw, dwritten)))
                {
                    r[0] = 0u;
                    if (nb)
                    {
                        pc = nbt;     // after the delay slot: branch target
                        cpu.branchTarget = nbt;
                    }
                    else
                    {
                        if (sReg != dwritten)
                            r[sReg] = sVal; // the load lands after the next instruction, unless that one wrote the same register
                        cpu.pendingLoadReg = sReg;
                        cpu.pendingLoadValue = sVal;
                        pc = nextPc + 4u;
                        cpu.branchTarget = 0u;
                    }
                    n += 2u;
                    continue;
                }
            }
            r[0] = 0u;
            if (cpu.pendingLoad)
            {
                if (cpu.pendingLoadReg != wreg)
                    r[cpu.pendingLoadReg] = cpu.pendingLoadValue;
                cpu.pendingLoad = false;
            }
            if (sReg)
            {
                cpu.pendingLoad = true;
                cpu.pendingLoadReg = sReg;
                cpu.pendingLoadValue = sVal;
            }
            if (cpu.branchPending)
            {
                pc = cpu.branchTarget;
                cpu.branchPending = false;
            }
            else
            {
                pc = nextPc;
                cpu.branchPending = nb;
                cpu.branchTarget = nbt;
            }
            busy = (cpu.pendingLoad ? 1u : 0u) | (cpu.branchPending ? 1u : 0u);
            ++n;
            continue;

        bail_out:
            r[0] = 0u; // a bailed instruction may not have changed anything, but keep the invariant explicit
            bail = true;
            break;

#undef IOPF_PLAIN
#undef IOPF_W
#undef IOPF_BRANCH
#undef IOPF_LOAD
        }

        cpu.pc = pc;
        if (n != 0u)
        {
            cpu.exception = false;
            cpu.yielded = false;
        }
        bailed = bail;
        return n;
    }
}
