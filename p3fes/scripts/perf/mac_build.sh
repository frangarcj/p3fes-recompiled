#!/bin/sh
# Serialised incremental build of the Mac runtime (several agents share build-mac-runtime).
# usage: mac_build.sh   -> prints only errors; exit status is the build's
root=$(cd "$(dirname "$0")/../../.." && pwd)
lock=/tmp/p3fes-mac-build.lock
while ! mkdir "$lock" 2>/dev/null; do perl -e 'select(undef,undef,undef,2)'; done
trap 'rmdir "$lock"' EXIT
# The command-line tools' default SDK (27.2 since 2026-11-18) fails to link the runtime dylib with this linker
# ("tapi error: unknown architecture arm64e.x1-macos"): build against the 26.5 SDK while it is installed.
sdk=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
[ -z "$SDKROOT" ] && [ -d "$sdk" ] && export SDKROOT="$sdk"
out=$(cmake --build "$root/build-mac-runtime" --target ps2EntryRunner -j10 2>&1); status=$?
echo "$out" | grep -E " error |FAILED|undefined symbol" -A6 | cut -c1-240 | head -60
exit $status
