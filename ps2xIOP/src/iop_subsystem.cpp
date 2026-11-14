#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <map>
#include "ps2x/iop/iop_subsystem.h"

#include "iop_service.h"
#include "iop_module_manager.h"
#include "emulator/iop_emulator.h"
#include "module_factories.h"
#include "ps2x/iop/ps2_path.h"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ps2x::iop
{
    class IopSubsystem::Impl
    {
    public:
        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              emulator(hostRef)
        {
            coreServices.emplace_back(detail::createMcservService(host));
            coreServices.emplace_back(detail::createDbcmanService(host));
            coreServices.emplace_back(detail::createLibSdService(host));
            coreServices.emplace_back(detail::createCriDtxService(host));
            for (const auto &service : coreServices)
                service->attachPcmSink(&pcmSink);
            refreshServiceModuleKeys();
            rebuildRoutes();
        }

        bool serviceActive(const detail::IopService &service) const
        {
            return moduleManager.isLoaded(service.moduleAliases());
        }

        void refreshServiceModuleKeys()
        {
            std::vector<std::string> keys;
            for (const auto &service : coreServices)
            {
                for (std::string_view alias : service->moduleAliases())
                    keys.emplace_back(alias);
            }
            moduleManager.setServiceModuleKeys(std::move(keys));
        }

        void rebuildRoutes()
        {
            routes.clear();
            lastError.clear();
            for (const auto &service : coreServices)
            {
                if (!serviceActive(*service))
                    continue;
                for (const uint32_t sid : service->sids())
                {
                    if (!routes.emplace(sid, service.get()).second)
                    {
                        std::ostringstream out;
                        out << "duplicate IOP SID 0x" << std::hex << sid << " in core services";
                        lastError = out.str();
                        routes.clear();
                        return;
                    }
                }
            }
        }

        void recordLoadOutcome(std::string_view path, bool hle)
        {
            constexpr size_t maxOutcomes = 32u;
            if (loadOutcomes.size() >= maxOutcomes || !loggedLoadPaths.emplace(path).second)
                return;
            std::string message = hle ? "[IOP:HLE] fallback module='" : "[IOP:load-failed] module='";
            message.append(path);
            message += hle ? "' physical IRX unavailable; using registered HLE provider"
                           : "' no HLE provider accepted the module; physical IRX was not loaded";
            loadOutcomes.push_back(message);
            host.log(hle ? LogLevel::Info : LogLevel::Warning, message);
        }

        IopHost &host;
        detail::ServiceList coreServices;
        std::unordered_map<uint32_t, detail::IopService *> routes;
        std::vector<std::string> loadOutcomes;
        std::unordered_set<std::string> loggedLoadPaths;
        std::string lastError;
        detail::IopModuleManager moduleManager;
        detail::IopEmulator emulator;

        class EmulatorPcmSink final : public detail::PcmSink
        {
        public:
            explicit EmulatorPcmSink(detail::IopEmulator &target)
                : m_emulator(target)
            {
            }
            void push(uint32_t stream, uint32_t rate, const int16_t *stereo, size_t frames) override
            {
                m_emulator.pushPcmStream(stream, rate, stereo, frames);
            }
            [[nodiscard]] uint64_t consumed(uint32_t stream) const override
            {
                return m_emulator.pcmStreamConsumed(stream);
            }
            void reset(uint32_t stream) override
            {
                m_emulator.resetPcmStream(stream);
            }

        private:
            detail::IopEmulator &m_emulator;
        };
        EmulatorPcmSink pcmSink{emulator};
    };

    IopSubsystem::IopSubsystem(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopSubsystem::~IopSubsystem() = default;
    IopSubsystem::IopSubsystem(IopSubsystem &&) noexcept = default;
    IopSubsystem &IopSubsystem::operator=(IopSubsystem &&) noexcept = default;

    void IopSubsystem::reset()
    {
        m_impl->moduleManager.reset();
        m_impl->loadOutcomes.clear();
        m_impl->loggedLoadPaths.clear();
        for (auto &service : m_impl->coreServices)
        {
            if (service)
            {
                service->reset();
            }
        }
        m_impl->emulator.reset();
        m_impl->refreshServiceModuleKeys();
        m_impl->rebuildRoutes();
    }

    ModuleLoadResult IopSubsystem::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        const ParsedPs2Path parsed = parsePs2Path(path);
        if (!parsed)
            return {true, -1, -1};

        // PS2X_IOP_HLE_ONLY=1 never loads a physical IRX: every module goes to
        // its registered HLE provider (or is reported as loaded with no
        // services), so no R3000 code runs at all. PS2X_IOP_SKIP_IRX is the
        // same for a comma-separated list of module names (e.g. "CRI_ADXI,SKSOUND").
        static const bool hleOnlyAll = []
        {
            const char *value = std::getenv("PS2X_IOP_HLE_ONLY");
            return value != nullptr && value[0] == '1';
        }();
        static const std::string skipList = []
        {
            const char *value = std::getenv("PS2X_IOP_SKIP_IRX");
            std::string list = value ? value : "";
            for (char &c : list)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return "," + list + ",";
        }();
        bool hleOnly = hleOnlyAll;
        if (!hleOnly && skipList.size() > 2u)
        {
            std::string upper(path);
            for (char &c : upper)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const size_t slash = upper.find_last_of("\\/:");
            std::string leaf = slash == std::string::npos ? upper : upper.substr(slash + 1u);
            const size_t dot = leaf.find('.');
            if (dot != std::string::npos)
                leaf.resize(dot);
            hleOnly = skipList.find("," + leaf + ",") != std::string::npos;
        }
        if (parsed.device != Ps2PathDevice::Rom0 && !hleOnly)
        {
            ModuleLoadResult physical = m_impl->emulator.loadModule(path, arguments, argumentSize);
            if (physical.moduleId > 0)
            {
                m_impl->moduleManager.observePhysicalLoad(physical.moduleId, path);
                m_impl->rebuildRoutes();
                return physical;
            }
        }

        ModuleLoadResult hle = m_impl->moduleManager.loadHle(path);
        if (hle.moduleId > 0)
        {
            m_impl->rebuildRoutes();
            if (parsed.device != Ps2PathDevice::Rom0)
                m_impl->recordLoadOutcome(path, true);
        }
        else
        {
            m_impl->recordLoadOutcome(path, false);
            if (hleOnly)
            {
                // Probe mode: report the module as loaded with no services so
                // the boot shows which RPC servers the game then waits for.
                static int32_t nextStubId = 0x7000;
                return {true, nextStubId++, 0};
            }
        }
        return hle;
    }

    ModuleLoadResult IopSubsystem::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->emulator.loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopSubsystem::stopModule(int32_t moduleId, int32_t *result)
    {
        if (m_impl->moduleManager.stopHle(moduleId, result))
        {
            m_impl->rebuildRoutes();
            return true;
        }
        if (!m_impl->emulator.stopModule(moduleId, result))
            return false;
        m_impl->moduleManager.observePhysicalStop(moduleId);
        m_impl->rebuildRoutes();
        return true;
    }

    void IopSubsystem::runEeCycles(uint64_t eeCycles) noexcept
    {
        m_impl->emulator.runEeCycles(eeCycles);
    }

    RpcAbi IopSubsystem::selectRpcAbi(const RpcAbiRequest &request) const
    {
        for (const auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                const RpcAbi selected = service->selectRpcAbi(request);
                if (selected != RpcAbi::RuntimeDefault)
                {
                    return selected;
                }
            }
        }
        return RpcAbi::RuntimeDefault;
    }

    bool IopSubsystem::canBindRpc(uint32_t sid) const noexcept
    {
        if (m_impl->routes.find(sid) != m_impl->routes.end())
        {
            return true;
        }
        return m_impl->emulator.hasRpcServer(sid);
    }

    RpcResult IopSubsystem::handleRpc(const RpcRequest &request)
    {
        // Debug aid: PS2X_IOP_RPC_COUNTS=1 prints, every 2000 calls, how many RPC calls each server id / function got.
        static const bool rpcLog = std::getenv("PS2X_IOP_RPC_COUNTS") != nullptr;
        if (rpcLog)
        {
            static std::map<std::pair<uint32_t, uint32_t>, uint32_t> counts;
            static uint32_t total = 0;
            ++counts[{request.sid, request.function}];
            if (++total % 2000u == 0u)
            {
                std::fprintf(stderr, "[rpc] %u calls:", total);
                for (const auto &[key, n] : counts)
                    std::fprintf(stderr, " %08x/%u=%u", key.first, key.second, n);
                std::fprintf(stderr, "\n");
                counts.clear();
            }
        }
        const auto route = m_impl->routes.find(request.sid);
        detail::IopService *hle = route != m_impl->routes.end() ? route->second : nullptr;

        // Debug aid: PS2X_IOP_RPC_TRACE=<hex sid> dumps the send words and the reply words of every call to that
        // server (after the physical IRX or the HLE service has answered), to compare a physical IRX with its HLE
        // replacement.
        static const uint32_t traceSid = []
        {
            const char *value = std::getenv("PS2X_IOP_RPC_TRACE");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 16)) : 0u;
        }();
        const auto traceCall = [&](const char *who, const RpcResult &served)
        {
            if (traceSid == 0u || request.sid != traceSid)
                return;
            const auto words = [&](GuestBuffer buffer)
            {
                std::ostringstream out;
                uint32_t value = 0u;
                for (uint32_t i = 0u; i + 4u <= buffer.size && i < 64u; i += 4u)
                {
                    if (buffer.address == 0u || !m_impl->host.readGuest(buffer.address + i, &value, sizeof(value)))
                        break;
                    out << (i ? "," : "") << std::hex << value;
                }
                return out.str();
            };
            std::fprintf(stderr, "[iop-rpc-trace] %s sid=%08x fn=%x handled=%d send(%u)=[%s] recv(%u)=[%s]\n", who,
                         request.sid, request.function, served.handled ? 1 : 0, request.send.size,
                         words(request.send).c_str(), request.receive.size, words(request.receive).c_str());
        };

        RpcResult emulated = m_impl->emulator.handleRpc(request);
        if (emulated.handled || !hle)
        {
            traceCall("iop", emulated);
            return emulated;
        }
        RpcResult served = hle->handleRpc(request);
        traceCall("hle", served);
        return served;
    }

    void IopSubsystem::onSifTransfer(const SifTransfer &transfer)
    {
        // Debug aid: PS2X_IOP_DTX_TRACE=1 prints every EE -> IOP SIF DMA of 0x880 bytes (the CRI DTX channel blocks)
        // whose first word (entry count) is non-zero, with its first entries.
        static const bool traceXfer = std::getenv("PS2X_IOP_DTX_TRACE") != nullptr;
        // PS2X_IOP_DTX_TRACE=all also lists every other EE -> IOP DMA (source, destination, size).
        static const bool traceAll = traceXfer && std::string_view(std::getenv("PS2X_IOP_DTX_TRACE")) == "all";
        if (traceAll && transfer.kind == SifTransferKind::SetDma && transfer.phase == SifTransferPhase::AfterCopy &&
            transfer.size != 0x880u)
            std::fprintf(stderr, "[iop-dma] src=%08x dst=%08x size=%x\n", transfer.sourceAddress,
                         transfer.destinationAddress, transfer.size);
        if (traceXfer && transfer.kind == SifTransferKind::SetDma && transfer.phase == SifTransferPhase::AfterCopy &&
            transfer.size == 0x880u)
        {
            uint32_t block[4 + 4 * 6] = {};
            if (m_impl->emulator.readMemory(transfer.destinationAddress, block, sizeof(block)) && block[0] != 0u)
            {
                std::fprintf(stderr, "[iop-dtx-xfer] src=%08x dst=%08x count=%u", transfer.sourceAddress,
                             transfer.destinationAddress, block[0]);
                for (uint32_t i = 0; i < 6u; ++i)
                    std::fprintf(stderr, " | %08x %08x %08x %08x", block[4 + i * 4], block[5 + i * 4], block[6 + i * 4],
                                 block[7 + i * 4]);
                std::fprintf(stderr, "\n");
            }
        }
        for (auto &service : m_impl->coreServices)
        {
            if (service && m_impl->serviceActive(*service))
            {
                service->onSifTransfer(transfer);
            }
        }
        m_impl->emulator.onSifTransfer(transfer);
    }

    uint32_t IopSubsystem::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->emulator.allocateMemory(size, alignment);
    }

    bool IopSubsystem::freeMemory(uint32_t address)
    {
        return m_impl->emulator.freeMemory(address);
    }

    bool IopSubsystem::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return m_impl->emulator.readMemory(address, destination, size);
    }

    bool IopSubsystem::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return m_impl->emulator.writeMemory(address, source, size);
    }

    bool IopSubsystem::zeroMemory(uint32_t address, size_t size)
    {
        return m_impl->emulator.zeroMemory(address, size);
    }

    bool IopSubsystem::isMemoryRange(uint32_t address, size_t size) const
    {
        return m_impl->emulator.isMemoryRange(address, size);
    }

    size_t IopSubsystem::pendingAudioFrames() const
    {
        return m_impl->emulator.pendingAudioFrames();
    }

    size_t IopSubsystem::drainAudio(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames)
    {
        return m_impl->emulator.drainAudio(stereo, maxFrames, maxLatencyFrames);
    }

    DebugSnapshot IopSubsystem::debugSnapshot() const
    {
        DebugSnapshot snapshot;
        snapshot.emulatorCycles = m_impl->emulator.cycles();
        snapshot.emulatorInstructions = m_impl->emulator.instructions();
        snapshot.emulatorLoadedModules = m_impl->emulator.loadedModuleCount();
        snapshot.emulatorThreads = m_impl->emulator.threadCount();
        snapshot.emulatorRpcServers = m_impl->emulator.rpcServerCount();
        snapshot.diagnostics = m_impl->loadOutcomes;
        if (!m_impl->lastError.empty())
        {
            snapshot.diagnostics.push_back(m_impl->lastError);
        }

        for (const auto &service : m_impl->coreServices)
        {
            DebugService row;
            row.name = service->name();
            row.sids.assign(service->sids().begin(), service->sids().end());
            row.active = m_impl->serviceActive(*service);
            service->appendDebugMetrics(row.metrics);
            snapshot.services.push_back(std::move(row));
        }
        return snapshot;
    }
}
