#!/bin/sh
# Generate every suite's streams (build/streams/) and run them through the model
# against the oracle: the model without RDP_EXACT (rdpo) and with it
# (rdpo_exact: docs/rdp_exact.md, the console's rules). One TOTAL line a suite and build;
# the full reports go to build/logs/. Usage: ./run.sh [seeds] (default 10)
set -e
cd "$(dirname "$0")"
n=${1:-10}
model=${MODEL_DIR:-../../lang/c/mrdp}    # (the model: MODEL_DIR=<dir> for another copy, as the Makefile takes it)
export MODEL_DIR="$(cd "$model" && pwd)"
model=$MODEL_DIR
make -s all
rm -rf build/streams build/logs
mkdir -p build/streams build/logs
gen() {    # gen NAME 'VAR=value ...' SUITE SEED [N]
    env $2 ./build/rdpgen "$3" "$4" ${5:-16} > "build/streams/$1_$4.rdps"
}
seeds=$(seq 1 "$n")
for k in flat aa shade z aff persp rect t1; do for s in 1 2 3; do gen cal_$k "RDPGEN_CAL=$k" cal $s 20; done; done
for c in 0 1 2 3; do gen units_cc$c "RDPGEN_CC=$c" units $c; done
for s in 1 2 3; do gen floor "" floor $s; done
gen floor_affine "RDPGEN_PERSP=0" floor 1
for s in $seeds; do
    gen lod2 "" lod2 $s
    gen lod2_exact "RDPGEN_EXACT=1 RDPGEN_PERSP=0" lod2 $s
    gen frac2 "" frac2 $s
    gen frac2_exact "RDPGEN_EXACT=1 RDPGEN_PERSP=0" frac2 $s
    gen rect "" rect $s
    gen lod1 "" lod1 $s
    gen lod1_control "RDPGEN_TEXLOD=0 RDPGEN_CC=0" lod1 $s
    gen lod1_level0 "RDPGEN_LEVEL=0 RDPGEN_DETAIL=0 RDPGEN_CC=0" lod1 $s
    gen lod1_level0_detail "RDPGEN_LEVEL=0 RDPGEN_DETAIL=1 RDPGEN_CC=0" lod1 $s
    gen lod2_t1 "RDPGEN_CC=5" lod2 $s
    gen lod1_t1 "RDPGEN_CC=3" lod1 $s
done
run() {    # run BUILD SUITE...
    b=$1; shift
    for su in "$@"; do
        ./build/$b build/streams/${su}_[0-9]*.rdps > build/logs/${b}_$su.log 2>&1 || true
        printf '%-9s %-19s %s\n' "$b" "$su" "$(sed -n 's/^TOTAL //p' build/logs/${b}_$su.log)"
    done
}
echo "== calibration, no LOD (rdpo; rdpo_exact: RDP_EXACT)"
for b in rdpo rdpo_exact; do
    run $b cal_flat cal_aa cal_shade cal_z cal_aff cal_persp cal_rect cal_t1
done
echo "== LOD (rdpo: the model without RDP_EXACT; rdpo_exact: with it)"
for b in rdpo rdpo_exact; do
    run $b units_cc0 units_cc1 units_cc2 units_cc3 floor floor_affine lod2 lod2_exact frac2 frac2_exact rect lod1 lod1_control lod1_level0 lod1_level0_detail lod2_t1 lod1_t1
done
echo "== sensitivity: the model with one LOD choice changed, on the units streams (each should differ)"
mkdir -p build/mut
for m in "dy:s/^#define LOD_DY_MASK   (~0x7FFF)/#define LOD_DY_MASK   (~0)/" \
         "ones:s/^#define LOD_ONES_ABS  1/#define LOD_ONES_ABS  0/" \
         "dir:s/pr->ddx\[k\] = lft ? at\[AS + k\].dx : /pr->ddx[k] = !lft ? at[AS + k].dx : /" \
         "frac:s/^#define LOD_FRAC_USED 1/#define LOD_FRAC_USED 0/"; do
    n=${m%%:*}
    sed -e "${m#*:}" build/mrdp_hooked.c > build/mut/$n.c
    if cmp -s build/mut/$n.c build/mrdp_hooked.c; then echo "$n: the model's text has changed, no mutation made"; continue; fi
    ${CC:-cc} -O2 -w -DMRDP_N64 -I"$model" rdpo.c build/mut/$n.c build/oracle.o -o build/mut/rdpo_$n
    printf '%-9s %-19s %s\n' "mutant" "$n" "$(./build/mut/rdpo_$n build/streams/units_cc[0-9]_*.rdps | sed -n 's/^TOTAL //p')"
done
echo "== the minimal streams (streams/): colour, coverage and depth words that differ; without RDP_EXACT | with it"
words() {    # words BUILD STREAM: its differing colour, coverage and depth words
    ./build/$1 -q "$2" | sed -nE 's/^(SAME|DIFF) [^:]*: .*\(([0-9]+) colour, ([0-9]+) coverage, ([0-9]+) depth.*/\2 \3 \4/p'
}
for f in streams/*.rdps; do
    printf '%-26s %-18s | %s\n' "$(basename "$f" .rdps)" "$(words rdpo "$f")" "$(words rdpo_exact "$f")"
done
# the captured task dumps under build/triage (TRIAGE_DIR=<dir> for others), if there are any
dumps=$(find "${TRIAGE_DIR:-../../build/triage}" -name 'task_gfx_*.bin' 2>/dev/null | sort)
if [ -n "$dumps" ]; then
    mkdir -p build/frames
    for d in $dumps; do
        g=$(echo "$d" | sed 's|.*/triage/||; s|/task_gfx_|_|; s|/|_|g; s|\.bin$||')
        ./task2rdps.sh "$d" > "build/frames/$g.rdps"
    done
    ./build/rdpo -q build/frames/*.rdps > build/logs/frames.log 2>&1 || true
    echo "== captured frames (build/triage, rdpo): $(sed -n 's/^TOTAL //p' build/logs/frames.log)"
    ./build/rdpo_exact -q build/frames/*.rdps > build/logs/frames_exact.log 2>&1 || true
    echo "== captured frames (build/triage, rdpo_exact): $(sed -n 's/^TOTAL //p' build/logs/frames_exact.log)"
fi
