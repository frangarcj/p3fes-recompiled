#include "p3fes_adx_stream.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <utility>

#if defined(_WIN32)
#define P3FES_FSEEK64(f, off, whence) _fseeki64((f), static_cast<__int64>(off), (whence))
#else
#define P3FES_FSEEK64(f, off, whence) fseeko((f), static_cast<off_t>(off), (whence))
#endif

namespace p3fes_native_bgm::detail
{
    namespace
    {
        constexpr uint64_t kSector = 2048u;
        constexpr uint64_t kHeaderScanLimit = 1024u * 1024u;
        constexpr uint64_t kMaxDirectoryBytes = 8u * 1024u * 1024u;
        constexpr uint32_t kOutputRate = 48000u;

        inline uint16_t be16(const uint8_t *p)
        {
            return static_cast<uint16_t>((static_cast<uint32_t>(p[0]) << 8) | p[1]);
        }
        inline uint32_t be32(const uint8_t *p)
        {
            return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
        }
        inline uint32_t le32(const uint8_t *p)
        {
            return (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[1]) << 8) | static_cast<uint32_t>(p[0]);
        }

        bool readAtOffset(std::FILE *file, uint64_t offset, void *dst, size_t count)
        {
            if (count == 0u)
                return true;
            if (P3FES_FSEEK64(file, offset, SEEK_SET) != 0)
                return false;
            return std::fread(dst, 1u, count, file) == count;
        }

        bool fileLength(std::FILE *file, uint64_t &length)
        {
            if (P3FES_FSEEK64(file, 0, SEEK_END) != 0)
                return false;
            const long long end = static_cast<long long>(std::ftell(file));
            if (end < 0)
                return false;
            length = static_cast<uint64_t>(end);
            return true;
        }

        std::string upperAscii(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        inline int16_t clip16(int32_t v)
        {
            return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
        }

        uint64_t roundRate(uint64_t frames, uint32_t srcRate)
        {
            return (frames * kOutputRate + srcRate / 2u) / srcRate;
        }

        // Decodes one 18-byte ADX block (scale + 32 nibbles) into out[0], out[2],
        // ... (stride 2 int16). Bit-exact with the reference CRI/ffmpeg decoder:
        // s = (d*scale << 12 + c0*s1 + c1*s2) >> 12, the history keeps the
        // unclipped value, the output is clipped to int16.
        inline void decodeAdxBlock(const uint8_t *block, int32_t c0, int32_t c1,
                                   int32_t &h1, int32_t &h2, int16_t *out)
        {
            const int32_t scale = static_cast<int32_t>(be16(block));
            int32_t s1 = h1;
            int32_t s2 = h2;
            const uint8_t *nibbles = block + 2;
            for (uint32_t i = 0u; i < 16u; ++i)
            {
                const uint32_t byte = nibbles[i];
                const int32_t dHi = static_cast<int32_t>((byte >> 4) ^ 8u) - 8;
                const int32_t dLo = static_cast<int32_t>((byte & 15u) ^ 8u) - 8;

                int32_t v = dHi * scale + ((c0 * s1 + c1 * s2) >> 12);
                v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
                s2 = s1;
                s1 = v;
                out[(2u * i) * 2u] = static_cast<int16_t>(v);

                v = dLo * scale + ((c0 * s1 + c1 * s2) >> 12);
                v = v > 32767 ? 32767 : (v < -32768 ? -32768 : v);
                s2 = s1;
                s1 = v;
                out[(2u * i + 1u) * 2u] = static_cast<int16_t>(v);
            }
            h1 = s1;
            h2 = s2;
        }
    }

    // ------------------------------------------------------------------ CVM

    bool CvmFileLocator::open(const std::string &cvmPath)
    {
        entries_.clear();
        order_.clear();

        std::FILE *file = std::fopen(cvmPath.c_str(), "rb");
        if (!file)
            return false;

        struct Closer
        {
            std::FILE *f;
            ~Closer() { std::fclose(f); }
        } closer{file};

        uint64_t cvmSize = 0u;
        if (!fileLength(file, cvmSize))
            return false;

        // The ROFS header is followed by an ISO9660 image whose PVD sits at
        // sector 16 of the image; scan for it like p3fes_unpack.open_cvm.
        const uint64_t limit = std::min<uint64_t>(cvmSize, kHeaderScanLimit);
        uint64_t pvdOffset = 0u;
        bool found = false;
        for (uint64_t off = 16u * kSector; off + 6u <= limit; off += kSector)
        {
            uint8_t sig[6];
            if (!readAtOffset(file, off, sig, sizeof(sig)))
                return false;
            if (std::memcmp(sig, "\x01" "CD001", 6u) == 0)
            {
                pvdOffset = off;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
        const uint64_t base = pvdOffset - 16u * kSector;

        uint8_t pvd[190];
        if (!readAtOffset(file, pvdOffset, pvd, sizeof(pvd)))
            return false;
        const uint32_t rootExtent = le32(pvd + 156 + 2);
        const uint32_t rootSize = le32(pvd + 156 + 10);

        std::deque<std::pair<uint32_t, uint32_t>> pending;
        std::set<std::pair<uint32_t, uint32_t>> visited;
        pending.emplace_back(rootExtent, rootSize);
        std::vector<uint8_t> directory;

        while (!pending.empty())
        {
            const auto [lba, size] = pending.front();
            pending.pop_front();
            if (size == 0u || size > kMaxDirectoryBytes || !visited.insert({lba, size}).second)
                continue;
            const uint64_t dirOffset = base + static_cast<uint64_t>(lba) * kSector;
            if (dirOffset + size > cvmSize)
                continue;
            directory.resize(size);
            if (!readAtOffset(file, dirOffset, directory.data(), size))
                return false;

            size_t pos = 0u;
            while (pos < directory.size())
            {
                const uint32_t recLen = directory[pos];
                if (recLen == 0u)
                {
                    pos = (pos / kSector + 1u) * kSector;
                    continue;
                }
                if (recLen < 34u || pos + recLen > directory.size())
                    break;
                const uint8_t *rec = directory.data() + pos;
                pos += recLen;
                const uint32_t extent = le32(rec + 2);
                const uint32_t length = le32(rec + 10);
                const uint8_t flags = rec[25];
                const uint32_t nameLen = rec[32];
                if (33u + nameLen > recLen)
                    break;
                if (nameLen == 1u && (rec[33] == 0u || rec[33] == 1u))
                    continue;
                std::string name(reinterpret_cast<const char *>(rec + 33), nameLen);
                const size_t semi = name.find(';');
                if (semi != std::string::npos)
                    name.resize(semi);
                name = upperAscii(std::move(name));

                if (flags & 0x02u)
                {
                    pending.emplace_back(extent, length);
                }
                else
                {
                    Entry entry;
                    entry.offset = base + static_cast<uint64_t>(extent) * kSector;
                    entry.size = length;
                    if (entries_.emplace(name, entry).second)
                        order_.push_back(name);
                }
            }
        }
        return !entries_.empty();
    }

    bool CvmFileLocator::find(const std::string &name, uint64_t &offset, uint64_t &size) const
    {
        const auto it = entries_.find(upperAscii(name));
        if (it == entries_.end())
            return false;
        offset = it->second.offset;
        size = it->second.size;
        return true;
    }

    std::vector<std::string> CvmFileLocator::names() const
    {
        return order_;
    }

    std::shared_ptr<const CvmFileLocator> CvmFileLocator::shared(const std::string &cvmPath)
    {
        static std::mutex mutex;
        static std::map<std::string, std::shared_ptr<const CvmFileLocator>> cache;
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = cache.find(cvmPath);
        if (it != cache.end())
            return it->second;
        auto locator = std::make_shared<CvmFileLocator>();
        if (!locator->open(cvmPath))
            return nullptr;
        cache.emplace(cvmPath, locator);
        return locator;
    }

    // ------------------------------------------------------------------ ADX

    AdxStreamReader::~AdxStreamReader()
    {
        close();
    }

    void AdxStreamReader::close()
    {
        if (file_)
        {
            std::fclose(file_);
            file_ = nullptr;
        }
        metadata_ = {};
    }

    bool AdxStreamReader::open(const std::string &cvmPath, const std::string &trackName,
                               bool resampleTo48k)
    {
        close();
        const std::shared_ptr<const CvmFileLocator> locator = CvmFileLocator::shared(cvmPath);
        if (!locator)
            return false;
        uint64_t offset = 0u;
        uint64_t size = 0u;
        if (!locator->find(trackName, offset, size))
            return false;
        return openRange(cvmPath, offset, size, resampleTo48k);
    }

    bool AdxStreamReader::openRange(const std::string &path, uint64_t offset, uint64_t size,
                                    bool resampleTo48k)
    {
        close();
        file_ = std::fopen(path.c_str(), "rb");
        if (!file_)
            return false;
        fileBase_ = offset;
        if (size == 0u)
        {
            uint64_t total = 0u;
            if (!fileLength(file_, total) || total <= offset)
            {
                close();
                return false;
            }
            size = total - offset;
        }
        fileSize_ = size;
        if (!parseHeader(size, resampleTo48k))
        {
            close();
            return false;
        }
        return true;
    }

    bool AdxStreamReader::parseHeader(uint64_t size, bool resampleTo48k)
    {
        uint8_t h[0x40];
        if (size < sizeof(h) || !readAtOffset(file_, fileBase_, h, sizeof(h)))
            return false;
        if (be16(h) != 0x8000u)
            return false;
        const uint32_t headerSize = static_cast<uint32_t>(be16(h + 2)) + 4u;
        const uint32_t encoding = h[4];
        const uint32_t frameSize = h[5];
        const uint32_t bits = h[6];
        const uint32_t channels = h[7];
        const uint32_t sampleRate = be32(h + 8);
        const uint32_t sampleCount = be32(h + 12);
        const uint32_t cutoff = be16(h + 16);
        const uint32_t version = h[0x12];
        const uint32_t flags = h[0x13];

        // Only clear-text type-3 (fixed coefficient) ADPCM, versions 3 and 4.
        if (encoding != 3u || frameSize != kBlockBytes || bits != 4u || flags != 0u)
            return false;
        if (version != 3u && version != 4u)
            return false;
        if (channels != 1u && channels != 2u)
            return false;
        if (sampleRate == 0u || sampleCount == 0u || headerSize > size)
            return false;

        hasLoop_ = false;
        loopStart_ = loopEnd_ = 0u;
        const uint32_t loopOffset = (version == 3u) ? 0x14u
                                                    : 0x18u + (channels > 1u ? 4u * channels : 8u);
        if (headerSize >= 6u + loopOffset + 0x18u && loopOffset + 0x18u <= sizeof(h))
        {
            const uint32_t loopFlag = be32(h + loopOffset + 4u);
            if (loopFlag != 0u)
            {
                const uint32_t start = be32(h + loopOffset + 8u);
                const uint32_t end = be32(h + loopOffset + 16u);
                if (!(start < end && end <= sampleCount))
                    return false;
                hasLoop_ = true;
                loopStart_ = start;
                loopEnd_ = end;
            }
        }

        // Highpass coefficients, same derivation as the CRI/ffmpeg decoders.
        {
            const double a = std::sqrt(2.0) - std::cos(2.0 * 3.14159265358979323846 *
                                                         static_cast<double>(cutoff) /
                                                         static_cast<double>(sampleRate));
            const double b = std::sqrt(2.0) - 1.0;
            const double c = (a - std::sqrt((a + b) * (a - b))) / b;
            coef0_ = static_cast<int32_t>(std::lround(c * 8192.0));
            coef1_ = static_cast<int32_t>(std::lround(c * c * -4096.0)); // round to nearest, as ffmpeg
        }

        srcRate_ = sampleRate;
        srcChannels_ = channels;
        srcFrames_ = sampleCount;
        dataOffset_ = headerSize;
        frameBytes_ = channels * kBlockBytes;
        loopBlock_ = hasLoop_ ? (loopStart_ / kBlockSamples) : 0u;

        const uint64_t neededSamples = hasLoop_ ? loopEnd_ : srcFrames_;
        const uint64_t neededBlocks = (neededSamples + kBlockSamples - 1u) / kBlockSamples;
        const uint64_t fullBlocks = (static_cast<uint64_t>(srcFrames_) + kBlockSamples - 1u) / kBlockSamples;
        if (headerSize + neededBlocks * frameBytes_ > size)
            return false;
        regionEnd_ = std::min<uint64_t>(size, headerSize + fullBlocks * frameBytes_);

        // Decoder state
        std::memset(hist_, 0, sizeof(hist_));
        std::memset(loopHist_, 0, sizeof(loopHist_));
        loopHistValid_ = false;
        curBlock_ = 0u;
        blkIndex_ = 0u;
        blkValid_ = false;
        srcPos_ = 0u;
        sourceEnded_ = false;
        failed_ = false;
        chunk_.assign(kChunkBytes, 0u);
        chunkPos_ = chunkLen_ = 0u;
        if (!seekData(dataOffset_))
            return false;

        // Output stage
        resample_ = resampleTo48k && srcRate_ != kOutputRate;
        const uint32_t outRate = resampleTo48k ? kOutputRate : srcRate_;
        outPos_ = 0u;
        posInt_ = 0;
        posNum_ = 0u;
        bufBase_ = -1;
        bufCount_ = 0u;
        bufStarted_ = false;
        bufEnded_ = false;
        if (resample_)
        {
            stepInt_ = srcRate_ / kOutputRate;
            stepNum_ = srcRate_ % kOutputRate;
            srcBuf_.assign(static_cast<size_t>(kSrcChunkFrames + kSrcPadFrames) * 2u, 0);
        }
        else
        {
            stepInt_ = stepNum_ = 0u;
            srcBuf_.clear();
        }

        auto toOut = [&](uint64_t frames) {
            return resample_ ? roundRate(frames, srcRate_) : frames;
        };
        metadata_ = {};
        metadata_.sampleRate = outRate;
        metadata_.channels = 2u;
        metadata_.bitsPerSample = 16u;
        metadata_.blockAlign = 4u;
        metadata_.dataOffset = fileBase_ + dataOffset_;
        metadata_.dataBytes = toOut(srcFrames_) * 4u;
        metadata_.totalFrames = toOut(srcFrames_);
        metadata_.hasLoop = hasLoop_;
        metadata_.loopStart = hasLoop_ ? toOut(loopStart_) : 0u;
        metadata_.loopEndExclusive = hasLoop_ ? toOut(loopEnd_) : 0u;
        return true;
    }

    bool AdxStreamReader::seekData(uint64_t rel)
    {
        chunkPos_ = chunkLen_ = 0u;
        if (P3FES_FSEEK64(file_, fileBase_ + rel, SEEK_SET) != 0)
            return false;
        filePos_ = rel;
        return true;
    }

    bool AdxStreamReader::readBytes(uint8_t *dst, uint32_t count)
    {
        if (chunkLen_ - chunkPos_ < count)
        {
            const uint32_t remaining = chunkLen_ - chunkPos_;
            if (remaining != 0u)
                std::memmove(chunk_.data(), chunk_.data() + chunkPos_, remaining);
            chunkPos_ = 0u;
            chunkLen_ = remaining;
            const uint64_t available = regionEnd_ > filePos_ ? regionEnd_ - filePos_ : 0u;
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(available, kChunkBytes - remaining));
            const size_t got = want ? std::fread(chunk_.data() + remaining, 1u, want, file_) : 0u;
            chunkLen_ += static_cast<uint32_t>(got);
            filePos_ += got;
            if (chunkLen_ < count)
                return false;
        }
        std::memcpy(dst, chunk_.data() + chunkPos_, count);
        chunkPos_ += count;
        return true;
    }

    bool AdxStreamReader::decodeBlock()
    {
        uint8_t raw[2u * kBlockBytes];
        if (!readBytes(raw, frameBytes_))
            return false;
        if (hasLoop_ && !loopHistValid_ && curBlock_ == loopBlock_)
        {
            std::memcpy(loopHist_, hist_, sizeof(hist_));
            loopHistValid_ = true;
        }
        decodeAdxBlock(raw, coef0_, coef1_, hist_[0][0], hist_[0][1], blk_);
        if (srcChannels_ == 2u)
        {
            decodeAdxBlock(raw + kBlockBytes, coef0_, coef1_, hist_[1][0], hist_[1][1], blk_ + 1);
        }
        else
        {
            for (uint32_t i = 0u; i < kBlockSamples; ++i)
                blk_[i * 2u + 1u] = blk_[i * 2u];
        }
        blkIndex_ = curBlock_;
        blkValid_ = true;
        ++curBlock_;
        return true;
    }

    uint32_t AdxStreamReader::decodeFrames(int16_t *out, uint32_t count)
    {
        uint32_t done = 0u;
        const uint64_t boundary = hasLoop_ ? loopEnd_ : srcFrames_;
        while (done < count)
        {
            if (srcPos_ >= boundary)
            {
                if (!hasLoop_)
                {
                    sourceEnded_ = true;
                    break;
                }
                if (!loopHistValid_ ||
                    !seekData(dataOffset_ + static_cast<uint64_t>(loopBlock_) * frameBytes_))
                {
                    failed_ = true;
                    break;
                }
                std::memcpy(hist_, loopHist_, sizeof(hist_));
                curBlock_ = loopBlock_;
                srcPos_ = loopStart_;
                blkValid_ = false;
                continue;
            }
            const uint32_t block = static_cast<uint32_t>(srcPos_ / kBlockSamples);
            const uint32_t offset = static_cast<uint32_t>(srcPos_ % kBlockSamples);
            if (!blkValid_ || blkIndex_ != block)
            {
                if (block != curBlock_ || !decodeBlock())
                {
                    failed_ = true;
                    break;
                }
            }
            const uint64_t left = boundary - srcPos_;
            uint32_t take = kBlockSamples - offset;
            if (take > count - done)
                take = count - done;
            if (take > left)
                take = static_cast<uint32_t>(left);
            std::memcpy(out + static_cast<size_t>(done) * 2u, blk_ + static_cast<size_t>(offset) * 2u,
                        static_cast<size_t>(take) * 2u * sizeof(int16_t));
            done += take;
            srcPos_ += take;
        }
        return done;
    }

    bool AdxStreamReader::refillSource()
    {
        uint32_t keep = 0u;
        if (!bufStarted_)
        {
            // Frame -1 (before the first sample) repeats frame 0.
            const uint32_t want = kSrcChunkFrames;
            const uint32_t got = decodeFrames(srcBuf_.data() + 2, want);
            if (got == 0u)
                return false;
            srcBuf_[0] = srcBuf_[2];
            srcBuf_[1] = srcBuf_[3];
            bufBase_ = -1;
            bufCount_ = got + 1u;
            bufStarted_ = true;
            if (got < want)
            {
                if (hasLoop_ && failed_)
                    return false;
                goto pad;
            }
            return true;
        }
        else
        {
            const int64_t pos = posInt_;
            int64_t drop = pos - 1 - bufBase_;
            if (drop < 0)
                drop = 0;
            if (drop > static_cast<int64_t>(bufCount_))
                drop = bufCount_;
            keep = bufCount_ - static_cast<uint32_t>(drop);
            if (drop > 0)
            {
                std::memmove(srcBuf_.data(), srcBuf_.data() + static_cast<size_t>(drop) * 2u,
                             static_cast<size_t>(keep) * 2u * sizeof(int16_t));
                bufBase_ += drop;
                bufCount_ = keep;
            }
            const uint32_t want = kSrcChunkFrames - std::min<uint32_t>(keep, kSrcChunkFrames - 1u);
            const uint32_t got = decodeFrames(srcBuf_.data() + static_cast<size_t>(bufCount_) * 2u, want);
            bufCount_ += got;
            if (got < want)
            {
                if (hasLoop_ && failed_)
                    return false;
                goto pad;
            }
            return true;
        }
    pad:
        // End of a non-looping source: repeat the last frame so the final
        // output frames can still be interpolated.
        {
            const int16_t l = srcBuf_[static_cast<size_t>(bufCount_ - 1u) * 2u];
            const int16_t r = srcBuf_[static_cast<size_t>(bufCount_ - 1u) * 2u + 1u];
            for (uint32_t i = 0u; i < kSrcPadFrames; ++i)
            {
                srcBuf_[static_cast<size_t>(bufCount_ + i) * 2u] = l;
                srcBuf_[static_cast<size_t>(bufCount_ + i) * 2u + 1u] = r;
            }
            bufCount_ += kSrcPadFrames;
            bufEnded_ = true;
        }
        return true;
    }

    bool AdxStreamReader::resampleFrames(int16_t *dst, uint32_t capacity, uint32_t &produced)
    {
        const uint64_t total = metadata_.totalFrames;
        const bool exactDouble = (stepInt_ == 0u && stepNum_ * 2u == kOutputRate);
        while (produced < capacity && (hasLoop_ || outPos_ < total))
        {
            const int64_t local = posInt_ - bufBase_;
            if (!bufStarted_ || local + 2 >= static_cast<int64_t>(bufCount_))
            {
                if (bufEnded_)
                    break;
                if (!refillSource())
                    return false;
                continue;
            }
            const uint32_t frac = posNum_;
            const int16_t *s = srcBuf_.data() + static_cast<size_t>(local) * 2u;
            int16_t *o = dst + static_cast<size_t>(produced) * 2u;
            if (frac == 0u)
            {
                o[0] = s[0];
                o[1] = s[1];
            }
            else if (exactDouble)
            {
                // Catmull-Rom at t = 1/2: (-1, 9, 9, -1) / 16.
                o[0] = clip16((9 * (s[0] + s[2]) - s[-2] - s[4] + 8) >> 4);
                o[1] = clip16((9 * (s[1] + s[3]) - s[-1] - s[5] + 8) >> 4);
            }
            else
            {
                const float t = static_cast<float>(frac) * (1.0f / static_cast<float>(kOutputRate));
                const float t2 = t * t;
                const float w0 = t * (-0.5f + t * (1.0f - 0.5f * t));
                const float w1 = 1.0f + t2 * (-2.5f + 1.5f * t);
                const float w2 = t * (0.5f + t * (2.0f - 1.5f * t));
                const float w3 = t2 * (-0.5f + 0.5f * t);
                const float l = w0 * s[-2] + w1 * s[0] + w2 * s[2] + w3 * s[4];
                const float r = w0 * s[-1] + w1 * s[1] + w2 * s[3] + w3 * s[5];
                o[0] = clip16(static_cast<int32_t>(l + 65536.5f) - 65536);
                o[1] = clip16(static_cast<int32_t>(r + 65536.5f) - 65536);
            }
            posInt_ += stepInt_;
            posNum_ += stepNum_;
            if (posNum_ >= kOutputRate)
            {
                posNum_ -= kOutputRate;
                ++posInt_;
            }
            ++outPos_;
            ++produced;
        }
        return true;
    }

    bool AdxStreamReader::readFrames(int16_t *destination, uint32_t frameCapacity,
                                     uint32_t &framesRead, bool &finished)
    {
        framesRead = 0u;
        finished = false;
        if (!file_ || !destination || frameCapacity == 0u)
            return false;

        if (!resample_)
        {
            framesRead = decodeFrames(destination, frameCapacity);
            outPos_ += framesRead;
            if (hasLoop_)
                return !failed_;
            finished = sourceEnded_ || failed_ || srcPos_ >= srcFrames_;
            return true;
        }

        if (!resampleFrames(destination, frameCapacity, framesRead))
            return false;
        if (!hasLoop_ && outPos_ >= metadata_.totalFrames)
            finished = true;
        return true;
    }
}
