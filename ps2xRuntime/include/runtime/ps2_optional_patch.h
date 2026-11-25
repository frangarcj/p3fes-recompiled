#pragma once
#include <cstdint>

struct R5900Context;

// Called at a recompiled guest function entry, including scheduler resumes.
extern "C" bool ps2xRuntimePatchEnabled(const char *option, uint32_t entry,
                                       const uint8_t *ram, const R5900Context *ctx);
