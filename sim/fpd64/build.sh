#!/bin/bash
# fpu_d64 alone against its model and the host's doubles: build.sh, then obj_dir/Vfpu_d64 [millions of operands per operation] [seed]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$HERE/../../rtl/mips"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
O="$HERE/${OBJ:-obj_dir}"; mkdir -p "$O"
cd "$HERE"
"$VL" --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Mdir "$O" -CFLAGS "-O2 -ffp-contract=off" \
    --top-module fpu_d64 "$R/fpu_d64.sv" tb.cpp > "$O/build.log" 2>&1 || { grep -A4 "%Error" "$O/build.log" | head -30; exit 1; }
echo "OK -> $O/Vfpu_d64"
