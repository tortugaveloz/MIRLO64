#!/bin/bash
# RDP RTL == model: random seeds at three colour-image widths, LFSR+step mode and SDRAM timing, 8-bit and 32-bit images
set -u
cd "$(dirname "$0")/../../sim/rdp"
./build_rtl.sh >/dev/null || exit 1
fail=0; n=0
run() { out=$("$@" 2>&1); n=$((n+1)); case "$out" in *"RTL == model"*) ;; *) echo "FAIL: $* -> $(echo "$out" | tail -2)"; fail=$((fail+1));; esac; }
for cw in 292 296 320; do
  for s in 1 2 3 4 5 6 7 8; do TB_CW=$cw run ./obj_rdp/tb_rdp -r $s 200; done
done
for s in 11 12 13 14; do TB_LFSR=1 TB_STEP=1 run ./obj_rdp/tb_rdp -r $s 100; done
for s in 21 22 23; do TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r $s 150; done
# (those: the depth image in the colour image's bank; these: a bank on)
for s in 24 25 26; do TB_SDRAM=1 TB_CPU=30 TB_ZIMG=0x280800 run ./obj_rdp/tb_rdp -r $s 150; done
# an 8-bit colour image (a byte a pixel), at two widths and with the SoC's memory
for s in 31 32 33; do TB_C8=1 run ./obj_rdp/tb_rdp -r $s 200; done
for s in 34 35; do TB_C8=1 TB_CW=292 run ./obj_rdp/tb_rdp -r $s 200; done
TB_C8=1 TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r 36 150
# a 32-bit colour image (TB_C32: widths 320, 296, 640 and 1000 by the seed, a wide one's spans in two halves), and with
# the SoC's memory
for s in 41 42 43 44 45 46 47 48; do TB_C32=1 run ./obj_rdp/tb_rdp -r $s 200; done
for s in 49 50; do TB_C32=1 TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r $s 150; done
# the combiner's arithmetic: random streams that make it visible (TB_CC), and the functions on their own
for s in 51 52 53 54 55 56 57 58 59 60 61 62; do TB_CC=1 run ./obj_rdp/tb_rdp -r $s 200; done
./cc_unit.sh | grep -q " 0 mismatches" || { echo "FAIL: cc_unit"; fail=$((fail+1)); }; n=$((n+1))
echo "rdp_regress: $n runs, $fail failed"
exit $fail
