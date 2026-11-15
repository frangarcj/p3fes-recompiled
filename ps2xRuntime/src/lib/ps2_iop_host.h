#pragma once

#include "ps2x/iop/iop_host.h"

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

class PS2Runtime;
struct R5900Context;

class PS2IopHostAdapter final : public ps2x::iop::IopHost
{
public:
    class CallScope
    {
    public:
        CallScope() = default;
        CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram);
        ~CallScope();

        CallScope(const CallScope &) = delete;
        CallScope &operator=(const CallScope &) = delete;
        CallScope(CallScope &&other) noexcept;
        CallScope &operator=(CallScope &&other) noexcept;

        [[nodiscard]] uint64_t token() const { return m_token; }

    private:
        void release();

        std::unique_lock<std::recursive_mutex> m_lock;
        PS2IopHostAdapter *m_owner = nullptr;
        R5900Context *m_previousContext = nullptr;
        uint8_t *m_previousRdram = nullptr;
        uint64_t m_previousToken = 0;
        uint64_t m_token = 0;
    };

    explicit PS2IopHostAdapter(PS2Runtime &runtime);
    ~PS2IopHostAdapter() override;

    // ---- the IOP on its own host thread (PS2X_IOP_THREAD=1; unset or 0: everything below is inert) ----
    //
    // The game thread (the EE executor) only counts EE cycles (postIopCycles); a worker thread runs the IOP up to the
    // posted time in short slices. Everything else that touches the IOP (RPC, SIF transfers, IOP RAM, module loads,
    // reset, queries) goes through syncIop(): it takes the lock the worker holds while it runs a slice and, on the
    // executor, first runs the IOP the rest of the way to the posted time itself, so every EE <-> IOP interaction sees
    // the IOP at the EE's time exactly as with the inline path. What IOP code does to the EE (guest RAM writes, SIF
    // command packets) is queued by the worker and applied by the executor in order, before its next IOP interaction
    // and at every post, so the guest RAM and the EE scheduler are only ever touched by the EE thread.
    enum class IopSyncKind : uint32_t
    {
        Rpc = 0,
        Sif,
        Memory,
        Module,
        Reset,
        Query,
        Count
    };
    class IopSync
    {
    public:
        IopSync() = default;
        ~IopSync();
        IopSync(const IopSync &) = delete;
        IopSync &operator=(const IopSync &) = delete;
        IopSync(IopSync &&other) noexcept : m_owner(other.m_owner) { other.m_owner = nullptr; }
        IopSync &operator=(IopSync &&other) noexcept
        {
            if (this != &other)
            {
                release();
                m_owner = other.m_owner;
                other.m_owner = nullptr;
            }
            return *this;
        }

    private:
        friend class PS2IopHostAdapter;
        explicit IopSync(PS2IopHostAdapter *owner) : m_owner(owner) {}
        void release();
        PS2IopHostAdapter *m_owner = nullptr;
    };
    // Reads PS2X_IOP_THREAD; `run` advances the IOP by EE cycles (only ever called with the IOP lock held).
    void configureIopThread(std::function<void(uint64_t)> run);
    [[nodiscard]] bool iopThreaded() const noexcept { return m_iopThread != nullptr; }
    void postIopCycles(uint64_t eeCycles) noexcept; // executor
    // catchUp=false: only exclude the worker (queries from other threads).
    [[nodiscard]] IopSync syncIop(IopSyncKind kind, bool catchUp = true);
    void stopIopThread();

    [[nodiscard]] CallScope enterCall(R5900Context *context,
                                      uint8_t *rdram = nullptr);

    bool readGuest(uint32_t address, void *destination, size_t size) const override;
    bool writeGuest(uint32_t address, const void *source, size_t size) override;
    bool zeroGuest(uint32_t address, size_t size) override;
    bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const override;
    bool readIopMemory(uint32_t address, void *destination, size_t size) const override;
    bool writeIopMemory(uint32_t address, const void *source, size_t size) override;
    bool zeroIopMemory(uint32_t address, size_t size) override;
    bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const override;
    uint32_t allocateIopHandle(ps2x::iop::IopHandleKind kind) override;
    uint32_t allocateGuest(uint32_t size, uint32_t alignment) override;
    void freeGuest(uint32_t address) override;

    void audioCommand(uint32_t sid,
                      uint32_t function,
                      ps2x::iop::GuestBuffer send,
                      ps2x::iop::GuestBuffer receive) override;

    std::string hostPath(ps2x::iop::HostPathKind kind) const override;
    std::string translateGuestPath(std::string_view path) const override;
    uint64_t openHostFile(std::string_view path) override;
    bool hostFileSize(uint64_t handle, uint64_t &size) const override;
    bool readHostFile(uint64_t handle,
                      uint64_t offset,
                      void *destination,
                      size_t size,
                      size_t &bytesRead) override;
    void closeHostFile(uint64_t handle) override;

    int32_t memoryCard(const ps2x::iop::MemoryCardRequest &request) override;

    bool hasGuestFunction(uint32_t address) const override;
    bool invokeGuestFunction(uint64_t callToken,
                             uint32_t address,
                             uint32_t a0,
                             uint32_t a1,
                             uint32_t a2,
                             uint32_t a3,
                             uint32_t *resultAddress) override;
    bool sendSifCommand(uint32_t commandId, const void *packet, size_t packetSize) override;

    void log(ps2x::iop::LogLevel level, std::string_view message) override;

private:
    friend class CallScope;

    bool guestRange(uint32_t address, size_t size, uint8_t *&begin) const;
    uint8_t *activeRdram() const;
    bool writeGuestNow(uint32_t address, const void *source, size_t size);
    bool zeroGuestNow(uint32_t address, size_t size);
    void queueDeferred(uint32_t kind, uint32_t a, uint32_t b, const void *data, size_t bytes);
    struct IopThread;
    std::unique_ptr<IopThread> m_iopThread;

    PS2Runtime &m_runtime;
    std::recursive_mutex m_callMutex;
    R5900Context *m_activeContext = nullptr;
    uint8_t *m_activeRdram = nullptr;
    uint64_t m_activeToken = 0;
    uint64_t m_nextToken = 1;
    struct HostFile
    {
        std::FILE *stream = nullptr;
        uint64_t size = 0u;
    };
    mutable std::mutex m_hostFileMutex;
    std::unordered_map<uint64_t, HostFile> m_hostFiles;
    uint64_t m_nextHostFileHandle = 1u;
};
