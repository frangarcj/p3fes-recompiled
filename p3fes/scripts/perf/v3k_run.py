#!/usr/bin/env python3
"""Bounded Vita3K run of a given eboot; restores the installed SELF and runtime.cfg.
usage: v3k_run.py <label> <eboot.bin> [seconds]"""
from pathlib import Path
import os, re, subprocess, sys, time
label, eboot = sys.argv[1], Path(sys.argv[2])
secs = int(sys.argv[3]) if len(sys.argv) > 3 else 110
ROOT = Path('/Users/frangar/Fun/pes3fes')
P = ROOT / 'build-vita-native/3d-probe'
GAME = ROOT / 'game-mac'
OUT = Path(__file__).resolve().parent
EXE = ROOT.parent / 'vita3k/build/macos/bin/Vita3K.app/Contents/MacOS/Vita3K'
INSTALLED = EXE.parents[3] / 'portable/fs/ux0/app/P3FES0001/eboot.bin'
assert subprocess.run(['pgrep', '-f', '^' + str(EXE)], capture_output=True).returncode == 1, 'Vita3K already running'
saved_self = INSTALLED.read_bytes()
cfg_path = GAME / 'runtime.cfg'
saved_cfg = cfg_path.read_bytes()
guest_log = GAME / f'cg-{label}.log'
guest_log.write_bytes(b'')
# The opening movie is cut after 1 s (the native player reads the real pad, so
# the scripted pad cannot skip it); X presses then walk title -> Load Game -> slot 1.
script = ','.join(f'{t}:CROSS:0.35' for t in range(26, 51, 2))
cfg = f'''PS2X_LOG_FILE=ux0:data/p3fes/cg-{label}.log
PS2X_PERF_DIAG=frames
PS2X_PERF_FRAME_PC=0x001983A0
PS2X_MEMCARD2=none
P3FES_NATIVE_RW=1
P3FES_NATIVE_MOVIES=1
P3FES_NATIVE_CAMERA=1
P3FES_NATIVE_BGM=1
P3FES_NATIVE_MATH=1
P3FES_NATIVE_RW_MATH=1
P3FES_RESOURCE_DIAG=1
PS2X_IOP_IDLE_FAST=1
PS2X_BRANCH_RING=0
P3FES_GPU_SKIN=0
P3FES_MOVIE_TEST_SKIP_SECONDS=1
PS2X_PAD_SCRIPT_CLOCK=wall
PS2X_PAD_SCRIPT={script}
'''
child = None
try:
    INSTALLED.write_bytes(eboot.read_bytes())
    cfg_path.write_text(cfg)
    env = dict(os.environ, VITA3K_MUTE='1', VITA3K_KEEP_ACTIVE='1',
               DYLD_INSERT_LIBRARIES=str(P / 'libvita3k_keep_active.dylib'),
               VITA3K_AUTO_SCREENSHOT=f'{secs - 5}')
    with (OUT / f'v3k-{label}.host.log').open('w') as out:
        child = subprocess.Popen([str(EXE), '-r', 'P3FES0001', '--config-location', str(P / 'branch-ring-test-vita3k.yml'),
                                  '--load-config', '--keep-config'], env=env, stdout=out, stderr=subprocess.STDOUT)
        start = time.monotonic()
        while child.poll() is None and time.monotonic() - start < secs:
            time.sleep(1)
finally:
    if child and child.poll() is None:
        child.kill(); child.wait()
    INSTALLED.write_bytes(saved_self)
    cfg_path.write_bytes(saved_cfg)
text = guest_log.read_text(errors='replace')
(OUT / f'v3k-{label}.guest.log').write_text(text)
guest_log.unlink()
speeds = re.findall(r'\[speed\] ([0-9.]+) vblanks/s \((\d+)% of a PS2\), ([0-9.]+) game frames/s', text)
fps = re.findall(r'\[native-present\]\s+fps=([0-9.]+)', text)
print(label, 'exit', child.returncode if child else None)
print('speed vblanks/s:', ' '.join(s[0] for s in speeds))
print('game frames/s  :', ' '.join(s[2] for s in speeds))
print('native presents:', ' '.join(fps))
