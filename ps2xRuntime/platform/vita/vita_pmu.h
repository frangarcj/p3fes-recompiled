// Optional Vita diagnostic: Cortex-A9 PMU counters of the game (EE) thread via
// libperf. Compiled only when CMake option PS2X_VITA_LIBPERF=ON; the runtime
// switch is PS2X_VITA_PMU=1|2|3 in ux0:data/p3fes/runtime.cfg (see vita_pmu.cpp).
#pragma once

#if defined(PS2X_VITA_LIBPERF)

#ifdef __cplusplus
extern "C" {
#endif

// Both are touched only by the game thread; plain ints on purpose.
extern int ps2x_vita_pmu_active;
extern int ps2x_vita_pmu_countdown;

// Game thread, once, at thread start. Loads libperf (if PS2X_VITA_PMU is set),
// binds the counters to the calling thread and starts them. Never fails hard:
// on any error it logs one "[vita:pmu]" line and leaves ps2x_vita_pmu_active 0.
void ps2x_vita_pmu_init_game_thread(void);

// Game thread, from PS2Runtime::eeCheckpointSlow. Reads the counters and
// accumulates 64-bit totals when enough wall time has passed.
void ps2x_vita_pmu_sample(void);

// Any other thread (the 5 s host report): prints finished windows as
// "[vita:pmu] ..." lines to stderr. Cheap when there is nothing new.
void ps2x_vita_pmu_report(void);

// Inline fast path for the hot hook: one load + branch while off.
static inline void ps2x_vita_pmu_tick(void)
{
    if (ps2x_vita_pmu_active && --ps2x_vita_pmu_countdown <= 0)
        ps2x_vita_pmu_sample();
}

#ifdef __cplusplus
}
#endif

#endif // PS2X_VITA_LIBPERF
