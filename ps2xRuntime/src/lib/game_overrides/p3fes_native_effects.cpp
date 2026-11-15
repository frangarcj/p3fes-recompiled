#include "p3fes_native_effects.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

// mdlEffect_0032af30 (0x32AF30..0x32B990): the per-frame update of one particle emitter of effect kind 0. Its callers reach
// it through the effect table at 0x69C520 ({start, end, particle stride 0x3C, template size 0xF0}); the effect object is
//
//   E+0x04 N    particle count          E+0x0C F  flags (bit 0: local space)    E+0x10 age (frames)
//   E+0x14 f    spawn remainder (float) E+0x18 P  particles, 0x20 bytes each    E+0x1C Q  per-particle data, 0x3C each
//   E+0x20 T    template (0xF0 bytes)
//
//   particle  +0x00 position vec4, +0x10 state (age; -1 free, -2 finished for good), +0x14 RGBA word, +0x18/+0x1C floats
//   template  +0x00 origin, +0x10 orientation quaternion, +0x20 duration D, +0x24 int, +0x28 spawn rate, +0x2C colour curve,
//             +0x50 alpha curve, +0x60 / +0x8C float curves, +0x9C kind (0, 1, 2), +0xB8 lifetime s7, +0xBD burst flag,
//             +0xC0/+0xC4 trail dimensions (C0 * C4 history slots per particle, stored behind the N particles in P),
//             +0xC8.. random ranges
//
// Per call: 0x357E30 turns the orientation into a rotation (vf28..vf30, VU0 macro mode); then for every one of the N slots
// (a) a particle whose state reached the lifetime is retired (state -2 when the template has a duration, else -1, trail
// slots cleared), (b) a free slot (-1) is respawned while the spawn counter s0 lasts (the effect's rand state at 0x957BF0
// gives 9..12 floats per spawn, the order of those calls is part of the result), (c) a live particle is moved and its colour
// / alpha / size curves evaluated (0x32A770 -> 0x32A120, 0x32A540) and, with a trail, 0x32A890 shifts and interpolates the
// history slots (0x329D60 is a cubic Hermite through four of them).
//
// The native version below does exactly that with the guest registers in locals: every FPU operation uses the runtime's
// ps2_f* helpers in the order of the recompiled code, every VU0 macro operation the same PS2_V* macros, `mula/madd/msub`
// with the accumulator as sequences of those helpers. The callees 357E30, 32A770, 32A120, 32A540, 32A890, 329D60 and the
// rand functions are inlined; sinf goes through the table (the P3FES_NATIVE_MATH hook or the guest function).
// What is not natively implemented is detected before the first write and the original function runs.
//
// Cycle accounting: the original charges 32 cycles per loop back edge and 8 per direct call; the same total is charged with
// eeTryChargeCall (never enters the scheduler: the native path does not suspend).
//
// Where the original spent its instructions (console, shop, PMU): ~26 k per emitter call, of which the 40-80 slot loop of
// the emitter (every slot costs a 25-instruction block with a scheduler checkpoint, almost all slots are finished or free)
// and the ~6 live particles of an emitter (~1.1 k instructions each: ~600 float operations through ctx->f, the three curve
// evaluations) are most. The native path scans the dead slots in a tight loop, keeps everything in locals and memoizes the
// pure functions: the curve values by (template bytes, age) and the 0x357E30 rotation by quaternion.
//
// Verification: P3FES_NATIVE_EFFECTS=verify runs both versions on the same RAM (see the header); P3FES_EFFECTS_FUZZ adds
// synthetic emitters. 0x32AF30 has the shape of a table entry (0x69C520: {start, end, particle stride, template size}) and is
// reached through function pointers, so there is no direct-call fast path to lose by hooking it. The three RenderWare helpers
// further down are called directly by the generated code: hooking them sends those calls through dispatchGuestBranch.
namespace p3fes_native_effects
{
    void logCounters();

    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;

        enum class Mode : uint8_t { Off, On, Verify, Log };
        Mode g_mode = Mode::Off;

        constexpr uint32_t kEntry = 0x0032AF30u;
        constexpr uint32_t kSinf = 0x0052E878u;
        constexpr uint32_t kRandState = 0x00957BF0u;
        constexpr uint32_t kUpVector = 0x0069C4F0u;
        constexpr uint32_t kSentinel = 0xFFFFFFFCu;
        constexpr uint32_t kRamSize = PS2_RAM_SIZE;

        GuestFunction g_orig = nullptr;

        struct Counters
        {
            uint64_t calls = 0u, native = 0u, fallback = 0u, early = 0u;
            uint64_t fbRange = 0u, fbAlias = 0u, fbCount = 0u, fbType2 = 0u, fbTrail = 0u, fbResumed = 0u;
            uint64_t particles = 0u, spawns = 0u, bursts = 0u, expired = 0u, trailLow = 0u, trailHigh = 0u, trailInit = 0u;
            uint64_t typeCalls[4] = {};
            uint64_t sinGuest = 0u, guestFail = 0u, wNonZero = 0u;
            uint64_t verified = 0u, verifiedFull = 0u, verifyBad = 0u, verifyRegBad = 0u, verifyScratch = 0u, verifyUnwound = 0u;
            uint64_t verifyRoiBad = 0u, verifyWriteBad = 0u, verifySoft = 0u;
            uint64_t ns = 0u, nsCalls = 0u;
            uint64_t aliasWhy[40] = {};
            uint64_t attrHit = 0u, attrMiss = 0u, attrNoCache = 0u, basisMiss = 0u, curveEntryMiss = 0u, s7Sum = 0u, nSum = 0u;
        } g_c;

        bool g_time = false;
        bool g_diag = false;
        uint32_t g_verifyEvery = 2u, g_verifyFull = 8u;
        uint64_t g_verifyTick = 0u;

        inline uint64_t nowNs()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

#if defined(PLATFORM_VITA)
        extern "C" __attribute__((weak)) int ps2x_vita_pmu_read_fast(uint32_t *instr, uint32_t *cycles);
#endif
        // P3FES_EFFECTS_TIME=2: segment costs of the native path (instructions of the game thread from the PMU on the
        // Vita with PS2X_VITA_PMU=1, nanoseconds elsewhere); the cost of a stamp is measured and subtracted
        int g_timeMode = 0;
        inline uint64_t stamp()
        {
#if defined(PLATFORM_VITA)
            uint32_t instr = 0u, cycles = 0u;
            if (ps2x_vita_pmu_read_fast && ps2x_vita_pmu_read_fast(&instr, &cycles))
                return instr;
            return 0u;
#else
            return nowNs();
#endif
        }
        inline uint32_t delta(uint64_t a, uint64_t b)
        {
#if defined(PLATFORM_VITA)
            return static_cast<uint32_t>(static_cast<uint32_t>(b) - static_cast<uint32_t>(a));
#else
            return static_cast<uint32_t>(b - a);
#endif
        }
        enum { SEG_PRE, SEG_SETUP, SEG_LOOP, SEG_ALIVE, SEG_SPAWN, SEG_POST, SEG_TOTAL, SEG_BASIS, SEG_CURVE, SEG_SIN, SEG_COUNT };
        uint64_t g_seg[SEG_COUNT] = {};
        uint64_t g_segN[SEG_COUNT] = {};
        uint64_t g_stampCost = 0u;
        // per-call cost of the whole hook: [0] original code, [1] native path; index 0 particles, 1 forAllAtomics, 2 scale, 3 translate
        uint64_t g_hu[2][6] = {};
        uint64_t g_hn[2][6] = {};
        template <bool kOn> struct SegTimerT
        {
            explicit SegTimerT(int) {}
        };
        template <> struct SegTimerT<true>
        {
            uint64_t t0 = 0u;
            int seg;
            explicit SegTimerT(int s) : t0(stamp()), seg(s) {}
            ~SegTimerT()
            {
                g_seg[seg] += delta(t0, stamp());
                ++g_segN[seg];
            }
        };
        using SegTimer = SegTimerT<true>;

        // ---- guest memory (all addresses here are plain RAM, checked by the precheck) -----------------------------

        // Verify mode journals every native write so the whole run can be rolled back.
        struct Journal
        {
            struct Entry { uint32_t addr; uint32_t len; uint8_t old[16]; };
            std::vector<Entry> entries;
            void clear() { entries.clear(); }
        };

        // Addresses are used as they are (no wrap, no scratchpad): every caller checks its ranges with rangeOk() first.
        struct Mem
        {
            uint8_t *ram;
            Journal *jr = nullptr;

            inline uint32_t r32(uint32_t a) const
            {
                uint32_t v;
                std::memcpy(&v, ram + a, 4u);
                return v;
            }
            inline int32_t ri32(uint32_t a) const { return static_cast<int32_t>(r32(a)); }
            inline uint8_t r8(uint32_t a) const { return ram[a]; }
            inline float rf(uint32_t a) const
            {
                float v;
                std::memcpy(&v, ram + a, 4u);
                return v;
            }
            inline __m128 ld4(uint32_t a) const // lq / lqc2: the low four address bits are ignored
            {
                __m128 v;
                std::memcpy(&v, ram + (a & ~0xFu), 16u);
                return v;
            }
            __attribute__((noinline)) void noteSlow(uint32_t a, uint32_t len)
            {
                Journal::Entry e;
                e.addr = a;
                e.len = len;
                std::memcpy(e.old, ram + e.addr, len);
                jr->entries.push_back(e);
            }
            inline void note(uint32_t a, uint32_t len)
            {
                if (__builtin_expect(jr != nullptr, 0))
                    noteSlow(a, len);
            }
            inline void w32(uint32_t a, uint32_t v)
            {
                note(a, 4u);
                std::memcpy(ram + a, &v, 4u);
            }
            inline void w8(uint32_t a, uint8_t v)
            {
                note(a, 1u);
                ram[a] = v;
            }
            inline void wf(uint32_t a, float v)
            {
                note(a, 4u);
                std::memcpy(ram + a, &v, 4u);
            }
            inline void st4(uint32_t a, __m128 v) // sq / sqc2
            {
                a &= ~0xFu;
                note(a, 16u);
                std::memcpy(ram + a, &v, 16u);
            }
            // memcpy of one 0x20 byte slot (the regions never overlap: the slots are 0x20 apart at least)
            inline void copy32(uint32_t dst, uint32_t src)
            {
                note(dst, 16u);
                note(dst + 16u, 16u);
                std::memmove(ram + dst, ram + src, 32u);
            }
        };

        inline uint32_t fbits(float f)
        {
            uint32_t b;
            std::memcpy(&b, &f, 4u);
            return b;
        }
        inline float bitsf(uint32_t b)
        {
            float f;
            std::memcpy(&f, &b, 4u);
            return f;
        }
        inline float cvtsw(int32_t v) { return FPU_CVT_S_W(v); }
        inline int32_t cvtws(float f) { return FPU_CVT_W_S(f); }
        // mtc1 + cvt.s.w for a register that may hold an unsigned value: (v >> 1 | v & 1) converted and doubled
        inline float u32ToFloat(uint32_t v)
        {
            if (static_cast<int32_t>(v) < 0)
            {
                const float h = FPU_CVT_S_W(static_cast<int32_t>((v >> 1) | (v & 1u)));
                return FPU_ADD_S(h, h);
            }
            return FPU_CVT_S_W(static_cast<int32_t>(v));
        }

        // ps2_fadd(+0.0f, x): x itself unless its exponent field is 0 (+-0 and the PS2's "denormals" give +0.0). Checked for all
        // 2^32 inputs against ps2_fadd on the Mac build; the add of zero is what `adda.s $f(0), x` / `mtc1 $zero` produce.
        inline float addZero(float x) { return (fbits(x) & 0x7F800000u) != 0u ? x : 0.0f; }

        template <int L> inline __m128 bc(__m128 v) { return _mm_shuffle_ps(v, v, _MM_SHUFFLE(L, L, L, L)); }

        // ---- environment of one native run -----------------------------------------------------------------------

        struct Env
        {
            uint8_t *rdram;
            PS2Runtime *runtime;
            R5900Context *ctx;
            Mem m;
            uint32_t gp = 0u;
            uint32_t sp = 0u;      // stack pointer at entry (the original's frame sits below it)
            uint32_t calls = 0u;   // direct calls the original would make (8 cycles each)
            uint32_t edges = 0u;   // loop back edges (32 cycles each)
            bool cSet = false, cVal = false; // last c.le.s result (FCR31 bit 23)
            bool failed = false;
            float cInv255 = 0.0f;  // [gp-0x7EA4]
            uint64_t live = 0u;    // live particles updated by this call (added to the counters afterwards)
            struct CurveEntry *curve = nullptr; // memoized curves of the template being updated (null: compute)
        };

        inline bool cLe(Env &env, float a, float b)
        {
            const bool r = FPU_C_OLE_S(a, b);
            env.cSet = true;
            env.cVal = r;
            return r;
        }

        inline bool cLt(Env &env, float a, float b)
        {
            const bool r = FPU_C_OLT_S(a, b);
            env.cSet = true;
            env.cVal = r;
            return r;
        }

        // ---- guest calls (sinf through the table, rare) ----------------------------------------------------------

        float callGuestFloat(Env &env, uint32_t entry, float f12)
        {
            R5900Context h = *env.ctx;
            h.pc = entry;
            h.f[12] = f12;
            h.r[29] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(env.sp - 0x400u)));
            h.r[31] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(kSentinel)));
            for (uint32_t resume = 0u; resume < 100000u; ++resume)
            {
                if (env.runtime->isStopRequested() || h.pc == 0u || h.pc == kSentinel || !env.runtime->hasFunction(h.pc))
                    break;
                env.runtime->lookupFunction(h.pc)(env.rdram, &h, env.runtime);
            }
            if (h.pc != kSentinel)
            {
                env.failed = true;
                ++g_c.guestFail;
                return 0.0f;
            }
            return h.f[0];
        }

        // sinf: 0 = not probed, 1 = the table holds the P3FES_NATIVE_MATH hook (host sin for ordinary arguments), 2 = guest
        int g_sinMode = 0;
        bool nativeSinArg(float value)
        {
            const uint32_t bits = fbits(value);
            const uint32_t magnitude = bits & 0x7FFFFFFFu;
            const uint32_t exponent = magnitude & 0x7F800000u;
            return magnitude <= 0x46000000u && (magnitude == 0u || (exponent != 0u && exponent != 0x7F800000u));
        }
        void probeSin(Env &env)
        {
            static const float probes[] = {0.7f, 1.9f, -3.3f, 100.5f, 0.01f, 5.5f, -0.2f};
            bool same = true;
            for (float p : probes)
            {
                const float expect = std::sin(p);
                const float got = callGuestFloat(env, kSinf, p);
                same = same && fbits(expect) == fbits(got);
            }
            env.failed = false;
            g_sinMode = same ? 1 : 2;
            std::fprintf(stderr, "[p3fes-effects] sinf at 0x%08X: %s\n", kSinf, same ? "host sin (P3FES_NATIVE_MATH hook)" : "guest function");
        }
        // separate functions: the compiler must not turn sin(x) and cos(x) of one argument into one sincos call (a different
        // library routine with other roundings than the separate sinf / cosf the P3FES_NATIVE_MATH hooks call)
        __attribute__((noinline)) float hostSinf(float x) { return std::sin(x); }
        __attribute__((noinline)) float hostCosf(float x) { return std::cos(x); }
        constexpr uint32_t kCosf = 0x0052E6D8u;
        int g_cosMode = 0;
        void probeCos(Env &env)
        {
            static const float probes[] = {0.7f, 1.9f, -3.3f, 100.5f, 0.01f, 5.5f, -0.2f};
            bool same = true;
            for (float p : probes)
                same = same && fbits(hostCosf(p)) == fbits(callGuestFloat(env, kCosf, p));
            env.failed = false;
            g_cosMode = same ? 1 : 2;
            std::fprintf(stderr, "[p3fes-effects] cosf at 0x%08X: %s\n", kCosf, same ? "host cos (P3FES_NATIVE_MATH hook)" : "guest function");
        }
        inline float psSin(Env &env, float x)
        {
            ++env.calls;
            if (g_sinMode == 0)
                probeSin(env);
            if (g_sinMode == 1 && nativeSinArg(x))
                return hostSinf(x);
            ++g_c.sinGuest;
            return callGuestFloat(env, kSinf, x);
        }

        // ---- 0x357FD0 / 0x358030: the effect random generator ----------------------------------------------------

        inline uint32_t randInt(Env &env)
        {
            ++env.calls;
            Mem &m = env.m;
            const uint32_t w0 = m.r32(kRandState), w1 = m.r32(kRandState + 4u), w2 = m.r32(kRandState + 8u), w3 = m.r32(kRandState + 12u);
            const uint32_t x = ((w1 << 2) | (w0 >> 30)) ^ ((w3 << 1) | (w2 >> 31));
            m.w32(kRandState, x);
            m.w32(kRandState + 4u, w0);
            m.w32(kRandState + 8u, w1);
            m.w32(kRandState + 12u, w2);
            return x;
        }
        // (x & 0xFFFFFF) / 2^24, exact (the original divides by 16777216.0 with ps2_fdiv)
        inline float randFloat(Env &env)
        {
            ++env.calls; // 0x358030 itself; its inner 0x357FD0 call is charged as well
            const uint32_t x = randInt(env);
            return static_cast<float>(static_cast<int32_t>(x & 0xFFFFFFu)) * 5.9604644775390625e-08f;
        }

        // ---- 0x357E30: orientation quaternion (vf10) -> rotation rows vf28..vf30 ----------------------------------

        struct Vu
        {
            __m128 vf[32];
            __m128 acc;
        };

        void basis357E30(Vu &u)
        {
            __m128 *vf = u.vf;
            __m128 &acc = u.acc;
            { __m128 res = PS2_VADD(vf[0], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(3,3,3,3))); __m128i mask = _mm_set_epi32(0, -1, -1, -1); vf[1] = _mm_blendv_ps(vf[1], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VADD(vf[10], vf[10]); __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[2] = PS2_VBLEND(vf[2], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[0], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(0,0,0,0))); __m128i mask = _mm_set_epi32(-1, 0, 0, 0); vf[28] = _mm_blendv_ps(vf[28], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[0], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(0,0,0,0))); __m128i mask = _mm_set_epi32(-1, 0, 0, 0); vf[29] = _mm_blendv_ps(vf[29], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[0], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(0,0,0,0))); __m128i mask = _mm_set_epi32(-1, 0, 0, 0); vf[30] = _mm_blendv_ps(vf[30], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[10], vf[2]); __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[3] = PS2_VBLEND(vf[3], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[10], _mm_shuffle_ps(vf[2], vf[2], _MM_SHUFFLE(1,1,1,1))); __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[4] = _mm_blendv_ps(vf[4], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[10], _mm_shuffle_ps(vf[2], vf[2], _MM_SHUFFLE(2,2,2,2))); __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[5] = _mm_blendv_ps(vf[5], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[10], _mm_shuffle_ps(vf[2], vf[2], _MM_SHUFFLE(0,0,0,0))); __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[6] = _mm_blendv_ps(vf[6], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VADD(vf[0], _mm_shuffle_ps(vf[0], vf[0], _MM_SHUFFLE(3,3,3,3))); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(1,1,1,1))); __m128 res = PS2_VSUB(acc, mul_res); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, 0, 0, -1))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(2,2,2,2))); __m128 res = PS2_VSUB(acc, mul_res); __m128i mask = _mm_set_epi32(0, 0, 0, -1); vf[28] = _mm_blendv_ps(vf[28], res, _mm_castsi128_ps(mask)); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(0,0,0,0))); __m128 res = PS2_VSUB(acc, mul_res); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, 0, -1, 0))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(2,2,2,2))); __m128 res = PS2_VSUB(acc, mul_res); __m128i mask = _mm_set_epi32(0, 0, -1, 0); vf[29] = _mm_blendv_ps(vf[29], res, _mm_castsi128_ps(mask)); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(0,0,0,0))); __m128 res = PS2_VSUB(acc, mul_res); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, 0))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[3], vf[3], _MM_SHUFFLE(1,1,1,1))); __m128 res = PS2_VSUB(acc, mul_res); __m128i mask = _mm_set_epi32(0, -1, 0, 0); vf[30] = _mm_blendv_ps(vf[30], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[4], vf[4], _MM_SHUFFLE(0,0,0,0))); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, 0, -1, 0))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[5], vf[5], _MM_SHUFFLE(3,3,3,3))); __m128 res = PS2_VSUB(acc, mul_res); __m128i mask = _mm_set_epi32(0, 0, -1, 0); vf[28] = _mm_blendv_ps(vf[28], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VADD(vf[4], _mm_shuffle_ps(vf[5], vf[5], _MM_SHUFFLE(3,3,3,3))); __m128i mask = _mm_set_epi32(0, 0, 0, -1); vf[29] = _mm_blendv_ps(vf[29], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VSUB(vf[5], _mm_shuffle_ps(vf[4], vf[4], _MM_SHUFFLE(3,3,3,3))); __m128i mask = _mm_set_epi32(0, 0, 0, -1); vf[30] = _mm_blendv_ps(vf[30], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[5], vf[5], _MM_SHUFFLE(0,0,0,0))); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, 0))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[4], vf[4], _MM_SHUFFLE(3,3,3,3))); __m128 res = PS2_VADD(acc, mul_res); __m128i mask = _mm_set_epi32(0, -1, 0, 0); vf[28] = _mm_blendv_ps(vf[28], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[5], vf[5], _MM_SHUFFLE(1,1,1,1))); acc = _mm_blendv_ps(acc, res, _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, 0))); }
            { __m128 mul_res = PS2_VMUL(vf[1], _mm_shuffle_ps(vf[6], vf[6], _MM_SHUFFLE(3,3,3,3))); __m128 res = PS2_VSUB(acc, mul_res); __m128i mask = _mm_set_epi32(0, -1, 0, 0); vf[29] = _mm_blendv_ps(vf[29], res, _mm_castsi128_ps(mask)); }
            { __m128 res = PS2_VADD(vf[5], _mm_shuffle_ps(vf[6], vf[6], _MM_SHUFFLE(3,3,3,3))); __m128i mask = _mm_set_epi32(0, 0, -1, 0); vf[30] = _mm_blendv_ps(vf[30], res, _mm_castsi128_ps(mask)); }
            { __m128i mask = _mm_set_epi32(-1, -1, -1, -1); vf[31] = _mm_blendv_ps(vf[31], vf[0], _mm_castsi128_ps(mask)); }
        }

        // vmulax / vmadday / vmaddz of the rotation rows with vf10 (0x32B2BC..0x32B2C4, 0x32B354..0x32B35C)
        inline __m128 rotate(const Vu &u, __m128 v)
        {
            const __m128 a = PS2_VMUL(u.vf[28], bc<0>(v));
            const __m128 b = PS2_VADD(a, PS2_VMUL(u.vf[29], bc<1>(v)));
            return PS2_VADD(b, PS2_VMUL(u.vf[30], bc<2>(v)));
        }

        // ---- colour helpers (pextlb/pextlh + vitof0, vftoi0 + ppach/ppacb) ---------------------------------------

        inline __m128 unpackColor(uint32_t w)
        {
            // pextlb/pextlh with $zero: the four bytes become four words; vitof0 (x * 1.0 is exact)
            return _mm_cvtepi32_ps(_mm_set_epi32(static_cast<int>((w >> 24) & 0xFFu), static_cast<int>((w >> 16) & 0xFFu),
                                                 static_cast<int>((w >> 8) & 0xFFu), static_cast<int>(w & 0xFFu)));
        }
        inline uint32_t packColor(__m128 v)
        {
            const __m128 src = _mm_mul_ps(v, _mm_set1_ps(1.0f));
            const __m128i positiveOverflow = _mm_andnot_si128(_mm_srai_epi32(_mm_castps_si128(src), 31),
                                                              _mm_castps_si128(_mm_cmpnlt_ps(src, _mm_set1_ps(2147483648.0f))));
            const __m128i r = _mm_xor_si128(_mm_cvttps_epi32(src), positiveOverflow);
            const __m128i zero = _mm_setzero_si128();
            return static_cast<uint32_t>(_mm_cvtsi128_si32(PS2_PPACB(zero, PS2_PPACH(zero, r))));
        }

        // 0x32A120(curve C, alpha block A, t, L): colour curve + alpha fade, returns the RGBA word
        uint32_t colorCurve(Env &env, uint32_t C, uint32_t A, int32_t t, int32_t L)
        {
            Mem &m = env.m;
            ++env.calls;
            if (L == 0)
                return (m.r32(C + 4u) & 0xFFFFFFu) | (m.r32(A) << 24);
            const float f0 = cvtsw(L);
            uint32_t wa, wb; // wa: colour at the start of the segment, wb: at its end
            float f3;
            switch (m.r8(C))
            {
            case 0:
                wa = m.r32(C + 4u);
                wb = m.r32(C + 8u);
                f3 = FPU_DIV_S(cvtsw(t), f0);
                break;
            case 1:
            {
                const int32_t v1 = cvtws(FPU_MUL_S(m.rf(C + 0x10u), f0));
                if (t < v1)
                {
                    wa = m.r32(C + 4u);
                    wb = m.r32(C + 0xCu);
                    f3 = FPU_DIV_S(cvtsw(t), cvtsw(v1));
                }
                else
                {
                    wa = m.r32(C + 0xCu);
                    wb = m.r32(C + 8u);
                    const float f2 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(v1)));
                    const float f1 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(v1)));
                    f3 = FPU_DIV_S(f1, f2);
                }
                break;
            }
            case 2:
            {
                const int32_t t0 = cvtws(FPU_MUL_S(m.rf(C + 0x10u), f0));
                if (t < t0)
                {
                    wa = m.r32(C + 4u);
                    wb = m.r32(C + 0xCu);
                    f3 = FPU_DIV_S(cvtsw(t), cvtsw(t0));
                }
                else
                {
                    const int32_t v1 = cvtws(FPU_MUL_S(m.rf(C + 0x18u), f0));
                    if (t < v1)
                    {
                        wa = m.r32(C + 0xCu);
                        wb = m.r32(C + 0x14u);
                        const float f2 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(v1) - static_cast<uint32_t>(t0)));
                        const float f1 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(t0)));
                        f3 = FPU_DIV_S(f1, f2);
                    }
                    else
                    {
                        wa = m.r32(C + 0x14u);
                        wb = m.r32(C + 8u);
                        const float f2 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(v1)));
                        const float f1 = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(v1)));
                        f3 = FPU_DIV_S(f1, f2);
                    }
                }
                break;
            }
            default:
                wa = m.r32(C + 4u);
                wb = m.r32(C + 8u);
                f3 = 0.0f;
                break;
            }
            // vf10 = colour(wb) * c, vf11 = vf10; vf10 = colour(wa) * c * (1 - f3), vf11 *= f3, sum, * 255, vftoi0, pack
            const __m128 cv = _mm_set1_ps(env.cInv255);
            __m128 vf11 = PS2_VMUL(unpackColor(wb), cv);
            __m128 vf10 = PS2_VMUL(unpackColor(wa), cv);
            const float f1 = FPU_SUB_S(1.0f, f3);
            vf10 = PS2_VMUL(vf10, _mm_set1_ps(f1));
            vf11 = PS2_VMUL(vf11, _mm_set1_ps(f3));
            vf10 = PS2_VADD(vf10, vf11);
            vf10 = PS2_VMUL(vf10, _mm_set1_ps(255.0f));
            const uint32_t packed = packColor(vf10);
            // alpha: [A+0] scaled by the fade-in (A+8) / fade-out (A+0xC) fractions of L
            const int32_t v0 = cvtws(FPU_MUL_S(m.rf(A + 8u), f0));
            const int32_t v1 = cvtws(FPU_MUL_S(m.rf(A + 0xCu), f0));
            float f2 = 1.0f;
            if (t < v0)
                f2 = FPU_DIV_S(cvtsw(t), cvtsw(v0));
            else if (v1 < t)
            {
                const float fa = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(t)));
                const float fb = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(v1)));
                f2 = FPU_DIV_S(fa, fb);
            }
            const float fAlpha = FPU_MUL_S(u32ToFloat(m.r32(A)), f2);
            int32_t a;
            if (cLe(env, 2147483648.0f, fAlpha))
                a = static_cast<int32_t>(static_cast<uint32_t>(cvtws(FPU_SUB_S(fAlpha, 2147483648.0f))) | 0x80000000u);
            else
                a = cvtws(fAlpha);
            return (packed & 0xFFFFFFu) | (static_cast<uint32_t>(a) << 24);
        }

        // 0x32A540(curve C, t, L)
        float floatCurve(Env &env, uint32_t C, int32_t t, int32_t L)
        {
            Mem &m = env.m;
            ++env.calls;
            if (L == 0)
                return m.rf(C + 4u);
            const float f1 = cvtsw(L);
            float f4, f3, f2;
            switch (m.r8(C))
            {
            case 0:
                f4 = m.rf(C + 4u);
                f3 = m.rf(C + 8u);
                f2 = FPU_DIV_S(cvtsw(t), f1);
                break;
            case 1:
            {
                const int32_t v1 = cvtws(FPU_MUL_S(m.rf(C + 0x18u), f1));
                if (t < v1)
                {
                    f4 = m.rf(C + 4u);
                    f3 = m.rf(C + 0x14u);
                    f2 = FPU_DIV_S(cvtsw(t), cvtsw(v1));
                }
                else
                {
                    f4 = m.rf(C + 0x14u);
                    f3 = m.rf(C + 8u);
                    const float fa = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(v1)));
                    const float fb = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(v1)));
                    f2 = FPU_DIV_S(fb, fa);
                }
                break;
            }
            case 2:
            {
                const int32_t a3 = cvtws(FPU_MUL_S(m.rf(C + 0x18u), f1));
                if (t < a3)
                {
                    f4 = m.rf(C + 4u);
                    f3 = m.rf(C + 0x14u);
                    f2 = FPU_DIV_S(cvtsw(t), cvtsw(a3));
                }
                else
                {
                    const int32_t v1 = cvtws(FPU_MUL_S(m.rf(C + 0x20u), f1));
                    if (t < v1)
                    {
                        f4 = m.rf(C + 0x14u);
                        f3 = m.rf(C + 0x1Cu);
                        const float fa = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(v1) - static_cast<uint32_t>(a3)));
                        const float fb = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(a3)));
                        f2 = FPU_DIV_S(fb, fa);
                    }
                    else
                    {
                        f4 = m.rf(C + 0x1Cu);
                        f3 = m.rf(C + 8u);
                        const float fa = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(L) - static_cast<uint32_t>(v1)));
                        const float fb = cvtsw(static_cast<int32_t>(static_cast<uint32_t>(t) - static_cast<uint32_t>(v1)));
                        f2 = FPU_DIV_S(fb, fa);
                    }
                }
                break;
            }
            default:
                f4 = m.rf(C + 4u);
                f3 = m.rf(C + 8u);
                f2 = 0.0f;
                break;
            }
            const float d = FPU_SUB_S(f3, f4);
            const float acc = addZero(f4);
            return FPU_ADD_S(acc, FPU_MUL_S(f2, d));
        }

        // ---- one emitter ------------------------------------------------------------------------------------------

        struct Plan
        {
            uint32_t E = 0u, N = 0u, F = 0u, P = 0u, Q = 0u, T = 0u;
            int32_t age = 0, D = 0, s7 = 0;
            uint32_t type = 0u;
            uint32_t tc = 0u;          // C0 * C4: history slots per particle
            uint32_t C0 = 0u, C4 = 0u;
            bool early = false;       // [T+0xB8] == 0: nothing to do
        };

        // sizes are small (< 2^25) and bases are checked against the RAM size first, so 32-bit sums cannot wrap
        inline bool rangeOk(uint32_t base, uint32_t size) { return base >= 0x100u && base <= kRamSize && size <= kRamSize - base; }
        inline bool overlap(uint32_t a, uint32_t as, uint32_t b, uint32_t bs) { return a < b + bs && b < a + as; }

        // Everything the native path needs to be able to run (nothing is written). false: the original runs.
        bool precheck(const Env &env, uint32_t E, Plan &pl)
        {
            const Mem &m = env.m;
            if (!rangeOk(E, 0x28u) || (E & 3u) != 0u)
            {
                ++g_c.fbRange;
                return false;
            }
            pl.E = E;
            pl.N = m.r32(E + 4u);
            pl.F = m.r32(E + 0xCu);
            pl.age = m.ri32(E + 0x10u);
            pl.P = m.r32(E + 0x18u);
            pl.Q = m.r32(E + 0x1Cu);
            pl.T = m.r32(E + 0x20u);
            if (!rangeOk(pl.T, 0x100u) || (pl.T & 3u) != 0u)
            {
                ++g_c.fbRange;
                return false;
            }
            pl.s7 = m.ri32(pl.T + 0xB8u);
            if (pl.s7 == 0)
            {
                pl.early = true;
                return true;
            }
            pl.D = m.ri32(pl.T + 0x20u);
            pl.type = m.r8(pl.T + 0x9Cu);
            pl.C0 = m.r32(pl.T + 0xC0u);
            pl.C4 = m.r32(pl.T + 0xC4u);
            pl.tc = pl.C0 * pl.C4;
            if (pl.s7 < 0 || pl.s7 > 0xFFFF || pl.N > 0x1000u || pl.C0 > 0x100u || pl.C4 > 0x100u || pl.tc > 0x1000u)
            {
                ++g_c.fbCount;
                return false;
            }
            if (pl.type == 2u)
            {
                ++g_c.fbType2;
                return false;
            }
            if (pl.tc != 0u && (pl.C0 < 1u))
            {
                ++g_c.fbTrail;
                return false;
            }
            if (!rangeOk(env.gp - 0x7F40u, 0x100u) || (pl.tc != 0u && !rangeOk(env.sp - 0x300u, 0x100u)))
            {
                ++g_c.fbRange;
                return false;
            }
            const uint32_t pBytes = (pl.N * (1u + pl.tc)) * 0x20u; // N <= 0x1000, tc <= 0x1000: < 2^30
            const uint32_t qBytes = pl.N * 0x3Cu;
            if (pl.N != 0u && (!rangeOk(pl.P, pBytes) || !rangeOk(pl.Q, qBytes) || ((pl.P | pl.Q) & 3u) != 0u))
            {
                ++g_c.fbRange;
                return false;
            }
            // the template, the effect object, the constants and the rand state must not be written by the update
            const uint32_t gpLo = env.gp - 0x7F40u;
            struct Rg { uint32_t a; uint32_t n; };
            const Rg readOnly[] = {{pl.T, 0x100u}, {E, 0x28u}, {kRandState, 16u}, {kUpVector, 16u}, {gpLo, 0x1000u}};
            const Rg written[] = {{pl.P, pBytes}, {pl.Q, qBytes}};
            if (pl.N != 0u && overlap(pl.P, pBytes, pl.Q, qBytes))
            {
                ++g_c.fbAlias; // the live-particle update keeps the slot and its data block in locals
                return false;
            }
            if (pl.N != 0u)
                for (const Rg &w : written)
                    for (int i = 0; i < 5; ++i)
                        if (overlap(w.a, w.n, readOnly[i].a, readOnly[i].n))
                        {
                            ++g_c.fbAlias;
                            g_c.aliasWhy[(&w - written) * 5 + i]++;
                            if (g_c.fbAlias < 6u)
                                std::fprintf(stderr, "[p3fes-effects] alias E=%08X N=%u P=%08X Q=%08X T=%08X tc=%u\n", E, pl.N, pl.P, pl.Q, pl.T, pl.tc);
                            return false;
                        }
            for (int i = 0; i < 5; ++i)
                for (int j = i + 1; j < 4; ++j)
                    if (overlap(readOnly[i].a, readOnly[i].n, readOnly[j].a, readOnly[j].n))
                    {
                        ++g_c.fbAlias;
                        g_c.aliasWhy[10 + i * 5 + j]++;
                        return false;
                    }
            return true;
        }

        // 0x329D60: cubic Hermite through four slots (pts[0..3]); the w lanes of the coefficient vectors and of the 0.5
        // constant are stale stack words of the original's frame (g[0..4]).
        __m128 cubic(const __m128 pts[4], float t, const uint32_t garbage[5])
        {
            const float f5 = FPU_MUL_S(t, t);
            const float f4 = FPU_MUL_S(f5, t);
            const __m128 half = _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(garbage[4]), 0x3F000000, 0x3F000000, 0x3F000000));
            __m128 vf12 = PS2_VSUB(pts[1], pts[0]);
            __m128 vf10 = PS2_VADD(PS2_VSUB(pts[2], pts[1]), vf12);
            const __m128 m1 = PS2_VMUL(vf10, half);
            vf12 = PS2_VSUB(pts[2], pts[1]);
            vf10 = PS2_VADD(PS2_VSUB(pts[3], pts[2]), vf12);
            const __m128 m2 = PS2_VMUL(vf10, half);
            const float f3 = FPU_MUL_S(3.0f, f5);
            const float f2 = 2.0f;
            float f0 = FPU_MUL_S(f2, f4);
            float f1 = FPU_SUB_S(f0, f3);
            const float h00 = FPU_ADD_S(1.0f, f1);
            f1 = 0.0f;
            float acc = FPU_ADD_S(f1, f4);
            f0 = FPU_SUB_S(acc, FPU_MUL_S(f2, f5));
            const float h10 = FPU_ADD_S(t, f0);
            const float h11 = FPU_SUB_S(f4, f5);
            acc = FPU_ADD_S(f1, f3);
            const float h01 = FPU_ADD_S(acc, FPU_MUL_S(-2.0f, f4));
            auto vec = [](float v, uint32_t w) { return _mm_castsi128_ps(_mm_set_epi32(static_cast<int>(w), static_cast<int>(fbits(v)), static_cast<int>(fbits(v)), static_cast<int>(fbits(v)))); };
            vf10 = PS2_VMUL(vec(h00, garbage[0]), pts[1]);
            vf12 = vf10;
            vf10 = PS2_VMUL(vec(h10, garbage[1]), m1);
            vf12 = PS2_VADD(vf12, vf10);
            vf10 = PS2_VMUL(vec(h11, garbage[2]), m2);
            vf12 = PS2_VADD(vf12, vf10);
            vf10 = PS2_VMUL(vec(h01, garbage[3]), pts[2]);
            return PS2_VADD(vf10, vf12);
        }

        // 0x32A890(E, p): history of the particle p (trail slots stored behind the N particles of P)
        void trailUpdate(Env &env, const Plan &pl, uint32_t p)
        {
            Mem &m = env.m;
            ++env.calls;
            const uint32_t s3 = pl.tc;
            const uint32_t s4 = pl.C4;
            const uint32_t s7 = pl.C0;
            if (s3 == 0u)
                return;
            const int32_t s2 = m.ri32(p + 0x10u);
            const uint32_t idx = (p - pl.P) >> 5;
            const uint32_t s0 = pl.P + ((pl.N + idx * s3) << 5);
            if (s2 == 0)
            {
                ++g_c.trailInit;
                ++env.calls;
                m.copy32(s0, p);
                uint32_t slot = s0;
                for (uint32_t a1 = 0u; a1 < s3; ++a1, slot += 0x20u)
                {
                    m.w32(slot + 0x10u, 0xFFFFFFFFu);
                    ++env.edges;
                }
                return;
            }
            m.w32(s0 + 0x10u, static_cast<uint32_t>(s2 - 1));
            float f20 = 1.0f;
            const float f22 = FPU_DIV_S(f20, u32ToFloat(s3));
            float f23 = 0.0f;
            const float f21 = cvtsw(static_cast<int32_t>(m.r32(p + 0x14u) >> 24));
            const uint32_t count = s3 - s4;
            uint32_t dst = s0 + (s3 << 5) - 0x20u;
            uint32_t src = dst - (s4 << 5);
            for (uint32_t s6 = 0u; s6 < count; ++s6)
            {
                ++env.calls;
                m.copy32(dst, src);
                const uint32_t color = m.r32(src + 0x14u);
                const float f1 = FPU_MUL_S(f21, f23);
                int32_t v1;
                if (cLe(env, 2147483648.0f, f1))
                    v1 = static_cast<int32_t>(static_cast<uint32_t>(cvtws(FPU_SUB_S(f1, 2147483648.0f))) | 0x80000000u);
                else
                    v1 = cvtws(f1);
                m.w32(dst + 0x14u, (color & 0xFFFFFFu) | (static_cast<uint32_t>(v1) << 24));
                f23 = FPU_ADD_S(f23, f22);
                dst -= 0x20u;
                src -= 0x20u;
                ++env.edges;
            }
            ++env.calls;
            m.copy32(s0, p);
            m.w32(s0 + 0x10u, 0xFFFFFFFFu);
            if (s4 == 1u || s7 < 2u)
                return;
            const uint32_t slotS4 = s0 + (s4 << 5);
            if (s2 < 2 || s7 < 3u)
            {
                // 0x32AD0C: straight interpolation between slot 0 and slot s4
                ++g_c.trailLow;
                if (s2 <= 0)
                    return;
                if (m.ri32(slotS4 + 0x10u) < 0)
                    return;
                uint32_t s1 = slotS4 - 0x20u;
                float f6 = 0.0f;
                const float f7 = FPU_DIV_S(1.0f, cvtsw(static_cast<int32_t>(s4 + 1u)));
                const float f5 = m.rf(slotS4 + 0x18u);
                const float f4 = FPU_SUB_S(m.rf(s0 + 0x18u), f5);
                const float f3 = m.rf(slotS4 + 0x1Cu);
                const float f2 = FPU_SUB_S(m.rf(s0 + 0x1Cu), f3);
                const __m128 cv = _mm_set1_ps(env.cInv255);
                __m128 vf11 = PS2_VMUL(unpackColor(m.r32(s0 + 0x14u)), cv);
                __m128 V1 = PS2_VMUL(unpackColor(m.r32(slotS4 + 0x14u)), cv);
                vf11 = PS2_VSUB(vf11, V1);
                const __m128 ones = _mm_setr_ps(1.0f, 1.0f, 1.0f, 0.0f);
                const __m128 D = PS2_VMUL(PS2_VMUL(ones, _mm_set1_ps(f7)), vf11);
                const uint32_t a0 = s4 - 1u;
                for (uint32_t t0 = 0u; t0 < a0; ++t0)
                {
                    f6 = FPU_ADD_S(f6, f7);
                    __m128 vf10 = m.ld4(slotS4);
                    vf11 = m.ld4(s0);
                    const __m128 vf2x = _mm_set1_ps(f6);
                    // vsubx.w vf3, vf0, vf2x: only the w lane (1 - t) is written
                    const __m128 vf3 = PS2_VSUB(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f), vf2x);
                    const __m128 accv = PS2_VMUL(vf11, vf2x);
                    vf10 = PS2_VADD(accv, PS2_VMUL(vf10, bc<3>(vf3)));
                    m.st4(s1, vf10);
                    float acc = addZero(f5);
                    m.wf(s1 + 0x18u, FPU_ADD_S(acc, FPU_MUL_S(f4, f6)));
                    acc = addZero(f3);
                    m.wf(s1 + 0x1Cu, FPU_ADD_S(acc, FPU_MUL_S(f2, f6)));
                    V1 = PS2_VADD(V1, D);
                    vf10 = PS2_VMUL(V1, _mm_set1_ps(255.0f));
                    m.w32(s1 + 0x14u, packColor(vf10));
                    m.w32(s1 + 0x10u, static_cast<uint32_t>(s2));
                    s1 -= 0x20u;
                    ++env.edges;
                }
                return;
            }
            // 0x32AB08: cubic through slots 2*s4, s4, 0, 0
            ++g_c.trailHigh;
            const uint32_t slot2 = s0 + (s4 << 6);
            if (m.ri32(slot2 + 0x10u) < 0)
                return;
            __m128 pts[4] = {m.ld4(slot2), m.ld4(slotS4), m.ld4(s0), m.ld4(s0)};
            // stale words of 0x329D60's frame: sp(entry) - 0x140 - 0x150 - 0x70
            uint32_t garbage[5];
            const uint32_t cubicSp = env.sp - 0x140u - 0x150u - 0x70u;
            for (int i = 0; i < 5; ++i)
                garbage[i] = m.r32(cubicSp + 0xCu + 0x10u * static_cast<uint32_t>(i));
            uint32_t s1 = slotS4 - 0x20u;
            float f21t = 0.0f;
            const float f22t = FPU_DIV_S(f20, cvtsw(static_cast<int32_t>(s4 + 1u)));
            f20 = m.rf(slotS4 + 0x18u);
            const float f26 = FPU_SUB_S(m.rf(s0 + 0x18u), f20);
            const float f25 = m.rf(slotS4 + 0x1Cu);
            const float f24 = FPU_SUB_S(m.rf(s0 + 0x1Cu), f25);
            const __m128 cv = _mm_set1_ps(env.cInv255);
            __m128 vf11 = PS2_VMUL(unpackColor(m.r32(s0 + 0x14u)), cv);
            __m128 V1 = PS2_VMUL(unpackColor(m.r32(slotS4 + 0x14u)), cv);
            vf11 = PS2_VSUB(vf11, V1);
            const __m128 ones = _mm_setr_ps(1.0f, 1.0f, 1.0f, 0.0f);
            const __m128 D = PS2_VMUL(PS2_VMUL(ones, _mm_set1_ps(f22t)), vf11);
            const uint32_t bound = s4 - 1u;
            for (uint32_t s3i = 0u; s3i < bound; ++s3i)
            {
                f21t = FPU_ADD_S(f21t, f22t);
                ++env.calls;
                const __m128 vf10 = cubic(pts, f21t, garbage);
                m.st4(s1, vf10);
                float acc = addZero(f20);
                m.wf(s1 + 0x18u, FPU_ADD_S(acc, FPU_MUL_S(f26, f21t)));
                acc = addZero(f25);
                m.wf(s1 + 0x1Cu, FPU_ADD_S(acc, FPU_MUL_S(f24, f21t)));
                V1 = PS2_VADD(V1, D);
                m.w32(s1 + 0x14u, packColor(PS2_VMUL(V1, _mm_set1_ps(255.0f))));
                m.w32(s1 + 0x10u, static_cast<uint32_t>(s2));
                s1 -= 0x20u;
                ++env.edges;
            }
        }

        // The colour / alpha / size curves of a template are pure functions of the template bytes 0x2C..0xBF, the lifetime and
        // the 1/255 constant, so every (template, age) pair is evaluated once and replayed (the FCR31 compare bit of the
        // colour curve is replayed too). Entries are matched byte for byte, never by hash alone.
        bool g_curveCache = true;
        bool g_trace = false;
        constexpr uint32_t kCurveAges = 128u;
        constexpr uint32_t kCurveEntries = 8u;
        constexpr uint32_t kCurveKey = 0x94u; // T+0x2C .. T+0xC0
        struct CurveEntry
        {
            bool valid = false;
            int32_t L = 0;
            uint32_t cbits = 0u;
            uint8_t key[kCurveKey];
            uint8_t have[kCurveAges];       // bit0 colour, bit1 +0x18 curve, bit2 +0x1C curve, bit3 colour compare result
            uint32_t color[kCurveAges];
            float f18[kCurveAges], f1c[kCurveAges];
        };
        CurveEntry g_curves[kCurveEntries];
        uint32_t g_curveNext = 0u;

        // newlib's memcmp costs ~4 instructions a byte on the Vita: 128-bit compares for the 148 byte template key
        inline bool sameKey(const uint8_t *a, const uint8_t *b)
        {
            __m128i acc = _mm_setzero_si128();
            for (uint32_t i = 0u; i < 9u; ++i)
                acc = _mm_or_si128(acc, _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + 16u * i)),
                                                      _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + 16u * i))));
            uint32_t ta, tb;
            std::memcpy(&ta, a + 144u, 4u);
            std::memcpy(&tb, b + 144u, 4u);
            return ta == tb && _mm_movemask_epi8(_mm_cmpeq_epi8(acc, _mm_setzero_si128())) == 0xFFFF;
        }

        CurveEntry *findCurve(const uint8_t *ram, uint32_t T, int32_t L, uint32_t cbits)
        {
            if (L <= 0 || static_cast<uint32_t>(L) > kCurveAges)
                return nullptr;
            const uint8_t *key = ram + ((T + 0x2Cu) & PS2_RAM_MASK);
            static uint32_t last = 0u; // most recently used entry first: neighbouring emitters usually share a template
            for (uint32_t n = 0u; n < kCurveEntries; ++n)
            {
                const uint32_t i = (last + n) % kCurveEntries;
                CurveEntry &e = g_curves[i];
                if (e.valid && e.L == L && e.cbits == cbits && sameKey(e.key, key))
                {
                    last = i;
                    return &e;
                }
            }
            ++g_c.curveEntryMiss;
            CurveEntry &e = g_curves[g_curveNext % kCurveEntries];
            last = g_curveNext++ % kCurveEntries;
            e.valid = true;
            e.L = L;
            e.cbits = cbits;
            std::memcpy(e.key, key, kCurveKey);
            std::memset(e.have, 0, sizeof(e.have));
            return &e;
        }

        // 0x32A770(p, T, age, prev) for kinds 0 and 1: colour, alpha/size curves
        inline void particleAttrs(Env &env, const Plan &pl, uint32_t p, int32_t age)
        {
            Mem &m = env.m;
            ++env.calls;
            const uint32_t T = pl.T;
            if (CurveEntry *e = env.curve; e && static_cast<uint32_t>(age) < static_cast<uint32_t>(pl.s7))
            {
                const uint32_t a = static_cast<uint32_t>(age);
                uint8_t &h = e->have[a];
                if (!(h & 1u))
                {
                    ++g_c.attrMiss;
                    e->color[a] = colorCurve(env, T + 0x2Cu, T + 0x50u, age, pl.s7);
                    h = static_cast<uint8_t>(h | 1u | (env.cVal ? 8u : 0u));
                }
                else
                {
                    env.calls += 1u;
                    env.cSet = true;
                    env.cVal = (h & 8u) != 0u;
                }
                if (!(h & 2u))
                {
                    e->f18[a] = floatCurve(env, T + 0x60u, age, pl.s7);
                    h = static_cast<uint8_t>(h | 2u);
                }
                else
                    env.calls += 1u;
                if (!(h & 4u))
                {
                    e->f1c[a] = floatCurve(env, T + 0x8Cu, age, pl.s7);
                    h = static_cast<uint8_t>(h | 4u);
                }
                else
                    env.calls += 1u;
                m.w32(p + 0x14u, e->color[a]);
                m.wf(p + 0x18u, e->f18[a]);
                m.wf(p + 0x1Cu, e->f1c[a]);
                return;
            }
            ++g_c.attrNoCache;
            m.w32(p + 0x14u, colorCurve(env, T + 0x2Cu, T + 0x50u, age, pl.s7));
            m.wf(p + 0x18u, floatCurve(env, T + 0x60u, age, pl.s7));
            m.wf(p + 0x1Cu, floatCurve(env, T + 0x8Cu, age, pl.s7));
        }

        // 0x357E30 results by orientation (the rows depend on nothing else)
        struct BasisEntry
        {
            bool valid = false;
            uint32_t q[4];
            __m128 vf[10]; // kBasisRegs
            __m128 acc;
        };
        constexpr uint32_t kBasisEntries = 128u; // direct mapped by a hash of the quaternion
        BasisEntry g_basis[kBasisEntries];
        constexpr int kBasisRegs[] = {1, 2, 3, 4, 5, 6, 28, 29, 30, 31};

        // The whole call. The plan comes from the precheck, so nothing here can bail out.
        // one free slot is respawned (0x32B28C..0x32B918); s0 is the spawn counter of the call
        template <bool kTime>
        __attribute__((noinline)) void spawnStep(Env &env, const Plan &pl, Vu &u, uint32_t s3, uint32_t s4, int32_t &s0, bool burst, bool local,
                                                  float f22, float f27)
        {
            Mem &m = env.m;
            const uint32_t T = pl.T, N = pl.N;
            const int32_t s7 = pl.s7;
            const float f26 = 0.0f, f25 = 1.0f, f24 = 0.5f, f23 = 2.0f, f21 = -1.0f;
        // ---- spawn ------------------------------------------------------------------------------------
        SegTimerT<kTime> tsp(SEG_SPAWN);
        ++g_c.spawns;
        if (local)
        {
            m.wf(s3 + 0u, f26);
            m.wf(s3 + 4u, f25);
            m.wf(s3 + 8u, f26);
        }
        else
        {
            const __m128 vf10 = rotate(u, m.ld4(kUpVector));
            alignas(16) float lanes[4];
            _mm_store_ps(lanes, vf10);
            m.w32(s3 + 0u, fbits(lanes[0]));
            m.w32(s3 + 4u, fbits(lanes[1]));
            m.w32(s3 + 8u, fbits(lanes[2]));
        }
        float g0 = randFloat(env);
        g0 = FPU_SUB_S(g0, f24);
        const float dx = FPU_MUL_S(f23, g0);
        g0 = randFloat(env);
        g0 = FPU_SUB_S(g0, f24);
        const float dz = FPU_MUL_S(f23, g0);
        __m128 vf10 = _mm_setr_ps(dx, f26, dz, f26);
        // vmul.xyz vf2 = vf10 * vf10; vf2.w = x*x + y*y + z*z (ACC.w sums); vrsqrt Q, vf0w, vf2w; vmulq.xyz
        {
            const __m128 sq = PS2_VMUL(vf10, vf10);
            const __m128 a = PS2_VMUL(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f), bc<0>(sq));
            const __m128 b = PS2_VADD(a, PS2_VMUL(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f), bc<1>(sq)));
            const __m128 c = PS2_VADD(b, PS2_VMUL(_mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f), bc<2>(sq)));
            const float ft = _mm_cvtss_f32(_mm_shuffle_ps(c, c, _MM_SHUFFLE(3, 3, 3, 3)));
            env.ctx->vu0_q = ps2VuRsqrt(env.ctx->vu0_status, 1.0f, ft);
            const __m128 res = PS2_VMUL(vf10, _mm_set1_ps(env.ctx->vu0_q));
            vf10 = _mm_blendv_ps(vf10, res, _mm_castsi128_ps(_mm_set_epi32(0, -1, -1, -1)));
        }
        if (!local)
            vf10 = rotate(u, vf10);
        {
            alignas(16) float lanes[4];
            _mm_store_ps(lanes, vf10);
            m.w32(s3 + 0xCu, fbits(lanes[0]));
            m.w32(s3 + 0x10u, fbits(lanes[1]));
            m.w32(s3 + 0x14u, fbits(lanes[2]));
        }
        // the ranges (0x32B380..0x32B510)
        auto ranged = [&](uint32_t rangeOff, uint32_t baseOff, bool useBase) -> float
        {
            const float f20 = m.rf(T + rangeOff);
            const float r = randFloat(env);
            const float f2 = FPU_SUB_S(f25, f20);
            const float acc = addZero(f2);
            const float f1 = FPU_ADD_S(acc, FPU_MUL_S(f20, r));
            return useBase ? FPU_MUL_S(m.rf(T + baseOff), f1) : f1;
        };
        m.wf(s3 + 0x2Cu, ranged(0xD0u, 0xCCu, true));
        const float lo = ranged(0xDCu, 0xD8u, true);
        const float hi = ranged(0xE4u, 0xE0u, true);
        m.wf(s3 + 0x20u, lo);
        {
            const float f1 = FPU_SUB_S(hi, lo);
            const float fs = cvtsw(s7);
            m.wf(s3 + 0x24u, FPU_DIV_S(f1, fs));
        }
        m.wf(s3 + 0x28u, ranged(0xECu, 0xE8u, true));
        {
            const float r = randFloat(env);
            const float f12 = FPU_MUL_S(f22, r);
            m.wf(s3 + 0x18u, f12);
            m.wf(s3 + 0x1Cu, psSin(env, f12));
        }
        {
            float r = randFloat(env);
            r = FPU_SUB_S(r, f24);
            const float f1 = FPU_MUL_S(f23, r);
            const float f0 = FPU_MUL_S(m.rf(T + 0xC8u), f1);
            const __m128 vf11 = _mm_setr_ps(f0, f0, f0, 0.0f);
            __m128 pos = PS2_VMUL(vf10, vf11);
            if (!local)
                pos = PS2_VADD(pos, m.ld4(T));
            m.st4(s4, pos);
        }
        {
            const float f20 = m.rf(T + 0x6Cu);
            const float r = randFloat(env);
            const float f2 = FPU_SUB_S(f25, f20);
            const float acc = addZero(f2);
            m.wf(s3 + 0x30u, FPU_ADD_S(acc, FPU_MUL_S(f20, r)));
        }
        if (pl.type != 2u)
        {
            const float f20 = m.rf(T + 0x98u);
            const float r = randFloat(env);
            const float f2 = FPU_SUB_S(f25, f20);
            const float acc = addZero(f2);
            m.wf(s3 + 0x38u, FPU_ADD_S(acc, FPU_MUL_S(f20, r)));
            if (pl.type == 1u)
            {
                const float r2 = randFloat(env);
                m.wf(s3 + 0x34u, FPU_MUL_S(f22, r2));
                if ((randInt(env) & 1u) != 0u)
                    m.wf(s3 + 0x38u, FPU_MUL_S(m.rf(s3 + 0x38u), f21));
            }
            else
                m.wf(s3 + 0x34u, f26);
        }
        else
        {
            m.wf(s3 + 0x34u, f26);
            m.wf(s3 + 0x38u, f25);
        }
        m.w32(s4 + 0x10u, 0u);
        if (burst)
        {
            ++g_c.bursts;
            const uint32_t rem = randInt(env) % static_cast<uint32_t>(s7);
            const float f20 = cvtsw(static_cast<int32_t>(rem));
            const float accm = FPU_MUL_S(m.rf(s3 + 0x2Cu), f20);
            float t0 = FPU_MUL_S(f27, f20);
            t0 = FPU_MUL_S(f20, t0);
            const float f3 = FPU_SUB_S(accm, FPU_MUL_S(f24, t0));
            for (uint32_t k = 0u; k < 3u; ++k)
            {
                const float f1 = m.rf(s3 + 4u * k);
                const float f0 = m.rf(s4 + 4u * k);
                const float acc = addZero(f0);
                m.wf(s4 + 4u * k, FPU_ADD_S(acc, FPU_MUL_S(f1, f3)));
            }
            {
                const float f1 = m.rf(s3 + 0x28u);
                const float f0 = m.rf(s3 + 0x18u);
                const float acc = addZero(f0);
                m.wf(s3 + 0x18u, FPU_ADD_S(acc, FPU_MUL_S(f1, f20)));
            }
            {
                const float f1 = m.rf(s3 + 0x24u);
                const float f0 = m.rf(s3 + 0x20u);
                const float acc = addZero(f0);
                m.wf(s3 + 0x20u, FPU_ADD_S(acc, FPU_MUL_S(f1, f20)));
            }
            const float sn = psSin(env, m.rf(s3 + 0x18u));
            const float f4 = FPU_MUL_S(m.rf(s3 + 0x20u), sn);
            for (uint32_t k = 0u; k < 3u; ++k)
            {
                const float f2 = m.rf(s3 + 0xCu + 4u * k);
                const float f1 = m.rf(s4 + 4u * k);
                const float acc = addZero(f1);
                m.wf(s4 + 4u * k, FPU_ADD_S(acc, FPU_MUL_S(f2, f4)));
            }
            m.wf(s3 + 0x18u, FPU_ADD_S(m.rf(s3 + 0x18u), m.rf(s3 + 0x28u)));
            m.wf(s3 + 0x20u, FPU_ADD_S(m.rf(s3 + 0x20u), m.rf(s3 + 0x24u)));
            m.wf(s3 + 0x1Cu, sn);
            m.w32(s4 + 0x10u, static_cast<uint32_t>(cvtws(f20)));
        }
        // 0x32B718: attributes with the age just stored
        particleAttrs(env, pl, s4, m.ri32(s4 + 0x10u));
        m.wf(s4 + 0x18u, FPU_MUL_S(m.rf(s4 + 0x18u), m.rf(s3 + 0x30u)));
        m.wf(s4 + 0x1Cu, FPU_MUL_S(m.rf(s4 + 0x1Cu), m.rf(s3 + 0x38u)));
        m.wf(s4 + 0x1Cu, FPU_ADD_S(m.rf(s4 + 0x1Cu), m.rf(s3 + 0x34u)));
        if (burst)
        {
            const uint32_t a2 = pl.tc;
            if (a2 != 0u)
            {
                ++env.calls;
                const uint32_t idx = (s4 - pl.P) >> 5;
                const uint32_t slot = pl.P + ((N + idx * a2) << 5);
                m.copy32(slot, s4);
                m.w32(slot + 0x10u, 0xFFFFFFFFu);
            }
            m.w32(s4 + 0x10u, m.r32(s4 + 0x10u) + 1u);
        }
        --s0;
        }

        // one live particle (0x32B7D4..0x32B918). The recompiled code stores the position twice (once per move) and the
        // attributes twice (curve value, then scaled); only the final values are observable, so they are kept in registers
        // and written once. The slot (P) and its data block (Q) never overlap (precheck).
        template <bool kTime>
        __attribute__((noinline)) void aliveStep(Env &env, const Plan &pl, uint32_t s3, uint32_t s4, int32_t s5, float f27)
        {
            Mem m = env.m;
            SegTimerT<kTime> tal(SEG_ALIVE);
            ++env.live;
            const float ang = m.rf(s3 + 0x18u);
            const float f0 = [&] { SegTimerT<kTime> ts(SEG_SIN); return psSin(env, ang); }();
            const float f1 = m.rf(s3 + 0x1Cu);
            const float f2 = FPU_SUB_S(f0, f1);
            const float f3 = FPU_MUL_S(m.rf(s3 + 0x20u), f2);
            const float fAge = cvtsw(s5);
            const float acc0 = addZero(m.rf(s3 + 0x2Cu));
            const float f3b = FPU_SUB_S(acc0, FPU_MUL_S(f27, fAge));
            for (uint32_t k = 0u; k < 3u; ++k)
            {
                const float p = m.rf(s4 + 4u * k);
                const float a1 = addZero(p);
                const float r1 = FPU_ADD_S(a1, FPU_MUL_S(m.rf(s3 + 0xCu + 4u * k), f3));
                const float a2 = addZero(r1);
                m.wf(s4 + 4u * k, FPU_ADD_S(a2, FPU_MUL_S(m.rf(s3 + 4u * k), f3b)));
            }
            m.wf(s3 + 0x18u, FPU_ADD_S(ang, m.rf(s3 + 0x28u)));
            m.wf(s3 + 0x20u, FPU_ADD_S(m.rf(s3 + 0x20u), m.rf(s3 + 0x24u)));
            m.wf(s3 + 0x1Cu, f0);
            CurveEntry *e = env.curve;
            float f18, f1c;
            if (e && static_cast<uint32_t>(s5) < static_cast<uint32_t>(pl.s7) && (e->have[s5] & 7u) == 7u)
            {
                const uint32_t a = static_cast<uint32_t>(s5);
                env.calls += 4u;
                env.cSet = true;
                env.cVal = (e->have[a] & 8u) != 0u;
                m.w32(s4 + 0x14u, e->color[a]);
                f18 = e->f18[a];
                f1c = e->f1c[a];
            }
            else
            {
                particleAttrs(env, pl, s4, s5);
                f18 = m.rf(s4 + 0x18u);
                f1c = m.rf(s4 + 0x1Cu);
            }
            m.wf(s4 + 0x18u, FPU_MUL_S(f18, m.rf(s3 + 0x30u)));
            m.wf(s4 + 0x1Cu, FPU_ADD_S(FPU_MUL_S(f1c, m.rf(s3 + 0x38u)), m.rf(s3 + 0x34u)));
            if (pl.tc != 0u)
                trailUpdate(env, pl, s4);
            else
                ++env.calls;
            m.w32(s4 + 0x10u, static_cast<uint32_t>(s5 + 1));
        }

        template <bool kTime>
        void runEmitterT(Env &env, const Plan &pl, Vu &u)
        {
            Mem &m = env.m;
            const uint32_t E = pl.E, T = pl.T;
            const uint64_t tSetup0 = kTime ? stamp() : 0u;
            const uint32_t gp = env.gp;
            const int32_t s7 = pl.s7;
            const bool local = (pl.F & 1u) != 0u;
            env.cInv255 = m.rf(gp - 0x7EA4u);

            // 0x32AFC4..0x32AFC8: vf10 = orientation, 0x357E30 builds the rotation
            {
            SegTimerT<kTime> tBasis(SEG_BASIS);
            u.vf[10] = m.ld4(T + 0x10u);
            ++env.calls;
            {
                uint32_t q[4];
                std::memcpy(q, env.rdram + ((T + 0x10u) & PS2_RAM_MASK), 16u);
                BasisEntry &slot = g_basis[((q[0] * 0x9E3779B1u) ^ (q[1] * 0x85EBCA6Bu) ^ (q[2] * 0xC2B2AE35u) ^ (q[3] * 0x27D4EB2Fu)) >> 25];
                if (g_curveCache && slot.valid && slot.q[0] == q[0] && slot.q[1] == q[1] && slot.q[2] == q[2] && slot.q[3] == q[3])
                {
                    for (int i = 0; i < 10; ++i)
                        u.vf[kBasisRegs[i]] = slot.vf[i];
                    u.acc = slot.acc;
                }
                else
                {
                    basis357E30(u);
                    ++g_c.basisMiss;
                    slot.valid = true;
                    std::memcpy(slot.q, q, 16u);
                    for (int i = 0; i < 10; ++i)
                        slot.vf[i] = u.vf[kBasisRegs[i]];
                    slot.acc = u.acc;
                }
            }
            }
            CurveEntry *curve = nullptr;
            {
                SegTimerT<kTime> tCurve(SEG_CURVE);
                if (g_curveCache)
                    curve = findCurve(env.rdram, T, s7, fbits(env.cInv255));
            }
            env.curve = curve;
            g_c.s7Sum += static_cast<uint64_t>(s7);
            g_c.nSum += pl.N;

            int32_t s0 = 0;
            bool burst = false; // s6
            const int32_t D = pl.D;
            const int32_t age = pl.age;
            const float cRate = m.rf(gp - 0x7F40u);
            if (D != 0 && !(age < D))
            {
                // 0x32AFEC: the effect is over its duration: no spawning
            }
            else if (age == 0 && m.r8(T + 0xBDu) != 0u)
            {
                burst = true; // 0x32B014
                const float f1 = m.rf(T + 0x28u);
                if (cLe(env, f1, 0.0f))
                    s0 = static_cast<int32_t>(pl.N);
                else
                {
                    const float r = randFloat(env);
                    float g1 = FPU_SUB_S(cRate, m.rf(T + 0x28u));
                    g1 = FPU_MUL_S(g1, r);
                    const float g0 = FPU_MUL_S(u32ToFloat(pl.N), g1);
                    s0 = cvtws(g0);
                }
            }
            else
            {
                // 0x32B0A8
                const float f1 = m.rf(T + 0x28u);
                if (cLe(env, f1, 0.0f))
                {
                    const float f1b = u32ToFloat(m.r32(T + 0x24u));
                    m.wf(E + 0x14u, FPU_ADD_S(m.rf(E + 0x14u), f1b));
                }
                else
                {
                    const float r = randFloat(env);
                    float g1 = FPU_SUB_S(cRate, m.rf(T + 0x28u));
                    const float f3 = FPU_MUL_S(g1, r);
                    const float f2 = u32ToFloat(m.r32(T + 0x24u));
                    const float acc = addZero(m.rf(E + 0x14u));
                    m.wf(E + 0x14u, FPU_ADD_S(acc, FPU_MUL_S(f2, f3)));
                }
                // 0x32B184
                const float f1c = m.rf(E + 0x14u);
                const float f0 = FPU_ABS_S(f1c);
                const int32_t w = cvtws(f0);
                s0 = w;
                m.wf(E + 0x14u, FPU_SUB_S(f1c, cvtsw(w)));
            }

            // 0x32B1A0: constants of the loop
            const float f22 = m.rf(gp - 0x7EA0u);
            const float f27 = m.rf(T + 0xD4u);
            uint32_t s4 = pl.P, s3 = pl.Q;
            const uint32_t N = pl.N;
            const uint64_t tLoop0 = kTime ? stamp() : 0u;
            if (kTime)
            {
                g_seg[SEG_SETUP] += delta(tSetup0, tLoop0);
                ++g_segN[SEG_SETUP];
            }
            if (N > 1u)
                env.edges += N - 1u; // back edges of the slot loop (0x32B92C), 32 cycles each
            for (uint32_t fp = 0u; fp < N; ++fp, s3 += 0x3Cu, s4 += 0x20u)
            {
                // a free (-1) slot without anything to spawn and a finished (-2) one are both skipped untouched
                // (s7 > 0, so neither is retired first): run over them without touching anything else
                {
                    const uint8_t *q = env.rdram + ((s4 + 0x10u) & PS2_RAM_MASK);
                    const int32_t freeSkip = s0 == 0 ? -1 : -2;
                    uint32_t k = 0u;
                    const uint32_t left = N - fp;
                    for (; k < left; ++k)
                    {
                        int32_t st;
                        std::memcpy(&st, q + 0x20u * k, 4u);
                        if (st != -2 && st != freeSkip)
                            break;
                    }
                    if (k != 0u)
                    {
                        fp += k;
                        s4 += k * 0x20u;
                        s3 += k * 0x3Cu;
                        if (fp >= N)
                            break;
                    }
                }
                int32_t s5 = m.ri32(s4 + 0x10u);
                if (!(s5 < s7))
                {
                    // 0x32B1E4: retired
                    ++g_c.expired;
                    m.w32(s4 + 0x10u, D != 0 ? 0xFFFFFFFEu : 0xFFFFFFFFu);
                    const uint32_t a2 = pl.tc;
                    if (a2 != 0u)
                    {
                        const uint32_t idx = (s4 - pl.P) >> 5;
                        uint32_t slot = pl.P + ((N + idx * a2) << 5);
                        for (uint32_t a1 = 0u; a1 < a2; ++a1, slot += 0x20u)
                        {
                            m.w32(slot + 0x10u, 0xFFFFFFFFu);
                            ++env.edges;
                        }
                    }
                    s5 = m.ri32(s4 + 0x10u);
                }
                if (s5 == -2)
                    continue;
                if (s5 == -1)
                {
                    if (s0 == 0)
                        continue;
                    SegTimerT<kTime> tsp(SEG_SPAWN);
                    spawnStep<kTime>(env, pl, u, s3, s4, s0, burst, local, f22, f27);
                    continue;
                }
                aliveStep<kTime>(env, pl, s3, s4, s5, f27);
            }
            g_c.particles += env.live;
            if (kTime)
            {
                g_seg[SEG_LOOP] += delta(tLoop0, stamp());
                ++g_segN[SEG_LOOP];
            }
        }

        // ---- hook ---------------------------------------------------------------------------------------------------

        inline void runEmitter(Env &env, const Plan &pl, Vu &u)
        {
            if (g_timeMode == 2)
                runEmitterT<true>(env, pl, u);
            else
                runEmitterT<false>(env, pl, u);
        }

        inline void chargeCycles(PS2Runtime *runtime, uint32_t cycles)
        {
            if (cycles == 0u || runtime->eeTryChargeCall(cycles))
                return;
            for (uint32_t n = 0u; n < cycles / 32u; ++n)
                if (!runtime->eeTryChargeCall(32u))
                    break;
        }

        void loadVu(Vu &u, const R5900Context *ctx)
        {
            // 0x357E30 writes every lane of the registers it leaves behind (the rotation rows) and the lanes it reads of the
            // others (vf1.xyz, vf2..vf6) itself, so nothing but the constant vf0 has to come from the guest context
            (void)ctx;
            for (int i : {1, 2, 3, 4, 5, 6, 10, 28, 29, 30, 31})
                u.vf[i] = _mm_setzero_ps();
            u.vf[0] = _mm_setr_ps(0.0f, 0.0f, 0.0f, 1.0f);
            u.acc = _mm_setzero_ps();
        }
        void storeVu(const Vu &u, R5900Context *ctx)
        {
            // what 0x357E30 leaves behind (the rotation rows are the only part a later reader could use)
            for (int i : {28, 29, 30, 31})
                ctx->vu0_vf[i] = u.vf[i];
        }

        void finishCall(Env &env, uint32_t ra)
        {
            R5900Context *ctx = env.ctx;
            if (env.cSet)
                ctx->fcr31 = env.cVal ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
            ctx->pc = ra;
        }

        // ---- verify -------------------------------------------------------------------------------------------------

        std::vector<uint8_t> g_snapStart, g_snapNative;

        struct Roi
        {
            uint32_t base, len;
        };

        void verifyCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Plan &pl, uint32_t ra)
        {
            ++g_verifyTick;
            const bool full = g_c.verified < 64u || (g_verifyFull != 0u && (g_c.verified % g_verifyFull) == 0u);
            uint8_t *spr = ps2GetScratchpadHostPtr();
            uint8_t sprStart[16384], sprNative[16384];
            if (full)
            {
                g_snapStart.resize(kRamSize);
                g_snapNative.resize(kRamSize);
                std::memcpy(g_snapStart.data(), rdram, kRamSize);
            }
            if (spr)
                std::memcpy(sprStart, spr, sizeof(sprStart));
            // regions of interest, read before and after
            std::vector<Roi> roi;
            roi.push_back({pl.E & PS2_RAM_MASK, 0x28u});
            roi.push_back({pl.T & PS2_RAM_MASK, 0x100u});
            if (pl.N != 0u)
            {
                roi.push_back({pl.P & PS2_RAM_MASK, static_cast<uint32_t>(pl.N * (1u + pl.tc) * 0x20u)});
                roi.push_back({pl.Q & PS2_RAM_MASK, static_cast<uint32_t>(pl.N * 0x3Cu)});
            }
            roi.push_back({kRandState, 16u});
            const R5900Context saved = *ctx;

            // native run on a private context copy, journaled
            R5900Context nctx = *ctx;
            Journal jr;
            Env env{rdram, runtime, &nctx, Mem{rdram, &jr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            Vu vu;
            loadVu(vu, &nctx);
            runEmitter(env, pl, vu);
            storeVu(vu, &nctx);
            finishCall(env, ra);
            const bool nativeFailed = env.failed;

            std::vector<std::vector<uint8_t>> roiNative(roi.size());
            for (size_t i = 0; i < roi.size(); ++i)
                roiNative[i].assign(rdram + roi[i].base, rdram + roi[i].base + roi[i].len);
            if (full)
                std::memcpy(g_snapNative.data(), rdram, kRamSize);
            if (spr)
                std::memcpy(sprNative, spr, sizeof(sprNative));
            // roll back (reverse order restores the oldest bytes last)
            for (size_t i = jr.entries.size(); i-- > 0;)
                std::memcpy(rdram + jr.entries[i].addr, jr.entries[i].old, jr.entries[i].len);

            // the original
            g_orig(rdram, ctx, runtime);
            if (ctx->pc != ra)
            {
                ++g_c.verifyUnwound;
                return;
            }
            ++g_c.verified;
            if (full)
                ++g_c.verifiedFull;
            bool bad = nativeFailed;
            uint32_t writeBad = 0u;
            for (size_t i = 0; i < roi.size(); ++i)
                if (std::memcmp(rdram + roi[i].base, roiNative[i].data(), roi[i].len) != 0)
                {
                    uint32_t shown = 0u;
                    for (uint32_t o = 0u; o < roi[i].len; o += 4u)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, rdram + roi[i].base + o, 4u);
                        std::memcpy(&b, roiNative[i].data() + o, 4u);
                        if (a != b)
                        {
                            if (shown++ < 4u)
                                std::fprintf(stderr, "[p3fes-effects] ROI MISMATCH region %u at %08X (+%X): native %08X original %08X\n", static_cast<unsigned>(i),
                                             roi[i].base + o, o, b, a);
                            ++writeBad;
                        }
                    }
                }
            if (writeBad != 0u)
            {
                ++g_c.verifyRoiBad;
                bad = true;
            }
            if (full)
            {
                const uint32_t sp = getRegU32(&saved, 29);
                const uint32_t lowGuard = (sp - 0x2000u) & PS2_RAM_MASK, highGuard = (sp + 0x10u) & PS2_RAM_MASK;
                uint32_t diff = 0u;
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
                            if (diff < 6u)
                            {
                                uint32_t a, b, c;
                                std::memcpy(&a, native + i, 4u);
                                std::memcpy(&b, after + i, 4u);
                                std::memcpy(&c, g_snapStart.data() + i, 4u);
                                std::fprintf(stderr, "[p3fes-effects] MEM MISMATCH at %08X: native %08X original %08X (start %08X)\n", i, a, b, c);
                            }
                            ++diff;
                        }
                    }
                }
                if (diff != 0u)
                {
                    ++g_c.verifyWriteBad;
                    bad = true;
                }
            }
            if (spr && std::memcmp(sprNative, spr, sizeof(sprNative)) != 0)
            {
                ++g_c.verifyScratch;
                bad = true;
            }
            // registers: callee-saved GPRs, f20..f31, pc
            bool regBad = false;
            for (int reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
                regBad = regBad || getRegU32(ctx, reg) != getRegU32(&saved, reg);
            for (int i = 20; i < 32; ++i)
                regBad = regBad || std::memcmp(&ctx->f[i], &saved.f[i], 4u) != 0;
            if (regBad)
            {
                ++g_c.verifyRegBad;
                bad = true;
            }
            // informational: state the original leaves that a caller could read (not counted as a mismatch)
            bool soft = false;
            soft = soft || std::memcmp(&ctx->vu0_vf[28], &nctx.vu0_vf[28], 16u * 4u) != 0;
            soft = soft || (nctx.fcr31 & 0x800000u) != (ctx->fcr31 & 0x800000u);
            soft = soft || ctx->vu0_q != nctx.vu0_q || ctx->vu0_status != nctx.vu0_status;
            if (soft)
                ++g_c.verifySoft;
            if (bad)
            {
                ++g_c.verifyBad;
                std::fprintf(stderr, "[p3fes-effects] VERIFY MISMATCH E=%08X N=%u type=%u tc=%u age=%d\n", pl.E, pl.N, pl.type, pl.tc, pl.age);
            }
        }


        // ---- fuzz verify: random emitters built in the dead stack area, native vs original ----------------------------
        //
        // The scenes only exercise some shapes (shop: kind 0, no trail). With P3FES_EFFECTS_FUZZ=n every sampled real call
        // is followed by n synthetic emitters (kinds 0/1, trails, bursts, durations, curve kinds, random and special floats)
        // that are built below the stack pointer, run natively on a journaled RAM, rolled back and run through the original.
        struct Rng
        {
            uint32_t s;
            uint32_t next()
            {
                s ^= s << 13;
                s ^= s >> 17;
                s ^= s << 5;
                return s;
            }
            uint32_t below(uint32_t n) { return next() % n; }
            float flt(float lo, float hi) { return lo + (hi - lo) * static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f; }
            float any()
            {
                const uint32_t k = below(100u);
                if (k < 55u) return flt(-2.0f, 2.0f);
                if (k < 70u) return flt(-100.0f, 100.0f);
                if (k < 76u) return 0.0f;
                if (k < 80u) return bitsf(0x80000000u);
                if (k < 84u) return bitsf(next() & 0x007FFFFFu);             // exponent 0 with a mantissa
                if (k < 88u) return flt(1.0e20f, 1.0e30f);
                if (k < 92u) return bitsf(0x7F800000u | (next() & 0x007FFFFFu)); // exponent 255 (a PS2 number, a host Inf/NaN)
                if (k < 96u) return flt(0.0f, 1.0f);
                return flt(-1.0e-30f, 1.0e-30f);
            }
        };

        struct FuzzStats { uint64_t runs = 0u, bad = 0u, skipped = 0u, unwound = 0u; } g_fz;

        bool fuzzOne(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Plan &real, uint32_t ra, uint32_t seed)
        {
            Rng rng{seed ? seed : 1u};
            const uint32_t sp = getRegU32(ctx, 29);
            const uint32_t S = (sp - 0x60000u) & ~0xFFu;
            constexpr uint32_t kSpan = 0x10000u;
            if (S < 0x200000u || S + kSpan > kRamSize)
                return true;
            std::vector<uint8_t> saved(rdram + S, rdram + S + kSpan);
            uint8_t randSaved[16];
            std::memcpy(randSaved, rdram + kRandState, 16u);
            Mem m{rdram, nullptr};
            const uint32_t E = S + 0x100u, T = S + 0x200u, P = S + 0x400u, Q = S + 0x6000u;
            // template: a real one with mutations
            std::memcpy(rdram + T, rdram + (real.T & PS2_RAM_MASK), 0x100u);
            for (uint32_t o = 0u; o < 0x100u; o += 4u)
                if (rng.below(3u) == 0u && o >= 0xC8u)
                    m.wf(T + o, rng.below(4u) == 0u ? rng.any() : rng.flt(0.0f, 2.0f));
            for (uint32_t o = 0u; o < 0x20u; o += 4u)
                m.wf(T + o, rng.below(6u) == 0u ? rng.any() : rng.flt(-1.0f, 1.0f));
            const int32_t s7 = rng.below(50u) == 0u ? 0 : 1 + static_cast<int32_t>(rng.below(40u));
            const int32_t D = rng.below(2u) == 0u ? 0 : 1 + static_cast<int32_t>(rng.below(60u));
            const uint32_t C0 = rng.below(3u) == 0u ? 0u : 1u + rng.below(5u);
            const uint32_t C4 = rng.below(3u) == 0u ? 0u : 1u + rng.below(3u);
            const uint32_t tc = C0 * C4;
            m.w32(T + 0xB8u, static_cast<uint32_t>(s7));
            m.w32(T + 0x20u, static_cast<uint32_t>(D));
            m.w32(T + 0xC0u, C0);
            m.w32(T + 0xC4u, C4);
            rdram[T + 0x9Cu] = static_cast<uint8_t>(rng.below(2u));
            rdram[T + 0xBDu] = static_cast<uint8_t>(rng.below(2u));
            m.wf(T + 0x28u, rng.below(4u) == 0u ? 0.0f : rng.flt(0.0f, 1.2f));
            m.w32(T + 0x24u, rng.below(8u) == 0u ? static_cast<uint32_t>(-static_cast<int32_t>(rng.below(5u))) : rng.below(6u));
            rdram[T + 0x2Cu] = static_cast<uint8_t>(rng.below(4u));
            rdram[T + 0x60u] = static_cast<uint8_t>(rng.below(4u));
            rdram[T + 0x8Cu] = static_cast<uint8_t>(rng.below(4u));
            for (uint32_t o : {0x30u, 0x34u, 0x38u, 0x3Cu, 0x40u, 0x44u, 0x48u, 0x4Cu, 0x64u, 0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu, 0x80u, 0x84u, 0x88u, 0x90u, 0x94u, 0x98u, 0x9Cu + 4u, 0xA0u, 0xA4u, 0xA8u, 0xACu, 0xB0u, 0xB4u})
                if (o != 0x9Cu)
                {
                    // thresholds are fractions of the lifetime, colours random words, the rest floats
                    if (rng.below(2u) == 0u)
                        m.wf(T + o, rng.flt(0.0f, 1.0f));
                    else
                        m.w32(T + o, rng.next());
                }
            m.w32(T + 0x50u, rng.below(256u));
            m.wf(T + 0x58u, rng.flt(0.0f, 0.5f));
            m.wf(T + 0x5Cu, rng.flt(0.5f, 1.0f));
            // effect object
            const uint32_t N = rng.below(25u) == 0u ? 0u : 1u + rng.below(7u);
            m.w32(E + 4u, N);
            m.w32(E + 0xCu, rng.below(2u) ? 1u : 0u);
            m.w32(E + 0x10u, rng.below(3u) == 0u ? 0u : rng.below(70u));
            m.wf(E + 0x14u, rng.flt(0.0f, 1.0f));
            m.w32(E + 0x18u, P);
            m.w32(E + 0x1Cu, Q);
            m.w32(E + 0x20u, T);
            const uint32_t slots = N * (1u + tc);
            if (slots * 0x20u > Q - P || N * 0x3Cu > 0x1000u)
            {
                ++g_fz.skipped;
                std::memcpy(rdram + S, saved.data(), kSpan);
                std::memcpy(rdram + kRandState, randSaved, 16u);
                return true;
            }
            for (uint32_t i = 0u; i < N; ++i)
            {
                const uint32_t p = P + i * 0x20u, q = Q + i * 0x3Cu;
                for (uint32_t o = 0u; o < 0x20u; o += 4u)
                    m.wf(p + o, rng.any());
                const uint32_t k = rng.below(10u);
                m.w32(p + 0x10u, k < 2u ? 0xFFFFFFFEu : (k < 5u ? 0xFFFFFFFFu : rng.below(static_cast<uint32_t>(s7 + 3))));
                m.w32(p + 0x14u, rng.next());
                for (uint32_t o = 0u; o < 0x3Cu; o += 4u)
                    m.wf(q + o, rng.below(8u) == 0u ? rng.any() : rng.flt(-1.0f, 1.0f));
            }
            for (uint32_t i = N; i < slots; ++i) // trail slots
            {
                const uint32_t p = P + i * 0x20u;
                for (uint32_t o = 0u; o < 0x20u; o += 4u)
                    m.wf(p + o, rng.flt(-5.0f, 5.0f));
                m.w32(p + 0x10u, rng.below(3u) == 0u ? 0xFFFFFFFFu : rng.below(20u));
                m.w32(p + 0x14u, rng.next());
            }
            for (uint32_t o = 0u; o < 16u; o += 4u)
                m.w32(kRandState + o, rng.next());

            R5900Context base = *ctx;
            base.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(E)));
            Plan pl;
            Env env{rdram, runtime, &base, Mem{rdram, nullptr}};
            env.gp = getRegU32(&base, 28);
            env.sp = sp;
            bool ok = true;
            if (!precheck(env, E, pl) || pl.early)
            {
                ++g_fz.skipped;
            }
            else
            {
                ++g_fz.runs;
                const std::vector<uint8_t> start(rdram + S, rdram + S + kSpan);
                uint8_t randStart[16];
                std::memcpy(randStart, rdram + kRandState, 16u);
                // native, journaled
                R5900Context nctx = base;
                Journal jr;
                Env nenv{rdram, runtime, &nctx, Mem{rdram, &jr}};
                nenv.gp = env.gp;
                nenv.sp = sp;
                Vu vu;
                loadVu(vu, &nctx);
                runEmitter(nenv, pl, vu);
                storeVu(vu, &nctx);
                finishCall(nenv, ra);
                const std::vector<uint8_t> nativeRoi(rdram + S, rdram + S + kSpan);
                uint8_t randNative[16];
                std::memcpy(randNative, rdram + kRandState, 16u);
                for (size_t i = jr.entries.size(); i-- > 0;)
                    std::memcpy(rdram + jr.entries[i].addr, jr.entries[i].old, jr.entries[i].len);
                // original, run to completion
                R5900Context octx = base;
                octx.pc = kEntry;
                octx.r[31] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(kSentinel)));
                g_orig(rdram, &octx, runtime);
                for (uint32_t resume = 0u; resume < 100000u && octx.pc != kSentinel && octx.pc != 0u && runtime->hasFunction(octx.pc); ++resume)
                    runtime->lookupFunction(octx.pc)(rdram, &octx, runtime);
                if (octx.pc != kSentinel)
                    ++g_fz.unwound;
                else
                {
                    uint32_t diff = 0u;
                    for (uint32_t o = 0u; o < kSpan; o += 4u)
                    {
                        // the dead stack area is not part of the clone (S is far below sp): compare everything
                        if (std::memcmp(rdram + S + o, nativeRoi.data() + o, 4u) != 0)
                        {
                            if (diff < 4u)
                            {
                                uint32_t a, b, c;
                                std::memcpy(&a, nativeRoi.data() + o, 4u);
                                std::memcpy(&b, rdram + S + o, 4u);
                                std::memcpy(&c, start.data() + o, 4u);
                                std::fprintf(stderr, "[p3fes-effects] FUZZ MISMATCH seed=%08X at S+%X: native %08X original %08X (start %08X) type=%u N=%u tc=%u C0=%u C4=%u s7=%d D=%d\n",
                                             seed, o, a, b, c, pl.type, N, tc, C0, C4, s7, D);
                            }
                            ++diff;
                        }
                    }
                    if (std::memcmp(rdram + kRandState, randNative, 16u) != 0)
                        ++diff;
                    bool regBad = false;
                    for (int reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30})
                        regBad = regBad || getRegU32(&octx, reg) != getRegU32(&base, reg);
                    regBad = regBad || getRegU32(&octx, 31) != kSentinel;
                    for (int i = 20; i < 32; ++i)
                        regBad = regBad || std::memcmp(&octx.f[i], &base.f[i], 4u) != 0;
                    if (diff != 0u || regBad)
                    {
                        ++g_fz.bad;
                        ok = false;
                        if (regBad)
                            std::fprintf(stderr, "[p3fes-effects] FUZZ REGISTER MISMATCH seed=%08X\n", seed);
                    }
                }
                std::memcpy(rdram + S, start.data(), kSpan);
                std::memcpy(rdram + kRandState, randStart, 16u);
            }
            std::memcpy(rdram + S, saved.data(), kSpan);
            std::memcpy(rdram + kRandState, randSaved, 16u);
            return ok;
        }

        uint32_t g_fuzzPerCall = 0u;

        void maybeLog()
        {
            if (!g_diag)
                return;
            static uint32_t tick = 0u; // the clock read is a system call on the console: look at it every 1024th call only
            if ((++tick & 1023u) != 0u)
                return;
            static uint64_t last = 0u;
            const uint64_t now = nowNs();
            if (now - last >= 5000000000ull)
            {
                last = now;
                logCounters();
            }
        }

        // P3FES_EFFECTS_ALTERNATE=<seconds> (measurement): see install()
        int g_alternate = 0;

        void hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_c.calls;

            if (g_mode == Mode::Off || ctx->pc != kEntry || g_mode == Mode::Log)
            {
                if (g_mode == Mode::Log && ctx->pc == kEntry)
                {
                    const uint64_t t0 = g_timeMode >= 2 ? stamp() : nowNs();
                    g_orig(rdram, ctx, runtime);
                    if (g_timeMode >= 2)
                    {
                        g_hu[0][0] += delta(t0, stamp());
                        ++g_hn[0][0];
                    }
                    else
                    {
                        g_c.ns += nowNs() - t0;
                        ++g_c.nsCalls;
                    }
                    maybeLog();
                    return;
                }
                if (ctx->pc != kEntry)
                    ++g_c.fbResumed;
                g_orig(rdram, ctx, runtime);
                return;
            }
            const uint64_t t0 = g_time ? nowNs() : 0u;
            const uint32_t ra = getRegU32(ctx, 31);
            if (g_trace)
            {
                static uint32_t seen[8];
                static uint32_t nSeen = 0u;
                bool known = false;
                for (uint32_t i = 0u; i < nSeen; ++i)
                    known = known || seen[i] == ra;
                if (!known && nSeen < 8u)
                {
                    seen[nSeen++] = ra;
                    std::fprintf(stderr, "[p3fes-effects] caller ra=%08X a0=%08X a1=%08X a2=%08X sp=%08X\n", ra, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 6), getRegU32(ctx, 29));
                }
            }
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            Plan pl;
            bool pre;
            {
                SegTimer tp(SEG_PRE);
                pre = precheck(env, getRegU32(ctx, 4), pl);
            }
            if (!pre)
            {
                ++g_c.fallback;
                g_orig(rdram, ctx, runtime);
            }
            else if (pl.early)
            {
                ++g_c.early;
                ctx->pc = ra;
                chargeCycles(runtime, 8u);
            }
            else
            {
                ++g_c.typeCalls[pl.type & 3u];
                if (g_mode == Mode::Verify)
                {
                    ++g_verifyTick;
                    if (g_c.verified < 200u || (g_verifyTick % g_verifyEvery) == 0u)
                    {
                        // the fuzz runs first: the real call below is the authoritative one
                        for (uint32_t k = 0u; k < g_fuzzPerCall; ++k)
                            fuzzOne(rdram, ctx, runtime, pl, ra, static_cast<uint32_t>(g_verifyTick * 2654435761ull) ^ (k * 0x9E3779B1u) ^ 0xA5A5u);
                        verifyCall(rdram, ctx, runtime, pl, ra);
                    }
                    else
                        g_orig(rdram, ctx, runtime);
                }
                else
                {
                    SegTimer tt(SEG_TOTAL);
                    const uint64_t th0 = g_timeMode >= 2 ? stamp() : 0u;
                    Vu vu;
                    loadVu(vu, ctx);
                    runEmitter(env, pl, vu);
                    {
                        SegTimer tpost(SEG_POST);
                        storeVu(vu, ctx);
                        finishCall(env, ra);
                        ++g_c.native;
                        chargeCycles(runtime, env.calls * 8u + env.edges * 32u);
                    }
                    if (g_timeMode >= 2)
                    {
                        g_hu[1][0] += delta(th0, stamp());
                        ++g_hn[1][0];
                    }
                }
            }
            if (g_time)
            {
                g_c.ns += nowNs() - t0;
                ++g_c.nsCalls;
            }
            maybeLog();
        }


        // ---- small RenderWare helpers of the effect drawer (0x321CE0 -> 0x34DC00) -------------------------------------------
        //
        // 0x34DC00 draws every sprite of an effect as a clump: it sets the material colours of the clump with
        // RpClumpForAllAtomics(clump, 0x34D0D0, &rgba) and builds the clump matrix with RwMatrixRotate / Scale / Translate.
        // None of these changes anything but the guest memory they are given, so they are replaced one by one:
        //   0x4916D0 RpClumpForAllAtomics  only with the callback 0x34D0D0 (rgba of the argument into the four bytes at
        //            material+4 of every material of every atomic); any other callback runs the original
        //   0x4C33D0 RwMatrixScale(M, v, op), 0x4C35D0 RwMatrixTranslate(M, v, op)   op 0 replace, 1 pre-, 2 post-concat
        // Everything else (an unusual combine op, pointers outside RAM, overlapping operands) runs the original before any
        // write.

        struct SmallStats
        {
            uint64_t calls = 0u, native = 0u, fallback = 0u, verified = 0u, verifiedFull = 0u, bad = 0u, unwound = 0u, regBad = 0u;
            uint64_t ops[4] = {};
            uint64_t tick = 0u;
            uint64_t fuzzRuns = 0u, fuzzBad = 0u;
        };
        SmallStats g_stForAll, g_stScale, g_stTranslate;
        GuestFunction g_origForAll = nullptr, g_origScale = nullptr, g_origTranslate = nullptr, g_colorCb = nullptr;
        constexpr uint32_t kForAllAtomics = 0x004916D0u, kColorCallback = 0x0034D0D0u, kMatrixScale = 0x004C33D0u, kMatrixTranslate = 0x004C35D0u;
        // all on by default: the whole set saves 0.9 M instructions per shop frame on the console (A/B inside one run); each piece
        // is verified clean on every scene (hooking a directly called function costs ~75 instructions per call, the bodies save more)
        bool g_enForAll = true, g_enScale = true, g_enTranslate = true;

        // Generic verify: the native body runs journaled on a private context, is rolled back and the original runs.
        // Compared: every byte the native body wrote (against what the original left), the whole RAM on every 32nd check,
        // callee-saved registers, $v0 and the return pc.
        template <class Native>
        void verifySmall(SmallStats &st, const char *name, GuestFunction orig, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                         uint32_t ra, Native &&native, bool compareV0 = true)
        {
            const bool full = st.verified < 16u || (st.verified % 32u) == 0u;
            if (full)
            {
                g_snapStart.resize(kRamSize);
                g_snapNative.resize(kRamSize);
            }
            uint8_t *spr = ps2GetScratchpadHostPtr();
            const R5900Context saved = *ctx;
            R5900Context nctx = *ctx;
            Journal jr;
            Env env{rdram, runtime, &nctx, Mem{rdram, &jr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            native(env, nctx);
            nctx.pc = ra;
            struct Fin { uint32_t addr, len; uint8_t data[16]; };
            std::vector<Fin> fin;
            fin.reserve(jr.entries.size());
            for (const auto &e : jr.entries)
            {
                Fin f;
                f.addr = e.addr;
                f.len = e.len;
                std::memcpy(f.data, rdram + e.addr, e.len);
                fin.push_back(f);
            }
            if (full)
                std::memcpy(g_snapNative.data(), rdram, kRamSize);
            // the full compare needs the start state: reconstruct it from the rolled back RAM below
            for (size_t i = jr.entries.size(); i-- > 0;)
                std::memcpy(rdram + jr.entries[i].addr, jr.entries[i].old, jr.entries[i].len);
            if (full)
                std::memcpy(g_snapStart.data(), rdram, kRamSize);
            orig(rdram, ctx, runtime);
            if (ctx->pc != ra)
            {
                ++st.unwound;
                return;
            }
            ++st.verified;
            bool bad = false;
            uint32_t shown = 0u;
            for (const auto &f : fin)
                if (std::memcmp(rdram + f.addr, f.data, f.len) != 0)
                {
                    if (shown++ < 4u)
                    {
                        uint32_t a, b;
                        std::memcpy(&a, f.data, 4u);
                        std::memcpy(&b, rdram + f.addr, 4u);
                        std::fprintf(stderr, "[p3fes-effects] %s WRITE MISMATCH at %08X: native %08X original %08X\n", name, f.addr, a, b);
                    }
                    bad = true;
                }
            if (full)
            {
                ++st.verifiedFull;
                const uint32_t sp = getRegU32(&saved, 29);
                const uint32_t lowGuard = (sp - 0x2000u) & PS2_RAM_MASK, highGuard = (sp + 0x10u) & PS2_RAM_MASK;
                for (uint32_t base = 0u; base < kRamSize; base += 4096u)
                {
                    if (std::memcmp(g_snapNative.data() + base, rdram + base, 4096u) == 0)
                        continue;
                    for (uint32_t i = base; i < base + 4096u; i += 4u)
                        if (!(i >= lowGuard && i < highGuard) && std::memcmp(g_snapNative.data() + i, rdram + i, 4u) != 0)
                        {
                            if (shown++ < 6u)
                            {
                                uint32_t a, b, c;
                                std::memcpy(&a, g_snapNative.data() + i, 4u);
                                std::memcpy(&b, rdram + i, 4u);
                                std::memcpy(&c, g_snapStart.data() + i, 4u);
                                std::fprintf(stderr, "[p3fes-effects] %s RAM MISMATCH at %08X: native %08X original %08X (start %08X)\n", name, i, a, b, c);
                            }
                            bad = true;
                        }
                }
            }
            (void)spr; // the native bodies only touch plain RAM
            bool regBad = (compareV0 && getRegU32(ctx, 2) != getRegU32(&nctx, 2)) || ctx->fcr31 != nctx.fcr31;
            for (int reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
                regBad = regBad || getRegU32(ctx, reg) != getRegU32(&saved, reg);
            for (int i = 20; i < 32; ++i)
                regBad = regBad || std::memcmp(&ctx->f[i], &saved.f[i], 4u) != 0;
            if (regBad)
            {
                ++st.regBad;
                bad = true;
            }
            if (bad)
            {
                ++st.bad;
                std::fprintf(stderr, "[p3fes-effects] %s VERIFY MISMATCH (a0=%08X a1=%08X a2=%08X)\n", name, getRegU32(&saved, 4), getRegU32(&saved, 5), getRegU32(&saved, 6));
            }
        }

        inline void setV0(R5900Context *ctx, uint32_t value) { ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value))); }

        void fuzzForAll(SmallStats &st, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra, uint32_t seed);

        // ---- RpClumpForAllAtomics(clump, 0x34D0D0, rgba) ------------------------------------------------------------------
        struct ForAllPlan
        {
            uint32_t clump = 0u, arg = 0u, atoms = 0u, mats = 0u;
        };
        bool forAllPrecheck(const Env &env, uint32_t clump, uint32_t arg, ForAllPlan &pl)
        {
            const Mem &m = env.m;
            if (!rangeOk(clump, 0x10u) || !rangeOk(arg, 4u))
                return false;
            pl.clump = clump;
            pl.arg = arg;
            const uint32_t head = clump + 8u;
            uint32_t node = m.r32(head);
            // ranges the writes must not touch: the list head, every atomic node, its geometry and material array, the colour
            uint32_t seen[3 * 64 + 2][2];
            uint32_t nSeen = 0u;
            seen[nSeen][0] = clump, seen[nSeen++][1] = 0x10u;
            seen[nSeen][0] = arg, seen[nSeen++][1] = 4u;
            while (node != head)
            {
                if (pl.atoms >= 64u || (node & 3u) != 0u || !rangeOk(node - 0x40u, 0x48u))
                    return false;
                const uint32_t atomic = node - 0x40u;
                const uint32_t geom = m.r32(atomic + 0x18u);
                if (!rangeOk(geom, 0x28u) || (geom & 3u) != 0u)
                    return false;
                const int32_t n = m.ri32(geom + 0x24u);
                const uint32_t arr = m.r32(geom + 0x20u);
                if (n < 0 || n > 256 || (n > 0 && (!rangeOk(arr, static_cast<uint32_t>(n) * 4u) || (arr & 3u) != 0u)))
                    return false;
                seen[nSeen][0] = atomic, seen[nSeen++][1] = 0x48u;
                seen[nSeen][0] = geom, seen[nSeen++][1] = 0x28u;
                seen[nSeen][0] = arr, seen[nSeen++][1] = static_cast<uint32_t>(n) * 4u;
                pl.atoms++;
                pl.mats += static_cast<uint32_t>(n);
                if (pl.mats > 4096u)
                    return false;
                // the materials are checked against every range seen so far and the ones still to come: do a second pass
                node = m.r32(node);
            }
            // second pass: every material block (+4..+8) must be in RAM and outside all the ranges
            node = m.r32(head);
            for (uint32_t a = 0u; a < pl.atoms; ++a)
            {
                const uint32_t atomic = node - 0x40u;
                const uint32_t geom = m.r32(atomic + 0x18u);
                const uint32_t n = m.r32(geom + 0x24u);
                const uint32_t arr = m.r32(geom + 0x20u);
                for (uint32_t i = 0u; i < n; ++i)
                {
                    const uint32_t mat = m.r32(arr + 4u * i);
                    if (!rangeOk(mat, 8u))
                        return false;
                    for (uint32_t k = 0u; k < nSeen; ++k)
                        if (overlap(mat + 4u, 4u, seen[k][0], seen[k][1]))
                            return false;
                }
                node = m.r32(node);
            }
            return true;
        }

        // every material pointer of every atomic: rgba of the argument into material+4
        void forAllRun(Env &env, const ForAllPlan &pl)
        {
            Mem &m = env.m;
            const uint32_t color = m.r32(pl.arg);
            const uint32_t head = pl.clump + 8u;
            uint32_t node = m.r32(head);
            env.calls += 1u;
            for (uint32_t a = 0u; a < pl.atoms; ++a)
            {
                const uint32_t atomic = node - 0x40u;
                const uint32_t next = m.r32(node);
                const uint32_t geom = m.r32(atomic + 0x18u);
                const uint32_t n = m.r32(geom + 0x24u);
                const uint32_t arr = m.r32(geom + 0x20u);
                for (uint32_t i = 0u; i < n; ++i)
                    m.w32(m.r32(arr + 4u * i) + 4u, color);
                env.calls += 1u;
                env.edges += n > 0u ? n - 1u : 0u;
                node = next;
            }
            env.edges += pl.atoms > 0u ? pl.atoms - 1u : 0u;
            setV0(env.ctx, pl.clump);
            env.ctx->pc = 0u; // the caller sets the return pc
        }

        void forAllHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_stForAll.calls;
            maybeLog();
            if (!g_enForAll || g_mode == Mode::Off || ctx->pc != kForAllAtomics || getRegU32(ctx, 5) != kColorCallback ||
                runtime->lookupFunction(kColorCallback) != g_colorCb)
            {
                g_origForAll(rdram, ctx, runtime);
                return;
            }
            const uint64_t t0 = g_timeMode >= 2 ? stamp() : 0u;
            if (g_mode == Mode::Log)
            {
                g_origForAll(rdram, ctx, runtime);
                if (g_timeMode >= 2)
                {
                    g_hu[0][1] += delta(t0, stamp());
                    ++g_hn[0][1];
                }
                return;
            }
            const uint32_t ra = getRegU32(ctx, 31);
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            ForAllPlan pl;
            if (!forAllPrecheck(env, getRegU32(ctx, 4), getRegU32(ctx, 6), pl))
            {
                ++g_stForAll.fallback;
                g_origForAll(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Verify)
            {
                ++g_stForAll.tick;
                if ((g_stForAll.tick % g_verifyEvery) == 0u)
                {
                    for (uint32_t k = 0u; k < g_fuzzPerCall; ++k)
                        fuzzForAll(g_stForAll, rdram, ctx, runtime, ra, static_cast<uint32_t>(g_verifyTick * 2654435761ull) ^ (k * 0x85EBCA6Bu));
                    verifySmall(g_stForAll, "ForAllAtomics", g_origForAll, rdram, ctx, runtime, ra,
                                [&](Env &e, R5900Context &) { forAllRun(e, pl); });
                    return;
                }
                g_origForAll(rdram, ctx, runtime);
                return;
            }
            forAllRun(env, pl);
            ctx->pc = ra;
            ++g_stForAll.native;
            chargeCycles(runtime, env.calls * 8u + env.edges * 32u);
            if (g_timeMode >= 2)
            {
                g_hu[1][1] += delta(t0, stamp());
                ++g_hn[1][1];
            }
        }

        // ---- RwMatrixScale / RwMatrixTranslate ------------------------------------------------------------------------
        // the matrix: right (0), flags (0xC), up (0x10), at (0x20), pos (0x30)
        inline bool matrixArgsOk(const Env &env, uint32_t M, uint32_t V, uint32_t op)
        {
            (void)env;
            return op <= 2u && rangeOk(M, 0x40u) && rangeOk(V, 12u) && (M & 3u) == 0u && (V & 3u) == 0u && !overlap(M, 0x40u, V, 12u);
        }

        void scaleRun(Env &env, uint32_t M, uint32_t V, uint32_t op)
        {
            Mem &m = env.m;
            const float f3 = m.rf(V), f2 = m.rf(V + 4u), f1 = m.rf(V + 8u);
            const uint32_t flags = m.r32(M + 0xCu);
            if (op == 0u)
            {
                const uint32_t zero = 0u;
                // 0x4C33FC: identity with the scale on the diagonal; the flags get |0x20003 first and the tail then clears them
                m.wf(M + 0x00u, f3);
                m.w32(M + 0x04u, zero);
                m.w32(M + 0x08u, zero);
                m.w32(M + 0x10u, zero);
                m.wf(M + 0x14u, f2);
                m.w32(M + 0x18u, zero);
                m.w32(M + 0x20u, zero);
                m.w32(M + 0x24u, zero);
                m.wf(M + 0x28u, f1);
                m.w32(M + 0x30u, zero);
                m.w32(M + 0x34u, zero);
                m.w32(M + 0x38u, zero);
                m.w32(M + 0xCu, (flags | 0x20003u) & 0xFFFDFFFCu);
            }
            else
            {
                if (op == 1u)
                {
                    m.wf(M + 0x00u, FPU_MUL_S(m.rf(M + 0x00u), f3));
                    m.wf(M + 0x04u, FPU_MUL_S(m.rf(M + 0x04u), f3));
                    m.wf(M + 0x08u, FPU_MUL_S(m.rf(M + 0x08u), f3));
                    m.wf(M + 0x10u, FPU_MUL_S(m.rf(M + 0x10u), f2));
                    m.wf(M + 0x14u, FPU_MUL_S(m.rf(M + 0x14u), f2));
                    m.wf(M + 0x18u, FPU_MUL_S(m.rf(M + 0x18u), f2));
                    m.wf(M + 0x20u, FPU_MUL_S(m.rf(M + 0x20u), f1));
                    m.wf(M + 0x24u, FPU_MUL_S(m.rf(M + 0x24u), f1));
                    m.wf(M + 0x28u, FPU_MUL_S(m.rf(M + 0x28u), f1));
                }
                else
                {
                    for (uint32_t row = 0u; row < 4u; ++row)
                    {
                        const uint32_t r = M + row * 0x10u + (row == 3u ? 0u : 0u);
                        m.wf(r + 0x00u, FPU_MUL_S(m.rf(r + 0x00u), f3));
                        m.wf(r + 0x04u, FPU_MUL_S(m.rf(r + 0x04u), f2));
                        m.wf(r + 0x08u, FPU_MUL_S(m.rf(r + 0x08u), f1));
                    }
                }
                m.w32(M + 0xCu, flags & 0xFFFDFFFCu);
            }
        }

        void translateRun(Env &env, uint32_t M, uint32_t V, uint32_t op)
        {
            Mem &m = env.m;
            const uint32_t flags = m.r32(M + 0xCu);
            if (op == 0u)
            {
                const uint32_t zero = 0u;
                const float tx = m.rf(V), ty = m.rf(V + 4u), tz = m.rf(V + 8u);
                m.wf(M + 0x00u, 1.0f);
                m.w32(M + 0x04u, zero);
                m.w32(M + 0x08u, zero);
                m.w32(M + 0x10u, zero);
                m.wf(M + 0x14u, 1.0f);
                m.w32(M + 0x18u, zero);
                m.w32(M + 0x20u, zero);
                m.w32(M + 0x24u, zero);
                m.wf(M + 0x28u, 1.0f);
                m.wf(M + 0x30u, tx);
                m.wf(M + 0x34u, ty);
                m.wf(M + 0x38u, tz);
                m.w32(M + 0xCu, (flags | 0x20003u) & 0xFFFDFFFFu);
            }
            else
            {
                if (op == 1u)
                {
                    const float f4 = m.rf(V + 4u), f5 = m.rf(V), f6 = m.rf(V + 8u);
                    for (uint32_t k = 0u; k < 3u; ++k)
                    {
                        const float f1 = m.rf(M + 0x10u + 4u * k), f2 = m.rf(M + 4u * k), f3 = m.rf(M + 0x20u + 4u * k);
                        const float f0 = m.rf(M + 0x30u + 4u * k);
                        float acc = FPU_MUL_S(f4, f1);
                        acc = FPU_ADD_S(acc, FPU_MUL_S(f5, f2));
                        const float r1 = FPU_ADD_S(acc, FPU_MUL_S(f6, f3));
                        m.wf(M + 0x30u + 4u * k, FPU_ADD_S(f0, r1));
                    }
                }
                else
                {
                    const float f0 = m.rf(V), f2 = m.rf(V + 4u), f1 = m.rf(V + 8u);
                    m.wf(M + 0x30u, FPU_ADD_S(m.rf(M + 0x30u), f0));
                    m.wf(M + 0x34u, FPU_ADD_S(m.rf(M + 0x34u), f2));
                    m.wf(M + 0x38u, FPU_ADD_S(m.rf(M + 0x38u), f1));
                }
                m.w32(M + 0xCu, flags & 0xFFFDFFFFu);
            }
        }


        // synthetic calls of the small helpers (random matrices with special floats, every combine op)
        void fuzzMatrix(bool scale, SmallStats &st, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra, uint32_t seed)
        {
            Rng rng{seed ? seed : 1u};
            const uint32_t sp = getRegU32(ctx, 29);
            const uint32_t S = ((sp - 0x60000u) & ~0xFFu) + 0x8000u;
            if (S < 0x200000u)
                return;
            uint8_t saved[0x100];
            std::memcpy(saved, rdram + S, sizeof(saved));
            Mem m{rdram, nullptr};
            const uint32_t M = S, V = S + 0x80u;
            for (uint32_t o = 0u; o < 0x40u; o += 4u)
                m.wf(M + o, rng.any());
            m.w32(M + 0xCu, rng.next());
            for (uint32_t o = 0u; o < 12u; o += 4u)
                m.wf(V + o, rng.any());
            const uint32_t op = rng.below(3u);
            R5900Context fctx = *ctx;
            fctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(M));
            fctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(V));
            fctx.r[6] = _mm_set_epi64x(0, static_cast<int64_t>(op));
            ++st.fuzzRuns;
            const uint64_t before = st.bad;
            auto run = [&](Env &e, R5900Context &c)
            {
                if (scale)
                    scaleRun(e, M, V, op);
                else
                    translateRun(e, M, V, op);
                setV0(&c, M);
            };
            verifySmall(st, scale ? "RwMatrixScale(fuzz)" : "RwMatrixTranslate(fuzz)", scale ? g_origScale : g_origTranslate, rdram, &fctx, runtime, ra, run);
            if (st.bad != before)
                ++st.fuzzBad;
            std::memcpy(rdram + S, saved, sizeof(saved));
        }

        void fuzzForAll(SmallStats &st, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra, uint32_t seed)
        {
            Rng rng{seed ? seed : 1u};
            const uint32_t sp = getRegU32(ctx, 29);
            const uint32_t S = ((sp - 0x60000u) & ~0xFFu) + 0xA000u;
            if (S < 0x200000u)
                return;
            uint8_t saved[0x2000];
            std::memcpy(saved, rdram + S, sizeof(saved));
            Mem m{rdram, nullptr};
            const uint32_t clump = S, color = S + 0x40u;
            m.w32(color, rng.next());
            const uint32_t atoms = rng.below(4u);
            uint32_t prev = clump + 8u;
            for (uint32_t a = 0u; a < atoms; ++a)
            {
                const uint32_t atomic = S + 0x100u + a * 0x200u, node = atomic + 0x40u, geom = atomic + 0x80u, arr = atomic + 0xC0u;
                const uint32_t n = rng.below(5u);
                m.w32(prev, node);
                m.w32(atomic + 0x18u, geom);
                m.w32(geom + 0x24u, n);
                m.w32(geom + 0x20u, arr);
                for (uint32_t i = 0u; i < n; ++i)
                    m.w32(arr + 4u * i, atomic + 0x100u + i * 0x10u);
                prev = node;
            }
            m.w32(prev, clump + 8u);
            if (atoms == 0u)
                m.w32(clump + 8u, clump + 8u);
            R5900Context fctx = *ctx;
            fctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(clump));
            fctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(kColorCallback));
            fctx.r[6] = _mm_set_epi64x(0, static_cast<int64_t>(color));
            Env env{rdram, runtime, &fctx, Mem{rdram, nullptr}};
            ForAllPlan pl;
            if (!forAllPrecheck(env, clump, color, pl))
                return;
            ++st.fuzzRuns;
            const uint64_t before = st.bad;
            verifySmall(st, "ForAllAtomics(fuzz)", g_origForAll, rdram, &fctx, runtime, ra, [&](Env &e, R5900Context &) { forAllRun(e, pl); });
            if (st.bad != before)
                ++st.fuzzBad;
            std::memcpy(rdram + S, saved, sizeof(saved));
        }


        // ---- RwMatrixRotate(M, axis, angle in degrees, op) with op 0 (replace) ----------------------------------------------
        // 0x4C31B0 builds the axis-angle matrix with sinf/cosf and 0x4C2FC0 (RotateOneMinusCosineSine). For op 0 the second
        // routine fills a temporary matrix on its stack (three pad words of it are never written: stale stack words that are
        // copied into the pads of M, read here from the same stack addresses) and copies it into M; other ops multiply and
        // run the original. sinf/cosf must be the host functions (a huge angle or the guest versions run the original).
        void fuzzRotate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra, uint32_t seed);
        constexpr uint32_t kMatrixRotate = 0x004C31B0u;
        SmallStats g_stRotate;
        GuestFunction g_origRotate = nullptr;
        bool g_enRotate = true;

        inline bool rotateArgs(Env &env, uint32_t M, uint32_t axis, uint64_t op, float angleDeg)
        {
            if (op != 0u || !rangeOk(M, 0x40u) || !rangeOk(axis, 12u) || ((M | axis) & 3u) != 0u || overlap(M, 0x40u, axis, 12u) ||
                !rangeOk(env.sp - 0x200u, 0x100u))
                return false;
            if (g_sinMode == 0)
                probeSin(env);
            if (g_cosMode == 0)
                probeCos(env);
            if (g_sinMode != 1 || g_cosMode != 1)
                return false;
            const float rad = FPU_MUL_S(bitsf(0x3C8EFA35u), angleDeg);
            return nativeSinArg(rad);
        }

        void rotateRun(Env &env, uint32_t M, uint32_t axis, float angleDeg)
        {
            Mem m = env.m;
            R5900Context *ctx = env.ctx;
            const float one = 1.0f;
            const float f20 = FPU_MUL_S(bitsf(0x3C8EFA35u), angleDeg);
            const float y0 = m.rf(axis + 4u), x0 = m.rf(axis), z0 = m.rf(axis + 8u);
            float acc = FPU_MUL_S(y0, y0);
            acc = FPU_ADD_S(acc, FPU_MUL_S(x0, x0));
            const float len2 = FPU_ADD_S(acc, FPU_MUL_S(z0, z0));
            float f3 = ps2FpuSqrt(ctx->fcr31, len2);
            if (!cLe(env, f3, 0.0f))
                f3 = ps2FpuDivide(ctx->fcr31, one, f3);
            const float x = FPU_MUL_S(x0, f3), y = FPU_MUL_S(y0, f3), z = FPU_MUL_S(z0, f3);
            env.calls += 3u; // sinf, cosf, 0x4C2FC0
            const float sn = hostSinf(f20);
            const float cs = hostCosf(f20);
            const float omc = FPU_SUB_S(one, cs);
            const float acc1 = addZero(one);
            auto diag = [&](float v) { return FPU_SUB_S(one, FPU_MUL_S(FPU_SUB_S(acc1, FPU_MUL_S(v, v)), omc)); };
            const float m00 = diag(x), m11 = diag(y), m22 = diag(z);
            const float f3x = FPU_MUL_S(x, sn);
            const float f6 = FPU_MUL_S(FPU_MUL_S(y, z), omc);
            const float f5 = FPU_MUL_S(FPU_MUL_S(z, x), omc);
            const float f0 = FPU_MUL_S(z, sn);
            const float f4 = FPU_MUL_S(FPU_MUL_S(x, y), omc);
            const float f2 = FPU_MUL_S(y, sn);
            // stale words of the temporary matrix: sp(entry) - 0x50 (0x4C31B0 frame) - 0xB0 (0x4C2FC0 frame) + 0x60 + word
            const uint32_t tmp = env.sp - 0x50u - 0xB0u + 0x60u;
            const uint32_t stale7 = m.r32(tmp + 0x1Cu), stale11 = m.r32(tmp + 0x2Cu), stale15 = m.r32(tmp + 0x3Cu);
            m.wf(M + 0x00u, m00);
            m.wf(M + 0x04u, FPU_ADD_S(f4, f0));
            m.wf(M + 0x08u, FPU_SUB_S(f5, f2));
            m.w32(M + 0x0Cu, 3u);
            m.wf(M + 0x10u, FPU_SUB_S(f4, f0));
            m.wf(M + 0x14u, m11);
            m.wf(M + 0x18u, FPU_ADD_S(f6, f3x));
            m.w32(M + 0x1Cu, stale7);
            m.wf(M + 0x20u, FPU_ADD_S(f5, f2));
            m.wf(M + 0x24u, FPU_SUB_S(f6, f3x));
            m.wf(M + 0x28u, m22);
            m.w32(M + 0x2Cu, stale11);
            m.w32(M + 0x30u, 0u);
            m.w32(M + 0x34u, 0u);
            m.w32(M + 0x38u, 0u);
            m.w32(M + 0x3Cu, stale15);
            env.edges += 7u;
            if (env.cSet)
                ctx->fcr31 = env.cVal ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
        }

        void rotateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_stRotate.calls;
            maybeLog();
            if (!g_enRotate || g_mode == Mode::Off || ctx->pc != kMatrixRotate)
            {
                g_origRotate(rdram, ctx, runtime);
                return;
            }
            const uint64_t t0 = g_timeMode >= 2 ? stamp() : 0u;
            const uint32_t M = getRegU32(ctx, 4), axis = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            const float angle = ctx->f[12];
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            if (g_mode == Mode::Log || !rotateArgs(env, M, axis, GPR_U64(ctx, 6), angle))
            {
                if (g_mode != Mode::Log)
                    ++g_stRotate.fallback;
                g_origRotate(rdram, ctx, runtime);
                if (g_mode == Mode::Log && g_timeMode >= 2)
                {
                    g_hu[0][4] += delta(t0, stamp());
                    ++g_hn[0][4];
                }
                return;
            }
            if (g_mode == Mode::Verify)
            {
                ++g_stRotate.tick;
                if ((g_stRotate.tick % g_verifyEvery) == 0u)
                {
                    for (uint32_t k = 0u; k < g_fuzzPerCall; ++k)
                        fuzzRotate(rdram, ctx, runtime, ra, static_cast<uint32_t>(g_verifyTick * 2246822519ull) ^ (k * 0x9E3779B1u));
                    verifySmall(g_stRotate, "RwMatrixRotate", g_origRotate, rdram, ctx, runtime, ra, [&](Env &e, R5900Context &c)
                                {
                                    rotateRun(e, M, axis, angle);
                                    setV0(&c, M);
                                });
                    return;
                }
                g_origRotate(rdram, ctx, runtime);
                return;
            }
            rotateRun(env, M, axis, angle);
            setV0(ctx, M);
            ctx->pc = ra;
            ++g_stRotate.native;
            chargeCycles(runtime, env.calls * 8u + env.edges * 32u);
            if (g_timeMode >= 2)
            {
                g_hu[1][4] += delta(t0, stamp());
                ++g_hn[1][4];
            }
        }

        void fuzzRotate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t ra, uint32_t seed)
        {
            Rng rng{seed ? seed : 1u};
            const uint32_t sp = getRegU32(ctx, 29);
            const uint32_t S = ((sp - 0x60000u) & ~0xFFu) + 0xE000u;
            if (S < 0x200000u)
                return;
            uint8_t saved[0x100];
            std::memcpy(saved, rdram + S, sizeof(saved));
            Mem m{rdram, nullptr};
            const uint32_t M = S, axis = S + 0x80u;
            for (uint32_t o = 0u; o < 0x40u; o += 4u)
                m.wf(M + o, rng.any());
            for (uint32_t o = 0u; o < 12u; o += 4u)
                m.wf(axis + o, rng.below(5u) == 0u ? rng.any() : rng.flt(-1.0f, 1.0f));
            if (rng.below(8u) == 0u)
                for (uint32_t o = 0u; o < 12u; o += 4u)
                    m.wf(axis + o, 0.0f);
            const float angle = rng.below(4u) == 0u ? rng.any() : rng.flt(-720.0f, 720.0f);
            R5900Context fctx = *ctx;
            fctx.r[4] = _mm_set_epi64x(0, static_cast<int64_t>(M));
            fctx.r[5] = _mm_set_epi64x(0, static_cast<int64_t>(axis));
            fctx.r[6] = _mm_set_epi64x(0, 0);
            fctx.f[12] = angle;
            fctx.fcr31 = rng.next() & 0x00FFFFFFu & ~0x0u;
            Env env{rdram, runtime, &fctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = sp;
            if (!rotateArgs(env, M, axis, 0u, angle))
            {
                std::memcpy(rdram + S, saved, sizeof(saved));
                return;
            }
            ++g_stRotate.fuzzRuns;
            const uint64_t before = g_stRotate.bad;
            verifySmall(g_stRotate, "RwMatrixRotate(fuzz)", g_origRotate, rdram, &fctx, runtime, ra, [&](Env &e, R5900Context &c)
                        {
                            rotateRun(e, M, axis, angle);
                            setV0(&c, M);
                        });
            if (g_stRotate.bad != before)
            {
                ++g_stRotate.fuzzBad;
            }
            std::memcpy(rdram + S, saved, sizeof(saved));
        }


        // ---- 0x3275D0, draw variant 1 without local space: effect particles -> the sky pipeline's vertex streams ------------
        //
        // The draw object D ([D+0x4C] = the effect's emitter block, [D+0x50] = the pipeline object, [D+0xC] = variant) is
        // drawn by 0x3275D0 through a jump table. Variant 1 with the local-space flag clear does: 0x477050 (advance the ring
        // of stream descriptors of the pipeline object), four 0x474640 (position / colour / size / angle stream: base and
        // stride from the descriptors, each also ORs a flag into the object), then for every live slot of the emitter the
        // position, colour word, normalised angle (the slot's angle is written back) and size*scale are stored into the
        // four streams, 0x4747F0 (merges the flag words) and, when something was drawn, the material colour, a flag, the
        // vertex count and the draw is queued (0x323860 -> 0x321140: a singly linked list per layer). All of these
        // callees are small and pure on guest memory, so they are inlined; nothing here calls the guest.
        constexpr uint32_t kDraw = 0x003275D0u;
        SmallStats g_stDraw;
        GuestFunction g_origDraw = nullptr;
        bool g_enDraw = true;

        struct DrawPlan
        {
            uint32_t D = 0u, s3 = 0u, P = 0u, s1 = 0u, s6 = 0u, B = 0u, R = 0u, M = 0u;
            uint32_t N = 0u, live = 0u;
            bool ring = false;       // 0x477050 advances the ring
            int32_t newIdx = 0;
            bool copyEntries = false;
            uint32_t ent[9] = {};
            uint32_t ptr[4] = {}, stride[4] = {}; // position, colour, size, angle streams
            uint32_t k4580 = 0u;
            uint32_t listSlot = 0u;
            uint32_t flagBits = 0u;
            float f3 = 0.0f, f4 = 0.0f, f2 = 0.0f, scale = 0.0f;
        };

        inline bool dfail(int) { return false; } // (print the line number here to see why a precheck refuses)

        bool drawPrecheck(Env &env, uint32_t D, float scale, DrawPlan &pl)
        {
            const Mem &m = env.m;
            const uint32_t gp = env.gp;
            if (!rangeOk(D, 0x60u) || (D & 3u) != 0u || !rangeOk(gp - 0x7F40u, 0x100u) || !rangeOk(gp - 0x4600u, 0x500u))
                return dfail(__LINE__);
            pl.D = D;
            pl.scale = scale;
            pl.s3 = m.r32(D + 0x4Cu);
            if (!rangeOk(pl.s3, 0x30u) || (pl.s3 & 3u) != 0u)
                return dfail(__LINE__);
            const int32_t n = m.ri32(pl.s3 + 8u);
            if (n <= 0 || n > 0x1000 || m.r32(pl.s3 + 0x10u) == 0u)
                return dfail(__LINE__); // the original's early exits
            pl.N = static_cast<uint32_t>(n);
            pl.P = m.r32(pl.s3 + 0x18u);
            const uint32_t pBytes = pl.N * 0x20u;
            if (!rangeOk(pl.P, pBytes) || (pl.P & 3u) != 0u)
                return dfail(__LINE__);
            uint16_t variant;
            std::memcpy(&variant, env.rdram + D + 0xCu, 2u);
            if (variant != 1u || (m.r32(pl.s3 + 0xCu) & 1u) != 0u)
                return dfail(__LINE__);
            const uint32_t pipe = m.r32(D + 0x50u);
            if (!rangeOk(pipe, 0x20u) || (pipe & 3u) != 0u)
                return dfail(__LINE__);
            pl.s1 = m.r32(pipe + 0x10u);
            pl.s6 = m.r32(pl.s3 + 0x20u);
            pl.k4580 = m.r32(gp - 0x4580u);
            if (!rangeOk(pl.s6, 0x60u) || !rangeOk(pl.s1 + pl.k4580, 4u) || pl.s1 + pl.k4580 < pl.s1)
                return dfail(__LINE__);
            pl.B = m.r32(pl.s1 + pl.k4580);
            if (!rangeOk(pl.B, 0x100u) || (pl.B & 3u) != 0u)
                return dfail(__LINE__);
            pl.R = m.r32(pl.B + 0x9Cu);
            if (!rangeOk(pl.R, 0x400u) || (pl.R & 3u) != 0u)
                return dfail(__LINE__);
            // 0x477050 (virtual)
            pl.ring = m.r32(pl.R + 0xD8u) != 0u;
            if (pl.ring)
            {
                const int32_t idx = m.ri32(pl.R + 0xE4u), cnt = m.ri32(pl.R + 0xE8u);
                pl.newIdx = idx < cnt ? idx + 1 : 0;
                if (cnt != 0)
                {
                    pl.copyEntries = true;
                    if (pl.newIdx < 0 || pl.newIdx > 0x1000 || !rangeOk(pl.R + 0x140u + 36u * static_cast<uint32_t>(pl.newIdx), 36u))
                        return dfail(__LINE__);
                    for (uint32_t k = 0u; k < 3u; ++k)
                        for (uint32_t j = 0u; j < 3u; ++j)
                            pl.ent[k * 3u + j] = m.r32(pl.R + 0x140u + 36u * static_cast<uint32_t>(pl.newIdx) + 12u * k + 4u * j);
                }
            }
            // 0x474640 x4 (virtual): the descriptors as they will be after the ring advance
            auto bv = [&](uint32_t off) -> uint32_t
            {
                if (pl.copyEntries)
                    for (uint32_t k = 0u; k < 3u; ++k)
                        for (uint32_t j = 0u; j < 3u; ++j)
                            if (off == 0x54u + 0x18u * k + 8u * j)
                                return pl.ent[k * 3u + j];
                return m.r32(pl.B + off);
            };
            // entries land at B+0x54+0x18k, +0x5C+0x18k, +0x64+0x18k
            static const uint32_t kMask[4] = {1u, 2u, 4u, 0x20u};
            // 0x474640 reads the stream (base, stride) pairs at descriptor+4/+8 (id 1), +0x24/+0x28 (2), +0x1C/+0x20 (4), +0x34/+0x38 (0x20);
            // the descriptor is B+0x50
            static const uint32_t kBase[4] = {0x54u, 0x74u, 0x6Cu, 0x84u};
            const uint32_t avail = m.r32(pl.B + 0xA8u), over = m.r32(pl.B + 0xA4u);
            for (int i = 0; i < 4; ++i)
            {
                // 0x474640: a stream the pipeline object does not have, or one without a base, comes back as base 0 / stride
                // 0 and the loop of 0x3275D0 stores through it all the same (to the words at address 0); only a real one
                // marks its flag
                bool present = (avail & kMask[i]) != 0u;
                pl.ptr[i] = present ? bv(kBase[i]) : 0u;
                pl.stride[i] = present ? bv(kBase[i] + 4u) : 0u;
                if (present && over != 0u)
                    pl.stride[i] = over;
                if (pl.ptr[i] == 0u)
                {
                    present = false;
                    pl.stride[i] = 0u;
                }
                if (present)
                    pl.flagBits |= 0x40000000u | kMask[i];
            }
            // live slots and their angles; the angle loops need a sane step
            pl.f3 = m.rf(gp - 0x7EA0u);
            pl.f4 = m.rf(gp - 0x7E98u);
            pl.f2 = m.rf(gp - 0x7E88u);
            const uint32_t stepBits = fbits(pl.f3);
            if (stepBits < 0x3F800000u || stepBits > 0x46000000u) // 1.0 .. 8192.0
                return dfail(__LINE__);
            uint32_t live = 0u;
            for (uint32_t i = 0u; i < pl.N; ++i)
            {
                const uint32_t slot = pl.P + 0x20u * i;
                if (m.ri32(slot + 0x10u) < 0)
                    continue;
                ++live;
                if ((m.r32(slot + 0x1Cu) & 0x7FFFFFFFu) > 0x447A0000u) // |angle| <= 1000 (also excludes exponent 255)
                    return dfail(__LINE__);
            }
            pl.live = live;
            // streams: 4-aligned, small positive stride, inside RAM, away from everything that is read afterwards
            struct Rg { uint32_t a, n; };
            // (R is only read before the first stream store, exactly as in the original: no entry for it)
            Rg avoid[6] = {{pl.P, pBytes}, {D, 0x60u}, {pl.s3, 0x30u}, {pl.s6, 0x60u}, {pl.B, 0x100u}, {pl.s1 + pl.k4580, 4u}};
            for (int i = 0; i < 4; ++i)
            {
                if (pl.ptr[i] == 0u)
                    continue; // the stores land on the words at address 0 (stride 0): nothing else is touched
                if ((pl.ptr[i] & 3u) != 0u || pl.stride[i] == 0u || pl.stride[i] > 0x400u || (pl.stride[i] & 3u) != 0u)
                    return dfail(__LINE__);
                const uint32_t len = pl.stride[i] * (live ? live : 1u) + 16u;
                if (!rangeOk(pl.ptr[i], len))
                    return dfail(__LINE__);
                for (const Rg &r : avoid)
                    if (overlap(pl.ptr[i], len, r.a, r.n))
                        return dfail(__LINE__);
            }
            // streams at address 0 must not alias the data the loop reads either
            for (const Rg &r : avoid)
                if (overlap(0u, 16u, r.a, r.n))
                    return dfail(__LINE__);
            if (live != 0u)
            {
                const uint32_t v = m.r32(pl.s1 + 0x18u);
                if (!rangeOk(v, 0x24u) || (v & 3u) != 0u)
                    return dfail(__LINE__);
                const uint32_t w = m.r32(v + 0x20u);
                if (!rangeOk(w, 4u) || (w & 3u) != 0u)
                    return dfail(__LINE__);
                pl.M = m.r32(w);
                const uint32_t node = m.r32(D + 0x50u);
                uint16_t layer, idx;
                std::memcpy(&layer, env.rdram + gp - 0x62C8u, 2u);
                std::memcpy(&idx, env.rdram + pl.s6 + 0x54u, 2u);
                pl.listSlot = 0x957920u + static_cast<uint32_t>(layer) * 28u + static_cast<uint32_t>(idx) * 4u;
                if (!rangeOk(pl.M, 8u) || !rangeOk(pl.listSlot, 4u) || !rangeOk(node, 0x20u) || overlap(pl.listSlot, 4u, node + 0x18u, 4u) ||
                    overlap(pl.M + 4u, 4u, pl.listSlot, 4u) || overlap(pl.M, 8u, pl.B, 0x100u) ||
                    overlap(pl.listSlot, 4u, pl.B, 0x100u))
                    return dfail(__LINE__);
                for (int i = 0; i < 4; ++i)
                    if (pl.ptr[i] != 0u && (overlap(pl.ptr[i], pl.stride[i] * live + 16u, pl.M, 8u) || overlap(pl.ptr[i], pl.stride[i] * live + 16u, pl.listSlot, 4u) ||
                        overlap(pl.ptr[i], pl.stride[i] * live + 16u, node, 0x20u)))
                        return dfail(__LINE__);
                if (overlap(0u, 16u, pl.M, 8u) || overlap(0u, 16u, pl.listSlot, 4u) || overlap(0u, 16u, node, 0x20u))
                    return dfail(__LINE__);
            }
            return true;
        }

        void drawRun(Env &env, const DrawPlan &pl)
        {
            Mem m = env.m;
            const float f21 = FPU_MUL_S(m.rf(pl.D + 0x54u), pl.scale);
            const float f20 = FPU_MUL_S(m.rf(pl.D + 0x58u), pl.scale);
            env.calls += 8u; // 0x477050, 4 x 0x474640, 0x4747F0, 0x323860 (-> 0x321140)
            // 0x477050
            if (pl.ring)
            {
                m.w32(pl.R + 0xE4u, static_cast<uint32_t>(pl.newIdx));
                if (pl.copyEntries)
                    for (uint32_t k = 0u; k < 3u; ++k)
                    {
                        m.w32(pl.B + 0x54u + 0x18u * k, pl.ent[k * 3u + 0u]);
                        m.w32(pl.B + 0x5Cu + 0x18u * k, pl.ent[k * 3u + 1u]);
                        m.w32(pl.B + 0x64u + 0x18u * k, pl.ent[k * 3u + 2u]);
                    }
            }
            // four 0x474640: the flag word of the pipeline object collects 0x40000000 | stream id
            const uint32_t flags3C = m.r32(pl.B + 0x3Cu) | pl.flagBits;
            uint32_t p0 = pl.ptr[0], p1 = pl.ptr[1], p2 = pl.ptr[2], p3 = pl.ptr[3];
            uint32_t drawn = 0u;
            for (uint32_t i = 0u; i < pl.N; ++i)
            {
                const uint32_t slot = pl.P + 0x20u * i;
                if (m.ri32(slot + 0x10u) < 0)
                    continue;
                m.w32(p0, m.r32(slot));
                m.w32(p0 + 4u, m.r32(slot + 4u));
                m.w32(p0 + 8u, m.r32(slot + 8u));
                p0 += pl.stride[0];
                m.w32(p1, m.r32(slot + 0x14u));
                p1 += pl.stride[1];
                float f0 = m.rf(slot + 0x1Cu);
                if (cLe(env, f0, pl.f4))
                {
                    if (cLt(env, f0, pl.f2))
                        for (;;)
                        {
                            if (!cLt(env, f0, pl.f2))
                                break;
                            f0 = FPU_ADD_S(f0, pl.f3);
                            ++env.edges;
                        }
                }
                else
                    for (;;)
                    {
                        if (cLe(env, f0, pl.f4))
                            break;
                        f0 = FPU_SUB_S(f0, pl.f3);
                        ++env.edges;
                    }
                m.wf(slot + 0x1Cu, f0);
                m.wf(p3, f0);
                p3 += pl.stride[3];
                const float f1 = m.rf(slot + 0x18u);
                m.wf(p2, FPU_MUL_S(f21, f1));
                m.wf(p2 + 4u, FPU_MUL_S(f20, f1));
                p2 += pl.stride[2];
                ++drawn;
            }
            if (pl.N > 1u)
                env.edges += pl.N - 1u;
            // 0x4747F0
            m.w32(pl.B + 0x40u, m.r32(pl.B + 0x40u) | flags3C);
            m.w32(pl.B + 0x3Cu, 0u);
            if (drawn != 0u)
            {
                m.w32(pl.M + 4u, m.r32(pl.D + 4u));
                m.w32(pl.B + 0x40u, m.r32(pl.B + 0x40u) | 0x00800000u);
                m.w32(pl.B + 4u, drawn);
                const uint32_t node = m.r32(pl.D + 0x50u);
                m.w32(node + 0x18u, m.r32(pl.listSlot));
                m.w32(pl.listSlot, node);
            }
            if (env.cSet)
                env.ctx->fcr31 = env.cVal ? (env.ctx->fcr31 | 0x800000u) : (env.ctx->fcr31 & ~0x800000u);
        }

        void drawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_stDraw.calls;
            maybeLog();
            if (!g_enDraw || g_mode == Mode::Off || ctx->pc != kDraw)
            {
                g_origDraw(rdram, ctx, runtime);
                return;
            }
            const uint64_t t0 = g_timeMode >= 2 ? stamp() : 0u;
            const uint32_t ra = getRegU32(ctx, 31);
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            DrawPlan pl;
            if (g_mode == Mode::Log || !drawPrecheck(env, getRegU32(ctx, 4), ctx->f[12], pl))
            {
                if (g_mode != Mode::Log)
                    ++g_stDraw.fallback;
                g_origDraw(rdram, ctx, runtime);
                if (g_mode == Mode::Log && g_timeMode >= 2)
                {
                    g_hu[0][5] += delta(t0, stamp());
                    ++g_hn[0][5];
                }
                return;
            }
            if (g_mode == Mode::Verify)
            {
                ++g_stDraw.tick;
                if ((g_stDraw.tick % g_verifyEvery) == 0u)
                {
                    verifySmall(g_stDraw, "Draw 0x3275D0", g_origDraw, rdram, ctx, runtime, ra, [&](Env &e, R5900Context &) { drawRun(e, pl); }, false);
                    return;
                }
                g_origDraw(rdram, ctx, runtime);
                return;
            }
            drawRun(env, pl);
            ctx->pc = ra;
            ++g_stDraw.native;
            chargeCycles(runtime, env.calls * 8u + env.edges * 32u);
            if (g_timeMode >= 2)
            {
                g_hu[1][5] += delta(t0, stamp());
                ++g_hn[1][5];
            }
        }


        // ---- 0x4CB7F0 RwFrameTransform(frame, matrix, op 0) ------------------------------------------------------------------
        // op 0 copies the matrix over the frame's modelling matrix (frame+0x10, 0x40 bytes: 0x4C3760) and marks the frame
        // tree dirty (flag bits of the root at [frame+0xA0] and, if it was clean, the root joins the list headed at 0x96012C).
        constexpr uint32_t kFrameTransform = 0x004CB7F0u, kDirtyList = 0x0096012Cu;
        SmallStats g_stFrame;
        GuestFunction g_origFrame = nullptr;
        bool g_enFrame = true;

        bool frameArgs(const Env &env, uint32_t F, uint32_t M, uint64_t op)
        {
            const Mem &m = env.m;
            if (op != 0u || !rangeOk(F, 0xB0u) || !rangeOk(M, 0x40u) || ((F | M) & 3u) != 0u || !rangeOk(kDirtyList, 4u))
                return false;
            const uint32_t R = m.r32(F + 0xA0u);
            if (!rangeOk(R, 0x10u) || (R & 3u) != 0u)
                return false;
            const uint32_t X = m.r32(kDirtyList);
            if (!rangeOk(X, 8u))
                return false;
            struct Rg { uint32_t a, n; };
            // written: the copy, the root's header and the list; read afterwards: the root pointer, the list head
            const Rg r[6] = {{M, 0x40u}, {F + 0x10u, 0x40u}, {F + 0xA0u, 4u}, {kDirtyList, 4u}, {X, 8u}, {R, 0x10u}};
            for (int i = 0; i < 6; ++i)
                for (int j = i + 1; j < 6; ++j)
                    if (overlap(r[i].a, r[i].n, r[j].a, r[j].n))
                        return false;
            // the root may be the frame itself: its header (+0..+0xF) is outside the copy and the root pointer
            return true;
        }

        void frameRun(Env &env, uint32_t F, uint32_t M)
        {
            Mem m = env.m;
            env.calls += 1u;
            for (uint32_t i = 0u; i < 16u; ++i)
                m.w32(F + 0x10u + 4u * i, m.r32(M + 4u * i));
            env.edges += 7u;
            const uint32_t a1 = m.r8(m.r32(F + 0xA0u) + 3u);
            if ((a1 & 3u) == 0u)
            {
                const uint32_t R = m.r32(F + 0xA0u);
                const uint32_t X = m.r32(kDirtyList);
                m.w32(R + 8u, X);
                m.w32(m.r32(F + 0xA0u) + 0xCu, kDirtyList);
                const uint32_t v1 = m.r32(F + 0xA0u);
                m.w32(m.r32(kDirtyList) + 4u, v1 + 8u);
                m.w32(kDirtyList, m.r32(F + 0xA0u) + 8u);
            }
            m.w8(m.r32(F + 0xA0u) + 3u, static_cast<uint8_t>(a1 | 3u));
            m.w8(F + 3u, static_cast<uint8_t>(m.r8(F + 3u) | 0xCu));
        }

        void frameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_stFrame.calls;
            maybeLog();
            if (!g_enFrame || g_mode == Mode::Off || g_mode == Mode::Log || ctx->pc != kFrameTransform)
            {
                g_origFrame(rdram, ctx, runtime);
                return;
            }
            const uint32_t F = getRegU32(ctx, 4), M = getRegU32(ctx, 5), ra = getRegU32(ctx, 31);
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            env.gp = getRegU32(ctx, 28);
            env.sp = getRegU32(ctx, 29);
            if (!frameArgs(env, F, M, GPR_U64(ctx, 6)))
            {
                ++g_stFrame.fallback;
                g_origFrame(rdram, ctx, runtime);
                return;
            }
            if (g_mode == Mode::Verify)
            {
                ++g_stFrame.tick;
                if ((g_stFrame.tick % g_verifyEvery) == 0u)
                {
                    verifySmall(g_stFrame, "RwFrameTransform", g_origFrame, rdram, ctx, runtime, ra, [&](Env &e, R5900Context &c)
                                {
                                    frameRun(e, F, M);
                                    setV0(&c, F);
                                });
                    return;
                }
                g_origFrame(rdram, ctx, runtime);
                return;
            }
            frameRun(env, F, M);
            setV0(ctx, F);
            ctx->pc = ra;
            ++g_stFrame.native;
            chargeCycles(runtime, env.calls * 8u + env.edges * 32u);
        }

        template <bool kScale>
        void matrixHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            SmallStats &st = kScale ? g_stScale : g_stTranslate;
            GuestFunction orig = kScale ? g_origScale : g_origTranslate;
            const uint32_t entry = kScale ? kMatrixScale : kMatrixTranslate;
            ++st.calls;
            maybeLog();
            if (!(kScale ? g_enScale : g_enTranslate) || g_mode == Mode::Off || ctx->pc != entry)
            {
                orig(rdram, ctx, runtime);
                return;
            }
            const uint32_t M = getRegU32(ctx, 4), V = getRegU32(ctx, 5);
            const uint64_t op64 = GPR_U64(ctx, 6); // the original compares the whole register with 2, 1, 0
            const uint32_t op = op64 <= 2u ? static_cast<uint32_t>(op64) : 3u;
            const uint32_t ra = getRegU32(ctx, 31);
            Env env{rdram, runtime, ctx, Mem{rdram, nullptr}};
            if (!matrixArgsOk(env, M, V, op))
            {
                ++st.fallback;
                orig(rdram, ctx, runtime);
                return;
            }
            auto run = [&](Env &e, R5900Context &c)
            {
                if (kScale)
                    scaleRun(e, M, V, op);
                else
                    translateRun(e, M, V, op);
                setV0(&c, M);
            };
            const int hid = kScale ? 2 : 3;
            const uint64_t t0 = g_timeMode >= 2 ? stamp() : 0u;
            if (g_mode == Mode::Log)
            {
                orig(rdram, ctx, runtime);
                if (g_timeMode >= 2)
                {
                    g_hu[0][hid] += delta(t0, stamp());
                    ++g_hn[0][hid];
                }
                return;
            }
            if (g_mode == Mode::Verify)
            {
                ++st.tick;
                if ((st.tick % g_verifyEvery) == 0u)
                {
                    ++st.ops[op];
                    for (uint32_t k = 0u; k < g_fuzzPerCall; ++k)
                        fuzzMatrix(kScale, st, rdram, ctx, runtime, ra, static_cast<uint32_t>(g_verifyTick * 2246822519ull) ^ (k * 0x9E3779B1u));
                    verifySmall(st, kScale ? "RwMatrixScale" : "RwMatrixTranslate", orig, rdram, ctx, runtime, ra, run);
                    return;
                }
                orig(rdram, ctx, runtime);
                return;
            }
            run(env, *ctx);
            ctx->pc = ra;
            ++st.native;
            if (g_timeMode >= 2)
            {
                g_hu[1][hid] += delta(t0, stamp());
                ++g_hn[1][hid];
            }
        }


        // ---- measurement only: P3FES_EFFECTS_PROBE=addr[,addr...] with P3FES_EFFECTS_ALTERNATE=s --------------------------
        // Stubs that return at once ($v0 = 0): the host thread of install() swaps them for the original functions every s
        // seconds, so the instructions per frame of the stubbed windows minus the normal ones is the true inclusive cost of
        // those functions (scheduler visits that happen to land inside a function do not distort it, unlike a timer
        // around the function). The game keeps running; only the effect drawing it skips is not drawn.
        template <int I>
        void probeStub(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->r[2] = _mm_setzero_si128();
            ctx->pc = getRegU32(ctx, 31);
        }
        using ProbeFn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
        constexpr ProbeFn kProbeStubs[4] = {&probeStub<0>, &probeStub<1>, &probeStub<2>, &probeStub<3>};
        GuestFunction g_probeOrig[4] = {};
        uint32_t g_probeAddr[4] = {};
        uint32_t g_probeCount = 0u;


        // P3FES_EFFECTS_TIMEFN=addr[,addr...] (with P3FES_EFFECTS_TIME=2, measurement only): pass-through wrappers that
        // report the PMU instructions of the game thread per call of those functions, measured like the helpers above
        uint64_t g_tfUnits[4] = {}, g_tfCalls[4] = {};
        uint64_t g_hist[16] = {}, g_histN = 0u, g_histLive = 0u;
        inline uint32_t r32At(const uint8_t *r, uint32_t a) { uint32_t v; std::memcpy(&v, r + (a & PS2_RAM_MASK), 4u); return v; }
        inline uint32_t r16At(const uint8_t *r, uint32_t a) { uint16_t v; std::memcpy(&v, r + (a & PS2_RAM_MASK), 2u); return v; }
        uint32_t g_tfAddr[4] = {};
        GuestFunction g_tfOrig[4] = {};
        uint32_t g_tfCount = 0u;
        template <int I>
        void timeStub(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != g_tfAddr[I])
            {
                g_tfOrig[I](rdram, ctx, runtime);
                return;
            }
            if (g_tfAddr[I] == 0x003275D0u)
            {
                // measurement: variant, local-space flag, slots and live particles of the draw
                const uint32_t a0 = getRegU32(ctx, 4);
                const uint32_t variant = r16At(rdram, a0 + 0xCu) & 7u;
                const uint32_t blk = r32At(rdram, a0 + 0x4Cu);
                if (blk != 0u && blk < kRamSize - 0x100u)
                {
                    const uint32_t n = r32At(rdram, blk + 4u), pp = r32At(rdram, blk + 0x18u);
                    const uint32_t local = r32At(rdram, blk + 0xCu) & 1u;
                    if (variant < 8u && n < 0x1000u && pp < kRamSize - n * 0x20u)
                    {
                        uint32_t live = 0u;
                        for (uint32_t i = 0u; i < n; ++i)
                            live += static_cast<int32_t>(r32At(rdram, pp + i * 0x20u + 0x10u)) >= 0 ? 1u : 0u;
                        ++g_hist[variant * 2u + local];
                        g_histN += n;
                        g_histLive += live;
                    }
                }
            }
            const uint64_t t0 = stamp();
            g_tfOrig[I](rdram, ctx, runtime);
            g_tfUnits[I] += delta(t0, stamp());
            ++g_tfCalls[I];
            maybeLog();
        }
        constexpr ProbeFn kTimeStubs[4] = {&timeStub<0>, &timeStub<1>, &timeStub<2>, &timeStub<3>};
        bool installOne(PS2Runtime &runtime, uint32_t address, GuestFunction hookFn, GuestFunction &original)
        {
            if (!runtime.hasFunction(address))
                return false;
            GuestFunction current = runtime.lookupFunction(address);
            if (!current || current == hookFn)
                return false;
            original = current;
            return runtime.replaceFunction(address, hookFn);
        }
    }

    void logCounters()
    {
        if (g_mode == Mode::Off)
            return;
        std::fprintf(stderr,
                     "[p3fes-effects] calls=%llu native=%llu early=%llu fallback=%llu (range=%llu alias=%llu count=%llu type2=%llu trail=%llu) resumed=%llu | "
                     "kinds %llu/%llu/%llu | particles=%llu spawns=%llu bursts=%llu expired=%llu trail init/low/high=%llu/%llu/%llu | sinGuest=%llu guestFail=%llu\n",
                     (unsigned long long)g_c.calls, (unsigned long long)g_c.native, (unsigned long long)g_c.early, (unsigned long long)g_c.fallback,
                     (unsigned long long)g_c.fbRange, (unsigned long long)g_c.fbAlias, (unsigned long long)g_c.fbCount,
                     (unsigned long long)g_c.fbType2, (unsigned long long)g_c.fbTrail, (unsigned long long)g_c.fbResumed,
                     (unsigned long long)g_c.typeCalls[0], (unsigned long long)g_c.typeCalls[1], (unsigned long long)g_c.typeCalls[2],
                     (unsigned long long)g_c.particles, (unsigned long long)g_c.spawns, (unsigned long long)g_c.bursts,
                     (unsigned long long)g_c.expired, (unsigned long long)g_c.trailInit, (unsigned long long)g_c.trailLow,
                     (unsigned long long)g_c.trailHigh, (unsigned long long)g_c.sinGuest, (unsigned long long)g_c.guestFail);
        {
            const struct { const char *name; const SmallStats *st; } smalls[] = {{"forAllAtomics", &g_stForAll}, {"matrixScale", &g_stScale}, {"matrixTranslate", &g_stTranslate}, {"matrixRotate", &g_stRotate}, {"draw 0x3275D0", &g_stDraw}, {"frameTransform", &g_stFrame}};
            for (const auto &sm : smalls)
                if (sm.st->calls)
                    std::fprintf(stderr, "[p3fes-effects] %s: calls=%llu native=%llu fallback=%llu | verify checked=%llu (full %llu) bad=%llu regBad=%llu unwound=%llu ops=%llu/%llu/%llu fuzz=%llu bad=%llu\n", sm.name,
                                 (unsigned long long)sm.st->calls, (unsigned long long)sm.st->native, (unsigned long long)sm.st->fallback,
                                 (unsigned long long)sm.st->verified, (unsigned long long)sm.st->verifiedFull, (unsigned long long)sm.st->bad,
                                 (unsigned long long)sm.st->regBad, (unsigned long long)sm.st->unwound, (unsigned long long)sm.st->ops[0],
                                 (unsigned long long)sm.st->ops[1], (unsigned long long)sm.st->ops[2], (unsigned long long)sm.st->fuzzRuns, (unsigned long long)sm.st->fuzzBad);
        }
        std::fprintf(stderr, "[p3fes-effects] attrs hit=%llu miss=%llu nocache=%llu curveEntryMiss=%llu basisMiss=%llu avg s7=%.1f avg N=%.1f\n",
                     (unsigned long long)g_c.attrHit, (unsigned long long)g_c.attrMiss, (unsigned long long)g_c.attrNoCache,
                     (unsigned long long)g_c.curveEntryMiss, (unsigned long long)g_c.basisMiss,
                     g_c.native + g_c.verified ? static_cast<double>(g_c.s7Sum) / static_cast<double>(g_c.native + g_c.verified) : 0.0,
                     g_c.native + g_c.verified ? static_cast<double>(g_c.nSum) / static_cast<double>(g_c.native + g_c.verified) : 0.0);
        if (g_c.fbAlias)
        {
            std::fprintf(stderr, "[p3fes-effects] alias reasons:");
            for (int i = 0; i < 40; ++i)
                if (g_c.aliasWhy[i])
                    std::fprintf(stderr, " #%d=%llu", i, (unsigned long long)g_c.aliasWhy[i]);
            std::fprintf(stderr, "\n");
        }
        if (g_mode == Mode::Verify && g_fuzzPerCall)
            std::fprintf(stderr, "[p3fes-effects] fuzz: runs=%llu bad=%llu skipped=%llu unwound=%llu\n", (unsigned long long)g_fz.runs,
                         (unsigned long long)g_fz.bad, (unsigned long long)g_fz.skipped, (unsigned long long)g_fz.unwound);
        if (g_mode == Mode::Verify)
            std::fprintf(stderr,
                         "[p3fes-effects] verify: checked=%llu (full %llu) bad=%llu roiBad=%llu ramBad=%llu scratchBad=%llu regBad=%llu unwound=%llu soft=%llu\n",
                         (unsigned long long)g_c.verified, (unsigned long long)g_c.verifiedFull, (unsigned long long)g_c.verifyBad,
                         (unsigned long long)g_c.verifyRoiBad, (unsigned long long)g_c.verifyWriteBad, (unsigned long long)g_c.verifyScratch,
                         (unsigned long long)g_c.verifyRegBad, (unsigned long long)g_c.verifyUnwound, (unsigned long long)g_c.verifySoft);
        if (g_timeMode == 2 && g_segN[SEG_TOTAL])
        {
            std::fprintf(stderr, "[p3fes-effects] stamp cost %llu instr; parts per call: basis %.0f curve %.0f; sin per live particle %.0f\n", (unsigned long long)g_stampCost,
                         static_cast<double>(g_seg[SEG_BASIS]) / static_cast<double>(g_segN[SEG_TOTAL]),
                         static_cast<double>(g_seg[SEG_CURVE]) / static_cast<double>(g_segN[SEG_TOTAL]),
                         g_segN[SEG_SIN] ? static_cast<double>(g_seg[SEG_SIN]) / static_cast<double>(g_segN[SEG_SIN]) : 0.0);
            const double calls = static_cast<double>(g_segN[SEG_TOTAL]);
            auto per = [&](int seg) { return static_cast<double>(g_seg[seg]) / calls - static_cast<double>(g_stampCost) * static_cast<double>(g_segN[seg]) / calls; };
            std::fprintf(stderr,
                         "[p3fes-effects] per call (stamp=%llu): pre %.0f setup %.0f loop %.0f (alive %.0f spawn %.0f; %.1f alive + %.2f spawn per call) post %.0f total %.0f\n",
                         (unsigned long long)g_stampCost, per(SEG_PRE), per(SEG_SETUP), per(SEG_LOOP), per(SEG_ALIVE), per(SEG_SPAWN),
                         static_cast<double>(g_segN[SEG_ALIVE]) / calls, static_cast<double>(g_segN[SEG_SPAWN]) / calls, per(SEG_POST), per(SEG_TOTAL));
        }
        if (g_timeMode >= 2)
        {
            static const char *const names[6] = {"particles 0x32AF30", "forAllAtomics", "matrixScale", "matrixTranslate", "matrixRotate", "draw 0x3275D0"};
            for (int i = 0; i < 6; ++i)
                for (int k = 0; k < 2; ++k)
                    if (g_hn[k][i])
                        std::fprintf(stderr, "[p3fes-effects] %s %s: %.0f instr/call over %llu calls\n", names[i], k ? "native" : "original",
                                     static_cast<double>(g_hu[k][i]) / static_cast<double>(g_hn[k][i]), (unsigned long long)g_hn[k][i]);
        }
        {
            uint64_t tot = 0u;
            for (uint64_t h : g_hist)
                tot += h;
            if (tot)
            {
                std::fprintf(stderr, "[p3fes-effects] 0x3275D0 draws (variant,local):");
                for (int i = 0; i < 16; ++i)
                    if (g_hist[i])
                        std::fprintf(stderr, " v%d/l%d=%llu", i / 2, i & 1, (unsigned long long)g_hist[i]);
                std::fprintf(stderr, " avg slots %.1f avg live %.2f\n", static_cast<double>(g_histN) / static_cast<double>(tot), static_cast<double>(g_histLive) / static_cast<double>(tot));
            }
        }
        for (uint32_t i = 0u; i < g_tfCount; ++i)
            if (g_tfCalls[i])
                std::fprintf(stderr, "[p3fes-effects] function 0x%08X: %.0f instr/call over %llu calls\n", g_tfAddr[i],
                             static_cast<double>(g_tfUnits[i]) / static_cast<double>(g_tfCalls[i]), (unsigned long long)g_tfCalls[i]);
        if ((g_time || g_mode == Mode::Log) && g_c.nsCalls)
            std::fprintf(stderr, "[p3fes-effects] 0x32AF30: %llu calls, %.2f us avg\n", (unsigned long long)g_c.nsCalls,
                         static_cast<double>(g_c.ns) / 1e3 / static_cast<double>(g_c.nsCalls));
    }

    bool install(PS2Runtime &runtime)
    {
        const char *value = std::getenv("P3FES_NATIVE_EFFECTS");
        g_c = {};
        g_mode = Mode::On;
        if (value && *value)
        {
            if (std::strcmp(value, "0") == 0)
                g_mode = Mode::Off;
            else if (std::strcmp(value, "verify") == 0)
                g_mode = Mode::Verify;
            else if (std::strcmp(value, "log") == 0)
                g_mode = Mode::Log;
        }
        if (const char *e = std::getenv("P3FES_EFFECTS_VERIFY_EVERY"); e && *e)
            g_verifyEvery = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        if (g_verifyEvery == 0u)
            g_verifyEvery = 1u;
        if (const char *e = std::getenv("P3FES_EFFECTS_VERIFY_FULL"); e && *e)
            g_verifyFull = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        const char *timing = std::getenv("P3FES_EFFECTS_TIME");
        g_time = timing && *timing == '1';
        g_timeMode = timing && *timing == '2' ? 2 : (timing && *timing == '3' ? 3 : 0);
        if (g_timeMode >= 2)
        {
            const uint64_t c0 = stamp();
            for (int i = 0; i < 1000; ++i)
                (void)stamp();
            const uint64_t c1 = stamp();
            g_stampCost = delta(c0, c1) / 1001u; // instructions that one stamp() adds to whatever is measured around it
            // micro costs (instructions per operation) for the log
            volatile float sink = 0.0f;
            float x = 0.1f;
            const uint64_t t0 = stamp();
            for (int i = 0; i < 1000; ++i)
            {
                sink = hostSinf(x);
                x += 0.0371f;
            }
            const uint64_t t1 = stamp();
            float a = 1.0f;
            for (int i = 0; i < 1000; ++i)
            {
                a = FPU_ADD_S(a, 0.001f);
                a = FPU_MUL_S(a, 1.0001f);
            }
            sink = a;
            const uint64_t t2 = stamp();
            std::fprintf(stderr, "[p3fes-effects] stamp cost %llu instr; micro: sinf %u instr/call, ps2 add+mul pair %u instr\n", (unsigned long long)g_stampCost, delta(t0, t1) / 1000u, delta(t1, t2) / 1000u);
        }
        g_trace = std::getenv("P3FES_EFFECTS_TRACE") != nullptr;
        if (const char *e = std::getenv("P3FES_EFFECTS_CACHE"); e && *e == '0')
            g_curveCache = false;
        if (const char *e = std::getenv("P3FES_EFFECTS_ALTERNATE"); e && *e)
            g_alternate = std::atoi(e);
        if (const char *e = std::getenv("P3FES_EFFECTS_FUZZ"); e && *e)
            g_fuzzPerCall = static_cast<uint32_t>(std::strtoul(e, nullptr, 10));
        const char *diag = std::getenv("P3FES_RESOURCE_DIAG");
        g_diag = diag && *diag == '1';
        if (g_mode == Mode::Off)
        {
            std::fprintf(stderr, "[p3fes-native] effects off\n");
            return false;
        }
        bool enAf30 = true;
        if (const char *e = std::getenv("P3FES_EFFECTS_TIMEFN"); e && *e && g_timeMode >= 2)
        {
            const char *list = e;
            while (*list && g_tfCount < 4u)
            {
                char *end = nullptr;
                const uint32_t address = static_cast<uint32_t>(std::strtoul(list, &end, 16));
                if (end == list)
                    break;
                list = *end == ',' ? end + 1 : end;
                g_tfAddr[g_tfCount] = address;
                if (runtime.hasFunction(address) && installOne(runtime, address, kTimeStubs[g_tfCount], g_tfOrig[g_tfCount]))
                    ++g_tfCount;
            }
            enAf30 = false;
            g_enForAll = g_enScale = g_enTranslate = g_enRotate = g_enDraw = g_enFrame = false;
        }
        else if (const char *e = std::getenv("P3FES_EFFECTS_PROBE"); e && *e)
        {
            // measurement: only the probes, no native path
            enAf30 = false;
            g_enForAll = g_enScale = g_enTranslate = g_enRotate = g_enDraw = g_enFrame = false;
            const char *list = e;
            while (*list && g_probeCount < 4u)
            {
                char *end = nullptr;
                const uint32_t address = static_cast<uint32_t>(std::strtoul(list, &end, 16));
                if (end == list)
                    break;
                list = *end == ',' ? end + 1 : end;
                if (runtime.hasFunction(address) && installOne(runtime, address, kProbeStubs[g_probeCount], g_probeOrig[g_probeCount]))
                {
                    g_probeAddr[g_probeCount] = address;
                    std::fprintf(stderr, "[p3fes-effects] probe stub at 0x%08X\n", address);
                    ++g_probeCount;
                }
            }
        }
        else if (const char *e = std::getenv("P3FES_EFFECTS_FUNCS"); e && *e)
        {
            const std::string list = std::string(",") + e + ",";
            if (list == ",all,")
                g_enForAll = g_enScale = g_enTranslate = g_enRotate = g_enDraw = g_enFrame = true;
            else
            {
                enAf30 = list.find(",af30,") != std::string::npos;
                g_enForAll = list.find(",forall,") != std::string::npos;
                g_enScale = list.find(",scale,") != std::string::npos;
                g_enTranslate = list.find(",translate,") != std::string::npos;
                g_enRotate = list.find(",rotate,") != std::string::npos;
                g_enDraw = list.find(",draw,") != std::string::npos;
                g_enFrame = list.find(",frame,") != std::string::npos;
            }
        }
        g_colorCb = runtime.lookupFunction(kColorCallback);
        const bool h0 = enAf30 && installOne(runtime, kEntry, &hook, g_orig);
        const bool h1 = g_enForAll && installOne(runtime, kForAllAtomics, &forAllHook, g_origForAll);
        const bool h2 = g_enScale && installOne(runtime, kMatrixScale, &matrixHook<true>, g_origScale);
        const bool h3 = g_enTranslate && installOne(runtime, kMatrixTranslate, &matrixHook<false>, g_origTranslate);
        const bool h4 = g_enRotate && installOne(runtime, kMatrixRotate, &rotateHook, g_origRotate);
        const bool h5 = g_enDraw && installOne(runtime, kDraw, &drawHook, g_origDraw);
        const bool h6 = g_enFrame && installOne(runtime, kFrameTransform, &frameHook, g_origFrame);
        if (g_alternate > 0 && g_mode == Mode::On && (h0 || h1 || h2 || h3 || h4 || h5 || h6 || g_probeCount != 0u))
        {
            // measurement: a host thread swaps the hooks for the original functions and back every <seconds> (the game thread
            // keeps its direct calls to the generated functions while they are off, the same as without this file)
            struct Swap { uint32_t address; GuestFunction hook, original; };
            static Swap swaps[8];
            uint32_t n = 0u;
            if (h0) swaps[n++] = {kEntry, &hook, g_orig};
            if (h1) swaps[n++] = {kForAllAtomics, &forAllHook, g_origForAll};
            if (h2) swaps[n++] = {kMatrixScale, &matrixHook<true>, g_origScale};
            if (h3) swaps[n++] = {kMatrixTranslate, &matrixHook<false>, g_origTranslate};
            if (h4) swaps[n++] = {kMatrixRotate, &rotateHook, g_origRotate};
            if (h5) swaps[n++] = {kDraw, &drawHook, g_origDraw};
            if (h6) swaps[n++] = {kFrameTransform, &frameHook, g_origFrame};
            for (uint32_t i = 0u; i < g_probeCount; ++i)
                swaps[n++] = {g_probeAddr[i], kProbeStubs[i], g_probeOrig[i]};
            static PS2Runtime *rt = &runtime;
            static uint32_t nSwaps = n;
            std::thread([]()
                        {
                            const int period = g_alternate;
                            uint64_t seconds = 0u;
                            bool on = true;
                            for (;;)
                            {
                                std::this_thread::sleep_for(std::chrono::seconds(period));
                                seconds += static_cast<uint64_t>(period);
                                on = !on;
                                for (uint32_t i = 0u; i < nSwaps; ++i)
                                    rt->replaceFunction(swaps[i].address, on ? swaps[i].hook : swaps[i].original);
                                std::fprintf(stderr, "[p3fes-altcuts] %s at frame %llu (%llus after the effects were installed)\n", on ? "on" : "off",
                                             static_cast<unsigned long long>(g_c.calls), static_cast<unsigned long long>(seconds));
                            }
                        }).detach();
        }
        std::fprintf(stderr, "[p3fes-native] effects mode=%s hooks: particles(0x32AF30)=%u forAllAtomics=%u matrixScale=%u matrixTranslate=%u matrixRotate=%u draw=%u frame=%u\n",
                     g_mode == Mode::Verify ? "verify" : (g_mode == Mode::Log ? "log" : "on"), h0 ? 1u : 0u, h1 ? 1u : 0u, h2 ? 1u : 0u, h3 ? 1u : 0u, h4 ? 1u : 0u, h5 ? 1u : 0u, h6 ? 1u : 0u);
        return h0 || h1 || h2 || h3 || h4 || h5 || h6 || g_probeCount != 0u || g_tfCount != 0u;
    }
}
