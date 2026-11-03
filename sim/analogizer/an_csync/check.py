#!/usr/bin/env python3
"""an_csync against its documented rule, from the dump of tb.cpp: <ntsc|pal> <vs_lines> <active_low 0/1> <file>.

The rule is re-stated here from the timing alone: the sync is active (csync_n low, or high for active-high) when
hs XOR vint, vint being high from the hs fall of the first line in which vs is high, for vs_lines lines (0: as long as
vs is high). Measured on the output: the pulse widths (hs wide, the serrations hs wide, the broad pulses a line less
an hs), the count of broad pulses, the latency of 2 clocks."""
import sys
import numpy as np

sysname, vsl, alow, fn = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
LAT = 2
d = np.fromfile(fn, dtype=np.uint8)
hs = (d & 1).astype(np.int8); vs = ((d >> 1) & 1).astype(np.int8); cs_n = ((d >> 2) & 1).astype(np.int8)
act = (1 - cs_n) if alow else cs_n                       # sync active
npx = 790 if sysname == "ntsc" else 800
P = npx * 2
HSW = 96
fail = 0
def bad(msg):
    global fail
    fail += 1
    print("FAIL:", msg)

# edges of hs in the input
hs_rise = np.flatnonzero((hs[1:] == 1) & (hs[:-1] == 0)) + 1
hs_fall = np.flatnonzero((hs[1:] == 0) & (hs[:-1] == 1)) + 1
vs_rise = np.flatnonzero((vs[1:] == 1) & (vs[:-1] == 0)) + 1
vs_fall = np.flatnonzero((vs[1:] == 0) & (vs[:-1] == 1)) + 1

# the model: vint from the hs falls
vint = np.zeros(len(hs), dtype=np.int8)
state, nl = 0, 0
for f in hs_fall:
    v = vs[f]
    if not v:
        nl = 0; state = 0
    elif vsl == 0 or nl != vsl:
        nl = min(nl + 1, 7); state = 1
    else:
        state = 0
    vint[f:] = state
exp = (hs ^ vint).astype(np.int8)
exp = np.concatenate([np.zeros(LAT, dtype=np.int8), exp[:-LAT]])
n0 = P * 3                                               # (the first lines are the start-up)
if not np.array_equal(exp[n0:], act[n0:]):
    k = np.flatnonzero(exp[n0:] != act[n0:])[0] + n0
    bad("csync differs from hs ^ vint at clock %d (%d samples differ)" % (k, (exp[n0:] != act[n0:]).sum()))

# pulse measures on the output, in the second field
a = act.astype(np.int8)
r = np.flatnonzero((a[1:] == 1) & (a[:-1] == 0)) + 1
f = np.flatnonzero((a[1:] == 0) & (a[:-1] == 1)) + 1
r = r[r > n0]; f = f[f > r[0]]
m = min(len(r), len(f)); r = r[:m]; f = f[:m]
w = f - r
gaps = r[1:] - f[:-1]
nfield = 264 if sysname == "ntsc" else 313
nvl = vsl if vsl else (vs_fall[1] - vs_rise[1]) // P     # the lines of the interval (vs_lines or vs)
normal = np.sum(w == HSW)
broad = w[w > HSW]
print("%s vs_lines=%d active_low=%d: %d pulses/%d clocks; hs-wide %d, broad %s, gaps %s" % (
    sysname, vsl, alow, m, len(a), normal, sorted(set(broad.tolist())), sorted(set(gaps.tolist()))[:4]))
# the first field after the start: count the pulses between two vs rises
v0, v1 = vs_rise[0], vs_rise[1]
inwin = (r >= v0) & (r < v1)
wl = w[inwin]
nb = int(np.sum(wl > HSW))
if nb != nvl:
    bad("broad pulses in a field: %d, expected %d" % (nb, nvl))
if np.any(wl[wl > HSW][1:] != P - HSW) or wl[wl > HSW][0] != P:
    bad("broad pulse widths %s, expected %d then %d" % (wl[wl > HSW].tolist(), P, P - HSW))
if set(wl[wl <= HSW].tolist()) != {HSW}:
    bad("narrow pulse widths %s" % sorted(set(wl[wl <= HSW].tolist())))
# serrations: the inactive gaps inside the interval are hs wide
gi = gaps[(r[1:] >= v0) & (r[1:] < v1)]
sg = sorted(set(gi[gi < P // 2].tolist()))
if sg != [HSW]:
    bad("serration widths %s, expected [%d]" % (sg, HSW))
if fail:
    print("FAIL"); sys.exit(1)
print("an_csync: PASS (%s, vs_lines %d, active %s)" % (sysname, vsl, "low" if alow else "high"))
