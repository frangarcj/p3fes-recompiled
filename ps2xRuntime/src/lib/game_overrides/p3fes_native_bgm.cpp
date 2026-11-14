#include "p3fes_native_bgm.h"
#include "p3fes_native_bgm_internal.h"
#include "p3fes_adx_stream.h"

#if defined(PLATFORM_VITA)
#include <psp2/audioout.h>
#include <psp2/avconfig.h>
#include <psp2/kernel/threadmgr.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace p3fes_native_bgm::detail
{
    namespace
    {
        uint16_t readU16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8u);
        }

        uint32_t readU32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8u) |
                   (static_cast<uint32_t>(p[2]) << 16u) |
                   (static_cast<uint32_t>(p[3]) << 24u);
        }

        bool seekTo(std::FILE *file, uint64_t offset)
        {
            if (offset > static_cast<uint64_t>(std::numeric_limits<long>::max()))
                return false;
            return std::fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
        }

        bool readAt(std::FILE *file, uint64_t offset, void *destination, size_t bytes)
        {
            return seekTo(file, offset) && std::fread(destination, 1u, bytes, file) == bytes;
        }
    }

    uint32_t volumeToGainQ16(float volume)
    {
        if (!std::isfinite(volume))
            volume = 0.0f;
        volume = std::clamp(volume, 0.0f, 1.0f);
        return static_cast<uint32_t>(volume * 65536.0f + 0.5f);
    }

    void applyGainQ16(int16_t *samples, size_t sampleCount, uint32_t gainQ16)
    {
        if (!samples || gainQ16 >= 65536u)
            return;
        for (size_t i = 0u; i < sampleCount; ++i)
        {
            const int32_t signedSample = samples[i];
            const bool negative = signedSample < 0;
            // Widen before negation so -32768 is represented as magnitude
            // 32768 without overflowing a signed 16-bit value.
            const uint32_t magnitude = negative
                ? static_cast<uint32_t>(-signedSample)
                : static_cast<uint32_t>(signedSample);
            const uint64_t product = static_cast<uint64_t>(magnitude) * gainQ16 + 0x8000u;
            const uint32_t scaledMagnitude = static_cast<uint32_t>(product >> 16u);
            if (negative)
                samples[i] = static_cast<int16_t>(-static_cast<int32_t>(std::min(scaledMagnitude, 32768u)));
            else
                samples[i] = static_cast<int16_t>(std::min(scaledMagnitude, 32767u));
        }
    }

    WavStreamReader::~WavStreamReader()
    {
        if (file_)
            std::fclose(file_);
    }

    bool WavStreamReader::open(const std::string &path)
    {
        if (file_)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
        metadata_ = {};
        framePosition_ = 0u;

        file_ = std::fopen(path.c_str(), "rb");
        if (!file_)
            return false;

        if (std::fseek(file_, 0, SEEK_END) != 0)
            return false;
        const long fileSizeSigned = std::ftell(file_);
        if (fileSizeSigned < 12 || !seekTo(file_, 0u))
            return false;
        const uint64_t fileSize = static_cast<uint64_t>(fileSizeSigned);

        std::array<uint8_t, 12> riff{};
        if (std::fread(riff.data(), 1u, riff.size(), file_) != riff.size() ||
            std::memcmp(riff.data(), "RIFF", 4u) != 0 ||
            std::memcmp(riff.data() + 8u, "WAVE", 4u) != 0)
            return false;

        const uint64_t declaredEnd = static_cast<uint64_t>(readU32(riff.data() + 4u)) + 8u;
        if (declaredEnd > fileSize || declaredEnd < 12u)
            return false;
        const uint64_t riffEnd = declaredEnd;

        bool haveFormat = false;
        bool haveData = false;
        uint64_t cursor = 12u;
        while (cursor <= riffEnd && riffEnd - cursor >= 8u)
        {
            std::array<uint8_t, 8> chunk{};
            if (!readAt(file_, cursor, chunk.data(), chunk.size()))
                return false;
            const uint32_t chunkSize = readU32(chunk.data() + 4u);
            const uint64_t payload = cursor + 8u;
            if (chunkSize > riffEnd - payload)
                return false;
            const uint64_t chunkEnd = payload + chunkSize;

            if (std::memcmp(chunk.data(), "fmt ", 4u) == 0)
            {
                if (chunkSize < 16u)
                    return false;
                std::array<uint8_t, 16> format{};
                if (!readAt(file_, payload, format.data(), format.size()))
                    return false;
                const uint16_t encoding = readU16(format.data());
                metadata_.channels = readU16(format.data() + 2u);
                metadata_.sampleRate = readU32(format.data() + 4u);
                const uint32_t byteRate = readU32(format.data() + 8u);
                metadata_.blockAlign = readU16(format.data() + 12u);
                metadata_.bitsPerSample = readU16(format.data() + 14u);
                if (encoding != 1u || metadata_.channels != 2u ||
                    metadata_.sampleRate != 48000u || metadata_.bitsPerSample != 16u ||
                    metadata_.blockAlign != 4u || byteRate != 192000u)
                    return false;
                haveFormat = true;
            }
            else if (std::memcmp(chunk.data(), "data", 4u) == 0 && !haveData)
            {
                metadata_.dataOffset = payload;
                metadata_.dataBytes = chunkSize;
                haveData = true;
            }
            else if (std::memcmp(chunk.data(), "smpl", 4u) == 0)
            {
                if (chunkSize < 36u)
                    return false;
                std::array<uint8_t, 36> sampleHeader{};
                if (!readAt(file_, payload, sampleHeader.data(), sampleHeader.size()))
                    return false;
                const uint32_t loopCount = readU32(sampleHeader.data() + 28u);
                const uint64_t availableLoops = (chunkSize - 36u) / 24u;
                if (loopCount > availableLoops)
                    return false;
                const uint32_t loopsToRead = loopCount;
                for (uint32_t i = 0; i < loopsToRead; ++i)
                {
                    std::array<uint8_t, 24> loop{};
                    if (!readAt(file_, payload + 36u + static_cast<uint64_t>(i) * 24u,
                                loop.data(), loop.size()))
                        return false;
                    const uint32_t loopType = readU32(loop.data() + 4u);
                    const uint32_t start = readU32(loop.data() + 8u);
                    const uint32_t endInclusive = readU32(loop.data() + 12u);
                    if (endInclusive < start)
                        return false;
                    if (loopType == 0u)
                    {
                        metadata_.hasLoop = true;
                        metadata_.loopStart = start;
                        metadata_.loopEndExclusive = static_cast<uint64_t>(endInclusive) + 1u;
                        break;
                    }
                }
            }

            const uint64_t next = chunkEnd + (chunkSize & 1u);
            if (next <= cursor || next > riffEnd)
                return false;
            cursor = next;
        }

        if (!haveFormat || !haveData || metadata_.dataBytes == 0u ||
            metadata_.dataBytes % metadata_.blockAlign != 0u)
            return false;
        metadata_.totalFrames = metadata_.dataBytes / metadata_.blockAlign;
        if (metadata_.hasLoop &&
            (metadata_.loopStart >= metadata_.loopEndExclusive ||
             metadata_.loopEndExclusive > metadata_.totalFrames))
            return false;
        return true;
    }

    bool WavStreamReader::readFrames(int16_t *destination, uint32_t frameCapacity,
                                     uint32_t &framesRead, bool &finished)
    {
        framesRead = 0u;
        finished = false;
        if (!file_ || !destination || frameCapacity == 0u)
            return false;

        while (framesRead < frameCapacity)
        {
            const uint64_t boundary = metadata_.hasLoop ? metadata_.loopEndExclusive : metadata_.totalFrames;
            if (framePosition_ >= boundary)
            {
                if (metadata_.hasLoop)
                {
                    framePosition_ = metadata_.loopStart;
                    continue;
                }
                finished = true;
                break;
            }

            const uint64_t remaining = boundary - framePosition_;
            const uint32_t frames = static_cast<uint32_t>(
                std::min<uint64_t>(remaining, frameCapacity - framesRead));
            const uint64_t byteOffset = metadata_.dataOffset + framePosition_ * metadata_.blockAlign;
            const size_t byteCount = static_cast<size_t>(frames) * metadata_.blockAlign;
            if (!readAt(file_, byteOffset,
                        destination + static_cast<size_t>(framesRead) * metadata_.channels,
                        byteCount))
                return false;
            framePosition_ += frames;
            framesRead += frames;
        }

        if (!metadata_.hasLoop && framePosition_ >= metadata_.totalFrames)
            finished = true;
        return true;
    }
}

namespace p3fes_native_bgm
{
    namespace
    {
        std::atomic<bool> g_playing{false};
        std::atomic<float> g_volume{1.0f};

        std::mutex g_controlMutex;
        std::atomic<bool> g_stopRequested{false};
        std::atomic<bool> g_paused{false};
        std::atomic<float> g_fadeDuration{0.0f};
        std::atomic<bool> g_fadeRequested{false};
        std::thread g_audioThread;
        std::mutex g_startupMutex;
        std::condition_variable g_startupCondition;
        bool g_startupDone = false;
        bool g_startupSuccess = false;

        void stopLocked()
        {
            g_stopRequested.store(true, std::memory_order_release);
            if (g_audioThread.joinable())
                g_audioThread.join();
            g_playing.store(false, std::memory_order_release);
            g_paused.store(false, std::memory_order_release);
            g_fadeRequested.store(false, std::memory_order_release);
            g_fadeDuration.store(0.0f, std::memory_order_relaxed);
            g_stopRequested.store(false, std::memory_order_release);
        }

        void signalStartup(bool success)
        {
            {
                std::lock_guard<std::mutex> lock(g_startupMutex);
                g_startupSuccess = success;
                g_startupDone = true;
            }
            g_startupCondition.notify_one();
        }

        struct ShutdownGuard
        {
            ~ShutdownGuard() { stopLocked(); }
        };
        ShutdownGuard g_shutdownGuard;

        bool g_portInfoLogged = false;

        constexpr uint32_t kFramesPerBuffer = 1024u;
        constexpr uint32_t kChannels = 2u;
        constexpr uint32_t kSampleRate = 48000u;

        void applyGain(std::array<int16_t, kFramesPerBuffer * kChannels> &samples, float gain)
        {
            detail::applyGainQ16(samples.data(), samples.size(), detail::volumeToGainQ16(gain));
        }

        // Output sink. Everything else in audioLoop (WAV reading, looping,
        // fade, volume, pause) is shared between platforms.
#if defined(PLATFORM_VITA)
        class AudioSink
        {
        public:
            bool open()
            {
                port_ = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN,
                                            static_cast<int>(kFramesPerBuffer),
                                            static_cast<int>(kSampleRate),
                                            SCE_AUDIO_OUT_MODE_STEREO);
                // Only one MAIN port may exist; when the SPU2 output (SDL) already has it, use a BGM port.
                if (port_ < 0)
                    port_ = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, static_cast<int>(kFramesPerBuffer),
                                                static_cast<int>(kSampleRate), SCE_AUDIO_OUT_MODE_STEREO);
                if (port_ < 0)
                {
                    std::fprintf(stderr, "[native-bgm] audio port open failed: %d\n", port_);
                    return false;
                }
                // Full port volume, stated rather than assumed, and the console's own volume in the
                // log: "no sound" reports need both to tell a muted console from a silent stream.
                int volume[2] = {SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL};
                const int set = sceAudioOutSetVolume(port_, static_cast<SceAudioOutChannelFlag>(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), volume);
                int systemVolume = -1;
                const int got = sceAVConfigGetSystemVol(&systemVolume);
                std::fprintf(stderr, "[native-bgm] port %d set-volume=%08X system-volume=%d (%08X) rest=%d\n", port_,
                             static_cast<unsigned>(set), systemVolume, static_cast<unsigned>(got),
                             sceAudioOutGetRestSample(port_));
                return true;
            }
            int id() const { return port_; }
            bool output(const int16_t *samples) { return sceAudioOutOutput(port_, samples) >= 0; }
            void close()
            {
                sceAudioOutOutput(port_, nullptr);
                sceAudioOutReleasePort(port_);
            }

        private:
            int port_ = -1;
        };
#else
        // Null sink for desktop benches: discards blocks but paces them by the
        // wall clock so the audio thread consumes PCM at the real-time rate.
        class AudioSink
        {
        public:
            bool open()
            {
                next_ = std::chrono::steady_clock::now();
                return true;
            }
            int id() const { return 0; }
            bool output(const int16_t *)
            {
                next_ += std::chrono::nanoseconds(
                    static_cast<int64_t>(kFramesPerBuffer) * 1000000000ll / kSampleRate);
                const auto now = std::chrono::steady_clock::now();
                if (next_ > now)
                    std::this_thread::sleep_until(next_);
                else if (now - next_ > std::chrono::milliseconds(250))
                    next_ = now; // fell far behind (e.g. paused/stalled); do not burst
                return true;
            }
            void close() {}

        private:
            std::chrono::steady_clock::time_point next_{};
        };
#endif

        // The track being played: a prepared WAV, or an ADX decoded straight out of the game's BGM.CVM
        // ("<cvm path>|<TRACK.ADX>"). Both readers deliver 48 kHz stereo and loop by themselves.
        struct PcmSource
        {
            std::unique_ptr<detail::WavStreamReader> wav;
            std::unique_ptr<detail::AdxStreamReader> adx;

            bool open(const std::string &path)
            {
                const size_t bar = path.find('|');
                if (bar != std::string::npos)
                {
                    adx = std::make_unique<detail::AdxStreamReader>();
                    return adx->open(path.substr(0u, bar), path.substr(bar + 1u));
                }
                wav = std::make_unique<detail::WavStreamReader>();
                return wav->open(path);
            }
            const detail::WaveMetadata &metadata() const { return adx ? adx->metadata() : wav->metadata(); }
            bool readFrames(int16_t *destination, uint32_t frameCapacity, uint32_t &framesRead, bool &finished)
            {
                return adx ? adx->readFrames(destination, frameCapacity, framesRead, finished)
                           : wav->readFrames(destination, frameCapacity, framesRead, finished);
            }
        };

        void audioLoop(std::unique_ptr<PcmSource> reader)
        {
#if defined(PLATFORM_VITA)
            // Threads made by std::thread start at the lowest user priority (191), below the game
            // thread (128): whenever the game or the renderer was busy this thread missed the 21 ms
            // deadline of its next block and the music crackled. Audio must win: highest user
            // priority, and off the game thread's core (1 unless PS2X_VITA_GAME_CORE says otherwise).
            {
                const char *value = std::getenv("PS2X_VITA_GAME_CORE");
                const int gameCore = value ? std::atoi(value) : 1;
                const SceUID self = sceKernelGetThreadId();
                sceKernelChangeThreadPriority(self, 64);
                if (gameCore >= 0 && gameCore <= 2)
                    sceKernelChangeThreadCpuAffinityMask(self, 0x70000 & ~(0x10000 << gameCore));
            }
#endif
            const char *diagEnv = std::getenv("P3FES_RESOURCE_DIAG");
            const bool diagnostics = diagEnv && diagEnv[0] == '1' && diagEnv[1] == '\0';
            AudioSink sink;
            if (!sink.open())
            {
                g_playing.store(false, std::memory_order_release);
                signalStartup(false);
                return;
            }

            if (diagnostics && !g_portInfoLogged)
            {
                std::fprintf(stderr, "[native-bgm] audio port opened id=%d rate=%u channels=%u frames=%u\n",
                             sink.id(), kSampleRate, kChannels, kFramesPerBuffer);
                g_portInfoLogged = true;
            }

            g_playing.store(true, std::memory_order_release);
            signalStartup(true);
            std::array<int16_t, kFramesPerBuffer * kChannels> samples{};
            // Reading and decoding happen on a feeder thread that keeps about half a second of
            // PCM queued. This thread only takes finished blocks, so a slow read from the memory
            // card (the game streams from the same card) can no longer make it miss the port's
            // 21 ms deadline; that was the crackle left after the priority fix.
            struct Block
            {
                std::array<int16_t, kFramesPerBuffer * kChannels> samples;
                uint32_t frames = 0u;
                bool finished = false;
                bool failed = false;
            };
            constexpr size_t kQueuedBlocks = 24u;
            std::vector<Block> ring(kQueuedBlocks);
            size_t ringHead = 0u, ringCount = 0u;
            bool feederDone = false, feederStop = false;
            struct Pacing
            {
                uint64_t blocks = 0u, frames = 0u;
                double seconds = 0.0;
                int peak = 0;
                float gain = 0.0f;
                unsigned late = 0u;
                double worstGapMs = 0.0;
                unsigned dry = 0u;
            } pacing;
            bool pacingReady = false;
            std::mutex ringMutex;
            std::condition_variable ringSpace, ringData;
            std::thread feeder([&] {
#if defined(PLATFORM_VITA)
                {
                    const char *value = std::getenv("PS2X_VITA_GAME_CORE");
                    const int gameCore = value ? std::atoi(value) : 1;
                    const SceUID self = sceKernelGetThreadId();
                    sceKernelChangeThreadPriority(self, 100);
                    if (gameCore >= 0 && gameCore <= 2)
                        sceKernelChangeThreadCpuAffinityMask(self, 0x70000 & ~(0x10000 << gameCore));
                }
#endif
                Block block;
                for (;;)
                {
                    block.frames = 0u;
                    block.finished = false;
                    block.failed = !reader->readFrames(block.samples.data(), kFramesPerBuffer, block.frames, block.finished);
                    if (block.frames < kFramesPerBuffer)
                        std::fill(block.samples.begin() + static_cast<size_t>(block.frames) * kChannels,
                                  block.samples.end(), 0);
                    // P3FES_BGM_TEST_TONE=1: replace the music by a steady 440 Hz tone at -12 dB. A
                    // listener can then tell an output-path fault (the tone crackles too) from one
                    // in the music itself (the tone is clean).
                    static const bool testTone = [] {
                        const char *value = std::getenv("P3FES_BGM_TEST_TONE");
                        return value && *value == '1';
                    }();
                    if (testTone && block.frames != 0u)
                    {
                        static uint32_t phase = 0u; // 440 Hz at 48 kHz repeats every 1200 frames
                        for (uint32_t i = 0; i < kFramesPerBuffer; ++i, phase = (phase + 1u) % 1200u)
                        {
                            const auto value = static_cast<int16_t>(8192.0 * std::sin(phase * (2.0 * 3.14159265358979323846 * 440.0 / 48000.0)));
                            block.samples[i * 2u] = block.samples[i * 2u + 1u] = value;
                        }
                    }
                    // P3FES_BGM_DUMP=<seconds>: save that much of the decoded music (s16le stereo
                    // 48 kHz, before the volume) as ux0:data/p3fes/bgm-dump.raw, to check off-line
                    // what the console really plays.
                    {
                        static const int dumpBlocks = [] {
                            const char *value = std::getenv("P3FES_BGM_DUMP");
                            return value ? std::atoi(value) * static_cast<int>(kSampleRate / kFramesPerBuffer) : 0;
                        }();
                        static int dumped = 0;
                        static FILE *dump = nullptr;
                        if (dumped < dumpBlocks && block.frames != 0u)
                        {
#if defined(PLATFORM_VITA)
                            if (!dump) dump = std::fopen("ux0:data/p3fes/bgm-dump.raw", "wb");
#else
                            if (!dump) dump = std::fopen("bgm-dump.raw", "wb");
#endif
                            if (dump)
                            {
                                std::fwrite(block.samples.data(), sizeof(int16_t), block.samples.size(), dump);
                                if (++dumped == dumpBlocks) { std::fclose(dump); dump = nullptr; }
                            }
                        }
                    }
                    const bool last = block.failed || (block.frames == 0u && block.finished) ||
                                      (block.finished && !reader->metadata().hasLoop);
                    std::unique_lock<std::mutex> lock(ringMutex);
                    ringSpace.wait(lock, [&] { return ringCount < kQueuedBlocks || feederStop; });
                    if (feederStop)
                        break;
                    ring[(ringHead + ringCount) % kQueuedBlocks] = block;
                    ++ringCount;
                    if (last)
                        feederDone = true;
                    ringData.notify_one();
                    const bool report = pacingReady;
                    const Pacing line = pacing;
                    pacingReady = false;
                    lock.unlock();
                    if (report)
                        std::fprintf(stderr,
                                     "[native-bgm] pacing blocks=%llu pcm_frames=%llu wall_seconds=%.3f peak=%d gain=%.3f late=%u worst-gap-ms=%.1f dry=%u\n",
                                     static_cast<unsigned long long>(line.blocks), static_cast<unsigned long long>(line.frames),
                                     line.seconds, line.peak, line.gain, line.late, line.worstGapMs, line.dry);
                    if (last)
                        break;
                }
                std::lock_guard<std::mutex> lock(ringMutex);
                feederDone = true;
                ringData.notify_one();
            });
            const auto stopFeeder = [&] {
                {
                    std::lock_guard<std::mutex> lock(ringMutex);
                    feederStop = true;
                }
                ringSpace.notify_one();
                if (feeder.joinable())
                    feeder.join();
            };
            {
                // Start with a few blocks in hand.
                std::unique_lock<std::mutex> lock(ringMutex);
                ringData.wait_for(lock, std::chrono::milliseconds(500), [&] { return ringCount >= 6u || feederDone; });
            }
            unsigned dryBlocks = 0u;
            // Per-stream statistics for the pacing line.
            int peak = 0;
            unsigned lateBlocks = 0u;
            double worstGapMs = 0.0;
            std::chrono::steady_clock::time_point lastOutput{};
            uint64_t outputPcmFrames = 0u;
            uint64_t outputBlocks = 0u;
            const auto streamStarted = std::chrono::steady_clock::now();
            bool fadeActive = false;
            float fadeDuration = 0.0f;
            float fadeStartVolume = 1.0f;
            auto fadeStart = std::chrono::steady_clock::now();
            bool wasPaused = false;
            std::chrono::steady_clock::time_point pauseStart{};

            while (!g_stopRequested.load(std::memory_order_acquire))
            {
                if (g_paused.load(std::memory_order_acquire))
                {
                    if (!wasPaused)
                    {
                        wasPaused = true;
                        pauseStart = std::chrono::steady_clock::now();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    continue;
                }
                if (wasPaused)
                {
                    fadeStart += std::chrono::steady_clock::now() - pauseStart;
                    wasPaused = false;
                }

                if (g_fadeRequested.exchange(false, std::memory_order_acq_rel))
                {
                    fadeDuration = g_fadeDuration.load(std::memory_order_relaxed);
                    fadeStartVolume = g_volume.load(std::memory_order_relaxed);
                    fadeStart = std::chrono::steady_clock::now();
                    fadeActive = true;
                }

                uint32_t framesRead = 0u;
                bool finished = false;
                {
                    bool failed = false, ended = false;
                    std::unique_lock<std::mutex> lock(ringMutex);
                    if (ringCount == 0u && !feederDone)
                        ringData.wait_for(lock, std::chrono::milliseconds(10), [&] { return ringCount != 0u || feederDone; });
                    if (ringCount != 0u)
                    {
                        const Block &block = ring[ringHead];
                        samples = block.samples;
                        framesRead = block.frames;
                        finished = block.finished;
                        failed = block.failed;
                        ringHead = (ringHead + 1u) % kQueuedBlocks;
                        --ringCount;
                        ringSpace.notify_one();
                    }
                    else if (feederDone)
                        ended = true;
                    else
                    {
                        // Nothing decoded in time: play silence for this block rather than stall.
                        samples.fill(0);
                        framesRead = kFramesPerBuffer;
                        ++dryBlocks;
                    }
                    lock.unlock();
                    if (failed)
                    {
                        std::fprintf(stderr, "[native-bgm] WAV stream read failed\n");
                        break;
                    }
                    if (ended || (framesRead == 0u && finished))
                        break;
                }

                // Headroom: several tracks are mastered right up to full scale (the dorm theme
                // peaks at 32767 throughout). On the PS2 they pass through the SPU2 mixer well below
                // that; sent to the Vita's mixer at full scale they leave it no room. P3FES_BGM_GAIN
                // (default 0.7, about -3 dB) scales the music; 1 restores the raw level.
                static const float masterGain = [] {
                    const char *value = std::getenv("P3FES_BGM_GAIN");
                    const float parsed = value && *value ? static_cast<float>(std::atof(value)) : 0.7f;
                    return parsed > 0.0f && parsed <= 1.0f ? parsed : 0.7f;
                }();
                float gain = g_volume.load(std::memory_order_relaxed) * masterGain;
                bool fadeFinished = false;
                if (fadeActive)
                {
                    const float elapsed = std::chrono::duration<float>(
                        std::chrono::steady_clock::now() - fadeStart).count();
                    const float fade = fadeDuration <= 0.0f ? 0.0f :
                        std::clamp(1.0f - elapsed / fadeDuration, 0.0f, 1.0f);
                    gain = std::min(gain, fadeStartVolume * fade);
                    fadeFinished = fade <= 0.0f;
                }
                applyGain(samples, gain);
                for (const int16_t sample : samples)
                    peak = std::max(peak, static_cast<int>(sample < 0 ? -sample : sample));

                // A block lasts 21.3 ms and output() blocks until the port takes it: a call that
                // comes back much later than that after the previous one means the port ran dry.
                {
                    const auto now = std::chrono::steady_clock::now();
                    if (lastOutput.time_since_epoch().count() != 0)
                    {
                        const double ms = std::chrono::duration<double, std::milli>(now - lastOutput).count();
                        worstGapMs = std::max(worstGapMs, ms);
                        if (ms > 32.0) ++lateBlocks;
                    }
                    lastOutput = now;
                }
                if (!sink.output(samples.data()))
                {
                    std::fprintf(stderr, "[native-bgm] audio output failed\n");
                    break;
                }
                outputPcmFrames += framesRead;
                ++outputBlocks;
                if (diagnostics && outputBlocks % 240u == 0u)
                {
                    const double seconds = std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - streamStarted).count();
                    // Printed by the feeder thread: a write to the log file from this thread can
                    // block longer than a block lasts, which is itself an audible gap.
                    {
                        std::lock_guard<std::mutex> lock(ringMutex);
                        pacing = {outputBlocks, outputPcmFrames, seconds, peak, gain, lateBlocks, worstGapMs, dryBlocks};
                        pacingReady = true;
                    }
                    dryBlocks = 0u;
                    peak = 0;
                    lateBlocks = 0u;
                    worstGapMs = 0.0;
                }
                if (fadeFinished || (finished && !reader->metadata().hasLoop))
                    break;
            }

            stopFeeder();
            sink.close();
            if (diagnostics)
            {
                const double seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - streamStarted).count();
                std::fprintf(stderr,
                             "[native-bgm] closed blocks=%llu pcm_frames=%llu wall_seconds=%.3f\n",
                             static_cast<unsigned long long>(outputBlocks),
                             static_cast<unsigned long long>(outputPcmFrames), seconds);
            }
            g_playing.store(false, std::memory_order_release);
        }
    }

    bool start(const std::string &path)
    {
        std::lock_guard<std::mutex> lock(g_controlMutex);
        stopLocked();

        auto reader = std::make_unique<PcmSource>();
        if (!reader->open(path))
        {
            std::fprintf(stderr, "[native-bgm] cannot open track: %s\n", path.c_str());
            return false;
        }

        const detail::WaveMetadata &metadata = reader->metadata();
        std::fprintf(stderr,
                     "[native-bgm] start %s frames=%llu loop=%u [%llu,%llu)\n",
                     path.c_str(), static_cast<unsigned long long>(metadata.totalFrames),
                     metadata.hasLoop ? 1u : 0u,
                     static_cast<unsigned long long>(metadata.loopStart),
                     static_cast<unsigned long long>(metadata.loopEndExclusive));
        g_stopRequested.store(false, std::memory_order_release);
        g_paused.store(false, std::memory_order_release);
        g_fadeRequested.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> startupLock(g_startupMutex);
            g_startupDone = false;
            g_startupSuccess = false;
        }
        try
        {
            g_audioThread = std::thread(audioLoop, std::move(reader));
        }
        catch (...)
        {
            g_playing.store(false, std::memory_order_release);
            return false;
        }

        std::unique_lock<std::mutex> startupLock(g_startupMutex);
        g_startupCondition.wait(startupLock, [] { return g_startupDone; });
        const bool started = g_startupSuccess;
        startupLock.unlock();
        if (!started)
        {
            if (g_audioThread.joinable())
                g_audioThread.join();
            g_playing.store(false, std::memory_order_release);
        }
        return started;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(g_controlMutex);
        stopLocked();
    }

    void fadeOut(float seconds)
    {
        if (!g_playing.load(std::memory_order_acquire))
            return;
        g_fadeDuration.store(std::max(0.0f, seconds), std::memory_order_relaxed);
        g_fadeRequested.store(true, std::memory_order_release);
    }

    bool playing()
    {
        return g_playing.load(std::memory_order_acquire);
    }

    void setVolume(float volume)
    {
        if (!std::isfinite(volume))
            volume = 0.0f;
        g_volume.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void pause(bool paused)
    {
        g_paused.store(paused, std::memory_order_release);
    }
}
