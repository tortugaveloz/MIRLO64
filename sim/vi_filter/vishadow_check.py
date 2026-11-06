#!/usr/bin/env python3
"""vishadow_gen's picture against a frame the pads sent (sim/mips_soc VGA=1: vNNNNNN.ppm, 640 x 240).   vishadow_check.py <expected.ppm> <pads frame .ppm>
Prints the number of pixels that differ, and the distinct levels along the red ramp's middle row."""
import sys
import numpy as np
from PIL import Image
want = np.asarray(Image.open(sys.argv[1]).convert('RGB'))
got = np.asarray(Image.open(sys.argv[2]).convert('RGB'))
if got.shape != want.shape:
    print('FAIL: pads frame %s, want %s' % (got.shape, want.shape)); sys.exit(1)
bad = (got != want).any(2)
lv = len(set(got[24, 80:560:2, 0].tolist()))
print('%s: %d of %d pad pixels differ from the model (%d distinct red levels along the red ramp)' % ('PASS' if not bad.any() else 'FAIL', bad.sum(), bad.size, lv))
if bad.any():
    ys, xs = np.nonzero(bad); print('first at', list(zip(xs[:5].tolist(), ys[:5].tolist())), 'pads', got[ys[0], xs[0]], 'model', want[ys[0], xs[0]])
sys.exit(1 if bad.any() else 0)
