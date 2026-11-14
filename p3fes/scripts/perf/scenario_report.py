#!/usr/bin/env python3
"""Summarise one scenario run of scenarios.sh.
usage: scenario_report.py OUTDIR LABEL FROM_S TO_S
FROM_S..TO_S are process seconds (the [perf] t= clock) of the window the scene was held on screen.
Prints: mean game frames/s of the [speed] lines in the window, the game thread's busy share and top self-time
symbols from OUTDIR/LABEL.sample (macOS `sample`, via sample_thread.py), and the [ee-sample] top functions of the
5-second windows that lie inside FROM_S..TO_S (via the same name table as sample_report.py)."""
import re, sys, csv, bisect, collections, subprocess, os

out, label, t0, t1 = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
here = os.path.dirname(os.path.abspath(__file__))
log = open(f'{out}/{label}.log', errors='replace').read()
for tag in ('[ee-sample]', '[speed]', '[perf]'):
    log = log.replace(tag, '\n' + tag)
lines = log.splitlines()

# --- fps: every [speed] line gets the process time of the [perf] line just before it
now = 0.0
fps = []
for l in lines:
    m = re.match(r'\[perf\] t=([0-9.]+)', l)
    if m:
        now = float(m.group(1))
    m = re.match(r'\[speed\] ([0-9.]+) vblanks/s.*?, ([0-9.]+) game frames/s', l)
    if m and t0 <= now <= t1 + 5.5:
        fps.append(float(m.group(2)))
print('fps_samples', len(fps), 'fps_mean %.1f' % (sum(fps) / len(fps) if fps else float('nan')),
      'min %.1f' % (min(fps) if fps else float('nan')))

# --- macOS sample of the game thread
sample = f'{out}/{label}.sample'
if os.path.exists(sample):
    r = subprocess.run([sys.executable, f'{here}/sample_thread.py', sample, '8'], capture_output=True, text=True)
    txt = r.stdout
    m = re.search(r'game thread samples (\d+), busy (\d+) \(([0-9.]+)%\)', txt)
    print('busy_pct', m.group(3) if m else 'n/a')
    print('--- sample categories + top 8 self-time symbols (game thread)')
    print(txt.strip() if txt.strip() else r.stderr.strip()[-300:])
else:
    print('busy_pct n/a (no .sample)')

# --- ee-sample windows (5 s each, window k covers about [5(k-1), 5k] of process time)
starts = [i for i, l in enumerate(lines) if l.startswith('[ee-sample] total=')]
want = [k for k in range(len(starts)) if 5 * k >= t0 - 1 and 5 * (k + 1) <= t1 + 1]
fn = []
for r in csv.reader(open(os.path.join(here, '../../p3fes_map.csv'))):
    try:
        fn.append((int(r[1], 16), int(r[2], 16), r[0]))
    except Exception:
        pass
fn.sort()
ks = [f[0] for f in fn]


def name(pc):
    if pc == 0:
        return '(top level / scheduler / wait)'
    if pc == 0xFFFFFFF0:
        return '(IOP)'
    i = bisect.bisect_right(ks, pc) - 1
    return fn[i][2] if i >= 0 and fn[i][0] <= pc < fn[i][1] else hex(pc)


agg = collections.Counter()
tot = 0
for k in want:
    st = starts[k]
    m = re.match(r'\[ee-sample\] total=(\d+)', lines[st])
    tot += int(m.group(1))
    for l in lines[st + 1:]:
        m = re.match(r'\[ee-sample\] pc=([0-9A-F]+) n=(\d+)( native)?', l)
        if not m:
            if l.startswith('[ee-sample] total'):
                break
            continue
        agg[(name(int(m.group(1), 16)), bool(m.group(3)))] += int(m.group(2))
print('--- ee-sample (dispatcher level) windows', want, 'of', len(starts))
for (nm, isn), n in agg.most_common(8):
    print('%5.1f%%  %s%s' % (n * 100 / max(tot, 1), 'NATIVE ' if isn else '', nm))
