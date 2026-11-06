#!/bin/bash
# The shadow plane of the core's "Picture: 32-bit" setting (docs/video_options.md) in the RDP: tb_rdp against the C model, on all planes (the colour
# image, the hidden plane and the shadow plane), with the RTL's shadow_en and the model's shadow (written and read). build_rtl.sh first.
#   ./shadow_regress.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
T=./obj_rdp/tb_rdp; n=0; fail=0
run() { out=$(env "$@" 2>&1); n=$((n+1)); case "$out" in *"RTL == model"*) ;; *) echo "FAIL: $* -> $(echo "$out" | tail -2)"; fail=$((fail+1));; esac
        case "$out" in *TB_FOREIGN*) case "$out" in *"BAD"*) echo "FAIL (foreign): $*"; fail=$((fail+1));; esac;; esac; }
neg() { out=$(env "$@" 2>&1); n=$((n+1)); case "$out" in *MISMATCH*) ;; *) echo "FAIL (must differ): $*"; fail=$((fail+1));; esac; }
for s in 1 2 3 4 5 6 7 8; do run TB_SHADOW=2 $T -r $s 400; done
run TB_SHADOW=2 TB_C16=1 TB_CW=321 $T -r 1 400; run TB_SHADOW=2 TB_C16=1 TB_CW=292 $T -r 2 400      # odd widths: rows that start on the odd halfword
run TB_SHADOW=2 TB_C32=1 $T -r 1 400; run TB_SHADOW=2 TB_C8=1 $T -r 1 400                            # (no plane for a 32-bit or an 8-bit image)
run TB_SHADOW=2 TB_COPY=1 $T -r 5 400; run TB_SHADOW=2 TB_RECT=1 $T -r 5 400; run TB_SHADOW=2 TB_ZDZ=1 $T -r 5 400; run TB_SHADOW=2 TB_CC=1 $T -r 6 400
run TB_SHADOW=2 TB_SDRAM=1 TB_CPU=20 $T -r 5 400; run TB_SHADOW=2 TB_LAT=40 TB_STALL=3 $T -r 3 300
for s in 1 2 3 4 5 6; do run TB_FOREIGN=1 TB_SHADOW=2 TB_C16=1 $T -r $s 600; done                   # others write the colour image between draws
neg TB_SHADOW=1 $T -r 1 400          # the model's blender not reading the plane: it must differ from the RTL, which does
neg TB_SHADOW=model $T -r 1 400      # the model's plane alone
echo "shadow_regress: $n runs, $fail failed"
exit $fail
