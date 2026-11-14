#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
    using namespace ps2x::iop::detail;

    void require(bool condition, const char *message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    int createThread(IopKernel &kernel, IopMemory &memory, uint32_t descriptor,
                     uint32_t entry, uint32_t priority)
    {
        memory.write32(descriptor + 0u, 0u); // attr
        memory.write32(descriptor + 4u, 0u); // option
        memory.write32(descriptor + 8u, entry);
        memory.write32(descriptor + 12u, 0x400u);
        memory.write32(descriptor + 16u, priority);
        IopCpuState caller{};
        caller.gpr[4] = descriptor;
        require(kernel.dispatchThreadImport(4u, caller, 0u), "CreateThread import was not handled");
        const int id = static_cast<int32_t>(caller.gpr[2]);
        require(id > 0, "CreateThread failed");
        return id;
    }

    void startThread(IopKernel &kernel, int id)
    {
        IopCpuState caller{};
        caller.gpr[4] = static_cast<uint32_t>(id);
        require(kernel.dispatchThreadImport(6u, caller, 0u), "StartThread import was not handled");
        require(caller.gpr[2] == 0u, "StartThread failed");
    }

    void testExitDeleteReleasesStack()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        const int id = createThread(kernel, memory, 0x1000u, 0x10000u, 10u);
        startThread(kernel, id);

        IopThread *thread = kernel.beginNextReady(0u);
        require(thread && thread->id == id, "new thread was not scheduled");
        const uint32_t stackBase = thread->stackBase;
        require(memory.allocationContaining(stackBase).has_value(), "thread stack allocation missing before exit");

        require(kernel.dispatchThreadImport(9u, thread->cpu, 0u), "ExitDeleteThread import was not handled");
        require(thread->state == IopThreadState::Dead, "ExitDeleteThread did not mark the running thread dead");
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);

        require(kernel.threadCount() == 0u, "ExitDeleteThread thread survived endTimeslice");
        require(!memory.allocationContaining(stackBase).has_value(), "ExitDeleteThread leaked its stack allocation");
        require(kernel.beginNextReady(0u) == nullptr, "deleted thread became schedulable again");
    }

    void testExitThreadCanBeRestarted()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        const int id = createThread(kernel, memory, 0x1000u, 0x11000u, 10u);
        startThread(kernel, id);
        IopThread *thread = kernel.beginNextReady(0u);
        require(thread && thread->id == id, "thread did not start");
        const uint32_t stackBase = thread->stackBase;

        require(kernel.dispatchThreadImport(8u, thread->cpu, 0u), "ExitThread import was not handled");
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        require(kernel.threadCount() == 1u, "ExitThread removed the dormant thread");
        require(memory.allocationContaining(stackBase).has_value(), "ExitThread released a restartable thread stack");
        require(kernel.beginNextReady(0u) == nullptr, "dormant ExitThread was scheduled without restart");

        startThread(kernel, id);
        thread = kernel.beginNextReady(1u);
        require(thread && thread->id == id, "dormant thread could not be restarted");
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        require(kernel.threadCount() == 1u, "ordinary timeslice cleanup removed a live thread");
        require(memory.allocationContaining(stackBase).has_value(), "ordinary timeslice cleanup freed a live stack");
    }

    void testRangeTerminationRemovesOnlyMatches()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        const int first = createThread(kernel, memory, 0x1000u, 0x10000u, 30u);
        const int second = createThread(kernel, memory, 0x1020u, 0x11000u, 20u);
        const int outside = createThread(kernel, memory, 0x1040u, 0x20000u, 10u);
        startThread(kernel, first);
        startThread(kernel, second);

        uint32_t firstStack = 0u;
        uint32_t secondStack = 0u;
        uint32_t outsideStack = 0u;
        IopThread *thread = kernel.beginNextReady(0u);
        require(thread && thread->id == second, "priority selection was incorrect before range termination");
        secondStack = thread->stackBase;
        kernel.sleepCurrent(thread->cpu);
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        thread = kernel.beginNextReady(0u);
        require(thread && thread->id == first, "second targeted thread was not selected");
        firstStack = thread->stackBase;
        kernel.sleepCurrent(thread->cpu);
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        startThread(kernel, outside);
        thread = kernel.beginNextReady(0u);
        require(thread && thread->id == outside, "out-of-range thread did not start");
        outsideStack = thread->stackBase;
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);

        kernel.terminateThreadsInRange(0x10000u, 0x2000u);
        kernel.cleanupDeadThreads(); // Explicit cleanup must also consume the pending-dead marker.
        require(kernel.threadCount() == 1u, "range termination removed the wrong number of threads");
        require(!memory.allocationContaining(firstStack).has_value(), "range-terminated first stack was not freed");
        require(!memory.allocationContaining(secondStack).has_value(), "range-terminated second stack was not freed");
        require(memory.allocationContaining(outsideStack).has_value(), "out-of-range thread stack was freed");

        thread = kernel.beginNextReady(0u);
        require(thread && thread->id == outside, "range-excluded thread was not schedulable");
        kernel.terminateThreadsInRange(0x20000u, 0x1000u);
        require(kernel.threadCount() == 1u, "current thread was erased before its timeslice ended");
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        require(kernel.threadCount() == 0u, "range-terminated current thread survived endTimeslice");
        require(!memory.allocationContaining(outsideStack).has_value(), "range-terminated current stack was not freed");
    }

    void testPriorityAndDelaySchedulingWithoutDeadThreads()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        const int lowPriorityId = createThread(kernel, memory, 0x1000u, 0x10000u, 20u);
        const int firstTieId = createThread(kernel, memory, 0x1020u, 0x11000u, 10u);
        const int secondTieId = createThread(kernel, memory, 0x1040u, 0x12000u, 10u);
        startThread(kernel, lowPriorityId);
        startThread(kernel, firstTieId);
        startThread(kernel, secondTieId);

        IopThread *thread = kernel.beginNextReady(0u);
        require(thread && thread->id == firstTieId, "equal-priority scheduling did not choose the lower id");
        kernel.sleepCurrent(thread->cpu);
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);

        thread = kernel.beginNextReady(0u);
        require(thread && thread->id == secondTieId, "second equal-priority thread was not selected");
        kernel.delayCurrentUntil(50u, thread->cpu);
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);

        thread = kernel.beginNextReady(0u);
        require(thread && thread->id == lowPriorityId, "lower-priority ready thread was not selected");
        kernel.sleepCurrent(thread->cpu);
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);

        require(kernel.beginNextReady(0u) == nullptr, "sleeping/delayed threads were selected early");
        require(kernel.nextWakeCycle(100u) == 50u, "delay wake cycle changed");
        require(kernel.beginNextReady(49u) == nullptr, "delayed thread woke before its deadline");
        thread = kernel.beginNextReady(50u);
        require(thread && thread->id == secondTieId, "delayed thread did not wake at its deadline");
        kernel.endTimeslice(*thread, 0xFFFFFFFCu);
        require(kernel.threadCount() == 3u, "no-dead scheduling cleanup removed a live thread");
    }
}

int main()
{
    try
    {
        testExitDeleteReleasesStack();
        testExitThreadCanBeRestarted();
        testRangeTerminationRemovesOnlyMatches();
        testPriorityAndDelaySchedulingWithoutDeadThreads();
        std::cout << "IOP kernel lifecycle tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        std::cerr << "IOP kernel lifecycle test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
