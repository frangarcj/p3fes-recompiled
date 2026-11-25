#include "../game_overrides/p3fes_scene_forward.h"
#include <utility>
#include <string>
#include "runtime/ee_scheduler.h"
#include "ee_clock_duration.h"

#include "ps2_log.h"
#include "runtime/ps2_perf.h"
#include "ps2_runtime_macros.h"
#if defined(PS2X_P3FES_FRAME_TRACE)
#include "../game_overrides/p3fes_frame_trace.h"
#endif

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>


extern std::atomic<uint32_t> g_ps2EeSampleCurrent;
// PS2X_FRAME_LIST=<ms> (with PS2X_FRAME_HIST=1): one [fdip] line for every game frame whose wall time reaches <ms> or
// whose CPU time reaches 85 % of it, with the time it sat in the scheduler's waits and in IOP syncs. The frame number is
// published for the EE sampler (PS2X_EE_SAMPLE_FRAMES), which prints the functions of the same frames.
std::atomic<uint64_t> g_ps2FrameSeq{0u};
// Sentinels the waits publish as "current function" for the EE sampler (bit 1 = native tag, so they read as such).
constexpr uint32_t kSampleWaitPacing = 0xFFFFFF01u, kSampleWaitEvent = 0xFFFFFF05u;
extern "C" void ps2xIopFrameCounters(uint64_t *out); // ps2_iop_host.cpp: syncs, wait us, tail us, hold us, tail EE cycles

#if defined(__vita__)
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#endif

// PS2X_FRAME_HIST=1: per game frame (the main-loop counter at 0x7CE10C), wall time against the thread's own
// CPU time and the time it sat in the scheduler's waits, summed per 5 s as a histogram by field count
// ([fhist] lines). Timestamps use sceKernelGetProcessTimeWide on the Vita (steady_clock costs ~6 us there).
namespace
{
    bool frameHistOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_FRAME_HIST"); return v && *v == '1'; }();
        return on;
    }
    uint64_t fhNowUs()
    {
#if defined(__vita__)
        return sceKernelGetProcessTimeWide();
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
    }
    uint64_t fhRunUs()
    {
#if defined(__vita__)
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) >= 0)
            return info.runClocks;
#endif
        return 0u;
    }
    // Wall time inside the scheduler's blocking waits, by kind (executor thread only).
    uint64_t g_fhPacingUs = 0u, g_fhEventUs = 0u, g_fhPacingCount = 0u, g_fhEventCount = 0u;
    uint64_t g_fhCapFields = 0u;
    // Host-thread hand-off waits (p3fes_native_present producer side), added through ps2xFrameHistAddLock.
    uint64_t g_fhLockUs = 0u, g_fhIoUs = 0u, g_fhIoCount = 0u, g_fhIoBytes = 0u, g_fhLastIo = 0u, g_fhDynUs = 0u, g_fhDynCount = 0u, g_fhLastDynUs = 0u, g_fhLastDynCount = 0u, g_fhHashUs = 0u, g_fhHashCount = 0u, g_fhLastHashUs = 0u, g_fhLastHashCount = 0u;
    struct FhBucket { unsigned n = 0; double wall = 0, cpu = 0, pace = 0, evt = 0, lock = 0, io = 0; unsigned cap = 0; };
    constexpr int kFhBuckets = 7; // 1..6 fields, 7 = more
    FhBucket g_fhBuckets[kFhBuckets];
    // PS2X_VSYNC_SLIP=1: a game frame that has already run past its two-field budget does not wait for the next
    // vblank (see waitForEvent). Host time of the last frame-counter advance, the frame it belongs to, and counters.
    std::chrono::steady_clock::time_point g_slipFrameStart{};
    uint64_t g_slipFrameId = 0u, g_slipDoneFrameId = 0u, g_slipCount = 0u, g_slipSavedUs = 0u;
    bool vsyncSlipOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_VSYNC_SLIP"); return v && *v == '1'; }();
        return on;
    }
    double frameListMs()
    {
        static const double v = [] { const char *e = std::getenv("PS2X_FRAME_LIST"); return e ? std::atof(e) : 0.0; }();
        return v;
    }
    uint64_t g_fhIopLast[5] = {0, 0, 0, 0, 0};
    // Coarse per-frame event accumulators (PS2X_FRAME_LIST): kind 0 = EE-side sceCdRead (CD.cpp / Support.h), 1 = guest
    // texture preparation, 2 = texture upload, 3 = vertex-buffer bake, 4..7 free for other owners. Atomic because the
    // render thread may feed some of them; one clock read per coarse event, never per hot call.
    constexpr int kFhEventKinds = 8;
    const char *const kFhEventNames[kFhEventKinds] = {"cd", "texprep", "upload", "bake", "ev4", "ev5", "ev6", "ev7"};
    std::atomic<uint64_t> g_fhEvUs[kFhEventKinds], g_fhEvCount[kFhEventKinds], g_fhEvBytes[kFhEventKinds], g_fhEvMaxUs[kFhEventKinds];
    uint64_t g_fhEvLastUs[kFhEventKinds] = {}, g_fhEvLastCount[kFhEventKinds] = {}, g_fhEvLastBytes[kFhEventKinds] = {};
    uint64_t g_fhLastWall = 0u, g_fhLastRun = 0u, g_fhLastTick = 0u, g_fhWindowStart = 0u;
    uint64_t g_fhLastPace = 0u, g_fhLastEvt = 0u, g_fhLastLock = 0u, g_fhLastCap = 0u;
    double g_fhWorstWall = 0.0, g_fhWorstCpu = 0.0, g_fhPrintMs = 0.0;

    std::atomic<int> g_stallTag{0}; // what the executor is doing: 0 guest code, 1 pacing wait, 2 event wait
    bool stallDiagOn()
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_STALL_DIAG"); return v && *v == '1'; }();
        return on;
    }
    struct FhScopedWait
    {
        uint64_t &sink; uint64_t &count; uint64_t start; bool on; bool tag; uint32_t prevSample;
        FhScopedWait(uint64_t &s, uint64_t &c) : sink(s), count(c), start(0u), on(frameHistOn()), tag(stallDiagOn()), prevSample(0u)
        {
            if (on) { start = fhNowUs(); prevSample = g_ps2EeSampleCurrent.exchange(&s == &g_fhPacingUs ? kSampleWaitPacing : kSampleWaitEvent, std::memory_order_relaxed); }
            if (tag) g_stallTag.store(&s == &g_fhPacingUs ? 1 : 2, std::memory_order_relaxed);
        }
        ~FhScopedWait()
        {
            if (on) { sink += fhNowUs() - start; ++count; g_ps2EeSampleCurrent.store(prevSample, std::memory_order_relaxed); }
            if (tag) g_stallTag.store(0, std::memory_order_relaxed);
        }
    };

    uint64_t g_fhLastEeCycle = 0u;
    void frameHistFrame(uint64_t tick, bool capped, uint64_t eeCycle)
    {
        const uint64_t wall = fhNowUs(), run = fhRunUs();
        if (g_fhLastWall != 0u)
        {
            const double wallMs = (wall - g_fhLastWall) / 1000.0, cpuMs = (run - g_fhLastRun) / 1000.0;
            const double fields = wallMs / 16.683;
            int k = static_cast<int>(fields + 0.5);
            k = k < 1 ? 1 : (k > kFhBuckets ? kFhBuckets : k);
            FhBucket &b = g_fhBuckets[k - 1];
            ++b.n;
            b.wall += wallMs;
            b.cpu += cpuMs;
            b.pace += (g_fhPacingUs - g_fhLastPace) / 1000.0;
            b.evt += (g_fhEventUs - g_fhLastEvt) / 1000.0;
            b.lock += (g_fhLockUs - g_fhLastLock) / 1000.0;
            b.io += (g_fhIoUs - g_fhLastIo) / 1000.0;
            b.cap += static_cast<unsigned>(g_fhCapFields - g_fhLastCap);
            g_fhWorstWall = std::max(g_fhWorstWall, wallMs);
            g_fhWorstCpu = std::max(g_fhWorstCpu, cpuMs);
            const double listMs = frameListMs();
            uint64_t iop[5];
            ps2xIopFrameCounters(iop);
            if (listMs > 0.0 && (wallMs >= listMs || cpuMs >= listMs * 0.85))
            {
                std::fprintf(stderr, "[fdip] seq=%llu t=%.3f wall=%.1f cpu=%.1f pace=%.1f event=%.1f lock=%.1f io=%.1f iop-sync n=%llu wait=%.2f tail=%.2f hold=%.2f tail-ee-ms=%.2f ee-model-ms=%.1f\n",
                             static_cast<unsigned long long>(g_ps2FrameSeq.load(std::memory_order_relaxed)), wall / 1e6, wallMs, cpuMs,
                             (g_fhPacingUs - g_fhLastPace) / 1000.0, (g_fhEventUs - g_fhLastEvt) / 1000.0, (g_fhLockUs - g_fhLastLock) / 1000.0,
                             (g_fhIoUs - g_fhLastIo) / 1000.0, static_cast<unsigned long long>(iop[0] - g_fhIopLast[0]),
                             (iop[1] - g_fhIopLast[1]) / 1000.0, (iop[2] - g_fhIopLast[2]) / 1000.0, (iop[3] - g_fhIopLast[3]) / 1000.0,
                             (iop[4] - g_fhIopLast[4]) / 294912.0, (eeCycle - g_fhLastEeCycle) / 294912.0);
            }
            for (int i = 0; i < 5; ++i) g_fhIopLast[i] = iop[i];
            // Coarse event totals of this frame (printed only for the frames the [fdip] line is for).
            {
                char evs[256];
                evs[0] = '\0';
                int en = 0;
                for (int k = 0; k < kFhEventKinds; ++k)
                {
                    const uint64_t us = g_fhEvUs[k].load(std::memory_order_relaxed), cnt = g_fhEvCount[k].load(std::memory_order_relaxed),
                                   by = g_fhEvBytes[k].load(std::memory_order_relaxed);
                    if (cnt != g_fhEvLastCount[k] && en < static_cast<int>(sizeof(evs)) - 70)
                        en += std::snprintf(evs + en, sizeof(evs) - en, " %s[n=%llu ms=%.1f kb=%llu max=%.1f]", kFhEventNames[k],
                                            static_cast<unsigned long long>(cnt - g_fhEvLastCount[k]), (us - g_fhEvLastUs[k]) / 1000.0,
                                            static_cast<unsigned long long>((by - g_fhEvLastBytes[k]) / 1024u),
                                            g_fhEvMaxUs[k].load(std::memory_order_relaxed) / 1000.0);
                    g_fhEvMaxUs[k].store(0u, std::memory_order_relaxed);
                    g_fhEvLastUs[k] = us; g_fhEvLastCount[k] = cnt; g_fhEvLastBytes[k] = by;
                }
                if (listMs > 0.0 && (wallMs >= listMs || cpuMs >= listMs * 0.85) && en > 0)
                    std::fprintf(stderr, "[fdip-ev] seq=%llu%s\n", static_cast<unsigned long long>(g_ps2FrameSeq.load(std::memory_order_relaxed)), evs);
            }
        }
        g_ps2FrameSeq.fetch_add(1u, std::memory_order_relaxed);
        g_fhLastEeCycle = eeCycle;
        (void)tick; (void)capped;
        g_fhLastWall = wall; g_fhLastRun = run; g_fhLastTick = tick;
        g_fhLastPace = g_fhPacingUs; g_fhLastEvt = g_fhEventUs; g_fhLastLock = g_fhLockUs; g_fhLastCap = g_fhCapFields; g_fhLastIo = g_fhIoUs;
        if (g_fhWindowStart == 0u) g_fhWindowStart = wall;
        static const uint64_t windowUs = [] {
            const char *v = std::getenv("PS2X_FRAME_HIST_SECONDS");
            const long n = v ? std::strtol(v, nullptr, 10) : 5;
            return static_cast<uint64_t>(n < 1 ? 5 : (n > 600 ? 600 : n)) * 1000000u;
        }();
        if (wall - g_fhWindowStart >= windowUs)
        {
            const uint64_t printStart = fhNowUs();
            unsigned total = 0; double wsum = 0, csum = 0, psum = 0, esum = 0, lsum = 0;
            for (const FhBucket &b : g_fhBuckets) { total += b.n; wsum += b.wall; csum += b.cpu; psum += b.pace; esum += b.evt; lsum += b.lock; }
            std::fprintf(stderr, "[fhist] t=%.2f %.1fs frames=%u fps=%.2f wall=%.2f cpu=%.2f (%.0f%%) pace-wait=%.2f event-wait=%.2f lock=%.2f other-off-cpu=%.2f ms/frame worst wall=%.1f cpu=%.1f\n",
                         wall / 1e6, (wall - g_fhWindowStart) / 1e6, total, total * 1e6 / double(wall - g_fhWindowStart), total ? wsum / total : 0.0, total ? csum / total : 0.0,
                         wsum > 0 ? csum * 100.0 / wsum : 0.0, total ? psum / total : 0.0, total ? esum / total : 0.0, total ? lsum / total : 0.0,
                         total ? (wsum - csum - psum - esum - lsum) / total : 0.0, g_fhWorstWall, g_fhWorstCpu);
            for (int i = 0; i < kFhBuckets; ++i)
            {
                const FhBucket &b = g_fhBuckets[i];
                if (!b.n) continue;
                std::fprintf(stderr, "[fhist]   %d field%s%s: n=%u wall=%.1f cpu=%.1f pace=%.1f event=%.1f lock=%.1f io=%.1f capsleep=%.2f\n", i + 1, i ? "s" : "", i + 1 == kFhBuckets ? "+" : "",
                             b.n, b.wall / b.n, b.cpu / b.n, b.pace / b.n, b.evt / b.n, b.lock / b.n, b.io / b.n, double(b.cap) / b.n);
            }
            if (vsyncSlipOn())
            {
                static uint64_t lastCount = 0u, lastSaved = 0u;
                std::fprintf(stderr, "[fhist]   vsync-slip: %llu frames released early, %.1f ms saved\n",
                             static_cast<unsigned long long>(g_slipCount - lastCount), (g_slipSavedUs - lastSaved) / 1000.0);
                lastCount = g_slipCount; lastSaved = g_slipSavedUs;
            }
            std::fprintf(stderr, "[fhist]   dynamic geometry: %.2f rebuilds/frame, %.2f ms/frame\n",
                         total ? double(g_fhDynCount - g_fhLastDynCount) / total : 0.0, total ? (g_fhDynUs - g_fhLastDynUs) / 1000.0 / total : 0.0);
            std::fprintf(stderr, "[fhist]   content-hash checks: %.1f/frame, %.2f ms/frame\n",
                         total ? double(g_fhHashCount - g_fhLastHashCount) / total : 0.0, total ? (g_fhHashUs - g_fhLastHashUs) / 1000.0 / total : 0.0);
            g_fhLastHashUs = g_fhHashUs; g_fhLastHashCount = g_fhHashCount;
            g_fhLastDynUs = g_fhDynUs; g_fhLastDynCount = g_fhDynCount;
            std::fprintf(stderr, "[fhist]   previous report took %.1f ms to print\n", g_fhPrintMs);
            g_fhPrintMs = (fhNowUs() - printStart) / 1000.0;
            for (FhBucket &b : g_fhBuckets) b = FhBucket{};
            g_fhWorstWall = g_fhWorstCpu = 0.0;
            g_fhWindowStart = wall;
        }
    }
}

// (PS2X_STALL_DIAG sampler thread removed: with priority 0x40 it hung the game at boot on the console; the stalls it was
// meant to find were the diagnostic log writes themselves, see [fhist] 'previous report took'.)
namespace { inline void startStallSampler() {} }

extern "C" void ps2xFrameHistAddLock(uint64_t us) { g_fhLockUs += us; }
extern "C" void ps2xFrameHistNoteHash(uint64_t startUs)
{
    if (startUs == 0u) return;
    g_fhHashUs += fhNowUs() - startUs;
    ++g_fhHashCount;
}
extern "C" void ps2xFrameHistNoteDynamic(uint64_t startUs)
{
    if (startUs == 0u) return;
    g_fhDynUs += fhNowUs() - startUs;
    ++g_fhDynCount;
}
extern "C" uint64_t ps2xFrameHistNowUs() { return (frameHistOn() || stallDiagOn()) ? fhNowUs() : 0u; }
// Host file reads (ps2_iop_host.cpp): summed per frame; one slower than 20 ms is logged with its size.
extern "C" void ps2xFrameHistNoteRead(uint64_t startUs, size_t bytes)
{
    if (startUs == 0u) return;
    const uint64_t us = fhNowUs() - startUs;
    g_fhIoUs += us;
    ++g_fhIoCount;
    g_fhIoBytes += bytes;
    if (us >= 20000u)
        std::fprintf(stderr, "[io-slow] t=%.2f read of %u bytes took %.1f ms\n", fhNowUs() / 1e6, static_cast<unsigned>(bytes), us / 1000.0);
}
// Coarse event accounting for the [fdip-ev] line (see kFhEventNames). startUs comes from ps2xFrameHistNowUs() (0 = off).
extern "C" void ps2xFrameHistNoteEvent(int kind, uint64_t startUs, uint64_t bytes)
{
    if (startUs == 0u || kind < 0 || kind >= kFhEventKinds) return;
    const uint64_t us = fhNowUs() - startUs;
    g_fhEvUs[kind].fetch_add(us, std::memory_order_relaxed);
    g_fhEvCount[kind].fetch_add(1u, std::memory_order_relaxed);
    g_fhEvBytes[kind].fetch_add(bytes, std::memory_order_relaxed);
    uint64_t old = g_fhEvMaxUs[kind].load(std::memory_order_relaxed);
    while (us > old && !g_fhEvMaxUs[kind].compare_exchange_weak(old, us, std::memory_order_relaxed)) {}
}
extern "C" int ps2xFrameHistEnabled() { return frameHistOn() ? 1 : 0; }

namespace
{
    // PS2X_PC_SAMPLE=<ms> (with PS2X_FRAME_HIST=1): a 1 ms sampler of the running guest context's pc (the generated code
    // stores the callee's entry there at every call and the return label after it), printed as one [pc-frame] line per
    // game frame whose samples reach <ms>. Unlike PS2X_EE_SAMPLE it sees through direct calls, so it names the guest
    // function that really spends the time (pc -> function through p3fes_map.csv, see p3fes/scripts/perf/pcframe_report.py).
    // "wait" samples are the scheduler's pacing / event waits. Costs nothing when off; the sampler thread only reads.
    std::atomic<R5900Context *> g_pcSampleCtx{nullptr};
    uint32_t pcSampleMs()
    {
        static const uint32_t v = [] { const char *e = std::getenv("PS2X_PC_SAMPLE"); return e ? static_cast<uint32_t>(std::atol(e)) : 0u; }();
        return v;
    }
    void pcSamplerLoop(uint32_t thresholdMs)
    {
        std::unordered_map<uint32_t, uint32_t> counts;
        uint64_t seq = g_ps2FrameSeq.load(std::memory_order_relaxed);
        uint32_t samples = 0u, waits = 0u;
        auto next = std::chrono::steady_clock::now();
        for (;;)
        {
            next += std::chrono::milliseconds(1);
            std::this_thread::sleep_until(next);
            const uint64_t now = g_ps2FrameSeq.load(std::memory_order_relaxed);
            if (now != seq)
            {
                if (samples >= thresholdMs)
                {
                    std::vector<std::pair<uint32_t, uint32_t>> v(counts.begin(), counts.end());
                    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
                    char line[1024];
                    int n = std::snprintf(line, sizeof(line), "[pc-frame] seq=%llu samples=%u wait=%u busy=%u top:", static_cast<unsigned long long>(seq), samples, waits, samples - waits);
                    for (size_t i = 0; i < v.size() && i < 24 && n > 0 && n < static_cast<int>(sizeof(line)) - 24; ++i)
                        n += std::snprintf(line + n, sizeof(line) - n, " %08X=%u", v[i].first, v[i].second);
                    std::fprintf(stderr, "%s\n", line);
                }
                counts.clear();
                samples = waits = 0u;
                seq = now;
            }
            ++samples;
            if (g_ps2EeSampleCurrent.load(std::memory_order_relaxed) >= 0xFFFFFF00u) { ++waits; continue; } // FhScopedWait's sentinels
            if (R5900Context *c = g_pcSampleCtx.load(std::memory_order_relaxed))
                ++counts[c->pc];
        }
    }
    void pcSampleNote(R5900Context *ctx)
    {
        static const bool started = []
        {
            const uint32_t ms = pcSampleMs();
            if (ms != 0u) std::thread(pcSamplerLoop, ms).detach();
            return true;
        }();
        (void)started;
        g_pcSampleCtx.store(ctx, std::memory_order_relaxed);
    }
}

namespace
{
    // PS2X_EE_THROW_DIAG=1: how often a guest thread leaves through the
    // dispatcher exception and how long each throw-to-catch takes. A C++
    // throw unwinds every generated frame on the host stack; on ARMv7 that is
    // a table search per frame through a very large executable.
    bool transferDiagEnabled()
    {
        static const bool enabled = []()
        {
            const char *env = std::getenv("PS2X_EE_THROW_DIAG");
            return env && *env == '1';
        }();
        return enabled;
    }
    uint64_t g_transferThrownAt = 0u;
    uint64_t g_transferCount = 0u, g_transferNanos = 0u, g_transferMaxNanos = 0u, g_transferWindowStart = 0u;

    uint64_t transferNow()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }

    void noteTransferThrow()
    {
        if (transferDiagEnabled())
            g_transferThrownAt = transferNow();
    }

    void noteTransferCatch()
    {
        if (!transferDiagEnabled() || g_transferThrownAt == 0u)
            return;
        const uint64_t now = transferNow();
        const uint64_t elapsed = now - g_transferThrownAt;
        g_transferThrownAt = 0u;
        ++g_transferCount;
        g_transferNanos += elapsed;
        g_transferMaxNanos = std::max(g_transferMaxNanos, elapsed);
        if (g_transferWindowStart == 0u)
            g_transferWindowStart = now;
        if (now - g_transferWindowStart >= 5000000000ull)
        {
            std::fprintf(stderr, "[ee-throw] window=%.2fs throws=%llu total_ms=%.1f avg_us=%.1f max_us=%.1f\n",
                         (now - g_transferWindowStart) / 1e9, static_cast<unsigned long long>(g_transferCount),
                         g_transferNanos / 1e6, g_transferCount ? g_transferNanos / 1e3 / g_transferCount : 0.0,
                         g_transferMaxNanos / 1e3);
            g_transferCount = g_transferNanos = g_transferMaxNanos = 0u;
            g_transferWindowStart = now;
        }
    }
}

namespace
{
    // Debug aid: PS2X_LOG_HANDOFFS=1 logs every scheduler hand-off (the
    // EeDispatcherTransfer throw sites) with the thread's base and active context.
    bool handoffLogEnabled()
    {
        static const bool enabled = []()
        {
            const char *env = std::getenv("PS2X_LOG_HANDOFFS");
            return env && *env && *env != '0';
        }();
        return enabled;
    }

    void logHandoff(const char *site, const GuestThread *thread)
    {
        if (!handoffLogEnabled() || !thread)
            return;
        const R5900Context &base = thread->context;
        const R5900Context &active = thread->activeContext();
        std::fprintf(stderr,
                     "[handoff] %s thread=%d base(pc=0x%08X sp=0x%08X ra=0x%08X) active(pc=0x%08X sp=0x%08X ra=0x%08X) invocations=%zu\n",
                     site, thread->id,
                     base.pc, getRegU32(&base, 29), getRegU32(&base, 31),
                     active.pc, getRegU32(&active, 29), getRegU32(&active, 31),
                     thread->invocations.size());
        std::fflush(stderr);
    }


    // Debug aid: PS2X_LOG_SUSPEND=1 logs Suspend/Resume/Wakeup calls with the caller.
    bool suspendLogEnabled()
    {
        static const bool enabled = []()
        {
            const char *env = std::getenv("PS2X_LOG_SUSPEND");
            return env && *env && *env != '0';
        }();
        return enabled;
    }

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    // The NTSC field is 60/1.001 Hz (59.94 Hz, 16,683.35 us). With 16,667 us movies ran at 30.00 frames/s instead of
    // 29.97 and the audio fell 0.1% behind (from god-of-war-recomp, vblank-ntsc).
    constexpr auto kVBlankPeriod = std::chrono::nanoseconds(16683350);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);

    // The guest still receives every field and timer in order. Only wall-clock
    // deadlines change, so camera tracks, actor updates and script waits agree.
    template<class Duration>
    Duration sceneDuration(Duration duration, bool forwarding)
    {
        return forwarding ? duration / static_cast<int>(p3fes_scene_forward::speed) : duration;
    }
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    [[gnu::noinline]] std::chrono::nanoseconds eeCyclesToHostDurationGeneric(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        static_assert(EeScheduler::kEeClockHz == 294912000u);
        uint64_t nanoseconds = 0u;
        if (ps2x::ee_clock::tryNanoseconds(cycles, nanoseconds))
            return std::chrono::nanoseconds(nanoseconds);
        return eeCyclesToHostDurationGeneric(cycles);
    }

    constexpr uint64_t kVBlankPeriodCycles = EeScheduler::kEeClockHz * 1001ull / 60000ull; // one NTSC field
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    // PS2X_DEADLINE_LOG counters
    uint64_t g_dbgEvents[8] = {};
    uint64_t g_dbgWaitSignaled = 0, g_dbgWaitTimeout = 0, g_dbgWaitForever = 0, g_dbgPacingWaits = 0;

    // EE cycle at which the next VBlankStart is due (kept by the scheduling code below).
    uint64_t g_nextVBlankStartCycle = 0u;

    // Movies: Sofdec paces its decoding by the game loop, so the cap is off while the IPU is in use.
    uint64_t g_frameCapIpuSeen = 0u;
    uint64_t g_frameCapIpuTick = 0u;
    // Frame cap (see checkpointDue): thread to put to sleep until the vblank after g_frameCapAfterTick, 0 = none.
    int g_frameCapThread = 0;
    uint64_t g_frameCapAfterTick = 0u;

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_sceneForward = false;
    p3fes_scene_forward::active.store(false, std::memory_order_relaxed);
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    // reset base and value together, without inheriting another execution's clock.
    m_runtime.writeCop0Count(nullptr, mainContext.cop0_count);
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    g_nextVBlankStartCycle = m_eeCycle + kVBlankPeriodCycles;
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    startStallSampler();
    ps2_perf::Scope perf(ps2_perf::Bucket::Ee); // exclusive executor times.
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty() && handoffLogEnabled())
            {
                // Debug aid: going idle while some thread claims to be Ready means
                // the ready queues and thread states disagree.
                static uint32_t s_idleReports = 0u;
                bool anyReady = false;
                for (const auto &[tid, t] : m_threads)
                {
                    if (t.status == EeThreadStatus::Ready)
                    {
                        anyReady = true;
                        break;
                    }
                }
                if (anyReady && s_idleReports < 20u)
                {
                    ++s_idleReports;
                    std::fprintf(stderr, "[sched-idle] no queued thread, but Ready threads exist:");
                    for (const auto &[tid, t] : m_threads)
                    {
                        if (t.status == EeThreadStatus::Ready)
                            std::fprintf(stderr, " id=%d(prio=%d)", t.id, t.currentPriority);
                    }
                    std::fprintf(stderr, " | non-empty queues:");
                    for (int p = 0; p < kPriorityCount; ++p)
                    {
                        if (!m_readyQueues[p].empty())
                            std::fprintf(stderr, " q%d=%zu", p, m_readyQueues[p].size());
                    }
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                }
            }
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, invocationStackTop());
                }
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
                noteTransferCatch();
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        if (g_frameCapThread != 0)
        {
            if (running->id == g_frameCapThread && running->invocations.empty() && m_vsyncTick <= g_frameCapAfterTick)
            {
                // Frame cap: sleep until the next vblank. Waking sets $v0 (a wait's return value); put it back.
                const __m128i savedV0 = running->activeContext().r[2];
                running->wait = EeWaitState{EeWaitReason::VSync, EeVSyncWait{g_frameCapAfterTick, 0},
                                            [savedV0](R5900Context &ctx) { ctx.r[2] = savedV0; }};
                running->status = running->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
                g_frameCapThread = 0;
                m_currentThreadId = 0;
                publishSnapshot();
                continue;
            }
            if (running->id == g_frameCapThread)
                g_frameCapThread = 0;
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                        noteTransferCatch();
                    }
                }
                continue;
            }
            if (running->id > 0)
            {
                std::fprintf(stderr,
                             "[thread-end] thread id=%d returned to pc=0 (entry=0x%08X ra=0x%08X sp=0x%08X) trace=%s\n",
                             running->id, running->entry, getRegU32(&context, 31), getRegU32(&context, 29),
                             m_runtime.dispatchHistoryString().c_str());
                std::fflush(stderr);
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            if (getRegU32(&invocation.context, 29) == 0u)
            {
                SET_GPR_U32(&invocation.context, 29, invocationStackTop());
            }
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            if (handoffLogEnabled())
            {
                // Debug aid: per-thread dispatch counts, printed every 2^12 dispatches,
                // to tell a spinning thread from one that is never scheduled.
                static std::unordered_map<int, uint64_t> s_dispatchCounts;
                static std::unordered_map<int, uint32_t> s_lastDispatchPc;
                static uint64_t s_totalDispatches = 0u;
                ++s_dispatchCounts[running->id];
                s_lastDispatchPc[running->id] = context.pc;
                if ((++s_totalDispatches & 0xFFFu) == 0u)
                {
                    std::fprintf(stderr, "[dispatch] total=%llu", static_cast<unsigned long long>(s_totalDispatches));
                    for (const auto &[tid, n] : s_dispatchCounts)
                    {
                        std::fprintf(stderr, " t%d=%llu(last pc=0x%08X)", tid, static_cast<unsigned long long>(n), s_lastDispatchPc[tid]);
                    }
                    std::fprintf(stderr, "\n");
                    std::fflush(stderr);
                }
            }
            {
                // PS2X_DISPATCH_DIAG=1: how often guest code is (re)entered from this loop, and at which PCs. Every
                // entry here means the host stack was empty: a yield, a block, or a guest transfer that the generated
                // code could not follow with a host call/return.
                static const bool dispatchDiag = std::getenv("PS2X_DISPATCH_DIAG") != nullptr;
                if (dispatchDiag)
                {
                    static std::unordered_map<uint32_t, uint32_t> s_pcs;
                    static uint64_t s_total = 0u;
                    static auto s_start = std::chrono::steady_clock::now();
                    ++s_pcs[context.pc];
                    if ((++s_total % 200000u) == 0u)
                    {
                        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
                        std::vector<std::pair<uint32_t, uint32_t>> top(s_pcs.begin(), s_pcs.end());
                        std::sort(top.begin(), top.end(), [](const auto &l, const auto &r) { return l.second > r.second; });
                        std::fprintf(stderr, "[dispatch-diag] 200000 dispatches in %.2f s, %zu distinct pcs; top:", seconds, top.size());
                        for (size_t i = 0u; i < top.size() && i < 24u; ++i)
                            std::fprintf(stderr, " %08X=%u", top[i].first, top[i].second);
                        std::fprintf(stderr, "\n");
                        s_pcs.clear();
                        s_start = std::chrono::steady_clock::now();
                    }
                }
            }
            ps2_perf::guestEntry(context.pc);
            // The EE sampler's "current function" word is not unwound when a
            // thread leaves through a scheduler transfer; every dispatch
            // starts from the function that is actually (re)entered.
            g_ps2EeSampleCurrent.store(context.pc, std::memory_order_relaxed);
            loadSharedVu0Random(context);
#if defined(PS2X_P3FES_FRAME_TRACE)
            {
                p3fes_frame_trace::GuestScope frameTraceScope(context.pc, 0u, true);
                function(m_rdram, &context, &m_runtime);
            }
#else
            function(m_rdram, &context, &m_runtime);
#endif
            saveSharedVu0Random(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (const EeDispatcherTransfer &)
        {
            noteTransferCatch();
            saveSharedVu0Random(context);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

void ps2AudioOnVBlank(); // ps2_runtime.cpp
extern uint64_t g_ps2xIpuActivity; // ipu_glue.cpp

namespace
{
    // Busy EE cycles charged since the last vblank (for PS2X_EE_CYCLE_STATS).
    uint64_t g_busyCyclesThisField = 0u;
    uint64_t g_busyCyclesSum = 0u;
    uint64_t g_busyCyclesMax = 0u;
    uint32_t g_busyFields = 0u;


    // Game frame cap: Persona 3 FES counts its main-loop iterations at 0x7CE10C (kwlnUpdate's frame counter). On the
    // console its logic runs at 30 updates/s; with the generated code's cheaper cycle estimates, lighter scenes finish a
    // frame within one field and the whole game ran at 60 (twice the speed). When the counter advances less than
    // PS2X_FRAME_CAP_FIELDS (default 2) fields after its previous advance, the thread that advanced it sleeps until
    // the next vblank, as if its frame had overrun the field on the console. Other threads (the movie decoder, sound)
    // keep running meanwhile: charging the rest of the field as busy time instead starved them and slowed the
    // movies down. PS2X_FRAME_CAP_FIELDS=1 turns the cap off;
    // PS2X_FRAME_COUNTER_ADDR moves the counter (0 = off).
    struct FrameCap
    {
        uint32_t addr = 0u;
        uint32_t fields = 0u;
        uint32_t lastCount = 0u;
        uint64_t lastTick = 0u;
        bool primed = false;
    };
    FrameCap &frameCap()
    {
        static FrameCap cap = []
        {
            FrameCap c;
            const char *a = std::getenv("PS2X_FRAME_COUNTER_ADDR");
            c.addr = a ? static_cast<uint32_t>(std::strtoul(a, nullptr, 0)) : 0x7CE10Cu;
            const char *f = std::getenv("PS2X_FRAME_CAP_FIELDS");
            const long n = f ? std::strtol(f, nullptr, 10) : 2;
            c.fields = static_cast<uint32_t>(n < 1 ? 1 : (n > 6 ? 6 : n));
            if (c.fields <= 1u || c.addr == 0u || c.addr + 4u > 0x2000000u)
                c.addr = 0u;
            return c;
        }();
        return cap;
    }
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    // The generated code charges a flat amount per loop back edge and per call, far below what the PS2 spends:
    // measured on Persona 3 FES, a 3D frame is charged ~1.25 M cycles of a 4.92 M field, so every frame finished in one
    // field and the game ran its 30 fps 3D logic at 60 (twice the speed). PS2X_EE_CYCLE_SCALE (default 4) multiplies
    // the charge: 3D frames then overrun a field like on the console (30 fps), while menus (~0.45 M) stay at 60.
    static const uint32_t cycleScale = []
    {
        const char *v = std::getenv("PS2X_EE_CYCLE_SCALE");
        const long n = v ? std::strtol(v, nullptr, 10) : 4;
        return static_cast<uint32_t>(n < 1 ? 1 : (n > 64 ? 64 : n));
    }();
    cycles *= cycleScale;
#if defined(PS2X_P3FES_FRAME_TRACE)
    p3fes_frame_trace::noteCheckpoint(cycles);
#endif
    g_busyCyclesThisField += cycles; // charged by guest code (generated back edges, dispatches)
    accountCycles(cycles);
    if (pcSampleMs() != 0u) pcSampleNote(currentContext());

    FrameCap &cap = frameCap();
    if (cap.addr != 0u && m_rdram)
    {
        uint32_t count;
        std::memcpy(&count, m_rdram + cap.addr, sizeof(count));
        if (count != cap.lastCount)
        {
            const uint64_t tick = m_vsyncTick;
            if (frameHistOn()) frameHistFrame(tick, false, m_eeCycle);
            if (vsyncSlipOn())
            {
                g_slipFrameStart = std::chrono::steady_clock::now();
                ++g_slipFrameId;
            }
            if (g_ps2xIpuActivity != g_frameCapIpuSeen)
            {
                g_frameCapIpuSeen = g_ps2xIpuActivity;
                g_frameCapIpuTick = tick;
            }
            const bool moviePlaying = g_frameCapIpuTick != 0u && tick - g_frameCapIpuTick < 30u;
            const GuestThread *running = currentThread();
            if (!moviePlaying && cap.primed && count == cap.lastCount + 1u && tick - cap.lastTick < cap.fields && running != nullptr &&
                running->invocations.empty() && !m_insideInterrupt)
            {
                // The dispatch loop puts this thread to sleep until the next vblank (its vblank wait then lands one
                // field later).
                g_frameCapThread = running->id;
                g_fhCapFields += 1u;
                g_frameCapAfterTick = tick;
                cap.lastTick = tick + 1u;
                cap.lastCount = count;
                m_checkpointPending.store(true, std::memory_order_release);
                return true;
            }
            else
            {
                cap.lastTick = tick;
            }
            cap.lastCount = count;
            cap.primed = true;
        }
    }

    // These are scalar hints/state only; event payloads are published under m_eventMutex,
    // and the deadline is an executor-owned scalar, so no acquire payload is consumed here.
    if (m_checkpointPending.load(std::memory_order_relaxed) ||
        m_stopRequested.load(std::memory_order_relaxed))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_relaxed);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    m_pendingEeTimerInterrupts |= m_runtime.memory().advanceEeTimers(elapsed);
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (handoffLogEnabled())
    {
        std::fprintf(stderr, "[create-thread] request entry=0x%08X stack=0x%08X size=0x%X gp=0x%08X prio=%d attr=0x%X by-thread=%d\n",
                     params.entry, params.stack, params.stackSize, params.gp, params.priority, params.attr, m_currentThreadId);
        std::fflush(stderr);
    }
    // EE thread priorities run 0 (highest) to 127; the main thread itself starts
    // at 0, and CRI ADXM asks for priority 0 for its worker threads. Rejecting 0
    // (the previous `< 1` check) left ADX's file/audio servers with no threads.
    if (params.priority < 0 || params.priority >= kPriorityCount)
    {
        if (handoffLogEnabled())
        {
            std::fprintf(stderr, "[create-thread] REJECTED illegal priority %d (entry=0x%08X)\n", params.priority, params.entry);
            std::fflush(stderr);
        }
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        if (handoffLogEnabled())
        {
            std::fprintf(stderr, "[create-thread] REJECTED no free thread id (entry=0x%08X)\n", params.entry);
            std::fflush(stderr);
        }
        return KE_ERROR;
    }
    if (handoffLogEnabled())
    {
        std::fprintf(stderr, "[create-thread] created id=%d entry=0x%08X\n", id, params.entry);
        std::fflush(stderr);
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    if (handoffLogEnabled())
    {
        std::fprintf(stderr,
                     "[start-thread] id=%d entry=0x%08X stack=0x%08X size=0x%X -> sp=0x%08X (from %s) prio=%d caller(pc=0x%08X sp=0x%08X ra=0x%08X) by-thread=%d\n",
                     id, target->entry, target->stack, target->stackSize, stackTop,
                     target->stack != 0u ? "own stack" : "CALLER sp",
                     target->currentPriority, caller.pc, getRegU32(&caller, 29), getRegU32(&caller, 31),
                     m_currentThreadId);
        std::fflush(stderr);
    }
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    std::fprintf(stderr, "[thread-end] thread id=%d called %s trace=%s\n", id,
                 deleteThreadRecord ? "ExitDeleteThread" : "ExitThread",
                 m_runtime.dispatchHistoryString().c_str());
    std::fflush(stderr);
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    noteTransferThrow();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (suspendLogEnabled())
    {
        const GuestThread *caller = currentThread();
        std::fprintf(stderr, "[suspend] target=%d caller=%d pc=0x%08X ra=0x%08X irq=%d\n", id,
                     caller ? caller->id : -1,
                     caller ? caller->activeContext().pc : 0u,
                     caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                     m_insideInterrupt ? 1 : 0);
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (suspendLogEnabled())
    {
        const GuestThread *caller = currentThread();
        std::fprintf(stderr, "[resume] target=%d caller=%d pc=0x%08X ra=0x%08X irq=%d\n", id,
                     caller ? caller->id : -1,
                     caller ? caller->activeContext().pc : 0u,
                     caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                     m_insideInterrupt ? 1 : 0);
    }
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (suspendLogEnabled())
    {
        const GuestThread *caller = currentThread();
        std::fprintf(stderr, "[wakeup] target=%d caller=%d pc=0x%08X ra=0x%08X irq=%d\n", id,
                     caller ? caller->id : -1,
                     caller ? caller->activeContext().pc : 0u,
                     caller ? getRegU32(&caller->activeContext(), 31) : 0u,
                     m_insideInterrupt ? 1 : 0);
    }
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    // Experiment: accepting priority 0 here (PS2X_ALLOW_PRIO0_CHANGE=1) let CRI
    // raise a spinning worker above the main thread and regressed boot, so the
    // original 1..127 check stays the default until real-kernel behaviour is known.
    static const bool s_allowPrio0Change = []()
    {
        const char *env = std::getenv("PS2X_ALLOW_PRIO0_CHANGE");
        return env && *env && *env != '0';
    }();
    if (priority < (s_allowPrio0Change ? 0 : 1) || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        logHandoff("transfer", self);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    noteTransferThrow();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + sceneDuration(std::chrono::microseconds(tickCount * kAlarmTickMicroseconds), m_sceneForward),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    if (getRegU32(&invocation.context, 29) == 0u)
    {
        SET_GPR_U32(&invocation.context, 29, invocationStackTop());
    }
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    logHandoff("invoke", owner);
    publishSnapshot();
    noteTransferThrow();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        if (getRegU32(&it->context, 29) == 0u)
        {
            SET_GPR_U32(&it->context, 29, invocationStackTop());
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    logHandoff("invoke-seq", owner);
    publishSnapshot();
    noteTransferThrow();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    if (suspendLogEnabled())
        std::fprintf(stderr, "[irq-add] %s cause=%u handler=0x%08X arg=0x%08X id=%d\n",
                     dmac ? "dmac" : "intc", cause, handler, argument, id);
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (suspendLogEnabled() && !(dmac == false && cause <= 3u))
        std::fprintf(stderr, "[irq] %s cause=%u enabled=%d\n", dmac ? "dmac" : "intc", cause,
                     (cause < 32u && (mask & (1u << cause))) ? 1 : 0);
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        return;
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

namespace
{
    // The snapshot is a debugging view (debug panel, hang reports). Building it copies and sorts every thread,
    // semaphore and event flag, and it used to be rebuilt on every thread state change: several times per vblank,
    // 3% of the game thread in the dorm scene. It is now rebuilt when a reader has asked since the last one, and
    // otherwise at most four times per emulated second so a hang report is never far behind.
    std::atomic<bool> g_snapshotWanted{true};
    uint64_t g_snapshotLastCycle = 0u;
    constexpr uint64_t kSnapshotIdleCycles = 294912000ull / 4u;
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    g_snapshotWanted.store(true, std::memory_order_relaxed);
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    if (!g_snapshotWanted.load(std::memory_order_relaxed) && m_eeCycle - g_snapshotLastCycle < kSnapshotIdleCycles &&
        m_eeCycle >= g_snapshotLastCycle && m_running.load(std::memory_order_relaxed))
    {
        return;
    }
    g_snapshotWanted.store(false, std::memory_order_relaxed);
    g_snapshotLastCycle = m_eeCycle;
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    for (auto &queue : m_readyQueues)
    {
        if (queue.empty())
        {
            continue;
        }
        const int id = queue.front();
        queue.pop_front();
        GuestThread *selected = thread(id);
        assert(selected != nullptr);
        assert(selected->status == EeThreadStatus::Ready);
        return selected;
    }
    return nullptr;
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

void dumpBranchRing(const char *why);
void EeScheduler::makeDormant(GuestThread &item)
{
    { static int n = 0; if (item.id == 1 && n++ < 1) dumpBranchRing("main thread dormant"); }
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    logHandoff("block", self);
    m_currentThreadId = 0;
    publishSnapshot();
    noteTransferThrow();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

namespace
{
    // edge state of the GS interrupt (only used on the executor thread). Kept here rather than in EeScheduler so
    // the class size does not change.
    uint32_t g_gsIrqLatched = 0u;
}

void EeScheduler::pollGsInterrupt()
{
    // The runtime set CSR.FINISH/SIGNAL but never raised the GS interrupt (INTC 0). Games wait for the end of a
    // DMA chain with a handler for that interrupt (God of War's vid::WaitForDMAComplete waits on a flag only that
    // handler sets). IMR: bit 8 = SIGMSK, bit 9 = FINISHMSK (1 = masked). The handler clears the bit by writing CSR.
    auto &gs = m_runtime.memory().gs();
    const uint32_t csr = static_cast<uint32_t>(gs.csr.load(std::memory_order_acquire)) & 0x3u;
    const uint32_t masked = static_cast<uint32_t>(gs.imr >> 8) & 0x3u;
    const uint32_t active = csr & ~masked;
    const uint32_t rising = active & ~g_gsIrqLatched;
    g_gsIrqLatched = active;
    if (rising != 0u)
    {
        dispatchIrq(false, 0u);
    }
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    pollGsInterrupt();
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        ++g_dbgEvents[static_cast<size_t>(event.type) & 7u];
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            // Debug aid: PS2X_DEADLINE_LOG=1 lists the pending scheduler deadlines every 2 s.
            static const bool deadlineLog = std::getenv("PS2X_DEADLINE_LOG") != nullptr;
            if (deadlineLog)
            {
                static auto lastLog = now;
                if (now - lastLog > std::chrono::seconds(2))
                {
                    lastLog = now;
                    std::fprintf(stderr, "[deadlines] cycle=%llu vsync=%llu events=%zu:", (unsigned long long)m_eeCycle,
                                 (unsigned long long)m_vsyncTick, m_events.size());
                    for (const ScheduledEvent &item : m_deadlines)
                        std::fprintf(stderr, " t%d@%+lld/%+lldms", (int)item.event.type,
                                     (long long)item.deadlineCycle - (long long)m_eeCycle,
                                     (long long)std::chrono::duration_cast<std::chrono::milliseconds>(item.hostDeadline - now).count());
                    std::fprintf(stderr, " | events wake=%llu alarm=%llu dmac=%llu; waits signaled=%llu timeout=%llu forever=%llu pacing=%llu\n",
                                 (unsigned long long)g_dbgEvents[4], (unsigned long long)g_dbgEvents[5], (unsigned long long)g_dbgEvents[3],
                                 (unsigned long long)g_dbgWaitSignaled, (unsigned long long)g_dbgWaitTimeout,
                                 (unsigned long long)g_dbgWaitForever, (unsigned long long)g_dbgPacingWaits);
                    for (auto &c : g_dbgEvents) c = 0;
                    g_dbgWaitSignaled = g_dbgWaitTimeout = g_dbgWaitForever = g_dbgPacingWaits = 0;
                }
            }
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                ++g_dbgPacingWaits;
                FhScopedWait fhWait(g_fhPacingUs, g_fhPacingCount);
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                // PS2X_EE_CYCLE_STATS=1: every 300 fields, average / max busy EE cycles charged per field, against the
                // 4.92 M a field lasts. Shows how far the generated code's cycle estimates are from a real frame's cost.
                static const bool cycleStats = std::getenv("PS2X_EE_CYCLE_STATS") != nullptr;
                if (cycleStats)
                {
                    g_busyCyclesSum += g_busyCyclesThisField;
                    g_busyCyclesMax = std::max(g_busyCyclesMax, g_busyCyclesThisField);
                    if (++g_busyFields >= 300u)
                    {
                        std::fprintf(stderr, "[ee-cycles] field %llu: busy avg %.2f M max %.2f M per field (field = %.2f M)\n",
                                     (unsigned long long)m_vsyncTick, g_busyCyclesSum / 300.0 / 1e6, g_busyCyclesMax / 1e6,
                                     kVBlankPeriodCycles / 1e6);
                        g_busyCyclesSum = g_busyCyclesMax = 0u;
                        g_busyFields = 0u;
                    }
                }
                g_busyCyclesThisField = 0u;
                const bool wasForwarding = m_sceneForward;
                m_sceneForward = p3fes_scene_forward::sample(m_rdram);
                ps2AudioOnVBlank(); // SPU2 output to the host sound pipe (ps2_runtime.cpp)
                // Pace against the host clock, but never "catch up": after a stretch slower than real time
                // (loading, a heavy scene), restart the schedule from now instead of running fields back to back.
                auto hostBase = scheduled.hostDeadline;
                const auto hostNow = std::chrono::steady_clock::now();
                if (wasForwarding != m_sceneForward)
                {
                    // An alarm can have been scheduled several fields ahead under the previous rate.
                    // Re-anchor it on this field without changing its guest cycle or delivery order.
                    std::lock_guard lock(m_eventMutex);
                    for (auto &pending : m_deadlines)
                        if (pending.event.type == EeEventType::Alarm && pending.deadlineCycle >= scheduled.deadlineCycle)
                            pending.hostDeadline = hostNow + sceneDuration(
                                eeCyclesToHostDuration(pending.deadlineCycle - scheduled.deadlineCycle), m_sceneForward);
                }
                // PS2X_PACE_DIAG=1: every 300 fields, how often the schedule was restarted, the host time
                // given up by it, and the longest gap between two consecutive fields.
                {
                    static const bool paceDiag = [] { const char *v = std::getenv("PS2X_PACE_DIAG"); return v && *v == '1'; }();
                    static unsigned fields = 0u, resets = 0u, lateFields = 0u;
                    static double lostMs = 0.0, worstGapMs = 0.0, worstLateMs = 0.0;
                    static std::chrono::steady_clock::time_point previous{};
                    if (paceDiag)
                    {
                        const double lateMs = std::chrono::duration<double, std::milli>(hostNow - hostBase).count();
                        if (previous.time_since_epoch().count() != 0)
                            worstGapMs = std::max(worstGapMs, std::chrono::duration<double, std::milli>(hostNow - previous).count());
                        previous = hostNow;
                        worstLateMs = std::max(worstLateMs, lateMs);
                        if (lateMs > 1.0) ++lateFields;
                        if (hostBase + 15 * kVBlankPeriod < hostNow)
                        {
                            ++resets;
                            lostMs += lateMs;
                        }
                        if (++fields >= 300u)
                        {
                            std::fprintf(stderr, "[pace] 300 fields: late=%u resets=%u lost=%.0f ms worst-late=%.1f ms worst-gap=%.1f ms\n",
                                         lateFields, resets, lostMs, worstLateMs, worstGapMs);
                            fields = resets = lateFields = 0u;
                            lostMs = worstGapMs = worstLateMs = 0.0;
                        }
                    }
                }
                // How far behind the schedule may fall before it is restarted. Two fields (the old rule)
                // made every hiccup longer than 33 ms a permanent loss: on the Vita the game thread is
                // stalled for 90-190 ms a couple of times every few seconds (log writes to the memory
                // card and the like), which cost 4-7 % of game speed with the thread only 80 % busy. Up to
                // a quarter of a second is now recovered by running fields back to back with the spare
                // CPU; longer stretches (loading) still restart. PS2X_PACE_CATCHUP_FIELDS overrides (2 =
                // old behaviour).
                static const int catchUpFields = [] {
                    const char *v = std::getenv("PS2X_PACE_CATCHUP_FIELDS");
                    const int parsed = v && *v ? std::atoi(v) : 15;
                    return parsed >= 1 && parsed <= 600 ? parsed : 15;
                }();
                if (wasForwarding != m_sceneForward || hostBase + catchUpFields * sceneDuration(kVBlankPeriod, m_sceneForward) < hostNow)
                    hostBase = hostNow;
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              hostBase + sceneDuration(kVBlankDuration, m_sceneForward),
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                g_nextVBlankStartCycle = scheduled.deadlineCycle + kVBlankPeriodCycles;
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              hostBase + sceneDuration(kVBlankPeriod, m_sceneForward),
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        // registers consistent from the EE before waking game callbacks.
        { extern uint64_t g_path3TraceVsync; g_path3TraceVsync = m_vsyncTick; }
        m_runtime.memory().flushMaskedPath3Packets(); // a vsync drains PATH3 data left waiting after an unmask
        m_runtime.gs().notifyExternalVSync(m_vsyncTick, static_cast<uint32_t>(m_vsyncTick & 1u));
        // Debug aid: PS2X_RAM_DUMP=<field>:<path> writes the 32 MB of EE RAM to <path> at that vsync.
        {
            static const std::pair<uint64_t, std::string> ramDump = []() -> std::pair<uint64_t, std::string>
            {
                const char *v = std::getenv("PS2X_RAM_DUMP");
                if (!v || !std::strchr(v, ':'))
                    return {0u, {}};
                return {std::strtoull(v, nullptr, 10), std::string(std::strchr(v, ':') + 1)};
            }();
            if (ramDump.first != 0u && m_vsyncTick == ramDump.first && m_rdram)
            {
                if (std::FILE *f = std::fopen(ramDump.second.c_str(), "wb"))
                {
                    std::fwrite(m_rdram, 1, PS2_RAM_SIZE, f);
                    std::fclose(f);
                    std::fprintf(stderr, "[ram-dump] field %llu -> %s\n", (unsigned long long)m_vsyncTick,
                                 ramDump.second.c_str());
                }
            }
            // ... and the 2 MB of IOP RAM to <path>.iop
            if (ramDump.first != 0u && m_vsyncTick == ramDump.first)
            {
                std::vector<uint8_t> iop(0x200000u);
                if (m_runtime.readIopMemory(0u, iop.data(), iop.size()))
                    if (std::FILE *f = std::fopen((ramDump.second + ".iop").c_str(), "wb"))
                    {
                        std::fwrite(iop.data(), 1, iop.size(), f);
                        std::fclose(f);
                    }
            }
        }
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        queueInvocation(std::move(invocation));
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    ps2_perf::Scope perf(ps2_perf::Bucket::GuestWait);
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    if (vsyncSlipOn() && g_slipFrameId != g_slipDoneFrameId && g_slipFrameStart != std::chrono::steady_clock::time_point{})
    {
        // The frame has used up its field budget (PS2X_FRAME_CAP_FIELDS fields since the frame counter last
        // advanced) and every guest thread is blocked: the wait for the next vblank would only round the frame up
        // to a whole number of fields (a 36 ms frame costs 50 ms). Slide the whole host schedule earlier so that
        // vblank is due now; the schedule then continues from here. Not during movies (their decode is paced by
        // the vblank clock).
        const auto now = std::chrono::steady_clock::now();
        const uint32_t fields = frameCap().fields;
        const bool moviePlaying = g_frameCapIpuTick != 0u && m_vsyncTick - g_frameCapIpuTick < 30u;
        if (!moviePlaying && now - g_slipFrameStart >= fields * sceneDuration(kVBlankPeriod, m_sceneForward) - std::chrono::microseconds(500))
        {
            const ScheduledEvent *vblank = nullptr;
            for (const ScheduledEvent &item : m_deadlines)
                if (item.event.type == EeEventType::VBlankStart && (!vblank || item.deadlineCycle < vblank->deadlineCycle))
                    vblank = &item;
            if (vblank && vblank->hostDeadline > now)
            {
                const auto delta = vblank->hostDeadline - now;
                for (ScheduledEvent &item : m_deadlines)
                    item.hostDeadline -= delta;
                ++g_slipCount;
                g_slipSavedUs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(delta).count());
            }
            g_slipDoneFrameId = g_slipFrameId;
        }
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        ++g_dbgWaitForever;
        FhScopedWait fhWait(g_fhEventUs, g_fhEventCount);
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        // Host time of the timer interrupt. Measured back from the next scheduled event (in practice
        // the vblank, whose host deadline follows the frame schedule) rather than forward from now:
        // when the frame's work ran past a vblank, "now + duration" made every timer tick of the
        // idle remainder wait its full real-time length, so the lateness was never recovered. With
        // the game's sound drivers loaded (EE timer ticks all through the frame) that held the game
        // at 93 % speed with the thread only 81 % busy. PS2X_TIMER_PACE=now restores the old rule.
        static const bool paceFromNow = [] { const char *v = std::getenv("PS2X_TIMER_PACE"); return v && std::strcmp(v, "now") == 0; }();
        auto timerHostDeadline = std::chrono::steady_clock::now() + sceneDuration(eeCyclesToHostDuration(timerCycles), m_sceneForward);
        const uint64_t timerCycle = m_eeCycle + timerCycles;
        if (!paceFromNow && hostDeadline != std::chrono::steady_clock::time_point::max() && timerCycle < deadlineCycle)
            timerHostDeadline = std::min(timerHostDeadline, hostDeadline - sceneDuration(eeCyclesToHostDuration(deadlineCycle - timerCycle), m_sceneForward));
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    FhScopedWait fhWait(g_fhEventUs, g_fhEventCount);
    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    ++(signaled ? g_dbgWaitSignaled : g_dbgWaitTimeout);
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        // One clock for every paced event. Callers give "now + the event's duration", which is only
        // right while the host keeps up: when a frame's work has run past a vblank, emulated time is
        // behind the host and an alarm or thread delay scheduled then would wait its full real
        // length on top of that lateness (with the game's sound drivers loaded this held the game at
        // 93 % speed with the thread 82 % busy). The pending vblank carries the frame schedule's
        // mapping from emulated cycles to host time; no other event may be later than that mapping
        // puts it. PS2X_TIMER_PACE=now restores the callers' deadlines.
        static const bool paceFromNow = [] { const char *v = std::getenv("PS2X_TIMER_PACE"); return v && std::strcmp(v, "now") == 0; }();
        if (!paceFromNow && event.type != EeEventType::VBlankStart && event.type != EeEventType::VBlankEnd)
        {
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.event.type != EeEventType::VBlankStart)
                    continue;
                const auto scheduled = deadlineCycle <= item.deadlineCycle
                    ? item.hostDeadline - sceneDuration(eeCyclesToHostDuration(item.deadlineCycle - deadlineCycle), m_sceneForward)
                    : item.hostDeadline + sceneDuration(eeCyclesToHostDuration(deadlineCycle - item.deadlineCycle), m_sceneForward);
                hostDeadline = std::min(hostDeadline, scheduled);
                break;
            }
        }
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::loadSharedVu0Random(R5900Context &context)
{
    if (!m_sharedVu0RandomValid)
    {
        saveSharedVu0Random(context);
        return;
    }
    context.vu0_r = m_sharedVu0Random;
}

void EeScheduler::saveSharedVu0Random(const R5900Context &context)
{
    m_sharedVu0Random = context.vu0_r;
    m_sharedVu0RandomValid = true;
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
