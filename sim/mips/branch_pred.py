#!/usr/bin/env python3
"""How many conditional branches a table of outcomes guesses right, on the branches a game ran.

    CONMODEL=30 JBTRACE=btrace.bin JBFROM=30 tb_soc n64rt.bin ...      (sim/mips_soc/con_model.h writes 5 bytes a branch:
                                                                        the address, then 1 taken, 2 backward, 4 likely, 8 the runtime's)
    sim/mips/branch_pred.py btrace.bin [...]

The core guesses a backward branch taken and a forward one by a table of 64 bits, by the low bits of the branch's address, written with the
outcome of every conditional branch (docs/mips.md, "Branches"); a wrong guess costs two cycles. The lines give the wrong guesses of that and of
other tables, on the same branches in the order they retired (a real fetch also sees a few wrong-path branches).
"""
import struct
import sys

def load(files):
    rows = []
    for f in files:
        d = open(f, "rb").read()
        for i in range(0, len(d) - 4, 5):
            pc = struct.unpack_from("<I", d, i)[0]; fl = d[i + 4]
            rows.append((pc, fl & 1, (fl >> 1) & 1, (fl >> 3) & 1))
    return rows

def run(rows, n, bits, fwd_only, tagged=False, init=0, tb=None):
    """n entries; bits 1 or 2 (a counter: taken when >= half); the table is read for forward branches only (fwd_only: backward ones are always
    guessed taken) or for all; written by every branch"""
    mask = n - 1
    hi = (1 << bits) - 1; half = 1 << (bits - 1)
    t = [init] * n
    tag = [None] * n
    wrong = [0, 0]
    for pc, tk, back, rt in rows:
        i = (pc >> 2) & mask
        e = t[i]
        if tagged and tag[i] != pc:
            e = None
        if back and fwd_only: g = 1
        elif e is None: g = 1 if back else 0
        else: g = 1 if e >= (tb if back and tb is not None else half) else 0
        if g != tk: wrong[rt] += 1
        if tagged and tag[i] != pc: t[i] = (half if tk else half - 1); tag[i] = pc
        elif bits == 1: t[i] = tk
        else: t[i] = min(hi, t[i] + 1) if tk else max(0, t[i] - 1)
    return wrong

def main():
    rows = load(sys.argv[1:])
    g = [r for r in rows if not r[3]]
    print(f"{len(rows)} conditional branches ({len(g)} the game's, {len(rows) - len(g)} the runtime's); taken {sum(r[1] for r in rows)}, backward {sum(r[2] for r in rows)}")
    base = run(g, 64, 1, True)
    print(f"{'table':44s} {'wrong (game)':>13s} {'of branches':>12s} {'saved vs now':>13s}")
    def line(name, w):
        print(f"{name:44s} {w:13d} {100.0 * w / len(g):11.2f} % {100.0 * (base[0] - w) / len(g):11.2f} % of branches")
    line("now: 64 x 1 bit, forward only", base[0])
    for n, b, fo, tg, ini in [(64, 1, 0, 0, 0), (128, 1, 1, 0, 0), (256, 1, 1, 0, 0), (512, 1, 1, 0, 0), (64, 2, 1, 0, 1), (64, 2, 0, 0, 1), (128, 2, 0, 0, 1), (256, 2, 0, 0, 1), (512, 2, 0, 0, 1),
                              (256, 2, 0, 1, 1), (64, 2, 0, 1, 1), (1024, 2, 0, 0, 1)]:
        w = run(g, n, b, fo, tg, ini)
        line(f"{n} x {b} bit{'s' if b > 1 else ' '}, {'forward only' if fo else 'all branches'}{', tagged' if tg else ''}", w[0])
    for n, ini, tb in [(64, 0, 1), (64, 1, 1), (64, 2, 1), (128, 1, 1), (256, 1, 1)]:
        line(f"{n} x 2 bits, backward taken from 1, start {ini}", run(g, n, 2, 0, False, ini, tb)[0])

main()
