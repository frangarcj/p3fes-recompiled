#!/usr/bin/env python3
"""Compare the outputs of the legacy and new test binaries. Exit 0 on PASS."""
import sys, collections

def load(path):
    d = {}
    for line in open(path):
        p = line.rstrip('\n').split(' ', 3)
        if len(p) < 3: continue
        d[(p[0], int(p[1]))] = ' '.join(p[2:])
    return d

legacy, new = load(sys.argv[1]), load(sys.argv[2])
keys = sorted(set(legacy) | set(new))
same_ok = same_fail = 0
mism = []
fail_reasons = collections.Counter()
for k in keys:
    a, b = legacy.get(k), new.get(k)
    if a == b:
        if a.startswith('OK'): same_ok += 1
        else:
            same_fail += 1
            fail_reasons[a] += 1
    else:
        mism.append((k, a, b))
funcs_ok = {k[0] for k in keys if legacy.get(k, '').startswith('OK') and legacy.get(k) == new.get(k)}
funcs_all = {k[0] for k in keys}
print(f'functions in list: {len(funcs_all)}, functions with >=1 comparable seed: {len(funcs_ok)}')
print(f'(function,seed) pairs: total={len(keys)} compared-identical-OK={same_ok} '
      f'dropped(identical failure in both)={same_fail} MISMATCH={len(mism)}')
print('drop reasons:', dict(fail_reasons))
for (n, s), a, b in mism:
    print(f'MISMATCH {n} seed={s}\n   legacy: {a}\n   new   : {b}')
print(f'{"PASS" if not mism and same_ok else "FAIL"}: {same_ok} function x seed pairs compared')
sys.exit(0 if not mism and same_ok else 1)
