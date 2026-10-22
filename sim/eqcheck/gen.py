#!/usr/bin/env python3
# gen.py <module.sv> <module> <cmp-every-cycle-excluded outputs,comma>: eq_top.sv instantiating <module>_old and <module> on the
# same inputs, every input a top input, each output pair compared (mm[i]: output i differs), the excluded ones out as a_ / b_
import os, re, sys
src, mod, excl = sys.argv[1], sys.argv[2], set(sys.argv[3].split(',')) if len(sys.argv) > 3 else set()
txt = open(src).read()
hdr = re.sub(r'//[^\n]*', '', txt[txt.index('module ' + mod):])
hdr = hdr[:hdr.index(');')]
hdr = re.sub(r'//[^\n]*', '', hdr)
PARAMS = dict((a, b.strip()) for a, b in re.findall(r"parameter\s+(?:int\s+|bit\s+|logic\s+)?(\w+)\s*=\s*([^,)\n/]+)", hdr.split("(", 1)[0] + hdr))
ports = []   # (dir, width-text, name)
for m in re.finditer(r'(input|output)\s+(?:wire|logic|reg)?\s*((?:\[[^\]]+\]\s*)*)([^;]*?)(?=,\s*(?:input|output)|\s*$)', hdr.split('(', 1)[1], re.S):
    d, w, names = m.group(1), m.group(2).strip(), m.group(3)
    for pn, pv in PARAMS.items(): w = re.sub(r"\b%s\b" % pn, "(%s)" % pv, w)
    for n in names.split(','):
        n = n.strip()
        if n: ports.append((d, w, n))
ins = [p for p in ports if p[0] == 'input']; outs = [p for p in ports if p[0] == 'output']
o = ['`default_nettype none', 'module eq_top (']
decl = [f'    input wire {w} {n}' for _, w, n in ins]
cmp = [p for p in outs if p[2] not in excl]
decl.append(f'    output wire [{len(cmp)-1}:0] mm')
for _, w, n in outs:
    if n in excl: decl += [f'    output wire {w} a_{n}', f'    output wire {w} b_{n}']
o.append(',\n'.join(decl) + ');')
for _, w, n in outs:
    o.append(f'wire {w} o_{n}, n_{n};')
conn = lambda pfx: ', '.join([f'.{n}({n})' for _, _, n in ins] + [f'.{n}({pfx}_{n})' for _, _, n in outs])
o.append(f'{mod}_old u_old ({conn("o")});')
o.append(f'{mod} u_new ({conn("n")});')
for i, (_, w, n) in enumerate(cmp):
    o.append(f'assign mm[{i}] = o_{n} != n_{n};')
for _, w, n in outs:
    if n in excl: o.append(f'assign a_{n} = o_{n}; assign b_{n} = n_{n};')
o.append('endmodule')
open('eq_top.sv', 'w').write('\n'.join(o) + '\n')
open('eq_names.txt', 'w').write('\n'.join(n for _, _, n in cmp) + '\n')
print(len(ins), 'inputs', len(outs), 'outputs')
for d, w, n in ports: print(d, w, n)
