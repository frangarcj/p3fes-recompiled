#include "guest_heap.h"

#include <algorithm>
#include <limits>

namespace ps2x
{
    namespace
    {
        constexpr uint32_t kInitialBucketBits = 10u;

        inline bool isPow2(uint32_t v) { return v != 0u && (v & (v - 1u)) == 0u; }

        inline uint32_t normalizeAlignment(uint32_t alignment)
        {
            if (!isPow2(alignment))
            {
                return GuestHeap::kGranule;
            }
            return std::max(alignment, GuestHeap::kGranule);
        }

        inline uint32_t roundToGranule(uint32_t size)
        {
            return (size + (GuestHeap::kGranule - 1u)) & ~(GuestHeap::kGranule - 1u);
        }

        inline bool sizeOverflows(uint32_t size)
        {
            return size > (std::numeric_limits<uint32_t>::max() - (GuestHeap::kGranule - 1u));
        }
    }

    GuestHeap::GuestHeap()
    {
        m_nodes.reserve(1024);
        m_nodes.emplace_back(); // sentinel
        m_buckets.assign(size_t{1} << kInitialBucketBits, 0u);
        m_hashShift = 32u - kInitialBucketBits;
    }

    uint32_t GuestHeap::nextRandom()
    {
        // xorshift32, deterministic so runs are reproducible
        uint32_t x = m_rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        m_rng = x;
        return x;
    }

    uint32_t GuestHeap::newNode(uint32_t addr, uint32_t size, bool isFree)
    {
        uint32_t id;
        if (m_poolFree != 0u)
        {
            id = m_poolFree;
            m_poolFree = m_nodes[id].next;
        }
        else
        {
            id = static_cast<uint32_t>(m_nodes.size());
            m_nodes.emplace_back();
        }
        Node &n = m_nodes[id];
        n = Node{};
        n.addr = addr;
        n.size = size;
        n.free = isFree;
        n.maxFree = size;
        n.prio = nextRandom();
        return id;
    }

    void GuestHeap::releaseNode(uint32_t n)
    {
        m_nodes[n].next = m_poolFree;
        m_poolFree = n;
    }

    // ---- treap over free blocks (key: address, augmented with the subtree's max size) -------------

    void GuestHeap::pull(uint32_t t)
    {
        Node &n = m_nodes[t];
        uint32_t m = n.size;
        if (n.left)
        {
            m = std::max(m, m_nodes[n.left].maxFree);
        }
        if (n.right)
        {
            m = std::max(m, m_nodes[n.right].maxFree);
        }
        n.maxFree = m;
    }

    // l receives the nodes with addr < key, r the rest.
    void GuestHeap::split(uint32_t t, uint32_t key, uint32_t &l, uint32_t &r)
    {
        if (!t)
        {
            l = r = 0u;
            return;
        }
        if (m_nodes[t].addr < key)
        {
            uint32_t rightRoot = m_nodes[t].right;
            uint32_t a = 0u, b = 0u;
            split(rightRoot, key, a, b);
            m_nodes[t].right = a;
            pull(t);
            l = t;
            r = b;
        }
        else
        {
            uint32_t leftRoot = m_nodes[t].left;
            uint32_t a = 0u, b = 0u;
            split(leftRoot, key, a, b);
            m_nodes[t].left = b;
            pull(t);
            l = a;
            r = t;
        }
    }

    uint32_t GuestHeap::merge(uint32_t a, uint32_t b)
    {
        if (!a)
        {
            return b;
        }
        if (!b)
        {
            return a;
        }
        if (m_nodes[a].prio > m_nodes[b].prio)
        {
            const uint32_t m = merge(m_nodes[a].right, b);
            m_nodes[a].right = m;
            pull(a);
            return a;
        }
        const uint32_t m = merge(a, m_nodes[b].left);
        m_nodes[b].left = m;
        pull(b);
        return b;
    }

    uint32_t GuestHeap::treapInsert(uint32_t t, uint32_t n)
    {
        if (!t)
        {
            m_nodes[n].left = m_nodes[n].right = 0u;
            pull(n);
            return n;
        }
        if (m_nodes[n].prio > m_nodes[t].prio)
        {
            uint32_t a = 0u, b = 0u;
            split(t, m_nodes[n].addr, a, b);
            m_nodes[n].left = a;
            m_nodes[n].right = b;
            pull(n);
            return n;
        }
        if (m_nodes[n].addr < m_nodes[t].addr)
        {
            const uint32_t c = treapInsert(m_nodes[t].left, n);
            m_nodes[t].left = c;
        }
        else
        {
            const uint32_t c = treapInsert(m_nodes[t].right, n);
            m_nodes[t].right = c;
        }
        pull(t);
        return t;
    }

    uint32_t GuestHeap::treapErase(uint32_t t, uint32_t key)
    {
        if (!t)
        {
            return 0u;
        }
        if (m_nodes[t].addr == key)
        {
            return merge(m_nodes[t].left, m_nodes[t].right);
        }
        if (key < m_nodes[t].addr)
        {
            const uint32_t c = treapErase(m_nodes[t].left, key);
            m_nodes[t].left = c;
        }
        else
        {
            const uint32_t c = treapErase(m_nodes[t].right, key);
            m_nodes[t].right = c;
        }
        pull(t);
        return t;
    }

    // Changes the key/size of the node currently stored under oldKey. newKey must keep the in-order
    // position (it lies between the same neighbours), so no restructuring is needed.
    void GuestHeap::treapUpdate(uint32_t t, uint32_t oldKey, uint32_t newKey, uint32_t newSize)
    {
        if (!t)
        {
            return;
        }
        Node &n = m_nodes[t];
        if (n.addr == oldKey)
        {
            n.addr = newKey;
            n.size = newSize;
        }
        else if (oldKey < n.addr)
        {
            treapUpdate(n.left, oldKey, newKey, newSize);
        }
        else
        {
            treapUpdate(n.right, oldKey, newKey, newSize);
        }
        pull(t);
    }

    bool GuestHeap::fits(const Node &node, uint32_t size, uint32_t alignment)
    {
        if (node.size < size)
        {
            return false;
        }
        const uint64_t start = node.addr;
        const uint64_t end = start + node.size;
        const uint64_t mask = static_cast<uint64_t>(alignment) - 1u;
        const uint64_t aligned = (start + mask) & ~mask;
        return aligned + size <= end;
    }

    // Lowest-address free block that can hold `size` bytes at `alignment` (0 if none). With the default
    // 16-byte alignment every candidate with maxFree >= size fits, so this is a plain O(log n) descent.
    uint32_t GuestHeap::findFit(uint32_t t, uint32_t size, uint32_t alignment) const
    {
        while (t && m_nodes[t].maxFree >= size)
        {
            const Node &n = m_nodes[t];
            if (n.left && m_nodes[n.left].maxFree >= size)
            {
                const uint32_t r = findFit(n.left, size, alignment);
                if (r)
                {
                    return r;
                }
            }
            if (fits(n, size, alignment))
            {
                return t;
            }
            t = n.right;
        }
        return 0u;
    }

    // ---- used-block hash ----------------------------------------------------------------------

    size_t GuestHeap::bucketOf(uint32_t addr) const
    {
        return static_cast<size_t>(((addr >> 4) * 2654435761u) >> m_hashShift);
    }

    void GuestHeap::hashInsert(uint32_t n)
    {
        const size_t b = bucketOf(m_nodes[n].addr);
        m_nodes[n].hnext = m_buckets[b];
        m_buckets[b] = n;
        if (m_usedCount > m_buckets.size())
        {
            growHash();
        }
    }

    void GuestHeap::hashErase(uint32_t n)
    {
        uint32_t *link = &m_buckets[bucketOf(m_nodes[n].addr)];
        while (*link && *link != n)
        {
            link = &m_nodes[*link].hnext;
        }
        if (*link == n)
        {
            *link = m_nodes[n].hnext;
        }
        m_nodes[n].hnext = 0u;
    }

    uint32_t GuestHeap::hashFind(uint32_t addr) const
    {
        for (uint32_t i = m_buckets[bucketOf(addr)]; i; i = m_nodes[i].hnext)
        {
            if (m_nodes[i].addr == addr)
            {
                return i;
            }
        }
        return 0u;
    }

    void GuestHeap::growHash()
    {
        std::vector<uint32_t> old;
        old.swap(m_buckets);
        m_buckets.assign(old.size() * 2u, 0u);
        m_hashShift -= 1u;
        for (uint32_t head : old)
        {
            uint32_t i = head;
            while (i)
            {
                const uint32_t next = m_nodes[i].hnext;
                const size_t b = bucketOf(m_nodes[i].addr);
                m_nodes[i].hnext = m_buckets[b];
                m_buckets[b] = i;
                i = next;
            }
        }
    }

    // ---- physical (address ordered) list ------------------------------------------------------

    void GuestHeap::unlink(uint32_t n)
    {
        const uint32_t p = m_nodes[n].prev;
        const uint32_t x = m_nodes[n].next;
        if (p)
        {
            m_nodes[p].next = x;
        }
        else
        {
            m_head = x;
        }
        if (x)
        {
            m_nodes[x].prev = p;
        }
    }

    void GuestHeap::insertAfter(uint32_t after, uint32_t n)
    {
        const uint32_t x = after ? m_nodes[after].next : m_head;
        m_nodes[n].prev = after;
        m_nodes[n].next = x;
        if (after)
        {
            m_nodes[after].next = n;
        }
        else
        {
            m_head = n;
        }
        if (x)
        {
            m_nodes[x].prev = n;
        }
    }

    // ---- public interface -----------------------------------------------------------------------

    void GuestHeap::reset(uint32_t base, uint32_t limit)
    {
        m_nodes.resize(1);
        m_poolFree = 0u;
        m_root = 0u;
        m_head = 0u;
        m_usedCount = 0u;
        m_freeCount = 0u;
        std::fill(m_buckets.begin(), m_buckets.end(), 0u);
        m_base = base;
        m_limit = limit;
        m_highWater = base;
        if (limit > base)
        {
            const uint32_t n = newNode(base, limit - base, true);
            insertAfter(0u, n);
            m_root = treapInsert(0u, n);
            m_freeCount = 1u;
        }
    }

    uint32_t GuestHeap::allocateRounded(uint32_t allocSize, uint32_t alignment)
    {
        const uint32_t t = findFit(m_root, allocSize, alignment);
        if (!t)
        {
            return 0u;
        }

        const uint32_t blockAddr = m_nodes[t].addr;
        const uint32_t blockSize = m_nodes[t].size;
        const uint64_t blockEnd = static_cast<uint64_t>(blockAddr) + blockSize;
        const uint64_t mask = static_cast<uint64_t>(alignment) - 1u;
        const uint32_t alignedAddr = static_cast<uint32_t>((static_cast<uint64_t>(blockAddr) + mask) & ~mask);
        const uint64_t allocEnd = static_cast<uint64_t>(alignedAddr) + allocSize;
        const uint32_t prefix = alignedAddr - blockAddr;
        const uint32_t suffix = static_cast<uint32_t>(blockEnd - allocEnd);

        if (prefix == 0u)
        {
            if (suffix == 0u)
            {
                // exact fit: the free block itself becomes the used block
                m_root = treapErase(m_root, blockAddr);
                --m_freeCount;
                m_nodes[t].free = false;
                ++m_usedCount;
                hashInsert(t);
            }
            else
            {
                // used block in front, the free node keeps the remainder (key moves forward in place)
                const uint32_t u = newNode(blockAddr, allocSize, false);
                insertAfter(m_nodes[t].prev, u);
                treapUpdate(m_root, blockAddr, blockAddr + allocSize, suffix);
                ++m_usedCount;
                hashInsert(u);
            }
        }
        else
        {
            const uint32_t u = newNode(alignedAddr, allocSize, false);
            insertAfter(t, u);
            treapUpdate(m_root, blockAddr, blockAddr, prefix);
            if (suffix != 0u)
            {
                const uint32_t v = newNode(static_cast<uint32_t>(allocEnd), suffix, true);
                insertAfter(u, v);
                m_root = treapInsert(m_root, v);
                ++m_freeCount;
            }
            ++m_usedCount;
            hashInsert(u);
        }

        m_highWater = std::max(m_highWater, static_cast<uint32_t>(allocEnd));
        return alignedAddr;
    }

    uint32_t GuestHeap::allocate(uint32_t size, uint32_t alignment)
    {
        if (size == 0u || sizeOverflows(size))
        {
            return 0u;
        }
        return allocateRounded(roundToGranule(size), normalizeAlignment(alignment));
    }

    void GuestHeap::freeNode(uint32_t n)
    {
        hashErase(n);
        --m_usedCount;

        const uint32_t p = m_nodes[n].prev;
        const uint32_t x = m_nodes[n].next;
        const bool prevFree = p && m_nodes[p].free;
        const bool nextFree = x && m_nodes[x].free;

        if (prevFree)
        {
            uint32_t newSize = m_nodes[p].size + m_nodes[n].size;
            if (nextFree)
            {
                newSize += m_nodes[x].size;
                m_root = treapErase(m_root, m_nodes[x].addr);
                --m_freeCount;
                unlink(x);
                releaseNode(x);
            }
            unlink(n);
            releaseNode(n);
            treapUpdate(m_root, m_nodes[p].addr, m_nodes[p].addr, newSize);
        }
        else if (nextFree)
        {
            // the following free block grows backwards over n (same node, key moves back in place)
            const uint32_t oldKey = m_nodes[x].addr;
            const uint32_t newKey = m_nodes[n].addr;
            const uint32_t newSize = m_nodes[x].size + m_nodes[n].size;
            unlink(n);
            releaseNode(n);
            treapUpdate(m_root, oldKey, newKey, newSize);
        }
        else
        {
            m_nodes[n].free = true;
            m_root = treapInsert(m_root, n);
            ++m_freeCount;
        }
    }

    bool GuestHeap::free(uint32_t addr)
    {
        const uint32_t n = hashFind(addr);
        if (!n)
        {
            return false;
        }
        freeNode(n);
        return true;
    }

    uint32_t GuestHeap::usedBlockSize(uint32_t addr) const
    {
        const uint32_t n = hashFind(addr);
        return n ? m_nodes[n].size : 0u;
    }

    GuestHeap::ReallocResult GuestHeap::reallocate(uint32_t addr, uint32_t newSize, uint32_t alignment)
    {
        ReallocResult result;
        if (newSize == 0u || sizeOverflows(newSize))
        {
            return result;
        }
        const uint32_t n = hashFind(addr);
        if (!n)
        {
            return result;
        }

        const uint32_t normalizedAlignment = normalizeAlignment(alignment);
        const uint32_t requestedSize = roundToGranule(newSize);
        const uint32_t oldAddr = m_nodes[n].addr;
        const uint32_t oldSize = m_nodes[n].size;
        result.oldAddr = oldAddr;

        if (requestedSize <= oldSize)
        {
            if (requestedSize < oldSize)
            {
                const uint32_t tailAddr = oldAddr + requestedSize;
                const uint32_t tailSize = oldSize - requestedSize;
                m_nodes[n].size = requestedSize;
                const uint32_t x = m_nodes[n].next;
                if (x && m_nodes[x].free)
                {
                    treapUpdate(m_root, m_nodes[x].addr, tailAddr, m_nodes[x].size + tailSize);
                }
                else
                {
                    const uint32_t t = newNode(tailAddr, tailSize, true);
                    insertAfter(n, t);
                    m_root = treapInsert(m_root, t);
                    ++m_freeCount;
                }
            }
            result.addr = oldAddr;
            return result;
        }

        const uint32_t x = m_nodes[n].next;
        if (x && m_nodes[x].free &&
            static_cast<uint64_t>(oldSize) + m_nodes[x].size >= requestedSize)
        {
            const uint32_t extraNeeded = requestedSize - oldSize;
            const uint32_t nextAddr = m_nodes[x].addr;
            const uint32_t nextSize = m_nodes[x].size;
            m_nodes[n].size = requestedSize;
            if (nextSize == extraNeeded)
            {
                m_root = treapErase(m_root, nextAddr);
                --m_freeCount;
                unlink(x);
                releaseNode(x);
            }
            else
            {
                treapUpdate(m_root, nextAddr, nextAddr + extraNeeded, nextSize - extraNeeded);
            }
            m_highWater = std::max(m_highWater, oldAddr + requestedSize);
            result.addr = oldAddr;
            return result;
        }

        const uint32_t newAddr = allocateRounded(requestedSize, normalizedAlignment);
        if (newAddr == 0u)
        {
            return result;
        }
        result.addr = newAddr;
        result.copyBytes = std::min(oldSize, newSize);
        freeNode(n);
        return result;
    }

    // ---- validation (tests / debugging) ---------------------------------------------------------

    bool GuestHeap::validate(const char **why) const
    {
        auto fail = [&](const char *msg)
        {
            if (why)
            {
                *why = msg;
            }
            return false;
        };

        // physical list tiles [base, limit)
        size_t used = 0, freeCnt = 0;
        uint32_t expect = m_base;
        uint32_t prev = 0;
        bool prevFree = false;
        for (uint32_t i = m_head; i; i = m_nodes[i].next)
        {
            const Node &n = m_nodes[i];
            if (n.prev != prev)
                return fail("bad prev link");
            if (n.addr != expect)
                return fail("blocks do not tile the heap");
            if (n.size == 0u)
                return fail("empty block");
            if (n.free)
            {
                if (prevFree)
                    return fail("adjacent free blocks");
                ++freeCnt;
            }
            else
            {
                ++used;
                if (hashFind(n.addr) != i)
                    return fail("used block missing from hash");
            }
            prevFree = n.free;
            prev = i;
            expect = n.addr + n.size;
        }
        if (m_limit > m_base && expect != m_limit)
            return fail("blocks do not reach the limit");
        if (used != m_usedCount || freeCnt != m_freeCount)
            return fail("block counters out of sync");

        // hash chains only hold used blocks
        size_t hashed = 0;
        for (uint32_t head : m_buckets)
        {
            for (uint32_t i = head; i; i = m_nodes[i].hnext)
            {
                if (m_nodes[i].free)
                    return fail("free block in hash");
                ++hashed;
            }
        }
        if (hashed != m_usedCount)
            return fail("hash count mismatch");

        // treap: ordered, heap-ordered by prio, augmented maxima right, holds exactly the free blocks
        struct Frame
        {
            uint32_t t;
            int state;
        };
        std::vector<Frame> stack;
        size_t treapCount = 0;
        bool haveLast = false;
        uint32_t lastKey = 0;
        if (m_root)
            stack.push_back({m_root, 0});
        while (!stack.empty())
        {
            Frame &f = stack.back();
            const Node &n = m_nodes[f.t];
            if (f.state == 0)
            {
                f.state = 1;
                if (n.left)
                {
                    if (m_nodes[n.left].prio > n.prio)
                        return fail("treap priority violation");
                    stack.push_back({n.left, 0});
                }
                continue;
            }
            if (f.state == 1)
            {
                if (!n.free)
                    return fail("used block in treap");
                if (haveLast && n.addr <= lastKey)
                    return fail("treap order violation");
                haveLast = true;
                lastKey = n.addr;
                ++treapCount;
                uint32_t m = n.size;
                if (n.left)
                    m = std::max(m, m_nodes[n.left].maxFree);
                if (n.right)
                    m = std::max(m, m_nodes[n.right].maxFree);
                if (m != n.maxFree)
                    return fail("stale maxFree");
                f.state = 2;
                if (n.right)
                {
                    if (m_nodes[n.right].prio > n.prio)
                        return fail("treap priority violation");
                    stack.push_back({n.right, 0});
                }
                continue;
            }
            stack.pop_back();
        }
        if (treapCount != m_freeCount)
            return fail("treap does not hold every free block");

        // node pool accounting
        size_t pooled = 0;
        for (uint32_t i = m_poolFree; i; i = m_nodes[i].next)
            ++pooled;
        if (m_nodes.size() - 1u != used + freeCnt + pooled)
            return fail("node pool leak");
        return true;
    }
}
