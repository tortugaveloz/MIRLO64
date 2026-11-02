#!/bin/bash
# RDP RTL == model: random seeds at three colour-image widths, LFSR+step mode and SDRAM timing, 8-bit and 32-bit images
set -u
cd "$(dirname "$0")/../../sim/rdp"
./build_rtl.sh >/dev/null || exit 1                          # (obj_rdp: the noise LFSR stepping for each pixel with coverage, RDP_LFSR_PIXEL, as the model's)
VDEFS= OBJ=obj_free ./build_rtl.sh >/dev/null || exit 1      # (obj_free: the default, free-running)
fail=0; n=0
run() { out=$("$@" 2>&1); rc=$?; n=$((n+1)); case "$out" in *"RTL == model"*) [ $rc -eq 0 ] || { echo "FAIL ($rc): $* -> $(echo "$out" | tail -2)"; fail=$((fail+1)); };; *) echo "FAIL: $* -> $(echo "$out" | tail -2)"; fail=$((fail+1));; esac; }
for cw in 292 296 320; do
  for s in 1 2 3 4 5 6 7 8; do TB_CW=$cw run ./obj_rdp/tb_rdp -r $s 200; done
done
# colour images of an odd width, whose rows start on the odd halfword of a word (16-bit images: TB_C16), alone, in copy mode,
# with the depth image's own dz, and with the SoC's memory
for cw in 289 301; do
  for s in 1 2 3 4 5 6; do TB_C16=1 TB_CW=$cw run ./obj_rdp/tb_rdp -r $s 200; done
  for s in 71 72 73; do TB_C16=1 TB_CW=$cw TB_COPY=1 run ./obj_rdp/tb_rdp -r $s 200; done
  for s in 111 112 113; do TB_C16=1 TB_CW=$cw TB_ZDZ=1 run ./obj_rdp/tb_rdp -r $s 200; done
done
TB_C16=1 TB_CW=289 TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r 21 150
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
# copy mode by groups of pixels, the scissor cutting a rectangle, mirrors and masks (rule 6), 8-bit images in groups of 8
for s in 71 72 73 74 75 76 77 78; do TB_COPY=1 run ./obj_rdp/tb_rdp -r $s 200; done
for s in 79 80; do TB_COPY=1 TB_C8=1 run ./obj_rdp/tb_rdp -r $s 200; done
TB_COPY=1 TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r 81 150
# a texture rectangle's LOD: the next line as the console keeps it (rule 2)
for s in 101 102 103 104 105 106; do TB_LOD=1 TB_RECT=1 run ./obj_rdp/tb_rdp -r $s 300; done
# the depth image's own dz (rule 8): planes at nearly the same depth, AA_EN, Z_CMP, IM_RD, the blender's B the memory's coverage;
# a width whose rows share depth-hidden groups, the SoC's memory, and 32-bit images (a wide span in two halves)
for s in 111 112 113 114 115 116; do TB_ZDZ=1 run ./obj_rdp/tb_rdp -r $s 200; done
for s in 117 118; do TB_ZDZ=1 TB_CW=292 run ./obj_rdp/tb_rdp -r $s 200; done
TB_ZDZ=1 TB_SDRAM=1 TB_CPU=30 run ./obj_rdp/tb_rdp -r 119 150
for s in 120 121 122 123; do TB_ZDZ=1 TB_C32=1 run ./obj_rdp/tb_rdp -r $s 200; done
# the free-running noise LFSR (the RTL's default; obj_free): it steps on every clock (TB_FREE), the streams that read no noise are
# RTL == model, and with the noise colour dither alone the colour words differ from the model's by a step of a channel and in nothing else
for s in 201 202 203 204 205 206; do TB_FREE=1 TB_NONOISE=1 run ./obj_free/tb_rdp -r $s 200; done
for s in 207 208; do TB_FREE=1 TB_NONOISE=1 TB_ZDZ=1 run ./obj_free/tb_rdp -r $s 200; done
TB_FREE=1 TB_NONOISE=1 TB_SDRAM=1 TB_CPU=30 run ./obj_free/tb_rdp -r 209 150
for s in 211 212 213 214 215 216; do TB_FREE=1 TB_NOISE=1 run ./obj_free/tb_rdp -r $s 200; done
# the next command's words and a triangle's start under the drain of the primitive before, the row walker's reads one row ahead:
# slow and uneven memory (long and variable latency, refused requests, another master asking often, the hidden plane's banks
# placed as sdr_arb does), short runs of state commands between primitives (the streams' own), narrow images
for s in 131 132 133 134; do TB_SDRAM=1 TB_CPU=70 TB_HREMAP=2 run ./obj_rdp/tb_rdp -r $s 250; done
for s in 135 136 137; do TB_STALL=2 TB_LAT=14 run ./obj_rdp/tb_rdp -r $s 250; done
for s in 138 139; do TB_STALL=2 TB_LAT=14 TB_C16=1 TB_CW=289 TB_ZDZ=1 run ./obj_rdp/tb_rdp -r $s 250; done
for s in 140 141; do TB_SDRAM=1 TB_CPU=50 TB_C32=1 run ./obj_rdp/tb_rdp -r $s 250; done
for s in 142 143; do TB_SDRAM=1 TB_CPU=50 TB_LOD=1 TB_RECT=1 run ./obj_rdp/tb_rdp -r $s 250; done
# the combiner's arithmetic: random streams that make it visible (TB_CC), and the functions on their own
for s in 51 52 53 54 55 56 57 58 59 60 61 62; do TB_CC=1 run ./obj_rdp/tb_rdp -r $s 200; done
./cc_unit.sh | grep -q " 0 mismatches" || { echo "FAIL: cc_unit"; fail=$((fail+1)); }; n=$((n+1))
echo "rdp_regress: $n runs, $fail failed"
exit $fail
