#!/usr/bin/env python3
"""A random integer program as an N64 ROM, for the CPU's lockstep bench
(the model's boot puts ROM 0x1000.. at the entry point and starts there).

Real games use few of the 64-bit instructions and almost never with operands
that are not a 32-bit value's sign extension; this is for the rest: every
integer instruction the core implements, on registers full of 64-bit values
(the limits among them), loads and stores of every size through the cache
and around it (KSEG1) on the same memory, branches and their likely forms on
64-bit operands, traps (a handler steps over them), multiplies and divides
back to back with the moves from HI and LO.

    gen_rand_rom.py SEED OUT.z64 [N instructions, default 120000] [full|nodiv|div|i32|i32d|nomd|alu|base]

An argument "tlb" adds the TLB: windows at 0x0800_0000, 0x0A00_0000, 0x0C00_0000 and 0x0E00_0000 whose pages are the scratch
memory (pairs of 4, 16 and 64 KiB pages, an ASID or global, valid / dirty / uncached as the program makes them), loads and stores
through them -- ll, sc, ld, sd, ldl, sdl among them -- and random tlbwi, tlbwr, tlbp, tlbr, ASID changes and reads of the TLB's
registers; a TLB exception's handler (at both vectors) steps over the instruction.
A last argument "k1" has the program run from KSEG1 (its own code fetched
uncached, a word at a time; the trap handler stays in KSEG0). An argument "ua"
adds ldl, ldr, sdl and sdr (the core's W64_UA) at every byte address, through
the cache and around it, and as often as ld and sd.
"""
import os
import random
import struct
import sys

ENTRY = 0x80000400
SCRATCH = 0x800F0000            # 512 bytes, random at the start (inside the MiB the boot copies)
TABLE = 0x800F0400              # 32 doublewords: the registers' first values
RESULT = 0x800F0800             # ("e2e") the registers at the end
SPECIAL = [0, 1, 0xFFFFFFFFFFFFFFFF, 0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0x00000000FFFFFFFF,
           0x0000000080000000, 0x000000007FFFFFFF, 0xFFFFFFFF80000000, 0xFFFFFFFF00000000,
           0x0000000100000000, 0x8000000080000000, 2, 0xFFFFFFFFFFFFFFFE]
GP = list(range(1, 26))         # the registers the program computes in
K0, K1, UNC, CACHED, TAB = 26, 27, 28, 29, 30
MAPPED = 31                     # ("tlb") the address of a window
WINDOWS = [0x08000000, 0x0A000000, 0x0C000000, 0x0E000000]
WSIZE = [(0x1E000, 0x10000), (0, 0x1000), (0x6000, 0x4000), (0x1E000, 0x10000)]   # a window's pages: their PageMask and size. Every entry of a
# window has them: entries of two sizes that overlap are undefined on the VR4300 (the TLB has no defined first match)
FRAME = 0xF0000                 # the scratch memory's physical page (64 KiB aligned): the windows' even pages


def main():
    seed, out = int(sys.argv[1]), sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 120000
    # what the core under test has (mips_core's levels): "full" W64_ALU + SH +
    # MD; "nodiv" without ddiv / ddivu (W64_DIV off); "div" the same as "full" but mostly divides, on the values where a divider
    # goes wrong (zero, one, the limits, powers of two and their neighbours, equal operands, a quotient of one bit or of 64);
    # "i32" and "i32d": "base" (every register but 20 to 25 holds a 32-bit value) with the multiplies and divides too, dmult(u)
    # ("i32") or ddiv ("i32d") among them -- to compare the cycles of a core with those of another, an instruction a core
    # does not do being the point of the other modes;
    # "nomd" without dmult / ddiv; "alu" without the d-shifts too; "base"
    # W64 alone -- 64-bit values come from ld and lwu only, into registers 20
    # to 25, and nothing the core leaves to the runtime reads those
    mode = sys.argv[4] if len(sys.argv) > 4 else "full"
    assert mode in ("full", "nodiv", "div", "i32", "i32d", "nomd", "alu", "base")
    nomd, base = mode not in ("full", "nodiv", "div", "i32", "i32d"), mode in ("base", "i32", "i32d")
    WIDE = list(range(20, 26))
    # "e2e": the program as a game under the runtime (n64rt) on the SoC -- no
    # traps and no handler of its own, and at the end every register, HI and
    # LO stored at RESULT with a mark after them, to compare with the model's
    e2e = len(sys.argv) > 5 and sys.argv[5] == "e2e"
    k1 = sys.argv[-1] == "k1"
    ua = "ua" in sys.argv[5:] and not base
    tlb = "tlb" in sys.argv[5:] and not e2e
    rnd = random.Random(seed)
    code = []
    emit = code.append
    R = lambda rs, rt, rd, sa, fn: emit(rs << 21 | rt << 16 | rd << 11 | sa << 6 | fn)
    I = lambda op, rs, rt, imm: emit(op << 26 | rs << 21 | rt << 16 | (imm & 0xFFFF))
    reg = lambda: rnd.choice(GP)
    # (k0 and k1 are the exception handler's: under the runtime they change at any time)
    src = lambda: rnd.choice(GP + [0]) if rnd.random() < 0.9 else rnd.choice([CACHED] if e2e else [K1, CACHED])
    nreg = lambda: rnd.choice(GP[:19]) if base else reg()          # a register that is never wide ("base")
    nsrc = lambda: rnd.choice(GP[:19] + [0]) if base else src()
    wreg = lambda: rnd.choice(WIDE) if base else reg()

    def li32(r, v):
        I(0x0F, 0, r, v >> 16)
        I(0x0D, r, r, v & 0xFFFF)

    if k1:                                      # on from here in KSEG1
        li32(K0, (ENTRY + 16) | 0x20000000)
        R(K0, 0, 0, 0, 0x08)                    # jr k0
        emit(0)
    # the trap handler at 0x80000180: step over the instruction
    handler = [0x401A7000,                      # mfc0 k0, EPC
               0x275A0004,                      # addiu k0, k0, 4
               0x409A7000,                      # mtc0 k0, EPC
               0x00000000, 0x00000000,
               0x42000018,                      # eret
               0x00000000]
    for vec in ((0x80000000, 0x80000180) if tlb else (0x80000180,)):
        li32(K1, vec)
        for i, w in enumerate(handler if not e2e else []):
            li32(K0, w)
            I(0x2B, K1, K0, 4 * i)              # sw
    LO = lambda pa, c, d, v, g: ((pa >> 12) << 6) | (c << 3) | (d << 2) | (v << 1) | g
    mtc0 = lambda rt, rd: emit(0x40800000 | rt << 16 | rd << 11)
    mfc0 = lambda rt, rd: emit(0x40000000 | rt << 16 | rd << 11)

    def tlbent(op, idx, pm, hi, lo0, lo1):
        # the entry's registers, then tlbwi (op 2) at idx or tlbwr (6): through K0, which only the handler uses
        for v, r in ((pm, 5), (hi, 10), (lo0, 2), (lo1, 3), (idx, 0)):
            li32(K0, v)
            mtc0(K0, r)
        emit(0x42000000 | op)

    def tlbrand():
        # an entry for a window (or not), pages of 4, 16 or 64 KiB onto the scratch frame and the one after it
        wi = rnd.randrange(4)
        w = WINDOWS[wi] + (1 << 24 if rnd.random() < 0.1 else 0)           # (and now and then another VA: tlbp finds nothing there)
        pm, size = WSIZE[wi]
        asid = rnd.choice([0, 1, 0])
        g = rnd.random() < 0.4
        def lo(pa):
            if rnd.random() < 0.1: return 0
            return LO(pa, rnd.choice([2, 3, 3]), int(rnd.random() < 0.8), int(rnd.random() < 0.9), int(g))
        idx = rnd.randrange(4) if rnd.random() < 0.15 else rnd.randrange(4, 32)
        tlbent(2 if "wr" in TLBOFF else rnd.choice([2, 2, 2, 6]), idx, pm, w | asid, lo(FRAME), lo(FRAME + size))

    TLBOFF = os.environ.get("TLBGEN_OFF", "")           # (for finding a difference: "ops", "asid", "wr", "p", "r" leave that out)
    def tlbops():
        k = rnd.random()
        if "ops" in TLBOFF: return
        if k < 0.35: tlbrand()
        elif k < 0.55:                          # tlbp: Index (P for none)
            li32(K0, rnd.choice(WINDOWS) | rnd.choice([0, 0, 1]) | (rnd.randrange(2) << 13))
            mtc0(K0, 10)
            emit(0x42000008)
            mfc0(reg(), 0)
        elif k < 0.70:                          # tlbr: the registers it loads
            li32(K0, rnd.randrange(32))
            mtc0(K0, 0)
            emit(0x42000001)
            for r in rnd.sample([5, 10, 2, 3], 2): mfc0(reg(), r)
        elif k < 0.80:                          # another ASID
            if "asid" in TLBOFF: return
            li32(K0, rnd.choice([0, 1, 1]))
            mtc0(K0, 10)
        else:                                   # what a TLB exception or ll left
            mfc0(reg(), rnd.choice([4, 8, 10, 17, 1, 6]))

    if tlb:                                     # four windows: 64 KiB pages global, 4 KiB pages of ASID 1, 16 KiB pages, 64 KiB pages global
        for i, (pm, size, w, asid, g) in enumerate([(*WSIZE[0], WINDOWS[0], 0, 1), (*WSIZE[1], WINDOWS[1], 1, 0),
                                                    (*WSIZE[2], WINDOWS[2], 0, 0), (*WSIZE[3], WINDOWS[3], 0, 1)]):
            tlbent(2, i, pm, w | asid, LO(FRAME, 3, 1, 1, g), LO(FRAME + size, 3 if i != 2 else 2, 1, 1, g))
    li32(UNC, SCRATCH | 0x20000000)
    li32(CACHED, SCRATCH)
    li32(TAB, TABLE)
    for r in GP:
        I(0x37 if not base or r in WIDE else 0x23, TAB, r, 8 * r + (4 if base and r not in WIDE else 0))   # ld (lw)

    def alu():
        k = rnd.random()
        if k < 0.30:                            # three registers
            fn = rnd.choice([0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
                             0x04, 0x06, 0x07, 0x14, 0x16, 0x17])
            if base and fn in (0x2C, 0x2D, 0x2E, 0x2F): fn = rnd.choice([0x21, 0x23])
            if mode in ("alu", "base") and fn in (0x14, 0x16, 0x17): fn -= 0x10
            if fn in (0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B): R(nsrc(), nsrc(), nreg(), 0, fn)
            else: R(src(), src(), nreg(), 0, fn)
        elif k < 0.48:                          # shifts by a constant
            fn = rnd.choice([0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F])
            if mode in ("alu", "base") and fn >= 0x38: fn = rnd.choice([0x00, 0x02, 0x03])
            R(0, src(), nreg(), rnd.randrange(32), fn)
        elif k < 0.72:                          # immediates
            op = rnd.choice([0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19])
            imm = rnd.choice([0, 1, 0xFFFF, 0x8000, 0x7FFF]) if rnd.random() < 0.3 else rnd.randrange(0x10000)
            if base and op in (0x18, 0x19): op = 0x09
            I(op, 0 if op == 0x0F else src() if op in (0x09, 0x0C, 0x18, 0x19) else nsrc(), nreg(), imm)
        elif k < (0.95 if mode == "div" else 0.84):   # multiply, divide, HI and LO
            fn = rnd.choice([0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x10, 0x12, 0x10, 0x12, 0x11, 0x13])
            if mode == "i32": fn = rnd.choice([0x18, 0x19, 0x1A, 0x1B, 0x1A, 0x1B, 0x1C, 0x1D, 0x10, 0x12, 0x10, 0x12, 0x11, 0x13])
            if mode == "i32d": fn = rnd.choice([0x18, 0x19, 0x1A, 0x1B, 0x1A, 0x1B, 0x1E, 0x10, 0x12, 0x10, 0x12, 0x11, 0x13])
            if mode == "div" and rnd.random() < 0.8: fn = rnd.choice([0x1A, 0x1B, 0x1E, 0x1F, 0x1E, 0x1F, 0x10, 0x12])
            if nomd and fn in (0x1C, 0x1D, 0x1E, 0x1F): fn = rnd.choice([0x18, 0x19, 0x1A, 0x1B])
            if mode == "nodiv" and fn in (0x1E, 0x1F): fn = rnd.choice([0x1C, 0x1D, 0x1C, 0x1D, 0x10, 0x12])   # (more dmult: back to back, and mfhi / mflo right behind)
            if fn in (0x10, 0x12): R(0, 0, nreg(), 0, fn)
            elif fn in (0x11, 0x13):            # (HI and LO are 32 bits without dmult: a 32-bit value)
                R(src() if mode in ("full", "nodiv", "div") else nreg() if base else rnd.choice([0, CACHED, UNC]), 0, 0, 0, fn)
            elif mode in ("i32", "i32d"): R(nsrc(), nsrc(), 0, 0, fn)
            else: R(src(), src(), 0, 0, fn)
        else:                                   # a 64-bit value again
            I(0x37, TAB, wreg(), 8 * rnd.randrange(32))

    def mem_pair():
        # ("ua") a store and, right behind it, an access to the same doubleword at any byte: the store's second word may be the
        # one the next access reads first (the cache's write against its read, and the write buffer's)
        base = UNC if rnd.random() < 0.3 else CACHED
        o = rnd.randrange(64) * 8
        s = rnd.choice([0x2C, 0x2D, 0x2C, 0x2D, 0x3F, 0x2B, 0x2A, 0x2E, 0x28, 0x29])
        l = rnd.choice([0x1A, 0x1B, 0x1A, 0x1B, 0x37, 0x23, 0x22, 0x26, 0x20, 0x25])
        so, lo = o + rnd.randrange(8), o + rnd.randrange(8)
        if s in (0x3F,): so = o
        elif s in (0x2B,): so = o + 4 * rnd.randrange(2)
        elif s in (0x29,): so = o + 2 * rnd.randrange(4)
        if l == 0x37: lo = o
        elif l == 0x23: lo = o + 4 * rnd.randrange(2)
        I(s, base, src(), so)
        I(l, base, wreg() if l in (0x1A, 0x1B, 0x37) else nreg(), lo)

    def mem():
        if ua and rnd.random() < 0.3: return mem_pair()
        base = UNC if rnd.random() < 0.3 else CACHED
        op = rnd.choice([0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37, 0x37,
                         0x28, 0x29, 0x2A, 0x2B, 0x2E, 0x3F, 0x3F, 0x2B])
        if ua and rnd.random() < 0.3:
            op = rnd.choice([0x1A, 0x1B, 0x2C, 0x2D])
        if tlb and rnd.random() < 0.1: op = rnd.choice([0x30, 0x38])           # ll, sc
        off = rnd.randrange(512)
        if tlb and rnd.random() < 0.6:                                          # through a window, the even page or the odd one
            li32(MAPPED, rnd.choice(WINDOWS) + rnd.choice([0, 0, 0x1000, 0x4000, 0x10000]))
            base = MAPPED
        if op in (0x37, 0x3F): off &= ~7
        elif op in (0x23, 0x27, 0x2B, 0x30, 0x38): off &= ~3
        elif op in (0x21, 0x25, 0x29): off &= ~1
        I(op, base, (wreg() if op in (0x27, 0x37, 0x1A, 0x1B) else nreg()) if op < 0x28 or op == 0x37 or op == 0x30 else src(), off)

    def trap():
        # (on 32-bit values: with any other the core has a Reserved Instruction)
        a, b = nreg(), nreg()
        R(0, a, a, 0, 0x00)                     # sll a, a, 0
        if rnd.random() < 0.5:
            R(0, b, b, 0, 0x00)
            R(a, b, 0, 0, rnd.choice([0x30, 0x31, 0x32, 0x33, 0x34, 0x36]))
        else:
            I(0x01, a, rnd.choice([0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E]), rnd.choice([0, 1, 0xFFFF, rnd.randrange(0x10000)]))

    def branch():
        skip = rnd.randrange(4)
        k = rnd.randrange(6)
        likely = rnd.random() < 0.35
        if k == 0: I(0x14 if likely else 0x04, src(), src(), skip + 1)
        elif k == 1: I(0x15 if likely else 0x05, src(), src(), skip + 1)
        elif k == 2: I(0x16 if likely else 0x06, src(), 0, skip + 1)
        elif k == 3: I(0x17 if likely else 0x07, src(), 0, skip + 1)
        elif k == 4: I(0x01, src(), 0x02 if likely else 0x00, skip + 1)
        else: I(0x01, src(), 0x03 if likely else 0x01, skip + 1)
        alu() if rnd.random() < 0.7 else mem()          # the delay slot
        for _ in range(skip): alu()

    def fpu64():
        # ("e2e") the FPU's 64-bit integers, which the core leaves to the runtime
        # whole: through f4, f6, f8 and back into an integer register
        C1 = 0x11 << 26
        mtc1 = lambda rt, fs, d=0: emit(C1 | (5 if d else 4) << 21 | rt << 16 | fs << 11)
        mfc1 = lambda rt, fs, d=0: emit(C1 | (1 if d else 0) << 21 | rt << 16 | fs << 11)
        op = lambda fmt, fs, fd, fn: emit(C1 | fmt << 21 | fs << 11 | fd << 6 | fn)
        k = rnd.randrange(5)
        if k == 0: mtc1(src(), 4, 1); op(0x15, 4, 6, 0x20); mfc1(reg(), 6)                         # cvt.s.l
        elif k == 1: mtc1(src(), 4, 1); op(0x15, 4, 6, 0x21); op(0x11, 6, 8, 0x20); mfc1(reg(), 8)   # cvt.d.l, cvt.s.d
        elif k == 2: mtc1(src(), 4); op(0x10, 4, 6, rnd.choice([0x08, 0x09, 0x0A, 0x0B, 0x25])); mfc1(reg(), 6, 1)
        elif k == 3: mtc1(src(), 4, 1); mfc1(reg(), 4, 1)
        else: mtc1(src(), 4); op(0x10, 4, 6, 0x21); op(0x11, 6, 8, rnd.choice([0x08, 0x09, 0x0A, 0x0B, 0x25])); mfc1(reg(), 8, 1)

    while len(code) < n:
        k = rnd.random()
        if e2e and k < 0.04: fpu64()
        elif k < 0.52: alu()
        elif k < 0.80: mem()
        elif k < 0.95: branch()
        elif tlb and k < 0.98: tlbops()
        elif not e2e: trap()
    if e2e:
        li32(TAB, RESULT)
        for r in range(1, 26):
            I(0x3F, TAB, r, 8 * r)              # sd
        R(0, 0, 1, 0, 0x10); I(0x3F, TAB, 1, 8 * 26)    # mfhi, mflo
        R(0, 0, 1, 0, 0x12); I(0x3F, TAB, 1, 8 * 27)
        li32(1, 0x600DC0DE)
        I(0x2B, TAB, 1, 0)
    emit(0x1000FFFF)                            # b .
    emit(0)
    assert 4 * len(code) < SCRATCH - ENTRY

    rom = bytearray(0x101000 + 0x100000)
    struct.pack_into(">I", rom, 0, 0x80371240)
    struct.pack_into(">I", rom, 8, ENTRY)
    rom[0x20:0x34] = b"MIRLO CPU TEST      "
    rom[0x1000:0x1000 + 4 * len(code)] = struct.pack(f">{len(code)}I", *code)
    rnd = random.Random(seed ^ 0x5EED)          # (the data its own: a shorter program is a prefix, on the same data)
    o = 0x1000 + SCRATCH - ENTRY
    rom[o:o + 512] = bytes(rnd.getrandbits(8) for _ in range(512))
    o = 0x1000 + TABLE - ENTRY
    def edge():
        # (a divider's trouble: the operand is 0, 1, -1, near a power of two, short, or the other's multiple)
        k = rnd.randrange(8)
        sh = rnd.randrange(64)
        if k == 0: v = (1 << sh) + rnd.choice([-1, 0, 1])
        elif k == 1: v = -((1 << sh) + rnd.choice([-1, 0, 1]))
        elif k == 2: v = rnd.getrandbits(rnd.randrange(1, 65))
        elif k == 3: v = -rnd.getrandbits(rnd.randrange(1, 65))
        elif k == 4: v = rnd.choice(SPECIAL)
        elif k == 5: v = rnd.getrandbits(32) << rnd.choice([0, 32])
        elif k == 6: v = rnd.getrandbits(rnd.randrange(1, 33)) * rnd.getrandbits(rnd.randrange(1, 33))
        else: v = (rnd.getrandbits(32) ^ 0x80000000) - 0x80000000
        return v & 0xFFFFFFFFFFFFFFFF
    for i in range(32):
        v = (edge() if mode == "div" else rnd.choice(SPECIAL)) if rnd.random() < (0.9 if mode == "div" else 0.4) else rnd.getrandbits(64)
        if rnd.random() < 0.15: v = (rnd.getrandbits(32) ^ 0x80000000) - 0x80000000 & 0xFFFFFFFFFFFFFFFF   # a 32-bit value
        struct.pack_into(">Q", rom, o + 8 * i, v)
    open(out, "wb").write(rom)
    print(f"{out}: {len(code)} instructions")


if __name__ == "__main__":
    main()
