#!/usr/bin/env python3
"""[pc-frame] lines (PS2X_PC_SAMPLE=<ms> with PS2X_FRAME_HIST=1) -> per frame: wall/cpu from the [fdip] line plus the guest
functions (p3fes_map.csv) that spent the time.  usage: pcframe_report.py LOG [first_seq last_seq] [top=6]"""
import re, sys, csv, bisect, collections
fn = []
for r in csv.reader(open('/Users/frangar/Fun/pes3fes/p3fes/p3fes_map.csv')):
    try: fn.append((int(r[1], 16), int(r[2], 16), r[0]))
    except: pass
fn.sort(); ks = [f[0] for f in fn]
def name(pc):
    i = bisect.bisect_right(ks, pc) - 1
    return fn[i][2] if i >= 0 and fn[i][0] <= pc < fn[i][1] else hex(pc)
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 0
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 1 << 60
top = int(sys.argv[4]) if len(sys.argv) > 4 else 6
fd = {}
for l in open(sys.argv[1], errors='replace'):
    m = re.match(r'\[fdip\] seq=(\d+) t=(\S+) wall=(\S+) cpu=(\S+) pace=(\S+) event=(\S+)', l)
    if m: fd[int(m.group(1))] = m.groups()[1:]; continue
    m = re.match(r'\[pc-frame\] seq=(\d+) samples=(\d+) wait=(\d+) busy=(\d+) top:(.*)', l)
    if m and lo <= int(m.group(1)) <= hi:
        s = int(m.group(1)); agg = collections.Counter()
        for t in m.group(5).split():
            a, n = t.split('='); agg[name(int(a, 16))] += int(n)
        f = fd.get(s, ('?',) * 5)
        print(s, 't=%s wall=%s cpu=%s pace=%s evt=%s' % f, '| busy', m.group(4), '|', ' '.join('%s=%d' % (k, v) for k, v in agg.most_common(top)))
