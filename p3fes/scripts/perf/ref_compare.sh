#!/bin/sh
# Capture the same moments from the native path (P3FES_NATIVE_RW=1, what the Vita uses) and from the GS path
# (P3FES_NATIVE_RW=0: the game's real GS packets rasterised by the emulated Graphics Synthesizer, the reference),
# then write per-moment comparison pictures.
# usage: ref_compare.sh OUT [seconds...]          (default: 100 110 120)
#   OUT/native/frame-S.p3fb  OUT/gs/frame-S.p3fb  OUT/native.log  OUT/gs.log   raw captures and logs
#   OUT/S/native.png  gs.png  side.png (native | gs)  diff.png (|native-gs| x3)  stats.txt (luma per 8x7 tile)
# Environment:
#   REF_PAD_EXTRA="110:LEFT:2.5,..."  extra pad presses appended to the default script (times: emulated seconds
#                                     since the first pad read, see PS2X_PAD_SCRIPT_CLOCK=fields)
#   REF_PAD="..."                     replace the whole pad script
#   REF_VARS="VAR=value ..."          extra environment for both runs
#   REF_ONLY=native|gs                run just one side (then still rebuilds the pictures if both exist)
#   REF_GS_TIMEOUT=3000               wall seconds the GS run may take (it can run at 10-50% of real time)
# The native run uses P3FES_WIDESCREEN=0: by default the native path widens the 3D view to 16:9 (aspect written into the
# game), which is not comparable with the GS path's 4:3 picture in a 4:3 desktop window.
# Clocks. Both runs use PS2X_PAD_SCRIPT_CLOCK=fields, so the pad script hits the same emulated moments however
# slow the host is. S is "seconds after the first presented frame": wall time on the native path (which runs at
# ~100% of a PS2), emulated time (vsyncs / 59.94, P3FES_FRAME_DIAG_CLOCK=fields) on the GS path. The first
# presented frame differs slightly (native: first native draw; GS: first non-blank GS frame), so the moments agree
# to about a second or two, not exactly. Check the pictures. The GS run needs the build to include the GS capture
# in gs_host_present.cpp (sh p3fes/scripts/perf/mac_build.sh).
# The GS reference here is the built-in CPU rasteriser unless pcsx2gs.so sits next to ps2EntryRunner.
out=$1; [ -n "$out" ] || { sed -n 2,25p "$0"; exit 2; }; shift
secs="${*:-100 110 120}"
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
mkdir -p "$out"; out=$(cd "$out" && pwd)
list=$(echo $secs | tr ' ' ',')
last=$(echo $secs | tr ' ' '\n' | sort -n | tail -1 | cut -d. -f1)

if [ -n "$REF_PAD" ]; then script=$REF_PAD; else
  # same default as mac_run.sh: load memory-card slot 1 (START x4, then CROSS every 2 s)
  script=$(python3 -c "print(','.join([f'{t}:START:0.3' for t in (20,23,26,29)]+[f'{t}:CROSS:0.35' for t in range(34,74,2)]))")
  [ -n "$REF_PAD_EXTRA" ] && script="$script,$REF_PAD_EXTRA"
fi
skip="PS2X_IOP_SKIP_IRX=CRI_ADXI,SDRDRV,LIBSD,MODHSYN,MODMSIN,MODSESQ,MODMIDI,SKSOUND,SKHSYNTH,SKSESQ,SKMIDI,SKMSIN"

# run LABEL TIMEOUT VAR=value...  : launch, wait until every capture exists (or TIMEOUT wall seconds), kill.
run() {
  label=$1; limit=$2; shift 2
  mkdir -p "$out/$label"; rm -f "$out/$label"/frame-*.p3fb
  cd "$root/game-mac" || exit 1
  env PS2X_PAD_SCRIPT_CLOCK=fields PS2X_PAD_SCRIPT="$script" PS2X_CALLBACK_STACK_TOP=0x100000 PS2X_HIDE_DEBUG_UI=1 \
      PS2X_AUDIO=0 "$skip" P3FES_FRAME_DIAG="$list" P3FES_CAPTURE_DIR="$out/$label" $REF_VARS "$@" \
      "$root/build-mac-runtime/ps2xRuntime/ps2EntryRunner" "$PWD/SLUS_216.21" > "$out/$label.log" 2>&1 &
  pid=$!
  waited=0
  while [ $waited -lt "$limit" ]; do
    perl -e 'select(undef,undef,undef,5)'; waited=$((waited + 5))
    kill -0 $pid 2>/dev/null || break
    n=0; for s in $secs; do [ -s "$out/$label/frame-${s%%.*}.p3fb" ] && n=$((n + 1)); done
    [ $n -eq $(echo $secs | wc -w) ] && { perl -e 'select(undef,undef,undef,2)'; break; }
  done
  kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null; { wait $pid; } 2>/dev/null
  echo "$label: $(ls "$out/$label" | tr '\n' ' ')after ${waited}s wall; $(grep '^\[speed\]' "$out/$label.log" | tail -1 | cut -c1-60)"
}

if [ "$REF_ONLY" != gs ]; then
  run native $((last + 60)) P3FES_NATIVE_RW=1 P3FES_NATIVE_CAMERA=1 P3FES_NATIVE_MATH=1 P3FES_NATIVE_RW_MATH=1 \
      P3FES_RESOURCE_DIAG=1 P3FES_WIDESCREEN=0
fi
if [ "$REF_ONLY" != native ]; then
  run gs "${REF_GS_TIMEOUT:-3000}" P3FES_NATIVE_RW=0 P3FES_FRAME_DIAG_CLOCK=fields
fi
python3 "$here/ref_compare.py" "$out" $secs
