#!/usr/bin/env python3
"""What every COP1 instruction costs in the CPU, in cycles, measured in the lockstep bench (sim/mips).

    sim/mips/fpu_cycles.py [--bench obj_dir/Vmips_core] [--only substring]

Builds a ROM of short blocks: a marker (ori $0,$0,2k), the block repeated REP times, a marker (ori $0,$0,2k+1); the whole program
runs twice, so that the second pass finds the caches warm. The bench (TB_RETLOG=1) prints the cycle every instruction retires in;
a block's cost is (end marker - start marker - 1) / REP, the cycles it takes in a stream of instructions (a block of n instructions
that never stalls is n). The model checks every result as the program runs, so a wrong value fails the run.

The bench must be built with the core's parameters, e.g. GEN="-GFAST=1 -GXWB=1 -GW64_ALU=1 -GW64_SH=1 -GW64_MD=1 -GW64_DIV=1 -GW64_UA=1"
sim/mips/build.sh.
"""
import argparse, os, re, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("MIPS_BIN", os.path.expanduser("~/local/mips-bin/mips-linux-gnu-"))

D = lambda v: struct.pack(">d", v)
S = lambda v: struct.pack(">f", v)

VAL_D = {
    "1.5": D(1.5), "2.25": D(2.25), "1.25": D(1.25), "1.0": D(1.0), "3.0": D(3.0), "256": D(256.0), "2^30": D(2.0**30), "2^60": D(2.0**60),
    "1+ulp": D(1.0 + 2.0**-52), "0": D(0.0), "-0": D(-0.0), "inf": D(float("inf")), "nan": struct.pack(">Q", 0x7FF8000000000000),
    "100.5": D(100.5), "1.0625": D(1.0625), "1.03125": D(1.03125), "-1.5": D(-1.5), "5.0": D(5.0),
}
VAL_S = {"1.5": S(1.5), "2.25": S(2.25), "1.0": S(1.0), "0": S(0.0), "100.5": S(100.5), "-1.5": S(-1.5), "5.0": S(5.0)}
INT = {"w": struct.pack(">i", 1234567), "l": struct.pack(">q", 123456789012), "lw": struct.pack(">q", -1234567)}


class Case:
    def __init__(s, name, group, body, pre=(), rep=8, note=""):
        s.name, s.group, s.body, s.pre, s.rep, s.note = name, group, list(body), list(pre), rep, note


def cases():
    C = []
    def ld(reg, size, val):
        return f"{'ldc1' if size == 'd' else 'lwc1'} $f{reg}, {{{size}:{val}}}($s0)"
    for op in ["add", "sub", "mul", "div"]:
        C.append(Case(f"{op}.s", "arith", [f"{op}.s $f4, $f0, $f2"], [ld(0, "s", "1.5"), ld(2, "s", "2.25")]))
        C.append(Case(f"{op}.s, result in an operand", "dep", [f"{op}.s $f0, $f0, $f2"], [ld(0, "s", "1.0"), ld(2, "s", "1.0")]))
        C.append(Case(f"{op}.d", "arith", [f"{op}.d $f4, $f0, $f2"], [ld(0, "d", "1.5"), ld(2, "d", "2.25")]))
        C.append(Case(f"{op}.d, result in an operand", "dep", [f"{op}.d $f0, $f0, $f2"], [ld(0, "d", "1.0"), ld(2, "d", "1.0")]))
    C.append(Case("sqrt.s", "arith", ["sqrt.s $f4, $f0"], [ld(0, "s", "2.25")]))
    C.append(Case("sqrt.d", "arith", ["sqrt.d $f4, $f0"], [ld(0, "d", "2.25")]))
    for f in "sd":
        for op in ["abs", "neg", "mov"]:
            C.append(Case(f"{op}.{f}", "arith", [f"{op}.{f} $f4, $f0"], [ld(0, f, "1.5")]))
    dv = [("same exponent, no carry", "1.0625", "1.03125"), ("same exponent, carry", "1.5", "1.25"), ("exponents 1 apart", "3.0", "1.5"),
          ("exponents 8 apart", "256", "1.5"), ("exponents 30 apart", "2^30", "1.5"), ("exponents 60 apart", "2^60", "1.5"),
          ("a zero operand", "0", "1.5"), ("an infinity", "inf", "1.5"), ("a NaN", "nan", "1.5")]
    for op in ["add", "sub"]:
        for nm, a, b in dv:
            C.append(Case(f"{op}.d, {nm}", "d-var", [f"{op}.d $f4, $f0, $f2"], [ld(0, "d", a), ld(2, "d", b)]))
    C.append(Case("sub.d, cancel 1 bit (1.5 - 1.0)", "d-var", ["sub.d $f4, $f0, $f2"], [ld(0, "d", "1.5"), ld(2, "d", "1.0")]))
    C.append(Case("sub.d, cancel 52 bits (1+ulp - 1)", "d-var", ["sub.d $f4, $f0, $f2"], [ld(0, "d", "1+ulp"), ld(2, "d", "1.0")]))
    C.append(Case("sub.d, x - x = 0", "d-var", ["sub.d $f4, $f0, $f2"], [ld(0, "d", "1.5"), ld(2, "d", "1.5")]))
    C.append(Case("mul.d, a zero operand", "d-var", ["mul.d $f4, $f0, $f2"], [ld(0, "d", "0"), ld(2, "d", "1.5")]))
    C.append(Case("mul.d, an infinity", "d-var", ["mul.d $f4, $f0, $f2"], [ld(0, "d", "inf"), ld(2, "d", "1.5")]))
    C.append(Case("add.s, exponents 8 apart", "s-var", ["add.s $f4, $f0, $f2"], [ld(0, "s", "100.5"), ld(2, "s", "1.5")]))
    C.append(Case("sub.s, cancel (x - x)", "s-var", ["sub.s $f4, $f0, $f2"], [ld(0, "s", "1.5"), ld(2, "s", "1.5")]))
    C.append(Case("mul.s, a zero operand", "s-var", ["mul.s $f4, $f0, $f2"], [ld(0, "s", "0"), ld(2, "s", "1.5")]))
    for f in "sd":
        for cv in ["cvt.w", "trunc.w", "round.w", "ceil.w", "floor.w", "cvt.l", "trunc.l", "round.l", "ceil.l", "floor.l"]:
            C.append(Case(f"{cv}.{f}", "cvt", [f"{cv}.{f} $f4, $f0"], [ld(0, f, "100.5")]))
    C.append(Case("cvt.s.d", "cvt", ["cvt.s.d $f4, $f0"], [ld(0, "d", "100.5")]))
    C.append(Case("cvt.d.s", "cvt", ["cvt.d.s $f4, $f0"], [ld(0, "s", "100.5")]))
    C.append(Case("cvt.s.w", "cvt", ["cvt.s.w $f4, $f0"], ["lwc1 $f0, {w}($s0)"]))
    C.append(Case("cvt.d.w", "cvt", ["cvt.d.w $f4, $f0"], ["lwc1 $f0, {w}($s0)"]))
    C.append(Case("cvt.s.l", "cvt", ["cvt.s.l $f4, $f0"], ["ldc1 $f0, {l}($s0)"]))
    C.append(Case("cvt.d.l", "cvt", ["cvt.d.l $f4, $f0"], ["ldc1 $f0, {l}($s0)"]))
    C.append(Case("cvt.s.l, a word", "cvt", ["cvt.s.l $f4, $f0"], ["ldc1 $f0, {lw}($s0)"]))
    C.append(Case("cvt.d.l, a word", "cvt", ["cvt.d.l $f4, $f0"], ["ldc1 $f0, {lw}($s0)"]))
    conds = ["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle", "seq", "ngl", "lt", "nge", "le", "ngt"]
    for f in "sd":
        for c in conds:
            C.append(Case(f"c.{c}.{f}", "cmp", [f"c.{c}.{f} $f0, $f2"], [ld(0, f, "1.5"), ld(2, f, "2.25")]))
    C.append(Case("c.eq.d, equal operands", "cmp-var", ["c.eq.d $f0, $f2"], [ld(0, "d", "1.5"), ld(2, "d", "1.5")]))
    C.append(Case("c.lt.d, a negative and a positive", "cmp-var", ["c.lt.d $f0, $f2"], [ld(0, "d", "-1.5"), ld(2, "d", "1.5")]))
    C.append(Case("c.eq.d, a NaN", "cmp-var", ["c.eq.d $f0, $f2"], [ld(0, "d", "nan"), ld(2, "d", "1.5")]))
    C.append(Case("mfc1", "move", ["mfc1 $t1, $f0"], [ld(0, "s", "1.5")]))
    C.append(Case("mtc1", "move", ["mtc1 $t1, $f4"], []))
    C.append(Case("dmfc1", "move", ["dmfc1 $t1, $f0"], [ld(0, "d", "1.5")]))
    C.append(Case("dmtc1", "move", ["dmtc1 $t1, $f4"], []))
    C.append(Case("cfc1", "move", ["cfc1 $t1, $31"], []))
    C.append(Case("ctc1", "move", ["ctc1 $t2, $31"], ["lui $t2, 0x0100"]))
    C.append(Case("lwc1", "mem", ["lwc1 $f4, {s:1.5}($s0)"], []))
    C.append(Case("ldc1", "mem", ["ldc1 $f4, {d:1.5}($s0)"], []))
    C.append(Case("swc1", "mem", ["swc1 $f0, 0($s1)"], [ld(0, "s", "1.5")]))
    C.append(Case("sdc1", "mem", ["sdc1 $f0, 0($s1)"], [ld(0, "d", "1.5")]))
    C.append(Case("lwc1 + add.s using it", "interlock", ["lwc1 $f4, {s:1.5}($s0)", "add.s $f6, $f4, $f2"], [ld(2, "s", "2.25")]))
    C.append(Case("ldc1 + add.d using it", "interlock", ["ldc1 $f4, {d:1.5}($s0)", "add.d $f6, $f4, $f2"], [ld(2, "d", "2.25")]))
    C.append(Case("lwc1 + swc1 of it", "interlock", ["lwc1 $f4, {s:1.5}($s0)", "swc1 $f4, 0($s1)"], []))
    C.append(Case("lwc1 + mfc1 of it", "interlock", ["lwc1 $f4, {s:1.5}($s0)", "mfc1 $t1, $f4"], []))
    C.append(Case("mtc1 + add.s using it", "interlock", ["mtc1 $t1, $f4", "add.s $f6, $f4, $f2"], [ld(2, "s", "2.25"), "lui $t1, 0x3FC0"]))
    C.append(Case("mfc1 + addu using it", "interlock", ["mfc1 $t1, $f0", "addu $t2, $t1, $t1"], [ld(0, "s", "1.5")]))
    C.append(Case("add.s + swc1 of it", "interlock", ["add.s $f4, $f0, $f2", "swc1 $f4, 0($s1)"], [ld(0, "s", "1.5"), ld(2, "s", "2.25")]))
    C.append(Case("add.d + sdc1 of it", "interlock", ["add.d $f4, $f0, $f2", "sdc1 $f4, 0($s1)"], [ld(0, "d", "1.5"), ld(2, "d", "2.25")]))
    C.append(Case("add.s + mfc1 of it", "interlock", ["add.s $f4, $f0, $f2", "mfc1 $t1, $f4"], [ld(0, "s", "1.5"), ld(2, "s", "2.25")]))
    for bc, tr, ops in [("bc1t", "taken", ("1.5", "2.25")), ("bc1f", "not taken", ("1.5", "2.25")),
                        ("bc1t", "not taken", ("2.25", "1.5")), ("bc1f", "taken", ("2.25", "1.5"))]:
        C.append(Case(f"c.lt.s + {bc} {tr}", "branch", ["c.lt.s $f0, $f2", f"{bc} 1f", "nop", "1:"], [ld(0, "s", ops[0]), ld(2, "s", ops[1])],
                      note="compare, branch, delay slot"))
    C.append(Case("c.lt.d + bc1t taken", "branch", ["c.lt.d $f0, $f2", "bc1t 1f", "nop", "1:"], [ld(0, "d", "1.5"), ld(2, "d", "2.25")],
                  note="compare, branch, delay slot"))
    C.append(Case("bc1t taken, flag set long before", "branch", ["bc1t 1f", "nop", "1:"], [ld(0, "s", "1.5"), ld(2, "s", "2.25"), "c.lt.s $f0, $f2"],
                  note="branch, delay slot"))
    # --- the integer side (the multiplier, the dividers, load-use, branches), against the console model's figures (con_model.h)
    ip = ["lui $t3, 0x1234", "ori $t3, $t3, 0x5678", "lui $t4, 0x0123", "ori $t4, $t4, 0x4567"]
    for op in ["mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu"]:
        C.append(Case(f"{op} + mflo", "int-md", [f"{op} {'$zero, ' if 'div' in op else ''}$t3, $t4", "mflo $t1"], ip, rep=4, note="issue, then the result at once"))
        C.append(Case(f"{op} + 7 nops + mflo", "int-md", [f"{op} {'$zero, ' if 'div' in op else ''}$t3, $t4"] + ["nop"] * 7 + ["mflo $t1"], ip, rep=4, note="the 7 nops are in the multiply's shadow"))
    C.append(Case("mult + mflo, then mfhi", "int-md", ["mult $t3, $t4", "mfhi $t1"], ip, rep=4))
    C.append(Case("lw + addu using it", "int-ls", ["lw $t1, 0($s0)", "addu $t2, $t1, $t1"], [], note="load-use"))
    C.append(Case("lw + nop + addu using it", "int-ls", ["lw $t1, 0($s0)", "nop", "addu $t2, $t1, $t1"], []))
    C.append(Case("lw, independent next", "int-ls", ["lw $t1, 0($s0)", "addu $t2, $t3, $t3"], []))
    C.append(Case("ld", "int-ls", ["ld $t1, 0($s0)"], []))
    C.append(Case("sd", "int-ls", ["sd $t1, 0($s1)"], []))
    C.append(Case("sw", "int-ls", ["sw $t1, 0($s1)"], []))
    C.append(Case("lwl + lwr", "int-ls", ["lwl $t1, 0($s0)", "lwr $t1, 3($s0)"], []))
    C.append(Case("ldl + ldr", "int-ls", ["ldl $t1, 0($s0)", "ldr $t1, 7($s0)"], [], rep=4))
    C.append(Case("beq not taken + delay slot", "int-br", ["beq $t3, $t4, 1f", "nop", "1:"], ip))
    C.append(Case("bne taken forward + delay slot", "int-br", ["bne $t3, $t4, 1f", "nop", "1:"], ip))
    C.append(Case("beq taken forward + delay slot", "int-br", ["beq $t3, $t3, 1f", "nop", "1:"], ip))
    C.append(Case("beql not taken (likely, slot annulled)", "int-br", ["beql $t3, $t4, 1f", "nop", "1:"], ip))
    C.append(Case("bnel taken (likely)", "int-br", ["bnel $t3, $t4, 1f", "nop", "1:"], ip))
    C.append(Case("j + delay slot", "int-br", ["j 1f", "nop", "1:"], []))
    C.append(Case("jal + jr $ra", "int-br", ["jal 1f", "nop", "b 2f", "nop", "1: jr $ra", "nop", "2:"], [], rep=4, note="call, return, branch over"))
    # --- jumps: against the console's (a jump and its delay slot are 2 cycles, a call and its return the same)
    C.append(Case("jal, empty callee (jr $ra) and back", "int-jmp", ["jal 1f", "nop", "b 2f", "nop", "1: jr $ra", "nop", "2:"], [], rep=4, note="6 instructions retire; 6 on the console"))
    C.append(Case("jal, callee saves and reloads $ra, returns", "int-jmp", ["jal 1f", "nop", "b 2f", "nop", "1: sw $ra, 0($s1)", "addiu $t1, $t1, 1", "lw $ra, 0($s1)", "addiu $t1, $t1, 1", "jr $ra", "nop", "2:"], [], rep=4, note="a function's prologue and epilogue around its return"))
    C.append(Case("lw $ra, then jr $ra at once", "int-jmp", ["jal 1f", "nop", "b 2f", "nop", "1: sw $ra, 0($s1)", "lw $ra, 0($s1)", "jr $ra", "nop", "2:"], [], rep=4, note="the return address loaded right before the jr (load-use on the console)"))
    C.append(Case("jr $t9 to the next instruction, $t9 set 5 before", "int-jmp", ["lui $t9, %hi(1f)", "addiu $t9, $t9, %lo(1f)", "nop", "nop", "nop", "nop", "jr $t9", "nop", "1:"], [], rep=4, note="a register jump to a fixed target"))
    C.append(Case("jalr $t9 and back", "int-jmp", ["lui $t9, %hi(1f)", "addiu $t9, $t9, %lo(1f)", "jalr $t9", "nop", "b 2f", "nop", "1: jr $ra", "nop", "2:"], [], rep=4, note="an indirect call (a function pointer), the callee returns"))
    C.append(Case("jr $t9, two targets in turn", "int-jmp", ["lui $t9, %hi(1f)", "addiu $t9, $t9, %lo(1f)", "addu $t9, $t9, $t6", "xori $t6, $t6, 4", "jr $t9", "nop", "1: nop", "nop"], ["move $t6, $zero"], rep=8, note="a switch: the target alternates between two instructions"))
    C.append(Case("j, 3 in a row", "int-jmp", ["j 1f", "nop", "1: j 2f", "nop", "2: j 3f", "nop", "3:"], [], rep=4, note="3 jumps and their slots"))
    C.append(Case("mfc0 Count", "int-cp0", ["mfc0 $t1, $9"], []))
    C.append(Case("nop (reference)", "ref", ["nop"], []))
    return C


def build(only):
    cs = [c for c in cases() if not only or only in c.name]
    data, off = bytearray(), {}
    for k, v in VAL_D.items(): off[("d", k)] = len(data); data += v
    for k, v in VAL_S.items(): off[("s", k)] = len(data); data += v + b"\0\0\0\0"
    for k, v in INT.items(): off[(k, "")] = len(data); data += v.ljust(8, b"\0")
    def sub(line):
        return re.sub(r"\{(lw|[dswl]):?([^}]*)\}", lambda m: str(off[(m.group(1), m.group(2))]), line)
    out = ["    .set noreorder\n    .set mips3\n    .set noat\n    .section .text.start,\"ax\"\n    .globl _start\n_start:",
           "    mfc0 $t0, $12\n    lui $t1, 0x2000\n    or $t0, $t0, $t1\n    mtc0 $t0, $12\n    nop\n    nop",
           "    lui $t0, 0x0100\n    ctc1 $t0, $31",
           "    lui $s0, %hi(datab)\n    addiu $s0, $s0, %lo(datab)\n    lui $s1, %hi(scratch)\n    addiu $s1, $s1, %lo(scratch)\n    li $s2, 2\npass:"]
    for k, c in enumerate(cs):
        out.append(f"    # {c.name}")
        for p in c.pre: out.append("    " + sub(p))
        out.append("    nop\n" * 8 + f"    ori $zero, $zero, {2 * k}")
        for i in range(c.rep):
            for b in c.body: out.append("    " + sub(b))
        out.append(f"    ori $zero, $zero, {2 * k + 1}")
    out.append("    addiu $s2, $s2, -1\n    bgtz $s2, pass\n    nop\ndone: b done\n    nop")
    out.append("    .data\n    .align 3\ndatab:\n    .byte " + ",".join(str(b) for b in data) + "\n    .align 3\nscratch: .space 64")
    return cs, "\n".join(out) + "\n"


def run(bench, only):
    cs, asm = build(only)
    T = tempfile.mkdtemp()
    open(f"{T}/t.S", "w").write(asm)
    open(f"{T}/rom.ld", "w").write("ENTRY(_start)\nSECTIONS { . = 0x80000400; .text : { *(.text.start) *(.text .text.*) } .data : { *(.data .data.*) } }\n")
    subprocess.check_call([BIN + "as", "-EB", "-march=vr4300", "-mabi=32", "-o", f"{T}/t.o", f"{T}/t.S"])
    subprocess.check_call([BIN + "ld", "-EB", "-T", f"{T}/rom.ld", "-o", f"{T}/t.elf", f"{T}/t.o"])
    subprocess.check_call([BIN + "objcopy", "-I", "elf32-tradbigmips", "-O", "binary", f"{T}/t.elf", f"{T}/t.bin"])
    code = open(f"{T}/t.bin", "rb").read()
    rom = bytearray(0x101000 + 0x100000)
    struct.pack_into(">I", rom, 0, 0x80371240); struct.pack_into(">I", rom, 8, 0x80000400)
    rom[0x20:0x34] = b"MIRLO FPU CYCLES     "
    rom[0x1000:0x1000 + len(code)] = code
    open(f"{T}/t.z64", "wb").write(rom)
    env = dict(os.environ, TB_RETLOG="1")
    p = subprocess.run([bench, f"{T}/t.z64", "-n", "600000"], env=env, stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, text=True)
    marks = {}
    for line in p.stderr.splitlines():
        m = re.match(r"RET (\d+) (\w+) (\w+)$", line)
        if m:
            w = int(m.group(3), 16)
            if (w >> 26) == 0x0D and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 0:      # ori $0,$0,imm; the last pass wins
                marks[w & 0xFFFF] = int(m.group(1))
    bad = [l for l in p.stderr.splitlines() if not l.startswith("RET") and ("MISMATCH" in l or "FAIL" in l)]
    if bad: sys.stderr.write("\n".join(bad[:5]) + "\n")
    res = []
    for k, c in enumerate(cs):
        a, b = marks.get(2 * k), marks.get(2 * k + 1)
        res.append((c, None if a is None or b is None else (b - a - 1) / c.rep))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", default=os.path.join(HERE, "obj_dir/Vmips_core"))
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    for c, per in run(a.bench, a.only):
        n = len(c.body) - sum(1 for b in c.body if b.endswith(":"))
        if per is None: print(f"{c.group:10s} {c.name:44s} (not reached)"); continue
        print(f"{c.group:10s} {c.name:44s} {per:7.2f}  ({n} instruction{'s' if n > 1 else ''})  {c.note}")


main()
