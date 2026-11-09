// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ipu_compat.h"

MULTI_ISA_DEF(extern void yuv2rgb_reference();)

#if defined(ARCH_X86)

#define yuv2rgb yuv2rgb_sse2
MULTI_ISA_DEF(extern void yuv2rgb_sse2();)

#elif defined(ARCH_ARM64)

#define yuv2rgb yuv2rgb_neon
MULTI_ISA_DEF(extern void yuv2rgb_neon();)

#elif defined(ARCH_ARM32)

// Cortex-A9/NEON lacks several AArch64-only zip/high-lane intrinsics used by
// the optimized path. Keep the precise scalar conversion on Vita for now;
// Sofdec correctness matters more than forcing an incomplete vector port.
#define yuv2rgb yuv2rgb_reference

#endif
