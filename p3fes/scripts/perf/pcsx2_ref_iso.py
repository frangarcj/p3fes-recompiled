#!/usr/bin/env python3
"""Build a bootable PS2 DVD image for PCSX2 from the loose game files, without copying them.

usage: pcsx2_ref_iso.py GAME_DIR OUT.iso [--real-mb N]

The disk may not have room for a 4.5 GB image, so the image is assembled as an APFS clone of
DATA.CVM (clonefile, shares blocks, costs no space) that is patched in place:

  * sectors 16/17 of DATA.CVM lie inside the zero-filled system area of the CVM's inner ISO, so the
    outer ISO9660 primary volume descriptor + terminator are written there (the file itself still
    starts at LBA 0 and is byte-identical for the game, which reads it through the CD file system);
  * everything else (path tables, directories, SYSTEM.CNF, the ELF, LIB31/*.IRX) is appended after it;
  * BTL.CVM and BGM.CVM are appended too, but only their first --real-mb MB (default 4: CVM header +
    inner directory tree) are real data, the rest is a sparse hole (reads as zeros), so the lounge
    route works but battle data / music are missing.  Pass --real-mb 0 for full copies if space allows.

Names are ISO9660 level 1 (upper case, ";1").  Needs the output on the same APFS volume as DATA.CVM.
"""
import os, struct, subprocess, sys, time

SEC = 2048


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def dtime7(t):
    g = time.gmtime(t)
    return bytes([g.tm_year - 1900, g.tm_mon, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, 0])


def dtime17(t):
    g = time.gmtime(t)
    return time.strftime('%Y%m%d%H%M%S', g).encode() + b'00\x00'


def dirrec(name, lba, size, isdir, t):
    ident = name if isinstance(name, bytes) else name.encode()
    n = len(ident)
    ln = 33 + n + (0 if n % 2 else 1)
    return (bytes([ln, 0]) + both32(lba) + both32(size) + dtime7(t) + bytes([2 if isdir else 0, 0, 0]) +
            both16(1) + bytes([n]) + ident + (b'' if n % 2 else b'\0'))


def pad_sectors(n):
    return (n + SEC - 1) // SEC


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    real_mb = 4
    if '--real-mb' in sys.argv:
        real_mb = int(sys.argv[sys.argv.index('--real-mb') + 1])
        args = [a for a in args if a != str(real_mb)]
    game, out = args[0], args[1]
    t = int(time.time())
    if os.path.exists(out):
        os.remove(out)
    subprocess.check_call(['cp', '-c', os.path.join(game, 'DATA.CVM'), out])   # APFS clone
    data_secs = os.path.getsize(out) // SEC
    assert os.path.getsize(out) % SEC == 0

    # (name, hostpath, real_bytes or None=all)
    root_files = [('SYSTEM.CNF', 'SYSTEM.CNF'), ('SLUS_216.21', 'SLUS_216.21'),
                  ('BTL.CVM', 'BTL.CVM'), ('BGM.CVM', 'BGM.CVM')]
    lib = sorted(f for f in os.listdir(os.path.join(game, 'LIB31'))
                 if os.path.isfile(os.path.join(game, 'LIB31', f)) and f.upper().endswith(('.IRX', '.IMG')))

    # ---- layout of the appended area
    cur = data_secs
    pt_l, pt_m = cur, cur + 1
    cur += 2
    root_lba = cur
    cur += 1
    lib_lba = cur
    cur += 1
    extents = {'DATA.CVM': (0, data_secs * SEC)}
    placed = []  # (hostpath, lba, size, real)

    def place(key, host, real=None):
        nonlocal cur
        size = os.path.getsize(host)
        extents[key] = (cur, size)
        placed.append((host, cur, size, real))
        cur += pad_sectors(size)

    for name, host in root_files:
        place(name, os.path.join(game, host), real_mb * 1048576 if (name.endswith('.CVM') and real_mb) else None)
    for f in lib:
        place('LIB31/' + f.upper(), os.path.join(game, 'LIB31', f))
    total = cur

    # ---- directories
    def dir_sector(self_lba, parent_lba, entries):
        recs = [dirrec(b'\0', self_lba, SEC, True, t), dirrec(b'\1', parent_lba, SEC, True, t)]
        for nm, lba, size, isdir in sorted(entries):
            recs.append(dirrec(nm if isdir else nm + ';1', lba, size, isdir, t))
        buf = b''.join(recs)
        assert len(buf) <= SEC
        return buf.ljust(SEC, b'\0')

    root_entries = [('LIB31', lib_lba, SEC, True)]
    for k, (lba, size) in extents.items():
        if '/' not in k:
            root_entries.append((k, lba, size, False))
    lib_entries = [(k.split('/')[1], lba, size, False) for k, (lba, size) in extents.items() if k.startswith('LIB31/')]
    root_sec = dir_sector(root_lba, root_lba, root_entries)
    lib_sec = dir_sector(lib_lba, root_lba, lib_entries)

    def path_table(little):
        e = lambda v: struct.pack('<I' if little else '>I', v)
        h = lambda v: struct.pack('<H' if little else '>H', v)
        pt = bytes([1, 0]) + e(root_lba) + h(1) + b'\0' + b'\0'
        pt += bytes([5, 0]) + e(lib_lba) + h(1) + b'LIB31' + b'\0'
        return pt

    ptl, ptm = path_table(True), path_table(False)
    pvd = bytearray(SEC)
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[8:40] = b'PLAYSTATION'.ljust(32)
    pvd[40:72] = b'PERSONA3FES'.ljust(32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SEC)
    pvd[132:140] = both32(len(ptl))
    pvd[140:144] = struct.pack('<I', pt_l)
    pvd[148:152] = struct.pack('>I', pt_m)
    pvd[156:190] = dirrec(b'\0', root_lba, SEC, True, t)
    for off, ln in ((190, 128), (318, 128), (446, 128), (574, 128)):
        pvd[off:off + ln] = b' ' * ln
    for off in (813, 830, 847, 864):
        pvd[off:off + 17] = dtime17(t)
    pvd[881] = 1
    term = bytearray(SEC)
    term[0] = 255
    term[1:6] = b'CD001'
    term[6] = 1

    with open(out, 'r+b') as f:
        for lba, buf in ((16, bytes(pvd)), (17, bytes(term)), (pt_l, ptl.ljust(SEC, b'\0')),
                         (pt_m, ptm.ljust(SEC, b'\0')), (root_lba, root_sec), (lib_lba, lib_sec)):
            f.seek(lba * SEC)
            f.write(buf)
        for host, lba, size, real in placed:
            f.seek(lba * SEC)
            nbytes = size if real is None else min(real, size)
            with open(host, 'rb') as src:
                left = nbytes
                while left:
                    chunk = src.read(min(left, 1 << 22))
                    f.write(chunk)
                    left -= len(chunk)
        f.truncate(total * SEC)
    print('wrote', out, total, 'sectors', total * SEC, 'bytes; DATA.CVM clone at LBA 0')
    for k, (lba, size) in sorted(extents.items(), key=lambda kv: kv[1][0])[:8]:
        print('  %-14s lba %-8d size %d' % (k, lba, size))


if __name__ == '__main__':
    main()
