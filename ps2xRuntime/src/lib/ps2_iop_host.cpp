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
#include <psp2/kernel/processmgr.h>
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
    thread_local uint64_t t_holdWait = 0; // (diagnostic) how long the current IopSync waited for the lock
}
static std::atomic<PS2IopHostAdapter *> g_frameCounterOwner{nullptr}; // for ps2xIopFrameCounters (PS2X_FRAME_LIST)
// Lock-acquisition counters for the PS2X_IOP_STATS line: plain integers, each written only while holding the mutex it
// counts (so exact), read without it by the reporter (a torn read of a 5 s sample is harmless).
static uint64_t g_hostFileLocks = 0; // m_hostFileMutex
static uint64_t g_callLocks = 0;     // m_callMutex
extern "C" void ps2xSpu2Counters(uint64_t *out); // spu2.cpp: [outLocks, streamLocks, runCalls, rendered, bulk, chunks, services, deferred, forced, catchUp, renderUs]
extern "C" void ps2xSpu2SetTiming(int enabled);   // spu2.cpp: time the rendering (PS2X_IOP_STATS)

PS2IopHostAdapter::CallScope::CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram)
    : m_lock(owner.m_callMutex),
      m_owner(&owner),
      m_previousContext(owner.m_activeContext),
      m_previousRdram(owner.m_activeRdram),
      m_previousToken(owner.m_activeToken)
{
    ++g_callLocks;
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
    ++g_hostFileLocks;
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
    ++g_hostFileLocks;
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
    bool timing = false; // diag, or PS2X_FRAME_LIST: time the syncs (cumulative totals for the per-frame [fdip] lines)
    std::atomic<uint64_t> totCount{0}, totWaitUs{0}, totTailUs{0}, totHoldUs{0}, totTailEe{0};

    // counters (PS2X_IOP_THREAD_DIAG=1: one line every 5 s from the worker)
    SyncStat syncs[static_cast<size_t>(IopSyncKind::Count)];
    std::atomic<uint64_t> slices{0}, sliceEeTotal{0}, sliceUs{0}, maxSliceUs{0}, wakes{0}, posts{0};
    std::atomic<uint64_t> defOps{0}, defBytes{0}, drains{0}, drainUs{0};

    // PS2X_IOP_STATS=1: one [iopstats] line every 5 s from the worker. Everything below is written under runMutex (by
    // the thread that holds it: the worker in its slice, or any thread inside an IopSync), except where noted, and read
    // by the worker with runMutex held, so plain integers are exact. No clock is read per instruction: the worker reads
    // one per slice (~1000/s) and a sync reads two.
    bool stats = false;
    bool wakeFix = true; // PS2X_IOP_WAKE_FIX=0 brings back the wrapping wake predicate (see workerMain)
    // PS2X_IOP_NOCATCHUP=<mask> (default 0 = exact): syncs of these kinds do not run the IOP's backlog inline before they
    // act, they work on the IOP as the worker has left it (at most a few ms of guest time behind the EE) and the worker
    // catches up afterwards. bit 0 Rpc, 1 Sif, 2 Mem, 3 Module, 4 Reset, 5 Query (IopSyncKind order). Cheaper for the game
    // thread, but an RPC then runs at an earlier IOP time than the EE's, so sound commands of a frame bunch up in the
    // IOP's timeline and the run is no longer reproducible; hence off unless asked for.
    uint32_t noCatchUpMask = 0;
    static constexpr uint64_t kBucketUs = 250u;
    static constexpr size_t kBuckets = 161u; // 0 .. 40 ms in 0.25 ms steps, the last one is "40 ms and more"
    struct Hist
    {
        uint32_t b[kBuckets] = {};
        uint64_t n = 0, maxUs = 0, sumUs = 0;
        void add(uint64_t us)
        {
            size_t i = static_cast<size_t>(us / kBucketUs);
            if (i >= kBuckets)
                i = kBuckets - 1u;
            ++b[i];
            ++n;
            sumUs += us;
            if (us > maxUs)
                maxUs = us;
        }
        double pctMs(double p) const // upper edge of the bucket holding the p-th fraction
        {
            if (n == 0u)
                return 0.0;
            const uint64_t need = static_cast<uint64_t>(static_cast<double>(n) * p + 0.999999);
            uint64_t acc = 0;
            for (size_t i = 0; i < kBuckets; ++i)
            {
                acc += b[i];
                if (acc >= need)
                    return static_cast<double>(i + 1u) * kBucketUs / 1000.0;
            }
            return static_cast<double>(kBuckets) * kBucketUs / 1000.0;
        }
        double meanMs() const { return n ? static_cast<double>(sumUs) / n / 1000.0 : 0.0; }
        void clear() { *this = Hist{}; }
    };
    struct KindStat
    {
        uint64_t n = 0, waitUs = 0, maxWaitUs = 0, holdUs = 0, maxHoldUs = 0, tailRuns = 0, tailUs = 0, maxTailUs = 0;
    };
    Hist lagExec;    // guest us the IOP was behind the EE when a sync wanted it caught up
    Hist lagWorker;  // the same at the start of each worker slice (what the worker still had to do)
    Hist wakeLat;    // wall us from the executor publishing `posted` to the worker starting the slice (scheduling delay)
    Hist rpcRound;   // wall us of an Rpc sync: lock wait + tail run + the handler (the game thread is blocked all along)
    KindStat kinds[static_cast<size_t>(IopSyncKind::Count)];
    // size of the cycle lumps the EE hands over (executor only; read racily by the report)
    static constexpr size_t kPostBuckets = 7u; // <50 us, <250 us, <1 ms, <4 ms, <10 ms, <20 ms, 20 ms and more (of guest time)
    uint32_t notifyCount = 0;  // wake-ups the executor sent, and what they cost it (executor only)
    uint64_t notifyUs = 0;
    uint32_t postCount[kPostBuckets] = {};
    uint64_t postCycles[kPostBuckets] = {};
    uint64_t wSlices = 0, wWallUs = 0, wMaxSliceUs = 0, wGuestEe = 0, wSleepUs = 0, wWakes = 0;
    uint64_t locksRun = 0, locksWake = 0, locksDeferred = 0; // under their own mutex
    std::atomic<uint64_t> postUs{0};                         // executor: wall clock of the last publish (stats only)

    static uint64_t nowUs()
    {
#if defined(PLATFORM_VITA)
        return sceKernelGetProcessTimeWide(); // steady_clock costs ~6 us there
#else
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
#endif
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
        const char *list = std::getenv("PS2X_FRAME_LIST");
        if (const char *v = std::getenv("PS2X_IOP_STATS"))
            stats = v[0] == '1';
        if (const char *v = std::getenv("PS2X_IOP_WAKE_FIX"))
            wakeFix = v[0] != '0';
        if (const char *v = std::getenv("PS2X_IOP_NOCATCHUP"))
            noCatchUpMask = static_cast<uint32_t>(std::strtoul(v, nullptr, 0));
        if (stats)
            ps2xSpu2SetTiming(1);
        timing = diag || stats || (list && std::atof(list) > 0.0);
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
            ++locksDeferred;
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
        // Called ~13 000 times a second by the one executor thread: the thread-local flag (140 instructions per access
        // on the Vita) is set once, when the worker is started, not on every call.
        if (!started)
        {
            t_iopExecutor = true;
            startWorker();
        }
        const uint64_t p = (postedLocal += eeCycles);
        if (stats)
        {
            const uint64_t us = eeCycles / 295u;
            const size_t b = us < 50u ? 0u : us < 250u ? 1u : us < 1000u ? 2u : us < 4000u ? 3u : us < 10000u ? 4u : us < 20000u ? 5u : 6u;
            ++postCount[b];
            postCycles[b] += eeCycles;
        }
        if (deferredCount.load(std::memory_order_relaxed) != 0u)
            drainDeferred();
        // Called tens of thousands of times a second: the atomics and the wake are only paid once per wake quantum.
        if (p - lastNotified >= wakeEe)
        {
            lastNotified = p;
            if (stats)
                postUs.store(nowUs(), std::memory_order_relaxed);
            posted.store(p); // seq_cst: pairs with the worker's sleeping flag (see workerMain)
            if (sleeping.load())
            {
                const uint64_t n0 = stats ? nowUs() : 0u;
                {
                    std::lock_guard<std::mutex> lock(wakeMutex); // the worker is in wait() or has not tested yet
                    ++locksWake;
                }
                wakeCv.notify_one();
                if (stats)
                {
                    ++notifyCount; // executor only; read racily by the report
                    notifyUs += nowUs() - n0;
                }
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
        const uint64_t t0 = timing ? nowUs() : 0u;
        eeWaiting.fetch_add(1);
        runMutex.lock();
        eeWaiting.fetch_sub(1);
        t_iopHolds = true;
        ++locksRun;
        const uint64_t t1 = timing ? nowUs() : 0u;
        if (catchUp && t_iopExecutor)
        {
            drainDeferred();
            const uint64_t p = postedLocal;
            if (stats)
                lagExec.add((p > delivered ? p - delivered : 0u) / 295u);
            if (p > delivered && (noCatchUpMask & (1u << static_cast<uint32_t>(kind))) == 0u)
            {
                const uint64_t delta = p - delivered;
                run(delta);
                delivered = p;
                deliveredMirror.store(p, std::memory_order_release);
                if (timing)
                {
                    const uint64_t us = nowUs() - t1;
                    if (stats)
                    {
                        KindStat &ks = kinds[static_cast<size_t>(kind)];
                        ++ks.tailRuns;
                        ks.tailUs += us;
                        if (us > ks.maxTailUs)
                            ks.maxTailUs = us;
                    }
                    totTailUs.fetch_add(us, std::memory_order_relaxed);
                    totTailEe.fetch_add(delta, std::memory_order_relaxed);
                    if (diag)
                    {
                        SyncStat &st = syncs[static_cast<size_t>(kind)];
                        st.tailEe.fetch_add(delta, std::memory_order_relaxed);
                        st.tailUs.fetch_add(us, std::memory_order_relaxed);
                        raiseMax(st.maxTailUs, us);
                    }
                }
            }
        }
        if (timing)
        {
            t_holdStart = t1;
            t_holdKind = static_cast<size_t>(kind);
            t_holdWait = t1 - t0;
            if (stats)
            {
                KindStat &ks = kinds[static_cast<size_t>(kind)];
                ++ks.n;
                ks.waitUs += t1 - t0;
                if (t1 - t0 > ks.maxWaitUs)
                    ks.maxWaitUs = t1 - t0;
            }
            totCount.fetch_add(1, std::memory_order_relaxed);
            totWaitUs.fetch_add(t1 - t0, std::memory_order_relaxed);
            if (diag)
            {
                SyncStat &st = syncs[static_cast<size_t>(kind)];
                st.count.fetch_add(1, std::memory_order_relaxed);
                st.waitUs.fetch_add(t1 - t0, std::memory_order_relaxed);
                raiseMax(st.maxWaitUs, t1 - t0);
            }
        }
    }

    void leave()
    {
        if (timing)
        {
            // everything the executor did inside the sync: the tail run, then the RPC / transfer / memory access itself
            const uint64_t us = nowUs() - t_holdStart;
            if (stats)
            {
                KindStat &ks = kinds[t_holdKind];
                ks.holdUs += us;
                if (us > ks.maxHoldUs)
                    ks.maxHoldUs = us;
                if (t_holdKind == static_cast<size_t>(IopSyncKind::Rpc))
                    rpcRound.add(us + t_holdWait);
            }
            totHoldUs.fetch_add(us, std::memory_order_relaxed);
            if (diag)
            {
                SyncStat &st = syncs[t_holdKind];
                st.holdUs.fetch_add(us, std::memory_order_relaxed);
                raiseMax(st.maxHoldUs, us);
            }
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
        const char *prioValue = std::getenv("PS2X_VITA_IOP_PRIO"); // default 120 (the BGM feeder is 100, the game thread 128)
        sceKernelChangeThreadPriority(self, prioValue ? std::atoi(prioValue) : 120);
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

    void reportStats(uint64_t &lastUs)
    {
        const uint64_t now = nowUs();
        if (now - lastUs < 5000000u)
            return;
        const double wall = static_cast<double>(now - lastUs) / 1e6;
        lastUs = now;
        static uint64_t lastCpu = 0;
        static uint64_t lastSpu[11] = {};
        static uint64_t lastFile = 0, lastCall = 0;
        const uint64_t cpu = threadCpuUs();
        uint64_t spu[11] = {};
        ps2xSpu2Counters(spu);
        const uint64_t fileLocks = g_hostFileLocks, callLocks = g_callLocks;
        char line[1536];
        {
            std::lock_guard<std::mutex> lock(runMutex); // the numbers below are written under it
            ++locksRun;
            const double idleUs = static_cast<double>(wSleepUs);
            int n = std::snprintf(
                line, sizeof(line),
                "[iopstats] %.1fs worker cpu %.0f%% run %.0f%% asleep %.0f%% slices %llu (avg %.0f us max %llu us, %.2f guest ms avg) wakeups %llu"
                " | worker lag ms p95 %.2f max %.2f | wake latency ms p95 %.2f max %.2f"
                " | sync lag ms p95 %.2f max %.2f | rpc round-trip n %llu avg %.2f p95 %.2f max %.2f ms |",
                wall, (cpu - lastCpu) * 100.0 / (wall * 1e6), wWallUs * 100.0 / (wall * 1e6), idleUs * 100.0 / (wall * 1e6),
                (unsigned long long)wSlices, wSlices ? (double)wWallUs / wSlices : 0.0, (unsigned long long)wMaxSliceUs,
                wSlices ? wGuestEe / 294912.0 / wSlices : 0.0, (unsigned long long)wWakes,
                lagWorker.pctMs(0.95), lagWorker.maxUs / 1000.0, wakeLat.pctMs(0.95), wakeLat.maxUs / 1000.0,
                lagExec.pctMs(0.95), lagExec.maxUs / 1000.0,
                (unsigned long long)rpcRound.n, rpcRound.meanMs(), rpcRound.pctMs(0.95), rpcRound.maxUs / 1000.0);
            static const char *const names[] = {"rpc", "sif", "mem", "mod", "reset", "query"};
            uint64_t inlineRuns = 0, inlineUs = 0;
            for (size_t k = 0; k < static_cast<size_t>(IopSyncKind::Count); ++k)
            {
                const KindStat &ks = kinds[k];
                inlineRuns += ks.tailRuns;
                inlineUs += ks.tailUs;
                if (ks.n != 0u && n > 0 && n < static_cast<int>(sizeof(line)) - 200)
                    n += std::snprintf(line + n, sizeof(line) - n, " %s %llu wait %.1f ms (max %.2f) hold %.1f ms (max %.2f) inline %llu runs %.1f ms (max %.2f) |",
                                       names[k], (unsigned long long)ks.n, ks.waitUs / 1000.0, ks.maxWaitUs / 1000.0, ks.holdUs / 1000.0,
                                       ks.maxHoldUs / 1000.0, (unsigned long long)ks.tailRuns, ks.tailUs / 1000.0, ks.maxTailUs / 1000.0);
            }
            if (n > 0 && n < static_cast<int>(sizeof(line)) - 400)
                n += std::snprintf(
                    line + n, sizeof(line) - n,
                    " INLINE %llu runs %.1f ms total | locks/s run %.0f wake %.0f deferred %.0f spu-out %.0f spu-stream %.0f hostfile %.0f call %.0f |"
                    " spu2 runs/s %.0f chunks/s %.0f rendered/s %.0f bulk/s %.0f services/s %.0f deferred/s %.0f forced/s %.0f catchup/s %.0f render %.1f ms",
                    (unsigned long long)inlineRuns, inlineUs / 1000.0, locksRun / wall, locksWake / wall, locksDeferred / wall,
                    (spu[0] - lastSpu[0]) / wall, (spu[1] - lastSpu[1]) / wall, (fileLocks - lastFile) / wall, (callLocks - lastCall) / wall,
                    (spu[2] - lastSpu[2]) / wall, (spu[5] - lastSpu[5]) / wall, (spu[3] - lastSpu[3]) / wall, (spu[4] - lastSpu[4]) / wall,
                    (spu[6] - lastSpu[6]) / wall, (spu[7] - lastSpu[7]) / wall, (spu[8] - lastSpu[8]) / wall, (spu[9] - lastSpu[9]) / wall,
                    (spu[10] - lastSpu[10]) / 1000.0);
            if (n > 0 && n < static_cast<int>(sizeof(line)) - 300)
            {
                static const char *const pb[] = {"<50us", "<250us", "<1ms", "<4ms", "<10ms", "<20ms", ">=20ms"};
                n = static_cast<int>(std::strlen(line));
                n += std::snprintf(line + n, sizeof(line) - n, " | notify %u for %.1f ms | posts", notifyCount, notifyUs / 1000.0);
                notifyCount = 0;
                notifyUs = 0;
                for (size_t b = 0; b < kPostBuckets; ++b)
                {
                    if (postCount[b] != 0u)
                        n += std::snprintf(line + n, sizeof(line) - n, " %s:%u(%.0f ms)", pb[b], postCount[b], postCycles[b] / 294912.0);
                    postCount[b] = 0u;
                    postCycles[b] = 0u;
                }
            }
            lagExec.clear();
            lagWorker.clear();
            wakeLat.clear();
            rpcRound.clear();
            for (KindStat &ks : kinds)
                ks = KindStat{};
            wSlices = wWallUs = wMaxSliceUs = wGuestEe = wSleepUs = wWakes = 0;
            locksRun = locksWake = locksDeferred = 0;
            for (int i = 0; i < 11; ++i)
                lastSpu[i] = spu[i];
            lastFile = fileLocks;
            lastCall = callLocks;
        }
        lastCpu = cpu;
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
        uint64_t lastStatsUs = lastReportUs;
        for (;;)
        {
            {
                const uint64_t w0 = stats ? nowUs() : 0u;
                std::unique_lock<std::mutex> lock(wakeMutex);
                ++locksWake;
                sleeping.store(true);
                // `posted - delivered` as unsigned wraps to a huge number whenever an IopSync caught the IOP up beyond
                // the last published `posted` (postedLocal runs ahead of it between publishes), which made this predicate
                // true at once and the worker spin (lock, find nothing to do, wait again) for up to a wake quantum after
                // every inline catch-up: ~1.4 M mutex acquisitions a second in busy RPC phases. PS2X_IOP_WAKE_FIX=0
                // restores that predicate for A/B runs.
                wakeCv.wait(lock, [this]
                            {
                                if (stop.load())
                                    return true;
                                const uint64_t pst = posted.load(), dlv = deliveredMirror.load();
                                return wakeFix ? (pst > dlv && pst - dlv >= wakeEe) : (pst - dlv >= wakeEe);
                            });
                sleeping.store(false);
                if (stats)
                    wSleepUs += nowUs() - w0; // only this thread writes it (read under runMutex copy below: approximate)
            }
            bool firstSlice = true;
            for (;;)
            {
                if (stop.load())
                    return;
                // Let a waiting EE thread in between two slices (a std::mutex is not fair).
                while (eeWaiting.load() != 0 && !stop.load())
                    std::this_thread::yield();
                std::unique_lock<std::mutex> lock(runMutex);
                ++locksRun;
                const uint64_t p = posted.load();
                if (p <= delivered)
                    break;
                const uint64_t delta = std::min<uint64_t>(p - delivered, sliceEe);
                const uint64_t t0 = (diag || stats) ? nowUs() : 0u;
                if (stats)
                {
                    lagWorker.add((p - delivered) / 295u);
                    if (firstSlice)
                    {
                        // wall time from the executor's publish to now: the worker's wake-up and lock latency
                        const uint64_t pu = postUs.load(std::memory_order_relaxed);
                        wakeLat.add(t0 > pu ? t0 - pu : 0u);
                        ++wWakes;
                    }
                }
                firstSlice = false;
                run(delta);
                delivered += delta;
                deliveredMirror.store(delivered, std::memory_order_release);
                if (diag || stats)
                {
                    const uint64_t us = nowUs() - t0;
                    if (stats)
                    {
                        ++wSlices;
                        wWallUs += us;
                        wGuestEe += delta;
                        if (us > wMaxSliceUs)
                            wMaxSliceUs = us;
                    }
                    if (diag)
                    {
                        slices.fetch_add(1, std::memory_order_relaxed);
                        sliceEeTotal.fetch_add(delta, std::memory_order_relaxed);
                        sliceUs.fetch_add(us, std::memory_order_relaxed);
                        raiseMax(maxSliceUs, us);
                    }
                }
            }
            if (diag)
                report(lastReportUs);
            if (stats)
                reportStats(lastStatsUs);
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
    ++m_iopThread->locksDeferred;
    m_iopThread->deferred.push_back(std::move(op));
    m_iopThread->deferredCount.fetch_add(1u, std::memory_order_relaxed);
}

void PS2IopHostAdapter::configureIopThread(std::function<void(uint64_t)> run)
{
    const char *value = std::getenv("PS2X_IOP_THREAD");
    if (!value || value[0] != '1')
        return;
    m_iopThread = std::make_unique<IopThread>(*this, std::move(run));
    g_frameCounterOwner.store(this, std::memory_order_release);
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

// Cumulative totals for the per-frame [fdip] lines (EeScheduler.cpp): syncs, wait us, tail us, hold us, tail EE cycles.
extern "C" void ps2xIopFrameCounters(uint64_t *out)
{
    out[0] = out[1] = out[2] = out[3] = out[4] = 0u;
    PS2IopHostAdapter *owner = g_frameCounterOwner.load(std::memory_order_acquire);
    if (owner)
        owner->frameCounters(out);
}

void PS2IopHostAdapter::frameCounters(uint64_t *out) const
{
    if (!m_iopThread)
        return;
    out[0] = m_iopThread->totCount.load(std::memory_order_relaxed);
    out[1] = m_iopThread->totWaitUs.load(std::memory_order_relaxed);
    out[2] = m_iopThread->totTailUs.load(std::memory_order_relaxed);
    out[3] = m_iopThread->totHoldUs.load(std::memory_order_relaxed);
    out[4] = m_iopThread->totTailEe.load(std::memory_order_relaxed);
}

void PS2IopHostAdapter::stopIopThread()
{
    if (m_iopThread)
        m_iopThread->shutdown();
}
