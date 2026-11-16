#!/usr/bin/env python3
"""The scan-out's VI filters (rtl/soc/n64_video.sv, MIRLO_VI_AA) on a raw
colour image: RGBA5551 big-endian, bit 0 the coverage's top bit, and its
pixels' hidden two coverage bits (image.hid, a byte a pixel: rdp_task
RDP_RAW=1). The same rules as tb_vi.cpp's model, which the RTL matches pixel
for pixel.

  vi_model.py image.raw width out.png [--divot] [--dedither] [--off]

--off: the picture as the scan-out shows it without the filters.
"""
import sys
import numpy as np
from PIL import Image


def load(path, w):
    a = np.frombuffer(open(path, 'rb').read(), dtype='>u2').astype(np.int32)
    return a[: (a.size // w) * w].reshape(-1, w)


def channels(fb):
    return [(fb >> 11) & 31, (fb >> 6) & 31, (fb >> 1) & 31]


def filt(fb, divot, hid=None, dedither=False):
    """R8 G8 B8 per pixel, as the pads take them"""
    h, w = fb.shape
    if hid is None:
        hid = np.full(fb.shape, 3)
    cvg = ((fb & 1) << 2) | hid
    comp = cvg == 7
    ch = channels(fb)
    out = []
    taps = [(-1, -1), (1, -1), (-2, 0), (2, 0), (-1, 1), (1, 1)]
    cp_ok = np.pad(comp, 2, constant_values=False)          # off the picture: not a tap
    for c in ch:
        o = c << 3                                     # unfiltered: the N64's 5 bits << 3
        if dedither:                                  # the ring's signs, added to the eight bits
            add = np.zeros_like(c)
            okp = np.pad(np.ones_like(comp), 1, constant_values=False)
            cp1 = np.pad(c, 1)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nb = cp1[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
                    ok = okp[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
                    add += np.where(ok, np.sign(nb - c), 0)
            o = np.minimum((c << 3) + add, 255)
        cp = np.pad(c, 2)
        vals = []
        for dx, dy in taps:
            nb = cp[2 + dy:2 + dy + h, 2 + dx:2 + dx + w]
            ok = cp_ok[2 + dy:2 + dy + h, 2 + dx:2 + dx + w]
            vals.append(np.where(ok, nb, c))          # not complete, or off: the centre
        s = np.sort(np.stack(vals), axis=0)
        mn = np.minimum(s[1], c)
        mx = np.maximum(s[4], c)
        o8 = (c << 3) + (mn + mx - 2 * c) * (7 - cvg)       # the N64's ((.. + 4) >> 3 on 8 bits): exact
        out.append(np.where(comp, o, o8))
    if divot:
        part = ~comp
        anyp = part.copy()
        anyp[:, 1:] |= part[:, :-1]
        anyp[:, :-1] |= part[:, 1:]
        res = []
        for o in out:
            l = o.copy(); l[:, 1:] = o[:, :-1]
            r = o.copy(); r[:, :-1] = o[:, 1:]
            med = np.median(np.stack([l, o, r]), axis=0).astype(np.int32)
            res.append(np.where(anyp, med, o))
        out = res
    return out


def to_rgb(r8, g8, b8):
    return np.stack([r8, g8, b8], axis=-1).astype(np.uint8)


def main():
    path, w, outp = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    fb = load(path, w)
    hp = path[:-4] + '.hid'
    import os
    hid = np.frombuffer(open(hp, 'rb').read(), dtype=np.uint8).astype(np.int32)[: fb.size].reshape(fb.shape) if os.path.exists(hp) else None
    if '--off' in sys.argv:
        c = channels(fb)
        r8, g8, b8 = [x << 3 for x in c]
    else:
        r8, g8, b8 = filt(fb, '--divot' in sys.argv, hid, '--dedither' in sys.argv)
    Image.fromarray(to_rgb(r8, g8, b8)).save(outp)


if __name__ == '__main__':
    main()
