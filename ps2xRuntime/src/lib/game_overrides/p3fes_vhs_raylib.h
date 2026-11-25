#pragma once

#include "p3fes_vhs.h"
#include "ps2_host_backend.h"
#include <cmath>

namespace p3fes_vhs
{
    inline void drawIn(Rectangle picture)
    {
        draw([&](int x, int y, int w, int h, Ink ink) {
            if (y < 0 || y >= 448) return;
            const int left = std::lround(picture.x + x * picture.width / 640);
            const int top = std::lround(picture.y + y * picture.height / 448);
            const int right = std::lround(picture.x + (x+w) * picture.width / 640);
            const int bottom = std::lround(picture.y + std::min(448, y+h) * picture.height / 448);
            DrawRectangle(left, top, right-left, bottom-top, Color{ink.r, ink.g, ink.b, ink.a});
        });
    }
}
