#pragma once

#include "ps2_runtime.h"

#include <cstdint>
#include <cstring>

namespace p3fes_guest_math_helper
{
    struct Result
    {
        bool completed = false;
        uint32_t value = 0u;
        uint32_t resumes = 0u;
    };

    // Run a nonblocking guest math helper to its caller return. Generated code
    // can return at an EE checkpoint with pc inside the function; resume those
    // blocks until the private RA sentinel is reached. The isolated context
    // keeps the native hook's guest PC/RA and registers untouched.
    inline Result run(uint8_t *rdram, const R5900Context &caller, PS2Runtime &runtime,
                      uint32_t entry, uint32_t a0, uint32_t a1 = 0u,
                      uint32_t maxResumes = 4096u)
    {
        constexpr uint32_t kReturnSentinel = 0xFFFFFFFCu;
        Result result;
        if (!rdram || !runtime.hasFunction(entry) || maxResumes == 0u)
            return result;

        R5900Context helper = caller;
        const __m128i originalSp = helper.r[29];
        helper.pc = entry;
        helper.r[4] = _mm_cvtsi32_si128(static_cast<int32_t>(a0));
        helper.r[5] = _mm_cvtsi32_si128(static_cast<int32_t>(a1));
        helper.r[31] = _mm_cvtsi32_si128(static_cast<int32_t>(kReturnSentinel));

        for (uint32_t resume = 0u; resume < maxResumes; ++resume)
        {
            if (runtime.isStopRequested() || helper.pc == 0u || helper.pc == kReturnSentinel ||
                !runtime.hasFunction(helper.pc))
                return result;

            const uint32_t resumePc = helper.pc;
            const PS2Runtime::RecompiledFunction function = runtime.lookupFunction(resumePc);
            if (!function)
                return result;
            function(rdram, &helper, &runtime);
            result.resumes = resume + 1u;

            if (runtime.isStopRequested())
                return result;
            if (helper.pc == kReturnSentinel)
            {
                if (std::memcmp(&helper.r[29], &originalSp, sizeof(originalSp)) != 0)
                    return result;
                result.completed = true;
                result.value = getRegU32(&helper, 2);
                return result;
            }
        }
        return result;
    }
} // namespace p3fes_guest_math_helper
