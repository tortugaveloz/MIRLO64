#!/usr/bin/env python3
"""Where the RDP's time goes, by kind of command, from tb_rdp's TB_CLS logs.

    for t in dumps/*.bin; do TB_CLS=cls.txt TB_SDRAM=1 obj_rdp/tb_rdp $t; done
    rdp_classes.py cls.txt [more files...]

TB_CLS=<file> appends, for each task, one line a command (opcode, words, the cycle it started to execute, the cycle its last
word was taken, the other modes, its first two words, the pixels drawn up to it) and the cycle the task ended. A command's time
is from its start to the next one's. A class is the opcode and, for a primitive, the cycle type, what a triangle carries (S shade,
T texture, Z depth) and whether the depth test is on (zc).

Prints, per class, the commands, their cycles, the share of the total, the pixels drawn and the clocks a pixel."""
import collections
import sys


def cls(op, oh, ol):
    cyc = (oh >> 20) & 3
    cn = ["1c", "2c", "cp", "fl"][cyc]
    if op == 0x36:
        return "fillrect/" + cn
    if op in (0x24, 0x25):
        return "texrect/" + cn
    if 0x08 <= op <= 0x0F:
        zc = (ol >> 4) & 1
        zu = (ol >> 5) & 1
        return "tri/%s%s%s/%s%s" % ("S" if op & 4 else "-", "T" if op & 2 else "-", "Z" if op & 1 else "-", cn,
                                     "/zc" if zc else "/zu" if zu else "")
    if op in (0x30, 0x33, 0x34):
        return "load"
    if op == 0x29:
        return "sync"
    return "state"


def main():
    t = collections.defaultdict(lambda: [0, 0, 0])
    tot = 0
    for fn in sys.argv[1:]:
        rows = []
        for line in open(fn):
            p = line.split()
            if not p or p[0] == "T":
                continue
            if p[0] == "E":
                end = int(p[1])
                tot += end
                for i, r in enumerate(rows):
                    nxt = rows[i + 1][2] if i + 1 < len(rows) else end
                    c = cls(r[0], r[4], r[5])
                    px = r[6] - (rows[i - 1][6] if i else 0)
                    v = t[c]
                    v[0] += 1
                    v[1] += nxt - r[2]
                    v[2] += px
                rows = []
                continue
            rows.append((int(p[0], 16), int(p[1]), int(p[2]), int(p[3]), int(p[4], 16), int(p[5], 16), int(p[8])))
    print("total cycles %d" % tot)
    print("%-26s %8s %12s %6s %10s %8s" % ("class", "n", "cycles", "%", "pixels", "c/px"))
    for c, v in sorted(t.items(), key=lambda x: -x[1][1]):
        print("%-26s %8d %12d %5.1f%% %10d %8.2f" % (c, v[0], v[1], 100.0 * v[1] / tot, v[2], v[1] / v[2] if v[2] else 0))


if __name__ == "__main__":
    main()
