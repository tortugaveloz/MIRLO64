#!/usr/bin/env python3
"""lang/mips/test/vi32_test.c's 32-bit picture against a frame the pads sent (sim/mips_soc VGA=1: vNNNNNN.ppm, 640 x 240:
each of the 320 pixels two clocks). The scan-out shows a 32-bit picture as its 16-bit view -- each channel's top five
bits, the alpha byte's top three bits as the coverage (rtl/soc/n64_video.sv) -- through the VI's anti-alias and divot
filters, by tb_vi.cpp's model of them. Pure Python.   vi32_check.py <pads frame .ppm> [out.png]"""
import sys, zlib, struct

W, H = 320, 240


def cvg(x, y):
    return (x + y) % 7 if ((x ^ y) % 7) == 0 else 7


def pixel(x, y):                      # vi32_test.c's word: R G B, then the coverage << 5 and noise
    r = (x * 255) // (W - 1)
    g = 0xE5 if ((x + y) // 24) & 1 else 0x1B
    b = (y * 255) // (H - 1)
    return r << 24 | g << 16 | b << 8 | cvg(x, y) << 5 | (x & 31)


# the 16-bit view, row-major, with the rows read on past their end (the scan-out's reads are contiguous); past the
# buffer, memory the test never wrote: 0
FB = [pixel(i % W, i // W) for i in range(W * H)]


def word(x, j):
    i = j * W + x
    return FB[i] if 0 <= i < len(FB) else 0


def ch5(x, j, k):
    return (word(x, j) >> (27 - 8 * k)) & 31


def cv(x, j):
    return (word(x, j) >> 5) & 7


def intap(x, j):
    return x >= 0 and 0 <= j < H


def complete(x, j):
    return cv(x, j) == 7


def aa_px(x, j):
    if complete(x, j):
        return [ch5(x, j, k) << 3 for k in range(3)]
    out = []
    taps = [(-1, -1), (1, -1), (-2, 0), (2, 0), (-1, 1), (1, 1)]
    for k in range(3):
        c = ch5(x, j, k)
        v = [ch5(x + dx, j + dy, k) if intap(x + dx, j + dy) and complete(x + dx, j + dy) else c for dx, dy in taps]
        s = sorted(v)
        mn, mx = min(s[1], c), max(s[4], c)
        w = 7 - cv(x, j)
        out.append((c << 3) + (((mn << 3) + (mx << 3) - 2 * (c << 3)) * w + 4) // 8)
    return out


def model(x, j):
    e = aa_px(x, j)
    if not complete(x, j) or (x > 0 and not complete(x - 1, j)) or not complete(x + 1, j):
        l = aa_px(x - 1, j) if x > 0 else e
        r = aa_px(x + 1, j)
        e = [sorted([l[k], e[k], r[k]])[1] for k in range(3)]
    return e


def read_ppm(p):
    d = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i + 1].isspace():
            i += 1
        j = i
        while not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j]); i = j
    return int(parts[1]), int(parts[2]), d[i + 1:]


def write_png(p, w, h, px):
    raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    open(p, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                        + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


w, h, px = read_ppm(sys.argv[1])
if len(sys.argv) > 2:
    write_png(sys.argv[2], w, h, px)
if (w, h) != (2 * W, H):
    print('FAIL: pads frame %d x %d, want %d x %d' % (w, h, 2 * W, H)); sys.exit(1)
bad, first, partial, lowbits = 0, [], 0, 0
for j in range(H):
    row = [model(x, j) for x in range(W)]
    for xo in range(2 * W):
        want = row[xo // 2]
        got = list(px[(j * w + xo) * 3:(j * w + xo) * 3 + 3])
        if got != want:
            bad += 1
            if len(first) < 5:
                first.append((xo, j, got, want))
for y in range(H):
    for x in range(W):
        partial += cvg(x, y) != 7
        lowbits += ((pixel(x, y) >> 8) & 0x070707) != 0
print('%s: %d of %d pad pixels differ from the model (%d partial pixels in the picture, %d with low bits the scan-out drops)'
      % ('PASS' if not bad else 'FAIL', bad, 2 * W * H, partial, lowbits))
if bad:
    print('first:', first)
sys.exit(1 if bad else 0)
