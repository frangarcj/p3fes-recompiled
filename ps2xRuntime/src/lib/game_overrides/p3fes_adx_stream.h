#pragma once

// Streams a CRI ADX music track straight out of a CVM (CRI "ROFS" container
// holding a plain ISO9660 image) as 48 kHz stereo signed 16-bit PCM.
//
// Standalone: depends only on the C++17 standard library and on the header-only
// WaveMetadata struct from p3fes_native_bgm_internal.h.

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "p3fes_native_bgm_internal.h"

namespace p3fes_native_bgm::detail
{
    // Maps an uppercase file name ("53.ADX") to its absolute byte range inside a
    // CVM file. The ISO9660 directory tree is parsed once in open().
    class CvmFileLocator
    {
    public:
        // Parses the directory tree of `cvmPath`. Returns false when the file is
        // unreadable or contains no ISO9660 volume descriptor.
        bool open(const std::string &cvmPath);

        // `name` is matched case-insensitively against the leaf file name with
        // the ";1" version suffix removed. Offset/size are absolute byte
        // positions inside the CVM file.
        bool find(const std::string &name, uint64_t &offset, uint64_t &size) const;

        size_t fileCount() const { return entries_.size(); }
        // Names in directory order (uppercase), for diagnostics and tests.
        std::vector<std::string> names() const;

        // Process-wide cache: parses each CVM path once. Thread-safe.
        static std::shared_ptr<const CvmFileLocator> shared(const std::string &cvmPath);

    private:
        struct Entry
        {
            uint64_t offset = 0u;
            uint64_t size = 0u;
        };
        std::map<std::string, Entry> entries_;
        std::vector<std::string> order_;
    };

    // Streaming ADX (encoding type 3, 4-bit ADPCM, version 3/4, unencrypted)
    // decoder with the same shape as WavStreamReader. Output is interleaved
    // stereo s16 at 48 kHz (mono sources are duplicated).
    //
    // Looping (ADX loop header) is performed in the source sample domain: at the
    // loop end the decoder jumps back to the loop-start block with the ADPCM
    // history snapshot taken the first time that block was reached, and the
    // resampler keeps running over the continuous looped stream, so the loop is
    // sample exact and click free. With a loop, readFrames never reports
    // `finished`.
    //
    // No heap allocation happens after open(); readFrames only does fread() of
    // 32 KB chunks of the ADX data.
    class AdxStreamReader
    {
    public:
        AdxStreamReader() = default;
        ~AdxStreamReader();
        AdxStreamReader(const AdxStreamReader &) = delete;
        AdxStreamReader &operator=(const AdxStreamReader &) = delete;

        // Locates `trackName` (e.g. "53.ADX") in the CVM (cached locator) and
        // opens it. When `resampleTo48k` is false the source rate is passed
        // through untouched (metadata().sampleRate is then the source rate and
        // all frame counts are in source samples); this is meant for tests.
        bool open(const std::string &cvmPath, const std::string &trackName,
                  bool resampleTo48k = true);

        // Opens an ADX stored at [offset, offset + size) of `path` (size 0 =
        // until end of file).
        bool openRange(const std::string &path, uint64_t offset, uint64_t size,
                       bool resampleTo48k = true);

        void close();
        bool isOpen() const { return file_ != nullptr; }

        // sampleRate/totalFrames/loopStart/loopEndExclusive are in output
        // frames (48 kHz unless resampling was disabled). channels is always 2.
        const WaveMetadata &metadata() const { return metadata_; }

        // Source (file) properties.
        uint32_t sourceSampleRate() const { return srcRate_; }
        uint32_t sourceChannels() const { return srcChannels_; }
        uint64_t sourceFrames() const { return srcFrames_; }
        bool sourceHasLoop() const { return hasLoop_; }
        uint32_t sourceLoopStart() const { return loopStart_; }
        uint32_t sourceLoopEnd() const { return loopEnd_; }

        // Reads up to frameCapacity interleaved stereo frames. `finished`
        // becomes true when a non-looping track has delivered its last frame.
        bool readFrames(int16_t *destination, uint32_t frameCapacity,
                        uint32_t &framesRead, bool &finished);

    private:
        static constexpr uint32_t kChunkBytes = 32u * 1024u;
        static constexpr uint32_t kBlockSamples = 32u;
        static constexpr uint32_t kBlockBytes = 18u;
        static constexpr uint32_t kSrcChunkFrames = 2048u;
        static constexpr uint32_t kSrcPadFrames = 8u;

        bool parseHeader(uint64_t size, bool resampleTo48k);
        bool readBytes(uint8_t *dst, uint32_t count);
        bool seekData(uint64_t dataRelativeOffset);
        bool decodeBlock();
        // Produces frames of the (possibly looped) source stream. Returns the
        // number of frames written; fewer than requested means end of source
        // (sets sourceEnded_) or an error (sets failed_).
        uint32_t decodeFrames(int16_t *out, uint32_t count);
        bool resampleFrames(int16_t *dst, uint32_t capacity, uint32_t &produced);
        bool refillSource();

        std::FILE *file_ = nullptr;
        WaveMetadata metadata_{};

        // Source description
        uint64_t fileBase_ = 0u;     // absolute offset of the ADX file in `file_`
        uint64_t fileSize_ = 0u;     // size of the ADX file
        uint32_t dataOffset_ = 0u;   // header size (ADX relative)
        uint32_t srcRate_ = 0u;
        uint32_t srcChannels_ = 0u;
        uint64_t srcFrames_ = 0u;
        bool hasLoop_ = false;
        uint32_t loopStart_ = 0u;
        uint32_t loopEnd_ = 0u;
        uint32_t loopBlock_ = 0u;
        int32_t coef0_ = 0;
        int32_t coef1_ = 0;
        uint32_t frameBytes_ = 0u;   // bytes of one block for all channels

        // Buffered file access (ADX-relative positions)
        std::vector<uint8_t> chunk_;
        uint32_t chunkPos_ = 0u;
        uint32_t chunkLen_ = 0u;
        uint64_t filePos_ = 0u;      // ADX-relative position of the byte after chunk end
        uint64_t regionEnd_ = 0u;    // ADX-relative end of readable data

        // ADPCM decoder state
        int32_t hist_[2][2] = {{0, 0}, {0, 0}};
        int32_t loopHist_[2][2] = {{0, 0}, {0, 0}};
        bool loopHistValid_ = false;
        uint32_t curBlock_ = 0u;     // next block to decode
        uint32_t blkIndex_ = 0u;
        bool blkValid_ = false;
        int16_t blk_[kBlockSamples * 2u] = {};
        uint64_t srcPos_ = 0u;       // next source sample (file timeline) to emit
        bool sourceEnded_ = false;
        bool failed_ = false;

        // Output stage
        bool resample_ = false;
        uint64_t outPos_ = 0u;       // output frames delivered
        // Source position of the next output frame = posInt_ + posNum_/48000
        // (exact rational accumulation: no drift, no division per frame).
        uint32_t stepInt_ = 0u;      // srcRate / 48000
        uint32_t stepNum_ = 0u;      // srcRate % 48000
        int64_t posInt_ = 0;         // absolute (looped) source frame index
        uint32_t posNum_ = 0u;
        std::vector<int16_t> srcBuf_; // interleaved stereo source window
        int64_t bufBase_ = 0;        // absolute source index of srcBuf_[0]
        uint32_t bufCount_ = 0u;
        bool bufStarted_ = false;
        bool bufEnded_ = false;      // padding appended, no more source
    };
}
