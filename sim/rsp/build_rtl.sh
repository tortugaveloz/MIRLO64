#!/bin/sh
# Builds tb_rsp: the RSP's RTL (rtl/rsp) with the reference model, in
# Verilator. LANES=8 for the 8-lane vector unit (default 4), MULW=1 for 4
# lanes with the multiplies on eight; OBJ=<dir>.
#   VERILATOR_ROOT and verilator on PATH, as for the other sims.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
OBJ=${OBJ:-$HERE/obj_rsp${LANES:+_$LANES}}
verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    --x-assign fast --x-initial fast -Mdir "$OBJ" --top-module rsp_top -GLANES=${LANES:-4} ${MULW:+-GMULW=$MULW} \
    -GDIV_ROM="\"$R/rtl/rsp/rsp_div_rom.hex\"" -CFLAGS "-O2 -I$HERE" \
    "$R/rtl/rsp/rsp_lutram.v" "$R/rtl/rsp/rsp_su.sv" "$R/rtl/rsp/rsp_vu.sv" "$R/rtl/rsp/rsp_dpram.sv" "$R/rtl/rsp/rsp_top.sv" \
    "$HERE/tb_rsp.cpp" "$HERE/rsp.c" "$HERE/rsp_time.c" -o tb_rsp > "$OBJ.log" 2>&1 || { grep -E "%Error|error:" "$OBJ.log" | head -30; exit 1; }
echo "OK -> $OBJ/tb_rsp"
