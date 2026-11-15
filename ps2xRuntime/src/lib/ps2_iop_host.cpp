#include "ps2_iop_host.h"

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "runtime/ps2_memory.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Syscalls/Common.h"
#include "ThreadNaming.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#endif
#if defined(PLATFORM_VITA)
#include <psp2/kernel/threadmgr.h>
#elif !defined(_WIN32)
#include <time.h>
#endif

namespace
{
    // PS2X_IOP_THREAD=1: this thread is the IOP worker / holds the IOP lock / is the EE executor (the one that posts cycles).
    thread_local bool t_iopWorker = false;
    thread_local bool t_iopHolds = false;
    thread_local bool t_iopExecutor = false;
    thread_local uint64_t t_holdStart = 0; // (diagnostic) when the current IopSync got the lock
    thread_local size_t t_holdKind = 0;
}

PS2IopHostAdapter::CallScope::CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram)
    : m_lock(owner.m_callMutex),
      m_owner(&owner),
      m_previousContext(owner.m_activeContext),
      m_previousRdram(owner.m_activeRdram),
      m_previousToken(owner.m_activeToken)
{
    m_token = owner.m_nextToken++;
    if (m_token == 0)
    {
        m_token = owner.m_nextToken++;
    }
    owner.m_activeContext = context;
    owner.m_activeRdram = rdram;
    owner.m_activeToken = m_token;
}

PS2IopHostAdapter::CallScope::~CallScope()
{
    release();
}

PS2IopHostAdapter::CallScope::CallScope(CallScope &&other) noexcept
    : m_lock(std::move(other.m_lock)),
      m_owner(std::exchange(other.m_owner, nullptr)),
      m_previousContext(other.m_previousContext),
      m_previousRdram(other.m_previousRdram),
      m_previousToken(other.m_previousToken),
      m_token(other.m_token)
{
}

PS2IopHostAdapter::CallScope &PS2IopHostAdapter::CallScope::operator=(CallScope &&other) noexcept
{
    if (this != &other)
    {
        release();
        m_lock = std::move(other.m_lock);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_previousContext = other.m_previousContext;
        m_previousRdram = other.m_previousRdram;
        m_previousToken = other.m_previousToken;
        m_token = other.m_token;
    }
    return *this;
}

void PS2IopHostAdapter::CallScope::release()
{
    if (m_owner)
    {
        m_owner->m_activeContext = m_previousContext;
        m_owner->m_activeRdram = m_previousRdram;
        m_owner->m_activeToken = m_previousToken;
        m_owner = nullptr;
    }
}

PS2IopHostAdapter::PS2IopHostAdapter(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

PS2IopHostAdapter::~PS2IopHostAdapter()
{
    stopIopThread(); // before the host files go: the worker may still be inside a CDVD read
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    for (auto &[handle, file] : m_hostFiles)
    {
        (void)handle;
        if (file.stream)
        {
            std::fclose(file.stream);
        }
    }
    m_hostFiles.clear();
}

PS2IopHostAdapter::CallScope PS2IopHostAdapter::enterCall(R5900Context *context, uint8_t *rdram)
{
    return CallScope(*this, context, rdram);
}

uint8_t *PS2IopHostAdapter::activeRdram() const
{
    // The IOP worker never sees the context of an RPC the executor is inside of.
    if (!t_iopWorker && m_activeRdram)
        return m_activeRdram;
    return m_runtime.memory().getRDRAM();
}

bool PS2IopHostAdapter::guestRange(uint32_t address, size_t size, uint8_t *&begin) const
{
    begin = nullptr;
    uint8_t *const rdram = activeRdram();
    if (!rdram)
    {
        return false;
    }
    if (size == 0)
    {
        begin = getMemPtr(rdram, address);
        return begin != nullptr;
    }
    if (size - 1 > std::numeric_limits<uint32_t>::max() - address)
    {
        return false;
    }
    uint8_t *const first = getMemPtr(rdram, address);
    uint8_t *const last = getMemPtr(rdram, address + static_cast<uint32_t>(size - 1));
    if (!first || !last || last < first || static_cast<size_t>(last - first) != size - 1)
    {
        return false;
    }
    begin = first;
    return true;
}

bool PS2IopHostAdapter::readGuest(uint32_t address, void *destination, size_t size) const
{
    if (!destination && size != 0)
    {
        return false;
    }
    uint8_t *source = nullptr;
    if (!guestRange(address, size, source))
    {
        return false;
    }
    if (size != 0)
    {
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::writeGuestNow(uint32_t address, const void *source, size_t size)
{
    if (!source && size != 0)
    {
        return false;
    }
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = activeRdram();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::writeGuest", nullptr);
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::zeroGuestNow(uint32_t address, size_t size)
{
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = activeRdram();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::zeroGuest", nullptr);
        std::memset(destination, 0, size);
    }
    return true;
}

bool PS2IopHostAdapter::writeGuest(uint32_t address, const void *source, size_t size)
{
    if (t_iopWorker)
    {
        // On the IOP thread the EE's RAM is only written by the EE thread: validate now, apply in order later.
        if (!source && size != 0)
            return false;
        uint8_t *destination = nullptr;
        if (!guestRange(address, size, destination))
            return false;
        if (size != 0)
            queueDeferred(0u, address, static_cast<uint32_t>(size), source, size);
        return true;
    }
    return writeGuestNow(address, source, size);
}

bool PS2IopHostAdapter::zeroGuest(uint32_t address, size_t size)
{
    if (t_iopWorker)
    {
        uint8_t *destination = nullptr;
        if (!guestRange(address, size, destination))
            return false;
        if (size != 0)
            queueDeferred(1u, address, static_cast<uint32_t>(size), nullptr, 0u);
        return true;
    }
    return zeroGuestNow(address, size);
}

bool PS2IopHostAdapter::normalizeGuestAddress(uint32_t address, uint32_t &normalized) const
{
    bool scratchpad = false;
    if (!ps2ResolveGuestPointer(address, normalized, scratchpad) || scratchpad)
    {
        normalized = 0;
        return false;
    }
    return true;
}

bool PS2IopHostAdapter::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_runtime.readIopMemory(address, destination, size);
}

bool PS2IopHostAdapter::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_runtime.writeIopMemory(address, source, size);
}

bool PS2IopHostAdapter::zeroIopMemory(uint32_t address, size_t size)
{
    return m_runtime.zeroIopMemory(address, size);
}

bool PS2IopHostAdapter::normalizeIopAddress(uint32_t address, uint32_t &normalized) const
{
    if (!m_runtime.isIopMemoryRange(address, 0u))
    {
        normalized = 0u;
        return false;
    }
    normalized = address & 0x1FFFFFFFu;
    return true;
}

uint32_t PS2IopHostAdapter::allocateIopHandle(ps2x::iop::IopHandleKind kind)
{
    uint8_t *const rdram = activeRdram();
    if (!rdram)
    {
        return 0;
    }
    return kind == ps2x::iop::IopHandleKind::RpcPacket
               ? rpcAllocPacketAddr(rdram)
               : rpcAllocServerAddr(rdram);
}

uint32_t PS2IopHostAdapter::allocateGuest(uint32_t size, uint32_t alignment)
{
    return m_runtime.guestMalloc(size, alignment);
}

void PS2IopHostAdapter::freeGuest(uint32_t address)
{
    m_runtime.guestFree(address);
}

void PS2IopHostAdapter::audioCommand(uint32_t sid,
                                     uint32_t function,
                                     ps2x::iop::GuestBuffer send,
                                     ps2x::iop::GuestBuffer receive)
{
    uint8_t *sendPointer = nullptr;
    uint8_t *receivePointer = nullptr;
    if (send.address && !guestRange(send.address, send.size, sendPointer))
    {
        sendPointer = nullptr;
    }
    if (receive.address && !guestRange(receive.address, receive.size, receivePointer))
    {
        receivePointer = nullptr;
    }
    m_runtime.audioBackend().onSoundCommand(sid,
                                            function,
                                            sendPointer,
                                            send.size,
                                            receivePointer,
                                            receive.size);
}

std::string PS2IopHostAdapter::hostPath(ps2x::iop::HostPathKind kind) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    switch (kind)
    {
    case ps2x::iop::HostPathKind::CdRoot:
        return paths.cdRoot.string();
    case ps2x::iop::HostPathKind::CdImage:
        return paths.cdImage.string();
    case ps2x::iop::HostPathKind::HostRoot:
        return paths.hostRoot.string();
    case ps2x::iop::HostPathKind::MemoryCardRoot:
        return paths.mcRoot.string();
    case ps2x::iop::HostPathKind::ElfDirectory:
    default:
        return paths.elfDirectory.string();
    }
}

std::string PS2IopHostAdapter::translateGuestPath(std::string_view path) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
    std::filesystem::path hostPath;
    if (!m_runtime.vfs().resolveHostPath(path, mounts, hostPath))
        return {};
    return hostPath.string();
}

uint64_t PS2IopHostAdapter::openHostFile(std::string_view path)
{
    if (path.empty())
    {
        return 0u;
    }

    const std::filesystem::path hostPath{std::string(path)};
#if defined(_WIN32)
    std::FILE *stream = ::_wfopen(hostPath.c_str(), L"rb");
#else
    std::FILE *stream = std::fopen(hostPath.string().c_str(), "rb");
#endif
    if (!stream)
    {
        return 0u;
    }

    std::error_code error;
    const uint64_t size = std::filesystem::file_size(hostPath, error);
    if (error)
    {
        std::fclose(stream);
        return 0u;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    uint64_t handle = m_nextHostFileHandle++;
    if (handle == 0u)
    {
        handle = m_nextHostFileHandle++;
    }
    m_hostFiles.emplace(handle, HostFile{stream, size});
    return handle;
}

bool PS2IopHostAdapter::hostFileSize(uint64_t handle, uint64_t &size) const
{
    size = 0u;
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }
    size = it->second.size;
    return true;
}

extern "C" uint64_t ps2xFrameHistNowUs();                   // EeScheduler.cpp
extern "C" void ps2xFrameHistNoteRead(uint64_t startUs, size_t bytes); // EeScheduler.cpp

bool PS2IopHostAdapter::readHostFile(uint64_t handle,
                                     uint64_t offset,
                                     void *destination,
                                     size_t size,
                                     size_t &bytesRead)
{
    bytesRead = 0u;
    if (!destination && size != 0u)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }

#if defined(_WIN32)
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        _fseeki64(it->second.stream, static_cast<int64_t>(offset), SEEK_SET) != 0)
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(it->second.stream, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
    {
        return false;
    }

    const uint64_t ioStart = ps2xFrameHistNowUs(); // PS2X_FRAME_HIST / PS2X_STALL_DIAG: time and size of host file reads
    bytesRead = size == 0u
                    ? 0u
                    : std::fread(destination, 1u, size, it->second.stream);
    ps2xFrameHistNoteRead(ioStart, size);
    if (bytesRead < size && std::ferror(it->second.stream))
    {
        std::clearerr(it->second.stream);
        return false;
    }
    return true;
}

void PS2IopHostAdapter::closeHostFile(uint64_t handle)
{
    std::FILE *stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_hostFileMutex);
        const auto it = m_hostFiles.find(handle);
        if (it == m_hostFiles.end())
        {
            return;
        }
        stream = it->second.stream;
        m_hostFiles.erase(it);
    }
    if (stream)
    {
        std::fclose(stream);
    }
}

int32_t PS2IopHostAdapter::memoryCard(const ps2x::iop::MemoryCardRequest &request)
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    Handler handler = nullptr;
    switch (request.operation)
    {
    case ps2x::iop::MemoryCardOperation::Init:
        handler = ps2_stubs::sceMcInit;
        break;
    case ps2x::iop::MemoryCardOperation::GetInfo:
        handler = ps2_stubs::sceMcGetInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Open:
        handler = ps2_stubs::sceMcOpen;
        break;
    case ps2x::iop::MemoryCardOperation::Close:
        handler = ps2_stubs::sceMcClose;
        break;
    case ps2x::iop::MemoryCardOperation::Seek:
        handler = ps2_stubs::sceMcSeek;
        break;
    case ps2x::iop::MemoryCardOperation::Read:
        handler = ps2_stubs::sceMcRead;
        break;
    case ps2x::iop::MemoryCardOperation::Write:
        handler = ps2_stubs::sceMcWrite;
        break;
    case ps2x::iop::MemoryCardOperation::Flush:
        handler = ps2_stubs::sceMcFlush;
        break;
    case ps2x::iop::MemoryCardOperation::Chdir:
        handler = ps2_stubs::sceMcChdir;
        break;
    case ps2x::iop::MemoryCardOperation::GetDir:
        handler = ps2_stubs::sceMcGetDir;
        break;
    case ps2x::iop::MemoryCardOperation::SetFileInfo:
        handler = ps2_stubs::sceMcSetFileInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Delete:
        handler = ps2_stubs::sceMcDelete;
        break;
    case ps2x::iop::MemoryCardOperation::Format:
        handler = ps2_stubs::sceMcFormat;
        break;
    case ps2x::iop::MemoryCardOperation::Unformat:
        handler = ps2_stubs::sceMcUnformat;
        break;
    case ps2x::iop::MemoryCardOperation::Mkdir:
        handler = ps2_stubs::sceMcMkdir;
        break;
    }
    if (!handler)
    {
        return -1;
    }

    R5900Context context{};
    for (size_t i = 0; i < 4u; ++i)
    {
        setRegU32(&context, static_cast<int>(4 + i), request.arguments[i]);
    }

    // EE n32 ABI: the fifth argument travels in $t0, matching the sceMc* stubs.
    setRegU32(&context, 8, request.arguments[4]);

    handler(activeRdram(),
            &context,
            &m_runtime);
    return ps2_stubs::getMemoryCardDebugSnapshot().lastResult;
}

bool PS2IopHostAdapter::hasGuestFunction(uint32_t address) const
{
    return m_runtime.hasFunction(address);
}

bool PS2IopHostAdapter::invokeGuestFunction(uint64_t callToken,
                                            uint32_t address,
                                            uint32_t a0,
                                            uint32_t a1,
                                            uint32_t a2,
                                            uint32_t a3,
                                            uint32_t *resultAddress)
{
    (void)callToken;
    (void)address;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    if (resultAddress)
    {
        *resultAddress = 0u;
    }
    return false;
}

bool PS2IopHostAdapter::sendSifCommand(uint32_t commandId,
                                       const void *packet,
                                       size_t packetSize)
{
    if (t_iopWorker)
    {
        // dispatchSifCommand queues a guest invocation in the EE scheduler, which only its executor may do.
        if (!packet || packetSize < 16u || packetSize > 112u)
            return false;
        queueDeferred(2u, commandId, 0u, packet, packetSize);
        return true;
    }
    uint8_t *const rdram = activeRdram();
    return ps2_stubs::dispatchSifCommand(rdram,
                                         &m_runtime,
                                         commandId,
                                         packet,
                                         packetSize);
}

void PS2IopHostAdapter::log(ps2x::iop::LogLevel level, std::string_view message)
{
    const char *prefix = "[ps2xIOP]";
    if (level == ps2x::iop::LogLevel::Warning)
    {
        prefix = "[ps2xIOP:warning]";
    }
    else if (level == ps2x::iop::LogLevel::Error)
    {
        prefix = "[ps2xIOP:error]";
    }
    std::cerr << prefix << ' ' << message << std::endl;
}

// ---------------------------------------------------------------------------------------------------------------------
// The IOP on its own thread (PS2X_IOP_THREAD=1). See the comment in ps2_iop_host.h for the scheme.
//
//   postedLocal  EE cycles the executor has accounted so far (executor only, exact).
//   posted       the same, published to the worker about once per wake quantum (a stale value only makes the worker
//                run a little less ahead, never past the EE).
//   delivered  EE cycles already handed to the IOP (under runMutex, mirrored in deliveredMirror for the wait predicate).
//   runMutex   the IOP's single executor: held by the worker while it runs a slice, and by the executor (or any other
//              thread) for the duration of an IopSync.
// The worker runs the IOP from `delivered` towards `posted` in slices of at most maxSlice EE cycles, releasing the lock
// between them, and sleeps once it has caught up until posted is a wake quantum ahead. It never runs past `posted`.
// ---------------------------------------------------------------------------------------------------------------------
struct PS2IopHostAdapter::IopThread
{
    struct Deferred
    {
        uint32_t kind = 0; // 0 write guest RAM, 1 zero guest RAM, 2 SIF command packet
        uint32_t a = 0;
        uint32_t b = 0;
        std::vector<uint8_t> bytes;
    };
    struct SyncStat
    {
        std::atomic<uint64_t> count{0}, waitUs{0}, maxWaitUs{0}, tailUs{0}, maxTailUs{0}, tailEe{0}, holdUs{0}, maxHoldUs{0};
    };

    PS2IopHostAdapter &owner;
    std::function<void(uint64_t)> run;
    std::mutex runMutex;
    std::atomic<uint64_t> posted{0};
    uint64_t postedLocal = 0; // executor only
    uint64_t delivered = 0;
    std::atomic<uint64_t> deliveredMirror{0};
    uint64_t lastNotified = 0; // executor only
    std::atomic<int> eeWaiting{0};
    std::mutex wakeMutex;
    std::condition_variable wakeCv;
    std::atomic<bool> sleeping{false};
    std::atomic<bool> stop{false};
    std::thread thread;
    bool started = false; // executor only
    std::mutex deferredMutex;
    std::vector<Deferred> deferred;
    std::atomic<uint32_t> deferredCount{0};
    uint64_t wakeEe = 294912u;  // EE cycles (1 ms) the executor posts before it wakes the worker
    uint64_t sliceEe = 294912u; // longest run with the lock held (1 ms of guest time)
    bool diag = false;

    // counters (PS2X_IOP_THREAD_DIAG=1: one line every 5 s from the worker)
    SyncStat syncs[static_cast<size_t>(IopSyncKind::Count)];
    std::atomic<uint64_t> slices{0}, sliceEeTotal{0}, sliceUs{0}, maxSliceUs{0}, wakes{0}, posts{0};
    std::atomic<uint64_t> defOps{0}, defBytes{0}, drains{0}, drainUs{0};

    static uint64_t nowUs()
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    }
    static uint64_t threadCpuUs()
    {
#if defined(PLATFORM_VITA)
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetThreadInfo(sceKernelGetThreadId(), &info) < 0)
            return 0u;
        return info.runClocks; // microseconds
#elif !defined(_WIN32)
        timespec ts{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
            return 0u;
        return static_cast<uint64_t>(ts.tv_sec) * 1000000u + static_cast<uint64_t>(ts.tv_nsec) / 1000u;
#else
        return 0u;
#endif
    }
    static void raiseMax(std::atomic<uint64_t> &slot, uint64_t value)
    {
        uint64_t old = slot.load(std::memory_order_relaxed);
        while (value > old && !slot.compare_exchange_weak(old, value, std::memory_order_relaxed))
        {
        }
    }

    IopThread(PS2IopHostAdapter &o, std::function<void(uint64_t)> r)
        : owner(o), run(std::move(r))
    {
        if (const char *v = std::getenv("PS2X_IOP_THREAD_WAKE_US"))
            wakeEe = std::max<uint64_t>(1u, std::strtoull(v, nullptr, 10)) * 295u;
        if (const char *v = std::getenv("PS2X_IOP_THREAD_SLICE_US"))
            sliceEe = std::max<uint64_t>(1u, std::strtoull(v, nullptr, 10)) * 295u;
        if (const char *v = std::getenv("PS2X_IOP_THREAD_DIAG"))
            diag = v[0] == '1';
    }

    ~IopThread() { shutdown(); }

    void shutdown()
    {
        if (!thread.joinable())
            return;
        stop.store(true);
        {
            std::lock_guard<std::mutex> lock(wakeMutex);
        }
        wakeCv.notify_all();
        thread.join();
    }

    void startWorker()
    {
        started = true;
        thread = std::thread([this] { workerMain(); });
    }

    void applyDeferred(Deferred &op)
    {
        switch (op.kind)
        {
        case 0:
            (void)owner.writeGuestNow(op.a, op.bytes.data(), op.bytes.size());
            break;
        case 1:
            (void)owner.zeroGuestNow(op.a, op.b);
            break;
        default:
            (void)ps2_stubs::dispatchSifCommand(owner.activeRdram(), &owner.m_runtime, op.a, op.bytes.data(), op.bytes.size());
            break;
        }
    }

    // Executor only.
    void drainDeferred()
    {
        std::vector<Deferred> work;
        {
            std::lock_guard<std::mutex> lock(deferredMutex);
            work.swap(deferred);
            deferredCount.store(0u, std::memory_order_relaxed);
        }
        if (work.empty())
            return;
        const uint64_t t0 = diag ? nowUs() : 0u;
        for (Deferred &op : work)
            applyDeferred(op);
        if (diag)
        {
            drains.fetch_add(1, std::memory_order_relaxed);
            drainUs.fetch_add(nowUs() - t0, std::memory_order_relaxed);
        }
    }

    void post(uint64_t eeCycles)
    {
        t_iopExecutor = true;
        if (!started)
            startWorker();
        const uint64_t p = (postedLocal += eeCycles);
        if (deferredCount.load(std::memory_order_relaxed) != 0u)
            drainDeferred();
        // Called tens of thousands of times a second: the atomics and the wake are only paid once per wake quantum.
        if (p - lastNotified >= wakeEe)
        {
            lastNotified = p;
            posted.store(p); // seq_cst: pairs with the worker's sleeping flag (see workerMain)
            if (sleeping.load())
            {
                {
                    std::lock_guard<std::mutex> lock(wakeMutex); // the worker is in wait() or has not tested yet
                }
                wakeCv.notify_one();
                if (diag)
                    wakes.fetch_add(1, std::memory_order_relaxed);
            }
            if (diag)
                posts.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void enter(IopSyncKind kind, bool catchUp)
    {
        if (t_iopHolds)
            return; // nested (an IOP -> host callback that calls back into the runtime), or the worker itself
        const uint64_t t0 = diag ? nowUs() : 0u;
        eeWaiting.fetch_add(1);
        runMutex.lock();
        eeWaiting.fetch_sub(1);
        t_iopHolds = true;
        const uint64_t t1 = diag ? nowUs() : 0u;
        if (catchUp && t_iopExecutor)
        {
            drainDeferred();
            const uint64_t p = postedLocal;
            if (p > delivered)
            {
                const uint64_t delta = p - delivered;
                run(delta);
                delivered = p;
                deliveredMirror.store(p, std::memory_order_release);
                if (diag)
                {
                    SyncStat &st = syncs[static_cast<size_t>(kind)];
                    st.tailEe.fetch_add(delta, std::memory_order_relaxed);
                    const uint64_t us = nowUs() - t1;
                    st.tailUs.fetch_add(us, std::memory_order_relaxed);
                    raiseMax(st.maxTailUs, us);
                }
            }
        }
        if (diag)
        {
            t_holdStart = t1;
            t_holdKind = static_cast<size_t>(kind);
            SyncStat &st = syncs[static_cast<size_t>(kind)];
            st.count.fetch_add(1, std::memory_order_relaxed);
            st.waitUs.fetch_add(t1 - t0, std::memory_order_relaxed);
            raiseMax(st.maxWaitUs, t1 - t0);
        }
    }

    void leave()
    {
        if (diag)
        {
            // everything the executor did inside the sync: the tail run, then the RPC / transfer / memory access itself
            SyncStat &st = syncs[t_holdKind];
            const uint64_t us = nowUs() - t_holdStart;
            st.holdUs.fetch_add(us, std::memory_order_relaxed);
            raiseMax(st.maxHoldUs, us);
        }
        t_iopHolds = false;
        runMutex.unlock();
    }

    void setupWorkerThread()
    {
#if defined(PLATFORM_VITA)
        // std::thread starts at the lowest user priority (191). The IOP thread works for the game thread: a little
        // above it (smaller number), on a core of its own (not the game's core; PS2X_VITA_IOP_CORE=0..2 picks it).
        const char *gameValue = std::getenv("PS2X_VITA_GAME_CORE");
        const int gameCore = gameValue ? std::atoi(gameValue) : 1;
        const char *iopValue = std::getenv("PS2X_VITA_IOP_CORE");
        int core = iopValue ? std::atoi(iopValue) : -1;
        if (core < 0 || core > 2 || core == gameCore)
        {
            core = -1;
            for (int c = 0; c <= 2; ++c)
                if (c != gameCore)
                {
                    core = c;
                    break;
                }
        }
        const SceUID self = sceKernelGetThreadId();
        sceKernelChangeThreadPriority(self, 120);
        if (core >= 0)
            sceKernelChangeThreadCpuAffinityMask(self, 0x10000 << core);
#endif
    }

    void report(uint64_t &lastReportUs)
    {
        const uint64_t now = nowUs();
        if (now - lastReportUs < 5000000u)
            return;
        const double wall = static_cast<double>(now - lastReportUs) / 1e6;
        lastReportUs = now;
        static uint64_t lastCpu = 0;
        const uint64_t cpu = threadCpuUs();
        char line[1024];
        int n = std::snprintf(line, sizeof(line),
                              "[iopthread] %.1fs worker cpu %.0f%% slices %llu (%.2f guest ms avg, %.0f us avg, max %llu us) posts %llu wakes %llu defer ops %llu bytes %llu drain %llu/%llu us |",
                              wall, (cpu - lastCpu) * 100.0 / (wall * 1e6), (unsigned long long)slices.load(),
                              slices.load() ? sliceEeTotal.load() / 294912.0 / slices.load() : 0.0,
                              slices.load() ? (double)sliceUs.load() / slices.load() : 0.0, (unsigned long long)maxSliceUs.load(),
                              (unsigned long long)posts.load(), (unsigned long long)wakes.load(),
                              (unsigned long long)defOps.load(), (unsigned long long)defBytes.load(),
                              (unsigned long long)drains.load(), (unsigned long long)drainUs.load());
        lastCpu = cpu;
        static const char *const names[] = {"rpc", "sif", "mem", "mod", "reset", "query"};
        for (size_t k = 0; k < static_cast<size_t>(IopSyncKind::Count) && n > 0 && n < static_cast<int>(sizeof(line)) - 160; ++k)
        {
            SyncStat &st = syncs[k];
            const uint64_t count = st.count.exchange(0);
            n += std::snprintf(line + n, sizeof(line) - n, " %s %llu wait %llu us (max %llu) tail %llu us (max %llu, %llu ee-cyc) hold %llu us (max %llu) |",
                               names[k], (unsigned long long)count, (unsigned long long)st.waitUs.exchange(0),
                               (unsigned long long)st.maxWaitUs.exchange(0), (unsigned long long)st.tailUs.exchange(0),
                               (unsigned long long)st.maxTailUs.exchange(0), (unsigned long long)st.tailEe.exchange(0),
                               (unsigned long long)st.holdUs.exchange(0), (unsigned long long)st.maxHoldUs.exchange(0));
        }
        slices = 0; sliceEeTotal = 0; sliceUs = 0; maxSliceUs = 0; posts = 0; wakes = 0; defOps = 0; defBytes = 0; drains = 0; drainUs = 0;
        std::fprintf(stderr, "%s\n", line);
        std::fflush(stderr);
    }

    void workerMain()
    {
        ThreadNaming::SetCurrentThreadName("IopThread");
        t_iopWorker = true;
        t_iopHolds = true; // every host callback the IOP makes runs with the lock held
        setupWorkerThread();
        uint64_t lastReportUs = nowUs();
        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(wakeMutex);
                sleeping.store(true);
                wakeCv.wait(lock, [this]
                            { return stop.load() || posted.load() - deliveredMirror.load() >= wakeEe; });
                sleeping.store(false);
            }
            for (;;)
            {
                if (stop.load())
                    return;
                // Let a waiting EE thread in between two slices (a std::mutex is not fair).
                while (eeWaiting.load() != 0 && !stop.load())
                    std::this_thread::yield();
                std::unique_lock<std::mutex> lock(runMutex);
                const uint64_t p = posted.load();
                if (p <= delivered)
                    break;
                const uint64_t delta = std::min<uint64_t>(p - delivered, sliceEe);
                const uint64_t t0 = diag ? nowUs() : 0u;
                run(delta);
                delivered += delta;
                deliveredMirror.store(delivered, std::memory_order_release);
                if (diag)
                {
                    const uint64_t us = nowUs() - t0;
                    slices.fetch_add(1, std::memory_order_relaxed);
                    sliceEeTotal.fetch_add(delta, std::memory_order_relaxed);
                    sliceUs.fetch_add(us, std::memory_order_relaxed);
                    raiseMax(maxSliceUs, us);
                }
            }
            if (diag)
                report(lastReportUs);
        }
    }
};

void PS2IopHostAdapter::queueDeferred(uint32_t kind, uint32_t a, uint32_t b, const void *data, size_t bytes)
{
    IopThread::Deferred op;
    op.kind = kind;
    op.a = a;
    op.b = b;
    if (data && bytes)
        op.bytes.assign(static_cast<const uint8_t *>(data), static_cast<const uint8_t *>(data) + bytes);
    if (m_iopThread->diag)
    {
        m_iopThread->defOps.fetch_add(1, std::memory_order_relaxed);
        m_iopThread->defBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(m_iopThread->deferredMutex);
    m_iopThread->deferred.push_back(std::move(op));
    m_iopThread->deferredCount.fetch_add(1u, std::memory_order_relaxed);
}

void PS2IopHostAdapter::configureIopThread(std::function<void(uint64_t)> run)
{
    const char *value = std::getenv("PS2X_IOP_THREAD");
    if (!value || value[0] != '1')
        return;
    m_iopThread = std::make_unique<IopThread>(*this, std::move(run));
    std::fprintf(stderr, "[iopthread] the IOP runs on its own thread (wake %llu EE cycles, slice %llu)\n",
                 static_cast<unsigned long long>(m_iopThread->wakeEe), static_cast<unsigned long long>(m_iopThread->sliceEe));
}

void PS2IopHostAdapter::postIopCycles(uint64_t eeCycles) noexcept
{
    m_iopThread->post(eeCycles);
}

PS2IopHostAdapter::IopSync PS2IopHostAdapter::syncIop(IopSyncKind kind, bool catchUp)
{
    if (!m_iopThread || t_iopHolds)
        return IopSync();
    m_iopThread->enter(kind, catchUp);
    return IopSync(this);
}

void PS2IopHostAdapter::IopSync::release()
{
    if (m_owner)
    {
        m_owner->m_iopThread->leave();
        m_owner = nullptr;
    }
}

PS2IopHostAdapter::IopSync::~IopSync()
{
    release();
}

void PS2IopHostAdapter::stopIopThread()
{
    if (m_iopThread)
        m_iopThread->shutdown();
}
