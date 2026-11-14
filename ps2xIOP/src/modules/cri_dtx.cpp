#include "module_factories.h"
#include "rpc_reply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

// HLE replacement for the RPC server of CRI_ADXI.IRX ("CRI ADX Driver Ver.9.44"), sid 0x90000200 ("DTX").
//
// Everything below was reverse engineered from the EE side of the game (libadxe in ps2xRuntime/src/runner, names from
// p3fes/p3fes_map.csv) and checked against a trace of the physical IRX answering (PS2X_IOP_RPC_TRACE=90000200,
// PS2X_IOP_TRACE_DMA, PS2X_IOP_DTX_TRACE). On the PS2 the ADX decoder runs on the EE; the IRX is only the renderer
// that plays the decoded PCM through the SPU2. The wire protocol has two layers:
//
//  1. Plain SIF RPCs (EE -> IOP) on sid 0x90000200. DTX_CallUrpc (0x559C10) sends function 0x400 + n ("urpc n") and
//     reads the first reply word as the result. dtx_create_rmt (0x559588) / dtx_destroy_rmt (0x5595F8) use functions 2
//     and 3 to open and close a DTX channel.
//  2. A DTX channel is a pair of 0x880-byte buffers, one in EE RAM (DTX_Create a1) and one in IOP RAM (a2, allocated by
//     the EE with sceSifAllocIopHeap). The last 0x40 bytes are a header whose word +0x3C (buffer offset 0x87C) is a
//     counter. Every EE tick (DTX_ExecServer 0x559938, once per frame per channel) the EE builds a command block, DMAs
//     it to the IOP buffer, and polls its own buffer until the counter there exceeds the one it sent; then the next
//     tick sends again (stop and wait). The IOP side therefore has to answer every block, even an empty one, with the
//     counter incremented by one, or that channel stalls for good.
//
// Block layout (both directions): word 0 = entry count, entries of 16 bytes from offset 0x10, counter at 0x87C.
//
// Channel 0 is "SJX" (sjx_sndcbf 0x564400 / sjx_rcvcbf 0x564360) and moves stream-joint (SJ) chunks. An SJ is a ring
// buffer in IOP RAM; the EE DMAs decoded 16-bit mono PCM (one SJ per output channel) straight into that RAM and
// announces it with an entry {u8 0, u8 id=1, u16 0, u32 sjx handle, u32 chunk address (IOP RAM), u32 length}. The
// IOP gives the space back, once it has played it, with an entry {u8 0, u8 id=0, u16 0, u32 EE slot address, u32 chunk
// address, u32 length}; the EE-side SJ only accepts new PCM as that free space comes back, so the pace of the returns
// is the audio clock of the whole game (the opening movie's sync depends on it). Channel 1 is the "RNA" (ADX renderer)
// command channel (0x55FC00): entries {u16 command, u16 0, u32 rna handle, u32 a, u32 b}, 4 = sample rate,
// 8 = play switch, 9 = pan ...
//
// Playback: the PCM of an RNA's two SJs is read from IOP RAM, interleaved and handed to the SPU2's HLE PCM stream
// (Spu2::streamPush), which resamples it to 48 kHz and mixes it into the host output; its consumption counter paces
// the returns. Volume, pan and the play switch are not modelled yet (data plays as soon as it arrives).

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr uint32_t kDtxSid = 0x90000200u;

        constexpr uint32_t kFnCreateChannel = 2u;  // send {index, ee buffer, iop buffer, size}, reply {channel handle}
        constexpr uint32_t kFnDestroyChannel = 3u; // send {channel handle}
        constexpr uint32_t kUrpcBase = 0x400u;
        constexpr uint32_t kUrpcCount = 0x40u; // the EE dispatch table of the IOP -> EE direction has the same size

        // Urpc numbers (function - 0x400), with the EE function that issues them.
        constexpr uint32_t kUrpcSjxCreate = 0x00u;     // SJX_Create 0x564738: {local SJ, remote SJ handle, id, EE slot}
        constexpr uint32_t kUrpcSjxDestroy = 0x01u;    // SJX_Destroy 0x564830: {sjx handle}
        constexpr uint32_t kUrpcRnaCreate = 0x08u;     // 0x560758: {channels, 0, remote SJ handles...} -> rna handle
        constexpr uint32_t kUrpcRnaDestroy = 0x09u;    // 0x560af8 / 0x560758: {rna handle}
        constexpr uint32_t kUrpcRnaQueryMode = 0x0Cu;  // 0x560298: {sample rate} -> output mode (0 on the real IRX)
        constexpr uint32_t kUrpcSjCreateRingBuf = 0x20u; // SJRBF_CreateRmt 0x563F38 -> handle
        constexpr uint32_t kUrpcSjCreateMemory = 0x21u;  // SJMEM_CreateRmt 0x563F90 -> handle
        constexpr uint32_t kUrpcSjCreateUniform = 0x22u; // SJUNI_CreateRmt 0x563FE0: {count, iop address, size} -> handle
        constexpr uint32_t kUrpcSjReset = 0x25u;         // SJRMT_Reset 0x5640D8: {sj handle}

        constexpr uint32_t kRnaCmdSampleRate = 4u; // RNA command entry {4, rna handle, sample rate, 0}
        constexpr uint32_t kDefaultSampleRate = 48000u;
        constexpr uint32_t kReturnGranularity = 0x40u; // bytes; the physical IRX returned 0x400-byte pieces
        constexpr uint32_t kMaxRnaChannels = 2u;

        constexpr uint32_t kBlockHeaderBytes = 0x40u;
        constexpr uint32_t kBlockEntryOffset = 0x10u;
        constexpr uint32_t kBlockEntryBytes = 0x10u;
        constexpr uint32_t kMaxChannels = 16u; // the EE keeps 16 DTX slots (0x7A1160, 0x44 bytes each)

        // The EE only treats handles as opaque non-zero tokens (0 is the failure value). They look like IOP heap
        // addresses, as those of the physical module do (0x3E090, 0x44450 ...).
        constexpr uint32_t kFirstHandle = 0x00044000u;
        constexpr uint32_t kHandleStep = 0x10u;

        // Spu2::StreamCount, mirrored here so this file does not depend on the emulator headers.
        constexpr uint32_t kPcmStreams = 4u;

        struct Channel
        {
            uint32_t handle = 0u;
            uint32_t index = 0u; // DTX_Create a0: 0 = SJX (stream joints), 1 = RNA (renderer commands)
            uint32_t eeBuffer = 0u;
            uint32_t iopBuffer = 0u;
            uint32_t size = 0u;
        };

        struct Chunk
        {
            uint32_t address = 0u; // IOP RAM
            uint32_t length = 0u;
        };

        // One stream joint as seen through SJX.
        struct SjxLink
        {
            uint32_t slotAddress = 0u; // EE-side SJX slot, echoed back in the "space returned" entries
            uint32_t id = 0u;          // the id the EE passes to GetChunk, 1 for the EE -> IOP direction
            uint32_t sjuni = 0u;       // remote stream joint this link feeds (SJUNI_CreateRmt handle)
            std::deque<Chunk> unplayed; // announced by the EE, not yet handed to the SPU2
            std::deque<Chunk> playing;  // handed to the SPU2, space not yet returned
            uint64_t pushedBytes = 0u;
            uint64_t returnedBytes = 0u;
        };

        // One ADX renderer: up to two remote stream joints (left, right) played through one PCM stream.
        struct Rna
        {
            uint32_t channels = 0u;
            std::array<uint32_t, kMaxRnaChannels> sjuni{};
            uint32_t sampleRate = kDefaultSampleRate;
            int stream = -1; // SPU2 PCM stream, -1 when none was free
        };

        class CriDtxService final : public IopService
        {
        public:
            explicit CriDtxService(IopHost &host)
                : m_host(host)
            {
            }

            [[nodiscard]] std::string_view name() const override
            {
                return "cri_dtx";
            }

            [[nodiscard]] std::span<const uint32_t> sids() const override
            {
                return kSids;
            }

            [[nodiscard]] std::span<const std::string_view> moduleAliases() const override
            {
                return kModuleAliases;
            }

            void attachPcmSink(PcmSink *sink) override
            {
                m_sink = sink;
            }

            void reset() override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_sink != nullptr)
                    for (uint32_t stream = 0u; stream < kPcmStreams; ++stream)
                        m_sink->reset(stream);
                m_channels.clear();
                m_sjx.clear();
                m_rna.clear();
                m_sjuniToLink.clear();
                m_nextHandle = kFirstHandle;
                m_rpcCount = 0u;
                m_blockCount = 0u;
                m_chunksReturned = 0u;
                m_framesQueued = 0u;
                m_unknownLogs = 0u;
            }

            [[nodiscard]] RpcResult handleRpc(const RpcRequest &request) override
            {
                RpcResult result;
                if (request.sid != kDtxSid)
                    return result;
                result.handled = true;
                result.resultAddress = request.receive.address;

                std::array<uint32_t, 16> send{};
                const size_t sendWords = std::min<size_t>(request.send.size / sizeof(uint32_t), send.size());
                if (sendWords != 0u && request.send.address != 0u)
                    (void)m_host.readGuest(request.send.address, send.data(), sendWords * sizeof(uint32_t));

                std::array<uint32_t, 16> reply{};
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    ++m_rpcCount;
                    dispatchLocked(request, send, sendWords, reply);
                }
                if (request.receive.address != 0u && request.receive.size != 0u)
                    (void)writeRpcWords(m_host, request.receive, reply);
                return result;
            }

            // The EE has just DMAed a block to the IOP side of a DTX channel: answer it by DMAing one back.
            void onSifTransfer(const SifTransfer &transfer) override
            {
                if (transfer.kind != SifTransferKind::SetDma || transfer.phase != SifTransferPhase::AfterCopy)
                    return;

                std::lock_guard<std::mutex> lock(m_mutex);
                const Channel *channel = nullptr;
                for (const auto &entry : m_channels)
                {
                    if (entry.second.iopBuffer == transfer.destinationAddress && entry.second.size == transfer.size)
                    {
                        channel = &entry.second;
                        break;
                    }
                }
                if (channel == nullptr)
                    return;

                std::vector<uint32_t> block(channel->size / sizeof(uint32_t), 0u);
                if (!m_host.readGuest(transfer.sourceAddress, block.data(), channel->size))
                    return;
                ++m_blockCount;

                const uint32_t maxEntries = (channel->size - kBlockHeaderBytes - kBlockEntryOffset) / kBlockEntryBytes;
                const uint32_t entryCount = std::min<uint32_t>(block[0], maxEntries);
                for (uint32_t i = 0u; i < entryCount; ++i)
                    parseEntryLocked(&block[(kBlockEntryOffset + i * kBlockEntryBytes) / sizeof(uint32_t)]);

                std::vector<uint32_t> answer(block.size(), 0u);
                uint32_t answered = 0u;
                // Only the SJX channel has a receive callback that understands "space returned" entries.
                if (channel->index == 0u)
                {
                    playPendingLocked();
                    answered = returnSpaceLocked(answer, maxEntries);
                }
                answer[0] = answered;
                // The EE waits until the counter in its buffer is greater than the one it sent (observed: sent N,
                // physical IRX answers N + 1).
                const uint32_t counterIndex = static_cast<uint32_t>(block.size()) - 1u;
                answer[counterIndex] = block[counterIndex] + 1u;
                (void)m_host.writeGuest(channel->eeBuffer, answer.data(), channel->size);
            }

            void appendDebugMetrics(std::vector<DebugMetric> &metrics) const override
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                metrics.push_back({"rpcs", m_rpcCount, false});
                metrics.push_back({"channels", m_channels.size(), false});
                metrics.push_back({"blocks", m_blockCount, false});
                metrics.push_back({"pcm_frames_queued", m_framesQueued, false});
                metrics.push_back({"chunks_returned", m_chunksReturned, false});
            }

        private:
            uint32_t allocateHandleLocked()
            {
                const uint32_t handle = m_nextHandle;
                m_nextHandle += kHandleStep;
                return handle;
            }

            SjxLink *linkForSjuniLocked(uint32_t sjuni)
            {
                const auto mapped = m_sjuniToLink.find(sjuni);
                if (mapped == m_sjuniToLink.end())
                    return nullptr;
                const auto link = m_sjx.find(mapped->second);
                return link == m_sjx.end() ? nullptr : &link->second;
            }

            Rna *rnaForSjuniLocked(uint32_t sjuni, uint32_t *channelIndex = nullptr)
            {
                for (auto &entry : m_rna)
                {
                    for (uint32_t c = 0u; c < entry.second.channels; ++c)
                    {
                        if (entry.second.sjuni[c] == sjuni)
                        {
                            if (channelIndex != nullptr)
                                *channelIndex = c;
                            return &entry.second;
                        }
                    }
                }
                return nullptr;
            }

            // One entry of a block the EE sent.
            void parseEntryLocked(const uint32_t *entry)
            {
                const auto link = m_sjx.find(entry[1]);
                if (link != m_sjx.end())
                {
                    // SJX entry: a chunk of PCM the EE has just DMAed into IOP RAM (id 1 = data).
                    if (link->second.id == 1u && ((entry[0] >> 8) & 0xFFu) == 1u && entry[3] != 0u)
                        link->second.unplayed.push_back({entry[2], entry[3]});
                    return;
                }
                const auto rna = m_rna.find(entry[1]);
                if (rna != m_rna.end())
                {
                    if ((entry[0] & 0xFFFFu) == kRnaCmdSampleRate && entry[2] >= 4000u && entry[2] <= 192000u)
                        rna->second.sampleRate = entry[2];
                    static const bool logCommands = std::getenv("PS2X_DTX_LOG") != nullptr;
                    if (logCommands)
                        std::fprintf(stderr, "[CRI_DTX] rna=%08x cmd=%u args=%08x,%08x\n", entry[1], entry[0] & 0xFFFFu,
                                     entry[2], entry[3]);
                }
            }

            // Moves PCM the EE has announced into the SPU2 stream of its renderer, left and right together.
            void playPendingLocked()
            {
                if (m_sink == nullptr)
                    return;
                for (auto &entry : m_rna)
                {
                    Rna &rna = entry.second;
                    if (rna.stream < 0 || rna.channels == 0u)
                        continue;
                    std::array<SjxLink *, kMaxRnaChannels> links{};
                    bool complete = true;
                    for (uint32_t c = 0u; c < rna.channels; ++c)
                    {
                        links[c] = linkForSjuniLocked(rna.sjuni[c]);
                        complete = complete && links[c] != nullptr;
                    }
                    if (!complete)
                        continue;
                    for (;;)
                    {
                        uint32_t bytes = UINT32_MAX;
                        for (uint32_t c = 0u; c < rna.channels; ++c)
                            bytes = std::min<uint32_t>(bytes, links[c]->unplayed.empty() ? 0u : links[c]->unplayed.front().length);
                        bytes &= ~1u; // whole 16-bit samples
                        if (bytes == 0u)
                            break;
                        const uint32_t frames = bytes / sizeof(int16_t);
                        std::vector<int16_t> channelPcm[kMaxRnaChannels];
                        bool readable = true;
                        for (uint32_t c = 0u; c < rna.channels; ++c)
                        {
                            channelPcm[c].resize(frames);
                            readable = readable && m_host.readIopMemory(links[c]->unplayed.front().address,
                                                                        channelPcm[c].data(), bytes);
                        }
                        std::vector<int16_t> stereo(static_cast<size_t>(frames) * 2u, 0);
                        if (readable)
                        {
                            for (uint32_t f = 0u; f < frames; ++f)
                            {
                                stereo[f * 2u] = channelPcm[0][f];
                                stereo[f * 2u + 1u] = rna.channels > 1u ? channelPcm[1][f] : channelPcm[0][f];
                            }
                        }
                        // Unreadable memory plays as silence of the same length so the pacing is unchanged.
                        m_sink->push(static_cast<uint32_t>(rna.stream), rna.sampleRate, stereo.data(), frames);
                        m_framesQueued += frames;
                        for (uint32_t c = 0u; c < rna.channels; ++c)
                        {
                            Chunk &front = links[c]->unplayed.front();
                            links[c]->playing.push_back({front.address, bytes});
                            links[c]->pushedBytes += bytes;
                            front.address += bytes;
                            front.length -= bytes;
                            if (front.length == 0u)
                                links[c]->unplayed.pop_front();
                        }
                    }
                }
            }

            // Writes the "space returned" entries for everything the SPU2 has played since the last block.
            uint32_t returnSpaceLocked(std::vector<uint32_t> &answer, uint32_t maxEntries)
            {
                uint32_t answered = 0u;
                const auto emit = [&](const SjxLink &link, uint32_t address, uint32_t length)
                {
                    const uint32_t out = (kBlockEntryOffset + answered * kBlockEntryBytes) / sizeof(uint32_t);
                    answer[out + 0u] = 0u; // status 0, id 0 = space returned
                    answer[out + 1u] = link.slotAddress;
                    answer[out + 2u] = address;
                    answer[out + 3u] = length;
                    ++answered;
                    ++m_chunksReturned;
                };
                for (auto &entry : m_sjx)
                {
                    SjxLink &link = entry.second;
                    if (link.id != 1u)
                        continue;
                    const Rna *rna = rnaForSjuniLocked(link.sjuni);
                    if (rna == nullptr || rna->stream < 0 || m_sink == nullptr)
                    {
                        // Nothing plays this joint (no renderer, or no free PCM stream): give the space back at once
                        // so the EE does not stall on it.
                        while (!link.unplayed.empty() && answered < maxEntries)
                        {
                            emit(link, link.unplayed.front().address, link.unplayed.front().length);
                            link.unplayed.pop_front();
                        }
                        continue;
                    }
                    const uint64_t playedBytes = m_sink->consumed(static_cast<uint32_t>(rna->stream)) * sizeof(int16_t);
                    while (!link.playing.empty() && answered < maxEntries && playedBytes > link.returnedBytes)
                    {
                        Chunk &front = link.playing.front();
                        uint32_t piece = static_cast<uint32_t>(std::min<uint64_t>(playedBytes - link.returnedBytes, front.length));
                        if (piece < front.length)
                            piece &= ~(kReturnGranularity - 1u);
                        if (piece == 0u)
                            break;
                        emit(link, front.address, piece);
                        link.returnedBytes += piece;
                        front.address += piece;
                        front.length -= piece;
                        if (front.length == 0u)
                            link.playing.pop_front();
                    }
                }
                return answered;
            }

            void resetRnaLocked(Rna &rna)
            {
                if (rna.stream >= 0 && m_sink != nullptr)
                    m_sink->reset(static_cast<uint32_t>(rna.stream));
                for (uint32_t c = 0u; c < rna.channels; ++c)
                {
                    if (SjxLink *link = linkForSjuniLocked(rna.sjuni[c]))
                    {
                        link->unplayed.clear();
                        link->playing.clear();
                        link->pushedBytes = 0u;
                        link->returnedBytes = 0u;
                    }
                }
            }

            void dispatchLocked(const RpcRequest &request,
                                const std::array<uint32_t, 16> &send,
                                size_t sendWords,
                                std::array<uint32_t, 16> &reply)
            {
                if (request.function == kFnCreateChannel)
                {
                    // {index, ee buffer, iop buffer, size}. The reply is the IOP-side object, 0 means failure.
                    if (sendWords < 4u || send[0] >= kMaxChannels || send[3] < kBlockHeaderBytes * 2u)
                    {
                        logUnknownLocked(request, "bad channel create");
                        return;
                    }
                    Channel channel;
                    channel.handle = allocateHandleLocked();
                    channel.index = send[0];
                    channel.eeBuffer = send[1];
                    channel.iopBuffer = send[2];
                    channel.size = send[3] & ~3u;
                    m_channels[channel.handle] = channel;
                    reply[0] = channel.handle;
                    return;
                }
                if (request.function == kFnDestroyChannel)
                {
                    if (sendWords >= 1u)
                        m_channels.erase(send[0]);
                    return;
                }
                if (request.function < kUrpcBase || request.function >= kUrpcBase + kUrpcCount)
                {
                    logUnknownLocked(request, "unknown function");
                    return;
                }

                switch (request.function - kUrpcBase)
                {
                case kUrpcSjxCreate:
                    // The EE slot address (send[3]) and id (send[2]) are what the "space returned" entries need.
                    reply[0] = allocateHandleLocked();
                    if (sendWords >= 4u)
                    {
                        SjxLink link;
                        link.slotAddress = send[3];
                        link.id = send[2];
                        link.sjuni = send[1];
                        m_sjx[reply[0]] = std::move(link);
                        m_sjuniToLink[send[1]] = reply[0];
                    }
                    break;
                case kUrpcSjxDestroy:
                    if (sendWords >= 1u)
                    {
                        const auto link = m_sjx.find(send[0]);
                        if (link != m_sjx.end())
                        {
                            m_sjuniToLink.erase(link->second.sjuni);
                            m_sjx.erase(link);
                        }
                    }
                    break;
                case kUrpcRnaCreate:
                {
                    // {channel count, 0, remote SJ handle per channel...}: this renderer plays those joints.
                    reply[0] = allocateHandleLocked();
                    Rna rna;
                    rna.channels = sendWords >= 1u ? std::min<uint32_t>(send[0], kMaxRnaChannels) : 0u;
                    for (uint32_t c = 0u; c < rna.channels && 2u + c < sendWords; ++c)
                        rna.sjuni[c] = send[2u + c];
                    std::array<bool, kPcmStreams> used{};
                    for (const auto &other : m_rna)
                        if (other.second.stream >= 0)
                            used[static_cast<size_t>(other.second.stream)] = true;
                    for (uint32_t stream = 0u; stream < kPcmStreams; ++stream)
                    {
                        if (!used[stream])
                        {
                            rna.stream = static_cast<int>(stream);
                            break;
                        }
                    }
                    m_rna[reply[0]] = rna;
                    break;
                }
                case kUrpcRnaDestroy:
                    if (sendWords >= 1u)
                    {
                        const auto rna = m_rna.find(send[0]);
                        if (rna != m_rna.end())
                        {
                            resetRnaLocked(rna->second);
                            m_rna.erase(rna);
                        }
                    }
                    break;
                case kUrpcSjReset:
                    // The EE emptied a joint (ADXRNA stop): whatever was queued for its renderer is stale.
                    if (sendWords >= 1u)
                        if (Rna *rna = rnaForSjuniLocked(send[0]))
                            resetRnaLocked(*rna);
                    break;
                case kUrpcSjCreateRingBuf:
                case kUrpcSjCreateMemory:
                case kUrpcSjCreateUniform:
                    reply[0] = allocateHandleLocked();
                    break;
                case kUrpcRnaQueryMode:
                    reply[0] = 0u; // the physical IRX answered 0 (SPU2 core 0, "SPU CORE Number = 0" in its banner)
                    break;
                default:
                    // The remaining urpcs are one-way setters/destructors (3, 0xA, 0xB, 0xD-0x11, 0x23, 0x27-0x29)
                    // or queries the boot never makes; a zero reply is a valid "nothing/false" for every one.
                    break;
                }
            }

            void logUnknownLocked(const RpcRequest &request, const char *what)
            {
                if (m_unknownLogs >= 16u)
                    return;
                ++m_unknownLogs;
                std::ostringstream message;
                message << "[CRI_DTX:HLE] " << what << " fn=0x" << std::hex << request.function << " sendSize=0x"
                        << request.send.size << " recvSize=0x" << request.receive.size;
                m_host.log(LogLevel::Warning, message.str());
            }

            inline static constexpr std::array<uint32_t, 1> kSids{kDtxSid};
            inline static constexpr std::array<std::string_view, 1> kModuleAliases{"cri_adxi"};

            IopHost &m_host;
            PcmSink *m_sink = nullptr;
            mutable std::mutex m_mutex;
            std::unordered_map<uint32_t, Channel> m_channels;
            std::unordered_map<uint32_t, SjxLink> m_sjx;
            std::unordered_map<uint32_t, uint32_t> m_sjuniToLink; // remote SJ handle -> sjx handle
            std::unordered_map<uint32_t, Rna> m_rna;
            uint32_t m_nextHandle = kFirstHandle;
            uint64_t m_rpcCount = 0u;
            uint64_t m_blockCount = 0u;
            uint64_t m_chunksReturned = 0u;
            uint64_t m_framesQueued = 0u;
            uint32_t m_unknownLogs = 0u;
        };
    }

    std::unique_ptr<IopService> createCriDtxService(IopHost &host)
    {
        return std::make_unique<CriDtxService>(host);
    }
}
