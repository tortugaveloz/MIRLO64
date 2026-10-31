#!/bin/bash
# The real RSP across the clock crossing, against the model (tb_ocdpc.cpp): ./build_oc.sh -> obj_oc/tb_ocdpc
#   TB_DUTY=2,3 obj_oc/tb_ocdpc task.bin ...
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
O="${OBJ:-$HERE/obj_oc}"; mkdir -p "$O"; S="$R/rtl/rsp"
"$VL" --cc --exe --build -j 8 -O2 --timing -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --x-assign fast --x-initial fast \
    -Mdir "$O" --top-module rsp_oc_wrap -GDIV_ROM="\"$S/rsp_div_rom.hex\"" -CFLAGS "-O2 -I$HERE/../rsp" \
    "$S/rsp_lutram.v" "$S/rsp_su.sv" "$S/rsp_vu.sv" "$S/rsp_dpram.sv" "$S/rsp_top.sv" "$R/rtl/rdp/rdp_dpc.sv" \
    "$R/rtl/soc/rsp_gate.sv" "$R/rtl/soc/rsp_clk.sv" "$R/rtl/soc/rsp_xing.sv" "$HERE/rsp_oc_wrap.sv" "$HERE/tb_ocdpc.cpp" "$HERE/../rsp/rsp.c" -o tb_ocdpc \
    > "$O/build.log" 2>&1 || { grep -E "%Error" "$O/build.log" | head -30; tail -5 "$O/build.log"; exit 1; }
echo "OK -> $O/tb_ocdpc"
