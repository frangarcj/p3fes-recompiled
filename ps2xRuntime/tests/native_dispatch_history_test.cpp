#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
    constexpr uint32_t kTableBase = 0x00100000u;
    constexpr uint32_t kSlotCount = 96u;
    constexpr uint32_t kFallthrough = 0x00110000u;
    constexpr uint32_t kTailFallthrough = 0x00110010u;
    constexpr uint32_t kJumpTarget = kTableBase + 12u;

    [[noreturn]] void fail(const char *message)
    {
        std::fprintf(stderr, "native dispatch history test failed: %s\n", message);
        std::exit(EXIT_FAILURE);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
            fail(message);
    }

    void setBranchRingEnvironment(const char *value)
    {
#if defined(_WIN32)
        _putenv_s("PS2X_BRANCH_RING", value);
        _putenv_s("PS2X_EE_TRACE_CALLS", "");
#else
        setenv("PS2X_BRANCH_RING", value, 1);
        unsetenv("PS2X_EE_TRACE_CALLS");
#endif
    }

    void returnWithoutJump(uint8_t *, R5900Context *, PS2Runtime *)
    {
    }

    void tailJump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime->dispatchGuestBranch(rdram,
                                           ctx,
                                           kJumpTarget,
                                           kTableBase + 4u,
                                           kTailFallthrough,
                                           PS2Runtime::GuestBranchKind::DirectJump,
                                           "dispatch history tail-jump probe");
    }

    std::string expectedWrappedHistory()
    {
        std::ostringstream expected;
        for (uint32_t index = 16u; index < 80u; ++index)
        {
            if (index != 16u)
                expected << " -> ";
            expected << "0x" << std::hex << (kTableBase + index * 4u);
        }
        return expected.str();
    }

    void setReg(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void runChild(const char *mode)
    {
        const bool enabled = std::string(mode) == "child-on";
        require(enabled || std::string(mode) == "child-off", "unknown child mode");
        setBranchRingEnvironment(enabled ? "1" : "0");

        std::vector<PS2Runtime::RecompiledFunction> table(kSlotCount, &returnWithoutJump);
        table[1] = &tailJump;
        ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableBase + kSlotCount * 4u, kSlotCount});

        PS2Runtime runtime;
        std::vector<uint8_t> rdram(1024u, 0u);
        for (uint32_t index = 0u; index < 80u; ++index)
        {
            const auto expected = index == 1u ? &tailJump : &returnWithoutJump;
            require(runtime.lookupFunction(kTableBase + index * 4u) == expected,
                    "dense fake function lookup failed");
        }

        const std::string history = runtime.dispatchHistoryString();
        if (enabled)
            require(history == expectedWrappedHistory(), "enabled history lost or reordered wrapped entries");
        else
            require(history == "(empty)", "disabled history retained dispatch entries");

        R5900Context returning{};
        setReg(returning, 31u, kFallthrough);
        const bool returned = runtime.dispatchGuestBranch(rdram.data(),
                                                          &returning,
                                                          kTableBase,
                                                          kTableBase + 0x100u,
                                                          kFallthrough,
                                                          PS2Runtime::GuestBranchKind::DirectCall,
                                                          "native dispatch history return probe");
        require(returned && returning.pc == kFallthrough,
                "ordinary guest return no longer selected the call fallthrough");

        R5900Context tail{};
        setReg(tail, 31u, kTailFallthrough);
        const bool tailReturned = runtime.dispatchGuestBranch(rdram.data(),
                                                              &tail,
                                                              kTableBase + 4u,
                                                              kTableBase + 0x104u,
                                                              kTailFallthrough,
                                                              PS2Runtime::GuestBranchKind::DirectCall,
                                                              "native dispatch history tail-jump probe");
        require(!tailReturned && tail.pc == kJumpTarget,
                "tail-jump target was rewritten to the call fallthrough");
    }

    int runSeparateProcess(const char *program, const char *mode)
    {
#if defined(_WIN32)
        return static_cast<int>(_spawnl(_P_WAIT, program, program, mode, static_cast<char *>(nullptr)));
#else
        const pid_t child = fork();
        if (child < 0)
            return -1;
        if (child == 0)
        {
            execlp(program, program, mode, static_cast<char *>(nullptr));
            _exit(127);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child)
            return -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
#endif
    }
}

int main(int argc, char **argv)
{
    if (argc == 2)
    {
        runChild(argv[1]);
        return EXIT_SUCCESS;
    }

    require(runSeparateProcess(argv[0], "child-off") == 0, "branch-ring-off child failed");
    require(runSeparateProcess(argv[0], "child-on") == 0, "branch-ring-on child failed");
    std::puts("native dispatch history tests passed");
    return EXIT_SUCCESS;
}
