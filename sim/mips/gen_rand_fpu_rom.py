#!/usr/bin/env python3
"""A random FPU program as an N64 ROM, for the CPU's lockstep bench (as
gen_rand_rom.py's, which has the integer instructions).

Every COP1 operation fpu_n64 implements, single and double, on registers
full of the values that matter -- zeros, the smallest and largest normals,
what is below them, infinities, NaNs, integers at the conversions' limits,
halves (the rounding modes) and plain random bits -- with the compares and
their branches, the moves to and from the integer registers, FCR31's
rounding mode, and loads and stores of both sizes through the cache and
around it.

    gen_rand_fpu_rom.py SEED OUT.z64 [N instructions, default 120000] [w64]

"w64" adds the 64-bit side of COP1 (dmtc1 and dmfc1 through a 64-bit integer loaded from memory, cvt.s.l, cvt.d.l and the
conversions to a 64-bit integer, on integers of every size class); a build whose model leaves 64-bit instructions to the
runtime (TB_NO64) cannot run them.
"""
import random
import struct
import sys

ENTRY = 0x80000400
SCRATCH = 0x800F0000            # 512 bytes, random at the start
C1 = 0x11 << 26
SPECIAL = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001,
           0x00800000, 0x007FFFFF, 0x00000001, 0x7F7FFFFF, 0xFF7FFFFF, 0x4F000000, 0xCF000000, 0x4EFFFFFF,
           0xCF000001, 0x3F000000, 0x3FC00000, 0x40200000, 0xBF000000, 0x3EFFFFFF, 0x3F000001, 0x4B000000,
           0x4B800000, 0x33800000, 0x00FFFFFF, 0x01000000, 0x7F000000, 0x3F7FFFFF, 0x3F800001, 0x40490FDB]
SPECIAL_D = [0x0000000000000000, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
             0xFFF0000000000000, 0x7FF8000000000000, 0x7FF0000000000001, 0x3810000000000000, 0x380FFFFFFFFFFFFF,
             0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x47F0000000000000, 0x41DFFFFFFFC00000, 0xC1E0000000000000,
             0x41E0000000000000, 0x3FE0000000000000, 0x3FF8000000000000, 0x4004000000000000, 0x3FEFFFFFFFFFFFFF,
             0x36A0000000000000, 0x369FFFFFFFFFFFFF, 0x3FF0000010000000, 0x3FF0000030000000, 0x3FF0000008000000,
             0x400921FB54442D18, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF]


def main():
    seed, out = int(sys.argv[1]), sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 120000
    w64 = "w64" in sys.argv[4:]
    rnd = random.Random(seed)
    code = []
    emit = code.append
    I = lambda op, rs, rt, imm: emit(op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF))
    T0, T1, UNC, CACHED = 8, 9, 28, 29

    def li32(r, v):
        I(0x0F, 0, r, v >> 16)
        I(0x0D, r, r, v & 0xFFFF)

    def fval():
        k = rnd.random()
        if k < 0.45: return rnd.choice(SPECIAL)
        if k < 0.75:                            # a normal of a middling exponent: sums and products that stay in range
            return rnd.getrandbits(1) << 31 | rnd.randrange(100, 160) << 23 | rnd.getrandbits(23)
        return rnd.getrandbits(32)

    def dval():
        k = rnd.random()
        if k < 0.45: return rnd.choice(SPECIAL_D)
        if k < 0.75: return rnd.getrandbits(1) << 63 | rnd.randrange(1000, 1050) << 52 | rnd.getrandbits(52)
        return rnd.getrandbits(64)

    freg = lambda: rnd.randrange(32)
    dreg = lambda: rnd.randrange(16) * 2
    mtc1 = lambda rt, fs: emit(C1 | 4 << 21 | rt << 16 | fs << 11)
    mfc1 = lambda rt, fs: emit(C1 | 0 << 21 | rt << 16 | fs << 11)
    op = lambda fmt, ft, fs, fd, fn: emit(C1 | fmt << 21 | ft << 16 | fs << 11 | fd << 6 | fn)

    def set_s(f=None):
        li32(T0, fval()); mtc1(T0, freg() if f is None else f)

    def set_d(f=None):
        f = dreg() if f is None else f
        v = dval()
        li32(T0, v & 0xFFFFFFFF); mtc1(T0, f)           # the pair: the even register the low half
        li32(T0, v >> 32); mtc1(T0, f + 1)

    li32(UNC, SCRATCH | 0x20000000)
    li32(CACHED, SCRATCH)
    for f in range(32): set_s(f)

    def arith():
        d = rnd.random() < 0.4
        fmt, r = (0x11, dreg) if d else (0x10, freg)
        fn = rnd.choice([0, 0, 1, 1, 2, 2, 3, 4, 5, 6, 7])
        op(fmt, r(), r(), r(), fn)

    def convert():
        k = rnd.randrange(5)
        if k == 0: op(0x11, 0, dreg(), freg(), 0x20)                             # cvt.s.d
        elif k == 1: op(0x10, 0, freg(), dreg(), 0x21)                           # cvt.d.s
        elif k == 2:                                                            # to an integer
            d = rnd.random() < 0.4
            op(0x11 if d else 0x10, 0, dreg() if d else freg(), freg(), rnd.choice([0x0C, 0x0D, 0x0E, 0x0F, 0x24]))
        elif k == 3: op(0x14, 0, freg(), freg(), 0x20)                           # cvt.s.w
        else: op(0x14, 0, freg(), dreg(), 0x21)                                  # cvt.d.w

    def compare():
        d = rnd.random() < 0.4
        r = dreg if d else freg
        op(0x11 if d else 0x10, r(), r(), 0, 0x30 + rnd.randrange(16))
        if rnd.random() < 0.7:                  # bc1f / bc1t and their likely forms, over a few instructions
            skip = rnd.randrange(3)
            emit(C1 | 8 << 21 | rnd.randrange(4) << 16 | (skip + 1))
            arith() if rnd.random() < 0.6 else emit(0)
            for _ in range(skip): arith()

    def move():
        k = rnd.randrange(6)
        if k == 0: set_s()
        elif k == 1: set_d()
        elif k == 2: mfc1(T1, freg())
        elif k == 3:                            # FCR31: the rounding mode, FS (and the condition bit as it comes)
            li32(T0, rnd.randrange(4) | rnd.getrandbits(1) << 24 | rnd.getrandbits(1) << 23)
            emit(C1 | 6 << 21 | T0 << 16 | 31 << 11)
        elif k == 4: emit(C1 | 2 << 21 | T1 << 16 | rnd.choice([0, 31]) << 11)   # cfc1
        else: mtc1(rnd.choice([0, T1]), freg())

    def mem():
        base = UNC if rnd.random() < 0.3 else CACHED
        k = rnd.randrange(4)
        if k == 0: I(0x31, base, freg(), rnd.randrange(512) & ~3)               # lwc1
        elif k == 1: I(0x39, base, freg(), rnd.randrange(512) & ~3)             # swc1
        elif k == 2: I(0x35, base, dreg(), rnd.randrange(512) & ~7)             # ldc1
        else: I(0x3D, base, dreg(), rnd.randrange(512) & ~7)                    # sdc1

    T2 = 10

    def lval():                                 # a 64-bit integer of every size class
        k = rnd.randrange(8)
        if k == 0: return rnd.choice([0, 1, (1 << 64) - 1, 1 << 63, (1 << 63) - 1, 1 << 31, (1 << 32) - 1, 1 << 32, (1 << 24) + 1, (1 << 53) + 1])
        if k == 1: return rnd.getrandbits(32)                                   # an unsigned word
        if k == 2: return (rnd.getrandbits(32) | -(rnd.getrandbits(1) << 32)) & ((1 << 64) - 1)   # a word, sign-extended
        if k == 3: return (1 << rnd.randrange(64)) + rnd.randrange(-2, 3) & ((1 << 64) - 1)
        if k == 4: return (rnd.getrandbits(64) >> rnd.randrange(64)) & ((1 << 64) - 1)             # few significant bits
        if k == 5: return (rnd.getrandbits(64) << rnd.randrange(64)) & ((1 << 64) - 1)             # trailing zeros
        if k == 6: return (-(rnd.getrandbits(64) >> rnd.randrange(64))) & ((1 << 64) - 1)
        return rnd.getrandbits(64)

    def wide():
        k = rnd.randrange(5)
        if k <= 1:                                                              # dmtc1 of a 64-bit integer from memory
            v = lval(); off = rnd.randrange(0, 512, 8)
            li32(T0, v >> 32); I(0x2B, CACHED, T0, off)
            li32(T0, v & 0xFFFFFFFF); I(0x2B, CACHED, T0, off + 4)
            I(0x37, CACHED, T2, off)
            emit(C1 | 5 << 21 | T2 << 16 | dreg() << 11)
        elif k == 2: emit(C1 | 1 << 21 | T2 << 16 | dreg() << 11)               # dmfc1
        elif k == 3:                                                            # cvt.s.l, cvt.d.l
            if rnd.random() < 0.6: op(0x15, 0, dreg(), freg(), 0x20)
            else: op(0x15, 0, dreg(), dreg(), 0x21)
        else:                                                                   # to a 64-bit integer: of a value that a word holds
            d = rnd.random() < 0.4                                              # (the unit makes a larger one a word's saturation, the model does not)
            fs = dreg() if d else freg()
            e = rnd.randrange(100, 158)
            if d:
                v = rnd.getrandbits(1) << 63 | (e + 896) << 52 | rnd.getrandbits(52)
                li32(T0, v & 0xFFFFFFFF); mtc1(T0, fs); li32(T0, v >> 32); mtc1(T0, fs + 1)
            else:
                li32(T0, rnd.getrandbits(1) << 31 | e << 23 | rnd.getrandbits(23)); mtc1(T0, fs)
            op(0x11 if d else 0x10, 0, fs, dreg(), rnd.choice([0x08, 0x09, 0x0A, 0x0B, 0x25]))

    while len(code) < n:
        if w64 and rnd.random() < 0.15:
            wide()
            continue
        k = rnd.random()
        if k < 0.45: arith()
        elif k < 0.62: convert()
        elif k < 0.76: compare()
        elif k < 0.90: move()
        else: mem()
    emit(0x1000FFFF)                            # b .
    emit(0)
    assert 4 * len(code) < SCRATCH - ENTRY

    rom = bytearray(0x101000 + 0x100000)
    struct.pack_into(">I", rom, 0, 0x80371240)
    struct.pack_into(">I", rom, 8, ENTRY)
    rom[0x20:0x34] = b"MIRLO FPU TEST      "
    rom[0x1000:0x1000 + 4 * len(code)] = struct.pack(f">{len(code)}I", *code)
    rnd = random.Random(seed ^ 0x5EED)
    o = 0x1000 + SCRATCH - ENTRY
    for i in range(0, 512, 8):
        struct.pack_into(">Q", rom, o + i, rnd.choice(SPECIAL_D) if rnd.random() < 0.5 else rnd.getrandbits(64))
    open(out, "wb").write(rom)
    print(f"{out}: {len(code)} instructions")


if __name__ == "__main__":
    main()
