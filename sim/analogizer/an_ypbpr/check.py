#!/usr/bin/env python3
"""an_ypbpr against the BT.601 matrix in floating point: python3 check.py <dump of tb.cpp>.
Studio levels from the full-range RGB:  Y = 16 + (65.481 R + 128.553 G + 24.966 B) / 255,
Cb = 128 + (-37.797 R - 74.203 G + 112 B) / 255, Cr = 128 + (112 R - 93.786 G - 18.214 B) / 255.
Pass: every output within 1 LSB of the exact value (not rounded), blanking (de low) Y 16, Pb = Pr = 128."""
import sys
import numpy as np

d = np.fromfile(sys.argv[1], dtype=np.uint8).reshape(-1, 3)
N = 1 << 24
assert len(d) == N
k = np.arange(N, dtype=np.uint64)
de = (((k * np.uint64(2654435761)) & np.uint64(0xFFFFFFFF)) >> np.uint64(16)) & np.uint64(7) != 0
R = ((k >> np.uint64(16)) & np.uint64(255)).astype(np.float64)
G = ((k >> np.uint64(8)) & np.uint64(255)).astype(np.float64)
B = (k & np.uint64(255)).astype(np.float64)
Y = 16 + (65.481 * R + 128.553 * G + 24.966 * B) / 255
Cb = 128 + (-37.797 * R - 74.203 * G + 112.0 * B) / 255
Cr = 128 + (112.0 * R - 93.786 * G - 18.214 * B) / 255
Y[~de] = 16; Cb[~de] = 128; Cr[~de] = 128
pr, y, pb = d[:, 0].astype(np.float64), d[:, 1].astype(np.float64), d[:, 2].astype(np.float64)
ok = True
for name, got, ref in (("Y", y, Y), ("Pb", pb, Cb), ("Pr", pr, Cr)):
    e = np.abs(got - ref).max()
    lo, hi = got.min(), got.max()
    print("%-2s max error %.3f LSB, range %d..%d" % (name, e, lo, hi))
    if e >= 1.0:
        ok = False; print("FAIL:", name)
if (y[~de] != 16).any() or (pb[~de] != 128).any() or (pr[~de] != 128).any():
    ok = False; print("FAIL: blanking")
print("an_ypbpr: %s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
