#include "../src/lib/game_overrides/p3fes_renderware_projection.h"

#include <array>
#include <cassert>
#include <cmath>

namespace
{
    struct ClipPoint
    {
        float x;
        float y;
        float z;
        float w;
    };

    ClipPoint transform(const std::array<float, 16> &m, float x, float y, float z)
    {
        return {
            m[0] * x + m[4] * y + m[8] * z + m[12],
            m[1] * x + m[5] * y + m[9] * z + m[13],
            m[2] * x + m[6] * y + m[10] * z + m[14],
            m[3] * x + m[7] * y + m[11] * z + m[15],
        };
    }

    void near(float actual, float expected)
    {
        assert(std::fabs(actual - expected) < 1.0e-5f);
    }

    float ndcX(const ClipPoint &p) { return p.x / p.w; }
    float ndcY(const ClipPoint &p) { return p.y / p.w; }
    float ndcZ(const ClipPoint &p) { return p.z / p.w; }
}

int main()
{
    constexpr float wx = 2.0f;
    constexpr float wy = 1.5f;
    constexpr float n = 0.5f;
    constexpr float f = 10.0f;

    const auto ortho = p3fes_renderware_detail::makeCameraProjection(2, wx, wy, 0, 0, n, f);
    near(ndcZ(transform(ortho, 0, 0, n)), -1.0f);
    near(ndcZ(transform(ortho, 0, 0, f)), 1.0f);
    near(ndcX(transform(ortho, 0, 0, 3)), 0.0f);
    near(ndcY(transform(ortho, 0, 0, 3)), 0.0f);
    near(ndcX(transform(ortho, wx, 0, 3)), 1.0f);
    near(ndcX(transform(ortho, -wx, 0, 3)), -1.0f);
    near(ndcY(transform(ortho, 0, wy, 3)), 1.0f);
    near(ndcY(transform(ortho, 0, -wy, 3)), -1.0f);

    // Non-zero camera offsets retain the RW offset equation. At z=3 the
    // center ray shifts by offset/viewWindow * (z - 1) in parallel projection.
    constexpr float offsetX = 0.25f;
    constexpr float offsetY = -0.125f;
    const auto orthoOffset = p3fes_renderware_detail::makeCameraProjection(2, wx, wy, offsetX, offsetY, n, f);
    near(ndcX(transform(orthoOffset, 0, 0, 3)), (offsetX / wx) * (3.0f - 1.0f));
    near(ndcY(transform(orthoOffset, 0, 0, 3)), (offsetY / wy) * (3.0f - 1.0f));
    near(ndcZ(transform(orthoOffset, 0, 0, n)), -1.0f);
    near(ndcZ(transform(orthoOffset, 0, 0, f)), 1.0f);

    const auto perspective = p3fes_renderware_detail::makeCameraProjection(1, wx, wy, 0, 0, n, f);
    near(ndcZ(transform(perspective, 0, 0, n)), -1.0f);
    near(ndcZ(transform(perspective, 0, 0, f)), 1.0f);
    near(ndcX(transform(perspective, 0, 0, 3)), 0.0f);
    near(ndcY(transform(perspective, 0, 0, 3)), 0.0f);
    near(ndcX(transform(perspective, wx * 3.0f, 0, 3)), 1.0f);
    near(ndcX(transform(perspective, -wx * 3.0f, 0, 3)), -1.0f);
    near(ndcY(transform(perspective, 0, wy * 3.0f, 3)), 1.0f);
    near(ndcY(transform(perspective, 0, -wy * 3.0f, 3)), -1.0f);

    // For perspective, the existing offset terms shift a ray by
    // (offset/viewWindow) * (z - 1) / z in normalized device coordinates.
    const auto perspectiveOffset = p3fes_renderware_detail::makeCameraProjection(
        1, wx, wy, offsetX, offsetY, n, f);
    near(ndcX(transform(perspectiveOffset, 0, 0, 3)), (offsetX / wx) * (3.0f - 1.0f) / 3.0f);
    near(ndcY(transform(perspectiveOffset, 0, 0, 3)), (offsetY / wy) * (3.0f - 1.0f) / 3.0f);
    near(ndcZ(transform(perspectiveOffset, 0, 0, n)), -1.0f);
    near(ndcZ(transform(perspectiveOffset, 0, 0, f)), 1.0f);
}
