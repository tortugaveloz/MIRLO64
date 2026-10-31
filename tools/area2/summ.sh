#!/bin/bash
# summ.sh <projects dir>...: fit status, ALMs, LABs, M10K, DSP, worst setup/hold slack, pad check
for d in "$@"; do
  o=$d/output_files
  echo "== $(basename $d)"
  [ -f $o/${REV:-mirlo}.fit.summary ] || { echo " (no fit yet)"; continue; }
  grep -h "Fitter Status\|Logic utilization\|Total RAM\|Total DSP" $o/${REV:-mirlo}.fit.summary | sed 's/^/  /'
  grep -h "Total LABs\|Memory ALUT usage\|ALMs used for\|route-throughs" $o/${REV:-mirlo}.fit.rpt | head -6 | sed 's/^/  /'
  if [ -f $o/${REV:-mirlo}.sta.summary ]; then
    python3 - $o/${REV:-mirlo}.sta.summary <<'PY'
import sys,re
t=open(sys.argv[1]).read().split('\n\n')
su=[];ho=[]
for b in t:
    m=re.search(r"Type\s*:\s*(.*)\nSlack\s*:\s*(\S+)",b)
    if not m: continue
    ty,sl=m.group(1),float(m.group(2))
    nm=ty.split("'")[1] if "'" in ty else ty
    nm=nm.replace('ic|mp1|mf_pllbase_inst|altera_pll_i|','').replace('~PLL_OUTPUT_COUNTER|divclk','')
    (su if 'Setup' in ty else ho).append((sl,nm,ty.split()[0]+' '+ty.split()[1]+' '+ty.split()[2]))
su.sort(); ho.sort()
print("  setup worst:", ", ".join(f"{s:+.3f} {n}" for s,n,_ in su[:4]))
print("  hold  worst:", ", ".join(f"{s:+.3f} {n}" for s,n,_ in ho[:3]))
PY
  fi
  n176=$(grep -h "176279" $o/${REV:-mirlo}.fit.rpt | grep -ci "dram_a\|dram_ba")
  padok=$(grep -E "^; dram_(a|ba)\[[0-9]+\] +; [A-Z0-9]+ +; [0-9A-Z]+ +; [0-9]+ +; [0-9]+ +; [0-9]+ +; yes " $o/${REV:-mirlo}.fit.rpt | wc -l)
  echo "  pad check: 176279 dram_a/ba lines = $n176 (want 0); dram_a/ba with Output Register yes = $padok (want 15)"
done
