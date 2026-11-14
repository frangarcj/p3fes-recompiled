#include "native_renderer/renderer.h"
#include <cassert>

bool passes(unsigned function, unsigned alpha, unsigned reference)
{
    switch (function) {
    case 0: return false;
    case 1: return true;
    case 2: return alpha < reference;
    case 3: return alpha <= reference;
    case 4: return alpha == reference;
    case 5: return alpha >= reference;
    case 6: return alpha > reference;
    case 7: return alpha != reference;
    }
    return false;
}

int main()
{
    // Alpha-failure rendering must write each fragment in exactly one pass,
    // including equality at 0 and 255. Double hits would blend it twice.
    for (unsigned function = 0; function < 8; ++function)
        for (unsigned reference = 0; reference < 256; ++reference)
            for (unsigned alpha = 0; alpha < 256; ++alpha)
                assert(passes(function, alpha, reference) !=
                       passes(p3fes::native_renderer::complementGsAlphaFunction(function), alpha, reference));
}
