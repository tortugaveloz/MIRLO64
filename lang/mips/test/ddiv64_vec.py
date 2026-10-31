#!/usr/bin/env python3
"""Writes ddiv64_vec.h: ddiv, ddivu (on 64-bit operands) and div, divu (on the lower words) of the operand pairs below, as
exact integers -- the VR4300's results, among them a divisor of zero and the limit, -2^63 / -1.
    ddiv64_vec.py > ddiv64_vec.h"""
import random

M64, M32 = (1 << 64) - 1, (1 << 32) - 1


def s64(v): return v - (1 << 64) if v >> 63 else v
def s32(v):
    v &= M32
    return v - (1 << 32) if v >> 31 else v


def ddiv(a, b):
    sa, sb = s64(a), s64(b)
    if b == 0: return (1 if sa < 0 else M64), a             # (the VR4300: the quotient of the magnitudes, all ones, signed)
    if a == 1 << 63 and b == M64: return 1 << 63, 0
    q = abs(sa) // abs(sb)
    r = abs(sa) % abs(sb)
    return (-q if (sa < 0) != (sb < 0) else q) & M64, (-r if sa < 0 else r) & M64


def ddivu(a, b):
    if b == 0: return M64, a
    return a // b, a % b


def div32(a, b):
    sa, sb = s32(a), s32(b)
    if b & M32 == 0: return (1 if sa < 0 else M32), a & M32
    if sa == -(1 << 31) and sb == -1: return 1 << 31, 0
    q = abs(sa) // abs(sb)
    r = abs(sa) % abs(sb)
    return (-q if (sa < 0) != (sb < 0) else q) & M32, (-r if sa < 0 else r) & M32


def divu32(a, b):
    a, b = a & M32, b & M32
    if b == 0: return M32, a
    return a // b, a % b


def main():
    rnd = random.Random(64)
    edge = [0, 1, M64, 1 << 63, (1 << 63) - 1, M32, 1 << 31, (1 << 31) - 1, M64 - M32 + (1 << 31), M64 - M32, 1 << 32, 2, M64 - 1,
            (1 << 63) + 1, (1 << 32) + 1, 3, 10, 0xFFFFFFFF80000001]
    pairs = [(a, b) for a in edge for b in edge]
    for _ in range(40):                                     # a quotient of every length, either sign
        la, lb = rnd.randrange(1, 65), rnd.randrange(1, 65)
        a, b = rnd.getrandbits(la), rnd.getrandbits(lb) | 1
        if rnd.random() < .4: a = -a & M64
        if rnd.random() < .4: b = -b & M64
        pairs.append((a, b))
    for _ in range(20):                                     # a multiple of the divisor, +-1
        b = rnd.getrandbits(rnd.randrange(1, 40)) | 1
        a = b * rnd.getrandbits(rnd.randrange(1, 24)) + rnd.choice([-1, 0, 1])
        if rnd.random() < .5: a, b = -a, b if rnd.random() < .5 else -b
        pairs.append((a & M64, b & M64))
    print("/* ddiv64_test's table (ddiv64_vec.py): the operands (a, b), then ddiv's and ddivu's LO and HI (64 bits each, upper word first) and\n"
          " * div's and divu's, of a and b's lower words (LO, HI) */")
    print("static const struct { uint32_t ah, al, bh, bl, sq[4], uq[4], d[2], du[2]; } vec[] __attribute__((aligned(8))) = {")
    for a, b in pairs:
        sq, sr = ddiv(a, b)
        uq, ur = ddivu(a, b)
        d, dr = div32(a, b)
        du, dur = divu32(a, b)
        w = lambda v: "0x%08x,0x%08x" % (v >> 32, v & M32)
        print("  {0x%08x,0x%08x,0x%08x,0x%08x,{%s,%s},{%s,%s},{0x%08x,0x%08x},{0x%08x,0x%08x}}," %
              (a >> 32, a & M32, b >> 32, b & M32, w(sq), w(sr), w(uq), w(ur), d, dr, du, dur))
    print("};")


if __name__ == "__main__":
    main()
