#include "game_overrides/p3fes_guest_math_helper.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x00100000u;
    constexpr uint32_t kEntry = kTableBase;
    constexpr uint32_t kInterior = kTableBase + 4u;
    constexpr uint32_t kBadSp = kTableBase + 8u;
    constexpr uint32_t kBadPc = kTableBase + 12u;
    constexpr uint32_t kTableEnd = kTableBase + 16u;
    constexpr uint32_t kSp = 0x001FF000u;
    constexpr uint32_t kResult = 0x13579BDFu;
    constexpr uint32_t kBadTarget = 0x00400000u;

    uint32_t getReg(const R5900Context &ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[index]));
    }

    void setReg(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void entryCheckpoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = getReg(*ctx, 29u);
        const uint32_t savedReturn = getReg(*ctx, 31u);
        std::memcpy(rdram + sp - 4u, &savedReturn, sizeof(savedReturn));
        setReg(*ctx, 29u, sp - 16u);
        setReg(*ctx, 31u, kInterior);
        setReg(*ctx, 8u, 0xDEADBEEFu); // caller-saved scratch must stay isolated.
        ctx->pc = kInterior;           // Simulate a checkpoint return mid-helper.
    }

    void resumeCheckpoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = getReg(*ctx, 29u);
        const uint32_t iteration = getReg(*ctx, 10u) + 1u;
        setReg(*ctx, 10u, iteration);
        if (iteration < 3u)
        {
            ctx->pc = kInterior; // A backward-edge checkpoint can repeat this PC.
            return;
        }
        uint32_t savedReturn = 0u;
        std::memcpy(&savedReturn, rdram + sp + 12u, sizeof(savedReturn));
        setReg(*ctx, 29u, sp + 16u);
        setReg(*ctx, 31u, savedReturn);
        setReg(*ctx, 2u, kResult);
        ctx->pc = savedReturn;
    }

    void badStackReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReg(*ctx, 29u, getReg(*ctx, 29u) - 16u);
        ctx->pc = getReg(*ctx, 31u);
    }

    void badPcReturn(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = kBadTarget;
    }

    void require(bool condition, const char *message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "native guest helper test failed: %s\n", message);
        std::exit(1);
    }
}

int main()
{
    std::vector<PS2Runtime::RecompiledFunction> table((kTableEnd - kTableBase) / 4u, nullptr);
    table[0] = &entryCheckpoint;
    table[1] = &resumeCheckpoint;
    table[2] = &badStackReturn;
    table[3] = &badPcReturn;
    ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableEnd,
                                   static_cast<uint32_t>(table.size())});

    PS2Runtime runtime;
    std::vector<uint8_t> rdram(PS2_RAM_SIZE);
    R5900Context caller;
    caller.pc = 0x00401000u;
    setReg(caller, 4u, 0xAAAAu);
    setReg(caller, 5u, 0xBBBBu);
    setReg(caller, 8u, 0x11111111u);
    setReg(caller, 29u, kSp);
    setReg(caller, 31u, 0x00402000u);

    const auto resumed = p3fes_guest_math_helper::run(
        rdram.data(), caller, runtime, kEntry, 0x1234u, 0x5678u);
    require(resumed.completed, "checkpointed helper did not resume to its RA sentinel");
    require(resumed.resumes == 4u, "helper did not resume repeated interior checkpoints");
    require(resumed.value == kResult, "helper return value was not preserved");
    require(caller.pc == 0x00401000u && getReg(caller, 31u) == 0x00402000u &&
                getReg(caller, 29u) == kSp && getReg(caller, 8u) == 0x11111111u,
            "isolated helper changed caller PC/RA/SP/scratch registers");

    const auto badSp = p3fes_guest_math_helper::run(rdram.data(), caller, runtime, kBadSp, 0u);
    require(!badSp.completed, "helper return with unbalanced SP was accepted");
    const auto badPc = p3fes_guest_math_helper::run(rdram.data(), caller, runtime, kBadPc, 0u);
    require(!badPc.completed, "helper return to an unmapped PC was accepted");
    const auto bounded = p3fes_guest_math_helper::run(
        rdram.data(), caller, runtime, kEntry, 0x1234u, 0x5678u, 2u);
    require(!bounded.completed && bounded.resumes == 2u,
            "helper exceeded its configured checkpoint-resume bound");
    std::puts("native guest math helper test passed");
}
