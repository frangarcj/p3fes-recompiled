// Host test for p3fes_adx_stream. Build (from the repository root):
//   clang++ -std=c++17 -O2 -Wall -Wextra \
//     -I ps2xRuntime/src/lib/game_overrides \
//     ps2xRuntime/src/lib/game_overrides/p3fes_adx_stream.cpp \
//     ps2xRuntime/src/lib/game_overrides/tests/p3fes_adx_stream_test.cpp -o adx_test
// Run:
//   adx_test <BGM.CVM> <ffmpeg> <work dir> [--all] [track ...]
// Tracks are names such as 53.ADX. With no track the default set is used;
// --all also tests every one of the CVM's ADX files at source rate.

#include "p3fes_adx_stream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using p3fes_native_bgm::detail::AdxStreamReader;
using p3fes_native_bgm::detail::CvmFileLocator;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what, const std::string &track)
    {
        std::printf("  [%s] %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok)
        {
            ++g_failures;
            std::printf("    ^ track %s\n", track.c_str());
        }
    }

    bool readFileRange(const std::string &path, uint64_t offset, uint64_t size, std::vector<uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        out.resize(size);
        const bool ok = fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0 &&
                        std::fread(out.data(), 1, size, f) == size;
        std::fclose(f);
        return ok;
    }

    bool writeFile(const std::string &path, const std::vector<uint8_t> &data)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
        std::fclose(f);
        return ok;
    }

    // Decodes with ffmpeg to raw s16le at the file's native rate and channel count.
    bool ffmpegDecode(const std::string &ffmpeg, const std::string &adxPath, std::vector<int16_t> &out)
    {
        const std::string cmd = "\"" + ffmpeg + "\" -v error -i \"" + adxPath + "\" -f s16le -acodec pcm_s16le -";
        std::FILE *pipe = popen(cmd.c_str(), "r");
        if (!pipe)
            return false;
        out.clear();
        std::vector<int16_t> buf(65536);
        size_t n;
        while ((n = std::fread(buf.data(), sizeof(int16_t), buf.size(), pipe)) > 0)
            out.insert(out.end(), buf.begin(), buf.begin() + n);
        return pclose(pipe) == 0;
    }

    inline int16_t clip16(int v) { return static_cast<int16_t>(std::max(-32768, std::min(32767, v))); }

    // Reads `total` frames in irregular chunk sizes. Returns false on reader failure.
    bool readIrregular(AdxStreamReader &r, uint64_t total, std::vector<int16_t> &out, bool &finished)
    {
        static const uint32_t sizes[] = {1024, 977, 4096, 31, 2048, 3, 8191};
        out.assign(total * 2u, 0);
        uint64_t got = 0;
        size_t si = 0;
        finished = false;
        while (got < total)
        {
            const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(sizes[si++ % 7], total - got));
            uint32_t n = 0;
            if (!r.readFrames(out.data() + got * 2u, want, n, finished))
                return false;
            got += n;
            if (finished)
                break;
            if (n < want)
                return false; // short read without finishing
        }
        out.resize(got * 2u);
        return true;
    }

    // Independent reference upsampler (2x Catmull-Rom, integer) over an unrolled
    // stereo stream `u` (frames); `n` output frames; u[-1]=u[0], u[>=len]=u[len-1].
    void refUpsample2x(const std::vector<int16_t> &u, uint64_t n, std::vector<int16_t> &out)
    {
        const int64_t len = static_cast<int64_t>(u.size() / 2);
        auto at = [&](int64_t i, int c) {
            i = std::max<int64_t>(0, std::min<int64_t>(len - 1, i));
            return static_cast<int>(u[static_cast<size_t>(i) * 2 + c]);
        };
        out.resize(n * 2);
        for (uint64_t k = 0; k < n; ++k)
        {
            const int64_t p = static_cast<int64_t>(k / 2);
            for (int c = 0; c < 2; ++c)
            {
                int v;
                if (k % 2 == 0)
                    v = at(p, c);
                else
                    v = (-at(p - 1, c) + 9 * at(p, c) + 9 * at(p + 1, c) - at(p + 2, c) + 8) >> 4;
                out[k * 2 + c] = clip16(v);
            }
        }
    }

    // Reference general-ratio Catmull-Rom in double.
    void refUpsampleGeneric(const std::vector<int16_t> &u, uint32_t srcRate, uint64_t n, std::vector<int16_t> &out)
    {
        const int64_t len = static_cast<int64_t>(u.size() / 2);
        auto at = [&](int64_t i, int c) {
            i = std::max<int64_t>(0, std::min<int64_t>(len - 1, i));
            return static_cast<double>(u[static_cast<size_t>(i) * 2 + c]);
        };
        out.resize(n * 2);
        for (uint64_t k = 0; k < n; ++k)
        {
            const uint64_t num = k * srcRate;
            const int64_t p = static_cast<int64_t>(num / 48000u);
            const double t = static_cast<double>(num % 48000u) / 48000.0;
            for (int c = 0; c < 2; ++c)
            {
                const double a = at(p - 1, c), b = at(p, c), cc = at(p + 1, c), d = at(p + 2, c);
                const double v = 0.5 * ((2 * b) + (-a + cc) * t + (2 * a - 5 * b + 4 * cc - d) * t * t +
                                        (-a + 3 * b - 3 * cc + d) * t * t * t);
                out[k * 2 + c] = clip16(static_cast<int>(std::lround(v)));
            }
        }
    }

    std::vector<int16_t> unroll(const std::vector<int16_t> &lin, uint64_t ls, uint64_t le, uint64_t frames)
    {
        std::vector<int16_t> u(frames * 2);
        const uint64_t L = le - ls;
        for (uint64_t i = 0; i < frames; ++i)
        {
            const uint64_t src = i < le ? i : ls + (i - ls) % L;
            u[i * 2] = lin[src * 2];
            u[i * 2 + 1] = lin[src * 2 + 1];
        }
        return u;
    }

    // Compares two interleaved buffers; returns max abs diff and first mismatch frame (-1 if equal).
    int compare(const std::vector<int16_t> &a, const std::vector<int16_t> &b, size_t frames, int64_t &firstMismatch)
    {
        int maxDiff = 0;
        firstMismatch = -1;
        for (size_t i = 0; i < frames * 2; ++i)
        {
            const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
            if (d != 0 && firstMismatch < 0)
                firstMismatch = static_cast<int64_t>(i / 2);
            maxDiff = std::max(maxDiff, d);
        }
        return maxDiff;
    }

    double nowSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct Context
    {
        std::string cvm, ffmpeg, work;
    };

    void testTrack(const Context &ctx, const std::string &track)
    {
        std::printf("== %s\n", track.c_str());
        auto locator = CvmFileLocator::shared(ctx.cvm);
        uint64_t off = 0, size = 0;
        if (!locator || !locator->find(track, off, size))
        {
            check(false, "locate in CVM", track);
            return;
        }
        std::vector<uint8_t> bytes;
        const std::string adxPath = ctx.work + "/" + track;
        if (!readFileRange(ctx.cvm, off, size, bytes) || !writeFile(adxPath, bytes))
        {
            check(false, "extract", track);
            return;
        }

        AdxStreamReader raw;
        if (!raw.open(ctx.cvm, track, false))
        {
            check(false, "open (source rate)", track);
            return;
        }
        const uint32_t sr = raw.sourceSampleRate();
        const uint64_t sc = raw.sourceFrames();
        const uint32_t ch = raw.sourceChannels();
        const bool loop = raw.sourceHasLoop();
        const uint64_t ls = raw.sourceLoopStart(), le = raw.sourceLoopEnd();
        std::printf("  rate=%u ch=%u frames=%llu loop=%d [%llu,%llu)\n", sr, ch, (unsigned long long)sc, loop,
                    (unsigned long long)ls, (unsigned long long)le);

        // 1. Bit exactness against ffmpeg.
        std::vector<int16_t> ff;
        if (!ffmpegDecode(ctx.ffmpeg, adxPath, ff))
        {
            check(false, "ffmpeg decode", track);
            return;
        }
        std::vector<int16_t> lin(sc * 2);
        const uint64_t ffFrames = ff.size() / ch;
        std::printf("  ffmpeg frames=%llu (header %llu)\n", (unsigned long long)ffFrames, (unsigned long long)sc);
        if (ch == 1)
        { // ffmpeg is mono; our reader duplicates
            std::vector<int16_t> dup(ff.size() * 2);
            for (size_t i = 0; i < ff.size(); ++i)
                dup[i * 2] = dup[i * 2 + 1] = ff[i];
            ff.swap(dup);
        }
        const uint64_t lim = loop ? le : sc;
        {
            std::vector<int16_t> mine;
            bool fin = false;
            const bool ok = readIrregular(raw, lim, mine, fin);
            check(ok && mine.size() == lim * 2, "source-rate read of first pass", track);
            if (ok && mine.size() == lim * 2 && ff.size() >= lim * 2)
            {
                int64_t first;
                const int md = compare(mine, ff, lim, first);
                std::printf("  vs ffmpeg: frames compared=%llu max|diff|=%d first mismatch=%lld\n",
                            (unsigned long long)lim, md, (long long)first);
                check(md <= 1, "max diff vs ffmpeg <= 1 LSB", track);
                check(!loop ? fin : true, "finished flag at end (no loop)", track);
                std::copy(mine.begin(), mine.end(), lin.begin());
            }
            else
                check(false, "ffmpeg long enough", track);
        }
        if (ff.size() >= sc * 2)
            std::copy(ff.begin(), ff.begin() + sc * 2, lin.begin()); // linear full decode for references

        // 2. Loop test at source rate: 2.5x track length.
        if (loop)
        {
            AdxStreamReader r;
            r.open(ctx.cvm, track, false);
            const uint64_t total = static_cast<uint64_t>(2.5 * static_cast<double>(le));
            std::vector<int16_t> out;
            bool fin = false;
            const bool ok = readIrregular(r, total, out, fin);
            check(ok && !fin && out.size() == total * 2, "loop: 2.5x read, never finished", track);
            const uint64_t L = le - ls;
            uint64_t bad = 0;
            int64_t firstBad = -1;
            for (uint64_t i = ls; i < total && ok; ++i)
            {
                const uint64_t src = i < le ? i : ls + (i - ls) % L;
                if (out[i * 2] != lin[src * 2] || out[i * 2 + 1] != lin[src * 2 + 1])
                {
                    if (!bad)
                        firstBad = static_cast<int64_t>(i);
                    ++bad;
                }
            }
            std::printf("  loop: %llu mismatching frames of %llu (first %lld)\n", (unsigned long long)bad,
                        (unsigned long long)total, (long long)firstBad);
            check(ok && bad == 0, "loop: exact equality after wrap (source domain)", track);
        }

        // 3. 48 kHz path.
        {
            AdxStreamReader r;
            if (!r.open(ctx.cvm, track, true))
            {
                check(false, "open (48 kHz)", track);
                return;
            }
            const auto &m = r.metadata();
            const uint64_t expected = (sc * 48000ull + sr / 2) / sr;
            const double exactF = static_cast<double>(sc) * 48000.0 / sr;
            check(m.sampleRate == 48000 && m.channels == 2, "metadata 48 kHz stereo", track);
            check(std::llabs(static_cast<long long>(m.totalFrames) - static_cast<long long>(std::llround(exactF))) <= 1,
                  "totalFrames == round(frames*48000/rate) +-1", track);
            std::printf("  48k: totalFrames=%llu expected=%llu loop=%d [%llu,%llu)\n",
                        (unsigned long long)m.totalFrames, (unsigned long long)expected, m.hasLoop,
                        (unsigned long long)m.loopStart, (unsigned long long)m.loopEndExclusive);

            std::vector<int16_t> out;
            bool fin = false;
            const uint64_t want = loop ? static_cast<uint64_t>(2.5 * static_cast<double>(m.loopEndExclusive))
                                       : m.totalFrames + 100;
            const bool ok = readIrregular(r, want, out, fin);
            if (!loop)
            {
                check(ok && fin && out.size() / 2 == m.totalFrames, "non-loop: finished with exactly totalFrames", track);
                std::printf("  48k delivered=%llu\n", (unsigned long long)(out.size() / 2));
            }
            else
                check(ok && !fin && out.size() / 2 == want, "loop: never finished", track);

            // Reference.
            if (ok && (sr == 24000 || sr == 48000))
            {
                std::vector<int16_t> ref;
                const uint64_t n = out.size() / 2;
                std::vector<int16_t> u;
                if (sr == 48000)
                {
                    u = loop ? unroll(lin, ls, le, n) : lin;
                    ref.assign(u.begin(), u.begin() + std::min<size_t>(u.size(), n * 2));
                }
                else
                {
                    const uint64_t srcFrames = n / 2 + 8;
                    u = loop ? unroll(lin, ls, le, srcFrames) : lin;
                    refUpsample2x(u, n, ref);
                }
                int64_t first;
                const int md = compare(out, ref, std::min<size_t>(n, ref.size() / 2), first);
                std::printf("  48k vs reference (unrolled loop + independent resampler): max|diff|=%d first=%lld\n", md,
                            (long long)first);
                check(md == 0, "48 kHz output equals reference exactly (incl. across loop wraps)", track);
            }
        }

        // 4. Generic-ratio path: patch the header rate to 44100 and compare to a
        // double precision reference built from the source-rate decode.
        if (sr == 24000 && ch == 2)
        {
            std::vector<uint8_t> patched = bytes;
            patched[8] = 0x00; patched[9] = 0x00; patched[10] = 0xAC; patched[11] = 0x44; // 44100
            const std::string pPath = ctx.work + "/p44100_" + track;
            writeFile(pPath, patched);
            AdxStreamReader p0, p1;
            if (p0.openRange(pPath, 0, 0, false) && p1.openRange(pPath, 0, 0, true))
            {
                const uint64_t total = loop ? static_cast<uint64_t>(2.5 * static_cast<double>(p1.metadata().loopEndExclusive))
                                            : p1.metadata().totalFrames;
                std::vector<int16_t> out, srcLin;
                bool fin = false;
                const bool ok = readIrregular(p1, total, out, fin);
                bool fin0 = false;
                const uint64_t srcNeed = loop ? static_cast<uint64_t>(2.5 * static_cast<double>(le)) + 16 : sc;
                const bool ok0 = readIrregular(p0, srcNeed, srcLin, fin0);
                if (ok && ok0)
                {
                    std::vector<int16_t> ref;
                    refUpsampleGeneric(srcLin, 44100, out.size() / 2, ref);
                    int64_t first;
                    const size_t n = std::min(out.size(), ref.size()) / 2;
                    const int md = compare(out, ref, n, first);
                    std::printf("  44.1k->48k generic path vs double reference: frames=%zu max|diff|=%d first=%lld\n", n, md,
                                (long long)first);
                    check(md <= 1, "generic resampler within 1 LSB of reference", track);
                }
                else
                    check(false, "generic path read", track);
            }
            else
                check(false, "open patched 44.1k", track);
        }

        // 5. Speed.
        {
            AdxStreamReader r;
            r.open(ctx.cvm, track, true);
            std::vector<int16_t> buf(4096 * 2);
            const uint64_t total = r.metadata().totalFrames;
            uint64_t got = 0;
            const double t0 = nowSeconds();
            while (got < total)
            {
                uint32_t n = 0;
                bool fin = false;
                if (!r.readFrames(buf.data(), 4096, n, fin))
                    break;
                got += n;
                if (fin)
                    break;
            }
            const double dt = nowSeconds() - t0;
            std::printf("  speed (48k path, 4096-frame reads): %llu frames in %.3f s = %.0fx realtime\n",
                        (unsigned long long)got, dt, (static_cast<double>(got) / 48000.0) / dt);
        }
    }

    // Synthetic variants built from a real stereo file: mono (left channel
    // blocks only), and files the reader must refuse.
    void testSynthetic(const Context &ctx, const std::string &track)
    {
        std::printf("== synthetic variants of %s\n", track.c_str());
        auto locator = CvmFileLocator::shared(ctx.cvm);
        uint64_t off = 0, size = 0;
        std::vector<uint8_t> bytes;
        if (!locator || !locator->find(track, off, size) || !readFileRange(ctx.cvm, off, size, bytes))
        {
            check(false, "locate", track);
            return;
        }
        const uint32_t headerSize = ((bytes[2] << 8) | bytes[3]) + 4u;
        std::vector<uint8_t> mono(bytes.begin(), bytes.begin() + headerSize);
        mono[7] = 1;
        for (size_t pos = headerSize; pos + 36 <= bytes.size(); pos += 36)
            mono.insert(mono.end(), bytes.begin() + pos, bytes.begin() + pos + 18);
        const std::string monoPath = ctx.work + "/mono_" + track;
        writeFile(monoPath, mono);
        AdxStreamReader rm, rs;
        std::vector<int16_t> ffm, got, stereo;
        bool fin = false;
        const bool opened = rm.openRange(monoPath, 0, 0, false) && rs.open(ctx.cvm, track, false);
        check(opened, "open synthetic mono", track);
        if (opened && ffmpegDecode(ctx.ffmpeg, monoPath, ffm))
        {
            readIrregular(rm, rm.sourceFrames(), got, fin);
            readIrregular(rs, rs.sourceFrames(), stereo, fin);
            size_t bad = 0, dupBad = 0;
            for (size_t i = 0; i < got.size() / 2 && i < ffm.size(); ++i)
            {
                bad += got[i * 2] != ffm[i] || got[i * 2 + 1] != ffm[i];
                dupBad += got[i * 2] != stereo[i * 2];
            }
            std::printf("  mono: %zu frames, %zu mismatches vs ffmpeg mono, %zu vs stereo left channel\n",
                        got.size() / 2, bad, dupBad);
            check(bad == 0 && dupBad == 0 && got.size() / 2 == rm.sourceFrames(), "mono duplicated to both channels, bit exact", track);
        }

        std::vector<uint8_t> enc = bytes;
        enc[0x13] = 8; // encrypted flag
        writeFile(ctx.work + "/enc_" + track, enc);
        AdxStreamReader r1;
        check(!r1.openRange(ctx.work + "/enc_" + track, 0, 0, false), "encrypted file refused", track);

        std::vector<uint8_t> type2 = bytes;
        type2[4] = 2;
        writeFile(ctx.work + "/t2_" + track, type2);
        AdxStreamReader r2;
        check(!r2.openRange(ctx.work + "/t2_" + track, 0, 0, false), "encoding type != 3 refused", track);

        std::vector<uint8_t> shortFile(bytes.begin(), bytes.begin() + bytes.size() / 2);
        writeFile(ctx.work + "/short_" + track, shortFile);
        AdxStreamReader r3;
        check(!r3.openRange(ctx.work + "/short_" + track, 0, 0, false), "truncated data refused", track);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: %s <BGM.CVM> <ffmpeg> <workdir> [--all] [tracks...]\n", argv[0]);
        return 2;
    }
    Context ctx{argv[1], argv[2], argv[3]};
    std::vector<std::string> tracks;
    bool all = false;
    for (int i = 4; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--all") == 0)
            all = true;
        else
            tracks.push_back(argv[i]);
    }
    if (tracks.empty())
        tracks = {"53.ADX", "115.ADX", "54.ADX", "02.ADX"};

    auto locator = CvmFileLocator::shared(ctx.cvm);
    if (!locator)
    {
        std::fprintf(stderr, "cannot parse CVM\n");
        return 1;
    }
    std::printf("CVM files: %zu\n", locator->fileCount());

    // Open every file: the reader must accept all of them.
    {
        size_t opened = 0, refused = 0;
        for (const std::string &name : locator->names())
        {
            AdxStreamReader r;
            if (r.open(ctx.cvm, name, true))
                ++opened;
            else
            {
                ++refused;
                std::printf("  REFUSED %s\n", name.c_str());
            }
        }
        std::printf("open all: %zu opened, %zu refused\n", opened, refused);
        if (refused)
            ++g_failures;
    }

    if (all)
    {
        // Exactness of every file at source rate.
        int worst = 0;
        std::string worstName;
        size_t n = 0;
        for (const std::string &name : locator->names())
        {
            uint64_t off, size;
            locator->find(name, off, size);
            std::vector<uint8_t> bytes;
            readFileRange(ctx.cvm, off, size, bytes);
            const std::string p = ctx.work + "/all_" + name;
            writeFile(p, bytes);
            std::vector<int16_t> ff;
            AdxStreamReader r;
            if (!r.open(ctx.cvm, name, false) || !ffmpegDecode(ctx.ffmpeg, p, ff))
            {
                std::printf("  %s: open/ffmpeg failed\n", name.c_str());
                ++g_failures;
                continue;
            }
            const uint64_t lim = r.sourceHasLoop() ? r.sourceLoopEnd() : r.sourceFrames();
            std::vector<int16_t> mine;
            bool fin = false;
            readIrregular(r, lim, mine, fin);
            const size_t frames = std::min<size_t>(lim, ff.size() / r.sourceChannels());
            int64_t first;
            std::vector<int16_t> ffs = ff;
            if (r.sourceChannels() == 1)
            {
                ffs.assign(ff.size() * 2, 0);
                for (size_t i = 0; i < ff.size(); ++i)
                    ffs[i * 2] = ffs[i * 2 + 1] = ff[i];
            }
            const int md = compare(mine, ffs, frames, first);
            if (md > worst)
            {
                worst = md;
                worstName = name;
            }
            if (md > 1 || frames < lim)
            {
                std::printf("  %s: max diff %d first %lld frames %zu/%llu\n", name.c_str(), md, (long long)first, frames,
                            (unsigned long long)lim);
                ++g_failures;
            }
            std::remove(p.c_str());
            ++n;
        }
        std::printf("ALL: %zu tracks compared with ffmpeg at source rate, worst max|diff|=%d (%s)\n", n, worst,
                    worstName.c_str());
    }

    for (const std::string &t : tracks)
        testTrack(ctx, t);
    testSynthetic(ctx, "02.ADX");

    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
