#!/usr/bin/env python3
"""What a game's RDP command streams use: from `rdp_task -n -v` output (stdin),
the state each primitive is drawn with -- cycle type, texture format and size,
TLUT, filter, the blender's and Z's modes, the combiner -- counted per
primitive. For deciding what RDP has to take, and what it can leave out.

    ./rdp_task -n -v task_gfx_*.bin | ./rdp_stats.py
"""
import sys
from collections import Counter

FMT = ["RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"]
CYC = ["1cycle", "2cycle", "copy", "fill"]
c = Counter()
oh = ol = 0
cc = (0, 0)
tiles = [None] * 8
timg = None
for line in sys.stdin:
    p = line.split()
    if not p or not p[0].endswith(":") or len(p[0]) != 3:
        continue
    op = int(p[0][:2], 16)
    w = [int(x, 16) for x in p[1:]]
    if op == 0x2F:
        oh, ol = w[0] & 0xFFFFFF, w[1]
    elif op == 0x3C:
        cc = (w[0] & 0xFFFFFF, w[1])
    elif op == 0x35:
        tiles[(w[1] >> 24) & 7] = ((w[0] >> 21) & 7, (w[0] >> 19) & 3)
    elif op == 0x3D:
        timg = ((w[0] >> 21) & 7, (w[0] >> 19) & 3)
    elif op in (0x30, 0x33, 0x34):
        c["load %s of a %s%d image" % ({0x30: "TLUT", 0x33: "BLOCK", 0x34: "TILE"}[op], FMT[timg[0]], 4 << timg[1])] += 1
    elif 0x08 <= op <= 0x0F or op in (0x24, 0x25, 0x36):
        cyc = (oh >> 20) & 3
        kind = "tri" if op < 0x10 else "texrect" if op != 0x36 else "fillrect"
        c["%s in %s" % (kind, CYC[cyc])] += 1
        if op < 0x10:
            c["tri %s%s%s" % ("shade " if op & 4 else "", "tex " if op & 2 else "", "z" if op & 1 else "")] += 1
        tex = (op & 2) if op < 0x10 else op != 0x36
        if tex:
            t = tiles[(w[0] >> 16) & 7 if op < 0x10 else (w[1] >> 24) & 7]
            if t:
                c["texture %s%d%s" % (FMT[t[0]], 4 << t[1], " through the TLUT" if oh & 0x8000 else "")] += 1
            if cyc < 2:
                c["filter %s" % ["point", "?1", "bilinear", "average"][(oh >> 12) & 3]] += 1
                c["persp %d" % ((oh >> 19) & 1)] += 1
                if (oh >> 16) & 1: c["texture LOD"] += 1
        if cyc < 2 and kind != "fillrect" or (kind == "fillrect" and cyc < 2):
            c["combine %06x %08x (%s)" % (cc[0], cc[1], CYC[cyc])] += 1
            fb = (ol >> 14) & 1
            c["blender %04x%s zmode %d%s%s%s%s%s" % (ol >> 16, " FORCE_BL" if fb else "", (ol >> 10) & 3,
              " Z_CMP" if ol & 0x10 else "", " Z_UPD" if ol & 0x20 else "", " AA" if ol & 8 else "",
              " IM_RD" if ol & 0x40 else "", " ALPHA_CMP" if ol & 1 else "")] += 1
            c["FORCE_BL %d" % fb] += 1
            if ol & 4: c["Z source: primitive"] += 1
        if cyc == 1:
            used1 = False
            a0 = (cc[0] >> 20) & 15; c0 = (cc[0] >> 15) & 31; a1 = (cc[0] >> 5) & 15; c1 = cc[0] & 31
            b0 = (cc[1] >> 28) & 15; b1 = (cc[1] >> 24) & 15; d0 = (cc[1] >> 15) & 7; d1 = (cc[1] >> 6) & 7
            if 2 in (a0, b0, a1, b1, c0, c1, d0, d1) or 9 in (c0, c1): used1 = True
            c["2cycle, TEXEL1 %s" % ("used" if used1 else "not used")] += 1
for k, v in sorted(c.items()):
    print("%8d  %s" % (v, k))
