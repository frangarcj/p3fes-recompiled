#pragma once

#include <atomic>

// Host-side draw policy shared by the GS frontend and external/native
// presenters. This intentionally suppresses only primitive rasterization:
// register writes, transfers, CLUT loads, local-memory reads/writes and sync
// operations continue to flow through the normal GS backend.
namespace ps2x::gs
{
inline std::atomic<bool> g_primitiveRasterSuppressed{false};

inline void setPrimitiveRasterSuppressed(bool suppressed)
{
    g_primitiveRasterSuppressed.store(suppressed, std::memory_order_release);
}

inline bool primitiveRasterSuppressed()
{
    return g_primitiveRasterSuppressed.load(std::memory_order_acquire);
}
} // namespace ps2x::gs
