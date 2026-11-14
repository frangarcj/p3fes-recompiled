#!/usr/bin/env python3
"""Ground-truth screenshots of Persona 3 FES from real PCSX2 (software renderer, native 640x448).

usage: pcsx2_ref.py OUT [scene] [--rebuild-states]       scene = lounge (default) | table | both

Everything lives in REF_ROOT (env, default: the scratch dir below), nothing in the owner's PCSX2 data:
  REF_ROOT/dp/PCSX2/        private PCSX2 data dir (PCSX2 appends "PCSX2" to -datapath); bios copied from the
                            owner's dir, ini generated from the owner's ini + overrides, memcard copied fresh
                            from game-mac/memcards/Mcd001.ps2 before every run
  REF_ROOT/image/p3fes.iso  the real disc image (extracted once from the owner's .7z; ISO9660-only images built
                            from the loose files do NOT boot on a DVD-size image, the IOP cdvdman wants UDF)
  REF_ROOT/states/{lounge,table}.p2s   save states made once by driving the menus (see build_states)

Input is synthetic keyboard events posted to the PCSX2 process only (CGEventPostToPid; needs the Accessibility
permission the terminal already has: AXIsProcessTrusted() == 1).  PCSX2 default pad bindings are kept:
Return=START, K=CROSS, arrows=D-pad.  F8 = screenshot, F1 = save state, F2 = next state slot.
"""
import ctypes, ctypes.util, glob, os, re, shutil, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
ROOT = os.environ.get('REF_ROOT', '/private/tmp/claude-501/-Users-frangar-Fun-pes3fes/6bb54d80-04ae-49d3-bdcc-7ddd71bfb4cf/scratchpad/pcsx2ref')
APP = '/Applications/PCSX2.app/Contents/MacOS/PCSX2'
OWNER = os.path.expanduser('~/Library/Application Support/PCSX2')
ARCHIVE = os.path.expanduser('~/Downloads/Shin Megami Tensei - Persona 3 FES (USA).7z')
DP = ROOT + '/dp'
DATA = DP + '/PCSX2'
ISO = ROOT + '/image/p3fes.iso'
STATES = ROOT + '/states'
KEYS = {'RETURN': 36, 'K': 40, 'LEFT': 123, 'F8': 100, 'F1': 122, 'F2': 120}

_cg = ctypes.CDLL(ctypes.util.find_library('CoreGraphics'))
_cg.CGEventCreateKeyboardEvent.restype = ctypes.c_void_p
_cg.CGEventCreateKeyboardEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool]
_cg.CGEventPostToPid.argtypes = [ctypes.c_int, ctypes.c_void_p]


def log(*a):
    print('[pcsx2_ref]', *a, file=sys.stderr, flush=True)


def setup():
    """Create/refresh the private data dir.  Never touches OWNER."""
    for d in ('bios', 'inis', 'memcards', 'snaps', 'sstates', 'logs', 'cache', 'cheats', 'patches', 'resources',
              'textures', 'videos', 'inputprofiles', 'covers', 'gamesettings'):
        os.makedirs(DATA + '/' + d, exist_ok=True)
    for f in glob.glob(OWNER + '/bios/SCPH-39001*'):
        dst = DATA + '/bios/' + os.path.basename(f)
        if not os.path.exists(dst):
            shutil.copy(f, dst)
    # fresh memory card every run (the game may write to it)
    shutil.copy(REPO + '/game-mac/memcards/Mcd001.ps2', DATA + '/memcards/Mcd001.ps2')
    ini = DATA + '/inis/PCSX2.ini'
    if not os.path.exists(ini):
        t = open(OWNER + '/inis/PCSX2.ini').read()
        for k in ('MainWindowGeometry', 'MainWindowState', 'DisplayWindowGeometry'):
            t = re.sub(r'^%s = .*\n' % k, '', t, flags=re.M)

        def setk(sec, key, val):
            nonlocal t
            m = re.search(r'^\[%s\]\n' % re.escape(sec), t, re.M)
            end = t.find('\n[', m.end())
            end = len(t) if end < 0 else end
            body = t[m.end():end]
            if re.search(r'^%s = .*$' % re.escape(key), body, re.M):
                body = re.sub(r'^%s = .*$' % re.escape(key), '%s = %s' % (key, val), body, flags=re.M)
            else:
                body += '%s = %s\n' % (key, val)
            t = t[:m.end()] + body + t[end:]
        for sec, key, val in [
                ('UI', 'InhibitScreensaver', 'false'), ('UI', 'ConfirmShutdown', 'false'),
                ('UI', 'PauseOnFocusLoss', 'false'), ('UI', 'StartFullscreen', 'false'),
                ('EmuCore', 'EnablePatches', 'false'), ('EmuCore', 'EnableCheats', 'false'),
                ('EmuCore', 'EnableWideScreenPatches', 'false'), ('EmuCore', 'EnableNoInterlacingPatches', 'false'),
                ('EmuCore', 'EnablePINE', 'false'), ('EmuCore', 'SaveStateOnShutdown', 'false'),
                ('EmuCore', 'EnableFastBoot', 'true'),
                ('EmuCore/GS', 'Renderer', '13'),            # 13 = software
                ('EmuCore/GS', 'upscale_multiplier', '1'), ('EmuCore/GS', 'AspectRatio', '4:3'),
                ('EmuCore/GS', 'ScreenshotSize', '2'),       # 2 = internal resolution, no aspect correction
                ('EmuCore/GS', 'ScreenshotFormat', '0'), ('EmuCore/GS', 'extrathreads', '2'),
                ('EmuCore/GS', 'deinterlace_mode', '0'),
                ('MemoryCards', 'Slot1_Enable', 'true'), ('MemoryCards', 'Slot1_Filename', 'Mcd001.ps2'),
                ('MemoryCards', 'Slot2_Enable', 'false'),
                ('SPU2/Output', 'OutputMuted', 'true'), ('SPU2/Output', 'Backend', 'Null'),
                ('Logging', 'EnableLogWindow', 'false'), ('InputSources', 'SDL', 'false')]:
            setk(sec, key, val)
        open(ini, 'w').write(t)


def ensure_image():
    if os.path.exists(ISO):
        return
    free = shutil.disk_usage(ROOT).free
    if free < 5.5e9:
        sys.exit('need 5.5 GB free to extract the disc image, have %.1f GB' % (free / 1e9))
    os.makedirs(ROOT + '/image', exist_ok=True)
    subprocess.check_call(['7z', 'x', '-y', '-o' + ROOT + '/image', ARCHIVE], stdout=subprocess.DEVNULL)
    iso = glob.glob(ROOT + '/image/*.iso')[0]
    os.rename(iso, ISO)


class Pcsx2:
    def __init__(self, statefile=None, tag='run'):
        setup()
        self.logfile = '%s/logs/%s.log' % (DATA, tag)
        if os.path.exists(self.logfile):
            os.remove(self.logfile)
        cmd = [APP, '-datapath', DP, '-nogui', '-fastboot', '-logfile', self.logfile]
        if statefile:
            cmd += ['-statefile', statefile]
        cmd += ['--', ISO]
        self.p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.t0 = time.time()

    def alive(self):
        return self.p.poll() is None

    def wait_log(self, pat, timeout):
        end = time.time() + timeout
        while time.time() < end:
            if not self.alive():
                sys.exit('PCSX2 exited early, see ' + self.logfile)
            try:
                if re.search(pat, open(self.logfile, errors='replace').read()):
                    return True
            except OSError:
                pass
            time.sleep(0.5)
        return False

    def key(self, name, hold=0.15):
        code = KEYS[name]
        for down in (True, False):
            _cg.CGEventPostToPid(self.p.pid, _cg.CGEventCreateKeyboardEvent(None, code, down))
            if down:
                time.sleep(hold)
        time.sleep(0.05)

    def shot(self, dest, timeout=10):
        before = set(glob.glob(DATA + '/snaps/*.png'))
        self.key('F8')
        end = time.time() + timeout
        while time.time() < end:
            new = set(glob.glob(DATA + '/snaps/*.png')) - before
            if new:
                time.sleep(0.3)
                shutil.move(new.pop(), dest)
                return dest
            time.sleep(0.2)
        sys.exit('no screenshot appeared (is the game rendering?)')

    def close(self):
        if self.alive():
            self.p.terminate()
            try:
                self.p.wait(8)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait()


def build_states():
    """Drive the menus once (timed recipe, same pad sequence as our port) and save lounge/table states."""
    os.makedirs(STATES, exist_ok=True)
    ensure_image()
    pc = Pcsx2(tag='build')
    try:
        pc.wait_log(r'Pad: DS2 Config Finished', 90)
        time.sleep(25)                       # logos (Sofdec/ADX/Atlus) before the title
        for _ in range(4):                   # START x4 -> mode select
            pc.key('RETURN', 0.2); time.sleep(3)
        for _ in range(2):                   # The Journey -> title menu -> LOAD GAME -> card list
            pc.key('K', 0.2); time.sleep(4)
        pc.key('K', 0.2); time.sleep(3)      # slot 1
        pc.key('K', 0.2); time.sleep(3)      # Load this file? Yes
        pc.key('K', 0.2); time.sleep(5)      # Load successful.
        pc.key('K', 0.2); time.sleep(16)     # -> lounge, camera settled
        pc.shot(STATES + '/build-lounge.png')
        pc.key('F1'); time.sleep(4)
        shutil.copy(DATA + '/sstates/SLUS-21621 (94A82AAA).01.p2s', STATES + '/lounge.p2s')
        pc.key('LEFT', 2.5); time.sleep(3)
        pc.shot(STATES + '/build-table.png')
        pc.key('F2'); time.sleep(0.5); pc.key('F1'); time.sleep(4)
        shutil.copy(DATA + '/sstates/SLUS-21621 (94A82AAA).02.p2s', STATES + '/table.p2s')
    finally:
        pc.close()


def run_scene(scene, out, shots):
    state = '%s/%s.p2s' % (STATES, scene)
    pc = Pcsx2(statefile=state, tag='scene-' + scene)
    try:
        if not pc.wait_log(r'Loading GS', 60):
            sys.exit('game did not start, see ' + pc.logfile)
        time.sleep(6)                        # state load + a few frames
        for i in range(shots):
            pc.shot('%s/pcsx2-%s-%d.png' % (out, scene, i))
            time.sleep(0.5)
    finally:
        pc.close()


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = os.path.abspath(args[0])
    scene = args[1] if len(args) > 1 else 'lounge'
    os.makedirs(out, exist_ok=True)
    ensure_image()
    scenes = ['lounge', 'table'] if scene == 'both' else [scene]
    if '--rebuild-states' in sys.argv or not all(os.path.exists('%s/%s.p2s' % (STATES, s)) for s in scenes):
        log('building states (one-off, ~2 min)')
        build_states()
    for s in scenes:
        run_scene(s, out, int(os.environ.get('REF_SHOTS', '2')))
        log('wrote', out, s)


if __name__ == '__main__':
    main()
