// Out-of-line access path for every guest address that is not plain main RAM.
// The generated code tests one mask inline (PS2_IS_PLAIN_RAM) and comes here
// for the scratchpad, I/O, GS, VU, BIOS, KSEG2/3 and unmapped mirrors, with the
// same behaviour the inline macros used to have.
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"

#if defined(PS2X_FAST_MEM)
uint8_t ps2SlowLoad8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
{
    return PS2_ORIG_READ(uint8_t, Load8, FAST_READ8, addr);
}
uint16_t ps2SlowLoad16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
{
    return PS2_ORIG_READ(uint16_t, Load16, FAST_READ16, addr);
}
uint32_t ps2SlowLoad32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
{
    return PS2_ORIG_READ(uint32_t, Load32, FAST_READ32, addr);
}
uint64_t ps2SlowLoad64(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
{
    return PS2_ORIG_READ(uint64_t, Load64, FAST_READ64, addr);
}
__m128i ps2SlowLoad128(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr)
{
    return PS2_ORIG_READ(__m128i, Load128, FAST_READ128, addr);
}
void ps2SlowStore8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint8_t value)
{
    PS2_ORIG_WRITE(Store8, FAST_WRITE8, addr, value);
}
void ps2SlowStore16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint16_t value)
{
    PS2_ORIG_WRITE(Store16, FAST_WRITE16, addr, value);
}
void ps2SlowStore32(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint32_t value)
{
    PS2_ORIG_WRITE(Store32, FAST_WRITE32, addr, value);
}
void ps2SlowStore64(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, uint64_t value)
{
    PS2_ORIG_WRITE(Store64, FAST_WRITE64, addr, value);
}
void ps2SlowStore128(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, __m128i value)
{
    PS2_ORIG_WRITE(Store128, FAST_WRITE128, addr, value);
}
#endif
