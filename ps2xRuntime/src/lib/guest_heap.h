#ifndef PS2X_GUEST_HEAP_H
#define PS2X_GUEST_HEAP_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2x
{
    // Allocator behind PS2Runtime::guestMalloc/guestFree/guestRealloc. It only manages address ranges
    // (all metadata lives on the host side, the game can overwrite guest memory freely) and has no
    // knowledge of the runtime or of RDRAM. It is NOT thread safe: the caller holds its own mutex.
    //
    // Placement policy: address-ordered first fit with immediate coalescing of adjacent free blocks and
    // 16-byte granularity, i.e. exactly the policy of the original linear-scan allocator (same addresses
    // for the same request sequence), but every operation is O(log n):
    //   - blocks tile [base, limit) completely and are linked in address order (prev/next indices);
    //   - free blocks are kept in a treap keyed by address whose nodes carry the maximum free size of
    //     their subtree, so "lowest-address free block that fits" is a single descent;
    //   - used blocks are found by address through a chained hash table;
    //   - nodes come from a pooled array (indices, no per-operation host allocation once warm).
    class GuestHeap
    {
    public:
        static constexpr uint32_t kGranule = 16u;

        struct ReallocResult
        {
            uint32_t addr = 0;      // new address, 0 on failure (unknown address or no room)
            uint32_t oldAddr = 0;   // block address before the call (valid when addr != 0)
            uint32_t copyBytes = 0; // bytes the caller must copy oldAddr -> addr when addr != oldAddr
        };

        GuestHeap();
        GuestHeap(const GuestHeap &) = delete;
        GuestHeap &operator=(const GuestHeap &) = delete;

        // Drops every block and makes [base, limit) one free block (empty heap when limit <= base).
        // base is expected to be 16-byte aligned. The high-water mark restarts at base.
        void reset(uint32_t base, uint32_t limit);

        // size == 0 or an overflowing size gives 0. alignment is normalised (non power of two -> 16,
        // otherwise at least 16). Returns 0 when no free block can hold the request.
        uint32_t allocate(uint32_t size, uint32_t alignment);

        // Frees the used block that starts exactly at addr. Unknown / already free addresses are ignored
        // (returns false).
        bool free(uint32_t addr);

        // newSize must not be 0 (the caller treats that as free). Shrinks and grows in place when it
        // can (growing only into an adjacent free block), otherwise allocates a new block and frees the
        // old one; in that case the caller copies result.copyBytes bytes. On failure the old block is
        // untouched.
        ReallocResult reallocate(uint32_t addr, uint32_t newSize, uint32_t alignment);

        bool anyUsed() const { return m_usedCount != 0u; }
        uint32_t highWater() const { return m_highWater; }
        uint32_t base() const { return m_base; }
        uint32_t limit() const { return m_limit; }
        size_t usedBlockCount() const { return m_usedCount; }
        size_t freeBlockCount() const { return m_freeCount; }
        uint32_t largestFreeBlock() const { return m_root ? m_nodes[m_root].maxFree : 0u; }
        // Size of the used block starting at addr, 0 if none.
        uint32_t usedBlockSize(uint32_t addr) const;

        // Debug / test: checks every structural invariant. Returns false (and a reason) on corruption.
        bool validate(const char **why = nullptr) const;

    private:
        struct Node
        {
            uint32_t addr = 0;
            uint32_t size = 0;
            uint32_t prev = 0; // physical neighbours, 0 = none; reused as pool free-list link
            uint32_t next = 0;
            uint32_t left = 0; // treap links (free blocks only)
            uint32_t right = 0;
            uint32_t maxFree = 0; // max block size in this treap subtree
            uint32_t prio = 0;
            uint32_t hnext = 0; // hash chain (used blocks only)
            bool free = false;
        };

        uint32_t newNode(uint32_t addr, uint32_t size, bool isFree);
        void releaseNode(uint32_t n);
        uint32_t nextRandom();

        // treap over free blocks
        void pull(uint32_t t);
        void split(uint32_t t, uint32_t key, uint32_t &l, uint32_t &r);
        uint32_t merge(uint32_t a, uint32_t b);
        uint32_t treapInsert(uint32_t t, uint32_t n);
        uint32_t treapErase(uint32_t t, uint32_t key);
        void treapUpdate(uint32_t t, uint32_t oldKey, uint32_t newKey, uint32_t newSize);
        uint32_t findFit(uint32_t t, uint32_t size, uint32_t alignment) const;
        static bool fits(const Node &node, uint32_t size, uint32_t alignment);

        // used-block hash
        size_t bucketOf(uint32_t addr) const;
        void hashInsert(uint32_t n);
        void hashErase(uint32_t n);
        uint32_t hashFind(uint32_t addr) const;
        void growHash();

        void unlink(uint32_t n);
        void insertAfter(uint32_t after, uint32_t n); // physical list; after == 0 -> becomes head
        void freeNode(uint32_t n);
        uint32_t allocateRounded(uint32_t allocSize, uint32_t alignment);

        std::vector<Node> m_nodes; // [0] is the null sentinel
        std::vector<uint32_t> m_buckets;
        uint32_t m_hashShift = 22u;
        uint32_t m_poolFree = 0;
        uint32_t m_root = 0;
        uint32_t m_head = 0;
        uint32_t m_rng = 0x9E3779B9u;
        uint32_t m_base = 0;
        uint32_t m_limit = 0;
        uint32_t m_highWater = 0;
        size_t m_usedCount = 0;
        size_t m_freeCount = 0;
    };
}

#endif
