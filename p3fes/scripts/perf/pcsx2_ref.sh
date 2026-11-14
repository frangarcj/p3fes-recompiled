#!/bin/sh
# Ground-truth screenshots from real PCSX2 (software renderer, 640x448, 4:3) of P3FES.
# usage: pcsx2_ref.sh OUT [lounge|table|both]
#   OUT/pcsx2-<scene>-<n>.png   (REF_SHOTS=2 per scene by default)
# Private data dir, ISO and save states live in REF_ROOT (see pcsx2_ref.py); the owner's PCSX2 data is only
# read (BIOS + ini template).  Hard-bounded by a 300 s watchdog; PCSX2 is killed on exit.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "$1" ] || { echo "usage: $0 OUT [lounge|table|both]" >&2; exit 2; }
python3 -I "$here/pcsx2_ref.py" "$@" &
pid=$!
( sleep "${REF_TIMEOUT:-300}"; kill $pid 2>/dev/null; pkill -f 'PCSX2.app/Contents/MacOS/PCSX2 -datapath' 2>/dev/null ) >/dev/null 2>&1 &
wd=$!
wait $pid; rc=$?
pkill -P $wd 2>/dev/null; kill $wd 2>/dev/null; wait $wd 2>/dev/null
pkill -f 'PCSX2.app/Contents/MacOS/PCSX2 -datapath' 2>/dev/null
exit $rc
