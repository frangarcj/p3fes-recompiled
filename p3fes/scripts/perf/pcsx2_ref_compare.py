#!/usr/bin/env python3
"""pcsx2_ref_compare.py OUTDIR PCSX2.png OURS.(png|p3fb) [regions]

Both images are brought to 640x448 (ours is 1280x896 on the Mac window: 2x2 box-filtered).  Writes
OUTDIR/side-by-side.png (PCSX2 | ours | |diff|x4, full frame) and, per region, OUTDIR/crop-<name>.png
(same three panels, enlarged 4x nearest), and prints the mean RGB / luma of the region in each image.
Region syntax: name=x,y,w,h (640x448 coordinates); defaults are the lounge-scene regions of interest.
Pure Python (no image library)."""
import struct, sys, zlib

DEFAULT_REGIONS = {
    'wall-tablelamp': (0, 0, 150, 110),
    'tablelamp': (90, 40, 40, 60),
    'moon': (585, 55, 50, 50),
    'floorlamp': (480, 25, 55, 70),
    'shadow': (300, 225, 140, 60),
    'character': (350, 130, 110, 140),
    'hud-banner': (0, 395, 330, 53),
    'hud-date': (440, 0, 200, 100),
    'whole': (0, 0, 640, 448),
}


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    pos, idat, ihdr = 8, b'', None
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        body = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            ihdr = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT':
            idat += body
        pos += 12 + n
    w, h, bd, ct, _, _, il = ihdr
    assert bd == 8 and il == 0 and ct in (2, 6), ihdr
    bpp = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        row = out[y * w * 3:(y + 1) * w * 3]
        row[0::3], row[1::3], row[2::3] = line[0::bpp], line[1::bpp], line[2::bpp]
        out[y * w * 3:(y + 1) * w * 3] = row
    return w, h, out


def read_p3fb(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'P3FB'
    w, h = struct.unpack_from('<II', d, 4)
    px = d[12:]
    out = bytearray(w * h * 3)
    out[0::3], out[1::3], out[2::3] = px[0::4], px[1::4], px[2::4]
    return w, h, out


def to_640(img):
    w, h, px = img
    if (w, h) == (640, 448):
        return px
    assert (w, h) == (1280, 896), (w, h)
    out = bytearray(640 * 448 * 3)
    for y in range(448):
        r0 = px[(2 * y) * w * 3:(2 * y + 1) * w * 3]
        r1 = px[(2 * y + 1) * w * 3:(2 * y + 2) * w * 3]
        for c in range(3):
            a, b = r0[c::3], r1[c::3]
            out[y * 640 * 3 + c:(y + 1) * 640 * 3:3] = bytes(
                (a[2 * x] + a[2 * x + 1] + b[2 * x] + b[2 * x + 1] + 2) >> 2 for x in range(640))
    return out


def rows(px, x, y, w, h):
    return [bytes(px[((y + j) * 640 + x) * 3:((y + j) * 640 + x + w) * 3]) for j in range(h)]


def write_png(path, rws, w):
    raw = b''.join(b'\x00' + r for r in rws)

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, len(rws), 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def panel(a, b, x, y, w, h, s, path):
    ra, rb = rows(a, x, y, w, h), rows(b, x, y, w, h)
    gap = b'\x40\x40\xff' * 3
    out = []
    for r1, r2 in zip(ra, rb):
        d = bytes(min(255, abs(p - q) * 4) for p, q in zip(r1, r2))
        line = r1 + gap + r2 + gap + d
        big = b''.join(line[i:i + 3] * s for i in range(0, len(line), 3))
        out.extend([big] * s)
    write_png(path, out, (w * 3 + 6) * s)


def stats(px, x, y, w, h):
    n = w * h
    sr = sg = sb = 0
    for r in rows(px, x, y, w, h):
        sr += sum(r[0::3]); sg += sum(r[1::3]); sb += sum(r[2::3])
    mr, mg, mb = sr / n, sg / n, sb / n
    return mr, mg, mb, 0.299 * mr + 0.587 * mg + 0.114 * mb


def main():
    out, pp, op = sys.argv[1:4]
    regions = dict(DEFAULT_REGIONS)
    for a in sys.argv[4:]:
        n, v = a.split('=')
        regions[n] = tuple(int(t) for t in v.split(','))
    import os
    os.makedirs(out, exist_ok=True)
    a = to_640(read_png(pp))
    b = to_640(read_p3fb(op) if op.endswith('.p3fb') else read_png(op))
    panel(a, b, 0, 0, 640, 448, 1, out + '/side-by-side.png')
    print('%-15s %-26s %-26s' % ('region', 'PCSX2 mean R,G,B (luma)', 'ours mean R,G,B (luma)'))
    for n, (x, y, w, h) in regions.items():
        s1, s2 = stats(a, x, y, w, h), stats(b, x, y, w, h)
        print('%-15s %5.1f %5.1f %5.1f (%5.1f)    %5.1f %5.1f %5.1f (%5.1f)' % ((n,) + s1 + s2))
        if n != 'whole':
            panel(a, b, x, y, w, h, 4, '%s/crop-%s.png' % (out, n))


main()
