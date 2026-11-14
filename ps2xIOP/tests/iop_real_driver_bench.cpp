// Offline benchmark of the IOP with the game's real sound drivers (no EE, no game): loads the IRX files of a
// LIB31 directory in the order the game does, then feeds the IOP like the EE does (small runEeCycles chunks) and
// reports guest instructions per guest second and HOST instructions per guest instruction (Apple Silicon
// ri_instructions from proc_pid_rusage; other platforms print only timings).
// usage: ps2_iop_real_driver_bench <LIB31 dir> [guest seconds=20] [warm-up seconds=5] [chunk IOP cycles=2047]
// env PS2X_IOP_FAST=0/1 selects the interpreter as everywhere else.
#include "ps2x/iop/iop_subsystem.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace
{
    using namespace ps2x::iop;

    uint64_t hostInstructions()
    {
#if defined(__APPLE__)
        rusage_info_v4 usage{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t *>(&usage)) == 0)
            return usage.ri_instructions;
#endif
        return 0;
    }

    class BenchHost final : public IopHost
    {
    public:
        explicit BenchHost(std::string dir) : directory(std::move(dir)), guest(0x200000u, 0u) {}
        bool readGuest(uint32_t address, void *destination, size_t size) const override
        {
            if (address > guest.size() || size > guest.size() - address) return false;
            std::memcpy(destination, guest.data() + address, size);
            return true;
        }
        bool writeGuest(uint32_t address, const void *source, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address) return false;
            std::memcpy(guest.data() + address, source, size);
            return true;
        }
        bool zeroGuest(uint32_t address, size_t size) override
        {
            if (address > guest.size() || size > guest.size() - address) return false;
            std::memset(guest.data() + address, 0, size);
            return true;
        }
        bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override { normalized = address; return address < guest.size(); }
        uint32_t allocateIopHandle(IopHandleKind) override { return nextHandle += 0x80u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind) const override { return {}; }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view path) override
        {
            std::string leaf(path);
            const size_t slash = leaf.find_last_of("\\/:");
            if (slash != std::string::npos) leaf = leaf.substr(slash + 1u);
            const size_t semicolon = leaf.find(';');
            if (semicolon != std::string::npos) leaf.resize(semicolon);
            std::ifstream in(directory + "/" + leaf, std::ios::binary);
            if (!in) return 0u;
            files.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return files.size();
        }
        bool hostFileSize(uint64_t handle, uint64_t &size) const override
        {
            if (handle == 0u || handle > files.size()) return false;
            size = files[handle - 1u].size();
            return true;
        }
        bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (handle == 0u || handle > files.size() || offset > files[handle - 1u].size()) return false;
            bytesRead = std::min(size, files[handle - 1u].size() - static_cast<size_t>(offset));
            std::memcpy(destination, files[handle - 1u].data() + offset, bytesRead);
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel level, std::string_view message) override
        {
            if (level >= LogLevel::Warning && warnings++ < 20)
                std::cerr << "[iop] " << message << "\n";
        }

        std::string directory;
        std::vector<uint8_t> guest;
        std::vector<std::vector<char>> files;
        uint32_t nextHandle = 0x1000u;
        int warnings = 0;
    };
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <LIB31 dir> [seconds] [warmup] [chunk]\n";
        return 2;
    }
    const double seconds = argc > 2 ? std::atof(argv[2]) : 20.0;
    const double warmup = argc > 3 ? std::atof(argv[3]) : 5.0;
    const uint64_t chunk = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 2047u;
    BenchHost host(argv[1]);
    IopSubsystem iop(host);
    for (const char *name : {"SIO2MAN", "SIO2D", "PADMAN", "DBCMAN", "MC2_S1", "LIBSD", "CRI_ADXI", "SDRDRV", "MODHSYN", "MODMSIN",
                             "MODSESQ", "SKSOUND", "SKHSYNTH", "SKHSYNTH", "SKSESQ"})
    {
        const std::string path = std::string("cdrom0:\\LIB31\\") + name + ".IRX;1";
        const auto result = iop.loadModule(path, nullptr, 0u);
        std::printf("%-9s id=%d start=%d\n", name, result.moduleId, result.startResult);
    }
    const uint64_t clock = 36864000ull;
    auto run = [&](double guestSeconds) { for (uint64_t c = 0; c < static_cast<uint64_t>(guestSeconds * clock); c += chunk) iop.runEeCycles(chunk * 8u); };
    run(warmup);
    const auto s0 = iop.debugSnapshot();
    const uint64_t h0 = hostInstructions();
    const auto t0 = std::chrono::steady_clock::now();
    run(seconds);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const uint64_t h1 = hostInstructions();
    const auto s1 = iop.debugSnapshot();
    const double guestInstr = static_cast<double>(s1.emulatorInstructions - s0.emulatorInstructions);
    const double guestSec = static_cast<double>(s1.emulatorCycles - s0.emulatorCycles) / clock;
    const double calls = guestSec * clock / chunk;
    // state hash: all of IOP RAM + the cycle/instruction counters (identical for any correct interpreter)
    std::vector<uint8_t> ram(0x200000u);
    uint64_t hash = 1469598103934665603ull;
    if (iop.readMemory(0u, ram.data(), ram.size()))
        for (uint8_t byte : ram) hash = (hash ^ byte) * 1099511628211ull;
    hash = (hash ^ s1.emulatorCycles) * 1099511628211ull;
    hash = (hash ^ s1.emulatorInstructions) * 1099511628211ull;
    {
        // audio produced so far (silence unless the drivers were told to play): frame count and content hash
        const size_t pending = iop.pendingAudioFrames();
        std::vector<int16_t> pcm(pending * 2u + 2u);
        const size_t frames = iop.drainAudio(pcm.data(), pending, SIZE_MAX);
        for (size_t i = 0; i < frames * 2u; ++i) hash = (hash ^ static_cast<uint16_t>(pcm[i])) * 1099511628211ull;
        hash = (hash ^ frames) * 1099511628211ull;
        std::printf("audio frames %zu\n", frames);
    }
    std::printf("state hash %016llx (cycles %llu, instructions %llu)\n", (unsigned long long)hash, (unsigned long long)s1.emulatorCycles, (unsigned long long)s1.emulatorInstructions);
    std::printf("guest %.2f s: %.0f instr/s; wall %.3f s (%.1f ms per guest second)\n", guestSec, guestInstr / guestSec, wall, wall * 1000.0 / guestSec);
    if (h1 != 0u)
        std::printf("host instructions per guest second: %.3f M; per guest instruction: %.1f; per runEeCycles call: %.0f (%.0f calls/s)\n",
                    static_cast<double>(h1 - h0) / guestSec / 1e6, static_cast<double>(h1 - h0) / guestInstr, static_cast<double>(h1 - h0) / calls, calls / guestSec);
    return 0;
}
