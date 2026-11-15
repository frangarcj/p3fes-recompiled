#if defined(PLATFORM_VITA)

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <psp2/kernel/threadmgr.h>

// Vita launchers do not supply a shell environment. Read the same runtime
// switches used on desktop before any of their function-local caches initialize.
// This also lets Vita3K boot tests change diagnostics without relinking the game.
void ps2xVitaConfigure()
{
    if (FILE *config = std::fopen("ux0:data/p3fes/runtime.cfg", "r"))
    {
        char line[2048];
        while (std::fgets(line, sizeof(line), config))
        {
            // Ignore an overlong entry rather than applying its truncated value.
            if (!std::strchr(line, '\n') && !std::feof(config))
            {
                int ch;
                while ((ch = std::fgetc(config)) != '\n' && ch != EOF) {}
                continue;
            }
            auto trim = [](char *text) {
                text += std::strspn(text, " \t\r\n");
                size_t length = std::strlen(text);
                while (length && std::strchr(" \t\r\n", text[length - 1]))
                    text[--length] = '\0';
                return text;
            };
            char *key = trim(line);
            if (*key == '#' || *key == '\0')
                continue;
            char *equals = std::strchr(key, '=');
            if (!equals)
                continue;
            *equals = '\0';
            key = trim(key);
            char *value = trim(equals + 1);
            if ((std::strncmp(key, "PS2X_", 5) == 0 || std::strncmp(key, "P3FES_", 6) == 0) &&
                std::strspn(key, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") == std::strlen(key))
                setenv(key, value, 1);
        }
        std::fclose(config);
    }
    if (const char *path = std::getenv("PS2X_LOG_FILE"); path && *path)
    {
        FILE *log = std::fopen(path, "w");
        if (!log)
            return;
        std::fclose(log);
        std::freopen(path, "a", stdout);
        std::freopen(path, "a", stderr);
        // A line-buffered log writes to the memory card on every line, on the thread that prints:
        // 20-60 ms each time on the console. The render thread's 5-second status lines alone held
        // a frame on screen for four or more refreshes every few seconds. Buffer the log in memory
        // and let a low-priority thread write it out once a second instead. PS2X_LOG_SYNC=1 keeps
        // the old behaviour (every line on disk at once, e.g. when chasing a crash).
        const char *sync = std::getenv("PS2X_LOG_SYNC");
        if (sync && *sync == '1')
        {
            std::setvbuf(stdout, nullptr, _IOLBF, 0);
            std::setvbuf(stderr, nullptr, _IOLBF, 0);
        }
        else
        {
            static char stdoutBuffer[64 * 1024];
            static char stderrBuffer[256 * 1024];
            std::setvbuf(stdout, stdoutBuffer, _IOFBF, sizeof(stdoutBuffer));
            std::setvbuf(stderr, stderrBuffer, _IOFBF, sizeof(stderrBuffer));
            std::thread([] {
                sceKernelChangeThreadPriority(sceKernelGetThreadId(), 180);
                sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000); // core 0: not the game's
                for (;;)
                {
                    sceKernelDelayThread(1000 * 1000);
                    std::fflush(stdout);
                    std::fflush(stderr);
                }
            }).detach();
        }
    }
}

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

// vita-elf-create appends its SCE data after the code segment and needs ~5 KB before the data segment, which the
// linker places at the next 1 MiB boundary. When the code ends within 5 KB of that boundary the conversion aborts
// ("Cannot allocate ... bytes for SCE data at end of segment 0"). This read-only pad moved the end of the code past
// the boundary the day that happened; change its size (or remove it) if the message comes back.
extern "C" __attribute__((used)) const char g_ps2xLayoutPad[16384] = {1};
