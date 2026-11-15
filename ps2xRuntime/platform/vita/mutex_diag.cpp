// PS2X_MUTEX_DIAG (CMake option, Vita): counts pthread mutex locks per thread and times an
// uncontended std::mutex against a SceKernelLwMutex. pthread_mutex_lock is intercepted with the
// linker's --wrap, so every std::mutex in the program is counted. Diagnostic builds only.
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <pthread.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t *mutex);

namespace
{
    constexpr int kSlots = 16;
    std::atomic<int> g_threadIds[kSlots];
    std::atomic<unsigned> g_counts[kSlots];
    std::atomic<unsigned> g_other{0u};
    // Callers (return address of the lock call) for the busiest threads, to name the locks.
    constexpr int kCallers = 128;
    std::atomic<unsigned> g_callerPc[kCallers];
    std::atomic<unsigned> g_callerCount[kCallers];
    std::atomic<int> g_started{0};

    int reporter(SceSize, void *)
    {
        // Cost of one uncontended lock + unlock.
        {
            std::mutex standard;
            const SceUInt64 a = sceKernelGetProcessTimeWide();
            for (int i = 0; i < 100000; ++i) { standard.lock(); standard.unlock(); }
            const SceUInt64 b = sceKernelGetProcessTimeWide();
            SceKernelLwMutexWork work;
            sceKernelCreateLwMutex(&work, "diag", 0, 0, nullptr);
            for (int i = 0; i < 100000; ++i) { sceKernelLockLwMutex(&work, 1, nullptr); sceKernelUnlockLwMutex(&work, 1); }
            const SceUInt64 c = sceKernelGetProcessTimeWide();
            sceKernelDeleteLwMutex(&work);
            // 100000 iterations timed in microseconds: ns per iteration = us / 100.
            std::fprintf(stderr, "[mutex-diag] uncontended lock+unlock: std::mutex %.0f ns, LwMutex %.0f ns\n",
                         static_cast<double>(b - a) / 100.0, static_cast<double>(c - b) / 100.0);
        }
        for (;;)
        {
            sceKernelDelayThread(5 * 1000 * 1000);
            char line[512];
            int used = std::snprintf(line, sizeof(line), "[mutex-diag] locks in 5 s:");
            for (int i = 0; i < kSlots; ++i)
            {
                const int id = g_threadIds[i].load(std::memory_order_relaxed);
                if (!id) break;
                const unsigned n = g_counts[i].exchange(0u, std::memory_order_relaxed);
                if (n && used < static_cast<int>(sizeof(line)) - 40)
                    used += std::snprintf(line + used, sizeof(line) - used, " %08X=%u", static_cast<unsigned>(id), n);
            }
            std::fprintf(stderr, "%s other=%u\n", line, g_other.exchange(0u, std::memory_order_relaxed));
            for (int i = 0; i < kCallers; ++i)
            {
                const unsigned pc = g_callerPc[i].load(std::memory_order_relaxed);
                const unsigned n = g_callerCount[i].exchange(0u, std::memory_order_relaxed);
                if (pc && n >= 500u)
                    std::fprintf(stderr, "[mutex-diag] caller %08X n=%u\n", pc, n);
            }
        }
        return 0;
    }
}

extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    const int self = sceKernelGetThreadId();
    int slot = -1;
    for (int i = 0; i < kSlots; ++i)
    {
        int id = g_threadIds[i].load(std::memory_order_relaxed);
        if (id == self) { slot = i; break; }
        if (id == 0)
        {
            if (g_threadIds[i].compare_exchange_strong(id, self)) { slot = i; break; }
            if (id == self) { slot = i; break; }
        }
    }
    if (slot >= 0) g_counts[slot].fetch_add(1u, std::memory_order_relaxed);
    else g_other.fetch_add(1u, std::memory_order_relaxed);
    {
        const unsigned pc = reinterpret_cast<unsigned>(__builtin_return_address(0));
        unsigned index = (pc >> 2) % kCallers;
        for (int probe = 0; probe < 8; ++probe, index = (index + 1u) % kCallers)
        {
            unsigned have = g_callerPc[index].load(std::memory_order_relaxed);
            if (have == 0u && g_callerPc[index].compare_exchange_strong(have, pc))
                have = pc;
            if (have == pc)
            {
                g_callerCount[index].fetch_add(1u, std::memory_order_relaxed);
                break;
            }
        }
    }
    if (g_started.load(std::memory_order_relaxed) == 0 && g_started.exchange(1) == 0)
    {
        const SceUID thread = sceKernelCreateThread("mutex-diag", reporter, 180, 0x10000, 0, 0x10000, nullptr);
        if (thread >= 0) sceKernelStartThread(thread, 0, nullptr);
    }
    return __real_pthread_mutex_lock(mutex);
}
