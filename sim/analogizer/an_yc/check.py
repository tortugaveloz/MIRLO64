#!/usr/bin/env python3
"""an_yc against a floating-point model of NTSC / PAL encoding: python3 check.py <ntsc|pal> <dump of tb.cpp> [CGAIN]

The model, from the standards (SMPTE 170M, ITU-R BT.470, BT.601), is built from the RGB the bench sent, not from the RTL:
  Y     = 16 + (65.481 R + 128.553 G + 24.966 B) / 255
  U, V  = g * 0.492 * 219/255 * (B - Yf),  g * 0.877 * 219/255 * (R - Yf),   Yf = 0.299 R + 0.587 G + 0.114 B  (R, G, B 0..255)
  c     = 128 + U sin(w t) + V cos(w t)      (PAL: +-V, the sign switching each line)
  burst = NTSC 9 cycles of -sin (180 degrees), amplitude g * 20 IRE = g * 43.8 codes;
          PAL 10 cycles, 135 / 225 degrees, amplitude g * 46.9 codes; none while vs is high
with g = CGAIN / 16. Each bar's chroma and each line's burst is measured by a least-squares fit of a sine and a cosine at the
exact subcarrier frequency (and a constant) over the samples, so nothing assumes the RTL's phase: the bar's phase against the
burst, its amplitude, the burst's position (15 / 22 clocks after the sync ends), length (whole cycles) and the PAL alternation
are the checks. Levels in codes (the unsigned 8-bit c, 128 = none)."""
import sys
import numpy as np

sysname, fn = sys.argv[1], sys.argv[2]
CGAIN = int(sys.argv[3]) if len(sys.argv) > 3 else 12
pal = sysname == "pal"
LAT = 5
CLK = 25e6
fsc = 4433618.75 if pal else 315e6 / 88
w = 2 * np.pi * fsc / CLK                                # rad / clock
NB = 10 if pal else 9
DELAY = 22 if pal else 15
P = 2 * (800 if pal else 790)
g = CGAIN / 16.0
BURST_A = g * (46.93 if pal else 43.8)                   # peak amplitude of the burst in codes

d = np.fromfile(fn, dtype=np.uint8).reshape(-1, 6)
flags = d[:, 0]
hs = flags & 1; vs = (flags >> 1) & 1; de = (flags >> 2) & 1; csn = (flags >> 3) & 1
y = d[:, 1].astype(np.float64); c = d[:, 2].astype(np.float64) - 128.0
R = d[:, 3].astype(np.float64); G = d[:, 4].astype(np.float64); B = d[:, 5].astype(np.float64)
N = len(d)
fail = 0
def bad(msg):
    global fail
    fail += 1
    if fail < 30:
        print("FAIL:", msg)

# ---- luma: the input LAT clocks earlier, within 1 LSB of the exact BT.601
Yf_in = 16 + (65.481 * R + 128.553 * G + 24.966 * B) / 255
Yf_in[de == 0] = 16
n0 = 8 * P
err = np.abs(y[n0:] - Yf_in[n0 - LAT:N - LAT])
print("luma: max error %.3f LSB over %d clocks" % (err.max(), len(err)))
if err.max() >= 1.0:
    bad("luma error %.3f" % err.max())

# ---- sync: csync_n low for hs, LAT clocks later, except in the vertical interval (checked by the an_csync bench)
hs_fall = np.flatnonzero((hs[1:] == 0) & (hs[:-1] == 1)) + 1
hs_rise = np.flatnonzero((hs[1:] == 1) & (hs[:-1] == 0)) + 1
vs_rise = np.flatnonzero((vs[1:] == 1) & (vs[:-1] == 0)) + 1
vs_fall = np.flatnonzero((vs[1:] == 0) & (vs[:-1] == 1)) + 1
m = np.ones(N, dtype=bool)
for a, b in zip(vs_rise, vs_fall):
    m[a - P: b + P] = False
m[:n0] = False; m[:LAT] = False
exp_cs = 1 - hs[np.arange(N) - LAT]
if not np.array_equal(csn[m], exp_cs[m]):
    bad("csync_n is not hs delayed by %d outside the vertical interval (%d clocks differ)" % (LAT, (csn[m] != exp_cs[m]).sum()))
else:
    print("sync: csync_n = not hs, %d clocks later, outside the vertical interval" % LAT)

# ---- chroma
tt = np.arange(N, dtype=np.float64)
def fit(i0, i1, t0):
    """c over [i0, i1) as a sin(w (n - t0)) + b cos(w (n - t0)) + dc -> a, b, rms residual"""
    n = np.arange(i0, i1)
    A = np.stack([np.sin(w * (n - t0)), np.cos(w * (n - t0)), np.ones(len(n))], 1)
    sol, res, _, _ = np.linalg.lstsq(A, c[i0:i1], rcond=None)
    r = c[i0:i1] - A @ sol
    return sol[0], sol[1], np.sqrt((r ** 2).mean())

eburst = []; ephase_b = []; epos = []; elen = []
eamp = []; eph = []
nburst = nnone = 0
last_sign = None; last_k = None
rms_b = 0
white_black = []
bar_err = {}
fl = hs_fall[hs_fall > 3 * P]
fl = fl[fl < N - 2 * P]
for k, f in enumerate(fl):
    out0 = f + LAT                                       # the output clock of the sync's trailing edge
    t0 = out0 + DELAY                                    # where the burst starts: the phase zero
    seg = c[out0: out0 + 160]
    nz = np.flatnonzero(np.abs(seg) > 3)
    if vs[f]:
        nnone += 1
        if len(nz): bad("burst in the vertical interval (line at clock %d)" % f)
        continue
    if len(nz) == 0:
        bad("no burst at clock %d" % f); continue
    nburst += 1
    first, last = out0 + nz[0], out0 + nz[-1]
    nsamp = int(np.ceil(NB * CLK / fsc))                 # samples of NB whole cycles
    if abs((last - t0) - (nsamp - 1)) > 1:
        bad("burst length: last sample at t0+%d, expected t0+%d" % (last - t0, nsamp - 1))
    elen.append(last - t0 + 1)
    # the window must be silent before and after
    if first < t0 - 1 or first > t0 + 2:       # (NTSC: the burst starts at a zero, so the first sample off 128 is t0 + 1)
        bad("burst starts at t0%+d" % (first - t0))
    epos.append(first - t0)
    a, b, rms = fit(t0, t0 + nsamp, t0)
    rms_b = max(rms_b, rms)
    Ab = np.hypot(a, b); psi = np.degrees(np.arctan2(b, a)) % 360
    if pal:
        sign = 1 if b > 0 else -1
        want = 135.0 if sign > 0 else 225.0
        if last_sign is not None:
            flip = (k - last_k) % 2 == 1
            if (sign != last_sign) != flip:
                bad("PAL line alternation broken at clock %d (lines since the last burst %d)" % (f, k - last_k))
        last_sign, last_k = sign, k
    else:
        sign, want = 1, 180.0
    eburst.append(Ab - BURST_A)
    ephase_b.append(((psi - want + 180) % 360) - 180)
    # the picture that follows: bars
    s_ = f + np.flatnonzero(de[f:f + P] == 1)
    if len(s_) == 0: continue
    s = s_[0]
    for j in range(8):
        ci = s + 160 * j + 80                            # input clock of the bar's middle
        r_, g_, b_ = R[ci], G[ci], B[ci]
        Yf = 0.299 * r_ + 0.587 * g_ + 0.114 * b_
        U = g * 0.492 * 219 / 255 * (b_ - Yf)
        V = g * 0.877 * 219 / 255 * (r_ - Yf)
        i0 = s + 160 * j + LAT + 50; i1 = s + 160 * j + LAT + 130
        a2, b2, rms2 = fit(i0, i1, t0)
        A2 = np.hypot(a2, b2)
        Aexp = np.hypot(U, V)
        eamp.append(A2 - Aexp)
        key = (int(r_), int(g_), int(b_))
        # expected components: a = U, b = sign * V
        va, vb = U, sign * V
        if Aexp > 15:
            ph = np.degrees(np.arctan2(b2, a2) - np.arctan2(vb, va))
            ph = (ph + 180) % 360 - 180
            # relative to the measured burst too
            rel = ((np.degrees(np.arctan2(b2, a2)) - psi) - (np.degrees(np.arctan2(vb, va)) - want) + 180) % 360 - 180
            eph.append(ph)
            bar_err.setdefault(key, []).append((A2 - Aexp, ph, rel, Aexp, np.degrees(np.arctan2(V, U)) % 360))
        else:
            white_black.append(A2)
print("bursts: %d lines with, %d lines without (vs high)" % (nburst, nnone))
print("burst amplitude error: max %.2f codes (nominal %.1f), phase error: max %.2f deg, start t0%+d..%+d, length %d..%d samples, fit rms %.2f" % (
    np.abs(eburst).max(), BURST_A, np.abs(ephase_b).max(), min(epos), max(epos), min(elen), max(elen), rms_b))
if np.abs(eburst).max() > 1.5: bad("burst amplitude")
if np.abs(ephase_b).max() > 6: bad("burst phase")
nvs = int(((vs_rise > fl[0]) & (vs_rise < fl[-1])).sum())
if nnone != 8 * nvs: bad("lines without burst %d, expected %d (vs is high for 8 lines in each of %d fields)" % (nnone, 8 * nvs, nvs))
print("colour bars (NTSC phase of U,V against the burst; amplitude in codes):")
names = {(255,255,255):"white", (255,255,0):"yellow", (0,255,255):"cyan", (0,255,0):"green", (255,0,255):"magenta",
         (255,0,0):"red", (0,0,255):"blue", (191,191,0):"yellow 75%", (0,191,191):"cyan 75%", (0,191,0):"green 75%",
         (191,0,191):"magenta 75%", (191,0,0):"red 75%", (0,0,191):"blue 75%"}
for key, v in sorted(bar_err.items()):
    v = np.array(v)
    print("  %-12s expected amplitude %6.2f at %6.1f deg (against +U); error: amplitude %+5.2f max %.2f, phase vs model %+5.2f max %.2f, vs burst max %.2f deg" % (
        names.get(key, str(key)), v[0, 3], v[0, 4], v[:, 0].mean(), np.abs(v[:, 0]).max(), v[:, 1].mean(), np.abs(v[:, 1]).max(), np.abs(v[:, 2]).max()))
ea = np.abs(eamp); ep = np.abs(eph)
print("all bars: amplitude error max %.2f codes, phase error max %.2f deg; white / black / grey bars residual chroma max %.2f" % (
    ea.max(), ep.max(), max(white_black)))
if ea.max() > 2.5: bad("bar amplitude")
if ep.max() > 3.0: bad("bar phase")
if max(white_black) > 1.5: bad("chroma on grey")

# ---- no chroma in the blanking outside the burst
mask = np.ones(N, dtype=bool)
for f in hs_fall:
    mask[f + LAT: f + LAT + DELAY + 80] = False
de_o = np.zeros(N, dtype=np.uint8); de_o[LAT:] = de[:N - LAT]
blank = (de_o == 0) & mask
blank[:n0] = False
# the chroma of the picture's last bar (black) is 0, so blanking is quiet right up to the burst
cb = np.abs(c[blank]).max()
print("blanking outside the burst: max |c - 128| = %.2f" % cb)
if cb > 0: bad("chroma in the blanking")
if fail:
    print("an_yc %s: FAIL" % sysname); sys.exit(1)
print("an_yc %s: PASS" % sysname)
