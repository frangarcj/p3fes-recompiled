#include "iop_emulator.h"
#include "imports/iop_cdvd.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        static constexpr uint64_t kSifDmaCompletionCycles = 2000u;
        static constexpr uint64_t kWorkDiagWindowCycles = 5u * 48000u * Spu2::CyclesPerSample;
        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            imports.setResolveCache(fastEnabled);
            reset();
            kernel.setOutsideThreadWait([this](const std::function<bool()> &ready) { return waitOutsideThread(ready); });
            // Memory cards (PCSX2 .ps2 format). Slot 1: PS2X_MEMCARD1, else
            // <elf dir>/memcards/Mcd001.ps2, created unformatted on first write. Slot 2 only
            // with PS2X_MEMCARD2. "none" or "0" leaves the slot empty.
            memory.sio2().setCardPathProvider([this](uint32_t port) -> std::string
            {
                const char *setting = std::getenv(port == 0u ? "PS2X_MEMCARD1" : "PS2X_MEMCARD2");
                if (setting != nullptr)
                {
                    const std::string value(setting);
                    return (value == "0" || value == "none") ? std::string() : value;
                }
                if (port != 0u)
                    return {};
                const std::string directory = host.hostPath(HostPathKind::ElfDirectory);
                const std::filesystem::path cards = directory.empty() ? std::filesystem::path("memcards")
                                                                      : std::filesystem::path(directory) / "memcards";
                std::error_code ignored;
                std::filesystem::create_directories(cards, ignored);
                return (cards / "Mcd001.ps2").string();
            });
        }

        void reset()
        {
            memory.reset();
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            ioman.reset();
            pendingDmaInterrupts.clear();
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            nextPcSampleInstruction = 0u;
            eeCycleCarry = 0;
            ++workDiagGeneration;
            clearWorkDiagWindow();
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (!memory.hasDmaStart()) // cheap check on every instruction
                return;
            while (const auto dma = memory.takeDmaStart())
                pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        // Library families the import dispatcher tells apart. With PS2X_IOP_FAST the family is classified once per
        // stub (IopImportCall::libraryClass) instead of comparing the name against each one in turn.
        enum LibraryClass : int32_t
        {
            LibSysmem, LibCdvdman, LibLoadcore, LibThbase, LibThreadman, LibThsemap, LibThevent, LibSifcmd, LibIntrman,
            LibSecrman, LibModload, LibIoman, LibSifman, LibVblank, LibTimrman, LibDmacman, LibStdio, LibSysclib,
            LibHeaplib, LibNone
        };
        static constexpr const char *kLibraryNames[] = {
            "sysmem", "cdvdman", "loadcore", "thbase", "threadman", "thsemap", "thevent", "sifcmd", "intrman",
            "secrman", "modload", "ioman", "sifman", "vblank", "timrman", "dmacman", "stdio", "sysclib", "heaplib"};

        static int32_t classifyLibrary(std::string_view name)
        {
            for (int32_t i = 0; i < LibNone; ++i)
                if (iequals(name, kLibraryNames[i]))
                    return i;
            return LibNone;
        }

        // iequals() against the library names for ASCII input: the lowercased name (up to 9 chars) is packed into
        // two words and looked up in a small open-addressing table built once.
        static int32_t classifyLibraryFast(const std::string &name)
        {
            struct Entry
            {
                uint64_t lo = 0, hi = 0;
                int32_t id = -1;
            };
            struct Table
            {
                std::array<Entry, 64> slots{};
                static uint32_t slot(uint64_t lo, uint64_t hi) { return static_cast<uint32_t>(((lo ^ (hi * 0x9E3779B97F4A7C15ull)) * 0xD6E8FEB86659FD93ull) >> 58u); }
                void insert(uint64_t lo, uint64_t hi, int32_t id)
                {
                    uint32_t i = slot(lo, hi);
                    while (slots[i].id >= 0)
                        i = (i + 1u) & 63u;
                    slots[i] = {lo, hi, id};
                }
            };
            static const Table table = []
            {
                Table t;
                for (int32_t i = 0; i < LibNone; ++i)
                {
                    uint64_t lo = 0, hi = 0;
                    const char *text = kLibraryNames[i];
                    for (size_t k = 0; text[k] != '\0'; ++k)
                        (k < 8u ? lo : hi) |= static_cast<uint64_t>(static_cast<uint8_t>(text[k])) << ((k & 7u) * 8u);
                    t.insert(lo, hi, i);
                }
                return t;
            }();
            const size_t length = name.size();
            if (length > 9u)
                return LibNone;
            uint64_t lo = 0, hi = 0;
            for (size_t k = 0; k < length; ++k)
            {
                char c = name[k];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c + ('a' - 'A'));
                (k < 8u ? lo : hi) |= static_cast<uint64_t>(static_cast<uint8_t>(c)) << ((k & 7u) * 8u);
            }
            for (uint32_t i = Table::slot(lo, hi); table.slots[i].id >= 0; i = (i + 1u) & 63u)
                if (table.slots[i].lo == lo && table.slots[i].hi == hi)
                    return table.slots[i].id;
            return LibNone;
        }

        template <bool Fast>
        static bool isLibrary(const IopImportCall &call, LibraryClass id)
        {
            if constexpr (Fast)
                return call.libraryClass == id;
            else
                return iequals(call.library, kLibraryNames[id]);
        }

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            return fastEnabled ? dispatchImportImpl<true>(call, cpu) : dispatchImportImpl<false>(call, cpu);
        }

        template <bool Fast>
        ImportDisposition dispatchImportImpl(const IopImportCall &call, CpuState &cpu)
        {
            const auto isLib = [](const IopImportCall &c, LibraryClass id) { return isLibrary<Fast>(c, id); };
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            // optional trace of import calls (PS2X_IOP_TRACE=<number of calls>).
            static const long traceLimit = []
            {
                const char *v = std::getenv("PS2X_IOP_TRACE");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            // PS2X_IOP_TRACE_EVERY=<n>: sampling, one call in n (to watch progress without slowing down).
            static const long traceEvery = []
            {
                const char *v = std::getenv("PS2X_IOP_TRACE_EVERY");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            // PS2X_IOP_TRACE_NOCLIB=1: do not trace sysclib (memcpy/strncmp...), which is most of the volume.
            static const bool traceNoClib = std::getenv("PS2X_IOP_TRACE_NOCLIB") != nullptr;
            static long traceCount = 0;
            static long callCount = 0;
            ++callCount;
            const bool traceSkip = traceNoClib && iequals(call.library, "sysclib");
            // PS2X_IOP_TRACE_FROM=<n>: the PS2X_IOP_TRACE trace starts at call n.
            static const long traceFrom = []
            {
                const char *v = std::getenv("PS2X_IOP_TRACE_FROM");
                return v ? std::strtol(v, nullptr, 10) : 0L;
            }();
            if (!traceSkip && ((callCount >= traceFrom && traceCount < traceLimit) || (traceEvery > 0 && callCount % traceEvery == 0)))
            {
                ++traceCount;
                std::ostringstream trace;
                trace << "[IOP:trace] #" << std::dec << callCount << " " << call.library << ':' << std::dec << call.ordinal << std::hex
                      << " a0=0x" << cpu.gpr[4] << " a1=0x" << cpu.gpr[5] << " a2=0x" << cpu.gpr[6]
                      << " a3=0x" << cpu.gpr[7] << " ra=0x" << cpu.gpr[31];
                log(LogLevel::Info, trace.str());
            }

            if (isLib(call, LibSysmem) && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (isLib(call, LibCdvdman) && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (isLib(call, LibLoadcore) && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (isLib(call, LibThbase) || isLib(call, LibThreadman))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (isLib(call, LibThsemap))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (isLib(call, LibThevent))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (isLib(call, LibSifcmd))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (isLib(call, LibIntrman) && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (isLib(call, LibSecrman))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 6: // SecrAuthCard: the emulated memory card has no MagicGate; report it as authenticated
                    setV0(memory.sio2Enabled() ? 1u : 0u);
                    return ImportDisposition::Handled;
                case 7: // SecrResetAuthCard
                    setV0(0);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (isLib(call, LibModload) && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (isLib(call, LibIoman) && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (isLib(call, LibSifman))
            {
                const bool handled = rpc.dispatchSifManImport(call.ordinal, cpu);
                // dbcman sends to the EE with sceSifSetDmaIntr and waits for its end-of-DMA callback.
                IopRpcBridge::DmaCompletionCallback callback{};
                if (rpc.takeDmaCompletionCallback(callback))
                    pendingGuestCallbacks.emplace(totalCycles + kSifDmaCompletionCycles,
                                                  ScheduledGuestCallback{callback.function, callback.gp, callback.argument});
                return handled ? ImportDisposition::Handled : ImportDisposition::Missing;
            }
            if (isLib(call, LibVblank) && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (isLib(call, LibTimrman) && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (isLib(call, LibDmacman))
            {
                // sio2man programs the SIO2's DMA 11 and 12 with sceSetSliceDMA/sceStartDMA.
                const uint32_t channel = a0;
                const bool sio2Channel = memory.sio2Enabled() && (channel == 11u || channel == 12u);
                const uint32_t channelBase = 0x1F801500u + (channel - 7u) * 0x10u;
                if (sio2Channel && call.ordinal == 28u) // sceSetSliceDMA(canal, direccion, palabras, bloques, sentido)
                {
                    const uint32_t direction = memory.read32(cpu.gpr[29] + 16u);
                    write32(channelBase, cpu.gpr[5] & 0xFFFFFFu);
                    write32(channelBase + 4u, (cpu.gpr[6] & 0xFFFFu) | (cpu.gpr[7] << 16u));
                    write32(channelBase + 8u, (direction & 1u) | 0x200u | (direction == 0u ? 0x40000000u : 0u));
                    setV0(1);
                    return ImportDisposition::Handled;
                }
                if (sio2Channel && call.ordinal == 32u) // sceStartDMA(canal)
                {
                    write32(channelBase + 8u, memory.read32(channelBase + 8u) | 0x01000000u);
                    setV0(0);
                    return ImportDisposition::Handled;
                }
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (isLib(call, LibStdio) && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (isLib(call, LibSysclib))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (isLib(call, LibHeaplib) && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        // PS2X_IOP_PROFILE=<file>: guest-side profile for finding hot guest code (use with PS2X_IOP_FAST=0, which steps
        // every instruction here). From instruction PS2X_IOP_PROFILE_FROM on it counts, per guest PC, per running
        // thread, per import call (thread, library:ordinal) and per load/store that leaves RAM (pc, address, opcode),
        // and rewrites <file> every 2 M instructions. Diagnostics only: nothing here changes guest execution.
        struct Profile
        {
            std::string path;
            uint64_t from = 0, startInstr = 0, startCycles = 0, nextDump = 0;
            bool started = false;
            std::vector<uint32_t> pc = std::vector<uint32_t>(kRamSize / 4u, 0u);
            std::map<std::pair<int, uint32_t>, uint64_t> imports; // (thread, lib/ordinal) -> calls
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> hw; // (pc, address) -> accesses, address low bits hold nothing else
            std::map<std::pair<uint32_t, uint32_t>, uint64_t> hwOp; // (pc, opcode) -> accesses
            std::map<int, uint64_t> threadInstr;
            std::map<int, uint32_t> threadEntry;
            std::map<std::string, uint32_t> libIds;
            uint64_t runCalls = 0, runCycles = 0, fastIdle = 0, readyLoops = 0, idleLoops = 0, cpuCalls = 0;
        };
        std::unique_ptr<Profile> profile = []() -> std::unique_ptr<Profile>
        {
            const char *path = std::getenv("PS2X_IOP_PROFILE");
            if (path == nullptr || path[0] == '\0')
                return nullptr;
            auto result = std::make_unique<Profile>();
            result->path = path;
            const char *from = std::getenv("PS2X_IOP_PROFILE_FROM");
            result->from = from ? std::strtoull(from, nullptr, 10) : 0ULL;
            return result;
        }();

        void profileDump()
        {
            std::ofstream out(profile->path, std::ios::trunc);
            out << "instructions " << (totalInstructions - profile->startInstr) << "\ncycles " << (totalCycles - profile->startCycles) << "\n";
            out << "sched calls " << profile->runCalls << " cyclesRequested " << profile->runCycles << " fastIdle " << profile->fastIdle << " readyLoops " << profile->readyLoops << " idleLoops " << profile->idleLoops << " cpuCalls " << profile->cpuCalls << "\n";
            timrman.describe(out);
            for (const auto &[tid, count] : profile->threadInstr)
                out << "thread " << tid << " entry " << std::hex << profile->threadEntry[tid] << std::dec << " instr " << count << "\n";
            for (uint32_t i = 0; i < profile->pc.size(); ++i)
                if (profile->pc[i])
                    out << "pc " << std::hex << (i * 4u) << std::dec << " " << profile->pc[i] << "\n";
            for (const auto &[key, count] : profile->imports)
                out << "imp " << key.first << " " << std::hex << (key.second >> 16u) << std::dec << ":" << (key.second & 0xFFFFu) << " " << count << "\n";
            for (const auto &[key, count] : profile->hw)
                out << "hw " << std::hex << key.first << " " << key.second << std::dec << " " << count << "\n";
            for (const auto &[key, count] : profile->hwOp)
                out << "hwop " << std::hex << key.first << " " << key.second << std::dec << " " << count << "\n";
            for (const auto &[name, id] : profile->libIds)
                out << "lib " << id << " " << name << "\n";
            out.close();
            std::ofstream ram(profile->path + ".ram", std::ios::binary | std::ios::trunc);
            const auto image = memory.ram();
            ram.write(reinterpret_cast<const char *>(image.data()), static_cast<std::streamsize>(image.size()));
        }

        void profileInstruction(CpuState &cpu)
        {
            Profile &p = *profile;
            if (!p.started)
            {
                if (totalInstructions < p.from)
                    return;
                p.started = true;
                p.startInstr = totalInstructions;
                p.startCycles = totalCycles;
                p.nextDump = totalInstructions + 2000000u;
            }
            else if (totalInstructions >= p.nextDump)
            {
                p.nextDump += 2000000u;
                profileDump();
            }
            const uint32_t phys = physicalAddress(cpu.pc);
            if (phys < kRamSize)
                ++p.pc[phys >> 2u];
            const IopThread *thread = kernel.currentThread();
            const int tid = thread ? thread->id : 0;
            ++p.threadInstr[tid];
            if (thread)
                p.threadEntry[tid] = thread->entry;
            const uint32_t word = memory.fetch32(cpu.pc);
            const uint32_t op = word >> 26u;
            if (op >= 0x20u && op <= 0x2Eu && op != 0x27u)
            {
                const uint32_t address = cpu.gpr[(word >> 21u) & 31u] + static_cast<uint32_t>(sign16(word));
                if (physicalAddress(address) >= kRamSize)
                {
                    ++p.hw[{phys, physicalAddress(address)}];
                    ++p.hwOp[{phys, op}];
                }
            }
        }

        void profileImport(const IopImportCall &call)
        {
            Profile &p = *profile;
            if (!p.started)
                return;
            std::string lower = call.library;
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto it = p.libIds.find(lower);
            if (it == p.libIds.end())
                it = p.libIds.emplace(lower, static_cast<uint32_t>(p.libIds.size() + 1u)).first;
            const IopThread *thread = kernel.currentThread();
            ++p.imports[{thread ? thread->id : 0, (it->second << 16u) | call.ordinal}];
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // import stubs start with "jr ra"; only those addresses are looked up in the tables.
            const uint32_t instruction = memory.fetch32(cpu.pc);
            std::optional<IopImportCall> import;
            if (instruction == 0x03E00008u)
            {
                import = imports.decode(cpu.pc);
                if (import && fastEnabled)
                    import->libraryClass = classifyLibraryFast(import->library);
            }
            if (import)
            {
                if (profile)
                    profileImport(*import);
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            // IOP PC sampling (PS2X_IOP_PC_EVERY=<instructions>), read once when the IOP is created.
            if (pcEvery != 0ULL && totalInstructions >= nextPcSampleInstruction)
            {
                std::ostringstream out;
                out << "[IOP:pc] instr=" << std::dec << totalInstructions << std::hex << " pc=0x" << cpu.pc
                    << " ra=0x" << cpu.gpr[31] << " sp=0x" << cpu.gpr[29] << " v0=0x" << cpu.gpr[2]
                    << " a0=0x" << cpu.gpr[4];
                if (const std::string *moduleName = moduleNameForPC(cpu.pc))
                    out << " module=" << *moduleName;
                log(LogLevel::Info, out.str());
                const uint64_t next = totalInstructions + pcEvery;
                nextPcSampleInstruction = next < totalInstructions ? UINT64_MAX : next;
            }

            if (profile)
                profileInstruction(cpu);
            const bool running = cpuCore.executeFetchedInstruction(cpu, instruction);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        // PS2X_IOP_FAST (read once; "0" = the original loop below): straight-line RAM/register code runs
        // through IopCpuCore::runBurst with the per-instruction checks of step() hoisted out of the loop. A burst is
        // only started when none of them can change inside it (no interrupt can fire, no DMA/callback becomes due, no
        // PC sample is due) and ends before the first instruction that could (see runBurst); that instruction, and
        // every one near a sentinel, import stub or hardware access, goes through step() exactly as before.
        const bool fastEnabled = []
        {
            const char *value = std::getenv("PS2X_IOP_FAST");
            // On by default since 2026-11-14 (differential fuzz and integration tests, Mac and console runs);
            // PS2X_IOP_FAST=0 selects the original loop.
            return value == nullptr || value[0] != '0';
        }();

        uint32_t runCpuFast(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                uint32_t ran = 0u;
                bool bailed = true;
                // checkInterrupt() without side effects: nothing in a burst can change what it looks at.
                const uint32_t status = cpu.cop0[12];
                const bool interruptDue = (status & 3u) == 1u && memory.interruptControl() != 0u &&
                                          (memory.interruptStatus() & memory.interruptMask()) != 0u;
                if (!interruptDue)
                {
                    uint64_t limit = instructionBudget - (totalInstructions - start);
                    if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    {
                        uint64_t due = UINT64_MAX;
                        for (const auto &entry : pendingDmaInterrupts)
                            due = std::min(due, entry.second);
                        limit = std::min<uint64_t>(limit, due > totalCycles ? due - totalCycles : 0u);
                    }
                    if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    {
                        const uint64_t due = pendingGuestCallbacks.begin()->first;
                        limit = std::min<uint64_t>(limit, due > totalCycles ? due - totalCycles : 0u);
                    }
                    if (pcEvery != 0ULL)
                        limit = std::min<uint64_t>(limit, nextPcSampleInstruction > totalInstructions ? nextPcSampleInstruction - totalInstructions : 0u);
                    if (limit != 0u)
                    {
                        ran = cpuCore.runBurst(cpu, static_cast<uint32_t>(limit), bailed);
                        totalInstructions += ran;
                        totalCycles += ran;
                    }
                }
                if (ran == 0u || bailed)
                {
                    if (!step(cpu))
                        break;
                }
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            if (fastEnabled)
                return runCpuFast(cpu, instructionBudget);
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            if (depth < kCallStackCapacity)
            {
                const uint32_t stackTop = kCallStackLimit - depth * kCallStackSize;
                cpu.gpr[29] = stackTop - 32u;
            }
            else if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
            {
                // Extremely deep re-entrancy borrows unused space below the
                // suspended caller's live frame. Stack growth remains away
                // from the caller, so its saved registers stay intact.
                cpu.gpr[29] = (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            }
            else
            {
                cpu.gpr[29] = kCallStackBase - 32u;
            }
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            if (!cpu.stopped && !cpu.yielded)
            {
                // warn instead of dropping the call silently.
                std::ostringstream out;
                out << "[IOP] guest call 0x" << std::hex << address << " exceeded its budget of " << std::dec << budget
                    << " instructions (pc=0x" << std::hex << cpu.pc << ")";
                log(LogLevel::Warning, out.str());
            }
            return cpu.gpr[2];
        }

        void runReadyThreads(uint64_t maxCycles, uint32_t serverPriority) override
        {
            // Only from outside IOP execution (an RPC from the EE), and without re-entering.
            if (activeCpu != nullptr || settlingThreads)
                return;
            settlingThreads = true;
            const uint64_t start = totalCycles;
            // Settle while a thread that would preempt the RPC server's thread is ready (the real IOP runs those before
            // the reply). Lower-priority threads wait: Persona 3's sound modules keep one ready at low priority, and
            // with "any ready thread" the IOP ran the full 4 M-cycle cap after each of the Sound Kit's ~18 RPCs a
            // frame, with the EE stopped (the game crawled at a few fields per second).
            static const bool settleAll = std::getenv("PS2X_IOP_SETTLE_ALL") != nullptr; // the old rule, for A/B tests
            while ((serverPriority != 0u && !settleAll ? kernel.hasReadyThreadAbove(serverPriority) : kernel.hasReadyThread()) &&
                   totalCycles - start < maxCycles)
                runCycles(kDefaultSlice * 16u);
            settlingThreads = false;
        }
        bool settlingThreads = false;

        // A wait with no current thread (e.g. WaitSema in an RPC server) lets the IOP threads run, as the real IOP
        // would by blocking the RPC thread, until the condition holds or kMaxOutsideWaitCycles pass.
        bool waitOutsideThread(const std::function<bool()> &ready)
        {
            if (ready())
                return true;
            if (waitingOutsideThread)
                return false;
            waitingOutsideThread = true;
            const uint64_t start = totalCycles;
            while (!ready() && totalCycles - start < kMaxOutsideWaitCycles)
                runCycles(kDefaultSlice);
            waitingOutsideThread = false;
            const bool satisfied = ready();
            if (!satisfied)
                log(LogLevel::Warning, "[IOP] wait outside a thread timed out");
            return satisfied;
        }
        bool waitingOutsideThread = false;
        static constexpr uint64_t kMaxOutsideWaitCycles = 36864000u; // ~1 s de reloj del IOP
        const unsigned long long pcEvery = []
        {
            const char *v = std::getenv("PS2X_IOP_PC_EVERY");
            return v ? std::strtoull(v, nullptr, 10) : 0ULL;
        }();

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty())
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        // The SPU2 produces one sample every 768 IOP cycles; its IRQ (IRQA reached) is interrupt 9.
        void serviceSpu2()
        {
            // PS2X_SPU2_IRQ=0 disables delivering the IRQ (to check whether the game behaves differently).
            static const bool irqEnabled = []
            {
                const char *value = std::getenv("PS2X_SPU2_IRQ");
                return value == nullptr || value[0] != '0';
            }();
            Spu2 &spu = memory.spu2();
            // nothing to do: no sample due, no ADMA completion and no IRQ pending (what the code below would find)
            if (!workDiagEnabled && spu.canSkipServiceAt(totalCycles))
                return;
            if (workDiagEnabled)
            {
                const uint64_t completedSamples = totalCycles / Spu2::CyclesPerSample;
                const uint64_t samplesDue = completedSamples >= workDiagSpu2CompletedSamples
                                                 ? completedSamples - workDiagSpu2CompletedSamples
                                                 : completedSamples;
                workDiagSpu2CompletedSamples = completedSamples;
                ++workDiagSpu2Calls;
                if (samplesDue == 0u)
                    ++workDiagSpu2Due0;
                else if (samplesDue == 1u)
                    ++workDiagSpu2Due1;
                else if (samplesDue <= 64u)
                    ++workDiagSpu2Due2To64;
                else
                    ++workDiagSpu2DueOver64;
            }
            spu.advanceTo(totalCycles);
            if (!workDiagEnabled && !spu.eventsPending())
                return; // no ADMA completion and no IRQ: the rest of this function would do nothing
            // ADMA transfers complete as the SPU2 takes their blocks: raise the core's DMA interrupt (4 / 7).
            for (uint32_t core = 0; core < 2u; ++core)
                while (spu.takeAdmaDone(core))
                    pendingDmaInterrupts[core == 0u ? 0x24 : 0x28] = totalCycles;
            if (!spu.takeInterrupt() || !irqEnabled || servicingSpu2Interrupt)
                return;
            servicingSpu2Interrupt = true;
            try
            {
                (void)intrman.dispatchInterrupt(Spu2::IopInterrupt, *this);
            }
            catch (...)
            {
                servicingSpu2Interrupt = false;
                throw;
            }
            servicingSpu2Interrupt = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        void clearWorkDiagWindow() noexcept
        {
            workDiagWindowStartCycles = totalCycles;
            workDiagSpu2CompletedSamples = totalCycles / Spu2::CyclesPerSample;
            workDiagRunCalls = 0u;
            workDiagRequestedCycles = 0u;
            workDiagMinRequestedCycles = 0u;
            workDiagMaxRequestedCycles = 0u;
            workDiagReadyLoops = 0u;
            workDiagIdleLoops = 0u;
            workDiagRunElapsedCycles = 0u;
            workDiagInstructions = 0u;
            workDiagSpu2Calls = 0u;
            workDiagSpu2Due0 = 0u;
            workDiagSpu2Due1 = 0u;
            workDiagSpu2Due2To64 = 0u;
            workDiagSpu2DueOver64 = 0u;
            workDiagWakeLimits = 0u;
            workDiagDmaLimits = 0u;
            workDiagCallbackLimits = 0u;
            workDiagTimerLimits = 0u;
            workDiagAdmaLimits = 0u;
            workDiagTargetLimits = 0u;
            workDiagFastIdle = 0u;
        }

        // How runCycles(target - now) can be skipped over. Pure: nothing can happen before `target` (the original idle
        // fast path). SpuCatchUp (PS2X_IOP_FAST): the same except that SPU2 samples fall due inside the interval; the
        // original loop would run serviceSpu2() (a no-op at `now`, which is required), find nothing else to do, jump
        // the clock to `target` and run serviceSpu2() once more, which is what the caller then does.
        enum class IdleJump { No, Pure, SpuCatchUp };

        IdleJump idleJumpTo(uint64_t target) const noexcept
        {
            const uint64_t current = totalCycles;
            if (target <= current || activeCpu != nullptr || callDepth != 0u || settlingThreads ||
                waitingOutsideThread || servicingDmaInterrupts || servicingGuestCallbacks ||
                servicingSpu2Interrupt)
                return IdleJump::No;
            // Require both SPU2 service points to be no-ops: no pending event
            // and no sample due anywhere through target. Other intervals use
            // the original loop, including its final sample/IRQ service.
            if (!kernel.cachedIdleCovers(current, target))
                return IdleJump::No;
            const bool spuIdleAtTarget = memory.spu2().canSkipServiceAt(target);
            if (!spuIdleAtTarget && (!fastEnabled || workDiagEnabled || !memory.spu2().canSkipServiceAt(current)))
                return IdleJump::No;

            // DMA is keyed by IRQ rather than deadline, so inspect every outstanding completion.
            for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
            {
                (void)irq;
                if (completionCycle < target)
                    return IdleJump::No;
            }
            if (!pendingGuestCallbacks.empty() && pendingGuestCallbacks.begin()->first < target)
                return IdleJump::No;
            if (timrman.nextEventCycle(target) < target)
                return IdleJump::No;

            constexpr uint64_t kAdmaWakeCycles = 64u * Spu2::CyclesPerSample;
            if ((memory.spu2().admaActive(0u) || memory.spu2().admaActive(1u)) &&
                target - current > kAdmaWakeCycles)
                return IdleJump::No;
            return spuIdleAtTarget ? IdleJump::Pure : IdleJump::SpuCatchUp;
        }

        bool canFastIdleTo(uint64_t target) const noexcept
        {
            return idleJumpTo(target) == IdleJump::Pure;
        }

        void emitWorkDiagWindow() noexcept
        {
            if (!workDiagEnabled || totalCycles < workDiagWindowStartCycles ||
                totalCycles - workDiagWindowStartCycles < kWorkDiagWindowCycles)
                return;

            try
            {
                std::ostringstream out;
                out << "[IOP:work] cycles=" << (totalCycles - workDiagWindowStartCycles)
                    << " run_calls=" << workDiagRunCalls
                    << " requested=" << workDiagRequestedCycles
                    << " requested_min=" << workDiagMinRequestedCycles
                    << " requested_max=" << workDiagMaxRequestedCycles
                    << " loops_ready=" << workDiagReadyLoops
                    << " loops_idle=" << workDiagIdleLoops
                    << " run_elapsed_cycles=" << workDiagRunElapsedCycles
                    << " instructions=" << workDiagInstructions
                    << " spu_calls=" << workDiagSpu2Calls
                    << " samples_due_0=" << workDiagSpu2Due0
                    << " samples_due_1=" << workDiagSpu2Due1
                    << " samples_due_2_64=" << workDiagSpu2Due2To64
                    << " samples_due_over64=" << workDiagSpu2DueOver64
                    << " idle_wake=" << workDiagWakeLimits
                    << " idle_dma=" << workDiagDmaLimits
                    << " idle_callback=" << workDiagCallbackLimits
                    << " idle_timer=" << workDiagTimerLimits
                    << " idle_adma=" << workDiagAdmaLimits
                    << " idle_target=" << workDiagTargetLimits
                    << " idle_fast=" << workDiagFastIdle;
                log(LogLevel::Info, out.str());
            }
            catch (...)
            {
                // Optional diagnostics must not affect the emulated scheduler.
            }
            clearWorkDiagWindow();
        }

        void runCycles(uint64_t cycles) noexcept
        {
            const bool workDiagOutermost = workDiagEnabled && workDiagRunDepth == 0u;
            uint64_t workDiagEntryCycles = 0u;
            uint64_t workDiagEntryInstructions = 0u;
            uint64_t workDiagEntryGeneration = 0u;
            if (workDiagEnabled)
            {
                workDiagEntryCycles = totalCycles;
                workDiagEntryInstructions = totalInstructions;
                workDiagEntryGeneration = workDiagGeneration;
                ++workDiagRunDepth;
            }
            try
            {
                const uint64_t target = totalCycles + cycles;
                if (profile && profile->started)
                {
                    ++profile->runCalls;
                    profile->runCycles += cycles;
                }
                const IdleJump idleJump = idleFastEnabled ? idleJumpTo(target) : IdleJump::No;
                if (idleJump == IdleJump::SpuCatchUp)
                {
                    if (profile && profile->started)
                        ++profile->fastIdle;
                    totalCycles = target;
                    serviceSpu2(); // the loop's final service, with the SPU2 samples that fell due on the way
                }
                else if (idleJump == IdleJump::Pure)
                {
                    if (profile && profile->started)
                        ++profile->fastIdle;
                    totalCycles = target;
                    if (workDiagEnabled)
                        ++workDiagFastIdle;
                }
                else
                {
                    while (totalCycles < target)
                    {
                        serviceSpu2();
                        if (!pendingDmaInterrupts.empty())
                            servicePendingDmaInterrupts();
                        if (!pendingGuestCallbacks.empty())
                            servicePendingGuestCallbacks();
                        if (!timrman.nothingDueAt(totalCycles))
                            timrman.serviceDue(totalCycles, *this);
                        IopThread *next = kernel.beginNextReady(totalCycles);
                        if (!next)
                        {
                            if (profile && profile->started)
                                ++profile->idleLoops;
                            if (workDiagEnabled)
                                ++workDiagIdleLoops;
                            // with PS2X_IOP_PC_EVERY, report now and then that the IOP is idle.
                            static unsigned long long idleCount = 0ULL;
                            // getenv scans the whole environment; in this idle loop it took almost 40% of the game thread
                            // (ucrtbase!strchr). It is read once.
                            static const bool idleTrace = std::getenv("PS2X_IOP_PC_EVERY") != nullptr;
                            if (idleTrace && (++idleCount % 200000ULL) == 1ULL)
                            {
                                std::ostringstream out;
                                out << "[IOP:idle] no ready threads (count=" << idleCount << ") instr=" << totalInstructions;
                                log(LogLevel::Info, out.str());
                            }
                            uint64_t nextWake = kernel.nextWakeCycle(target);
                            if (workDiagEnabled)
                            {
                                enum class WakeSource : uint8_t
                                {
                                    Wake,
                                    Target,
                                    Dma,
                                    Callback,
                                    Timer,
                                    Adma,
                                };
                                WakeSource source = nextWake < target ? WakeSource::Wake : WakeSource::Target;
                                for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                                {
                                    (void)irq;
                                    if (completionCycle < nextWake)
                                    {
                                        nextWake = completionCycle;
                                        source = WakeSource::Dma;
                                    }
                                }
                                if (!pendingGuestCallbacks.empty() && pendingGuestCallbacks.begin()->first < nextWake)
                                {
                                    nextWake = pendingGuestCallbacks.begin()->first;
                                    source = WakeSource::Callback;
                                }
                                const uint64_t timerWake = timrman.nextEventCycle(nextWake);
                                if (timerWake < nextWake)
                                {
                                    nextWake = timerWake;
                                    source = WakeSource::Timer;
                                }
                                // P3FES: while a core streams ADMA input, wake at least every 64 samples so its DMA
                                // completions reach the driver before the input buffer runs dry (idle jumps otherwise
                                // skip whole blocks and the stream drops samples).
                                if (memory.spu2().admaActive(0u) || memory.spu2().admaActive(1u))
                                {
                                    const uint64_t admaWake = totalCycles + 64u * Spu2::CyclesPerSample;
                                    if (admaWake < nextWake)
                                    {
                                        nextWake = admaWake;
                                        source = WakeSource::Adma;
                                    }
                                }
                                switch (source)
                                {
                                case WakeSource::Wake: ++workDiagWakeLimits; break;
                                case WakeSource::Target: ++workDiagTargetLimits; break;
                                case WakeSource::Dma: ++workDiagDmaLimits; break;
                                case WakeSource::Callback: ++workDiagCallbackLimits; break;
                                case WakeSource::Timer: ++workDiagTimerLimits; break;
                                case WakeSource::Adma: ++workDiagAdmaLimits; break;
                                }
                            }
                            else
                            {
                                for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                                    nextWake = std::min(nextWake, completionCycle);
                                if (!pendingGuestCallbacks.empty())
                                    nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                                nextWake = timrman.nextEventCycle(nextWake);
                                // P3FES: while a core streams ADMA input, wake at least every 64 samples so its DMA
                                // completions reach the driver before the input buffer runs dry (idle jumps otherwise
                                // skip whole blocks and the stream drops samples).
                                if (memory.spu2().admaActive(0u) || memory.spu2().admaActive(1u))
                                    nextWake = std::min<uint64_t>(nextWake, totalCycles + 64u * Spu2::CyclesPerSample);
                            }
                            totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                            continue;
                        }
                        if (workDiagEnabled)
                            ++workDiagReadyLoops;
                        const uint64_t before = totalCycles;
                        if (profile && profile->started)
                            ++profile->readyLoops;
                        runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                        kernel.endTimeslice(*next, kThreadReturnSentinel);
                        if (totalCycles == before)
                            ++totalCycles;
                    }
                    serviceSpu2(); // bring the SPU2 up to date after skipping idle cycles to the end
                }
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
            memory.spu2().flushOutput(); // PS2X_SPU2_BATCH: publish the frames staged by this run (no-op otherwise)
            if (workDiagEnabled)
            {
                --workDiagRunDepth;
                if (workDiagEntryGeneration == workDiagGeneration)
                {
                    if (workDiagRunCalls == 0u)
                        workDiagMinRequestedCycles = cycles;
                    else
                        workDiagMinRequestedCycles = std::min(workDiagMinRequestedCycles, cycles);
                    workDiagMaxRequestedCycles = std::max(workDiagMaxRequestedCycles, cycles);
                    ++workDiagRunCalls;
                    workDiagRequestedCycles += cycles;
                    if (workDiagOutermost)
                    {
                        if (totalCycles >= workDiagEntryCycles)
                            workDiagRunElapsedCycles += totalCycles - workDiagEntryCycles;
                        if (totalInstructions >= workDiagEntryInstructions)
                            workDiagInstructions += totalInstructions - workDiagEntryInstructions;
                    }
                }
                if (workDiagRunDepth == 0u)
                    emitWorkDiagWindow();
            }
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            // As the real loadcore, the module starts with start(argc, argv): argv[0] is the module path and the
            // arguments are separated by '\0' ("rpc_priority=64\0..."). It used to pass (size in bytes, pointer to the
            // text) and 989snd read the text as pointers ("989snd Error: cause 7").
            std::vector<std::string> argStrings;
            argStrings.push_back(module.path);
            if (arguments && argumentSize)
            {
                const char *text = static_cast<const char *>(arguments);
                size_t begin = 0u;
                for (size_t i = 0u; i <= argumentSize; ++i)
                {
                    if (i == argumentSize || text[i] == '\0')
                    {
                        if (i > begin)
                            argStrings.emplace_back(text + begin, i - begin);
                        begin = i + 1u;
                    }
                }
            }
            uint32_t stringBytes = 0u;
            for (const std::string &value : argStrings)
                stringBytes += static_cast<uint32_t>(value.size()) + 1u;
            const uint32_t pointerBytes = static_cast<uint32_t>(argStrings.size() + 1u) * 4u;
            const uint32_t args = allocate(pointerBytes + stringBytes, 16u);
            const uint32_t argc = static_cast<uint32_t>(argStrings.size());
            if (args)
            {
                uint32_t cursor = args + pointerBytes;
                for (size_t i = 0u; i < argStrings.size(); ++i)
                {
                    memory.write32(args + static_cast<uint32_t>(i) * 4u, cursor);
                    writeRam(cursor, argStrings[i].data(), static_cast<uint32_t>(argStrings[i].size()));
                    write8(cursor + static_cast<uint32_t>(argStrings[i].size()), 0u);
                    cursor += static_cast<uint32_t>(argStrings[i].size()) + 1u;
                }
                memory.write32(args + pointerBytes - 4u, 0u);
            }
            const uint32_t startResult = callFunction(module.entry, args ? argc : 0u, args, 0u, 0u, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " module=" << modules[result.moduleId].name
                << " path='" << modules[result.moduleId].path << "'"
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        const std::string *moduleNameForPC(uint32_t pc) const noexcept
        {
            const uint32_t address = physicalAddress(pc);
            for (const auto &[id, module] : modules)
                if (address >= module.base && address - module.base < module.size)
                    return &module.name;
            return nullptr;
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        bool servicingSpu2Interrupt = false;
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        uint64_t nextPcSampleInstruction = 0;
        uint64_t eeCycleCarry = 0;
        const bool idleFastEnabled = []
        {
            const char *value = std::getenv("PS2X_IOP_IDLE_FAST");
            return value == nullptr || value[0] != '0';
        }();
        // Optional counters are confined to the IOP worker; they use plain integers and no per-iteration clock reads.
#if defined(PS2X_IOP_WORK_DIAGNOSTICS)
        const bool workDiagEnabled = []
        {
            const char *value = std::getenv("PS2X_IOP_WORK_DIAG");
            return value != nullptr && value[0] == '1' && value[1] == '\0';
        }();
#else
        static constexpr bool workDiagEnabled = false;
#endif
        uint64_t workDiagGeneration = 0u;
        uint32_t workDiagRunDepth = 0u;
        uint64_t workDiagWindowStartCycles = 0u;
        uint64_t workDiagRunCalls = 0u;
        uint64_t workDiagRequestedCycles = 0u;
        uint64_t workDiagMinRequestedCycles = 0u;
        uint64_t workDiagMaxRequestedCycles = 0u;
        uint64_t workDiagReadyLoops = 0u;
        uint64_t workDiagIdleLoops = 0u;
        uint64_t workDiagRunElapsedCycles = 0u;
        uint64_t workDiagInstructions = 0u;
        uint64_t workDiagSpu2CompletedSamples = 0u;
        uint64_t workDiagSpu2Calls = 0u;
        uint64_t workDiagSpu2Due0 = 0u;
        uint64_t workDiagSpu2Due1 = 0u;
        uint64_t workDiagSpu2Due2To64 = 0u;
        uint64_t workDiagSpu2DueOver64 = 0u;
        uint64_t workDiagWakeLimits = 0u;
        uint64_t workDiagDmaLimits = 0u;
        uint64_t workDiagCallbackLimits = 0u;
        uint64_t workDiagTimerLimits = 0u;
        uint64_t workDiagAdmaLimits = 0u;
        uint64_t workDiagTargetLimits = 0u;
        uint64_t workDiagFastIdle = 0u;
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    namespace
    {
        // The EE charges its cycles in small steps (tens of thousands of
        // times per second). Entering the IOP each time costs far more than
        // the IOP work itself: on a PS Vita the entry/service machinery took
        // half of the frame while the IOP ran ~260k instructions/s. EE cycles
        // are accumulated and delivered in blocks; runCycles still steps
        // internally from event to event, so only the EE/IOP interleaving
        // coarsens. PS2X_IOP_BATCH_CYCLES is the block size in IOP cycles
        // (36.864 MHz); 0 delivers every step as before.
        uint64_t iopBatchEeCycles() noexcept
        {
            static const uint64_t value = []
            {
                const char *v = std::getenv("PS2X_IOP_BATCH_CYCLES");
                // Off by default: on a PS Vita a 16384-cycle block hung the boot and a 2048 one starved the movie path.
                const unsigned long long iopCycles = v ? std::strtoull(v, nullptr, 10) : 0ULL;
                return static_cast<uint64_t>(iopCycles > 1000000ULL ? 1000000ULL : iopCycles) * 8u;
            }();
            return value;
        }
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry + eeCycles;
        if (total < iopBatchEeCycles())
        {
            m_impl->eeCycleCarry = total;
            return;
        }
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    // Brings the IOP clock up to the EE before an interaction that the IOP
    // must see at the right time.
    void IopEmulator::deliverPendingEeCycles() noexcept
    {
        const uint64_t total = m_impl->eeCycleCarry;
        const uint64_t iopCycles = total / 8u;
        m_impl->eeCycleCarry = total % 8u;
        if (iopCycles)
            m_impl->runCycles(iopCycles);
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        deliverPendingEeCycles();
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        deliverPendingEeCycles();
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    size_t IopEmulator::drainAudio(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames)
    {
        return m_impl->memory.spu2().drainOutput(stereo, maxFrames, maxLatencyFrames);
    }

    void IopEmulator::pushPcmStream(uint32_t stream, uint32_t rate, const int16_t *stereo, size_t frames)
    {
        m_impl->memory.spu2().streamPush(stream, rate, stereo, frames);
    }

    uint64_t IopEmulator::pcmStreamConsumed(uint32_t stream) const
    {
        return m_impl->memory.spu2().streamConsumed(stream);
    }

    void IopEmulator::resetPcmStream(uint32_t stream)
    {
        m_impl->memory.spu2().streamReset(stream);
    }

    size_t IopEmulator::pendingAudioFrames() const
    {
        return m_impl->memory.spu2().pendingFrames();
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

}
