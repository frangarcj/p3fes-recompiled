// Standalone test + benchmark for ps2x::GuestHeap (src/lib/guest_heap.cpp).
//
//   c++ -std=c++20 -O2 -I ps2xRuntime/src/lib ps2xRuntime/tests/guest_heap_test.cpp \
//       ps2xRuntime/src/lib/guest_heap.cpp -o guest_heap_test && ./guest_heap_test
//
// The reference model below is the original PS2Runtime allocator (std::vector<Block>, linear first fit,
// erase/insert/coalesce) copied verbatim in behaviour. Because the new allocator keeps the same placement
// policy (address-ordered first fit, 16-byte granularity, immediate coalescing) the fuzz test demands
// IDENTICAL results (same addresses, same failures), not just "similar" ones.

#include "guest_heap.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <random>
#include <vector>

#define CHECK(cond)                                                                          \
    do                                                                                       \
    {                                                                                        \
        if (!(cond))                                                                         \
        {                                                                                    \
            std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
            std::abort();                                                                    \
        }                                                                                    \
    } while (0)

namespace
{
    using ps2x::GuestHeap;
    constexpr uint32_t kRamMask = 0x1FFFFFFFu; // PS2_RAM_MASK
    constexpr uint32_t kDefaultAlignment = 16u;

    // ---------------------------------------------------------------------------------------------
    // Reference model: the old allocator
    // ---------------------------------------------------------------------------------------------
    class RefHeap
    {
    public:
        struct Block
        {
            uint32_t addr;
            uint32_t size;
            bool free;
        };

        static uint32_t alignValue(uint32_t value, uint32_t alignment)
        {
            if (alignment == 0)
                return value;
            const uint32_t mask = alignment - 1u;
            if (value > (std::numeric_limits<uint32_t>::max() - mask))
                return std::numeric_limits<uint32_t>::max();
            return (value + mask) & ~mask;
        }
        static bool alignmentValid(uint32_t a) { return a != 0u && (a & (a - 1u)) == 0u; }
        static uint32_t normalizeAlignment(uint32_t a)
        {
            if (!alignmentValid(a))
                return kDefaultAlignment;
            return std::max(a, kDefaultAlignment);
        }

        void reset(uint32_t base, uint32_t limit)
        {
            blocks.clear();
            if (limit > base)
                blocks.push_back({base, limit - base, true});
            end = base;
        }

        int32_t findUsed(uint32_t guestAddr) const
        {
            const uint32_t a = guestAddr & kRamMask;
            for (size_t i = 0; i < blocks.size(); ++i)
                if (!blocks[i].free && blocks[i].addr == a)
                    return static_cast<int32_t>(i);
            return -1;
        }

        uint32_t allocateLocked(uint32_t size, uint32_t alignment)
        {
            if (size == 0u)
                return 0u;
            const uint32_t normalizedAlignment = normalizeAlignment(alignment);
            if (size > (std::numeric_limits<uint32_t>::max() - (kDefaultAlignment - 1u)))
                return 0u;
            const uint32_t allocSize = alignValue(size, kDefaultAlignment);
            if (allocSize == 0u)
                return 0u;
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                const Block block = blocks[i];
                if (!block.free)
                    continue;
                const uint64_t blockStart = block.addr;
                const uint64_t blockEnd = blockStart + static_cast<uint64_t>(block.size);
                const uint32_t alignedAddr = alignValue(block.addr, normalizedAlignment);
                if (alignedAddr < block.addr)
                    continue;
                const uint64_t alignedStart = alignedAddr;
                if (alignedStart > blockEnd)
                    continue;
                const uint64_t allocEnd = alignedStart + static_cast<uint64_t>(allocSize);
                if (allocEnd > blockEnd)
                    continue;
                const uint32_t prefixSize = static_cast<uint32_t>(alignedStart - blockStart);
                const uint32_t suffixSize = static_cast<uint32_t>(blockEnd - allocEnd);
                std::vector<Block> replacement;
                if (prefixSize > 0u)
                    replacement.push_back({block.addr, prefixSize, true});
                replacement.push_back({alignedAddr, allocSize, false});
                if (suffixSize > 0u)
                    replacement.push_back({static_cast<uint32_t>(allocEnd), suffixSize, true});
                blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
                blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(i), replacement.begin(), replacement.end());
                end = std::max(end, static_cast<uint32_t>(allocEnd));
                return alignedAddr;
            }
            return 0u;
        }

        void coalesce()
        {
            if (blocks.empty())
                return;
            size_t i = 1;
            while (i < blocks.size())
            {
                Block &prev = blocks[i - 1];
                Block &curr = blocks[i];
                const uint64_t prevEnd = static_cast<uint64_t>(prev.addr) + static_cast<uint64_t>(prev.size);
                if (prev.free && curr.free && prevEnd == curr.addr)
                {
                    prev.size += curr.size;
                    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
                    continue;
                }
                ++i;
            }
        }

        void freeLocked(uint32_t guestAddr)
        {
            const int32_t index = findUsed(guestAddr);
            if (index < 0)
                return;
            blocks[static_cast<size_t>(index)].free = true;
            coalesce();
        }

        struct Realloc
        {
            uint32_t addr = 0;
            uint32_t oldAddr = 0;
            uint32_t copyBytes = 0;
        };

        // guestRealloc body after the guestAddr==0 / newSize==0 handling.
        Realloc reallocLocked(uint32_t guestAddr, uint32_t newSize, uint32_t alignment)
        {
            Realloc r;
            if (newSize > (std::numeric_limits<uint32_t>::max() - (kDefaultAlignment - 1u)))
                return r;
            const uint32_t normalizedAlignment = normalizeAlignment(alignment);
            const uint32_t requestedSize = alignValue(newSize, kDefaultAlignment);
            const int32_t index = findUsed(guestAddr);
            if (index < 0)
                return r;
            const size_t blockIndex = static_cast<size_t>(index);
            const uint32_t oldAddr = blocks[blockIndex].addr;
            const uint32_t oldSize = blocks[blockIndex].size;
            r.oldAddr = oldAddr;
            if (requestedSize <= oldSize)
            {
                if (requestedSize < oldSize)
                {
                    const uint32_t tailAddr = oldAddr + requestedSize;
                    const uint32_t tailSize = oldSize - requestedSize;
                    blocks[blockIndex].size = requestedSize;
                    blocks.insert(blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u),
                                  Block{tailAddr, tailSize, true});
                    coalesce();
                }
                r.addr = oldAddr;
                return r;
            }
            if (blockIndex + 1u < blocks.size())
            {
                Block &next = blocks[blockIndex + 1u];
                const uint64_t blockEnd = static_cast<uint64_t>(blocks[blockIndex].addr) +
                                          static_cast<uint64_t>(blocks[blockIndex].size);
                if (next.free && blockEnd == next.addr)
                {
                    const uint64_t combined = static_cast<uint64_t>(blocks[blockIndex].size) +
                                              static_cast<uint64_t>(next.size);
                    if (combined >= requestedSize)
                    {
                        const uint32_t extraNeeded = requestedSize - blocks[blockIndex].size;
                        blocks[blockIndex].size = requestedSize;
                        if (next.size == extraNeeded)
                        {
                            blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(blockIndex + 1u));
                        }
                        else
                        {
                            next.addr += extraNeeded;
                            next.size -= extraNeeded;
                        }
                        end = std::max(end, oldAddr + requestedSize);
                        r.addr = oldAddr;
                        return r;
                    }
                }
            }
            const uint32_t newAddr = allocateLocked(newSize, normalizedAlignment);
            if (newAddr == 0u)
                return r;
            r.addr = newAddr;
            r.copyBytes = std::min(oldSize, newSize);
            freeLocked(oldAddr);
            return r;
        }

        size_t usedCount() const
        {
            size_t n = 0;
            for (const Block &b : blocks)
                n += b.free ? 0u : 1u;
            return n;
        }

        std::vector<Block> blocks;
        uint32_t end = 0;
    };

    // ---------------------------------------------------------------------------------------------
    // Fuzz harness: both allocators run the same operation stream, plus fake guest RAM
    // ---------------------------------------------------------------------------------------------
    constexpr uint32_t kTrackLimit = 65536u;

    struct Live
    {
        uint32_t addr;
        uint32_t bytes; // bytes the "game" owns (requested size)
        uint32_t seed;
        uint32_t align;
        bool tracked() const { return bytes <= kTrackLimit; } // contents are only patterned for small blocks
    };

    inline uint8_t patternByte(uint32_t seed, uint32_t i) { return static_cast<uint8_t>((seed * 2654435761u + i * 40503u) >> 13); }

    struct Fuzz
    {
        Fuzz(uint32_t base_, uint32_t limit_, uint64_t rngSeed) : base(base_), limit(limit_), rng(rngSeed)
        {
            heap.reset(base, limit);
            ref.reset(base, limit);
            ram.assign(limit + 16u, 0);
        }

        uint32_t pickSize(bool big)
        {
            const uint32_t r = rng() % 100u;
            const uint32_t span = limit - base;
            if (r < 55u)
                return 1u + rng() % 256u;
            if (r < 82u)
                return 257u + rng() % 4096u;
            if (r < 96u)
                return 4097u + rng() % 61440u;
            return 1u + rng() % std::max<uint32_t>(1u, big ? span / 2u : span / 8u);
        }

        uint32_t pickAlign()
        {
            static const uint32_t kAligns[] = {0, 1, 3, 8, 16, 16, 16, 16, 16, 32, 64, 64, 128, 256, 4096, 6, 65536};
            return kAligns[rng() % (sizeof(kAligns) / sizeof(kAligns[0]))];
        }

        void fill(uint32_t addr, uint32_t bytes, uint32_t seed, uint32_t from = 0)
        {
            if (bytes > kTrackLimit)
                return;
            for (uint32_t i = from; i < bytes; ++i)
                ram[addr + i] = patternByte(seed, i);
        }

        void verify(const Live &l, uint32_t bytes)
        {
            if (!l.tracked())
                return;
            for (uint32_t i = 0; i < bytes; ++i)
                CHECK(ram[l.addr + i] == patternByte(l.seed, i));
        }

        void checkPlacement(uint32_t addr, uint32_t bytes, uint32_t alignment)
        {
            CHECK(addr >= base && static_cast<uint64_t>(addr) + bytes <= limit);
            CHECK((addr % RefHeap::normalizeAlignment(alignment)) == 0u);
            // no overlap with the neighbours in the live set
            auto it = live.lower_bound(addr);
            if (it != live.end())
                CHECK(static_cast<uint64_t>(addr) + bytes <= it->first);
            if (it != live.begin())
            {
                auto p = std::prev(it);
                CHECK(static_cast<uint64_t>(p->first) + p->second.bytes <= addr);
            }
        }

        void doMalloc(bool big)
        {
            const uint32_t size = pickSize(big);
            const uint32_t align = pickAlign();
            const uint32_t a = heap.allocate(size, align);
            const uint32_t b = ref.allocateLocked(size, align);
            CHECK(a == b); // identical placement policy
            if (a == 0u)
            {
                ++failedAllocs;
                return;
            }
            CHECK(live.count(a) == 0u);
            checkPlacement(a, size, align);
            Live l{a, size, static_cast<uint32_t>(rng()), align};
            live[a] = l;
            fill(a, size, l.seed);
            ++okAllocs;
        }

        void doFree()
        {
            if (live.empty())
                return;
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            const Live l = it->second;
            verify(l, l.bytes);
            live.erase(it);
            // random KSEG-style alias, the wrapper masks with PS2_RAM_MASK
            const uint32_t alias = (rng() & 1u) ? (l.addr | 0x80000000u) : l.addr;
            CHECK(heap.free(alias & kRamMask));
            ref.freeLocked(alias);
        }

        void doBogusFree()
        {
            // double free / interior pointer / below base / above limit: ignored by both
            uint32_t a;
            switch (rng() % 4u)
            {
            case 0:
                a = base + rng() % (limit - base);
                break;
            case 1:
                a = lastFreed;
                break;
            case 2:
                a = base - 16u;
                break;
            default:
                a = limit + (rng() % 64u);
                break;
            }
            if (live.count(a))
                return; // happens to be a real block, not bogus
            const bool known = ref.findUsed(a) >= 0;
            CHECK(!known);
            CHECK(!heap.free(a & kRamMask));
            ref.freeLocked(a);
        }

        void doRealloc(bool big)
        {
            if (live.empty())
                return;
            auto it = live.begin();
            std::advance(it, rng() % live.size());
            Live l = it->second;
            uint32_t newSize;
            switch (rng() % 3u)
            {
            case 0:
                newSize = std::max<uint32_t>(1u, l.bytes / 2u + rng() % 8u); // shrink-ish
                break;
            case 1:
                newSize = l.bytes + 1u + rng() % 512u; // small grow, often in place
                break;
            default:
                newSize = pickSize(big);
                break;
            }
            const uint32_t align = pickAlign();
            const auto a = heap.reallocate(l.addr, newSize, align);
            const auto b = ref.reallocLocked(l.addr, newSize, align);
            CHECK(a.addr == b.addr && a.oldAddr == b.oldAddr && a.copyBytes == b.copyBytes);
            if (a.addr == 0u)
            {
                verify(l, l.bytes); // failure leaves the old block untouched
                ++failedReallocs;
                return;
            }
            const uint32_t keep = std::min(l.bytes, newSize);
            if (a.addr != a.oldAddr)
            {
                CHECK(a.copyBytes >= keep);
                std::memmove(ram.data() + a.addr, ram.data() + a.oldAddr, a.copyBytes);
                ++movedReallocs;
            }
            live.erase(l.addr);
            // an in-place resize keeps the old address even if the new alignment request is stricter
            checkPlacement(a.addr, newSize, a.addr != a.oldAddr ? align : 0u);
            Live n{a.addr, newSize, l.seed, align};
            live[a.addr] = n;
            if (l.tracked() && n.tracked())
            {
                // contents preserved across realloc
                for (uint32_t i = 0; i < keep; ++i)
                    CHECK(ram[a.addr + i] == patternByte(l.seed, i));
                if (newSize > keep)
                    fill(a.addr, newSize, l.seed, keep);
            }
            else if (n.tracked())
            {
                fill(a.addr, newSize, l.seed);
            }
            ++okReallocs;
        }

        void step(bool preferAlloc, bool big)
        {
            const uint32_t r = rng() % 100u;
            const uint32_t allocP = preferAlloc ? 55u : 30u;
            if (r < allocP)
                doMalloc(big);
            else if (r < allocP + 8u)
                doRealloc(big);
            else if (r < 97u)
                doFree();
            else
                doBogusFree();
            if (!live.empty() && (rng() % 4u) == 0u)
                lastFreed = live.begin()->first; // some live address, only used if it is freed later
            if ((++steps & 0x3FFu) == 0u)
                crossCheck();
        }

        void crossCheck()
        {
            const char *why = nullptr;
            CHECK(heap.validate(&why));
            CHECK(heap.usedBlockCount() == ref.usedCount());
            CHECK(heap.usedBlockCount() == live.size());
            CHECK(heap.highWater() == ref.end);
            // block-for-block comparison of the layout (free blocks only: used ones checked by lookup)
            size_t freeBlocks = 0;
            uint32_t largest = 0;
            for (const auto &b : ref.blocks)
            {
                if (b.free)
                {
                    ++freeBlocks;
                    largest = std::max(largest, b.size);
                }
                else
                {
                    CHECK(heap.usedBlockSize(b.addr) == b.size);
                }
            }
            CHECK(heap.freeBlockCount() == freeBlocks);
            CHECK(heap.largestFreeBlock() == largest);
        }

        void drain()
        {
            // full coalescing: everything freed -> single block -> one max size allocation succeeds
            while (!live.empty())
                doFree();
            crossCheck();
            CHECK(!heap.anyUsed());
            CHECK(heap.freeBlockCount() == 1u);
            CHECK(heap.largestFreeBlock() == limit - base);
            // (an odd, non multiple of 16 tail can only be handed out rounded down)
            const uint32_t maxReq = (limit - base) & ~15u;
            const uint32_t all = heap.allocate(maxReq, 16u);
            const uint32_t allRef = ref.allocateLocked(maxReq, 16u);
            CHECK(all == base && allRef == base);
            CHECK(heap.free(all));
            ref.freeLocked(allRef);
            CHECK(heap.freeBlockCount() == 1u);
            CHECK(heap.validate());
        }

        uint32_t base, limit;
        std::mt19937_64 rng;
        GuestHeap heap;
        RefHeap ref;
        std::vector<uint8_t> ram;
        std::map<uint32_t, Live> live;
        uint32_t lastFreed = 0;
        uint64_t steps = 0, okAllocs = 0, failedAllocs = 0, okReallocs = 0, failedReallocs = 0, movedReallocs = 0;
    };

    void runFuzz(const char *name, uint32_t base, uint32_t limit, uint64_t seed, uint32_t steps)
    {
        Fuzz f(base, limit, seed);
        // alternate allocation-heavy phases (heap exhaustion) and free-heavy phases
        for (uint32_t i = 0; i < steps; ++i)
        {
            const bool preferAlloc = ((i / 3000u) & 1u) == 0u;
            f.step(preferAlloc, (limit - base) < (1u << 20));
        }
        f.crossCheck();
        std::printf("  fuzz %-28s heap=%7u KiB steps=%u alloc ok/fail=%llu/%llu realloc ok/moved/fail=%llu/%llu/%llu\n",
                    name, (limit - base) / 1024u, steps,
                    static_cast<unsigned long long>(f.okAllocs), static_cast<unsigned long long>(f.failedAllocs),
                    static_cast<unsigned long long>(f.okReallocs), static_cast<unsigned long long>(f.movedReallocs),
                    static_cast<unsigned long long>(f.failedReallocs));
        CHECK(f.okAllocs > 100u);
        f.drain();
    }

    void edgeCases()
    {
        GuestHeap h;
        // not initialised / empty heap
        CHECK(h.allocate(16, 16) == 0u);
        CHECK(!h.free(0x1000));
        h.reset(0x1000, 0x1000);
        CHECK(h.allocate(16, 16) == 0u && !h.anyUsed() && h.freeBlockCount() == 0u);
        h.reset(0x2000, 0x1000);
        CHECK(h.allocate(16, 16) == 0u);

        h.reset(0x100000, 0x100000 + 0x10000);
        CHECK(h.highWater() == 0x100000u);
        CHECK(h.allocate(0, 16) == 0u);
        CHECK(h.allocate(0xFFFFFFFFu, 16) == 0u);
        CHECK(h.allocate(0xFFFFFFF1u, 16) == 0u); // > max-15
        CHECK(h.allocate(0xFFFFFFF0u, 16) == 0u); // rounds fine, does not fit
        CHECK(h.allocate(0x10001, 16) == 0u);
        // granularity and default alignment
        const uint32_t a = h.allocate(1, 0);
        CHECK(a == 0x100000u);
        CHECK(h.usedBlockSize(a) == 16u);
        const uint32_t b = h.allocate(17, 5); // non power of two -> 16
        CHECK(b == 0x100010u && h.usedBlockSize(b) == 32u);
        // alignment > 16 leaves the prefix as a free block that is reused (first fit)
        const uint32_t c = h.allocate(16, 256);
        CHECK(c == 0x100100u);
        const uint32_t d = h.allocate(16, 16);
        CHECK(d == 0x100030u);
        CHECK(h.highWater() == 0x100110u);
        CHECK(h.validate());
        // double free / unknown / interior
        CHECK(h.free(d));
        CHECK(!h.free(d));
        CHECK(!h.free(b + 16u));
        CHECK(!h.free(0));
        CHECK(h.free(a) && h.free(b) && h.free(c));
        CHECK(h.freeBlockCount() == 1u && h.largestFreeBlock() == 0x10000u && h.validate());
        // realloc: unknown address, in-place shrink/grow, move, failure keeps the old block
        auto r = h.reallocate(0x100000, 16, 16);
        CHECK(r.addr == 0u);
        const uint32_t p = h.allocate(64, 16);
        const uint32_t q = h.allocate(64, 16);
        CHECK(p == 0x100000u && q == 0x100040u);
        r = h.reallocate(p, 16, 16); // shrink in place
        CHECK(r.addr == p);
        CHECK(h.usedBlockSize(p) == 16u);
        r = h.reallocate(p, 64, 16); // grow back into the hole it just made (adjacent free block)
        CHECK(r.addr == p && h.usedBlockSize(p) == 64u);
        r = h.reallocate(p, 65, 16); // next block is used -> must move
        CHECK(r.addr != 0u && r.addr != p && r.oldAddr == p && r.copyBytes == 64u);
        CHECK(h.usedBlockSize(p) == 0u && h.usedBlockSize(r.addr) == 80u);
        r = h.reallocate(q, 0xFFFFFFF1u, 16);
        CHECK(r.addr == 0u && h.usedBlockSize(q) == 64u);
        r = h.reallocate(q, 0x20000, 16); // does not fit anywhere: q survives
        CHECK(r.addr == 0u && h.usedBlockSize(q) == 64u);
        r = h.reallocate(q, 0, 16); // size 0 is the caller's business
        CHECK(r.addr == 0u && h.usedBlockSize(q) == 64u);
        CHECK(h.validate());

        // exact fit, then free restores one block
        h.reset(0x200000, 0x200000 + 0x100);
        const uint32_t e = h.allocate(0x100, 16);
        CHECK(e == 0x200000u && h.freeBlockCount() == 0u && h.largestFreeBlock() == 0u);
        CHECK(h.allocate(1, 16) == 0u);
        CHECK(h.free(e) && h.freeBlockCount() == 1u && h.validate());
        // tail that is not a multiple of 16 (private heap with odd limit)
        h.reset(0x200000, 0x200000 + 0x108);
        CHECK(h.allocate(0x100, 16) == 0x200000u);
        CHECK(h.allocate(0x10, 16) == 0u);
        CHECK(h.allocate(8, 16) == 0u); // rounds to 16 > 8 left
        CHECK(h.validate());
        // reset forgets everything, hash included
        h.reset(0x300000, 0x300000 + 0x1000);
        CHECK(!h.free(0x200000) && !h.anyUsed());
    }

    // ---------------------------------------------------------------------------------------------
    // Benchmark: ~5000 live blocks, per-frame bursts of a few hundred malloc/free pairs
    // ---------------------------------------------------------------------------------------------
    struct Op
    {
        enum Kind : uint8_t { Malloc, Free } kind;
        uint32_t size;  // Malloc
        uint32_t token; // Malloc: slot id to store the result in; Free: slot id to free
    };

    template <class Alloc>
    double runBench(Alloc &alloc, const std::vector<Op> &warm, const std::vector<Op> &frames, size_t slots,
                    uint64_t &checksum)
    {
        std::vector<uint32_t> slotAddr(slots, 0u);
        for (const Op &op : warm)
        {
            if (op.kind == Op::Malloc)
                slotAddr[op.token] = alloc.malloc(op.size);
            else
                alloc.free(slotAddr[op.token]);
        }
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t sum = 0;
        for (const Op &op : frames)
        {
            if (op.kind == Op::Malloc)
            {
                const uint32_t a = alloc.malloc(op.size);
                slotAddr[op.token] = a;
                sum += a;
            }
            else
            {
                alloc.free(slotAddr[op.token]);
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        checksum = sum;
        return std::chrono::duration<double>(t1 - t0).count();
    }

    struct OldAlloc
    {
        RefHeap h;
        uint32_t malloc(uint32_t s) { return h.allocateLocked(s, 16u); }
        void free(uint32_t a) { h.freeLocked(a); }
    };
    struct NewAlloc
    {
        GuestHeap h;
        uint32_t malloc(uint32_t s) { return h.allocate(s, 16u); }
        void free(uint32_t a) { h.free(a & kRamMask); }
    };

    void benchmark(uint32_t liveTarget, uint32_t burst, uint32_t frameCount)
    {
        const uint32_t base = 0x00400000u, limit = 0x01F00000u;
        std::mt19937 rng(12345);
        auto sz = [&]()
        {
            const uint32_t r = rng() % 100u;
            if (r < 60u)
                return 16u + rng() % 240u;
            if (r < 92u)
                return 256u + rng() % 3840u;
            return 4096u + rng() % 28672u;
        };

        std::vector<Op> warm, frames;
        std::vector<uint32_t> longLived; // slots currently holding long-lived blocks
        uint32_t nextSlot = 0;
        // build the heap up past the target, then free a random third to make it fragmented
        for (uint32_t i = 0; i < liveTarget * 3u / 2u; ++i)
        {
            warm.push_back({Op::Malloc, sz(), nextSlot});
            longLived.push_back(nextSlot++);
        }
        while (longLived.size() > liveTarget)
        {
            const size_t k = rng() % longLived.size();
            warm.push_back({Op::Free, 0, longLived[k]});
            longLived[k] = longLived.back();
            longLived.pop_back();
        }
        for (uint32_t f = 0; f < frameCount; ++f)
        {
            std::vector<uint32_t> temp;
            for (uint32_t i = 0; i < burst; ++i)
            {
                frames.push_back({Op::Malloc, sz(), nextSlot});
                temp.push_back(nextSlot++);
            }
            // a few long-lived blocks turn over each frame
            for (uint32_t i = 0; i < burst / 10u; ++i)
            {
                const size_t k = rng() % longLived.size();
                frames.push_back({Op::Free, 0, longLived[k]});
                frames.push_back({Op::Malloc, sz(), longLived[k]});
            }
            std::shuffle(temp.begin(), temp.end(), rng);
            for (uint32_t t : temp)
                frames.push_back({Op::Free, 0, t});
        }
        const size_t slots = nextSlot;
        const size_t totalOps = frames.size();

        uint64_t sumOld = 0, sumNew = 0;
        OldAlloc oldA;
        oldA.h.reset(base, limit);
        const double tOld = runBench(oldA, warm, frames, slots, sumOld);
        NewAlloc newA;
        newA.h.reset(base, limit);
        const double tNew = runBench(newA, warm, frames, slots, sumNew);
        CHECK(sumOld == sumNew); // identical placement
        CHECK(newA.h.validate());
        std::printf("  bench live=%u burst=%u frames=%u (%zu timed ops, %zu live blocks at end, %zu free blocks)\n",
                    liveTarget, burst, frameCount, totalOps, newA.h.usedBlockCount(), newA.h.freeBlockCount());
        std::printf("    old: %8.2f ms  %10.0f ops/s\n", tOld * 1e3, totalOps / tOld);
        std::printf("    new: %8.2f ms  %10.0f ops/s  (%.1fx)\n", tNew * 1e3, totalOps / tNew, tOld / tNew);
    }
}

int main(int argc, char **argv)
{
    const bool quick = argc > 1 && std::strcmp(argv[1], "--quick") == 0;
    const uint32_t scale = quick ? 1u : 4u;

    std::printf("edge cases\n");
    edgeCases();

    std::printf("fuzz against the old allocator\n");
    runFuzz("tiny, constantly exhausted", 0x1000u, 0x1000u + 0x4000u, 1, 60000u * scale);
    runFuzz("small, odd tail (not 16-mult)", 0x2000u, 0x2000u + 0x40008u, 2, 80000u * scale);
    runFuzz("medium 1 MiB", 0x100000u, 0x200000u, 3, 80000u * scale);
    runFuzz("PS2 default 29 MiB", 0x00500000u, 0x01F00000u, 4, 60000u * scale);
    runFuzz("private heap (GoW) 380 KiB", 0x000A0000u, 0x000FF000u, 5, 80000u * scale);
    for (uint64_t s = 10; s < 10u + 6u * scale; ++s)
        runFuzz("random seeds, 256 KiB", 0x10000u, 0x50000u, s, 30000u);

    std::printf("benchmark (old = linear vector allocator, new = GuestHeap)\n");
    benchmark(5000, 300, quick ? 50u : 200u);
    benchmark(5000, 100, quick ? 50u : 300u);
    benchmark(1500, 300, quick ? 50u : 200u);

    std::printf("guest_heap_test: OK\n");
    return 0;
}
