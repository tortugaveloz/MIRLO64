#!/bin/bash
# fit.sh <tag> <seed> [extra.qsf|-] [map]
# Snapshots rtl/ platform/ target/ projects/ of this worktree into s_<tag>/ (so later edits cannot reach a run in
# progress), adds the closing macros + seed + extras, and compiles s_<tag>/projects (symlink: projects_a2_<tag>).
# With "map": quartus_map only. WAIT=1: the compile in the foreground, with its exit status (else it runs in the background). DROP="VI_AA PI2" leaves those macros out. CPUCLK=43 overrides MIRLO_CPU_CLK; EXTRA_MACROS="A=1 B=2" adds MIRLO_A=1 ...
set -e
W=$(cd "$(dirname "$0")/../.." && pwd)
tag=$1; seed=$2; extra=$3; mode=$4
R=$W/s_$tag
rm -rf $R $W/projects_a2_$tag; mkdir -p $R
cp -r $W/rtl $W/platform $W/target $W/projects $R/
rm -rf $R/projects/db $R/projects/incremental_db $R/projects/output_files
ln -s $R/projects $W/projects_a2_$tag
D=$R/projects
sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $seed/" $D/mirlo.qsf
for m in RSP=1 RSP_LANES=8 TLB_RAM=1 GB_CART=1 CPU_CLK=${CPUCLK:-32} CPU_IC_LOG2=10 CPU_DC_LOG2=10 CLK_N64=1 MON=1 CPU_64ALU=1 CPU_64SH=1 CPU_64MD=1 MI=1 PI=1 VI=1 AI=1 SI=1 VI_AA=1 PI2=1 ${EXTRA_MACROS:-}; do
  case " ${DROP:-} " in *" ${m%%=*} "*) continue;; esac
  echo "set_global_assignment -name VERILOG_MACRO \"MIRLO_$m\"" >> $D/mirlo.qsf
done
[ -n "$extra" ] && [ "$extra" != "-" ] && cat "$extra" >> $D/mirlo.qsf
cd $D
Q=${QUARTUS_BIN:-$HOME/altera/25.1std/quartus/bin}
if [ -n "${WAIT:-}" ]; then                # WAIT=1: in the foreground, and its exit status
  $Q/quartus_sh --flow compile mirlo > $W/projects_a2_$tag.compile.log 2>&1
  exit $?
fi
if [ "$mode" = map ]; then
  nohup $Q/quartus_map mirlo > $W/projects_a2_$tag.compile.log 2>&1 &
else
  nohup $Q/quartus_sh --flow compile mirlo > $W/projects_a2_$tag.compile.log 2>&1 &
fi
echo started $tag
