#!/usr/bin/env python3
"""lang/mips/test/viaa_test.c's picture through vi_model.py's filters (AA + divot), against a frame the pads sent
(sim/mips_soc VGA=1: vNNNNNN.ppm).   viaa_check.py <pads frame .ppm> [--off: the filters off]"""
import sys
import numpy as np
from PIL import Image
sys.path.insert(0, __file__.rsplit('/', 1)[0])
import vi_model

W, H = 320, 240
x = np.arange(W)[None, :].repeat(H, 0)
y = np.arange(H)[:, None].repeat(W, 1)
r = np.where((x // 40) & 1, 31, (x * 31) // W)
g = np.where(((x + y) // 24) & 1, 28, 3)
b = (y * 31) // H
cvg = np.where(((x ^ y) % 7) == 0, (x + y) % 7, 7)
fb = (r << 11) | (g << 6) | (b << 1) | (cvg >> 2)
hid = cvg & 3
if '--off' in sys.argv:
    c = vi_model.channels(fb)
    want = vi_model.to_rgb(*[v << 3 for v in c])
else:
    want = vi_model.to_rgb(*vi_model.filt(fb, True, hid))
got = np.asarray(Image.open(sys.argv[1]).convert('RGB'))
if got.shape != want.shape:
    print('FAIL: pads frame %s, want %s' % (got.shape, want.shape)); sys.exit(1)
bad = (got != want).any(2)
part = (fb & 1) == 0
print('%s: %d of %d pixels differ from the model (%d partial pixels, %d changed by the filters)'
      % ('PASS' if not bad.any() else 'FAIL', bad.sum(), W * H, part.sum(),
         (vi_model.to_rgb(*[v << 3 for v in vi_model.channels(fb)]) != want).any(2).sum()))
if bad.any():
    ys, xs = np.nonzero(bad); print('first at', list(zip(xs[:5], ys[:5])))
sys.exit(1 if bad.any() else 0)
