#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace p3fes_vhs
{
    // Geometry in 640x448 picture space. Both the native scene renderer and
    // the movie presenter draw the tracking noise over their finished picture.
    struct Ink { uint8_t r, g, b, a; };

    template<class Rect>
    void draw(Rect rect)
    {
        using namespace std::chrono;
        const uint32_t frame = static_cast<uint32_t>(duration_cast<milliseconds>(
            steady_clock::now().time_since_epoch()).count() / 42);
        uint32_t noise = frame * 2654435761u;
        auto next = [&] { noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5; return noise; };
        for (int y = frame % 2; y < 448; y += 5)
            rect(0, y, 640, 1, Ink{24, 15, 42, 38});
        for (int band = 0; band < 2; ++band)
        {
            const int y = static_cast<int>((frame * 5u + band * 229u) % 464u) - 16;
            rect(0, y + 2, 640, 5, Ink{188, 182, 210, 31});
            rect(0, y + 7, 640, 1, Ink{33, 17, 46, 78});
            for (int i = 0; i < 52; ++i)
            {
                const int x = next() % 640;
                const int row = next() % 9;
                const int w = std::min(640 - x, 2 + int(next() % 38));
                const auto alpha = static_cast<uint8_t>(54 + next() % 85);
                rect(x, y + row, w, 1, i % 4 == 0 ? Ink{85, 193, 212, alpha} : Ink{232, 224, 234, alpha});
            }
        }
        // The two transport arrows remain readable against both dark and bright scenes.
        for (int arrow = 0; arrow < 2; ++arrow)
            for (int row = 0; row < 13; ++row)
            {
                const int span = 2 + 2 * std::min(row, 12 - row);
                const int x = 24 + 17 * arrow;
                rect(x + 1, 27 + row, span, 1, Ink{20, 18, 31, 185});
                rect(x, 26 + row, span, 1, Ink{245, 244, 237, 225});
            }
    }
}
