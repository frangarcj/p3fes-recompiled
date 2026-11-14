#include "p3fes_native_math.h"

#include "ps2_runtime.h"

#include <cmath>
#include <cstring>
#include <unordered_map>

namespace p3fes_native_math
{
    namespace
    {
        constexpr uint32_t kAtanf = 0x0052E408u;
        constexpr uint32_t kSinf = 0x0052E878u;
        constexpr uint32_t kCosf = 0x0052E6D8u;
        constexpr uint32_t kAbsBitsMask = 0x7FFFFFFFu;
        constexpr uint32_t kExponentMask = 0x7F800000u;
        constexpr uint32_t kMaxFastMagnitudeBits = 0x46000000u; // 8192.0f

        using GuestFunction = PS2Runtime::RecompiledFunction;

        std::unordered_map<uint32_t, GuestFunction> g_originals;
        // These hooks run on the single EE worker thread. Plain counters avoid
        // introducing atomic runtime helpers on Vita's hot path.
        uint64_t g_nativeAtan = 0u;
        uint64_t g_nativeSin = 0u;
        uint64_t g_nativeCos = 0u;
        uint64_t g_guestFallbacks = 0u;
        uint64_t g_installFailures = 0u;

        bool useNativeMath(float value)
        {
            uint32_t bits = 0u;
            std::memcpy(&bits, &value, sizeof(bits));
            const uint32_t magnitudeBits = bits & kAbsBitsMask;
            const uint32_t exponentBits = magnitudeBits & kExponentMask;

            // Zero is supported, while subnormals deliberately stay on the
            // guest implementation. Bit checks avoid libm classification and
            // leave the original argument bits untouched.
            const bool zero = magnitudeBits == 0u;
            const bool normal = exponentBits != 0u && exponentBits != kExponentMask;
            return magnitudeBits <= kMaxFastMagnitudeBits && (zero || normal);
        }

        GuestFunction originalFor(uint32_t address)
        {
            const auto found = g_originals.find(address);
            return found == g_originals.end() ? nullptr : found->second;
        }

        enum class Operation : uint8_t
        {
            Atan,
            Sin,
            Cos,
        };

        void mathHook(Operation operation, uint32_t address,
                      uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!ctx)
                return;

            const float argument = ctx->f[12]; // EE COP1 ABI: f12 argument, f0 result.
            if (useNativeMath(argument))
            {
                switch (operation)
                {
                case Operation::Atan:
                    ctx->f[0] = std::atan(argument);
                    ++g_nativeAtan;
                    break;
                case Operation::Sin:
                    ctx->f[0] = std::sin(argument);
                    ++g_nativeSin;
                    break;
                case Operation::Cos:
                    ctx->f[0] = std::cos(argument);
                    ++g_nativeCos;
                    break;
                }
                // These routines are leaf math calls at the EE ABI boundary;
                // native fast path changes only f0 and the guest return PC.
                ctx->pc = getRegU32(ctx, 31);
                return;
            }

            ++g_guestFallbacks;
            if (const GuestFunction original = originalFor(address))
                original(rdram, ctx, runtime);
        }

        void atanfHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            mathHook(Operation::Atan, kAtanf, rdram, ctx, runtime);
        }

        void sinfHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            mathHook(Operation::Sin, kSinf, rdram, ctx, runtime);
        }

        void cosfHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            mathHook(Operation::Cos, kCosf, rdram, ctx, runtime);
        }
    }

    bool install(PS2Runtime &runtime, bool enabled)
    {
        if (!enabled)
            return false;

        struct HookSpec
        {
            uint32_t address;
            GuestFunction wrapper;
        };
        constexpr HookSpec hooks[] = {
            {kAtanf, &atanfHook},
            {kSinf, &sinfHook},
            {kCosf, &cosfHook},
        };

        bool allInstalled = true;
        for (const HookSpec &hook : hooks)
        {
            const GuestFunction current = runtime.lookupFunction(hook.address);
            if (!current)
            {
                ++g_installFailures;
                allInstalled = false;
                continue;
            }

            if (current == hook.wrapper)
            {
                if (g_originals.find(hook.address) == g_originals.end())
                {
                    ++g_installFailures;
                    allInstalled = false;
                }
                continue;
            }

            g_originals[hook.address] = current;
            if (!runtime.replaceFunction(hook.address, hook.wrapper))
            {
                ++g_installFailures;
                allInstalled = false;
            }
        }
        return allInstalled;
    }

    Counters counters()
    {
        return {
            g_nativeAtan,
            g_nativeSin,
            g_nativeCos,
            g_guestFallbacks,
            g_installFailures,
        };
    }

    void resetCounters()
    {
        g_nativeAtan = 0u;
        g_nativeSin = 0u;
        g_nativeCos = 0u;
        g_guestFallbacks = 0u;
        g_installFailures = 0u;
    }
}
