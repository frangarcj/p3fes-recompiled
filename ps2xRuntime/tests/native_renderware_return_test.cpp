#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr uint32_t kTableBase = 0x00100000u;
    constexpr uint32_t kTableEnd = 0x00560000u;
    constexpr uint32_t kEntry = 0x00100008u;
    constexpr uint32_t kReturnPc = 0x00401000u;
    constexpr uint32_t kOriginalMarker = 0x0BADCAFEu;

    constexpr uint32_t kAtomicRender = 0x00490860u;
    constexpr uint32_t kSectorRender = 0x00499B90u;
    constexpr uint32_t kPtankRender = 0x00474160u;
    constexpr uint32_t kCameraClear = 0x004C9E90u;
    constexpr uint32_t kIm3DTransform = 0x004F1ED0u;
    constexpr uint32_t kIm3DEnd = 0x004F1F80u;
    constexpr uint32_t kIm3DIndexed = 0x004F1FD0u;
    constexpr uint32_t kIm3DPrimitive = 0x004F2150u;
    constexpr uint32_t kIm3DLine = 0x004F22B0u;
    constexpr uint32_t kIm2DLine = 0x004ECA60u;
    constexpr uint32_t kIm2DTriangle = 0x004ECCE0u;
    constexpr uint32_t kIm2DPrimitive = 0x004EDBA0u;
    constexpr uint32_t kIm2DIndexed = 0x004EEB50u;

    uint32_t getReg(const R5900Context &ctx, uint32_t index)
    {
        return index == 0u ? 0u : static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[index]));
    }

    void setReg(R5900Context &ctx, uint32_t index, uint32_t value)
    {
        if (index != 0u)
            ctx.r[index] = _mm_cvtsi32_si128(static_cast<int32_t>(value));
    }

    void fakeFunction(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReg(*ctx, 2u, kOriginalMarker);
        ctx->pc = getReg(*ctx, 31u);
    }

    void require(bool condition, const char *message)
    {
        if (condition)
            return;
        std::fprintf(stderr, "native RenderWare return test failed: %s\n", message);
        std::exit(1);
    }

    void callLeaf(PS2Runtime &runtime, uint8_t *rdram, uint32_t entry,
                  uint32_t expectedV0, const char *name,
                  uint32_t expectedNestedV0 = UINT32_MAX)
    {
        auto function = runtime.lookupFunction(entry);
        require(function && function != &fakeFunction, "override did not install the native leaf");

        R5900Context ctx;
        ctx.pc = entry;
        setReg(ctx, 31u, kReturnPc);
        setReg(ctx, 2u, 0xDEADBEEFu);
        function(rdram, &ctx, &runtime);
        require(ctx.pc == kReturnPc, name);
        require(getReg(ctx, 2u) == expectedV0, name);

        R5900Context nested;
        nested.pc = entry;
        setReg(nested, 31u, kReturnPc);
        setReg(nested, 2u, 0xDEADBEEFu);
        const bool returned = runtime.dispatchGuestBranch(
            rdram, &nested, entry, kReturnPc - 8u, kReturnPc,
            PS2Runtime::GuestBranchKind::DirectCall, "native RenderWare leaf return test");
        require(returned && nested.pc == kReturnPc,
                "dispatchGuestBranch did not observe the leaf's nested return");
        const uint32_t nestedExpected = expectedNestedV0 == UINT32_MAX ? expectedV0 : expectedNestedV0;
        require(getReg(nested, 2u) == nestedExpected,
                "dispatchGuestBranch returned with the wrong V0");
    }
}

int main()
{
    setenv("P3FES_NATIVE_RW", "1", 1);
    setenv("P3FES_NATIVE_CAMERA", "0", 1);
    setenv("P3FES_NATIVE_BGM", "0", 1);

    const uint32_t slotCount = (kTableEnd - kTableBase) / 4u;
    std::vector<PS2Runtime::RecompiledFunction> table(slotCount, &fakeFunction);
    ps2SetRecompiledFunctionTable({table.data(), kTableBase, kTableEnd, slotCount});

    PS2Runtime runtime;
    ps2_game_overrides::applyMatching(runtime, "SLUS_216.21", kEntry, 0x9B2AF129u, true);
    std::vector<uint8_t> rdram(32u * 1024u * 1024u);

    // SkyIm2DRender* returns one for every input, including empty geometry.
    callLeaf(runtime, rdram.data(), kIm2DLine, 1u, "Im2D line did not return V0=1 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm2DTriangle, 1u, "Im2D triangle did not return V0=1 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm2DPrimitive, 1u, "Im2D primitive did not return V0=1 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm2DIndexed, 1u, "Im2D indexed primitive did not return V0=1 and the caller PC");

    // Invalid/empty Im3D inputs exercise each early-return path without creating renderer state.
    callLeaf(runtime, rdram.data(), kIm3DTransform, 0u, "Im3D transform did not return V0=0 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm3DEnd, 1u, "Im3D end did not return V0=1 and the caller PC", 0u);
    callLeaf(runtime, rdram.data(), kIm3DIndexed, 0u, "Im3D indexed primitive did not return V0=0 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm3DPrimitive, 0u, "Im3D primitive did not return V0=0 and the caller PC");
    callLeaf(runtime, rdram.data(), kIm3DLine, 0u, "Im3D line did not return V0=0 and the caller PC");

    callLeaf(runtime, rdram.data(), kAtomicRender, 0u, "atomic render did not return V0=atomic and the caller PC");
    callLeaf(runtime, rdram.data(), kSectorRender, 0u, "sector render did not return V0=sector and the caller PC");
    callLeaf(runtime, rdram.data(), kPtankRender, 0u, "PTank render did not return V0=atomic and the caller PC");

    // The zeroed runtime globals select an empty native clear frame; this checks
    // the native return path without creating camera geometry.
    callLeaf(runtime, rdram.data(), kCameraClear, 0u,
             "camera clear did not return its camera and caller PC");
}
