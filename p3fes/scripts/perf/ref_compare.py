#!/usr/bin/env python3
"""ref_compare.py OUT [seconds...]: build OUT/S/{native,gs,side,diff}.png and stats.txt from
OUT/native/frame-S.p3fb and OUT/gs/frame-S.p3fb (P3FB: "P3FB", u32 w, u32 h, RGBA8 rows top first)."""
import os, struct, sys, zlib


def load(path):
    d = open(path, 'rb').read()
    if d[:4] != b'P3FB':
        raise ValueError(path)
    w, h = struct.unpack_from('<II', d, 4)
    return w, h, d[12:12 + w * h * 4]


def png(path, w, h, rgb):
    """rgb: bytes of w*h*3."""
    raw = b''.join(b'\x00' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def rgb_of(px):
    out = bytearray(len(px) // 4 * 3)
    out[0::3], out[1::3], out[2::3] = px[0::4], px[1::4], px[2::4]
    return bytes(out)


def luma_grid(w, h, rgb, cols=8, rows=7):
    grid = []
    for ty in range(rows):
        line = []
        y0, y1 = ty * h // rows, (ty + 1) * h // rows
        for tx in range(cols):
            x0, x1 = tx * w // cols, (tx + 1) * w // cols
            total = 0
            n = 0
            for y in range(y0, y1, 4):          # every 4th row and pixel: enough for a tile mean
                row = rgb[(y * w + x0) * 3:(y * w + x1) * 3]
                r, g, b = row[0::12], row[1::12], row[2::12]
                total += sum(r) * 299 + sum(g) * 587 + sum(b) * 114
                n += len(r)
            line.append(total / 1000.0 / max(n, 1))
        grid.append(line)
    return grid


def main():
    out = sys.argv[1]
    secs = [s.split('.')[0] for s in sys.argv[2:]]
    for s in secs:
        n, g = (os.path.join(out, side, 'frame-%s.p3fb' % s) for side in ('native', 'gs'))
        if not (os.path.exists(n) and os.path.exists(g)):
            print('%s: missing %s' % (s, 'native' if not os.path.exists(n) else 'gs'))
            continue
        nw, nh, npx = load(n)
        gw, gh, gpx = load(g)
        d = os.path.join(out, s)
        os.makedirs(d, exist_ok=True)
        nrgb, grgb = rgb_of(npx), rgb_of(gpx)
        png(os.path.join(d, 'native.png'), nw, nh, nrgb)
        png(os.path.join(d, 'gs.png'), gw, gh, grgb)
        lines = ['native %dx%d  gs %dx%d' % (nw, nh, gw, gh)]
        if (nw, nh) == (gw, gh):
            diff = bytes(min(255, abs(a - b) * 3) for a, b in zip(nrgb, grgb))
            png(os.path.join(d, 'diff.png'), nw, nh, diff)
            side = b''.join(nrgb[y * nw * 3:(y + 1) * nw * 3] + grgb[y * nw * 3:(y + 1) * nw * 3] for y in range(nh))
            png(os.path.join(d, 'side.png'), nw * 2, nh, side)
            mad = sum(abs(a - b) for a, b in zip(nrgb[::7], grgb[::7])) / len(nrgb[::7])
            lines.append('mean abs channel difference (sampled): %.2f / 255' % mad)
            ln, lg = luma_grid(nw, nh, nrgb), luma_grid(gw, gh, grgb)
            lines.append('mean luma per 8x7 tile, native - gs (negative = native darker); native mean %.1f gs mean %.1f' % (
                sum(map(sum, ln)) / 56, sum(map(sum, lg)) / 56))
            for rn, rg in zip(ln, lg):
                lines.append(' '.join('%+6.1f' % (a - b) for a, b in zip(rn, rg)))
        open(os.path.join(d, 'stats.txt'), 'w').write('\n'.join(lines) + '\n')
        print('%s: %s' % (d, lines[1] if len(lines) > 1 else lines[0]))


main()
