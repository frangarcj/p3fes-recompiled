# ps2xIOP

`ps2xIOP` runs original IRX modules on an R3000A interpreter, with a virtual
IOP kernel providing imports without a PS2 BIOS. The C++20 static library
`ps2_iop` / `ps2x::iop` is linked into `ps2xRuntime`.

## Execution policy

Game-specific IOP code executes from IRX modules. There is no game-profile
selection or native profile-plugin loader. A physical IRX RPC server is
authoritative for its SID.

Generic HLE services remain available when no loaded IRX provides an endpoint:

| Service | SID | Activation |
| --- | --- | --- |
| MCSERV | `0x80000400`, `0x80000480` | Recognized module load |
| LIBSD | `0x80000701` | Recognized module load |
| DBCMAN | `0x80001300` | Recognized module load |

These services are dormant before module load and after reset or the final
module stop. Unknown modules fail to load; unknown RPC SIDs remain unhandled.
Games previously using TSNDDRV, CRI DTX, CLFILE, SOUND or SDRDRV profiles now
require their IRX modules and support for the imports and hardware they use.

## Lifecycle and transport

- `reset()` clears loaded modules, HLE service state and emulator state.
- `loadModule(...)` / `loadModuleBuffer(...)` load and start an IRX.
- `stopModule(...)` releases a module and its owned state.
- `runEeCycles(...)` advances the IOP from EE cycle accounting.
- `selectRpcAbi(...)`, `handleRpc(...)` and `onSifTransfer(...)` connect SIF transport.

IOP RAM is separate from EE RAM. The transport copies data through the IOP
memory accessors; SIF notifications do not mirror bytes into equal-numbered EE
addresses. `RpcResult` describes completion and dispatch actions for the runtime.

Link with `target_link_libraries(my_runtime PRIVATE ps2x::iop)`. The public API
is [iop_subsystem.h](include/ps2x/iop/iop_subsystem.h); `PS2Runtime` owns its
subsystem and host adapter.

`runEeCycles()` uses the kernel's versioned idle cache to avoid repeated scheduler
and SPU2 service calls for short idle intervals. It requires no ready thread,
callback, timer or DMA completion before the target, and no SPU2 sample or pending
IRQ/ADMA notification through the target. Other intervals retain the original loop
and final SPU2 service. Kernel mutations invalidate the idle cache; scheduling and
event queues retain their existing single-executor ownership. Set
`PS2X_IOP_IDLE_FAST=0` before constructing the subsystem to disable the fast path.

## Diagnostics and tests

`debugSnapshot()` exposes emulator cycle/instruction counts, loaded module,
thread and RPC-server counts, generic service metrics and load diagnostics.
The runtime debugger renders these in the **IOP/SIF** tab.

Build standalone tests with:

```sh
cmake -S ps2xIOP -B out/build/iop-tests -DPS2X_IOP_BUILD_TESTS=ON
cmake --build out/build/iop-tests
ctest --test-dir out/build/iop-tests --output-on-failure
```

The suites cover IRX execution, RPC, imports, version resolution, generic HLE
compatibility, fragmented SPU2 clocks and idle-path A/B comparisons with thread
wakes, timer callbacks and DMA interrupts. `ps2x_tests` also covers runtime SIF
RPC/DMA integration.

For aggregate scheduling diagnostics, configure with
`-DPS2X_IOP_WORK_DIAGNOSTICS=ON` and run with `PS2X_IOP_WORK_DIAG=1`.
The option defaults to OFF and removes the hot counter branches from normal
builds. The resulting `[IOP:work]` windows report calls, iterations and emulated
cycles, not host CPU time. With diagnostics enabled, the idle A/B test also checks
that the fast path was exercised.

## Burst interpreter (`PS2X_IOP_FAST=1`)

Off by default (unset or `0` runs the original interpreter loop). Read once when the emulator is created.
With `PS2X_IOP_FAST=1`, `IopEmulator::Impl::runCpu` runs straight-line register/RAM code through
`IopCpuCore::runBurst` (registers, PC and branch/load-delay state in locals, RAM accessed directly) and hoists the
checks `step()` makes per instruction (stop, sentinels, RAM range, pending interrupt, due DMA/guest callbacks, PC
sampling) out of the loop. A burst ends *before* the first instruction that could affect any of them (COP0,
syscall/break, overflowing add/sub, misaligned or non-RAM load/store, a `jr ra` that may be an import stub, a fetch
outside RAM); that instruction runs through `step()` as before, so timing, interrupts and DMA are unchanged. The
import dispatcher classifies the library once per call instead of comparing names in turn, and `resolve()` is
memoised until the set of export libraries changes. `tests/iop_fast_equivalence_tests.cpp` compares full CPU state
and RAM against `executeFetchedInstruction` on random instruction streams (`--bench` prints timings);
`tests/iop_fast_integration_tests.cpp` runs the same synthetic IRX with the switch off and on and compares cycle and
instruction counters, guest memory and logs (`--bench` times a compute loop).

Since 2026-11-14 the burst interpreter is ON by default; `PS2X_IOP_FAST=0` selects the original loop. Round 2 added, all
exact (the real-driver state hash is identical with `PS2X_IOP_FAST=0` and `PS2X_IOP_IDLE_FAST=0`):

- `runBurst` runs a taken branch together with a simple delay-slot instruction, and a load together with the next
  simple instruction, so no branch/load-delay state is carried between iterations; that state lives in `IopCpuState`.
- `runCycles` jumps over an idle interval even when SPU2 samples fall due in it (`IdleJump::SpuCatchUp`), and skips
  `serviceSpu2`, DMA, callback and timer servicing through inline "nothing due" tests.
- `Spu2` counts silent output frames (`m_silentPending`) and writes them to the ring only when the ring is read or real
  frames follow, instead of a mutex + memset per call.
- `IopImportRegistry::decode` reads RAM directly; the import dispatcher classifies the library with a hash lookup.

Diagnostics: `tests/iop_real_driver_bench.cpp` (`ps2_iop_real_driver_bench <game>/LIB31 [guest s] [warm-up] [chunk]`)
loads the game's real sound drivers without the game and prints guest instructions per guest second and, on Apple
Silicon, exact host instructions per guest instruction and per `runEeCycles` call, plus a state hash.
`PS2X_IOP_PROFILE=<file>` (with `PS2X_IOP_FAST=0`, optionally `PS2X_IOP_PROFILE_FROM=<instruction>`) writes a guest PC
histogram, per-thread instruction counts, import call counts per thread, hardware accesses and the running timers,
plus `<file>.ram` (IOP RAM image) every 2 M instructions.

## SPU2 lazy service and the IOP worker hand-off (2026-11-23)

- `PS2X_SPU2_LAZY` (default on, `=0` restores the old behaviour): the emulator calls `Spu2::serviceTo()` where it used to
  call `advanceTo()`. The SPU2 clock moves exactly as before, but the samples that fell due are only counted and are
  rendered in one go when something could tell the difference: any register read or write, sound-RAM or DMA access,
  PCM-stream call, a possible IRQ or ADMA event (IRQA armed while a voice plays, ADMA data flowing, an event waiting), a
  bound of 96 samples, or the end of a `runCycles` (`settle()`). The PCM is bit-identical to rendering at every service
  point. Proof: `spu2_equivalence_tests` runs every seed twice (eager, then lazy with owed samples left across operations
  and an interrupt / ADMA-completion check before anything is settled) against the frozen reference, and a 128 s Mac
  battle run's `PS2X_SPU2_TAP` stream is identical with the switch on and off.
- `renderFrames` runs voice by voice into per-frame accumulators and reads each voice's registers once per call
  (`VoiceCache`); the diagnostic build (`PS2X_SPU2_DIAG`) keeps the original frame-major loop.
- `PS2X_SPU2_TAP=<file>` writes every frame the SPU2 produces (silence included) as s16le stereo, in order, before any
  consumer or drop: a pure record for comparing two builds bit for bit (the audio dump goes through the sound pipe and
  depends on wall-clock timing).
- Host side (`ps2xRuntime/src/lib/ps2_iop_host.cpp`): `PS2X_IOP_STATS=1` prints one `[iopstats]` line every 5 s (worker
  CPU / run / asleep, worker and sync lag, wake latency, per-sync-kind wait / hold / inline totals, RPC round trip,
  lock acquisitions per second per mutex, SPU2 counters and render time, size histogram of the EE's cycle posts).
  `PS2X_IOP_WAKE_FIX=0` brings back the wrapping wake predicate that made the worker spin after every inline catch-up.
  `PS2X_IOP_NOCATCHUP=<mask>` (default 0, exact) lets syncs of the given kinds skip the inline catch-up (bit 0 Rpc, 1 Sif,
  2 Mem, 3 Module, 4 Reset, 5 Query); not reproducible, see the comment in the source.
