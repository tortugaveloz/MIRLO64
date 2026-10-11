#!/usr/bin/env python3
"""A random RSP program as a task dump (the format rsp_task and tb_rsp run):
straight-line code that fills DMEM and the vector registers with random data
and then runs random instructions of every kind rtl/rsp implements -- vector
operations with any element, loads and stores at any address and element
(LTV and STV among them), the COP2 moves, scalar memory accesses at any
alignment -- up to a BREAK. Real microcode exercises a small part of what the
instructions can do; this is for the rest.

    gen_rand_task.py SEED OUT.bin [N instructions, default 900]
"""
import random
import struct
import sys

RDRAM = 8 << 20
VEC_FNS = [0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0C, 0x0D, 0x0E, 0x0F,
           0x10, 0x11, 0x13, 0x14, 0x15, 0x1D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
           0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x3F]
LS_OPS = [0, 1, 2, 3, 4, 5, 6, 7, 11]
ST_ONLY = [8, 9]                                # SHV, SFV
SPECIAL = [0x7FFF, 0x8000, 0xFFFF, 0x0000, 0x0001, 0x8001, 0x4000, 0xC000]


def main():
    seed, out = int(sys.argv[1]), sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 900
    rnd = random.Random(seed)
    code = []
    emit = code.append
    ori = lambda rt, imm: emit(0x0D << 26 | rt << 16 | (imm & 0xFFFF))
    lui = lambda rt, imm: emit(0x0F << 26 | rt << 16 | (imm & 0xFFFF))
    mtc0 = lambda rt, rd: emit(0x10 << 26 | 4 << 21 | rt << 16 | rd << 11)

    # DMEM (all but the task header at 0xFC0) from RDRAM 0x10_0000, by DMA
    DATA = 0x100000
    ori(1, 0); mtc0(1, 0)
    lui(1, DATA >> 16); mtc0(1, 1)
    ori(1, 0xFBF); mtc0(1, 2)
    emit(0x10 << 26 | 1 << 16 | 6 << 11)               # mfc0 r1, SP_DMA_BUSY: wait for it, as microcode does
    emit(0x05 << 26 | 1 << 21 | 0xFFFE)                # bne r1, r0, the mfc0
    emit(0)
    # the vector registers: LQV from DMEM
    for v in range(32):
        ori(1, 16 * v)
        emit(0x32 << 26 | 1 << 21 | v << 16 | 4 << 11)
    # bases r1..r7 (any byte address), r8..r15 scratch
    def new_bases():
        for r in range(1, 8):
            ori(r, rnd.randrange(0x1000))
    new_bases()
    while len(code) < n:
        k = rnd.random()
        e = rnd.randrange(16)
        if k < 0.45:                                    # a vector operation
            fn = rnd.choice(VEC_FNS)
            emit(0x4A000000 | e << 21 | rnd.randrange(32) << 16 | rnd.randrange(32) << 11 | rnd.randrange(32) << 6 | fn)
        elif k < 0.75:                                  # a vector load or store
            st = rnd.random() < 0.5
            op = rnd.choice(LS_OPS + ST_ONLY if st else LS_OPS)
            el = e & 12 if op == 9 else e               # (SFV: an element a multiple of 4)
            emit((0x3A if st else 0x32) << 26 | rnd.randrange(1, 8) << 21 | rnd.randrange(32) << 16
                 | op << 11 | el << 7 | rnd.randrange(128))
        elif k < 0.83:                                  # MFC2 MTC2 CFC2 CTC2
            which = rnd.choice([0, 4, 2, 6])
            rd = rnd.randrange(32) if which in (0, 4) else rnd.randrange(4)
            emit(0x12 << 26 | which << 21 | rnd.randrange(8, 16) << 16 | rd << 11 | (e << 7 if which in (0, 4) else 0))
        elif k < 0.93:                                  # a scalar load or store, any alignment
            op = rnd.choice([0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B])
            emit(op << 26 | rnd.randrange(1, 8) << 21 | rnd.randrange(8, 16) << 16 | rnd.randrange(0x10000))
        elif k < 0.98:                                  # scalar arithmetic on the scratch registers
            fn = rnd.choice([0x20, 0x22, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B])
            emit(rnd.randrange(8, 16) << 21 | rnd.randrange(8, 16) << 16 | rnd.randrange(8, 16) << 11 | fn)
        else:
            new_bases()
    emit(0x0000000D)                                    # BREAK
    assert len(code) <= 1024

    ram = bytearray(RDRAM)
    data = bytearray(rnd.getrandbits(8) for _ in range(4096))
    for i in range(0, 4096, 2):                         # the values the clamps and compares turn on, often
        if rnd.random() < 0.25:
            struct.pack_into(">H", data, i, rnd.choice(SPECIAL))
    ram[DATA:DATA + 4096] = data
    BOOT, TASK = 0x200000, 0x300000
    ram[BOOT:BOOT + 4 * len(code)] = struct.pack(f">{len(code)}I", *code)
    task = [0] * 16
    task[0] = 1
    task[2], task[3] = 0x80000000 | BOOT, 4 * len(code)
    ram[TASK:TASK + 64] = struct.pack(">16I", *task)
    open(out, "wb").write(b"RSPT" + struct.pack(">I", TASK) + bytes(8) + ram)


if __name__ == "__main__":
    main()
