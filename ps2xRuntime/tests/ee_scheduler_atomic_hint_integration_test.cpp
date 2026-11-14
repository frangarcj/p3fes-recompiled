#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>
#include <vector>

namespace
{
    constexpr uint32_t kGuestEntry = 0x00100000u;
    constexpr uint32_t kAlarmEntry = kGuestEntry + 4u;
    constexpr uint32_t kExternalType = 0xEEC001u;
    constexpr uint64_t kTokenBase = 0xA110000000000000ull;
    constexpr uint32_t kExternalCount = 64u;

    enum class Scenario : uint8_t
    {
        ExternalEvents,
        StopDuringGuest,
        AlarmAndCancel,
    };

    std::atomic<Scenario> g_scenario{Scenario::ExternalEvents};
    std::atomic<uint32_t> g_guestEntries{0u};
    std::atomic<bool> g_guestWaitStopPoll{false};
    std::atomic<bool> g_guestStopPollAllowed{false};
    std::atomic<bool> g_guestStopObserved{false};
    std::atomic<bool> g_alarmSetupOk{false};
    std::atomic<bool> g_alarmReachedDeadline{false};
    std::atomic<uint32_t> g_alarmCallbacks{0u};

    uint32_t reg32(const R5900Context &ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[index]));
    }

    void setReg32(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void waitForGuest(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_guestEntries.fetch_add(1u, std::memory_order_release);
        const uint32_t index = reg32(*ctx, 4u);
        setReg32(*ctx, 4u, index + 1u);
        runtime->eeScheduler().waitExternal(EeWaitReason::External,
                                            kExternalType,
                                            kTokenBase + index);
    }

    void stopAwareGuest(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_guestWaitStopPoll.store(true, std::memory_order_release);
        while (!g_guestStopPollAllowed.load(std::memory_order_acquire))
            std::this_thread::yield();

        while (!runtime->eeScheduler().checkpointDue(1u))
        {
        }
        g_guestStopObserved.store(true, std::memory_order_release);
        ctx->pc = 0u;
    }

    void alarmGuest(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto &scheduler = runtime->eeScheduler();
        const int canceledId = scheduler.setAlarm(1u, kAlarmEntry, 0xCACEu, 0u, 0u);
        const int cancelResult = canceledId > 0 ? scheduler.cancelAlarm(canceledId) : -1;
        const int activeId = scheduler.setAlarm(1u, kAlarmEntry, 0xA11Au, 0u, 0u);
        g_alarmSetupOk.store(canceledId > 0 && cancelResult == 0 && activeId > 0,
                             std::memory_order_release);

        for (uint32_t i = 0u; i < 10000u; ++i)
        {
            if (scheduler.checkpointDue(128u))
            {
                g_alarmReachedDeadline.store(true, std::memory_order_release);
                ctx->pc = 0u;
                return;
            }
        }

        // The test thread will report a failure and request stop if the alarm never becomes due.
        ctx->pc = 0u;
    }

    void alarmCallback(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        if (reg32(*ctx, 6u) == 0xA11Au)
            g_alarmCallbacks.fetch_add(1u, std::memory_order_release);
        else
            g_alarmCallbacks.fetch_add(1000u, std::memory_order_release);
        ctx->pc = 0u;
    }

    [[noreturn]] void fail(const char *message)
    {
        std::fprintf(stderr, "EE scheduler atomic-hint integration test failed: %s\n", message);
        std::exit(EXIT_FAILURE);
    }

    void require(bool condition, const char *message)
    {
        if (!condition)
            fail(message);
    }

    template <typename Predicate>
    bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        const auto end = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < end)
        {
            if (predicate())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return predicate();
    }

    bool mainThreadIsWaitingExternal(const EeScheduler &scheduler)
    {
        const EeKernelSnapshot snapshot = scheduler.snapshot();
        for (const EeThreadSnapshot &thread : snapshot.threads)
        {
            if (thread.id == EeScheduler::kMainThreadId)
                return thread.status == EeThreadStatus::Waiting && thread.waitReason == EeWaitReason::External;
        }
        return false;
    }

    struct Executor
    {
        explicit Executor(PS2Runtime &runtime, std::vector<uint8_t> &rdram)
            : worker([this, &runtime, &rdram]
                     {
                         try
                         {
                             R5900Context context{};
                             context.pc = kGuestEntry;
                             setReg32(context, 4u, 0u);
                             runtime.eeScheduler().reset(rdram.data(), context);
                             started.store(true, std::memory_order_release);
                             runtime.eeScheduler().run();
                         }
                         catch (...)
                         {
                             error = std::current_exception();
                             started.store(true, std::memory_order_release);
                         }
                         finished.store(true, std::memory_order_release);
                     })
        {
        }

        ~Executor()
        {
            if (worker.joinable())
                worker.join();
        }

        void join()
        {
            if (worker.joinable())
                worker.join();
            if (error)
                std::rethrow_exception(error);
        }

        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        std::exception_ptr error;
        std::thread worker;
    };

    void testConcurrentExternalEventsAndStopWhileWaiting(PS2Runtime &runtime, std::vector<uint8_t> &rdram)
    {
        g_scenario.store(Scenario::ExternalEvents, std::memory_order_relaxed);
        g_guestEntries.store(0u, std::memory_order_relaxed);
        Executor executor(runtime, rdram);
        require(waitUntil([&] { return executor.started.load(std::memory_order_acquire); }),
                "executor did not start");
        require(waitUntil([&] { return mainThreadIsWaitingExternal(runtime.eeScheduler()); }),
                "guest did not enter its first external wait");

        for (uint32_t index = 0u; index < kExternalCount; ++index)
        {
            require(waitUntil([&] { return g_guestEntries.load(std::memory_order_acquire) == index + 1u; }),
                    "guest did not reach the expected token wait");
            runtime.eeScheduler().postEvent(EeEvent{EeEventType::ExternalWake,
                                                    kExternalType,
                                                    kTokenBase + index});
            require(waitUntil([&] { return g_guestEntries.load(std::memory_order_acquire) == index + 2u; }),
                    "posted unique wake token was lost or failed to resume guest");
        }

        require(waitUntil([&] { return mainThreadIsWaitingExternal(runtime.eeScheduler()); }),
                "guest did not block on the final token before stop");
        runtime.eeScheduler().requestStop();
        require(waitUntil([&] { return executor.finished.load(std::memory_order_acquire); }),
                "stop while waiting did not end the scheduler");
        executor.join();
        require(g_guestEntries.load(std::memory_order_acquire) == kExternalCount + 1u,
                "guest did not process every unique external event in order");
    }

    void testStopWhileGuestExecutes(PS2Runtime &runtime, std::vector<uint8_t> &rdram)
    {
        g_scenario.store(Scenario::StopDuringGuest, std::memory_order_relaxed);
        g_guestWaitStopPoll.store(false, std::memory_order_relaxed);
        g_guestStopPollAllowed.store(false, std::memory_order_relaxed);
        g_guestStopObserved.store(false, std::memory_order_relaxed);
        Executor executor(runtime, rdram);
        require(waitUntil([&] { return g_guestWaitStopPoll.load(std::memory_order_acquire); }),
                "guest did not enter stop-aware execution");
        runtime.eeScheduler().requestStop();
        g_guestStopPollAllowed.store(true, std::memory_order_release);
        require(waitUntil([&] { return executor.finished.load(std::memory_order_acquire); }),
                "stop during guest execution did not end the scheduler");
        executor.join();
        require(g_guestStopObserved.load(std::memory_order_acquire),
                "guest checkpoint failed to observe the stop request");
    }

    void testAlarmDeadlineAndCancellation(PS2Runtime &runtime, std::vector<uint8_t> &rdram)
    {
        g_scenario.store(Scenario::AlarmAndCancel, std::memory_order_relaxed);
        g_alarmSetupOk.store(false, std::memory_order_relaxed);
        g_alarmReachedDeadline.store(false, std::memory_order_relaxed);
        g_alarmCallbacks.store(0u, std::memory_order_relaxed);
        // The main entry switches to alarmGuest through the scenario selector below.
        Executor executor(runtime, rdram);
        require(waitUntil([&] { return g_alarmReachedDeadline.load(std::memory_order_acquire) ||
                                       executor.finished.load(std::memory_order_acquire); }),
                "alarm test did not reach a deadline");
        require(waitUntil([&] { return g_alarmCallbacks.load(std::memory_order_acquire) != 0u; }),
                "due alarm callback was not dispatched");
        runtime.eeScheduler().requestStop();
        require(waitUntil([&] { return executor.finished.load(std::memory_order_acquire); }),
                "scheduler did not stop after alarm callback");
        executor.join();
        require(g_alarmSetupOk.load(std::memory_order_acquire), "alarm setup/cancel contract failed");
        require(g_alarmReachedDeadline.load(std::memory_order_acquire), "active alarm did not reach its cycle deadline");
        require(g_alarmCallbacks.load(std::memory_order_acquire) == 1u,
                "canceled alarm fired or active alarm was dispatched more than once");
    }

    void dispatchMain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        switch (g_scenario.load(std::memory_order_relaxed))
        {
        case Scenario::ExternalEvents:
            waitForGuest(rdram, ctx, runtime);
            break;
        case Scenario::StopDuringGuest:
            stopAwareGuest(rdram, ctx, runtime);
            break;
        case Scenario::AlarmAndCancel:
            alarmGuest(rdram, ctx, runtime);
            break;
        }
    }
}

int main()
{
    // The first table entry is selected by scenario; the second is the alarm callback.
    static PS2Runtime::RecompiledFunction functions[] = {dispatchMain, alarmCallback};
    ps2SetRecompiledFunctionTable({functions, kGuestEntry, kGuestEntry + 8u, 2u});

    PS2Runtime runtime;
    std::vector<uint8_t> rdram(PS2_RAM_SIZE, 0u);
    testConcurrentExternalEventsAndStopWhileWaiting(runtime, rdram);
    testStopWhileGuestExecutes(runtime, rdram);
    testAlarmDeadlineAndCancellation(runtime, rdram);
    std::puts("EE scheduler atomic-hint integration tests passed");
    return EXIT_SUCCESS;
}
