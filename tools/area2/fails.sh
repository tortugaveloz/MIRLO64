#!/bin/bash
# fails.sh <projects dir> [slack threshold]: grouped failing endpoints (setup) of a finished fit
d=$1; th=${2:-0.05}
cd $d && cat > /tmp/a2_f_$$.tcl <<TCL
project_open ${REV:-mirlo}
create_timing_netlist -model slow
read_sdc
update_timing_netlist
report_timing -setup -npaths 2000 -nworst 1 -detail summary -file /tmp/a2_f_$$.txt
project_close
TCL
~/altera/25.1std/quartus/bin/quartus_sta -t /tmp/a2_f_$$.tcl >/dev/null 2>&1
python3 - /tmp/a2_f_$$.txt $th <<'PY'
import re,sys
d={}
th=float(sys.argv[2])
for l in open(sys.argv[1]):
    m=re.match(r'; (-?[\d.]+)\s+; (\S+)\s+; (\S+)\s+;',l)
    if not m: continue
    s=float(m.group(1))
    if s>=th: continue
    pre='core_top:ic|mirlo_mips:soc|'
    f=re.sub(r'\[\d+\]','[]',m.group(2).replace(pre,'')); t=re.sub(r'\[\d+\]','[]',m.group(3).replace(pre,''))
    t=re.sub(r'~porta.*|~portb.*','~ram',t)
    k=(f,t)
    if k not in d or s<d[k][0]: d[k]=[s,1]
    else: d[k][1]+=1
for (f,t),(s,n) in sorted(d.items(),key=lambda x:x[1][0])[:25]: print(f"{s:+.3f} x{n:<3d} {f} -> {t}")
PY
rm -f /tmp/a2_f_$$.*
