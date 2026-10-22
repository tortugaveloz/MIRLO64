#!/usr/bin/env python3
# gen_tb.py <module.sv> <module> <excluded outputs,comma> [extra C++]: tb.cpp for gen.py's eq_top.sv (eq.sh says how): random
# inputs every cycle (mirlo_regs: r_req in its protocol), the compared outputs every cycle, the excluded ones where they mean
# something
import os, re, sys
src, mod = sys.argv[1], sys.argv[2]
excl = set(sys.argv[3].split(',')) if len(sys.argv) > 3 else set()
txt = open(src).read()
hdr = re.sub(r'//[^\n]*', '', txt[txt.index('module ' + mod):])
hdr = hdr[:hdr.index(');')]
PARAMS = dict((a, b.strip()) for a, b in re.findall(r"parameter\s+(?:int\s+|bit\s+|logic\s+)?(\w+)\s*=\s*([^,)\n/]+)", hdr.split("(", 1)[0] + hdr))
ports = []
for m in re.finditer(r'(input|output)\s+(?:wire|logic|reg)?\s*((?:\[[^\]]+\]\s*)*)([^;]*?)(?=,\s*(?:input|output)|\s*$)', hdr.split('(', 1)[1], re.S):
    d, w, names = m.group(1), m.group(2).strip(), m.group(3)
    for pn, pv in PARAMS.items(): w = re.sub(r"\b%s\b" % pn, "(%s)" % pv, w)
    for n in names.split(','):
        n = n.strip()
        if n: ports.append((d, w, n))
ins = [p for p in ports if p[0] == 'input']; outs = [p for p in ports if p[0] == 'output']
cmp = [p for p in outs if p[2] not in excl]
def bits(w):
    n = 1
    for hi, lo in re.findall(r'\[([^:\]]+):([^\]]+)\]', w): n *= int(eval(hi)) - int(eval(lo)) + 1
    return n
regs = any(nm == "r_req" for _, _, nm in ins)
extra = open(sys.argv[4]).read().split("\n") if len(sys.argv) > 4 else []
addrs = [int(v, 16) for v in re.findall(r"16'h([0-9A-Fa-f]+)", open(src.replace(mod + ".sv", mod + ".svh")).read())] if regs else [0]
c = ['#include "Veq_top.h"', '#include <cstdio>', '#include <cstdlib>', '#include <cstdint>', '#include "verilated.h"',
     'static uint32_t r32() { return (uint32_t)rand() << 17 ^ (uint32_t)rand() << 3 ^ (uint32_t)rand(); }',
     'static const uint16_t A[] = {%s};' % ', '.join(hex(a) for a in addrs),
     'int main(int argc, char **argv) {', '  srand(argc > 1 ? atoi(argv[1]) : 1);',
     '  long n = argc > 2 ? atol(argv[2]) : 2000000; Veq_top *t = new Veq_top; long bad = 0, reads = 0;',
     '  int ph = 0, gap = 0; uint16_t ad = 0; int we = 0;', '  long outst = 0; (void)outst;',
     '  for (long cyc = 0; cyc < n; cyc++) {']
clks = [nm for _, _, nm in ins if nm.startswith("clk")]; rsts = [nm for _, _, nm in ins if nm.startswith("rst")]
skip = set(clks + rsts) | ({"r_req", "r_we", "r_addr", "r_wdata"} if regs else set())
SAMPLE = os.environ.get("SAMPLE", "1")
for _, w, nm in ins:
    if nm in skip: continue
    b = bits(w)
    if b > 64: c.append('    for (int i = 0; i < %d; i++) t->%s[i] = r32();' % ((b + 31) // 32, nm))
    elif b > 32: c.append('    t->%s = (uint64_t)r32() << 32 | r32();' % nm)
    else: c.append('    t->%s = r32() & 0x%xu;' % (nm, (1 << b) - 1))
c += ["    " + l for l in extra if l.strip()]
c += ["    t->%s = cyc < 20;" % r for r in rsts] + ["    int sample = 0;"]
if regs: c += [
      '    if (ph == 0) { if (gap > 0) { gap--; t->r_req = 0; t->r_addr = r32(); } else { ad = (rand() % 8) ? A[rand() % (sizeof A / 2)] : (r32() & 0xFFFC);'
      ' if (rand() % 4 == 0) ad |= (rand() % 2) ? 0x40 : 0x1000; we = rand() % 2; t->r_req = 0; t->r_addr = ad; ph = 1; } }',
      '    else if (ph == 1) { t->r_req = 1; t->r_addr = ad; t->r_we = we; t->r_wdata = r32(); ph = 2; }',
      '    else { t->r_req = 0; t->r_addr = r32(); sample = !we; gap = rand() % 4; ph = 0; }']
c += ["    " + " ".join("t->%s = 0;" % k for k in clks) + " t->eval();", "    if (!%d) sample = (%s);" % (regs, SAMPLE),
      ] + ["    " + l for l in (open(os.environ["EXTRA_POST"]).read().split("\n") if os.environ.get("EXTRA_POST") else []) if l.strip()] + [
      '    if (cyc > 20) {',
      '      bool m = false;']
nmm = len(cmp)
if nmm > 64: c.append('      for (int i = 0; i < %d; i++) if (t->mm[i]) m = true;' % ((nmm + 31) // 32))
else: c.append('      if (t->mm) m = true;')
for _, w, nm in outs:
    if nm in excl:
        c.append('      if (' + os.environ.get("SAMPLE_" + nm, "sample") + ') { reads++; if (t->a_%s != t->b_%s) { if (bad < 10) printf("cycle %%ld: %s %%08x vs %%08x (addr %%04x)\\n", cyc, (unsigned)t->a_%s, (unsigned)t->b_%s, ad); bad++; } }' % (nm, nm, nm, nm, nm))
if nmm > 64:
    c.append('      if (m) { if (bad < 10) { printf("cycle %%ld: outputs differ:", cyc); for (int i = 0; i < %d; i++) if ((t->mm[i / 32] >> (i %% 32)) & 1) printf(" %%d", i); printf("\\n"); } bad++; }' % nmm)
else:
    c.append('      if (m) { if (bad < 10) printf("cycle %ld: outputs differ: mm %llx\\n", cyc, (unsigned long long)t->mm); bad++; }')
c += ['    }', "    " + " ".join("t->%s = 1;" % k for k in clks) + " t->eval();", '  }',
      '  printf("eq: %ld cycles, %ld reads compared, %ld mismatches: %s\\n", n, reads, bad, bad ? "FAIL" : "PASS");',
      '  delete t; return bad != 0;', '}']
open('tb.cpp', 'w').write('\n'.join(c) + '\n')
print(len(ins), 'inputs', len(outs), 'outputs,', nmm, 'compared every cycle')
