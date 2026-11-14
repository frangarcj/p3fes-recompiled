#!/usr/bin/env python3
"""ref_crop.py OUT.png NATIVE.p3fb GS.p3fb X Y W H [SCALE [GSDX GSDY]]  (window pixels, 1280x896 on the Mac window)
GSDX/GSDY shift the GS region (it is read at X+GSDX, Y+GSDY) to line up a camera that ended up elsewhere.
Writes native | gs | |native-gs|x4 side by side for the region, enlarged SCALE times (default 3, nearest)."""
import struct, sys, zlib


def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'P3FB'
    w, h = struct.unpack_from('<II', d, 4)
    return w, h, d[12:]


def crop(img, x, y, cw, ch):
    w, h, px = img
    rows = []
    for yy in range(y, min(y + ch, h)):
        r = px[(yy * w + x) * 4:(yy * w + x + cw) * 4]
        rgb = bytearray(len(r) // 4 * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = r[0::4], r[1::4], r[2::4]
        rows.append(bytes(rgb))
    return rows


def main():
    out, npath, gpath = sys.argv[1:4]
    x, y, cw, ch = (int(v) for v in sys.argv[4:8])
    s = int(sys.argv[8]) if len(sys.argv) > 8 else 3
    dx, dy = (int(sys.argv[9]), int(sys.argv[10])) if len(sys.argv) > 10 else (0, 0)
    a, b = crop(load(npath), x, y, cw, ch), crop(load(gpath), max(0, x + dx), max(0, y + dy), cw, ch)
    d = [bytes(min(255, abs(p - q) * 4) for p, q in zip(r1, r2)) for r1, r2 in zip(a, b)]
    gap = b'\x40\x40\xff' * 4
    rows = []
    for r1, r2, r3 in zip(a, b, d):
        line = r1 + gap + r2 + gap + r3
        big = b''.join(line[i:i + 3] * s for i in range(0, len(line), 3))
        rows.extend([b'\x00' + big] * s)
    W = (cw * 3 + 8) * s
    H = len(rows)
    def chunk(t, bb):
        return struct.pack('>I', len(bb)) + t + bb + struct.pack('>I', zlib.crc32(t + bb) & 0xffffffff)
    open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                          chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))
    print(out, W, H)


main()
