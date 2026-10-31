#!/bin/sh
# Builds tb_rsp for the overclocked build's RSP (rtl/rsp_oc; sim/rsp has the faithful one's): the RTL with the reference model, in
# Verilator. LANES (8: the vector unit is built for eight; the parameter stays for the ports) and MULW (not used) as the SoC
# instantiates the RSP; OBJ=<dir>.
#   VERILATOR_ROOT and verilator on PATH, as for the other sims.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
M="$HERE/../rsp"          # (the model, rsp_div_rom.hex)
OBJ=${OBJ:-$HERE/obj_rsp_oc${LANES:+_$LANES}}
verilator --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    --x-assign fast --x-initial fast -Mdir "$OBJ" --top-module rsp_top_oc -GLANES=${LANES:-8} ${MULW:+-GMULW=$MULW} \
    -GDIV_ROM="\"$R/rtl/rsp/rsp_div_rom.hex\"" -CFLAGS "-O2 -I$HERE -I$M" \
    "$R/rtl/rsp_oc/rsp_lutram_oc.v" "$R/rtl/rsp_oc/rsp_su_oc.sv" "$R/rtl/rsp_oc/rsp_vu_oc.sv" "$R/rtl/rsp_oc/rsp_dpram_oc.sv" "$R/rtl/rsp_oc/rsp_top_oc.sv" \
    "$HERE/tb_rsp.cpp" "$M/rsp.c" "$M/rsp_time.c" -o tb_rsp > "$OBJ.log" 2>&1 || { grep -E "%Error|error:" "$OBJ.log" | head -30; exit 1; }
echo "OK -> $OBJ/tb_rsp"
