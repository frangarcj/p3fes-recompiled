#pragma once

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <string>

namespace p3fes_native_bgm::detail
{
    uint32_t volumeToGainQ16(float volume);
    void applyGainQ16(int16_t *samples, size_t sampleCount, uint32_t gainQ16);

    struct WaveMetadata
    {
        uint32_t sampleRate = 0u;
        uint16_t channels = 0u;
        uint16_t bitsPerSample = 0u;
        uint16_t blockAlign = 0u;
        uint64_t dataOffset = 0u;
        uint64_t dataBytes = 0u;
        uint64_t totalFrames = 0u;
        bool hasLoop = false;
        uint64_t loopStart = 0u;
        uint64_t loopEndExclusive = 0u;
    };

    // Streaming reader shared with parser/loop tests. It keeps the WAV file
    // open and seeks/reads only the requested PCM block; sample data is never
    // copied into a whole-track allocation.
    class WavStreamReader
    {
    public:
        WavStreamReader() = default;
        ~WavStreamReader();
        WavStreamReader(const WavStreamReader &) = delete;
        WavStreamReader &operator=(const WavStreamReader &) = delete;

        bool open(const std::string &path);
        const WaveMetadata &metadata() const { return metadata_; }

        // Reads up to frameCapacity interleaved PCM frames. `finished` becomes
        // true when a non-looping data chunk reaches EOF.
        bool readFrames(int16_t *destination, uint32_t frameCapacity,
                        uint32_t &framesRead, bool &finished);

    private:
        std::FILE *file_ = nullptr;
        WaveMetadata metadata_{};
        uint64_t framePosition_ = 0u;
    };
}
