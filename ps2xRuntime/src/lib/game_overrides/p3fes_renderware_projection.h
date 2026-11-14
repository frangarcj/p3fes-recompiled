#pragma once

#include <array>

namespace p3fes_renderware_detail
{
    // RenderWare camera to OpenGL clip matrix, in column-major element order.
    // The caller validates the camera fields and accepts projection 1/2 only.
    inline std::array<float, 16> makeCameraProjection(int projection,
                                                       float viewWindowX,
                                                       float viewWindowY,
                                                       float viewOffsetX,
                                                       float viewOffsetY,
                                                       float nearPlane,
                                                       float farPlane)
    {
        const float invwx = 1.0f / viewWindowX;
        const float invwy = 1.0f / viewWindowY;
        const float invz = 1.0f / (farPlane - nearPlane);
        const float ox = viewOffsetX * invwx;
        const float oy = viewOffsetY * invwy;
        if (projection == 1) // rwPERSPECTIVE
        {
            return {
                invwx, 0.0f, 0.0f, 0.0f,
                0.0f, invwy, 0.0f, 0.0f,
                ox, oy, (farPlane + nearPlane) * invz, 1.0f,
                -ox, -oy, -2.0f * nearPlane * farPlane * invz, 0.0f,
            };
        }

        // rwPARALLEL looks down +Z. Put near at -1 and far at +1 in GL clip
        // space: the Z scale is positive and the depth translation is negative.
        return {
            invwx, 0.0f, 0.0f, 0.0f,
            0.0f, invwy, 0.0f, 0.0f,
            ox, oy, 2.0f * invz, 0.0f,
            -ox, -oy, -(farPlane + nearPlane) * invz, 1.0f,
        };
    }
}
