#pragma once

#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/iop_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    // Where HLE services that replace an audio driver deliver decoded audio: 16-bit stereo PCM streams that the SPU2
    // mixes into the host output (implemented by IopSubsystem on top of Spu2::streamPush).
    class PcmSink
    {
    public:
        virtual ~PcmSink() = default;
        virtual void push(uint32_t stream, uint32_t rate, const int16_t *stereo, size_t frames) = 0;
        [[nodiscard]] virtual uint64_t consumed(uint32_t stream) const = 0;
        virtual void reset(uint32_t stream) = 0;
    };

    class IopService
    {
    public:
        virtual ~IopService() = default;

        [[nodiscard]] virtual std::string_view name() const = 0;
        [[nodiscard]] virtual std::span<const uint32_t> sids() const = 0;
        // A service with aliases is dormant until one of these IOP modules is
        // actually loaded.
        [[nodiscard]] virtual std::span<const std::string_view> moduleAliases() const
        {
            return {};
        }
        virtual void reset() = 0;

        // Called once after construction, only services that produce audio use it.
        virtual void attachPcmSink(PcmSink *sink)
        {
            (void)sink;
        }

        [[nodiscard]] virtual RpcAbi selectRpcAbi(const RpcAbiRequest &request) const
        {
            (void)request;
            return RpcAbi::RuntimeDefault;
        }

        [[nodiscard]] virtual RpcResult handleRpc(const RpcRequest &request) = 0;

        virtual void onSifTransfer(const SifTransfer &transfer)
        {
            (void)transfer;
        }

        virtual void appendDebugMetrics(std::vector<DebugMetric> &metrics) const
        {
            (void)metrics;
        }
    };

    using ServiceList = std::vector<std::unique_ptr<IopService>>;
}
