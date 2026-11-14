import sys, struct, zlib
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
assert d[:4] == b'P3FB'
w, h = struct.unpack_from('<II', d, 4)
px = d[12:12 + w * h * 4]
swap = len(sys.argv) > 3 and sys.argv[3] == 'bgr'
rows = []
for y in range(h):
    row = bytearray(px[y * w * 4:(y + 1) * w * 4])
    row[3::4] = b'\xff' * w
    if swap:
        row[0::4], row[2::4] = row[2::4], row[0::4]
    rows.append(b'\x00' + bytes(row))
def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(dst, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b''))
print(w, h)
