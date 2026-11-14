#!/bin/sh
# Mac bench of the Vita native path: loads memory-card slot 1 by pad script, samples the process for
# $SAMPLE_SECONDS (default 10) s in the scene and prints the game thread's busy time split by category and its top symbols.
#   mac_bench.sh LABEL [extra VAR=value ...]     (run from the repository root)
# Output files go to $P3FES_BENCH_OUT (default: /tmp/p3fes-bench).
label=$1; shift
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${P3FES_BENCH_OUT:-/tmp/p3fes-bench}; mkdir -p "$out"
script=$(python3 -c "print(','.join([f'{t}:START:0.3' for t in (20,23,26,29)]+[f'{t}:CROSS:0.35' for t in range(34,74,2)]))")
cd "$root/game-mac" || exit 1
env P3FES_NATIVE_RW=1 P3FES_NATIVE_CAMERA=1 P3FES_NATIVE_MATH=1 P3FES_NATIVE_RW_MATH=1 P3FES_RESOURCE_DIAG=1 \
    PS2X_PAD_SCRIPT_CLOCK=wall PS2X_PAD_SCRIPT="$script" PS2X_CALLBACK_STACK_TOP=0x100000 PS2X_HIDE_DEBUG_UI=1 \
    PS2X_AUDIO=0 PS2X_PERF_DIAG=frames PS2X_PERF_FRAME_PC=0x001983A0 "$@" \
    "$root/build-mac-runtime/ps2xRuntime/ps2EntryRunner" "$PWD/SLUS_216.21" > "$out/$label.log" 2>&1 &
pid=$!
perl -e 'sleep 100'
sample $pid "${SAMPLE_SECONDS:-10}" 1 -mayDie -file "$out/$label.sample" > /dev/null 2>&1
kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null
grep "^\[speed\]" "$out/$label.log" | tail -3 | sed -E 's/\[speed\] ([0-9.]+) vblanks.*, ([0-9.]+) game.*/vblanks\/frames \1\/\2/' | tr '\n' ' '; echo
grep "native-frame" "$out/$label.log" | tail -1 | cut -c1-60
python3 "$root/p3fes/scripts/perf/sample_thread.py" "$out/$label.sample" "${TOP:-25}"
