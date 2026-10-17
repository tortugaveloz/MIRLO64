#!/usr/bin/env python3
"""A task whose RSP program hands the RDP its commands through a ring buffer
in DMEM (XBUS), the way a microcode that writes its output there does it:
for tb_dpc.

It sets XBUS and DPC_START = DPC_END at the ring's start, then over and over
brings a few random 64-bit commands from RDRAM into DMEM by DMA, behind what
it wrote last, and writes DPC_END after them. When the next batch does not
fit before the ring's end it goes back to the start: it waits for DPC_STATUS'
START valid to clear and for DPC_CURRENT to be off the ring's start, writes
DPC_START there, and from then on (before each batch) waits while DPC_CURRENT
is inside what the batch is to overwrite. With the RDP still taking the list
before the wrap, a START and the ENDs after it make a pair that waits, and
the ENDs come at every distance from the cycle the list before it ends.
Then BREAK.

    gen_xbus_ring.py SEED OUT.bin
"""
import random, struct, sys

RDRAM = 8 << 20
seed, out = int(sys.argv[1]), sys.argv[2]
rnd = random.Random(seed)
code = []
emit = code.append
R = lambda op, rs, rt, imm: emit(op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF))
ori = lambda rt, imm: R(0x0D, 0, rt, imm)
mtc0 = lambda rt, rd: emit(0x10 << 26 | 4 << 21 | rt << 16 | rd << 11)
mfc0 = lambda rt, rd: emit(0x10 << 26 | rt << 16 | rd << 11)
def branch(op, rs, rt, target):                     # (offset from the delay slot; a nop in it)
    R(op, rs, rt, target - (len(code) + 1)); emit(0)
def bltz(rs, target): R(0x01, rs, 0, target - (len(code) + 1)); emit(0)

BASE, LIMIT = 0x400, 0x800                          # the ring: 1 KiB of DMEM
DATA = 0x100000
ram = bytearray(RDRAM)
pos, w = DATA, BASE
ori(1, 2); mtc0(1, 11)                              # DPC_STATUS: set XBUS
ori(1, BASE); mtc0(1, 8); mtc0(1, 9)                # DPC_START = DPC_END = the ring's start
wrapped = False
while len(code) < 900:
    n = rnd.randrange(1, 7)                         # 64-bit commands
    s = 8 * n
    for i in range(2 * n):                          # (one-word commands: 0x30..0x3F)
        x = rnd.getrandbits(32)
        struct.pack_into(">I", ram, pos + 4 * i, x if i & 1 else (x & 0xC0FFFFFF) | (0x30 + rnd.randrange(16)) << 24)
    if w + s > LIMIT:                               # back to the ring's start
        a = len(code); mfc0(1, 11); R(0x0C, 1, 1, 0x400); branch(0x05, 1, 0, a)   # while START valid
        a = len(code); mfc0(1, 10); ori(2, BASE); branch(0x04, 1, 2, a)           # while CURRENT == the start
        mtc0(2, 8)                                  # DPC_START
        w, wrapped = BASE, True
    if wrapped:                                     # while CURRENT is in (w, w + s]
        a = len(code); mfc0(1, 10)
        R(0x08, 1, 2, -(w + 1)); e = len(code); R(0x01, 2, 0, 0); emit(0)       # bltz: CURRENT <= w, done
        R(0x08, 1, 2, -(w + s + 1)); bltz(2, a)
        code[e] = 0x01 << 26 | 2 << 21 | ((len(code) - (e + 1)) & 0xFFFF)
    ori(1, w); mtc0(1, 0)                           # SP_MEM_ADDR
    R(0x0F, 0, 1, pos >> 16); R(0x0D, 1, 1, pos & 0xFFFF); mtc0(1, 1)   # SP_DRAM_ADDR
    ori(1, s - 1); mtc0(1, 2)                       # SP_RD_LEN
    a = len(code); mfc0(1, 6); branch(0x05, 1, 0, a)  # wait for the DMA
    for _ in range(rnd.choice((0, 0, 1, 2, 3, rnd.randrange(0, 40)))): emit(0)
    ori(1, w + s); mtc0(1, 9)                       # DPC_END
    w += s; pos += s
a = len(code); mfc0(1, 6); branch(0x05, 1, 0, a)
emit(0x0000000D)                                    # BREAK
assert len(code) <= 1024
BOOT, TASK = 0x200000, 0x300000
ram[BOOT:BOOT + 4 * len(code)] = struct.pack(f">{len(code)}I", *code)
task = [0] * 16
task[0] = 1
task[2], task[3] = 0x80000000 | BOOT, 4 * len(code)
ram[TASK:TASK + 64] = struct.pack(">16I", *task)
open(out, "wb").write(b"RSPT" + struct.pack(">I", TASK) + bytes(8) + ram)
