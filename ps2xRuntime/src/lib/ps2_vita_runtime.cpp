#if defined(PLATFORM_VITA)

#include <cstdarg>
#include <cstdio>

// Heap sizes must live in a native object as well(bc of LTO).
// the newlib malloc/new arena (PS2 guest RAM 32MB + GS + buffers live here).
// Reused from Celosia's Vita memory setup: its recompiled PS2 runtime needed
// substantially more than 96 MB once guest RAM, GS state and renderer buffers
// were live together. Keep enough headroom for the same workload here.
extern "C" unsigned int _newlib_heap_size_user = 176u * 1024u * 1024u;
extern "C" unsigned int sceLibcHeapSize = 8u * 1024u * 1024u;
// Recompiled guest code can nest native C++ calls deeply. Give both the main
// thread and native pthreads substantially more headroom than VitaSDK's small
// defaults; the runtime has several C++/renderer call chains that are unsafe on
// a 32 KiB pthread stack.
extern "C" unsigned int sceUserMainThreadStackSize = 16u * 1024u * 1024u;
extern "C" unsigned int _pthread_stack_default_user = 256u * 1024u;

// Celosia's host shim calls this optional runtime hook once every few seconds.
// Persona 3 does not yet have Celosia's emergency-heap controller, so keep the
// ABI and make the stock path a no-op rather than leaving an unresolved symbol.
extern "C" void ps2x_vita_heap_tick(double, size_t)
{
}

namespace
{
    void ttyPuts(const char *text)
    {
        if (text)
        {
            std::fputs(text, stdout); // Vita TTY (capturable via PrincessLog etc.)
        }
    }
}

// Overrides raylib's debugnet transport with TTY output.
extern "C"
{
    int debugNetInit(const char *, int, int)
    {
        return 0;
    }

    void debugNetFinish(void)
    {
    }

    int debugNetUDPSend(const char *text)
    {
        ttyPuts(text);
        return 0;
    }

    int debugNetUDPPrintf(const char *fmt, ...)
    {
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        ttyPuts(line);
        return 0;
    }

    int debugNetPrintf(int level, const char *fmt, ...)
    {
        (void)level;
        char line[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(line, sizeof(line), fmt, args);
        va_end(args);
        ttyPuts(line);
        return 0;
    }
}

#endif // PLATFORM_VITA
