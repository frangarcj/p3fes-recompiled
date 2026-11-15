#include "spu2.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ps2x::iop::detail
{
    namespace
    {
        constexpr int32_t kAdpcmFilters[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        constexpr size_t kAdmaBlockBytes = 1024u; // one ADMA block: 256 L + 256 R samples

        int16_t clamp16(int32_t value) noexcept
        {
            return static_cast<int16_t>(std::clamp(value, -32768, 32767));
        }

        // Voice or core volume. Fixed mode (bit 15 = 0): signed 15 bits. Sweeps (bit 15 = 1) are not emulated yet:
        // their target applies at once (maximum when rising, 0 when falling).
        int32_t volumeFromRegister(uint16_t value) noexcept
        {
            if ((value & 0x8000u) == 0u)
                return static_cast<int16_t>(value << 1u) >> 1; // -0x4000..0x3FFF
            return (value & 0x2000u) != 0u ? 0 : 0x3FFF;
        }
    }

    namespace
    {
        // PS2X_SPU2_DIAG counters, per core, since the last diag line: voices keyed on, voice samples produced, and
        // voice samples dropped because the voice's VMIX bit is off on both sides.
        uint32_t g_diagKeyOns[2] = {};
        uint32_t g_diagVoiceSamples[2] = {};
        uint32_t g_diagUnmixedSamples[2] = {};
        uint32_t g_diagWrites[2] = {};      // register writes
        uint32_t g_diagVoiceWrites[2] = {}; // writes to the per-voice parameter / address blocks
        uint64_t g_diagSamples = 0u;        // samples stepped, across all instances (the once-per-second dump)
        uint32_t g_diagUnderrunsReported = 0u;

        // PS2X_SPU2_DIAG is read once; hot paths take the answer as a template parameter or a local copy.
        bool diagEnabled() noexcept
        {
            static const bool enabled = std::getenv("PS2X_SPU2_DIAG") != nullptr;
            return enabled;
        }
    }

    Spu2::Spu2() : m_ram(RamHalfwords), m_outRing(OutputRingFrames * 2u)
    {
        const char *batch = std::getenv("PS2X_SPU2_BATCH");
        m_batch = batch != nullptr && batch[0] == '1';
        if (m_batch)
            m_stage.resize(StageFrames * 2u);
        reset();
    }

    void Spu2::reset()
    {
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        m_regs.fill(0);
        for (auto &core : m_voices)
            for (auto &voice : core)
                voice = VoiceState{};
        m_endx.fill(0);
        m_activeVoices.fill(0u);
        {
            std::lock_guard<std::mutex> lock(m_outputMutex);
            m_silentPending.store(0u, std::memory_order_relaxed);
            m_outRead = 0u;
            m_outCount = 0u;
            m_stageCount = 0u;
        }
        for (auto &input : m_input)
            input = InputState{};
        {
            std::lock_guard<std::mutex> lock(m_streamMutex);
            for (auto &stream : m_streams)
                stream = PcmStream{};
            m_streamsActive.store(false, std::memory_order_relaxed);
            m_streamLive.store(false, std::memory_order_relaxed);
        }
        m_lastCycle = 0;
        m_interrupt = false;
        reg(RegStatx) = StatxDmaReady;
        reg(0x400u + RegStatx) = StatxDmaReady;
    }

    uint32_t Spu2::reg32(uint32_t offset) const noexcept
    {
        return ((static_cast<uint32_t>(reg(offset)) & 0xFu) << 16u) | reg(offset + 2u);
    }

    void Spu2::setReg32(uint32_t offset, uint32_t value) noexcept
    {
        value &= RamHalfwords - 1u;
        reg(offset) = static_cast<uint16_t>(value >> 16u);
        reg(offset + 2u) = static_cast<uint16_t>(value);
    }

    void Spu2::writeRam(uint32_t halfwordAddress, uint16_t value) noexcept
    {
        halfwordAddress &= RamHalfwords - 1u;
        m_ram[halfwordAddress] = value;
        checkIrqAddress(halfwordAddress);
    }

    void Spu2::checkIrqAddress(uint32_t halfwordAddress) noexcept
    {
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t base = core * 0x400u;
            if ((reg(base + RegAttr) & 0x40u) != 0u && reg32(base + RegIrqa) == halfwordAddress)
            {
                m_interrupt = true;
                reg(0x7C2u) |= static_cast<uint16_t>(4u << core); // SPDIF_IRQINFO
            }
        }
    }

    bool Spu2::takeInterrupt() noexcept
    {
        const bool pending = m_interrupt;
        m_interrupt = false;
        return pending;
    }

    uint16_t Spu2::read16(uint32_t physical) const
    {
        const uint32_t offset = (physical - RegBase) & 0x7FEu;
        if (offset < 0x760u)
        {
            const uint32_t core = offset >= 0x400u ? 1u : 0u;
            const uint32_t local = offset & 0x3FFu;
            if (local < 0x180u)
            {
                const uint32_t index = local >> 4u;
                const uint32_t field = local & 0xFu;
                if (index < VoicesPerCore)
                {
                    const VoiceState &v = m_voices[core][index];
                    if (field == 0xAu)
                        return static_cast<uint16_t>(v.envelope);
                    if (field == 0xCu || field == 0xEu)
                        return static_cast<uint16_t>(volumeFromRegister(reg(voiceReg(core, index, field - 0xCu))) << 1u);
                }
            }
            else if (local >= RegVoiceAddr && local < RegVoiceAddr + VoicesPerCore * 0xCu)
            {
                const uint32_t index = (local - RegVoiceAddr) / 0xCu;
                const uint32_t field = (local - RegVoiceAddr) % 0xCu;
                if (field == 8u)
                    return static_cast<uint16_t>(m_voices[core][index].nax >> 16u);
                if (field == 10u)
                    return static_cast<uint16_t>(m_voices[core][index].nax);
            }
            else if (local == RegEndx)
                return static_cast<uint16_t>(m_endx[core]);
            else if (local == RegEndx + 2u)
                return static_cast<uint16_t>(m_endx[core] >> 16u);
        }
        return reg(offset);
    }

    void Spu2::write16(uint32_t physical, uint16_t value)
    {
        const uint32_t offset = (physical - RegBase) & 0x7FEu;
        reg(offset) = value;
        if (offset < 0x760u && diagEnabled())
        {
            const uint32_t c = offset >= 0x400u ? 1u : 0u;
            ++g_diagWrites[c];
            const uint32_t l = offset & 0x3FFu;
            if (l < 0x180u || (l >= 0x1C0u && l < 0x2E0u))
                ++g_diagVoiceWrites[c];
        }
        if (offset >= 0x760u)
            return;

        const uint32_t core = offset >= 0x400u ? 1u : 0u;
        const uint32_t base = core * 0x400u;
        const uint32_t local = offset & 0x3FFu;
        switch (local)
        {
        case RegKon:
            keyOn(core, value, 0u);
            return;
        case RegKon + 2u:
            keyOn(core, value & 0xFFu, 16u);
            return;
        case RegKoff:
            keyOff(core, value, 0u);
            return;
        case RegKoff + 2u:
            keyOff(core, value & 0xFFu, 16u);
            return;
        case RegData:
        {
            uint32_t tsa = reg32(base + RegTsa);
            writeRam(tsa, value);
            setReg32(base + RegTsa, tsa + 1u);
            return;
        }
        case RegAdmas:
            if ((value & (core + 1u)) == 0u)
                m_input[core] = InputState{};
            return;
        case RegEndx:
            m_endx[core] &= 0xFF0000u;
            return;
        case RegEndx + 2u:
            m_endx[core] &= 0x00FFFFu;
            return;
        default:
            break;
        }

        if (local >= RegVoiceAddr && local < RegVoiceAddr + VoicesPerCore * 0xCu)
        {
            const uint32_t index = (local - RegVoiceAddr) / 0xCu;
            const uint32_t field = (local - RegVoiceAddr) % 0xCu;
            VoiceState &v = m_voices[core][index];
            if (field == 4u || field == 6u) // LSAX written by the driver: overrides the block's loop flag
            {
                v.lsa = reg32(voiceAddrReg(core, index, 4u));
                v.lsaFromRegister = true;
            }
            else if (field == 8u || field == 10u)
            {
                v.nax = reg32(voiceAddrReg(core, index, 8u)) & ~7u;
            }
        }
    }

    bool Spu2::dmaWrite(uint32_t core, const uint8_t *source, size_t bytes)
    {
        const uint32_t base = core * 0x400u;
        if ((reg(base + RegAdmas) & (core + 1u)) != 0u)
        {
            // P3FES: AutoDMA. Needs proper testing beyond CRI ADX streams.
            InputState &input = m_input[core];
            if (input.queueRead != 0u && input.queueRead == input.queue.size())
            {
                input.queue.clear();
                input.queueRead = 0u;
            }
            input.queue.insert(input.queue.end(), source, source + bytes);
            input.bytesQueued += bytes;
            input.transferEnds.push_back(input.bytesQueued);
            feedInput(core);
            return true;
        }
        uint32_t tsa = reg32(base + RegTsa);
        for (size_t i = 0; i + 1u < bytes; i += 2u)
            writeRam(tsa++, static_cast<uint16_t>(source[i] | (source[i + 1u] << 8u)));
        setReg32(base + RegTsa, tsa);
        return true;
    }

    bool Spu2::dmaRead(uint32_t core, uint8_t *destination, size_t bytes)
    {
        const uint32_t base = core * 0x400u;
        if ((reg(base + RegAdmas) & (core + 1u)) != 0u)
            return false;
        uint32_t tsa = reg32(base + RegTsa);
        for (size_t i = 0; i + 1u < bytes; i += 2u)
        {
            const uint16_t value = ram(tsa++);
            destination[i] = static_cast<uint8_t>(value);
            destination[i + 1u] = static_cast<uint8_t>(value >> 8u);
        }
        setReg32(base + RegTsa, tsa);
        return true;
    }

    bool Spu2::takeAdmaDone(uint32_t core) noexcept
    {
        InputState &input = m_input[core & 1u];
        if (input.doneCount == 0u)
            return false;
        --input.doneCount;
        return true;
    }

    void Spu2::markDmaReady(uint32_t core) noexcept
    {
        reg(core * 0x400u + RegStatx) |= StatxDmaReady;
    }


    void Spu2::keyOn(uint32_t core, uint32_t mask, uint32_t firstVoice)
    {
        if (core < 2u && diagEnabled())
            g_diagKeyOns[core] += static_cast<uint32_t>(__builtin_popcount(mask));
        for (uint32_t bit = 0; bit < 16u; ++bit)
        {
            if ((mask & (1u << bit)) == 0u || firstVoice + bit >= VoicesPerCore)
                continue;
            const uint32_t index = firstVoice + bit;
            VoiceState &v = m_voices[core][index];
            const uint32_t ssa = reg32(voiceAddrReg(core, index, 0u)) & ~7u;
            v = VoiceState{};
            v.active = true;
            v.nax = ssa;
            v.lsa = ssa;
            v.phase = VoiceState::Phase::Attack;
            m_activeVoices[core] |= 1u << index;
            m_endx[core] &= ~(1u << index);
        }
    }

    void Spu2::keyOff(uint32_t core, uint32_t mask, uint32_t firstVoice)
    {
        for (uint32_t bit = 0; bit < 16u; ++bit)
        {
            if ((mask & (1u << bit)) == 0u || firstVoice + bit >= VoicesPerCore)
                continue;
            VoiceState &v = m_voices[core][firstVoice + bit];
            if (v.active)
            {
                v.phase = VoiceState::Phase::Release;
                v.envelopeCounter = 0;
            }
        }
    }

    void Spu2::decodeBlock(uint32_t core, uint32_t index)
    {
        VoiceState &v = m_voices[core][index];

        // End of the previous block: with the end flag, jump to LSA; without "repeat" the voice stops.
        if ((v.blockFlags & 1u) != 0u)
        {
            m_endx[core] |= 1u << index;
            v.nax = v.lsa;
            if ((v.blockFlags & 2u) == 0u)
            {
                v.active = false;
                v.phase = VoiceState::Phase::Off;
                v.envelope = 0;
            }
        }
        v.blockFlags = 0;
        if (!v.active)
            return;

        const uint32_t block = v.nax & (RamHalfwords - 1u);
        const uint16_t header = ram(block);
        uint32_t shift = header & 0xFu;
        if (shift > 12u)
            shift = 9u;
        const uint32_t filter = std::min<uint32_t>((header >> 4u) & 7u, 4u);
        v.blockFlags = static_cast<uint8_t>(header >> 8u);
        if ((v.blockFlags & 4u) != 0u && !v.lsaFromRegister)
            v.lsa = block;

        // ATTR cannot change while a block is decoded, so look at the IRQ enable bits once instead of once per word.
        const bool irqArmed = ((reg(RegAttr) | reg(0x400u + RegAttr)) & 0x40u) != 0u;
        for (uint32_t word = 1; word < 8u; ++word)
        {
            const uint16_t packed = ram(block + word);
            if (irqArmed)
                checkIrqAddress((block + word) & (RamHalfwords - 1u));
            for (uint32_t nibble = 0; nibble < 4u; ++nibble)
            {
                const int32_t raw = static_cast<int16_t>(((packed >> (nibble * 4u)) & 0xFu) << 12u) >> shift;
                const int32_t predicted = (v.hist1 * kAdpcmFilters[filter][0] + v.hist2 * kAdpcmFilters[filter][1] + 32) >> 6;
                const int16_t sample = clamp16(raw + predicted);
                v.hist2 = v.hist1;
                v.hist1 = sample;
                v.decoded[(word - 1u) * 4u + nibble] = sample;
            }
        }
        if (irqArmed)
            checkIrqAddress(block);
        v.decodedIndex = 0;
        v.nax = (block + 8u) & (RamHalfwords - 1u);
    }

    void Spu2::stepEnvelope(uint32_t core, uint32_t index)
    {
        VoiceState &v = m_voices[core][index];
        const uint16_t adsr1 = reg(voiceReg(core, index, 6u));
        const uint16_t adsr2 = reg(voiceReg(core, index, 8u));

        uint32_t rate = 0;
        bool exponential = false;
        bool increase = false;
        switch (v.phase)
        {
        case VoiceState::Phase::Attack:
            rate = (adsr1 >> 8u) & 0x7Fu;
            exponential = (adsr1 & 0x8000u) != 0u;
            increase = true;
            break;
        case VoiceState::Phase::Decay:
            rate = ((adsr1 >> 4u) & 0xFu) * 4u;
            exponential = true;
            break;
        case VoiceState::Phase::Sustain:
            rate = (adsr2 >> 6u) & 0x7Fu;
            exponential = (adsr2 & 0x8000u) != 0u;
            increase = (adsr2 & 0x4000u) == 0u;
            break;
        case VoiceState::Phase::Release:
            rate = (adsr2 & 0x1Fu) * 4u;
            exponential = (adsr2 & 0x20u) != 0u;
            break;
        case VoiceState::Phase::Off:
            return;
        }

        // psx-spx "SPU ADSR": ciclos = 1 << max(0, shift - 11); paso = valor << max(0, 11 - shift).
        const int32_t shiftValue = static_cast<int32_t>(rate >> 2u);
        const int32_t stepValue = static_cast<int32_t>(rate & 3u);
        int32_t step = increase ? 7 - stepValue : -8 + stepValue;
        uint32_t cycles = 1u << static_cast<uint32_t>(std::max(0, shiftValue - 11));
        step <<= std::max(0, 11 - shiftValue);
        if (exponential && increase && v.envelope > 0x6000)
            cycles *= 4u;
        if (exponential && !increase)
            step = step * v.envelope / 0x8000;

        if (++v.envelopeCounter >= cycles)
        {
            v.envelopeCounter = 0;
            v.envelope = std::clamp(v.envelope + step, 0, 0x7FFF);
        }

        switch (v.phase)
        {
        case VoiceState::Phase::Attack:
            if (v.envelope >= 0x7FFF)
            {
                v.phase = VoiceState::Phase::Decay;
                v.envelopeCounter = 0;
            }
            break;
        case VoiceState::Phase::Decay:
            if (v.envelope <= static_cast<int32_t>(((adsr1 & 0xFu) + 1u) * 0x800u))
            {
                v.phase = VoiceState::Phase::Sustain;
                v.envelopeCounter = 0;
            }
            break;
        case VoiceState::Phase::Release:
            if (v.envelope <= 0)
            {
                v.envelope = 0;
                v.phase = VoiceState::Phase::Off;
                v.active = false;
            }
            break;
        default:
            break;
        }
    }

    int32_t Spu2::stepVoice(uint32_t core, uint32_t index)
    {
        VoiceState &v = m_voices[core][index];
        if (!v.active)
            return 0;

        // Pitch is 4.12 (0x1000 = 48 kHz). Linear interpolation between the last two samples (the hardware uses a
        // 4-point Gaussian).
        v.pitchCounter += std::min<uint32_t>(reg(voiceReg(core, index, 4u)), 0x3FFFu);
        while (v.pitchCounter >= 0x1000u && v.active)
        {
            v.pitchCounter -= 0x1000u;
            if (v.decodedIndex >= 28u)
                decodeBlock(core, index);
            if (!v.active)
                break;
            v.prev[1] = v.prev[0];
            v.prev[0] = v.decoded[v.decodedIndex++];
        }
        if (!v.active)
            return 0;

        const int32_t fraction = static_cast<int32_t>(v.pitchCounter & 0xFFFu);
        const int32_t sample = v.prev[1] + (((v.prev[0] - v.prev[1]) * fraction) >> 12);
        stepEnvelope(core, index);
        v.lastOutput = clamp16((sample * v.envelope) >> 15);
        return v.lastOutput;
    }

    // Moves whole 512-byte ADMA blocks (256 L samples, then 256 R samples) into the input ring.
    void Spu2::feedInput(uint32_t core)
    {
        InputState &input = m_input[core];
        constexpr size_t kBlockBytes = kAdmaBlockBytes;
        while (input.queue.size() - input.queueRead >= kBlockBytes && input.ringCount + 256u <= InputFrames)
        {
            const uint8_t *block = input.queue.data() + input.queueRead;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                const uint32_t slot = (input.ringRead + input.ringCount + i) % InputFrames;
                input.ring[slot][0] = static_cast<int16_t>(block[i * 2u] | (block[i * 2u + 1u] << 8u));
                input.ring[slot][1] = static_cast<int16_t>(block[512u + i * 2u] | (block[512u + i * 2u + 1u] << 8u));
            }
            input.ringCount += 256u;
            input.queueRead += kBlockBytes;
            input.bytesConsumed += kBlockBytes;
        }
        // Transfers whose last byte is now in the ring are complete (a trailing partial block counts once the
        // rest of the queue is consumed).
        while (!input.transferEnds.empty())
        {
            const uint64_t end = input.transferEnds.front();
            const bool inRing = end <= input.bytesConsumed;
            const bool tailPartial = end == input.bytesQueued && input.bytesQueued - input.bytesConsumed < kBlockBytes;
            if (!inRing && !tailPartial)
                break;
            input.transferEnds.erase(input.transferEnds.begin());
            ++input.doneCount;
        }
        if (input.queueRead == input.queue.size() && input.queueRead != 0u)
        {
            input.queue.clear();
            input.queueRead = 0u;
        }
    }

    bool Spu2::inputIdle(uint32_t core) const noexcept
    {
        const InputState &input = m_input[core];
        // Exactly the states in which feedInput() changes nothing and no ring frame is consumed: an empty ring, no
        // pending transfer completions, less than a block queued and no fully-read queue left to clear.
        return input.ringCount == 0u && input.transferEnds.empty() &&
               input.queue.size() - input.queueRead < kAdmaBlockBytes &&
               !(input.queueRead != 0u && input.queueRead == input.queue.size());
    }

    // Debug aid: PS2X_SPU2_DIAG=1 prints the mixer-relevant registers once per emulated second.
    void Spu2::diagTick()
    {
        if ((++g_diagSamples % 48000u) != 0u)
            return;
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t base = core * 0x400u;
            const uint32_t m = RegMvolBase + core * RegMvolStride;
            std::fprintf(stderr,
                         "[spu2-diag] t=%llus core%u admas=%04X attr=%04X mmix=%04X mvol=%04X/%04X bvol=%04X/%04X "
                         "avol=%04X/%04X ring=%u queue=%zu kon=%u voiceSamples=%u unmixed=%u writes=%u voiceWrites=%u endx=%06X active=%06X\n",
                         static_cast<unsigned long long>(g_diagSamples / 48000u), core, reg(base + RegAdmas),
                         reg(base + RegAttr), reg(base + RegMmix), reg(m), reg(m + 2u), reg(m + 0x0Cu), reg(m + 0x0Eu),
                         reg(m + 0x08u), reg(m + 0x0Au), m_input[core].ringCount,
                         m_input[core].queue.size() - m_input[core].queueRead, g_diagKeyOns[core],
                         g_diagVoiceSamples[core], g_diagUnmixedSamples[core], g_diagWrites[core], g_diagVoiceWrites[core],
                         m_endx[core], [&] { uint32_t a = 0; for (uint32_t i = 0; i < VoicesPerCore; ++i) a |= m_voices[core][i].active ? 1u << i : 0u; return a; }());
            g_diagKeyOns[core] = g_diagVoiceSamples[core] = g_diagUnmixedSamples[core] = 0u;
            g_diagWrites[core] = g_diagVoiceWrites[core] = 0u;
        }
    }

    void Spu2::diagUnderrun(uint32_t core)
    {
        const InputState &input = m_input[core];
        if (input.ringCount == 0u && admaActive(core) && input.bytesQueued != 0u && g_diagUnderrunsReported < 40u)
        {
            ++g_diagUnderrunsReported;
            std::fprintf(stderr, "[spu2-diag] core%u ADMA underrun at cycle %llu: queued=%llu consumed=%llu pending=%zu done=%u\n",
                         core, static_cast<unsigned long long>(m_lastCycle), static_cast<unsigned long long>(input.bytesQueued),
                         static_cast<unsigned long long>(input.bytesConsumed), input.transferEnds.size(), input.doneCount);
        }
    }

    template <bool Diag>
    void Spu2::renderFrames(size_t count, int16_t *out, bool streamsLive)
    {
        // Registers cannot change while samples are rendered (the only register the mixer writes is SPDIF_IRQINFO),
        // so everything read from them every sample in the original loop is read once per call here.
        struct CoreSetup
        {
            uint32_t mixLeft, mixRight;
            int32_t bvolLeft, bvolRight; // BVOL is a plain signed 16-bit volume (0x7FFF = unity), unlike the sweep-format MVOL
            int32_t masterLeft, masterRight;
            bool inputLive;              // ADMA input has frames or work; otherwise the section below is a no-op
        } setup[2];
        for (uint32_t core = 0; core < 2u; ++core)
        {
            const uint32_t base = core * 0x400u;
            const uint32_t mvol = RegMvolBase + core * RegMvolStride;
            CoreSetup &c = setup[core];
            c.mixLeft = reg(base + RegVmixL) | (static_cast<uint32_t>(reg(base + RegVmixL + 2u)) << 16u);
            c.mixRight = reg(base + RegVmixR) | (static_cast<uint32_t>(reg(base + RegVmixR + 2u)) << 16u);
            c.bvolLeft = static_cast<int16_t>(reg(mvol + 0x0Cu));
            c.bvolRight = static_cast<int16_t>(reg(mvol + 0x0Eu));
            c.masterLeft = volumeFromRegister(reg(mvol));
            c.masterRight = volumeFromRegister(reg(mvol + 2u));
            // Idle input stays idle for the whole call: only dmaWrite()/write16() queue data. The diag underrun
            // report looks at idle input too, so diagnostics always run the section.
            c.inputLive = Diag || m_input[core].ringCount != 0u || !inputIdle(core);
        }

        for (size_t frame = 0; frame < count; ++frame)
        {
            if constexpr (Diag)
                diagTick();
            int32_t coreOut[2][2] = {};
            for (uint32_t core = 0; core < 2u; ++core)
            {
                const CoreSetup &c = setup[core];
                int32_t left = 0;
                int32_t right = 0;
                uint32_t pending = m_activeVoices[core];
                while (pending != 0u)
                {
                    const uint32_t index = static_cast<uint32_t>(__builtin_ctz(pending));
                    pending &= pending - 1u;
                    const int32_t sample = stepVoice(core, index);
                    if (!m_voices[core][index].active)
                        m_activeVoices[core] &= ~(1u << index); // stepVoice() is the only thing that ends a voice
                    if (sample == 0)
                        continue;
                    if constexpr (Diag)
                    {
                        ++g_diagVoiceSamples[core];
                        if ((c.mixLeft & (1u << index)) == 0u && (c.mixRight & (1u << index)) == 0u)
                            ++g_diagUnmixedSamples[core];
                    }
                    const int32_t volLeft = volumeFromRegister(reg(voiceReg(core, index, 0u)));
                    const int32_t volRight = volumeFromRegister(reg(voiceReg(core, index, 2u)));
                    if ((c.mixLeft & (1u << index)) != 0u)
                        left += (sample * volLeft) >> 14;
                    if ((c.mixRight & (1u << index)) != 0u)
                        right += (sample * volRight) >> 14;
                }
                // ADMA sound-data input, scaled by the core's BVOL (master block +0x0C/+0x0E).
                if (c.inputLive)
                {
                    InputState &input = m_input[core];
                    if (input.ringCount == 0u)
                        feedInput(core);
                    if constexpr (Diag)
                        diagUnderrun(core);
                    if (input.ringCount != 0u)
                    {
                        const auto &in = input.ring[input.ringRead];
                        left += (in[0] * c.bvolLeft) >> 15;
                        right += (in[1] * c.bvolRight) >> 15;
                        input.ringRead = (input.ringRead + 1u) % InputFrames;
                        --input.ringCount;
                    }
                }
                coreOut[core][0] = left;
                coreOut[core][1] = right;
            }

            // Core 0 output through its master volume, added to core 1's dry mix, all through core 1's master volume
            // (a simplification of the MMIX routing).
            int32_t streamOut[2] = {};
            if (streamsLive)
                mixStreams(streamOut[0], streamOut[1]);
            const int32_t core0Left = (coreOut[0][0] * setup[0].masterLeft) >> 14;
            const int32_t core0Right = (coreOut[0][1] * setup[0].masterRight) >> 14;
            out[frame * 2u] = clamp16((((core0Left + coreOut[1][0]) * setup[1].masterLeft) >> 14) + streamOut[0]);
            out[frame * 2u + 1u] = clamp16((((core0Right + coreOut[1][1]) * setup[1].masterRight) >> 14) + streamOut[1]);
        }
    }

    bool Spu2::anyStreamLive() const noexcept
    {
        for (const PcmStream &s : m_streams)
            if (s.read < s.samples.size() / 2u)
                return true; // not in underrun: mixStreams() would move it
        return false;
    }

    void Spu2::pushOutput(const int16_t *stereo, size_t frames)
    {
        if (m_batch)
        {
            if (stereo == nullptr)
            {
                // Silence goes behind the frames staged so far.
                if (m_stageCount != 0u)
                    flushOutput();
                m_silentPending.fetch_add(frames, std::memory_order_relaxed);
                return;
            }
            while (frames != 0u)
            {
                const size_t take = std::min(frames, StageFrames - m_stageCount);
                std::copy_n(stereo, take * 2u, m_stage.begin() + static_cast<std::ptrdiff_t>(m_stageCount * 2u));
                m_stageCount += take;
                stereo += take * 2u;
                frames -= take;
                if (m_stageCount == StageFrames)
                    flushOutput();
            }
            return;
        }
        if (stereo == nullptr)
        {
            m_silentPending.fetch_add(frames, std::memory_order_relaxed);
            return;
        }
        std::lock_guard<std::mutex> lock(m_outputMutex);
        flushSilenceLocked();
        pushOutputLocked(stereo, frames);
    }

    void Spu2::flushOutput()
    {
        if (m_stageCount == 0u)
            return;
        std::lock_guard<std::mutex> lock(m_outputMutex);
        flushSilenceLocked();
        pushOutputLocked(m_stage.data(), m_stageCount);
        m_stageCount = 0u;
    }

    void Spu2::flushSilenceLocked()
    {
        const uint64_t pending = m_silentPending.exchange(0u, std::memory_order_relaxed);
        if (pending != 0u)
            pushOutputLocked(nullptr, static_cast<size_t>(std::min<uint64_t>(pending, SIZE_MAX)));
    }

    void Spu2::pushOutputLocked(const int16_t *stereo, size_t frames)
    {
        while (frames != 0u)
        {
            // The original pushed one frame at a time and, as soon as more than MaxBufferedFrames were buffered,
            // dropped the oldest half; so a run is cut at the frame that triggers the drop.
            const size_t untilDrop = MaxBufferedFrames + 1u - m_outCount;
            const size_t take = std::min(frames, untilDrop);
            size_t write = m_outRead + m_outCount;
            if (write >= OutputRingFrames)
                write -= OutputRingFrames;
            const size_t first = std::min(take, OutputRingFrames - write);
            if (stereo != nullptr)
            {
                std::copy_n(stereo, first * 2u, m_outRing.begin() + static_cast<std::ptrdiff_t>(write * 2u));
                std::copy_n(stereo + first * 2u, (take - first) * 2u, m_outRing.begin());
                stereo += take * 2u;
            }
            else
            {
                std::fill_n(m_outRing.begin() + static_cast<std::ptrdiff_t>(write * 2u), first * 2u, int16_t{0});
                std::fill_n(m_outRing.begin(), (take - first) * 2u, int16_t{0});
            }
            m_outCount += take;
            frames -= take;
            if (m_outCount > MaxBufferedFrames)
            {
                const size_t drop = m_outCount / 2u;
                m_outRead = (m_outRead + drop) % OutputRingFrames;
                m_outCount -= drop;
            }
        }
    }

    void Spu2::runSamples(uint64_t count)
    {
        const bool diag = diagEnabled(); // read once per batch
        constexpr size_t kChunkFrames = 256u;
        int16_t chunk[kChunkFrames * 2u];
        while (count != 0u)
        {
            // Streams stay "active" forever once pushed, so look at whether any still has frames to play. The lock is
            // held for the whole chunk (mixStreams() needs it per sample) and released before the output is pushed.
            std::unique_lock<std::mutex> streamLock(m_streamMutex, std::defer_lock);
            bool streamsLive = false;
            if (m_batch ? m_streamLive.load(std::memory_order_relaxed) : m_streamsActive.load(std::memory_order_relaxed))
            {
                streamLock.lock();
                streamsLive = anyStreamLive();
                if (!streamsLive)
                    streamLock.unlock();
            }

            // Whole-batch fast path: with no voice to step, no ADMA input and no stream to mix every sample is exact
            // silence (zero input through any volume is zero) and nothing else in step() can change state.
            if (!diag && !streamsLive && m_activeVoices[0] == 0u && m_activeVoices[1] == 0u && inputIdle(0u) &&
                inputIdle(1u))
            {
                m_bulkSamples += count;
                pushOutput(nullptr, static_cast<size_t>(std::min<uint64_t>(count, SIZE_MAX)));
                return;
            }

            const size_t frames = static_cast<size_t>(std::min<uint64_t>(count, kChunkFrames));
            if (diag)
                renderFrames<true>(frames, chunk, streamsLive);
            else
                renderFrames<false>(frames, chunk, streamsLive);
            if (streamLock.owns_lock())
                streamLock.unlock();
            pushOutput(chunk, frames);
            count -= frames;
        }
    }

    void Spu2::step()
    {
        runSamples(1u);
    }

    void Spu2::streamPush(uint32_t stream, uint32_t rate, const int16_t *stereo, size_t frames)
    {
        if (stream >= StreamCount || frames == 0u || rate == 0u)
            return;
        std::lock_guard<std::mutex> lock(m_streamMutex);
        PcmStream &s = m_streams[stream];
        // Drop the frames already played so the vector does not grow without bound.
        if (s.read >= 16384u)
        {
            s.samples.erase(s.samples.begin(), s.samples.begin() + static_cast<std::ptrdiff_t>(s.read * 2u));
            s.read = 0u;
        }
        s.rate = rate;
        s.samples.insert(s.samples.end(), stereo, stereo + frames * 2u);
        m_streamsActive.store(true, std::memory_order_relaxed);
        updateStreamLive();
    }

    uint64_t Spu2::streamConsumed(uint32_t stream) const
    {
        if (stream >= StreamCount)
            return 0u;
        std::lock_guard<std::mutex> lock(m_streamMutex);
        return m_streams[stream].consumedFrames;
    }

    void Spu2::streamReset(uint32_t stream)
    {
        if (stream >= StreamCount)
            return;
        std::lock_guard<std::mutex> lock(m_streamMutex);
        m_streams[stream] = PcmStream{};
        updateStreamLive();
    }

    void Spu2::updateStreamLive() noexcept
    {
        m_streamLive.store(anyStreamLive(), std::memory_order_relaxed);
    }

    void Spu2::mixStreams(int32_t &left, int32_t &right)
    {
        for (PcmStream &s : m_streams)
        {
            const size_t frames = s.samples.size() / 2u;
            if (s.read >= frames)
                continue; // underrun: silence, and the source position does not move
            const size_t next = s.read + 1u < frames ? s.read + 1u : s.read;
            const double a = s.fraction;
            left += static_cast<int32_t>(s.samples[s.read * 2u] * (1.0 - a) + s.samples[next * 2u] * a);
            right += static_cast<int32_t>(s.samples[s.read * 2u + 1u] * (1.0 - a) + s.samples[next * 2u + 1u] * a);
            s.fraction += static_cast<double>(s.rate) / 48000.0;
            while (s.fraction >= 1.0 && s.read < frames)
            {
                s.fraction -= 1.0;
                ++s.read;
                ++s.consumedFrames;
            }
            if (s.read >= frames)
                s.fraction = 0.0;
        }
        if (m_batch)
            updateStreamLive();
    }

    void Spu2::advanceTo(uint64_t iopCycle)
    {
        if (iopCycle <= m_lastCycle)
            return;
        const uint64_t elapsed = iopCycle - m_lastCycle;
        // Most scheduler calls advance only a fraction of a sample. Preserve
        // that remainder without performing a wide division or storing +0.
        if (elapsed < CyclesPerSample)
            return;
        const uint64_t samples = elapsed / CyclesPerSample;
        // runSamples' whole-batch silence path, without entering it: nothing playing, no stream, no ADMA input
        if (!diagEnabled() && !(m_batch ? m_streamLive.load(std::memory_order_relaxed) : m_streamsActive.load(std::memory_order_relaxed)) && m_activeVoices[0] == 0u &&
            m_activeVoices[1] == 0u && inputIdle(0u) && inputIdle(1u))
        {
            m_bulkSamples += samples;
            pushOutput(nullptr, static_cast<size_t>(std::min<uint64_t>(samples, SIZE_MAX)));
        }
        else
        {
            runSamples(samples);
        }
        m_lastCycle += samples * CyclesPerSample;
    }

    size_t Spu2::pendingFrames() const
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        const_cast<Spu2 *>(this)->flushSilenceLocked();
        return m_outCount;
    }

    size_t Spu2::drainOutput(int16_t *stereo, size_t maxFrames, size_t maxLatencyFrames)
    {
        std::lock_guard<std::mutex> lock(m_outputMutex);
        flushSilenceLocked();
        if (maxLatencyFrames != SIZE_MAX && m_outCount > maxFrames + maxLatencyFrames)
        {
            const size_t drop = m_outCount - maxFrames - maxLatencyFrames;
            m_outRead = (m_outRead + drop) % OutputRingFrames;
            m_outCount -= drop;
        }
        const size_t frames = std::min(maxFrames, m_outCount);
        if (frames != 0u)
        {
            const size_t first = std::min(frames, OutputRingFrames - m_outRead);
            std::copy_n(m_outRing.begin() + static_cast<std::ptrdiff_t>(m_outRead * 2u), first * 2u, stereo);
            std::copy_n(m_outRing.begin(), (frames - first) * 2u, stereo + first * 2u);
            m_outRead = (m_outRead + frames) % OutputRingFrames;
            m_outCount -= frames;
        }
        return frames;
    }
}
