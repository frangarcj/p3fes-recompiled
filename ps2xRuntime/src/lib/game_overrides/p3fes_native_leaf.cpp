#include "p3fes_native_leaf.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// See p3fes_native_leaf.h.
namespace p3fes_native_leaf
{
    namespace
    {
        using GuestFunction = PS2Runtime::RecompiledFunction;
        enum class Mode : uint8_t { Off, On, Verify };

        inline uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + a, 4u); return v; }
        inline float rdf(const uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + a, 4u); return v; }
        inline bool inRam(uint32_t a, uint32_t size) { return a >= 0x100u && static_cast<uint64_t>(a) + size <= PS2_RAM_SIZE && (a & 3u) == 0u; }

        uint32_t g_verifyEvery = 7u;

        // ---- 0x4C9DF0 RwCameraFrustumTestSphere(a0 = camera, a1 = sphere {x, y, z, r}) -----------------------------------------
        //
        //   for the six planes P = camera + 0x94 + 0x14 * i (normal x, y, z at +0, +4, +8, distance at +0xC):
        //     dist = ((y * ny) + x * nx) + z * nz - d      (mula.s / madda.s / madd.s / sub.s, in this order)
        //     dist > r            -> return 0 at once (outside)
        //     dist > -r           -> the result drops from 2 to 1 (the sphere crosses the plane)
        //   returns 2 when every plane left it at 2. The compare bit (FCR31 bit 23) is left by the last comparison made;
        //   $v1, $a0, $a2, $a3 and the FPU temporaries are caller-saved and not reproduced.
        constexpr uint32_t kFrustum = 0x004C9DF0u;
        GuestFunction g_frustumOriginal = nullptr;
        Mode g_frustumMode = Mode::Off;
        uint64_t g_frustumCalls = 0u, g_frustumNative = 0u, g_frustumDeclined = 0u, g_frustumVerified = 0u, g_frustumBad = 0u, g_frustum[3] = {};

        // true: answered (result in v0, cond bit set); false: nothing was touched
        inline bool frustumNative(const uint8_t *rdram, uint32_t camera, uint32_t sphere, uint32_t &result, bool &cond, uint32_t &iterations)
        {
            if (!inRam(camera + 0x94u, 6u * 0x14u) || !inRam(sphere, 16u))
                return false;
            const float x = rdf(rdram, sphere), y = rdf(rdram, sphere + 4u), z = rdf(rdram, sphere + 8u), r = rdf(rdram, sphere + 0xCu);
            const float negR = FPU_NEG_S(r);
            uint32_t v0 = 2u;
            bool lastCond = false;
            uint32_t n = 0u;
            for (uint32_t plane = camera + 0x94u, end = plane + 6u * 0x14u; plane != end; plane += 0x14u)
            {
                ++n;
                const float ny = rdf(rdram, plane + 4u), nx = rdf(rdram, plane), nz = rdf(rdram, plane + 8u), d = rdf(rdram, plane + 0xCu);
                float acc = FPU_MUL_S(y, ny);
                acc = FPU_ADD_S(acc, FPU_MUL_S(x, nx));
                const float f2 = FPU_ADD_S(acc, FPU_MUL_S(z, nz));
                const float dist = FPU_SUB_S(f2, d);
                lastCond = FPU_C_OLE_S(dist, r);
                if (!lastCond)
                {
                    v0 = 0u;
                    break;
                }
                lastCond = FPU_C_OLE_S(dist, negR);
                if (!lastCond)
                    v0 = 1u;
            }
            result = v0;
            cond = lastCond;
            iterations = n;
            return true;
        }

        void frustumHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ++g_frustumCalls;
            if (ctx->pc != kFrustum)
            {
                g_frustumOriginal(rdram, ctx, runtime);
                return;
            }
            uint32_t result = 0u, iterations = 0u;
            bool cond = false;
            if (!frustumNative(rdram, getRegU32(ctx, 4), getRegU32(ctx, 5), result, cond, iterations))
            {
                ++g_frustumDeclined;
                g_frustumOriginal(rdram, ctx, runtime);
                return;
            }
            if (g_frustumMode == Mode::Verify && (g_frustumCalls % g_verifyEvery) == 0u)
            {
                // the original on a copy of the context: what the caller can see must match
                R5900Context copy = *ctx;
                const uint32_t ra = getRegU32(ctx, 31);
                g_frustumOriginal(rdram, &copy, runtime);
                // a scheduler checkpoint inside the loop returns early with the resume pc: run it to the end
                for (uint32_t resume = 0u; resume < 1000u && copy.pc != ra && copy.pc != 0u && runtime->hasFunction(copy.pc); ++resume)
                    runtime->lookupFunction(copy.pc)(rdram, &copy, runtime);
                ++g_frustumVerified;
                bool ok = copy.pc == getRegU32(ctx, 31) && getRegU32(&copy, 2) == result && ((copy.fcr31 & 0x800000u) != 0u) == cond;
                for (int reg : {16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
                    ok = ok && GPR_U64(&copy, reg) == GPR_U64(ctx, reg);
                for (int f = 20; f < 32; ++f)
                    ok = ok && std::memcmp(&copy.f[f], &ctx->f[f], sizeof(float)) == 0;
                if (!ok && g_frustumBad++ < 8u)
                    std::fprintf(stderr, "[p3fes-leaf] frustum verify MISMATCH native=%u/%d original=%u/%d pc=%08X\n", result, cond ? 1 : 0,
                                 getRegU32(&copy, 2), (copy.fcr31 & 0x800000u) != 0u ? 1 : 0, copy.pc);
                else if (!ok)
                    ++g_frustumBad;
                // continue with the original's result (identical when ok)
                *ctx = copy;
                return;
            }
            ++g_frustumNative;
            ++g_frustum[result];
            ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(result));
            ctx->fcr31 = cond ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
            ctx->pc = getRegU32(ctx, 31);
            // the original's loop back edges (32 cycles each); never enters the scheduler
            runtime->eeTryChargeCall(iterations > 1u ? (iterations - 1u) * 32u : 0u);
        }

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

        Mode modeOf(const char *name)
        {
            const char *v = std::getenv(name);
            if (!v || !*v || (v[0] == '0' && !v[1]))
                return Mode::Off;
            return std::strcmp(v, "verify") == 0 ? Mode::Verify : Mode::On;
        }
    }

    bool install(PS2Runtime &runtime)
    {
        if (const char *e = std::getenv("P3FES_CUT_LEAF_VERIFY_EVERY"); e && *e)
            g_verifyEvery = static_cast<uint32_t>(std::max(1, std::atoi(e)));
        bool any = false;
        g_frustumMode = modeOf("P3FES_CUT_FRUSTUM");
        if (g_frustumMode != Mode::Off)
        {
            const bool ok = installOne(runtime, kFrustum, &frustumHook, g_frustumOriginal);
            std::fprintf(stderr, "[p3fes-native] leaf frustum-test hook=%u mode=%s\n", ok ? 1u : 0u, g_frustumMode == Mode::Verify ? "verify" : "on");
            any = any || ok;
        }
        return any;
    }

    void logCounters()
    {
        if (g_frustumMode != Mode::Off)
            std::fprintf(stderr, "[p3fes-leaf] frustum calls=%llu native=%llu declined=%llu results 0/1/2=%llu/%llu/%llu | verify checked=%llu bad=%llu\n",
                         static_cast<unsigned long long>(g_frustumCalls), static_cast<unsigned long long>(g_frustumNative),
                         static_cast<unsigned long long>(g_frustumDeclined), static_cast<unsigned long long>(g_frustum[0]),
                         static_cast<unsigned long long>(g_frustum[1]), static_cast<unsigned long long>(g_frustum[2]),
                         static_cast<unsigned long long>(g_frustumVerified), static_cast<unsigned long long>(g_frustumBad));
    }
}
