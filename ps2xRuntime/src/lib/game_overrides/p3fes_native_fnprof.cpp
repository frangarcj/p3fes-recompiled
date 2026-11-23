#include "p3fes_native_fnprof.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <array>
#include <string>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <thread>
#include <vector>
#if defined(PLATFORM_VITA)
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#endif

// See p3fes_native_fnprof.h. A measurement tool: it is only installed when P3FES_FN_PROF is set.
namespace p3fes_native_fnprof
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;
        constexpr int kMax = 80;
        constexpr int kDepth = 48;

#if defined(PLATFORM_VITA)
        extern "C" __attribute__((weak)) int ps2x_vita_pmu_read_fast(uint32_t *instr, uint32_t *cycles);
#endif
        struct Stamp
        {
            uint32_t ins, cyc;
        };
        inline Stamp now()
        {
            Stamp s{0u, 0u};
#if defined(PLATFORM_VITA)
            if (ps2x_vita_pmu_read_fast && ps2x_vita_pmu_read_fast(&s.ins, &s.cyc))
                return s;
            return s;
#else
            const uint32_t ns = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            s.ins = ns;
            s.cyc = ns;
            return s;
#endif
        }

        struct Acc
        {
            uint64_t entered = 0u, wrongPc = 0u, incomplete = 0u, calls = 0u, inclIns = 0u, inclCyc = 0u, exclIns = 0u, exclCyc = 0u;
        };
        Acc g_acc[kMax];
        uint32_t g_addr[kMax];
        GuestFunction g_orig[kMax];
        int g_count = 0;
        uint32_t g_frameAddr = 0x001983A0u, g_every = 150u, g_top = 40u;
        uint64_t g_frames = 0u;
        uint32_t g_stampIns = 0u, g_stampCyc = 0u;
        // children's inclusive cost, per depth
        uint64_t g_childIns[kDepth + 2], g_childCyc[kDepth + 2];
        int g_depth = 0;
        bool g_tracing = false;

        GuestFunction g_stubs[kMax];
        void report(PS2Runtime *runtime)
        {
            {
                // measurement sanity: wrappers some other module has replaced since install are not seeing any call
                uint32_t intact = 0u;
                for (int i = 0; i < g_count; ++i)
                    intact += runtime->lookupFunction(g_addr[i]) == g_stubs[i] ? 1u : 0u;
                if (intact != static_cast<uint32_t>(g_count))
                    std::fprintf(stderr, "[fnprof] WARNING: only %u of %d wrappers are still in the function table\n", intact, g_count);
            }
            const double f = static_cast<double>(g_every);
            int order[kMax];
            for (int i = 0; i < g_count; ++i)
                order[i] = i;
            std::sort(order, order + g_count, [](int a, int b) { return g_acc[a].exclIns != g_acc[b].exclIns ? g_acc[a].exclIns > g_acc[b].exclIns : g_acc[a].entered > g_acc[b].entered; });
            double sumIns = 0.0, sumCyc = 0.0;
            for (int i = 0; i < g_count; ++i)
            {
                sumIns += static_cast<double>(g_acc[i].exclIns);
                sumCyc += static_cast<double>(g_acc[i].exclCyc);
            }
            std::fprintf(stderr, "[fnprof] frames=%llu window=%u stamp=%u/%u instr/cycles; sum of exclusive over the wrapped functions %.0fk instr %.0fk cyc per frame\n",
                         static_cast<unsigned long long>(g_frames), g_every, g_stampIns, g_stampCyc, sumIns / f / 1e3, sumCyc / f / 1e3);
            for (uint32_t k = 0u; k < g_top && static_cast<int>(k) < g_count; ++k)
            {
                const int i = order[k];
                const Acc &a = g_acc[i];
                if (a.calls == 0u && a.entered == 0u && a.wrongPc == 0u)
                    break;
                std::fprintf(stderr, "[fnprof] %08X calls/f=%.2f incl=%.1fk/%.1fk excl=%.1fk/%.1fk (instr/cycles per frame) per call excl=%.0f/%.0f entered=%llu wrongPc=%llu incomplete=%llu\n", g_addr[i],
                             static_cast<double>(a.calls) / f, static_cast<double>(a.inclIns) / f / 1e3, static_cast<double>(a.inclCyc) / f / 1e3,
                             static_cast<double>(a.exclIns) / f / 1e3, static_cast<double>(a.exclCyc) / f / 1e3,
                             static_cast<double>(a.exclIns) / static_cast<double>(a.calls ? a.calls : 1u), static_cast<double>(a.exclCyc) / static_cast<double>(a.calls ? a.calls : 1u),
                             (unsigned long long)a.entered, (unsigned long long)a.wrongPc, (unsigned long long)a.incomplete);
            }
            for (int i = 0; i < g_count; ++i)
                g_acc[i] = {};
        }

        template <int I>
        void stub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != g_addr[I])
            {
                ++g_acc[I].wrongPc;
                g_orig[I](rdram, ctx, runtime);
                return;
            }
            const uint32_t ra = getRegU32(ctx, 31);
            if (++g_acc[I].entered == 1u && g_acc[I].calls == 0u && g_tracing)
                std::fprintf(stderr, "[fnprof] first entry %08X ra=%08X\n", g_addr[I], ra);
            const int depth = g_depth < kDepth ? g_depth : kDepth;
            g_childIns[depth + 1] = 0u;
            g_childCyc[depth + 1] = 0u;
            ++g_depth;
            // an unwound call (blocking syscall) leaves through an exception: the depth must come back either way
            struct DepthRestore
            {
                int value;
                ~DepthRestore() { g_depth = value; }
            } restore{depth};
            const Stamp t0 = now();
            g_orig[I](rdram, ctx, runtime);
            const Stamp t1 = now();
            if (ctx->pc != ra)
            {
                ++g_acc[I].incomplete;
                return; // suspended or unwound: the call did not complete here
            }
            // the second stamp's own cost is inside [t0, t1]
            const uint64_t inIns = static_cast<uint32_t>(t1.ins - t0.ins) > g_stampIns ? static_cast<uint32_t>(t1.ins - t0.ins) - g_stampIns : 0u;
            const uint64_t inCyc = static_cast<uint32_t>(t1.cyc - t0.cyc) > g_stampCyc ? static_cast<uint32_t>(t1.cyc - t0.cyc) - g_stampCyc : 0u;
            const uint64_t chIns = g_childIns[depth + 1], chCyc = g_childCyc[depth + 1];
            Acc &a = g_acc[I];
            ++a.calls;
            a.inclIns += inIns;
            a.inclCyc += inCyc;
            a.exclIns += inIns > chIns ? inIns - chIns : 0u;
            a.exclCyc += inCyc > chCyc ? inCyc - chCyc : 0u;
            g_childIns[depth] += inIns;
            g_childCyc[depth] += inCyc;
            if (g_addr[I] == g_frameAddr && ++g_frames % g_every == 0u)
                report(runtime);
        }

        template <size_t... Is>
        constexpr std::array<GuestFunction, sizeof...(Is)> makeStubs(std::index_sequence<Is...>)
        {
            return {&stub<static_cast<int>(Is)>...};
        }
    }


    namespace
    {
        // P3FES_IO_BENCH=<path of a big archive> (diagnostic): 20 s after boot a detached thread times seek+read of 256 KB
        // blocks at pseudo-random offsets of the file, to tell a slow seek (cluster-chain walk of the FAT driver) from slow
        // reads: same descriptor / fresh descriptor per read / ascending / descending / repeat / sequential throughput.
        // One [io-bench] line per test; the game keeps running, so run it on a title screen.
        uint64_t benchNowUs()
        {
#if defined(PLATFORM_VITA)
            return sceKernelGetProcessTimeWide();
#else
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
        }
        struct BenchFile
        {
#if defined(PLATFORM_VITA)
            SceUID fd = -1;
            bool open(const char *p) { fd = sceIoOpen(p, SCE_O_RDONLY, 0); return fd >= 0; }
            bool seek(uint64_t o) { return sceIoLseek(fd, static_cast<SceOff>(o), SCE_SEEK_SET) == static_cast<SceOff>(o); }
            int64_t read(void *b, size_t n) { return sceIoRead(fd, b, n); }
            void close() { if (fd >= 0) sceIoClose(fd); fd = -1; }
            static uint64_t sizeOf(const char *p) { SceUID f = sceIoOpen(p, SCE_O_RDONLY, 0); if (f < 0) return 0; const SceOff e = sceIoLseek(f, 0, SCE_SEEK_END); sceIoClose(f); return e < 0 ? 0u : static_cast<uint64_t>(e); }
#else
            std::FILE *f = nullptr;
            bool open(const char *p) { f = std::fopen(p, "rb"); return f != nullptr; }
            bool seek(uint64_t o) { return fseeko(f, static_cast<off_t>(o), SEEK_SET) == 0; }
            int64_t read(void *b, size_t n) { return static_cast<int64_t>(std::fread(b, 1, n, f)); }
            void close() { if (f) std::fclose(f); f = nullptr; }
            static uint64_t sizeOf(const char *p) { std::FILE *f = std::fopen(p, "rb"); if (!f) return 0; fseeko(f, 0, SEEK_END); const off_t e = ftello(f); std::fclose(f); return e < 0 ? 0u : static_cast<uint64_t>(e); }
#endif
        };
        void benchOne(const std::string &path);
        void benchThread(std::string paths)
        {
#if defined(PLATFORM_VITA)
            sceKernelDelayThread(20u * 1000u * 1000u);
#else
            std::this_thread::sleep_for(std::chrono::seconds(20));
#endif
            size_t pos = 0u;
            while (pos <= paths.size())
            {
                size_t comma = paths.find(',', pos);
                if (comma == std::string::npos) comma = paths.size();
                if (comma > pos) benchOne(paths.substr(pos, comma - pos));
                pos = comma + 1u;
            }
            std::fprintf(stderr, "[io-bench] done\n");
        }
        void benchOne(const std::string &path)
        {
            const uint64_t size = BenchFile::sizeOf(path.c_str());
            constexpr size_t kBlock = 256u * 1024u;
            std::fprintf(stderr, "[io-bench] file %s size %llu MB\n", path.c_str(), static_cast<unsigned long long>(size >> 20));
            if (size < 64u * kBlock) { std::fprintf(stderr, "[io-bench] %s: size %llu, nothing to do\n", path.c_str(), static_cast<unsigned long long>(size)); return; }
            std::vector<uint8_t> buffer(kBlock);
            std::vector<uint64_t> offsets;
            uint64_t x = 0x9E3779B97F4A7C15ull;
            for (int i = 0; i < 24; ++i) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; offsets.push_back((x % ((size - kBlock) / 2048u)) * 2048u); }
            auto run = [&](const char *name, const std::vector<uint64_t> &list, bool fresh)
            {
                BenchFile file;
                if (!fresh && !file.open(path.c_str())) return;
                double seekMs = 0.0, readMs = 0.0, worst = 0.0, total = 0.0;
                std::string detail;
                for (uint64_t offset : list)
                {
                    if (fresh && !file.open(path.c_str())) return;
                    const uint64_t t0 = benchNowUs();
                    file.seek(offset);
                    const uint64_t t1 = benchNowUs();
                    file.read(buffer.data(), kBlock);
                    const uint64_t t2 = benchNowUs();
                    if (fresh) file.close();
                    seekMs += (t1 - t0) / 1000.0; readMs += (t2 - t1) / 1000.0;
                    worst = std::max(worst, (t2 - t0) / 1000.0);
                    if (detail.size() < 400) { char b[48]; std::snprintf(b, sizeof b, " %.0f/%.0f", (t1 - t0) / 1000.0, (t2 - t1) / 1000.0); detail += b; }
                }
                file.close();
                total = seekMs + readMs;
                std::fprintf(stderr, "[io-bench] %-22s n=%u total=%.0f ms (seek %.0f read %.0f) avg=%.1f worst=%.1f | seek/read ms:%s\n", name, static_cast<unsigned>(list.size()), total, seekMs, readMs,
                             total / list.size(), worst, detail.c_str());
            };
            std::vector<uint64_t> asc = offsets, desc;
            std::sort(asc.begin(), asc.end());
            desc.assign(asc.rbegin(), asc.rend());
            run("random same-fd", offsets, false);
            run("random fresh-fd", offsets, true);
            run("random same-fd repeat", offsets, false);
            run("ascending same-fd", asc, false);
            run("descending same-fd", desc, false);
            {
                std::vector<uint64_t> seq;
                for (int i = 0; i < 32; ++i) seq.push_back(size / 3 + static_cast<uint64_t>(i) * kBlock);
                run("sequential 8 MB", seq, false);
            }
        }
    }

    bool install(PS2Runtime &runtime)
    {
        if (const char *b = std::getenv("P3FES_IO_BENCH"); b && *b)
            std::thread(benchThread, std::string(b)).detach();
        const char *list = std::getenv("P3FES_FN_PROF");
        if (!list || !*list)
            return false;
        if (const char *e = std::getenv("P3FES_FN_PROF_FRAME"); e && *e)
            g_frameAddr = static_cast<uint32_t>(std::strtoul(e, nullptr, 16));
        if (const char *e = std::getenv("P3FES_FN_PROF_EVERY"); e && *e)
            g_every = std::max<uint32_t>(1u, static_cast<uint32_t>(std::strtoul(e, nullptr, 10)));
        if (const char *e = std::getenv("P3FES_FN_PROF_TOP"); e && *e)
            g_top = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        g_tracing = std::getenv("P3FES_FN_PROF_TRACE") != nullptr;
        static const auto stubs = makeStubs(std::make_index_sequence<kMax>{});
        {
            uint64_t si = 0u, sc = 0u;
            for (int i = 0; i < 200; ++i)
            {
                const Stamp a = now(), b = now();
                si += static_cast<uint32_t>(b.ins - a.ins);
                sc += static_cast<uint32_t>(b.cyc - a.cyc);
            }
            g_stampIns = static_cast<uint32_t>(si / 200u);
            g_stampCyc = static_cast<uint32_t>(sc / 200u);
        }
        std::string text = std::string(list) + ",";
        std::string frameHex;
        // the frame function is always wrapped (first), so a report window is counted in frames
        char buf[16];
        std::snprintf(buf, sizeof buf, "%X", g_frameAddr);
        text = std::string(buf) + "," + text;
        size_t pos = 0u;
        while (pos < text.size() && g_count < kMax)
        {
            size_t comma = text.find(',', pos);
            if (comma == std::string::npos)
                comma = text.size();
            const std::string item = text.substr(pos, comma - pos);
            pos = comma + 1u;
            if (item.empty())
                continue;
            const uint32_t address = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 16));
            bool dup = false;
            for (int i = 0; i < g_count; ++i)
                dup = dup || g_addr[i] == address;
            if (dup || !runtime.hasFunction(address))
                continue;
            GuestFunction current = runtime.lookupFunction(address);
            if (!current)
                continue;
            const int idx = g_count;
            g_addr[idx] = address;
            g_orig[idx] = current;
            g_stubs[idx] = stubs[idx];
            if (!runtime.replaceFunction(address, stubs[idx]))
                continue;
            if (g_tracing)
                std::fprintf(stderr, "[fnprof] wrapped %08X: table now %s\n", address, runtime.lookupFunction(address) == stubs[idx] ? "the wrapper" : "SOMETHING ELSE");
            ++g_count;
        }
        std::fprintf(stderr, "[p3fes-native] fnprof: %d functions wrapped, frame function %08X, report every %u frames, stamp cost %u instr / %u cycles\n", g_count,
                     g_frameAddr, g_every, g_stampIns, g_stampCyc);
        return g_count > 0;
    }
}
