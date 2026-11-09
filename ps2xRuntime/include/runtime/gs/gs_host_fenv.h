#pragma once

#include <cfenv>
#include <cstdint>
#include <limits>

#if !defined(USE_SSE2NEON)
#include <xmmintrin.h>
#endif

namespace GSHostFenv
{
    // The x86 path preserves the full MXCSR because the threaded GS already
    // transports it. ARM has no MXCSR equivalent, so carry the portable
    // rounding mode instead.
    inline uint32_t GetControlState()
    {
#if defined(USE_SSE2NEON)
        const int rounding = std::fegetround();
        return rounding == -1 ? std::numeric_limits<uint32_t>::max()
                              : static_cast<uint32_t>(rounding);
#else
        return _mm_getcsr();
#endif
    }

    inline void SetControlState(uint32_t state)
    {
#if defined(USE_SSE2NEON)
        if (state != std::numeric_limits<uint32_t>::max())
            std::fesetround(static_cast<int>(state));
#else
        _mm_setcsr(state);
#endif
    }

    class NearestRoundingScope
    {
    public:
        NearestRoundingScope()
        {
#if defined(USE_SSE2NEON)
            m_previous = std::fegetround();
            if (m_previous != -1 && m_previous != FE_TONEAREST)
                m_changed = std::fesetround(FE_TONEAREST) == 0;
#else
            m_previous = static_cast<int>(_mm_getcsr() & _MM_ROUND_MASK);
            if (m_previous != _MM_ROUND_NEAREST)
            {
                _mm_setcsr(_mm_getcsr() & ~_MM_ROUND_MASK);
                m_changed = true;
            }
#endif
        }

        ~NearestRoundingScope()
        {
            if (!m_changed)
                return;
#if defined(USE_SSE2NEON)
            std::fesetround(m_previous);
#else
            _mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) |
                       static_cast<unsigned>(m_previous));
#endif
        }

    private:
        int m_previous = 0;
        bool m_changed = false;
    };
}
