#!/bin/sh
# One bounded run of the Mac build on the Vita's native path, with captures and an optional profile.
# usage: mac_run.sh OUTDIR LABEL SECONDS [VAR=value ...]
#   OUTDIR/LABEL.log            runtime log
#   OUTDIR/LABEL/frame-N.p3fb   captures asked with P3FES_FRAME_DIAG=N[,N...] (seconds since first frame);
#                               convert with p3fb2png.py
#   OUTDIR/LABEL.sample         if SAMPLE_AT=<second> is in the environment (SAMPLE_SECONDS, default 10)
# Default pad script loads memory-card slot 1 (the dorm lounge, reached ~90 s in); override with
# PS2X_PAD_SCRIPT=... as a VAR. Sound drivers are skipped unless SFX=1 is in the environment.
# Runs are independent: several can run at once with different LABELs.
out=$1; label=$2; secs=$3; shift 3
root=$(cd "$(dirname "$0")/../../.." && pwd)
mkdir -p "$out/$label"; out=$(cd "$out" && pwd)
script=$(python3 -c "print(','.join([f'{t}:START:0.3' for t in (20,23,26,29)]+[f'{t}:CROSS:0.35' for t in range(34,74,2)]))")
skip="PS2X_IOP_SKIP_IRX=CRI_ADXI,SDRDRV,LIBSD,MODHSYN,MODMSIN,MODSESQ,MODMIDI,SKSOUND,SKHSYNTH,SKSESQ,SKMIDI,SKMSIN"
audio="PS2X_AUDIO=0"
if [ "$SFX" = 1 ]; then skip="P3FES_UNUSED=1"; audio="PS2X_AUDIO_DUMP=$out/$label.spu2.raw"; rm -f "$out/$label.spu2.raw"; fi
cd "$root/game-mac" || exit 1
env P3FES_NATIVE_RW=1 P3FES_NATIVE_CAMERA=1 P3FES_NATIVE_MATH=1 P3FES_NATIVE_RW_MATH=1 P3FES_NATIVE_BGM=1 P3FES_RESOURCE_DIAG=1 \
    PS2X_PAD_SCRIPT_CLOCK=wall PS2X_PAD_SCRIPT="$script" PS2X_CALLBACK_STACK_TOP=0x100000 PS2X_HIDE_DEBUG_UI=1 \
    PS2X_PERF_DIAG=frames PS2X_PERF_FRAME_PC=0x001983A0 P3FES_CAPTURE_DIR="$out/$label" "$skip" "$audio" "$@" \
    "$root/build-mac-runtime/ps2xRuntime/ps2EntryRunner" "$PWD/SLUS_216.21" > "$out/$label.log" 2>&1 &
pid=$!
if [ -n "$SAMPLE_AT" ]; then
    perl -e "sleep $SAMPLE_AT"
    sample $pid "${SAMPLE_SECONDS:-10}" 1 -mayDie -file "$out/$label.sample" > /dev/null 2>&1
    rest=$((secs - SAMPLE_AT - ${SAMPLE_SECONDS:-10})); [ "$rest" -gt 0 ] && perl -e "sleep $rest"
else
    perl -e "sleep $secs"
fi
kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null
grep "^\[speed\]" "$out/$label.log" | tail -2 | cut -c1-90
ls "$out/$label" 2>/dev/null | tr '\n' ' '; echo
