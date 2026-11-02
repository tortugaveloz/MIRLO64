#!/usr/bin/env python3
"""How many register jumps (jr, jalr) a small predictor would get right, on the jumps a game ran.

    CONMODEL=30 JTRACE=jtrace.txt tb_soc n64rt.bin ...      (sim/mips_soc/con_model.h writes a line per j, jal, jr, jalr)
    sim/mips/jump_pred.py jtrace.txt [...]

A line is "class kind pc target": class 0 the game's code, 1 the runtime's; kind 0 j, 1 jal, 2 jr $ra, 3 jr, 4 jalr. Every jr and jalr of
the core costs two cycles more than the console's (the fetch guesses it not taken); a prediction that is right saves them, one that is wrong
costs what it does now. Predictors, on the retired order (a real one sees a few wrong-path calls and returns more):
  table N      a table of N targets by the jump's address (no tag), written with the target each time
  ras D + table N   a stack of D return addresses (a jal or jalr pushes pc + 8, a jr $ra pops) for jr $ra, and the table for the others
"""
import sys
from collections import Counter

def sim(rows, ras_depth, tbl_n, tag=False):
    tbl = {}
    ras = []
    hit = Counter(); tot = Counter()
    for c, k, pc, tg in rows:
        if k in (0,):
            continue
        if k == 1:
            if ras_depth: ras.append(pc + 8); ras = ras[-ras_depth:]
            continue
        pred = None
        key = (pc >> 2) % tbl_n if tbl_n else None
        if k == 2 and ras_depth:
            pred = ras.pop() if ras else None
        elif tbl_n:
            e = tbl.get(key)
            pred = e[1] if e and (not tag or e[0] == pc) else None
        if k == 4 and ras_depth:
            ras.append(pc + 8); ras = ras[-ras_depth:]        # (jalr links)
        if tbl_n: tbl[key] = (pc, tg)
        name = {2: "jr ra", 3: "jr", 4: "jalr"}[k]
        tot[name] += 1
        if pred == tg: hit[name] += 1
    return hit, tot

def main():
    rows = []
    for f in sys.argv[1:]:
        for l in open(f):
            p = l.split()
            if len(p) == 4: rows.append((int(p[0]), int(p[1]), int(p[2], 16), int(p[3], 16)))
    n = Counter(k for _, k, _, _ in rows)
    print(f"{len(rows)} jumps: j {n[0]}, jal {n[1]}, jr ra {n[2]}, jr {n[3]}, jalr {n[4]}")
    jr = n[2] + n[3] + n[4]
    for ras, tbl, tag in [(0, 4, 0), (0, 8, 0), (0, 16, 0), (0, 32, 0), (0, 64, 0), (0, 16, 1), (0, 64, 1), (2, 0, 0), (4, 0, 0), (8, 0, 0), (2, 16, 0), (4, 16, 0), (4, 32, 0), (8, 32, 0), (16, 64, 0)]:
        hit, tot = sim(rows, ras, tbl, tag)
        h = sum(hit.values())
        print(f"ras {ras:2d} table {tbl:2d}{' tagged' if tag else '       '}: right {h}/{jr} = {100.0*h/jr:5.1f} %  "
              + "  ".join(f"{k} {hit[k]}/{tot[k]}" for k in ("jr ra", "jr", "jalr")))

main()
