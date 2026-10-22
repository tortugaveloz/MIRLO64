#!/bin/bash
# The combiner's arithmetic functions of rtl/rdp/rdp_pix.v (between its cc_begin / cc_end) against the model's
# (lang/c/rdp/rdp.c cc_eval, cc_color_res, cc_alpha_res): operands of 0..511 (the shade's range; COMBINED, as A / B / C
# read as signed numbers from 0x180 / 0x180 / 0x100, is the other kind: flags ka kb kc) for cycle 1, and the cycle-0 domain
# (all plain 0..511) -- 2^36 would take a day, so a stride over A/B/D plus 200 million random operands.   ./cc_unit.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
V="${VERILATOR:-$HOME/local/usr/bin/verilator}"
OBJ="$HERE/obj_cc_unit"
rm -rf "$OBJ"; mkdir -p "$OBJ"
{
  echo "module cc_unit(input [8:0] a, b, c, d, input ka, kb, kc, output [8:0] e1, output [7:0] cl, output f256);"
  sed -n '/cc_begin/,/cc_end/p' "$HERE/../../rtl/rdp/rdp_pix.v"
  echo "  wire [9:0] xa = ka ? cc_xab(a) : {1'b0, a}, xb = kb ? cc_xab(b) : {1'b0, b}, xc = kc ? cc_xc(c) : {1'b0, c};"
  echo "  assign e1 = cc_eval1(xa, xb, xc, d); assign cl = cc_clamp(e1); assign f256 = cc_a256(e1);"
  echo "endmodule"
} > "$OBJ/cc_unit.v"
"$V" --cc --exe --build -j 8 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-DECLFILENAME --top-module cc_unit --Mdir "$OBJ/m" \
    "$OBJ/cc_unit.v" "$HERE/cc_unit.cpp" -CFLAGS "-O2 -DRDP_N64 -I$HERE/../../lang/c/rdp" -o cc_unit > "$OBJ/build.log" 2>&1 || { tail -20 "$OBJ/build.log"; exit 1; }
"$OBJ/m/cc_unit"
