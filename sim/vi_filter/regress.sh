#!/bin/bash
# The scan-out against its model (tb_vi.cpp): the filters at 320 (doubled to 640), the VI's scales (440 x 330, 384 in a
# 597-clock window, other pitches and steps), the hi-res pictures (filters off), the crop options, PAL, and the TV system
# changing between fields.
#   ./regress.sh      (builds obj_vi first)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; cd "$HERE"
./build.sh >/dev/null || exit 1
T=./obj_vi/tb_vi; fail=0; n=0
run() { out=$("$@" 2>&1); n=$((n+1)); case "$out" in *": PASS ("*) ;; *) echo "FAIL: $* -> $(echo "$out" | tail -2)"; fail=$((fail+1));; esac; }
run $T 1 0 30 320; run $T 2 1 40 320; run $T 5 1 30 320 1
run $T 1 0 30 440; run $T 3 1 30 440 1                 # 440 x 330: X_SCALE 0x2C0, Y_SCALE 0x580
run $T 7 1 30 384; run $T 9 0 30 384 1                 # 384 wide: 0x20E in a 597 clock window, 222 lines from line 9
run $T 3 0 30 640; run $T 4 0 30 640p                  # hi-res: 640 x 480 (its even rows) and 640 x 240, filters off
TB_PX0=28 TB_PW=584 TB_Y0=12 TB_ROWS=216 run $T 3 1 30 292   # 292 x 216, a pixel two clocks, centred
TB_XS=0x200 TB_YS=0x400 run $T 2 0 30 352              # a 1:1 window of a wider buffer
TB_XS=0x333 TB_YS=0x7FF run $T 4 1 30 500
TB_XS=0x100 TB_YS=0x300 run $T 5 0 30 360              # (an enlargement)
TB_XS=0x3F0 TB_YS=0x400 run $T 6 1 30 480
TB_PX0=100 TB_PW=400 TB_Y0=20 TB_ROWS=150 run $T 8 1 30 320   # a window inside the field
TB_PX0=0 TB_PW=1 run $T 8 0 30 320
TB_CROPV=3 TB_CROPR=2 run $T 8 1 30 320
TB_CROPV=5 TB_CROPR=1 run $T 8 0 30 440
TB_PAL=1 run $T 6 1 30 320; TB_PAL=1 run $T 6 0 30 440
TB_SEQ=0,1,1,0,0 run $T 2 1 30 320
# serrated pictures (VI_CTRL SERRATE, hi-res), sent woven: the picture's 640 x 480 rows in order, a progressive frame of 528 lines
# (PAL 626) each field, its slot word -- libultra's modes (WIDTH 640, Y_SCALE 2.0), a 1280 pitch (WIDTH 1280, Y_SCALE 1.0, 237 lines), a
# window, the crop, PAL, the TV system changing; a serrated 240-row buffer (640ps), or another pitch (704s): not a pair of fields, as it is
run $T 3 0 30 640s; run $T 4 1 30 1280s; run $T 5 0 30 640ps; run $T 6 0 30 704s
TB_ROWS=237 TB_Y0=1 run $T 2 0 30 1280s; TB_ROWS=237 TB_Y0=1 run $T 6 0 30 640s
TB_PX0=100 TB_PW=400 TB_Y0=20 TB_ROWS=150 run $T 8 0 30 640s
TB_CROPV=5 TB_CROPR=1 run $T 8 0 30 1280s
TB_PAL=1 run $T 6 0 30 640s; TB_PAL=1 run $T 7 0 30 1280s
TB_SEQ=0,1,1,0,0 run $T 2 0 30 640s
# a serrated picture the VI scales (WIDTH 640, Y_SCALE under 2.0): the frame's line 2 j + p the field p's line j, as the VI has it
TB_YS=6AA run $T 3 0 30 640s; TB_YS=600 TB_PX0=32 TB_PW=584 TB_Y0=4 TB_ROWS=230 run $T 4 0 30 640s; TB_YS=7FF TB_LOAD=5 run $T 5 0 30 640s
# the SDRAM loaded: the scan-out granted 16 words in each round of n + 1 masters (the arbiter's round robin: 6 ports, so 5 is its worst case, 16.7 %); a serrated
# woven frame reads twice what a hi-res field does (153,600 words: 14.7 % of the SDRAM's cycles)
TB_LOAD=2 run $T 3 0 30 640s; TB_LOAD=5 run $T 3 0 30 1280s; TB_LOAD=5 run $T 4 0 30 640s; TB_LOAD=5 TB_PAL=1 run $T 5 0 30 1280s
TB_RDY=25 run $T 6 0 30 640s
# PAL: libultra's mode (V_SYNC 0x271, V_START 0x5F0239: 237 lines from the field's first line, Y_SCALE 1.0) and a wide one: V_START 0x2F0269 = 285 lines from before the
# field's first (47 against 95), Y_SCALE 0x35C on a 240-row buffer, which the runtime fits to the 240-line field (lang/n64rt/n64rt_vifit.h: 240 lines, Y_SCALE 0x3FD:
# the last line the buffer's row 238 as the VI's 285th is) -- so that none of the buffer's rows is cut, as the 240-line scan-out before the VI's window showed it
TB_PAL=1 TB_Y0=0 TB_ROWS=237 run $T 3 0 30 320
TB_PAL=1 TB_Y0=0 TB_ROWS=240 TB_YS=3FD run $T 4 1 30 320
TB_PAL=1 TB_Y0=0 TB_ROWS=240 TB_YS=3FD TB_CROPV=3 run $T 5 0 30 320
TB_SEQ=0,1,1,0,0 TB_Y0=0 TB_ROWS=240 TB_YS=3FD run $T 6 0 30 320       # (NTSC <-> PAL with the fitted window)
# the runtime's fit of a window taller than the field (host test of lang/n64rt/n64rt_vifit.h)
cc -O1 -o obj_vi/vifit_test vifit_test.c && n=$((n+1)) && { out=$(./obj_vi/vifit_test 2>&1); case "$out" in *"vifit: PASS"*) ;; *) echo "FAIL: vifit_test"; echo "$out" | tail -4; fail=$((fail+1));; esac; }
echo "vi_filter regress: $n runs, $fail failed"
exit $fail
