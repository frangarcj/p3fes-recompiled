#include "game_overrides/p3fes_native_bgm_internal.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    void appendU16(std::vector<uint8_t> &out, uint16_t value)
    {
        out.push_back(static_cast<uint8_t>(value));
        out.push_back(static_cast<uint8_t>(value >> 8u));
    }

    void appendU32(std::vector<uint8_t> &out, uint32_t value)
    {
        for (unsigned i = 0u; i < 4u; ++i)
            out.push_back(static_cast<uint8_t>(value >> (i * 8u)));
    }

    void appendId(std::vector<uint8_t> &out, const char id[4])
    {
        out.insert(out.end(), id, id + 4);
    }

    void appendChunk(std::vector<uint8_t> &wave, const char id[4], const std::vector<uint8_t> &payload)
    {
        appendId(wave, id);
        appendU32(wave, static_cast<uint32_t>(payload.size()));
        wave.insert(wave.end(), payload.begin(), payload.end());
        if (payload.size() & 1u)
            wave.push_back(0u);
    }

    std::string writeFixture(bool withLoop, uint32_t loopStart = 1u,
                             uint32_t loopEnd = 3u, bool truncateLoopRecord = false)
    {
        char path[] = "/tmp/p3fes-native-bgm-XXXXXX";
        const int fd = mkstemp(path);
        assert(fd >= 0);
        std::FILE *file = fdopen(fd, "wb");
        assert(file);

        std::vector<uint8_t> wave;
        appendId(wave, "RIFF");
        appendU32(wave, 0u);
        appendId(wave, "WAVE");

        std::vector<uint8_t> format;
        appendU16(format, 1u);       // PCM
        appendU16(format, 2u);       // stereo
        appendU32(format, 48000u);
        appendU32(format, 192000u);
        appendU16(format, 4u);
        appendU16(format, 16u);
        appendChunk(wave, "fmt ", format);

        if (withLoop)
        {
            std::vector<uint8_t> smpl(36u, 0u);
            smpl[28] = 1u; // one loop record
            appendU32(smpl, 7u); // cue id
            appendU32(smpl, 0u); // forward loop
            appendU32(smpl, loopStart); // start frame
            appendU32(smpl, loopEnd); // inclusive end frame
            appendU32(smpl, 0u); // fraction
            appendU32(smpl, 0u); // infinite repeat
            if (truncateLoopRecord)
                smpl.resize(40u);
            appendChunk(wave, "smpl", smpl);
        }

        std::vector<uint8_t> data;
        for (int16_t frame = 1; frame <= 5; ++frame)
        {
            appendU16(data, static_cast<uint16_t>(frame));
            appendU16(data, static_cast<uint16_t>(-frame));
        }
        appendChunk(wave, "data", data);
        const uint32_t riffBytes = static_cast<uint32_t>(wave.size() - 8u);
        for (unsigned i = 0u; i < 4u; ++i)
            wave[4u + i] = static_cast<uint8_t>(riffBytes >> (i * 8u));
        assert(std::fwrite(wave.data(), 1u, wave.size(), file) == wave.size());
        std::fclose(file);
        return path;
    }

    void truncateLastByte(const std::string &path)
    {
        std::FILE *file = std::fopen(path.c_str(), "rb");
        assert(file);
        assert(std::fseek(file, 0, SEEK_END) == 0);
        const long size = std::ftell(file);
        std::fclose(file);
        assert(size > 0);
        assert(truncate(path.c_str(), size - 1) == 0);
    }
}

int main()
{
    {
        std::array<int16_t, 7> samples{-32768, -3, -1, 0, 1, 3, 32767};
        const uint32_t halfGain = p3fes_native_bgm::detail::volumeToGainQ16(0.5f);
        assert(halfGain == 32768u);
        p3fes_native_bgm::detail::applyGainQ16(samples.data(), samples.size(), halfGain);
        const std::array<int16_t, 7> expected{-16384, -2, -1, 0, 1, 2, 16384};
        assert(samples == expected);
    }

    {
        const std::string path = writeFixture(true);
        p3fes_native_bgm::detail::WavStreamReader reader;
        assert(reader.open(path));
        const auto &metadata = reader.metadata();
        assert(metadata.sampleRate == 48000u && metadata.channels == 2u);
        assert(metadata.totalFrames == 5u);
        assert(metadata.hasLoop && metadata.loopStart == 1u && metadata.loopEndExclusive == 4u);

        std::array<int16_t, 16> samples{};
        uint32_t framesRead = 0u;
        bool finished = false;
        assert(reader.readFrames(samples.data(), 8u, framesRead, finished));
        assert(framesRead == 8u && !finished);
        const std::array<int16_t, 8> expectedLeft{1, 2, 3, 4, 2, 3, 4, 2};
        const std::array<int16_t, 8> expectedRight{-1, -2, -3, -4, -2, -3, -4, -2};
        for (size_t i = 0; i < expectedLeft.size(); ++i)
        {
            assert(samples[i * 2u] == expectedLeft[i]);
            assert(samples[i * 2u + 1u] == expectedRight[i]);
        }
        std::remove(path.c_str());
    }

    {
        const std::string path = writeFixture(false);
        p3fes_native_bgm::detail::WavStreamReader reader;
        assert(reader.open(path));
        assert(!reader.metadata().hasLoop);
        std::array<int16_t, 16> samples{};
        uint32_t framesRead = 0u;
        bool finished = false;
        assert(reader.readFrames(samples.data(), 8u, framesRead, finished));
        assert(framesRead == 5u && finished);
        std::remove(path.c_str());
    }

    // A declared loop must not be silently discarded when its record is
    // truncated or points outside the data chunk.
    {
        const std::string path = writeFixture(true, 1u, 3u, true);
        p3fes_native_bgm::detail::WavStreamReader reader;
        assert(!reader.open(path));
        std::remove(path.c_str());
    }
    {
        const std::string path = writeFixture(true, 4u, 9u);
        p3fes_native_bgm::detail::WavStreamReader reader;
        assert(!reader.open(path));
        std::remove(path.c_str());
    }
    {
        const std::string path = writeFixture(false);
        truncateLastByte(path);
        p3fes_native_bgm::detail::WavStreamReader reader;
        assert(!reader.open(path));
        std::remove(path.c_str());
    }
}
