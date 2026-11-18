#!/usr/bin/env python3
"""The RTL's time against the console's, by kind of command, from tb_rdp's TB_CTIME_CSV logs.

    for t in dumps/*.bin; do TB_SDRAM=1 TB_CTIME_CSV=ct/$(basename $t).csv obj_rdp_ct/tb_rdp $t; done     (a build with -DRDP_CTIME)
    rdp_ctime.py ct/*.csv

Each CSV line is one command: its index, opcode, the RTL's cycles (from the cycle it started to execute to the next command's,
so that what the pace made a command wait for is the command before's), the console's cycles with the pipeline alone and with
the RDRAM (sim/rdp/rdp_time.c). Prints, per class, the commands, the RTL's cycles, the console's with the RDRAM and with the
pipeline alone, their ratio, and the commands the RTL took more than 5 % longer than the console on."""
import collections
import sys


def kind(op):
    if 8 <= op <= 15:
        return "tri %s%s%s" % ("S" if op & 4 else "-", "T" if op & 2 else "-", "Z" if op & 1 else "-")
    if op in (0x24, 0x25):
        return "texrect"
    if op == 0x36:
        return "fillrect"
    if op in (0x30, 0x33, 0x34):
        return "load"
    if 0x26 <= op <= 0x29:
        return "sync%x" % op
    return "other"


def main():
    agg = collections.defaultdict(lambda: [0, 0, 0, 0, 0])
    for fn in sys.argv[1:]:
        for line in open(fn):
            _, op, rtl, pipe, mem = line.strip().split(",")
            rtl, pipe, mem = int(rtl), int(pipe), int(mem)
            a = agg[kind(int(op, 16))]
            a[0] += 1; a[1] += rtl; a[2] += mem; a[3] += pipe; a[4] += rtl > mem * 1.05
    print("%-10s %8s %12s %12s %12s %6s  %s" % ("class", "commands", "RTL", "console", "console pipe", "ratio", "RTL > 1.05 x console"))
    tot = [0, 0, 0]
    for k, a in sorted(agg.items(), key=lambda x: -x[1][1]):
        print("%-10s %8d %12d %12d %12d %6.2f  %d" % (k, a[0], a[1], a[2], a[3], a[1] / max(a[2], 1), a[4]))
        tot[0] += a[1]; tot[1] += a[2]; tot[2] += a[3]
    print("%-10s %8s %12d %12d %12d %6.2f" % ("total", "", tot[0], tot[1], tot[2], tot[0] / max(tot[1], 1)))


if __name__ == "__main__":
    main()
