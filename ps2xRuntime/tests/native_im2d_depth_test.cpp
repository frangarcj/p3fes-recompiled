#include "../src/lib/game_overrides/p3fes_im2d_depth.h"
#include <cassert>
#include <cmath>

int main()
{
    for (const unsigned format : {0u, 1u, 2u, 10u})
    {
        const float maximum = format == 2 || format == 10 ? 65535.0f :
                              format == 1 ? 16777215.0f : 4294967295.0f;
        const auto range = p3fes_im2d::inverseDepthRange(uint64_t{format} << 24u);
        assert(p3fes_im2d::normalizedDepth(0, range) == 0);
        assert(p3fes_im2d::normalizedDepth(maximum, range) == 1);
        const float halfway = p3fes_im2d::normalizedDepth(maximum * .5f, range);
        assert(std::abs(halfway - .5f) < 1e-6f);
        // Ortho(-1, 0) produces clip z=1-2*z, within the clip volume.
        const float observedTitleDepth = p3fes_im2d::normalizedDepth(65535, range);
        const float clip = 1 - 2 * observedTitleDepth;
        assert(clip >= -1 && clip <= 1);
        assert(p3fes_im2d::normalizedDepth(-10, range) == 0);
        assert(p3fes_im2d::normalizedDepth(maximum * 2, range) == 1);
    }
}
