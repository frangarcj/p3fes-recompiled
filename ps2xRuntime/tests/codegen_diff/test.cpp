// Differential test driver: runs every selected leaf generated function with a
// deterministic pseudo-random initial state and prints hashes of the final
// R5900Context and guest RAM. Built twice (with/without -DPS2X_LEGACY_ACCESS);
// the two outputs must be identical.
#include "harness.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

// The real out-of-line slow path of the new access macros (no-op in legacy mode).
// Its calls into PS2Runtime::LoadN/StoreN land in the aborting stubs below.
#include "../../src/lib/ps2_slow_mem.cpp"

#include "chunk_table.inc"

// ---------------------------------------------------------------- stubs
static const char *g_reason = "";
static const char *g_name = "";
static int g_seed = 0;

[[noreturn]] static void die(const char *why)
{
    g_reason = why;
    abort();
}

bool PS2Runtime::eeCheckpointSlow(uint32_t) noexcept { die("eeCheckpointSlow(loop budget exhausted)"); }
uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t) { die("rt->Load8"); }
uint16_t PS2Runtime::Load16(uint8_t *, R5900Context *, uint32_t) { die("rt->Load16"); }
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *, uint32_t) { die("rt->Load32"); }
uint64_t PS2Runtime::Load64(uint8_t *, R5900Context *, uint32_t) { die("rt->Load64"); }
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t) { die("rt->Load128"); }
void PS2Runtime::Store8(uint8_t *, R5900Context *, uint32_t, uint8_t) { die("rt->Store8"); }
void PS2Runtime::Store16(uint8_t *, R5900Context *, uint32_t, uint16_t) { die("rt->Store16"); }
void PS2Runtime::Store32(uint8_t *, R5900Context *, uint32_t, uint32_t) { die("rt->Store32"); }
void PS2Runtime::Store64(uint8_t *, R5900Context *, uint32_t, uint64_t) { die("rt->Store64"); }
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t, __m128i) { die("rt->Store128"); }

// ---------------------------------------------------------------- PRNG / hash
struct Rng
{
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return (uint32_t)(next() >> 32); }
    uint32_t below(uint32_t n) { return (uint32_t)((next() >> 32) % n); }
};

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; ++s)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return h;
}

static uint64_t hashBytes(const void *p, size_t n, uint64_t h = 0x84222325CBF29CE4ull)
{
    const uint8_t *b = (const uint8_t *)p;
    uint64_t l0 = h, l1 = h ^ 0x1111, l2 = h ^ 0x2222, l3 = h ^ 0x3333;
    constexpr uint64_t P = 0x100000001B3ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32)
    {
        uint64_t w[4];
        std::memcpy(w, b + i, 32);
        l0 = ((l0 ^ w[0]) * P) ^ (l0 >> 31);
        l1 = ((l1 ^ w[1]) * P) ^ (l1 >> 31);
        l2 = ((l2 ^ w[2]) * P) ^ (l2 >> 31);
        l3 = ((l3 ^ w[3]) * P) ^ (l3 >> 31);
    }
    for (; i < n; ++i)
        l0 = (l0 ^ b[i]) * P;
    uint64_t r = l0;
    r = (r ^ l1) * P; r ^= r >> 29;
    r = (r ^ l2) * P; r ^= r >> 29;
    r = (r ^ l3) * P; r ^= r >> 29;
    return r;
}

// ---------------------------------------------------------------- initial state
constexpr size_t kRam = 32u * 1024u * 1024u;
constexpr size_t kPad = 16;
static uint8_t *g_ram = nullptr;
static uint8_t *g_pristine = nullptr;

struct ClassParams { int ptrN, smallN; };
static const ClassParams kRamClass[8] = {{8, 0}, {8, 0}, {0, 0}, {8, 2}, {10, 0}, {6, 4}, {4, 8}, {4, 6}};

static uint32_t randPtr(Rng &r, bool align16)
{
    uint32_t a = 0x00110000u + r.below(0x01EC0000u);
    if (align16)
        return a & ~0xFu;
    switch (r.below(8))
    {
    case 0: return a;                // arbitrary
    case 1: return a | 1u;           // odd
    case 2: return (a & ~1u) | 2u;
    case 3: return a & ~3u;
    case 4: return a & ~7u;
    case 5: return a & ~0xFu;
    case 6: return (a & ~0xFu) | 4u;
    default: return (a & ~0xFu) | 8u;
    }
}

static void fillRam(int seed)
{
    const int cls = seed & 7;
    const ClassParams cp = kRamClass[cls];
    Rng r(0xC0DE0000ull + (uint64_t)seed * 7919u);
    uint32_t *w = (uint32_t *)g_ram;
    const size_t n = (kRam + kPad) / 4;
    for (size_t i = 0; i + 1 < n; i += 2)
    {
        uint64_t x = r.next();
        uint64_t sel = (x * 0x9E3779B97F4A7C15ull) >> 56;
        for (int k = 0; k < 2; ++k)
        {
            uint32_t raw = (uint32_t)(x >> (32 * k));
            int nib = (int)((sel >> (4 * k)) & 15);
            uint32_t v;
            if (nib < cp.ptrN)
                v = 0x00110000u + (raw % 0x01EC0000u), v &= (raw & 0x40000000u) ? ~3u : ~0xFu;
            else if (nib < cp.ptrN + cp.smallN)
                v = (raw & 0x100u) ? (uint32_t)(-(int32_t)(raw & 0x3Fu)) : (raw & 0x7Fu);
            else
                v = raw;
            w[i + k] = v;
        }
    }
}

static void initContext(R5900Context &c, int seed, const char *name)
{
    const int cls = seed & 7;
    Rng r(fnv(name) * 31u + (uint64_t)seed * 0x9E3779B97F4A7C15ull + 12345u);
    auto setGpr = [&](int i, uint64_t lo, uint64_t hi)
    {
        c.r[i] = _mm_set_epi64x((int64_t)hi, (int64_t)lo);
    };
    for (int i = 1; i < 32; ++i)
    {
        uint64_t lo, hi = 0;
        auto sx = [](uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; };
        switch (cls)
        {
        case 0: lo = sx(randPtr(r, false)); break;
        case 1: lo = sx(randPtr(r, true)); hi = r.next(); break;
        case 2:
        {
            switch (r.below(6))
            {
            case 0: lo = sx(randPtr(r, false)); break;
            case 1: lo = r.below(64); break;
            case 2: lo = (uint64_t)(-(int64_t)r.below(64)); break;
            case 3: lo = sx(r.u32()); break;
            case 4: lo = r.next(); break;
            default: lo = sx(randPtr(r, true)); break;
            }
            if (r.below(4) == 0) hi = r.next();
            break;
        }
        case 3: // mirror windows of main RAM
        {
            uint32_t p = randPtr(r, r.below(2) == 0);
            static const uint32_t win[4] = {0x00000000u, 0x20000000u, 0x80000000u, 0xA0000000u};
            lo = sx(p | win[r.below(4)]);
            break;
        }
        case 4: // last bytes of RAM
            if (r.below(3))
                lo = sx(0x01FFFFC0u + r.below(0x40));
            else
                lo = sx(randPtr(r, false));
            break;
        case 5: // very low addresses
            if (r.below(2))
                lo = r.below(0x800);
            else
                lo = sx(randPtr(r, false));
            break;
        case 6: // small counters/indices mixed with pointers
            lo = r.below(3) ? r.below(40) : sx(randPtr(r, false));
            if (r.below(5) == 0) lo = (uint64_t)(-(int64_t)r.below(8));
            break;
        default: // as 6, with 16-aligned pointers and garbage upper halves
            lo = r.below(3) ? r.below(17) : sx(randPtr(r, true));
            hi = r.next();
            break;
        }
        setGpr(i, lo, hi);
    }
    // ABI registers.
    setGpr(29, (uint64_t)(int64_t)(int32_t)(0x01E00000u + (r.below(0x8000u) << 4)), 0);
    setGpr(28, (uint64_t)(int64_t)(int32_t)randPtr(r, true), 0);
    setGpr(31, 0x00100000ull, cls == 1 || cls == 7 ? r.next() : 0);
    c.hi = r.next(); c.lo = r.next(); c.hi1 = r.next(); c.lo1 = r.next();
    c.sa = r.below(32);
    c.pc = 0; // enter at the function start (default arm of any resume switch)
    for (int i = 0; i < 32; ++i)
    {
        float f;
        switch (r.below(10))
        {
        case 0: f = 0.0f; break;
        case 1: f = (float)(int32_t)(r.below(2000)) - 1000.0f; break;
        case 2: { uint32_t b = r.u32(); std::memcpy(&f, &b, 4); break; }
        case 3: f = 1.0f; break;
        default: f = ((float)r.u32() / 4294967296.0f - 0.5f) * 2000.0f; break;
        }
        c.f[i] = f;
    }
    c.f_acc = ((float)r.u32() / 4294967296.0f - 0.5f) * 100.0f;
    for (int i = 1; i < 32; ++i)
        c.vu0_vf[i] = _mm_setr_ps(r.u32() / 4294967296.0f * 10 - 5, r.u32() / 4294967296.0f * 10 - 5,
                                  r.u32() / 4294967296.0f * 10 - 5, r.u32() / 4294967296.0f * 10 - 5);
    c.vu0_acc = _mm_setr_ps(1.5f, -2.5f, 3.5f, -4.5f);
    for (int i = 0; i < 16; ++i) c.vi[i] = (uint16_t)r.u32();
}

// ---------------------------------------------------------------- child
static const char *g_dumpDir = nullptr;
// --emulate-wrap: make the 16 padding bytes behave like the wrapped start of RAM, to check that
// end-of-RAM wrap-around is the only remaining difference between the variants.
static bool g_emulateWrap = false;
static uint8_t g_pad0[kPad];
static PS2Runtime *g_rt = nullptr;

static void writeAll(const char *s)
{
    size_t n = strlen(s);
    while (n)
    {
        ssize_t w = write(1, s, n);
        if (w <= 0) break;
        s += w; n -= (size_t)w;
    }
}

static void onSignal(int sig)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%s %d FAIL %s\n", g_name, g_seed,
             *g_reason ? g_reason : (sig == SIGALRM ? "timeout" : sig == SIGSEGV || sig == SIGBUS ? "SIGSEGV/SIGBUS" : "signal"));
    writeAll(buf);
    _exit(100);
}

static void runChild(const FuncEntry &fe, int seed)
{
    g_name = fe.name; g_seed = seed;
    for (int s : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGALRM, SIGFPE})
        signal(s, onSignal);
    alarm(20);
    alignas(16) R5900Context ctx;
    initContext(ctx, seed, fe.name);
    g_rt->m_eeBudget = 32 * 200000;
    fe.fn(g_ram, &ctx, g_rt);
    alarm(0);
    if (g_emulateWrap)
        for (size_t i = 0; i < kPad; ++i)
            if (g_ram[kRam + i] != g_pad0[i])
            {
                g_ram[i] = g_ram[kRam + i];
                g_ram[kRam + i] = g_pad0[i];
            }

    const uint64_t hCtx = hashBytes(&ctx, sizeof ctx);
    const uint64_t hGpr = hashBytes(&ctx.r[0], sizeof ctx.r);
    const uint64_t hRam = hashBytes(g_ram, kRam + kPad);
    char buf[256];
    snprintf(buf, sizeof buf, "%s %d OK ctx=%016llx gpr=%016llx ram=%016llx pc=%08x\n", fe.name, seed,
             (unsigned long long)hCtx, (unsigned long long)hGpr, (unsigned long long)hRam, ctx.pc);
    writeAll(buf);

    if (g_dumpDir)
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s.%d.ctx", g_dumpDir, fe.name, seed);
        if (FILE *f = fopen(path, "wb")) { fwrite(&ctx, sizeof ctx, 1, f); fclose(f); }
        snprintf(path, sizeof path, "%s/%s.%d.ram", g_dumpDir, fe.name, seed);
        if (FILE *f = fopen(path, "w"))
        {
            const uint64_t *a = (const uint64_t *)g_ram, *b = (const uint64_t *)g_pristine;
            for (size_t i = 0; i < (kRam + kPad) / 8; ++i)
                if (a[i] != b[i])
                    fprintf(f, "%08zx %016llx <- %016llx\n", i * 8, (unsigned long long)a[i], (unsigned long long)b[i]);
            fclose(f);
        }
    }
    _exit(0);
}

int main(int argc, char **argv)
{
    int nseeds = 16;
    const char *only = nullptr;
    int onlySeed = -1;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--seeds" && i + 1 < argc) nseeds = atoi(argv[++i]);
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--seed" && i + 1 < argc) onlySeed = atoi(argv[++i]);
        else if (a == "--emulate-wrap") g_emulateWrap = true;
        else if (a == "--dump" && i + 1 < argc) g_dumpDir = argv[++i];
        else { fprintf(stderr, "usage: %s [--seeds N] [--only name] [--seed S] [--dump dir]\n", argv[0]); return 2; }
    }
    g_ram = (uint8_t *)mmap(nullptr, kRam + kPad + 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (g_dumpDir)
        g_pristine = (uint8_t *)mmap(nullptr, kRam + kPad + 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    g_rt = (PS2Runtime *)calloc(1, sizeof(PS2Runtime) + 64);

    int total = 0;
    for (const ChunkRef &c : g_chunks)
        total += *c.count;
    fprintf(stderr, "variant=%s functions=%d seeds=%d\n",
#if defined(PS2X_LEGACY_ACCESS)
            "legacy",
#else
            "new",
#endif
            total, nseeds);

    setvbuf(stdout, nullptr, _IONBF, 0);
    for (int seed = 0; seed < nseeds; ++seed)
    {
        if (onlySeed >= 0 && seed != onlySeed) continue;
        fillRam(seed);
        if (g_emulateWrap)
        {
            std::memcpy(g_ram + kRam, g_ram, kPad);
            std::memcpy(g_pad0, g_ram, kPad);
        }
        if (g_pristine) std::memcpy(g_pristine, g_ram, kRam + kPad);
        for (const ChunkRef &c : g_chunks)
            for (int i = 0; i < *c.count; ++i)
            {
                const FuncEntry &fe = c.entries[i];
                if (only && strcmp(only, fe.name) != 0) continue;
                pid_t pid = fork();
                if (pid == 0) runChild(fe, seed);
                int st = 0;
                waitpid(pid, &st, 0);
                if (!WIFEXITED(st) || (WEXITSTATUS(st) != 0 && WEXITSTATUS(st) != 100))
                {
                    char buf[256];
                    snprintf(buf, sizeof buf, "%s %d FAIL uncaught-status-%d\n", fe.name, seed, st);
                    writeAll(buf);
                }
            }
    }
    return 0;
}
