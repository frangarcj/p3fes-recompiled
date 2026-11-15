#pragma once

#include <cstdint>

class PS2Runtime;

// Native versions of small, pure guest leaf functions that the scene draw calls hundreds of times a frame. Each one is a
// separate cut with its own switch, default OFF (the lead flips defaults after validation); `=verify` runs the native
// version and the original on the same input and compares everything the caller can see.
//   0x4C9DF0  RwCameraFrustumTestSphere(camera, sphere)   P3FES_CUT_FRUSTUM=1|verify
//             returns 0 (outside), 1 (intersects), 2 (inside) for the six planes at camera+0x94 (stride 0x14)
//   P3FES_CUT_LEAF_VERIFY_EVERY=n  verify every n-th call (default 7)
namespace p3fes_native_leaf
{
    bool install(PS2Runtime &runtime);
    void logCounters();
}
