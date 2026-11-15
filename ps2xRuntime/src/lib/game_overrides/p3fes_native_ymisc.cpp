#include "p3fes_native_ymisc.h"
#include "p3fes_native_effects.h"
#include "p3fes_native_fnprof.h"
#include "p3fes_native_leaf.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

// y_misc_004281f0 (0x4281F0..0x429C60) is the per-frame update of the Tartarus floor-tile task (the dungeon block
// streamer: a 16x16 grid of tile tasks at [state + 0x5C + z*0x80 + x*8], a (state[0xB28] + 4)^2 window of cells around the
// player; state byte [state] selects the phase, 14 is the steady one). For every cell of the window it calls five helpers:
//
//     0x42C0A0 (party member 1,2,3)   0x42CFC0   0x42DD90   0x42EA60   0x42F440     all as (task, listIdx|x, x|z, z|.., ..)
//
// Each helper looks for guest actors / effects standing in the cell (x, z) and, only when it finds one, builds or
// updates something there (task objects through 0x1158B0, positions in the state block, ...). The common case, a
// cell without any actor, runs the whole scan of the helper and changes nothing: the scan alone is the bulk of the
// function's cost (8.6 % of the console game thread in Tartarus; the helpers' slot loops run ~435 times per frame).
//
// The cut: each helper is hooked at its entry; the hook evaluates the helper's own "is something in this cell"
// predicate natively (same memory reads, same guest-exact float math for the position -> grid cell conversion) and, when
// no slot matches, returns to the caller immediately, which is exactly what the original does (proved below per helper
// from the disassembly, and by the verify mode). When something matches, or anything looks unusual, the original
// helper runs unchanged.
//
//   0x42C0A0 (task, idx, x, z, ..):  A = 0x8717A0 + (int8)idx * 0x1C0. Active iff [A+0x48] != 0 && [A+0x54] != 0.
//            Match iff act = [[A+0x54]+0x1E0]; XGrid(act) == (int16)x && ZGrid(act) == (int16)z, where
//            XGrid(a) = [[a+0x3C]+0x2C] and ZGrid(a) = [[a+0x3C]+0x30] (0x1AD910 / 0x1AD920).
//            No match: returns at 0x42CD40 without any store (0x42C140 / 0x42C16C / 0x42C18C branch there).
//   0x42CFC0 (task, x, z, ..):       24 slots at 0x86EDA0 + i * 0x1C0, same active test and match test as above.
//            No match: the loop tail 0x42DD40 (the `sb $zero,0x95C210+i` at 0x42DD3C is only on the match path).
//   0x42DD90 (task, x, z, ..):       8 slots at 0x86BE00 + i * 0x10, active iff [slot] != 0; position (float x at +4,
//            z at +0xC); match iff 0x44F120(slot+4) == (int16)x && 0x44F170(slot+4) == (int16)z.
//   0x42EA60 (task, x, z, ..):       32 slots at 0x86BE80 + i * 0x138, active iff [slot] != 0; position at +0x10C / +0x114.
//   0x42F440 (task, x, z, ..):       one entity: active iff [0x86E580] == 1; position at 0x86E684 / 0x86E68C.
//   0x44F120(p) = cvt.w.s((400.0 + p.x) / 800.0), 0x44F170(p) = cvt.w.s((400.0 + p.z) / 800.0) (PS2 float semantics).
// None of the five helpers has other side effects before its slot loop(s) (only stack stores); caller-saved registers
// are dead after the calls (the caller ignores $v0), so leaving them untouched is a valid result.
//
// Stage 2 (this file, mainHook): the function itself. In the steady phase (state byte [state] == 14, [state+0x864] != 0,
// [state+0x865] == 0) it is a read-only precheck plus a native replay of the recompiled code in the same order (hide the
// live tiles, the player/party marker bookkeeping, then the (B28+4)^2 window with the tile positions and the helpers),
// calling the few non-trivial guest callees (0x423FE0, 0x42A550, 0x429EF0, 0x429FE0, 0x42CD80 and a helper that found an
// actor in its cell) on a private context copy; the helpers' empty-cell scans come from a per-call cache of the slot cells
// (invalidated by every guest call that may write the slot tables). Every other situation runs the original.
//
// P3FES_NATIVE_YMISC: see the header.
namespace p3fes_native_ymisc
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;

        enum class Mode : uint8_t { Off, On, Verify, Log };
        enum class MainMode : uint8_t { Off, Native, Verify };
        MainMode g_mainMode = MainMode::Off;

        constexpr uint32_t kMainFn = 0x004281F0u;
        constexpr uint32_t kXGrid = 0x001AD910u;
        constexpr uint32_t kZGrid = 0x001AD920u;
        constexpr uint32_t kCopyPos = 0x001AD940u;
        constexpr uint32_t kGrid120 = 0x0044F120u;
        constexpr uint32_t kGrid170 = 0x0044F170u;

        enum Helper : uint32_t { H_42C0A0, H_42CFC0, H_42DD90, H_42EA60, H_42F440, H_COUNT };
        constexpr uint32_t kHelperAddr[H_COUNT] = {0x0042C0A0u, 0x0042CFC0u, 0x0042DD90u, 0x0042EA60u, 0x0042F440u};
        const char *const kHelperName[H_COUNT] = {"42C0A0", "42CFC0", "42DD90", "42EA60", "42F440"};

        constexpr uint32_t kRamSize = PS2_RAM_SIZE;

        struct Counters
        {
            uint64_t calls[H_COUNT] = {};
            uint64_t fast[H_COUNT] = {};       // returned natively
            uint64_t orig[H_COUNT] = {};       // original ran (a slot matched or looked unusual)
            uint64_t unsure[H_COUNT] = {};     // subset of orig: pointer outside RAM
            uint64_t ns[H_COUNT] = {};         // log mode: time of the original
            uint64_t verifyPath[H_COUNT] = {}; // verify: native no-match calls checked against the original's callee counts
            uint64_t verifyPathBad[H_COUNT] = {};
            uint64_t verifyMem[H_COUNT] = {};  // verify: of those, RAM compared in full
            uint64_t verifyMemBad[H_COUNT] = {};
            uint64_t verifyRegBad[H_COUNT] = {};
            uint64_t verifyUnwound[H_COUNT] = {};
            uint64_t memBytes = 0u;
            // stage 2 (whole function)
            uint64_t mainNative = 0u, mainFallback = 0u, mainFallbackRare = 0u, mainFallbackBad = 0u;
            uint64_t mainVerified = 0u, mainVerifyBad = 0u, mainVerifyRegBad = 0u, mainVerifyUnwound = 0u, mainVerifySkipped = 0u;
            uint64_t helperRun = 0u, cellsTotal = 0u, tileCells = 0u, cacheBad = 0u, mainFailed = 0u;
        } g_c;

        GuestFunction g_origHelper[H_COUNT] = {};
        GuestFunction g_origMain = nullptr;
        bool g_timeEval = false;
        bool g_timeMain = false;
        uint64_t g_seg[8] = {};
        bool g_traceEnabled = false;
        uint64_t g_verifyCounter = 0u;
        uint32_t g_verifyMainEvery = 4u;
        // P3FES_YMISC_TIME=1: time every call of the whole function (diagnostic; identical in all modes)
        uint64_t g_mainCalls = 0u, g_mainNs = 0u, g_mainMaxNs = 0u, g_mainUnwound = 0u;
        GuestFunction g_origProbe[5] = {};
        Mode g_mode = Mode::Off;
        bool g_diag = false;

        // ---- guest memory -------------------------------------------------------------------------------

        inline uint32_t r32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        inline float rf(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        inline bool inRam(uint32_t pointer)
        {
            return pointer != 0u && pointer < kRamSize && (pointer & 3u) == 0u;
        }

        inline int32_t sext16(uint32_t v) { return static_cast<int16_t>(static_cast<uint16_t>(v)); }

        // 0x44F120 / 0x44F170: cvt.w.s((400.0 + p) / 800.0) with the guest FPU rules.
        inline int32_t gridCoord(float p)
        {
            return ps2FpuConvertToWord(ps2_fdiv(ps2_fadd(400.0f, p), 800.0f));
        }

        // The callee calls the original would make on its no-match path (verify mode compares them).
        struct Calls
        {
            uint32_t n[5] = {}; // XGrid, ZGrid, 0x44F120, 0x44F170, CopyPos
        };
        enum { C_XGRID, C_ZGRID, C_G120, C_G170, C_COPYPOS };

        enum class Verdict : uint8_t { NoMatch, Match, Unsure };

        // act = the actor object whose 0x3C pointer holds the grid cell: XGrid = [[act+0x3C]+0x2C], ZGrid = [..+0x30]
        // (calls: the original calls XGrid first and ZGrid only when X is equal).
        inline Verdict gridOfObject(const uint8_t *rdram, uint32_t act, int32_t x, int32_t z, Calls &calls)
        {
            if (!inRam(act))
                return Verdict::Unsure;
            const uint32_t block = r32(rdram, act + 0x3Cu);
            if (!inRam(block))
                return Verdict::Unsure;
            ++calls.n[C_XGRID];
            if (static_cast<int32_t>(r32(rdram, block + 0x2Cu)) != x)
                return Verdict::NoMatch;
            ++calls.n[C_ZGRID];
            return static_cast<int32_t>(r32(rdram, block + 0x30u)) == z ? Verdict::Match : Verdict::NoMatch;
        }

        // slot with an actor pointer pair at +0x48 / +0x54 (0x42C0A0 / 0x42CFC0)
        inline Verdict actorSlot(const uint8_t *rdram, uint32_t slot, int32_t x, int32_t z, Calls &calls)
        {
            if (r32(rdram, slot + 0x48u) == 0u)
                return Verdict::NoMatch;
            const uint32_t holder = r32(rdram, slot + 0x54u);
            if (holder == 0u)
                return Verdict::NoMatch;
            if (!inRam(holder))
                return Verdict::Unsure;
            return gridOfObject(rdram, r32(rdram, holder + 0x1E0u), x, z, calls);
        }

        // float position at +0 (x) and +8 (z) of `pos`: the original calls 0x44F120 and, when equal, 0x44F170
        inline Verdict positionCell(const uint8_t *rdram, uint32_t pos, int32_t x, int32_t z, Calls &calls)
        {
            ++calls.n[C_G120];
            if (gridCoord(rf(rdram, pos)) != x)
                return Verdict::NoMatch;
            ++calls.n[C_G170];
            return gridCoord(rf(rdram, pos + 8u)) == z ? Verdict::Match : Verdict::NoMatch;
        }

        Verdict evaluate(Helper helper, const uint8_t *rdram, uint32_t a1, uint32_t a2, uint32_t a3, Calls &calls)
        {
            switch (helper)
            {
            case H_42C0A0:
            {
                const int32_t idx = static_cast<int8_t>(static_cast<uint8_t>(a1));
                const uint32_t slot = 0x008717A0u + static_cast<uint32_t>(idx) * 0x1C0u;
                return actorSlot(rdram, slot, sext16(a2), sext16(a3), calls);
            }
            case H_42CFC0:
            {
                const int32_t x = sext16(a1), z = sext16(a2);
                for (uint32_t i = 0u; i < 24u; ++i)
                {
                    const Verdict v = actorSlot(rdram, 0x0086EDA0u + i * 0x1C0u, x, z, calls);
                    if (v != Verdict::NoMatch)
                        return v;
                }
                return Verdict::NoMatch;
            }
            case H_42DD90:
            {
                const int32_t x = sext16(a1), z = sext16(a2);
                for (uint32_t i = 0u; i < 8u; ++i)
                {
                    const uint32_t slot = 0x0086BE00u + i * 0x10u;
                    if (r32(rdram, slot) == 0u)
                        continue;
                    const Verdict v = positionCell(rdram, slot + 4u, x, z, calls);
                    if (v != Verdict::NoMatch)
                        return v;
                }
                return Verdict::NoMatch;
            }
            case H_42EA60:
            {
                const int32_t x = sext16(a1), z = sext16(a2);
                for (uint32_t i = 0u; i < 32u; ++i)
                {
                    const uint32_t slot = 0x0086BE80u + i * 0x138u;
                    if (r32(rdram, slot) == 0u)
                        continue;
                    const Verdict v = positionCell(rdram, slot + 0x10Cu, x, z, calls);
                    if (v != Verdict::NoMatch)
                        return v;
                }
                return Verdict::NoMatch;
            }
            case H_42F440:
            default:
            {
                if (r32(rdram, 0x0086E580u) != 1u)
                    return Verdict::NoMatch;
                return positionCell(rdram, 0x0086E684u, sext16(a1), sext16(a2), calls);
            }
            }
        }

        inline Verdict evaluate(Helper helper, const uint8_t *rdram, const R5900Context *ctx, Calls &calls)
        {
            return evaluate(helper, rdram, getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7), calls);
        }

        inline void returnToCaller(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        inline uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        inline uint64_t segNow() { return g_timeEval ? nowNs() : 0u; }

        // ---- verify mode ----------------------------------------------------------------------------------

        uint32_t g_probeCount[5] = {};
        bool g_inHelper = false;

        template <size_t I>
        void probeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_probeCount[I];
            g_origProbe[I](rdram, ctx, runtime);
        }

        std::vector<uint8_t> g_before;
        uint32_t g_verifyEvery = 128u;

        void verifyHelper(Helper helper, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Calls calls;
            const Verdict verdict = evaluate(helper, rdram, ctx, calls);
            const uint32_t ra = getRegU32(ctx, 31);
            if (verdict != Verdict::NoMatch)
            {
                ++g_c.orig[helper];
                if (verdict == Verdict::Unsure)
                    ++g_c.unsure[helper];
                g_origHelper[helper](rdram, ctx, runtime);
                return;
            }
            ++g_c.fast[helper];

            const bool compareRam = (g_c.fast[helper] <= 60u) || (g_c.fast[helper] % g_verifyEvery == 0u);
            const uint32_t sp = getRegU32(ctx, 29);
            if (compareRam)
            {
                g_before.resize(kRamSize);
                std::memcpy(g_before.data(), rdram, kRamSize);
            }
            uint32_t savedRegs[24];
            static const int kSaved[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
            for (int i = 0; i < 12; ++i)
                savedRegs[i] = getRegU32(ctx, kSaved[i]);
            float savedF[12];
            for (int i = 0; i < 6; ++i)
                savedF[i] = ctx->f[20 + 2 * i];

            for (int i = 0; i < 5; ++i)
                g_probeCount[i] = 0u;
            g_origHelper[helper](rdram, ctx, runtime);
            if (ctx->pc != ra)
            {
                ++g_c.verifyUnwound[helper];
                return;
            }
            ++g_c.verifyPath[helper];
            bool pathBad = false;
            for (int i = 0; i < 5; ++i)
                pathBad |= g_probeCount[i] != calls.n[i];
            if (pathBad)
            {
                ++g_c.verifyPathBad[helper];
                std::fprintf(stderr, "[ymisc] PATH MISMATCH %s a1=%08X a2=%08X a3=%08X orig calls X%u Z%u G120 %u G170 %u CP%u native X%u Z%u G120 %u G170 %u CP%u\n",
                             kHelperName[helper], getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 7),
                             g_probeCount[0], g_probeCount[1], g_probeCount[2], g_probeCount[3], g_probeCount[4],
                             calls.n[0], calls.n[1], calls.n[2], calls.n[3], calls.n[4]);
            }
            bool regBad = false;
            for (int i = 0; i < 12; ++i)
                regBad |= savedRegs[i] != getRegU32(ctx, kSaved[i]);
            for (int i = 0; i < 6; ++i)
                regBad |= std::memcmp(&savedF[i], &ctx->f[20 + 2 * i], sizeof(float)) != 0;
            if (regBad)
                ++g_c.verifyRegBad[helper];
            if (compareRam)
            {
                ++g_c.verifyMem[helper];
                const uint32_t lowGuard = (sp - 0x800u) & PS2_RAM_MASK, highGuard = (sp + 0x10u) & PS2_RAM_MASK;
                uint32_t bad = 0u;
                const uint8_t *after = rdram;
                const uint8_t *before = g_before.data();
                for (uint32_t base = 0u; base < kRamSize; base += 4096u)
                {
                    if (std::memcmp(before + base, after + base, 4096u) == 0)
                        continue;
                    for (uint32_t i = base; i < base + 4096u; i += 4u)
                    {
                        if (i >= lowGuard && i < highGuard)
                            continue;
                        if (std::memcmp(before + i, after + i, 4u) != 0)
                        {
                            if (bad < 4u)
                                std::fprintf(stderr, "[ymisc] MEM MISMATCH %s at %08X: %08X -> %08X\n", kHelperName[helper], i,
                                             r32(before, i), r32(after, i));
                            ++bad;
                        }
                    }
                }
                g_c.memBytes += kRamSize;
                if (bad)
                    ++g_c.verifyMemBad[helper];
            }
        }

        // ---- stage 2: the whole function, state 14 with [state+0x864] != 0 ---------------------------------------
        //
        // The steady phase of the task (the floor is shown): see the header comment. Everything below follows the
        // recompiled code in the order it executes; the callees that are not trivial leaf functions are run through
        // callGuest() on a private copy of the context (they never block: the closure of their direct calls has no
        // syscall, and the indirect calls are task allocators), the others are done in place:
        //   0x456400(a, v)        [[a+0x3C]+0x148] = (u8)v          0x4302A0(a)  [[a+0x3C]+0x18] = 0
        //   0x4561D0(a, f12, f13) [[a+0x3C]+0x120] = f12, +0x124 = f13
        //   0x195460 (kwlnTaskExists) = membership of the task in the three lists headed at gp-0x4C8C, gp-0x4C74, gp-0x4C80
        //   0x1AD910 / 0x1AD920 / 0x1B9120 (grid x, grid z, field base 0x869F60)
        // Anything that would take a branch the native code does not implement (alternate [state+0x865] math, the
        // stair-cell block, state != 14, [state+0x864] == 0, the debug print at 0x429BCC, pointers outside RAM) is
        // detected before the first store (precheck) and the original runs.

        constexpr uint32_t kFieldBase = 0x00869F60u;
        constexpr uint32_t kSentinel = 0xFFFFFFFCu;

        inline uint8_t r8(const uint8_t *rdram, uint32_t address) { return rdram[address & PS2_RAM_MASK]; }
        inline uint16_t r16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }
        inline void w8(uint8_t *rdram, uint32_t address, uint8_t value) { rdram[address & PS2_RAM_MASK] = value; }
        inline void w32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }
        inline void wf(uint8_t *rdram, uint32_t address, float value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }
        inline int32_t s8(uint32_t v) { return static_cast<int8_t>(static_cast<uint8_t>(v)); }

        inline void setRegS32(R5900Context &c, int reg, int32_t v) { c.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(v)); }

        // Per-call cache of what the helpers' slot scans look for: the (x, z) cell of every active slot. Valid until a
        // guest function that may write the slot tables runs (every callGuest except the pure 0x429EF0 / 0x429FE0).
        struct CellSet
        {
            bool unsure = false;
            uint32_t n = 0u;
            int32_t x[40], z[40];
            inline void add(int32_t cx, int32_t cz)
            {
                if (n < 40u)
                {
                    x[n] = cx;
                    z[n] = cz;
                    ++n;
                }
                else
                    unsure = true;
            }
            inline bool hit(int32_t cx, int32_t cz) const
            {
                if (unsure)
                    return true;
                for (uint32_t i = 0u; i < n; ++i)
                    if (x[i] == cx && z[i] == cz)
                        return true;
                return false;
            }
        };
        struct HelperCache
        {
            bool valid = false;
            CellSet party[3]; // 0x42C0A0 for member 1..3
            CellSet set[H_COUNT];
        } g_cache;

        inline void cacheActor(const uint8_t *rdram, uint32_t slot, CellSet &out)
        {
            if (r32(rdram, slot + 0x48u) == 0u)
                return;
            const uint32_t holder = r32(rdram, slot + 0x54u);
            if (holder == 0u)
                return;
            if (!inRam(holder))
            {
                out.unsure = true;
                return;
            }
            const uint32_t act = r32(rdram, holder + 0x1E0u);
            if (!inRam(act))
            {
                out.unsure = true;
                return;
            }
            const uint32_t block = r32(rdram, act + 0x3Cu);
            if (!inRam(block))
            {
                out.unsure = true;
                return;
            }
            out.add(static_cast<int32_t>(r32(rdram, block + 0x2Cu)), static_cast<int32_t>(r32(rdram, block + 0x30u)));
        }

        void buildCache(const uint8_t *rdram)
        {
            g_cache = HelperCache();
            for (uint32_t k = 0u; k < 3u; ++k)
                cacheActor(rdram, 0x008717A0u + (k + 1u) * 0x1C0u, g_cache.party[k]);
            for (uint32_t i = 0u; i < 24u; ++i)
                cacheActor(rdram, 0x0086EDA0u + i * 0x1C0u, g_cache.set[H_42CFC0]);
            for (uint32_t i = 0u; i < 8u; ++i)
            {
                const uint32_t slot = 0x0086BE00u + i * 0x10u;
                if (r32(rdram, slot) != 0u)
                    g_cache.set[H_42DD90].add(gridCoord(rf(rdram, slot + 4u)), gridCoord(rf(rdram, slot + 12u)));
            }
            for (uint32_t i = 0u; i < 32u; ++i)
            {
                const uint32_t slot = 0x0086BE80u + i * 0x138u;
                if (r32(rdram, slot) != 0u)
                    g_cache.set[H_42EA60].add(gridCoord(rf(rdram, slot + 0x10Cu)), gridCoord(rf(rdram, slot + 0x114u)));
            }
            if (r32(rdram, 0x0086E580u) == 1u)
                g_cache.set[H_42F440].add(gridCoord(rf(rdram, 0x0086E684u)), gridCoord(rf(rdram, 0x0086E68Cu)));
            g_cache.valid = true;
        }


        struct NativeEnv
        {
            uint8_t *rdram;
            PS2Runtime *runtime;
            R5900Context *ctx;
            uint32_t gp;
            uint32_t spMain; // the stack pointer the original has after its prologue
            bool failed = false;
        };

        // Runs a guest function to completion on a copy of the caller's context. Returns $v0; f0 in *f0.
        uint32_t callGuest(NativeEnv &env, uint32_t entry, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0,
                           uint32_t t1, float f12, float f13, float *f0 = nullptr, GuestFunction first = nullptr)
        {
            if (entry != 0x00429EF0u && entry != 0x00429FE0u)
                g_cache.valid = false;
            R5900Context helper = *env.ctx;
            helper.pc = entry;
            setRegS32(helper, 4, static_cast<int32_t>(a0));
            setRegS32(helper, 5, static_cast<int32_t>(a1));
            setRegS32(helper, 6, static_cast<int32_t>(a2));
            setRegS32(helper, 7, static_cast<int32_t>(a3));
            setRegS32(helper, 8, static_cast<int32_t>(t0));
            setRegS32(helper, 9, static_cast<int32_t>(t1));
            setRegS32(helper, 29, static_cast<int32_t>(env.spMain));
            setRegS32(helper, 31, static_cast<int32_t>(kSentinel));
            helper.f[12] = f12;
            helper.f[13] = f13;
            if (first)
                first(env.rdram, &helper, env.runtime);
            for (uint32_t resume = 0u; resume < 100000u; ++resume)
            {
                if (env.runtime->isStopRequested() || helper.pc == 0u || helper.pc == kSentinel || !env.runtime->hasFunction(helper.pc))
                    break;
                env.runtime->lookupFunction(helper.pc)(env.rdram, &helper, env.runtime);
            }
            if (helper.pc != kSentinel)
            {
                env.failed = true;
                return 0u;
            }
            if (f0)
                *f0 = helper.f[0];
            return getRegU32(&helper, 2);
        }

        inline void callGuestHelper(NativeEnv &env, Helper h, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t t0, uint32_t t1)
        {
            callGuest(env, kHelperAddr[h], a0, a1, a2, a3, t0, t1, 0.0f, 0.0f, nullptr, g_origHelper[h]);
        }

        inline bool taskExists(const NativeEnv &env, uint32_t task)
        {
            if (task == 0u)
                return false;
            const uint32_t heads[3] = {r32(env.rdram, env.gp - 0x4C8Cu), r32(env.rdram, env.gp - 0x4C74u), r32(env.rdram, env.gp - 0x4C80u)};
            for (uint32_t list = 0u; list < 3u; ++list)
            {
                uint32_t node = heads[list];
                for (uint32_t guard = 0u; node != 0u && guard < (1u << 20); ++guard)
                {
                    if (node == task)
                        return true;
                    node = r32(env.rdram, node + 0x40u);
                }
            }
            return false;
        }

        inline int32_t gridX(const uint8_t *rdram, uint32_t actor) { return static_cast<int32_t>(r32(rdram, r32(rdram, actor + 0x3Cu) + 0x2Cu)); }
        inline int32_t gridZ(const uint8_t *rdram, uint32_t actor) { return static_cast<int32_t>(r32(rdram, r32(rdram, actor + 0x3Cu) + 0x30u)); }
        // [0x8717F4] -> +0x1E0: the player's actor object
        inline uint32_t playerActor(const uint8_t *rdram) { return r32(rdram, r32(rdram, 0x008717F4u) + 0x1E0u); }

        // Read-only: can the native path run this call? (every condition is re-derived from the disassembly above)
        uint64_t g_failReason[32] = {};
        inline bool fail(int reason) { ++g_failReason[reason]; return false; }

        bool precheck(const NativeEnv &env, uint32_t a0)
        {
            const uint8_t *rdram = env.rdram;
            if (!inRam(a0))
                return fail(1);
            const uint32_t state = r32(rdram, a0 + 0x3Cu);
            if (!inRam(state) || state + 0xB40u >= kRamSize)
                return fail(2);
            if (s8(r8(rdram, state + 0x864u)) == 0 || s8(r8(rdram, state)) != 14 || s8(r8(rdram, state + 0x865u)) != 0)
                return fail(3);
            if ((r16(rdram, 0x007E0988u) & 0x10u) != 0u)
                return fail(4);
            const int32_t b28 = s8(r8(rdram, state + 0xB28u));
            if (b28 < 0 || b28 > 12)
                return fail(5);
            const uint32_t g1 = r32(rdram, 0x008717E8u), g2 = r32(rdram, 0x008717F4u);
            if (!inRam(g2) || !inRam(r32(rdram, g2 + 0x1E0u)))
                return fail(6);
            if (!inRam(r32(rdram, r32(rdram, g2 + 0x1E0u) + 0x3Cu)))
                return fail(7);
            for (uint32_t i = 0u; i < 256u; ++i)
            {
                const uint32_t task = r32(rdram, state + 0x5Cu + i * 8u);
                if (task != 0u && (!inRam(task) || !inRam(r32(rdram, task + 0x3Cu)) || r32(rdram, task + 0x3Cu) + 0x150u >= kRamSize))
                    return fail(8);
            }
            for (uint32_t i = 0u; i < 4u; ++i)
            {
                const uint32_t p = r32(rdram, state + 0x4Cu + 4u * i);
                if (p != 0u && (!inRam(p) || p + 0x20u >= kRamSize))
                    return fail(9);
            }
            // party members, [state+0x898], [state+0xB1C]: the original dereferences them without a test
            static const uint32_t kParty[3][2] = {{0x008719A8u, 0x008719B4u}, {0x00871B68u, 0x00871B74u}, {0x00871D28u, 0x00871D34u}};
            for (const auto &pair : kParty)
            {
                if (r32(rdram, pair[0]) != 0u && r32(rdram, pair[1]) != 0u)
                {
                    const uint32_t actor = r32(rdram, r32(rdram, pair[1]) + 0x1E0u);
                    if (!inRam(r32(rdram, pair[1])) || !inRam(actor) || !inRam(r32(rdram, actor + 0x3Cu)))
                        return fail(10);
                }
            }
            if (r32(rdram, 0x008717E8u) != 0u && r32(rdram, 0x008717F4u) != 0u)
            {
                const uint32_t marker = r32(rdram, state + 0x898u);
                if (!inRam(marker) || !inRam(r32(rdram, marker + 0x3Cu)) || r32(rdram, marker + 0x3Cu) + 0x150u >= kRamSize)
                    return fail(11);
            }
            // the stair-cell block (flag + cell type 8) is not implemented: bail out when the window has such a cell
            const uint32_t player = playerActor(rdram);
            const int32_t px = gridX(rdram, player), pz = gridZ(rdram, player);
            // value of byte gp-0x462C once the 0x428F34 block has run (it is only set there, 0 -> 1)
            int32_t flag = s8(r8(rdram, env.gp - 0x462Cu));
            if (flag == 0 && g1 != 0u && g2 != 0u)
            {
                const uint32_t pcell = kFieldBase + (static_cast<uint32_t>(pz) << 8) + (static_cast<uint32_t>(px) << 4);
                if ((s8(r8(rdram, state + 0xB0Bu)) == px && s8(r8(rdram, state + 0xB0Cu)) == pz) || r8(rdram, pcell + 0x4Au) == 8u)
                    flag = 1;
            }
            if (flag != 1)
                return true;
            for (int32_t y = 0; y < b28 + 4; ++y)
                for (int32_t x = 0; x < b28 + 4; ++x)
                {
                    const int32_t cx = px - b28 / 2 + x - 2, cz = pz - b28 / 2 + y - 2;
                    if (cx < 0 || cz < 0 || cx >= 16 || cz >= 16)
                        continue;
                    const uint32_t cell = kFieldBase + (static_cast<uint32_t>(cz) << 8) + (static_cast<uint32_t>(cx) << 4);
                    if (r8(rdram, cell + 0x4Au) == 8u && r8(rdram, cell + 0x49u) == 1u)
                        return fail(12);
                }
            return true;
        }


        // 0x42A550 / 0x42CD80 / 0x429EF0 / 0x429FE0 are run as guest functions; the rest is in place.
        void nativeMain(NativeEnv &env, uint32_t a0)
        {
            uint8_t *rdram = env.rdram;
            const uint32_t T = r32(rdram, a0 + 0x3Cu);
            uint64_t sg = segNow();
            w8(rdram, T + 0x938u, 0u);
            w8(rdram, T + 0x939u, 0u);
            w8(rdram, T + 0x93Au, 0u);
            for (uint32_t i = 0u; i < 4u; ++i) // 0x428354
            {
                const uint32_t p = r32(rdram, T + 0x4Cu + 4u * i);
                if (p)
                    w8(rdram, p + 0x18u, 0u);
            }
            for (uint32_t y = 0u; y < 16u; ++y) // 0x428DE8: hide every live tile
                for (uint32_t x = 0u; x < 16u; ++x)
                {
                    const uint32_t task = r32(rdram, T + (y << 7) + (x << 3) + 0x5Cu);
                    if (taskExists(env, task))
                        w8(rdram, r32(rdram, task + 0x3Cu) + 0x150u, 1u);
                }
            if (g_timeEval) { const uint64_t n = nowNs(); g_seg[0] += n - sg; sg = n; }
            if (r32(rdram, 0x008717E8u) != 0u && r32(rdram, 0x008717F4u) != 0u) // 0x428E58
            {
                callGuest(env, 0x00423FE0u, env.spMain + 0x148u, 0u, 0x7Eu, 0x7Eu, 4u, 4u, 0.0f, 0.0f);
                wf(rdram, T + 0x89Cu, ps2_fadd(rf(rdram, T + 0x85Cu), rf(rdram, env.spMain + 0x148u)));
                wf(rdram, T + 0x8A0u, ps2_fadd(rf(rdram, T + 0x860u), rf(rdram, env.spMain + 0x14Cu)));
                {
                    const uint32_t marker = r32(rdram, T + 0x898u); // 0x4561D0
                    const uint32_t block = r32(rdram, marker + 0x3Cu);
                    w32(rdram, block + 0x120u, r32(rdram, T + 0x89Cu));
                    w32(rdram, block + 0x124u, r32(rdram, T + 0x8A0u));
                }
                w8(rdram, r32(rdram, r32(rdram, T + 0x898u) + 0x3Cu) + 0x148u, 1u); // 0x456400(marker, 1)
                {
                    const uint32_t player = playerActor(rdram);
                    callGuest(env, 0x0042A550u, a0, 0u, static_cast<uint32_t>(s8(static_cast<uint32_t>(gridX(rdram, player)))),
                              static_cast<uint32_t>(s8(static_cast<uint32_t>(gridZ(rdram, player)))), 0u, 0u, 0.0f, 0.0f);
                }
                if (s8(r8(rdram, env.gp - 0x462Cu)) == 0)
                {
                    const uint32_t player = playerActor(rdram);
                    bool set = false;
                    if (s8(r8(rdram, T + 0xB0Bu)) == gridX(rdram, player) && s8(r8(rdram, T + 0xB0Cu)) == gridZ(rdram, player))
                        set = true;
                    else
                    {
                        const uint32_t cell = kFieldBase + (static_cast<uint32_t>(gridZ(rdram, playerActor(rdram))) << 8) +
                                              (static_cast<uint32_t>(gridX(rdram, playerActor(rdram))) << 4);
                        set = r8(rdram, cell + 0x4Au) == 8u;
                    }
                    if (set)
                    {
                        w8(rdram, env.gp - 0x462Cu, 1u);
                        w8(rdram, env.gp - 0x4618u, 1u);
                    }
                }
                if (s8(r8(rdram, env.gp - 0x4628u)) == 0)
                {
                    const uint32_t player = playerActor(rdram);
                    if (s8(r8(rdram, T + 0xB08u)) == gridX(rdram, player) && s8(r8(rdram, T + 0xB09u)) == gridZ(rdram, player))
                        w8(rdram, env.gp - 0x4628u, 1u);
                }
            }
            static const uint32_t kParty[3][2] = {{0x008719A8u, 0x008719B4u}, {0x00871B68u, 0x00871B74u}, {0x00871D28u, 0x00871D34u}};
            for (uint32_t k = 0u; k < 3u; ++k)
                if (r32(rdram, kParty[k][0]) != 0u && r32(rdram, kParty[k][1]) != 0u)
                {
                    const uint32_t actor = r32(rdram, r32(rdram, kParty[k][1]) + 0x1E0u);
                    callGuest(env, 0x0042A550u, a0, k + 1u, static_cast<uint32_t>(s8(static_cast<uint32_t>(gridX(rdram, actor)))),
                              static_cast<uint32_t>(s8(static_cast<uint32_t>(gridZ(rdram, actor)))), 0u, 0u, 0.0f, 0.0f);
                }

            if (g_timeEval) { const uint64_t n = nowNs(); g_seg[1] += n - sg; sg = n; }
            // the window: (B28 + 4)^2 cells around the player
            for (int32_t s3 = 0; s3 < s8(r8(rdram, T + 0xB28u)) + 4; ++s3)
                for (int32_t s2 = 0; s2 < s8(r8(rdram, T + 0xB28u)) + 4; ++s2)
                {
                    const uint32_t player = playerActor(rdram);
                    const int32_t b28 = s8(r8(rdram, T + 0xB28u));
                    const int32_t s0 = gridX(rdram, player) - b28 / 2 + s2 - 2;
                    const int32_t s5 = gridZ(rdram, playerActor(rdram)) - b28 / 2 + s3 - 2;
                    if (s0 < 0 || s5 < 0 || s0 >= 16 || s5 >= 16)
                        continue;
                    ++g_c.cellsTotal;
                    const uint32_t cell = kFieldBase + (static_cast<uint32_t>(s5) << 8) + (static_cast<uint32_t>(s0) << 4);
                    const uint32_t slot = T + ((static_cast<uint32_t>(s5) * 16u + static_cast<uint32_t>(s0)) << 3) + 0x5Cu;
                    if (r8(rdram, cell + 0x48u) == 1u && r8(rdram, cell + 0x49u) == 1u && taskExists(env, r32(rdram, slot)))
                    {
                        ++g_c.tileCells;
                        const uint64_t tt = segNow();
                        const uint32_t tile = r32(rdram, slot);
                        const uint32_t block = r32(rdram, tile + 0x3Cu);
                        w8(rdram, block + 0x150u, 0u);
                        const bool shadeA = s8(r8(rdram, 0x0095C110u + static_cast<uint32_t>(s0) + (static_cast<uint32_t>(s5) << 4))) == 1;
                        for (uint32_t i = 0u; i < 4u; ++i)
                        {
                            w8(rdram, block + i * 4u + 0x110u, 0xFFu);
                            w8(rdram, block + i * 4u + 0x112u, 0xFFu);
                            w8(rdram, block + i * 4u + 0x111u, 0xFFu);
                            w8(rdram, block + i * 4u + 0x113u, shadeA ? 0xFFu : 0u);
                        }
                        const int32_t s6 = s8(r8(rdram, T + 0xB29u));
                        float r0 = 0.0f, r1 = 0.0f;
                        callGuest(env, 0x00429EF0u, 0u, 0u, 0u, 0u, 0u, 0u, FPU_CVT_S_W(s6), 0.0f, &r0);
                        float fx = ps2_fadd(rf(rdram, T + 0x85Cu), FPU_CVT_S_W(s6 * s2));
                        fx = ps2_fadd(fx, r0);
                        fx = ps2_fsub(fx, FPU_CVT_S_W(s6 << 1));
                        const int32_t s6b = s8(r8(rdram, T + 0xB29u));
                        callGuest(env, 0x00429FE0u, 0u, 0u, 0u, 0u, 0u, 0u, FPU_CVT_S_W(s6), 0.0f, &r1);
                        float fz = ps2_fadd(rf(rdram, T + 0x860u), FPU_CVT_S_W(s6b * s3));
                        fz = ps2_fadd(fz, r1);
                        fz = ps2_fsub(fz, FPU_CVT_S_W(s6b << 1));
                        const uint32_t block2 = r32(rdram, r32(rdram, slot) + 0x3Cu);
                        wf(rdram, block2 + 0x134u, fx);
                        wf(rdram, block2 + 0x138u, fz);
                        if (g_timeEval) g_seg[6] += nowNs() - tt;
                    }
                    // the five helpers (0x42C0A0 for party members 1..3)
                    const uint32_t xs = static_cast<uint32_t>(s0), zs = static_cast<uint32_t>(s5);
                    for (uint32_t k = 1u; k <= 3u; ++k)
                    {
                        if (!g_cache.valid)
                            buildCache(rdram);
                        const bool hit = g_cache.party[k - 1u].hit(s0, s5);
                        if (g_mainMode == MainMode::Verify)
                        {
                            Calls calls;
                            if (hit != (evaluate(H_42C0A0, rdram, k, xs, zs, calls) != Verdict::NoMatch))
                                ++g_c.cacheBad;
                        }
                        if (hit)
                        {
                            ++g_c.helperRun;
                            const uint64_t th = segNow();
                            callGuestHelper(env, H_42C0A0, a0, k, xs, zs, static_cast<uint32_t>(s2), static_cast<uint32_t>(s3));
                            if (g_timeEval) g_seg[5] += nowNs() - th;
                        }
                    }
                    for (uint32_t h = H_42CFC0; h < H_COUNT; ++h)
                    {
                        if (!g_cache.valid)
                            buildCache(rdram);
                        const bool hit = g_cache.set[h].hit(s0, s5);
                        if (g_mainMode == MainMode::Verify)
                        {
                            Calls calls;
                            if (hit != (evaluate(static_cast<Helper>(h), rdram, xs, zs, static_cast<uint32_t>(s2), calls) != Verdict::NoMatch))
                                ++g_c.cacheBad;
                        }
                        if (hit)
                        {
                            ++g_c.helperRun;
                            const uint64_t th = segNow();
                            callGuestHelper(env, static_cast<Helper>(h), a0, xs, zs, static_cast<uint32_t>(s2), static_cast<uint32_t>(s3), 0u);
                            if (g_timeEval) g_seg[5] += nowNs() - th;
                        }
                    }
                }
            if (g_timeEval) { const uint64_t n = nowNs(); g_seg[2] += n - sg; sg = n; }
            callGuest(env, 0x0042CD80u, a0, 0u, 0u, 0u, 0u, 0u, 0.0f, 0.0f);
            if (g_timeEval) { g_seg[3] += nowNs() - sg; }
        }

        void maybeLog();

        // Whole-function verify: the native path runs first on a snapshot, RAM is then restored and the original runs;
        // what the original leaves is what the game continues with.
        std::vector<uint8_t> g_snapStart, g_snapNative;

        // debugging aid (P3FES_YMISC_TRACE=1): entry trace of the allocators/effect creators during verify
        struct TraceEntry { uint32_t pc, ra, a0, a1, a2, cursor; };
        std::vector<TraceEntry> g_trace;
        bool g_traceOn = false;
        GuestFunction g_origTrace[4] = {};
        template <size_t I>
        void traceHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static constexpr uint32_t pcs[4] = {0x1158B0u, 0x1127D0u, 0x115980u, 0x429FE0u};
            if (g_traceOn)
                g_trace.push_back({pcs[I], getRegU32(ctx, 31), getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), r32(rdram, 0x7CE9D0u)});
            g_origTrace[I](rdram, ctx, runtime);
        }
        void dumpTrace(const char *tag)
        {
            std::fprintf(stderr, "[ymisc] trace %s (%u):", tag, static_cast<unsigned>(g_trace.size()));
            for (const auto &e : g_trace)
                std::fprintf(stderr, " [%X ra=%X a0=%X a1=%X a2=%X cur=%X]", e.pc, e.ra, e.a0, e.a1, e.a2, e.cursor);
            std::fprintf(stderr, "\n");
        }

        void verifyMainCall(NativeEnv &env, uint32_t a0, uint32_t ra)
        {
            uint8_t *rdram = env.rdram;
            R5900Context *ctx = env.ctx;
            g_snapStart.resize(kRamSize);
            g_snapNative.resize(kRamSize);
            std::memcpy(g_snapStart.data(), rdram, kRamSize);
            uint8_t *spr = ps2GetScratchpadHostPtr(); // the scratchpad is allocator state too (cursors in RAM point into it)
            uint8_t sprStart[16384], sprNative[16384];
            if (spr)
                std::memcpy(sprStart, spr, sizeof(sprStart));
            const R5900Context saved = *ctx;
            if (g_traceEnabled && g_c.mainVerified == 0u)
                std::fprintf(stderr, "[ymisc] fnptrs 960090=%X 960184=%X\n", r32(rdram, 0x960090u), r32(rdram, 0x960184u));
            g_trace.clear();
            g_traceOn = g_traceEnabled;
            nativeMain(env, a0);
            g_traceOn = false;
            std::vector<TraceEntry> nativeTrace = g_trace;
            g_trace.clear();
            const bool nativeFailed = env.failed;
            std::memcpy(g_snapNative.data(), rdram, kRamSize);
            if (spr)
            {
                std::memcpy(sprNative, spr, sizeof(sprNative));
                std::memcpy(spr, sprStart, sizeof(sprStart));
            }
            std::memcpy(rdram, g_snapStart.data(), kRamSize);
            *ctx = saved;
            g_traceOn = g_traceEnabled;
            g_origMain(rdram, ctx, env.runtime);
            g_traceOn = false;
            if (ctx->pc != ra)
            {
                ++g_c.mainVerifyUnwound;
                return;
            }
            ++g_c.mainVerified;
            bool regBad = nativeFailed || getRegU32(ctx, 2) != 0u;
            static const int kSaved[] = {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31};
            for (int reg : kSaved)
                regBad |= getRegU32(ctx, reg) != getRegU32(&saved, reg);
            if (regBad)
                ++g_c.mainVerifyRegBad;
            const uint32_t sp = getRegU32(&saved, 29);
            const uint32_t lowGuard = (sp - 0x2000u) & PS2_RAM_MASK, highGuard = (sp + 0x10u) & PS2_RAM_MASK;
            uint32_t bad = 0u;
            const uint8_t *after = rdram;
            const uint8_t *native = g_snapNative.data();
            for (uint32_t base = 0u; base < kRamSize; base += 4096u)
            {
                if (std::memcmp(native + base, after + base, 4096u) == 0)
                    continue;
                for (uint32_t i = base; i < base + 4096u; i += 4u)
                {
                    if (i >= lowGuard && i < highGuard)
                        continue;
                    if (std::memcmp(native + i, after + i, 4u) != 0)
                    {
                        if (bad < 8u)
                            std::fprintf(stderr, "[ymisc] MAIN MEM MISMATCH at %08X: native %08X original %08X (start %08X)\n", i,
                                         r32(native, i), r32(after, i), r32(g_snapStart.data(), i));
                        ++bad;
                    }
                }
            }
            if (spr && std::memcmp(sprNative, spr, sizeof(sprNative)) != 0)
            {
                ++bad;
                std::fprintf(stderr, "[ymisc] MAIN SCRATCHPAD MISMATCH\n");
            }
            if (bad)
            {
                ++g_c.mainVerifyBad;
                std::fprintf(stderr, "[ymisc] MAIN MISMATCH: %u words differ\n", bad);
                if (g_traceEnabled && g_c.mainVerifyBad < 4u)
                {
                    std::vector<TraceEntry> origTrace = g_trace;
                    g_trace = nativeTrace;
                    dumpTrace("native");
                    g_trace = origTrace;
                    dumpTrace("original");
                }
            }
        }

        void mainHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            const uint64_t t0 = g_timeMain ? nowNs() : 0u;
            NativeEnv env{rdram, runtime, ctx, getRegU32(ctx, 28), getRegU32(ctx, 29) - 0x180u};
            const uint32_t a0 = getRegU32(ctx, 4);
            bool done = false;
            if (g_mainMode != MainMode::Off)
            {
                const uint64_t tp = segNow();
                const bool pre = precheck(env, a0);
                if (g_timeEval) g_seg[4] += nowNs() - tp;
                if (pre)
                {
                    if (g_mainMode == MainMode::Native)
                    {
                        nativeMain(env, a0);
                        ++g_c.mainNative;
                        if (env.failed)
                            ++g_c.mainFailed;
                        setReturnS32(ctx, 0);
                        returnToCaller(ctx);
                        done = true;
                    }
                    else
                    {
                        ++g_verifyCounter;
                        if (g_c.mainVerified < 40u || g_verifyCounter % g_verifyMainEvery == 0u)
                        {
                            verifyMainCall(env, a0, ra);
                            done = true;
                        }
                        else
                            ++g_c.mainVerifySkipped;
                    }
                }
                else
                    ++g_c.mainFallback;
            }
            if (!done)
                g_origMain(rdram, ctx, runtime);
            if (g_timeMain)
            {
                const uint64_t dt = nowNs() - t0;
                if (ctx->pc != ra)
                    ++g_mainUnwound;
                else
                {
                    ++g_mainCalls;
                    g_mainNs += dt;
                    if (dt > g_mainMaxNs)
                        g_mainMaxNs = dt;
                }
            }
            maybeLog();
        }

        // ---- hooks ------------------------------------------------------------------------------------------

        void maybeLog()
        {
            if (!g_diag)
                return;
            static uint64_t last = 0u;
            const uint64_t now = nowNs();
            if (now - last < 5000000000ull)
                return;
            last = now;
            logCounters();
        }

        template <Helper H>
        void helperHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_c.calls[H];
            if (g_mode == Mode::On)
            {
                Calls calls;
                const uint64_t t0 = g_timeEval ? nowNs() : 0u;
                const Verdict verdict = evaluate(H, rdram, ctx, calls);
                if (g_timeEval)
                    g_c.ns[H] += nowNs() - t0;
                if (verdict == Verdict::NoMatch)
                {
                    ++g_c.fast[H];
                    returnToCaller(ctx);
                    return;
                }
                ++g_c.orig[H];
                if (verdict == Verdict::Unsure)
                    ++g_c.unsure[H];
                g_origHelper[H](rdram, ctx, runtime);
            }
            else if (g_mode == Mode::Verify)
            {
                verifyHelper(H, rdram, ctx, runtime);
            }
            else
            {
                const uint64_t t0 = nowNs();
                g_origHelper[H](rdram, ctx, runtime);
                g_c.ns[H] += nowNs() - t0;
                ++g_c.orig[H];
            }
            maybeLog();
        }

        bool installOne(PS2Runtime &runtime, uint32_t address, GuestFunction hook, GuestFunction &original)
        {
            if (!runtime.hasFunction(address))
                return false;
            GuestFunction current = runtime.lookupFunction(address);
            if (!current || current == hook)
                return false;
            original = current;
            return runtime.replaceFunction(address, hook);
        }

        template <size_t... I>
        bool installProbes(PS2Runtime &runtime, std::index_sequence<I...>)
        {
            static constexpr uint32_t addrs[5] = {kXGrid, kZGrid, kGrid120, kGrid170, kCopyPos};
            return (installOne(runtime, addrs[I], &probeHook<I>, g_origProbe[I]) && ...);
        }
    }

    void logCounters()
    {
        p3fes_native_leaf::logCounters();
        if (g_mainCalls)
            std::fprintf(stderr, "[p3fes-ymisc] main calls=%llu unwound=%llu avg=%.1fus max=%.1fus\n", static_cast<unsigned long long>(g_mainCalls),
                         static_cast<unsigned long long>(g_mainUnwound), static_cast<double>(g_mainNs) / 1e3 / static_cast<double>(g_mainCalls),
                         static_cast<double>(g_mainMaxNs) / 1e3);
        if (g_timeEval)
            std::fprintf(stderr, "[p3fes-ymisc] segments ms: loopB=%.1f S2=%.1f window=%.1f 42CD80=%.1f precheck=%.1f helperRuns=%.1f tileCells=%.1f\n", g_seg[0] / 1e6, g_seg[1] / 1e6,
                         g_seg[2] / 1e6, g_seg[3] / 1e6, g_seg[4] / 1e6, g_seg[5] / 1e6, g_seg[6] / 1e6);
        if (g_mainMode != MainMode::Off)
            std::fprintf(stderr,
                         "[p3fes-ymisc] main native=%llu fallback=%llu | window cells=%llu tile cells=%llu helperRun=%llu | verify=%llu bad=%llu regbad=%llu unwound=%llu skipped=%llu cachebad=%llu failed=%llu\n",
                         static_cast<unsigned long long>(g_c.mainNative), static_cast<unsigned long long>(g_c.mainFallback),
                         static_cast<unsigned long long>(g_c.cellsTotal), static_cast<unsigned long long>(g_c.tileCells),
                         static_cast<unsigned long long>(g_c.helperRun), static_cast<unsigned long long>(g_c.mainVerified),
                         static_cast<unsigned long long>(g_c.mainVerifyBad), static_cast<unsigned long long>(g_c.mainVerifyRegBad),
                         static_cast<unsigned long long>(g_c.mainVerifyUnwound), static_cast<unsigned long long>(g_c.mainVerifySkipped),
                         static_cast<unsigned long long>(g_c.cacheBad), static_cast<unsigned long long>(g_c.mainFailed));
        if (g_mainMode != MainMode::Off)
        {
            std::fprintf(stderr, "[p3fes-ymisc] precheck fail reasons:");
            for (int i = 1; i < 32; ++i)
                if (g_failReason[i])
                    std::fprintf(stderr, " #%d=%llu", i, static_cast<unsigned long long>(g_failReason[i]));
            std::fprintf(stderr, "\n");
        }
        if (g_mode == Mode::Off)
            return;
        std::fprintf(stderr, "[p3fes-ymisc] mode=%s", g_mode == Mode::On ? "on" : (g_mode == Mode::Verify ? "verify" : "log"));
        for (uint32_t h = 0u; h < H_COUNT; ++h)
            std::fprintf(stderr, " | %s calls=%llu fast=%llu orig=%llu unsure=%llu", kHelperName[h],
                         static_cast<unsigned long long>(g_c.calls[h]), static_cast<unsigned long long>(g_c.fast[h]),
                         static_cast<unsigned long long>(g_c.orig[h]), static_cast<unsigned long long>(g_c.unsure[h]));
        std::fprintf(stderr, "\n");
        if (g_mode == Mode::Verify)
        {
            for (uint32_t h = 0u; h < H_COUNT; ++h)
                std::fprintf(stderr, "[p3fes-ymisc] verify %s path=%llu bad=%llu | ram=%llu bad=%llu | regbad=%llu unwound=%llu\n", kHelperName[h],
                             static_cast<unsigned long long>(g_c.verifyPath[h]), static_cast<unsigned long long>(g_c.verifyPathBad[h]),
                             static_cast<unsigned long long>(g_c.verifyMem[h]), static_cast<unsigned long long>(g_c.verifyMemBad[h]),
                             static_cast<unsigned long long>(g_c.verifyRegBad[h]), static_cast<unsigned long long>(g_c.verifyUnwound[h]));
        }
        if (g_mode == Mode::Log || (g_mode == Mode::On && g_timeEval))
        {
            std::fprintf(stderr, "[p3fes-ymisc] original ms:");
            for (uint32_t h = 0u; h < H_COUNT; ++h)
                std::fprintf(stderr, " %s=%.1f", kHelperName[h], static_cast<double>(g_c.ns[h]) / 1e6);
            std::fprintf(stderr, "\n");
        }
    }

    bool install(PS2Runtime &runtime)
    {
        // measurement wrappers (P3FES_FN_PROF) go on last, over every hook this function installs
        struct FnProfGuard
        {
            PS2Runtime &rt;
            ~FnProfGuard() { p3fes_native_fnprof::install(rt); }
        } fnProfGuard{runtime};
        p3fes_native_leaf::install(runtime);
        p3fes_native_effects::install(runtime); // TEMPORARY unity build: p3fes_native_effects.cpp is included at the end of this file
        const char *value = std::getenv("P3FES_NATIVE_YMISC");
        const char *diag = std::getenv("P3FES_RESOURCE_DIAG");
        g_diag = diag && *diag == '1';
        g_c = {};
        g_mode = Mode::On;
        g_mainMode = MainMode::Native;
        if (const char *every = std::getenv("P3FES_YMISC_VERIFY_EVERY"); every && *every)
        {
            const uint32_t n = static_cast<uint32_t>(std::strtoul(every, nullptr, 10));
            if (n)
            {
                g_verifyEvery = n;
                g_verifyMainEvery = n;
            }
        }
        if (value && *value)
        {
            if (std::strcmp(value, "0") == 0)
                g_mode = Mode::Off, g_mainMode = MainMode::Off;
            else if (std::strcmp(value, "helpers") == 0)
                g_mainMode = MainMode::Off;
            else if (std::strcmp(value, "verify") == 0)
                g_mode = Mode::Log, g_mainMode = MainMode::Verify; // helpers pass through; the whole function is verified
            else if (std::strcmp(value, "verify-helpers") == 0)
                g_mode = Mode::Verify, g_mainMode = MainMode::Off;
            else if (std::strcmp(value, "log") == 0)
                g_mode = Mode::Log, g_mainMode = MainMode::Off;
        }
        if (const char *tr = std::getenv("P3FES_YMISC_TRACE"); tr && *tr == '1')
        {
            g_traceEnabled = true;
            installOne(runtime, 0x1158B0u, &traceHook<0>, g_origTrace[0]);
            installOne(runtime, 0x1127D0u, &traceHook<1>, g_origTrace[1]);
            installOne(runtime, 0x115980u, &traceHook<2>, g_origTrace[2]);
            installOne(runtime, 0x429FE0u, &traceHook<3>, g_origTrace[3]);
        }
        const char *timing = std::getenv("P3FES_YMISC_TIME");
        g_timeMain = timing && *timing == '1';
        g_timeEval = timing && *timing == '2';
        g_timeMain = g_timeMain || g_timeEval;
        if (g_mode == Mode::Off && g_mainMode == MainMode::Off)
        {
            std::fprintf(stderr, "[p3fes-native] ymisc cut off\n");
            if (g_timeMain)
                installOne(runtime, kMainFn, &mainHook, g_origMain);
            return false;
        }
        if (!runtime.hasFunction(kMainFn))
            return false;
        bool ok = true;
        if (g_mode == Mode::Verify)
            ok = installProbes(runtime, std::make_index_sequence<5>{});
        ok = ok && installOne(runtime, kHelperAddr[H_42C0A0], &helperHook<H_42C0A0>, g_origHelper[H_42C0A0]) &&
             installOne(runtime, kHelperAddr[H_42CFC0], &helperHook<H_42CFC0>, g_origHelper[H_42CFC0]) &&
             installOne(runtime, kHelperAddr[H_42DD90], &helperHook<H_42DD90>, g_origHelper[H_42DD90]) &&
             installOne(runtime, kHelperAddr[H_42EA60], &helperHook<H_42EA60>, g_origHelper[H_42EA60]) &&
             installOne(runtime, kHelperAddr[H_42F440], &helperHook<H_42F440>, g_origHelper[H_42F440]);
        if (g_mainMode != MainMode::Off || g_timeMain)
            ok = installOne(runtime, kMainFn, &mainHook, g_origMain) && ok;
        if (!ok)
            g_mainMode = MainMode::Off;
        std::fprintf(stderr, "[p3fes-native] ymisc cut hooks=%u helpers=%s main=%s\n", ok ? 1u : 0u,
                     g_mode == Mode::On ? "on" : (g_mode == Mode::Verify ? "verify" : "log"),
                     g_mainMode == MainMode::Native ? "native" : (g_mainMode == MainMode::Verify ? "verify" : "off"));
        return ok;
    }
}
