// Optional Vita diagnostic: hardware performance counters of the game (EE)
// thread through libperf (a reimplementation of Sony's libperf for retail Vitas
// with taiHEN; see CMake option PS2X_VITA_LIBPERF). Built only when it is ON.
//
// Switch (ux0:data/p3fes/runtime.cfg, any PS2X_ key becomes an environment variable):
//   PS2X_VITA_PMU=1            one event set ("core": cycles, instr, I$/D$/TLB/branch misses)
//   PS2X_VITA_PMU=2            rotate "core" and "stall" every window
//   PS2X_VITA_PMU=3            rotate "core", "stall" and "mem"
//   PS2X_VITA_PMU_WINDOW_MS=N  window length, default 5000 (500..60000)
//
// Files (both needed when the switch is on):
//   ux0:data/p3fes/libperf.suprx   user module  (libperf/build/user/libperf.suprx)
//   ux0:data/p3fes/libperf.skprx   kernel plugin (libperf/build/libperf.skprx)
// The user module is also looked for at app0:libperf.suprx. The kernel plugin
// is loaded with taiHEN's taiLoadStartKernelModule only if the user module
// does not start without it (a kernel plugin already loaded at boot or by an
// earlier launch is used as is). No taiHEN config.txt entry is involved; the
// plugin stays loaded until the console reboots.
//
// PMU state belongs to threads, so everything is done BY the game thread on
// itself: it loads the modules (module_start opens PMU access for the thread
// that runs it), selects the events with thread id SELF, and samples its own
// counters from PS2Runtime::eeCheckpointSlow. Totals are 64-bit atomics-free
// game-thread state; finished windows are published through a small ring that
// the host thread prints next to its "[vita:cpu]" line.
#if defined(PS2X_VITA_LIBPERF)

#include "vita_pmu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <taihen.h>

#include "libperf.h"

extern "C" {
int ps2x_vita_pmu_active = 0;
int ps2x_vita_pmu_countdown = 1 << 30;
}

namespace
{
    constexpr const char *kUserModulePaths[] = {"ux0:data/p3fes/libperf.suprx", "app0:libperf.suprx"};
    constexpr const char *kKernelModulePath = "ux0:data/p3fes/libperf.skprx";

    // Calls to eeCheckpointSlow between looks at the clock.
    constexpr int kCallInterval = 128;
    // Minimum wall time between counter reads. The 32-bit hardware counters
    // wrap after 2^32 events: the cycle counter every ~9.7 s at 444 MHz (PMCR
    // 0x11 sets no /64 divider), and instruction rename, which can reach two
    // per cycle, after ~4.8 s. Reading every 250 ms keeps every delta far
    // below one wrap; a gap above kCoarseGapUs is reported in the line.
    constexpr uint64_t kSampleGapUs = 250000;
    constexpr uint64_t kCoarseGapUs = 3000000;

    constexpr int kSelf = 0; // SCE_PERF_ARM_PMON_THREAD_ID_SELF

    // Cortex-A9 event numbers (ARM DDI 0388 "Cortex-A9 TRM", PMU event table;
    // names as in libperf docs/agents/api-contract.md "Accepted event codes").
    struct EventSet
    {
        const char *name;
        uint8_t code[6]; // counter 0..5
    };
    constexpr EventSet kSets[3] = {
        // core: 0x68 instruction rename (closest to instructions executed; not
        // retired - counts speculative work too), 0x01 L1 I-cache miss,
        // 0x03 L1 D-cache miss, 0x02 instruction micro-TLB miss,
        // 0x05 data micro-TLB miss, 0x10 branch misprediction.
        {"core", {0x68, 0x01, 0x03, 0x02, 0x05, 0x10}},
        // stall: cycles, not events. 0x60 / 0x61 stalled on I / D cache,
        // 0x82 / 0x83 stalled on an instruction / data MAIN TLB miss (the A9
        // has no main-TLB miss count, only these stall cycles),
        // 0x67 issue stage empty (front end starved).
        {"stall", {0x68, 0x60, 0x61, 0x82, 0x83, 0x67}},
        // mem: 0x04 D-cache access (miss ratio with set core's 0x03),
        // 0x84 / 0x85 instruction / data micro-TLB stall cycles,
        // 0x12 predictable branches (mispredict ratio), 0x66 issue stage
        // could not dispatch.
        {"mem", {0x68, 0x04, 0x84, 0x85, 0x12, 0x66}},
    };

    struct Window
    {
        int set = 0;
        uint64_t wallUs = 0;
        uint64_t maxGapUs = 0;
        uint64_t cycles = 0;
        uint64_t ev[6] = {};
    };

    // Game-thread-only state.
    bool g_ready = false;
    int g_numSets = 1;
    int g_set = 0;
    SceUID g_ownerTid = 0;
    uint64_t g_windowUs = 5000000;
    uint64_t g_windowStartUs = 0;
    uint64_t g_lastSampleUs = 0;
    uint32_t g_last[7] = {}; // 0..5 events, 6 cycles
    Window g_cur;

    // Published to the host thread.
    constexpr uint32_t kRing = 4;
    Window g_ring[kRing];
    std::atomic<uint32_t> g_published{0};
    std::atomic<int> g_activeForReport{0};

    uint64_t nowUs()
    {
        return sceKernelGetProcessTimeWide();
    }

    void fail(const char *what, int code)
    {
        const char *hint = "";
        if (static_cast<uint32_t>(code) == 0x80580005u)
            hint = " (SCE_PERF_ERROR_NOT_INITIALIZED: PMU user access is closed for this thread)";
        else if (static_cast<uint32_t>(code) == 0x80580000u)
            hint = " (SCE_PERF_ERROR_INVALID_ARGUMENT)";
        std::fprintf(stderr, "[vita:pmu] disabled: %s failed 0x%08X%s\n", what, static_cast<unsigned>(code), hint);
        ps2x_vita_pmu_active = 0;
        ps2x_vita_pmu_countdown = 1 << 30;
        g_activeForReport.store(0, std::memory_order_relaxed);
    }

    bool readAll(uint32_t raw[7])
    {
        for (uint32_t i = 0; i < 6; ++i)
        {
            const int r = scePerfArmPmonGetCounterValue(kSelf, i, &raw[i]);
            if (r < 0)
            {
                fail("scePerfArmPmonGetCounterValue", r);
                return false;
            }
        }
        const int r = scePerfArmPmonGetCounterValue(kSelf, SCE_PERF_ARM_PMON_CYCLE_COUNTER, &raw[6]);
        if (r < 0)
        {
            fail("scePerfArmPmonGetCounterValue(cycle)", r);
            return false;
        }
        return true;
    }

    // Stop, select the six events, zero all seven counters, start, and take
    // the first raw reading. SELF only: the calling thread is the game thread.
    bool programSet(int set)
    {
        int r = scePerfArmPmonStop(kSelf);
        if (r < 0)
        {
            fail("scePerfArmPmonStop", r);
            return false;
        }
        for (uint32_t i = 0; i < 6; ++i)
        {
            r = scePerfArmPmonSelectEvent(kSelf, i, kSets[set].code[i]);
            if (r < 0)
            {
                fail("scePerfArmPmonSelectEvent", r);
                return false;
            }
        }
        r = scePerfArmPmonReset(kSelf);
        if (r < 0)
        {
            fail("scePerfArmPmonReset", r);
            return false;
        }
        r = scePerfArmPmonStart(kSelf);
        if (r < 0)
        {
            fail("scePerfArmPmonStart", r);
            return false;
        }
        if (!readAll(g_last))
            return false;
        g_set = set;
        g_cur = Window{};
        g_cur.set = set;
        g_windowStartUs = g_lastSampleUs = nowUs();
        return true;
    }

    bool fileExists(const char *path)
    {
        if (FILE *f = std::fopen(path, "rb"))
        {
            std::fclose(f);
            return true;
        }
        return false;
    }

    // Returns 1 started, 0 not found / failed to load (retry possible after the
    // kernel plugin is up), -1 loaded but module_start failed (do not retry).
    int tryUserModules(int *lastError)
    {
        int result = 0;
        for (const char *path : kUserModulePaths)
        {
            int status = 0;
            const SceUID id = sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, &status);
            std::fprintf(stderr, "[vita:pmu] load %s -> id 0x%08X start status 0x%08X\n", path,
                         static_cast<unsigned>(id), static_cast<unsigned>(status));
            if (id >= 0)
            {
                if (status == SCE_KERNEL_START_SUCCESS)
                    return 1;
                *lastError = status < 0 ? status : -1;
                return -1; // module_start (sceKernelPerfArmPmonOpen) failed
            }
            *lastError = id;
        }
        return result;
    }

    bool loadLibperf()
    {
        bool haveUser = false;
        for (const char *path : kUserModulePaths)
            haveUser = haveUser || fileExists(path);
        if (!haveUser)
        {
            std::fprintf(stderr, "[vita:pmu] disabled: libperf.suprx not found (ux0:data/p3fes/ or app0:)\n");
            return false;
        }
        int err = 0;
        int r = tryUserModules(&err);
        if (r == 0)
        {
            // Most likely the kernel provider (libperf.skprx) is not running.
            if (!fileExists(kKernelModulePath))
            {
                std::fprintf(stderr,
                             "[vita:pmu] disabled: libperf.suprx did not load (0x%08X) and %s is missing; "
                             "load the kernel plugin from taiHEN config.txt or copy it there\n",
                             static_cast<unsigned>(err), kKernelModulePath);
                return false;
            }
            const SceUID kid = taiLoadStartKernelModule(kKernelModulePath, 0, nullptr, 0);
            std::fprintf(stderr, "[vita:pmu] taiLoadStartKernelModule(%s) -> 0x%08X%s\n", kKernelModulePath,
                         static_cast<unsigned>(kid), kid >= 0 ? " (loaded until reboot)" : "");
            if (kid < 0)
            {
                std::fprintf(stderr,
                             "[vita:pmu] disabled: kernel plugin failed to load/start (taiHEN missing, "
                             "firmware not 3.60/3.65, or injection signature mismatch)\n");
                return false;
            }
            r = tryUserModules(&err);
        }
        if (r != 1)
        {
            std::fprintf(stderr, "[vita:pmu] disabled: libperf.suprx did not start (0x%08X)\n",
                         static_cast<unsigned>(err));
            return false;
        }
        return true;
    }

    void publishWindow()
    {
        const uint32_t seq = g_published.load(std::memory_order_relaxed);
        g_ring[seq % kRing] = g_cur;
        g_published.store(seq + 1, std::memory_order_release);
    }

    unsigned long long u(uint64_t v) { return static_cast<unsigned long long>(v); }

    double per1k(uint64_t count, uint64_t instr)
    {
        return instr ? static_cast<double>(count) * 1000.0 / static_cast<double>(instr) : 0.0;
    }
    double pct(uint64_t part, uint64_t whole)
    {
        return whole ? static_cast<double>(part) * 100.0 / static_cast<double>(whole) : 0.0;
    }

    void printWindow(const Window &w)
    {
        const double secs = static_cast<double>(w.wallUs) / 1e6;
        const double ipc = w.cycles ? static_cast<double>(w.ev[0]) / static_cast<double>(w.cycles) : 0.0;
        const double mhz = w.wallUs ? static_cast<double>(w.cycles) / static_cast<double>(w.wallUs) : 0.0;
        char head[160];
        std::snprintf(head, sizeof(head),
                      "[vita:pmu] set=%s win=%.2fs cycles=%llu (%.0f MHz counted) instr=%llu ipc=%.3f",
                      kSets[w.set].name, secs, u(w.cycles), mhz, u(w.ev[0]), ipc);
        const uint64_t instr = w.ev[0];
        const char *gap = w.maxGapUs > kCoarseGapUs ? " COARSE-SAMPLING(counters may have wrapped)" : "";
        switch (w.set)
        {
        case 0:
            std::fprintf(stderr,
                         "%s l1i_miss=%llu (per 1k instr %.2f) l1d_miss=%llu (per 1k %.2f) "
                         "itlb=%llu (per 1k %.2f) dtlb=%llu (per 1k %.2f) br_miss=%llu (per 1k %.2f) max_gap=%llums%s\n",
                         head, u(w.ev[1]), per1k(w.ev[1], instr), u(w.ev[2]), per1k(w.ev[2], instr),
                         u(w.ev[3]), per1k(w.ev[3], instr), u(w.ev[4]), per1k(w.ev[4], instr),
                         u(w.ev[5]), per1k(w.ev[5], instr), u(w.maxGapUs / 1000), gap);
            break;
        case 1:
            std::fprintf(stderr,
                         "%s icache_stall=%llu (%.1f%% of cycles) dcache_stall=%llu (%.1f%%) "
                         "itlb_main_stall=%llu (%.1f%%) dtlb_main_stall=%llu (%.1f%%) "
                         "issue_empty=%llu (%.1f%%) max_gap=%llums%s\n",
                         head, u(w.ev[1]), pct(w.ev[1], w.cycles), u(w.ev[2]), pct(w.ev[2], w.cycles),
                         u(w.ev[3]), pct(w.ev[3], w.cycles), u(w.ev[4]), pct(w.ev[4], w.cycles),
                         u(w.ev[5]), pct(w.ev[5], w.cycles), u(w.maxGapUs / 1000), gap);
            break;
        default:
            std::fprintf(stderr,
                         "%s dcache_access=%llu (per 1k instr %.2f) itlb_utlb_stall=%llu (%.1f%% of cycles) "
                         "dtlb_utlb_stall=%llu (%.1f%%) pred_branches=%llu (per 1k %.2f) "
                         "issue_no_dispatch=%llu (%.1f%%) max_gap=%llums%s\n",
                         head, u(w.ev[1]), per1k(w.ev[1], instr), u(w.ev[2]), pct(w.ev[2], w.cycles),
                         u(w.ev[3]), pct(w.ev[3], w.cycles), u(w.ev[4]), per1k(w.ev[4], instr),
                         u(w.ev[5]), pct(w.ev[5], w.cycles), u(w.maxGapUs / 1000), gap);
            break;
        }
    }
} // namespace

extern "C" void ps2x_vita_pmu_init_game_thread(void)
{
    const char *value = std::getenv("PS2X_VITA_PMU");
    const int mode = value ? std::atoi(value) : 0;
    if (mode <= 0)
        return;
    g_numSets = mode > 3 ? 3 : mode;
    if (const char *ms = std::getenv("PS2X_VITA_PMU_WINDOW_MS"))
    {
        long w = std::strtol(ms, nullptr, 10);
        g_windowUs = static_cast<uint64_t>(w < 500 ? 500 : (w > 60000 ? 60000 : w)) * 1000u;
    }
    g_ownerTid = sceKernelGetThreadId();

    if (!loadLibperf())
        return;
    // Loading ran module_start on this thread, which is what opens PMU access
    // for it; stop() is the first guarded call and reports NOT_INITIALIZED if
    // that did not reach the game thread.
    if (!programSet(0))
        return;
    g_ready = true;
    g_activeForReport.store(1, std::memory_order_relaxed);
    ps2x_vita_pmu_countdown = kCallInterval;
    ps2x_vita_pmu_active = 1;
    std::fprintf(stderr, "[vita:pmu] counting game thread 0x%08X: %d set(s), window %llu ms, first set '%s'\n",
                 static_cast<unsigned>(g_ownerTid), g_numSets, u(g_windowUs / 1000), kSets[0].name);
}

extern "C" void ps2x_vita_pmu_sample(void)
{
    ps2x_vita_pmu_countdown = kCallInterval;
    if (!g_ready || sceKernelGetThreadId() != g_ownerTid)
        return; // another host thread reached the checkpoint: its PMU is not set up
    const uint64_t now = nowUs();
    const uint64_t gap = now - g_lastSampleUs;
    if (gap < kSampleGapUs)
        return;

    uint32_t raw[7];
    if (!readAll(raw))
        return;
    for (int i = 0; i < 6; ++i)
        g_cur.ev[i] += static_cast<uint32_t>(raw[i] - g_last[i]);
    g_cur.cycles += static_cast<uint32_t>(raw[6] - g_last[6]);
    for (int i = 0; i < 7; ++i)
        g_last[i] = raw[i];
    g_lastSampleUs = now;
    if (gap > g_cur.maxGapUs)
        g_cur.maxGapUs = gap;

    if (now - g_windowStartUs >= g_windowUs)
    {
        g_cur.wallUs = now - g_windowStartUs;
        publishWindow();
        if (g_numSets > 1)
        {
            programSet((g_set + 1) % g_numSets);
        }
        else
        {
            g_cur = Window{};
            g_cur.set = g_set;
            g_windowStartUs = now;
        }
    }
}

extern "C" void ps2x_vita_pmu_report(void)
{
    static uint32_t printed = 0;     // host thread only
    static int emptyReports = 0;
    const uint32_t published = g_published.load(std::memory_order_acquire);
    if (published - printed > kRing)
        printed = published - kRing; // fell behind: the oldest windows were overwritten
    if (printed == published)
    {
        if (g_activeForReport.load(std::memory_order_relaxed) && printed == 0 && emptyReports++ == 2)
            std::fprintf(stderr, "[vita:pmu] no window finished yet (the game thread has not sampled)\n");
        return;
    }
    while (printed < published)
    {
        const Window w = g_ring[printed % kRing];
        ++printed;
        printWindow(w);
    }
}

#endif // PS2X_VITA_LIBPERF
