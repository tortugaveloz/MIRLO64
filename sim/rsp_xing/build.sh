#!/bin/bash
# The RSP's clock crossing's bench (tb_xing.sv): ./build.sh -> obj_xing/tb_xing
#   obj_xing/tb_xing +dn=5 +dd=6 +pr=50 +preq=100 +pdpc=10 +ph=10 +lat=12 +cycles=300000 +reset=3 +seed=7
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
O="${OBJ:-$HERE/obj_xing}"; mkdir -p "$O"
"$VL" --cc --exe --build -j 8 -O2 --timing -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw -Mdir "$O" \
    --top-module tb_xing "$R/rtl/soc/rsp_gate.sv" "$R/rtl/soc/rsp_clk.sv" "$R/rtl/soc/rsp_xing.sv" "$HERE/tb_xing.sv" "$HERE/tb_xing.cpp" -o tb_xing \
    > "$O/build.log" 2>&1 || { grep -E "%Error" "$O/build.log" | head -30; tail -5 "$O/build.log"; exit 1; }
echo "OK -> $O/tb_xing"
