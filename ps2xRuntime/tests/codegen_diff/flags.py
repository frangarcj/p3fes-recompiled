#!/usr/bin/env python3
"""Print include dirs / defines taken from a Unity/unity_*_cxx.cxx entry of compile_commands.json."""
import json, shlex, sys
db = json.load(open(sys.argv[1]))
for e in db:
    if '/Unity/unity_' in e['file'] and e['file'].endswith('_cxx.cxx'):
        toks = shlex.split(e.get('command') or ' '.join(e['arguments']))
        out, i = [], 1
        while i < len(toks):
            t = toks[i]
            if t in ('-isystem',):
                out += [t, toks[i + 1]]; i += 2; continue
            if t.startswith(('-D', '-I', '-std=')):
                out.append(t)
            elif t == '-arch':
                out += [t, toks[i + 1]]; i += 2; continue
            i += 1
        print(' '.join(shlex.quote(x) for x in out))
        break
