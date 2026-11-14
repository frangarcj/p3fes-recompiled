#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace
{
    constexpr uint32_t kTableBase = 0x00100000u;
    constexpr uint32_t kTableEnd = 0x00110000u;
    constexpr uint32_t kTarget = 0x00100008u;
    constexpr uint32_t kSource = 0x00100004u;
    constexpr uint32_t kReturnPc = 0x00101000u;
    constexpr uint32_t kStringA0 = 0x00102000u;
    constexpr uint32_t kStringA1 = 0x00102040u;

    uint32_t getReg(const R5900Context &ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[index]));
    }

    void setReg(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void fakeFunction(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ctx->pc = getReg(*ctx, 31u);
    }

    [[noreturn]] void fail(const char *message)
    {
        std::fprintf(stderr, "guest branch trace test failed: %s\n", message);
        std::exit(1);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
            fail(message);
    }

    std::string readFile(FILE *file)
    {
        std::fflush(file);
        require(std::fseek(file, 0, SEEK_SET) == 0, "could not rewind stderr capture");
        std::string contents;
        char buffer[1024];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
            contents.append(buffer, count);
        return contents;
    }

    std::string captureDispatches(const char *mode, const char *traceSetting,
                                  const char *changedSetting)
    {
        char traceFile[] = "/tmp/p3fes-ee-trace-XXXXXX";
        int traceFd = mkstemp(traceFile);
        require(traceFd >= 0, "mkstemp for trace config failed");
        const std::string tracePath(traceFile);
        std::string configLine;
        if (std::strcmp(mode, "file") == 0)
            configLine = "00100008 menu-target\n";
        else if (std::strcmp(mode, "file-empty") == 0)
            configLine.clear();
        else
            configLine = "00100008\n";
        require(write(traceFd, configLine.data(), configLine.size()) == static_cast<ssize_t>(configLine.size()),
                "could not write trace config");
        close(traceFd);

        if (std::strcmp(mode, "unset") == 0)
            unsetenv("PS2X_EE_TRACE_CALLS");
        else if (std::strcmp(mode, "empty") == 0)
            setenv("PS2X_EE_TRACE_CALLS", "", 1);
        else if (std::strcmp(mode, "list") == 0)
            setenv("PS2X_EE_TRACE_CALLS", traceSetting, 1);
        else if (std::strcmp(mode, "list-miss") == 0)
            setenv("PS2X_EE_TRACE_CALLS", "0010000C", 1);
        else if (std::strcmp(mode, "file") == 0)
        {
            const std::string setting = "@" + tracePath;
            setenv("PS2X_EE_TRACE_CALLS", setting.c_str(), 1);
        }
        else if (std::strcmp(mode, "file-empty") == 0 || std::strcmp(mode, "file-missing") == 0)
        {
            if (std::strcmp(mode, "file-missing") == 0)
                std::remove(tracePath.c_str());
            const std::string setting = "@" + tracePath;
            setenv("PS2X_EE_TRACE_CALLS", setting.c_str(), 1);
        }
        else
            fail("mode must be unset, empty, list, or file");

        // Address-register diagnostics make the saved guest RA observable.
        setenv("PS2X_EE_TRACE_64", "1", 1);

        FILE *capture = std::tmpfile();
        require(capture != nullptr, "tmpfile for stderr capture failed");
        std::fflush(stderr);
        const int savedStderr = dup(STDERR_FILENO);
        require(savedStderr >= 0, "dup(stderr) failed");
        require(dup2(fileno(capture), STDERR_FILENO) >= 0, "redirecting stderr failed");

        const uint32_t slotCount = (kTableEnd - kTableBase) / 4u;
        std::vector<PS2Runtime::RecompiledFunction> table(slotCount, &fakeFunction);
        ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableEnd, slotCount});
        PS2Runtime runtime;
        std::vector<uint8_t> rdram(2u * 1024u * 1024u, 0);
        std::memcpy(rdram.data() + (kStringA0 & 0x1FFFFFFFu), "texture-name", 13);
        std::memcpy(rdram.data() + (kStringA1 & 0x1FFFFFFFu), "asset-name", 11);

        for (uint32_t i = 0; i < 16u; ++i)
        {
            R5900Context ctx;
            ctx.pc = kTarget;
            setReg(ctx, 31u, kReturnPc);
            setReg(ctx, 4u, kStringA0);
            setReg(ctx, 5u, kStringA1);
            setReg(ctx, 6u, 0x12345678u);
            setReg(ctx, 7u, 0x87654321u);
            const bool returned = runtime.dispatchGuestBranch(
                rdram.data(), &ctx, kTarget, kSource, kReturnPc,
                PS2Runtime::GuestBranchKind::DirectCall, "guest branch trace test");
            require(returned && ctx.pc == kReturnPc, "fake guest call did not return to fallthrough");

            // The trace selection is a first-use snapshot. Later environment changes
            // must not enable an initially disabled trace or change an enabled one.
            if (i == 0u)
            {
                if (changedSetting == nullptr)
                    unsetenv("PS2X_EE_TRACE_CALLS");
                else
                    setenv("PS2X_EE_TRACE_CALLS", changedSetting, 1);
            }
        }

        std::fflush(stderr);
        require(dup2(savedStderr, STDERR_FILENO) >= 0, "restoring stderr failed");
        close(savedStderr);
        std::string output = readFile(capture);
        std::fclose(capture);
        std::remove(tracePath.c_str());
        return output;
    }

    size_t countOccurrences(const std::string &text, const std::string &needle)
    {
        size_t count = 0;
        size_t position = 0;
        while ((position = text.find(needle, position)) != std::string::npos)
        {
            ++count;
            position += needle.size();
        }
        return count;
    }

    void runMode(const char *mode)
    {
        const bool enabled = std::strcmp(mode, "list") == 0 || std::strcmp(mode, "file") == 0;
        const std::string output = captureDispatches(mode, "00100008", enabled ? "" : "00100008");

        if (!enabled)
        {
            require(output.find("[ee-call]") == std::string::npos, "unset/empty setting was enabled after first use");
            require(output.find("[ee-count]") == std::string::npos, "unset/empty setting emitted count logs");
            return;
        }

        require(countOccurrences(output, "[ee-call] ") == 4u, "expected exactly four first-call records");
        require(countOccurrences(output, "[ee-call64] ") == 4u, "expected four 64-bit register records");
        require(countOccurrences(output, "[ee-count] ") == 2u, "expected count records at 8 and 16 calls");
        require(output.find("[ee-count] 00100008 ") != std::string::npos, "count record target missing");
        require(output.find(" n=8\n") != std::string::npos, "count record for 8 calls missing");
        require(output.find(" n=16\n") != std::string::npos, "count record for 16 calls missing");
        require(output.find("from 00100004 a0=00102000 a1=00102040 a2=12345678 a3=87654321") != std::string::npos,
                "source PC or argument registers missing from call record");
        require(output.find("ra=00101000") != std::string::npos, "return address missing from register record");
        require(output.find("\"texture-name\" \"asset-name\"") != std::string::npos,
                "guest string arguments missing from call record");
        if (std::strcmp(mode, "file") == 0)
            require(output.find("[ee-call] 00100008 menu-target from 00100004") != std::string::npos,
                    "@file label missing from call record");
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        fail("invoke in a fresh process with one of: unset, empty, list, list-miss, file, file-empty, file-missing");
    runMode(argv[1]);
    std::printf("guest branch trace mode %s passed\n", argv[1]);
    return 0;
}
