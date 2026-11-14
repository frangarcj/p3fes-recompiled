#!/bin/sh
# Serialised incremental build of the Mac runtime (several agents share build-mac-runtime).
# usage: mac_build.sh   -> prints only errors; exit status is the build's
root=$(cd "$(dirname "$0")/../../.." && pwd)
lock=/tmp/p3fes-mac-build.lock
while ! mkdir "$lock" 2>/dev/null; do perl -e 'select(undef,undef,undef,2)'; done
trap 'rmdir "$lock"' EXIT
out=$(cmake --build "$root/build-mac-runtime" --target ps2EntryRunner -j10 2>&1); status=$?
echo "$out" | grep -E " error |FAILED|undefined symbol" -A6 | cut -c1-240 | head -60
exit $status
