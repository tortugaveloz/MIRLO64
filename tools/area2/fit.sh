#!/bin/bash
# fit.sh <tag> <seed> [extra.qsf|-] [map]
# Snapshots rtl/ platform/ target/ projects/ of this worktree into s_<tag>/ (so later edits cannot reach a run in
# progress), adds the build's value macros + seed + extras, and compiles s_<tag>/projects (symlink: projects_a2_<tag>).
# The macros are the ones the RTL still reads with a value (the RSP's lanes; the CPU's caches -- MIRLO_CPU_IC_LOG2, MIRLO_CPU_DC_LOG2: 9 and 9, 10 and 10 with RSP_OC -- only by EXTRA_MACROS), at the values the RTL
# defaults to, so the bitstream is the one projects/mirlo.qsf alone builds. The
# features that were once macros of their own are always in now.
# REV=mirlo_oc compiles that revision (projects/mirlo_oc.qsf: the overclocked build, RSP_OC; its files are output_files/mirlo_oc.*; the macros and
# extras below go to its qsf, after the base one it sources). With "map": quartus_map only. WAIT=1: the compile in the foreground, with its exit status (else it runs in the background). DROP="RSP_LANES" leaves that macro out; EXTRA_MACROS="A=1 B=2" adds MIRLO_A=1 ....
set -e
W=$(cd "$(dirname "$0")/../.." && pwd)
tag=$1; seed=$2; extra=$3; mode=$4
REV=${REV:-mirlo}
R=$W/s_$tag
rm -rf $R $W/projects_a2_$tag; mkdir -p $R
cp -r $W/rtl $W/platform $W/target $W/projects $R/
rm -rf $R/projects/db $R/projects/incremental_db $R/projects/output_files
ln -s $R/projects $W/projects_a2_$tag
D=$R/projects
sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED $seed/" $D/mirlo.qsf
for m in RSP_LANES=8 ${EXTRA_MACROS:-}; do
  case " ${DROP:-} " in *" ${m%%=*} "*) continue;; esac
  # (RSP_LANES=8 set again in EXTRA_MACROS: left out, the qsf has the one definition)
  case "$m" in RSP_LANES=8) case " ${EXTRA_MACROS:-} " in *" ${m%%=*}="*) continue;; esac;; esac
  echo "set_global_assignment -name VERILOG_MACRO \"MIRLO_$m\"" >> $D/$REV.qsf
done
[ -n "$extra" ] && [ "$extra" != "-" ] && cat "$extra" >> $D/$REV.qsf
cd $D
Q=${QUARTUS_BIN:-$HOME/altera/25.1std/quartus/bin}
if [ -n "${WAIT:-}" ]; then                # WAIT=1: in the foreground, and its exit status
  $Q/quartus_sh --flow compile mirlo -c $REV > $W/projects_a2_$tag.compile.log 2>&1
  exit $?
fi
if [ "$mode" = map ]; then
  nohup $Q/quartus_map mirlo -c $REV > $W/projects_a2_$tag.compile.log 2>&1 &
else
  nohup $Q/quartus_sh --flow compile mirlo -c $REV > $W/projects_a2_$tag.compile.log 2>&1 &
fi
echo started $tag
