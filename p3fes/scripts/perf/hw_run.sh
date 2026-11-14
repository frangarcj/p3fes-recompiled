#!/bin/sh
# usage: hw_run.sh VPK CFG SECONDS OUTLOG  -> install, configure, screen on, launch, wait, fetch log
S=/private/tmp/claude-501/-Users-frangar-Fun-pes3fes/ed492313-3244-4318-a85d-1162aa27274b/scratchpad
printf 'quit all\n' | nc -w 5 192.168.0.25 1338 >/dev/null; sleep 2
[ -n "$1" ] && python3 ~/Fun/flycast-vita-sync-lab/scripts/vita_remote.py install "$1" 2>&1 | tail -1
curl -s -T "$2" ftp://192.168.0.25:1337/ux0:/data/p3fes/runtime.cfg
printf 'screen on\n' | nc -w 5 192.168.0.25 1338; sleep 1
printf 'launch P3FES0001\n' | nc -w 5 192.168.0.25 1338 >/dev/null
perl -e "sleep $3"
printf 'screen on\n' | nc -w 5 192.168.0.25 1338 >/dev/null
curl -s -m 60 ftp://192.168.0.25:1337/ux0:/data/p3fes/runtime.log -o "$4"
wc -l < "$4"
