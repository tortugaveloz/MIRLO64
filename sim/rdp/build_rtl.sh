#!/bin/bash
# Verilate rtl/rdp against the C model (tb_rdp.cpp). Needs VERILATOR_ROOT
# (where Verilator's include/ and bin/ are).
#   ./build_rtl.sh                               -> obj_rdp/tb_rdp
#   DPC=1 ./build_rtl.sh      the RSP with the RDP's command fetch (tb_dpc.cpp) -> obj_rdp_dpc/tb_dpc
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
R="${RDP_DIR:-$HERE/../../rtl/rdp}"
M="$HERE/../../rtl/rdp"
MD="${MODEL_DIR:-$HERE/../../lang/c/rdp}"     # (the model: MODEL_DIR=<dir> for another copy of it)
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
V="${VERILATOR:-$HOME/local/usr/bin/verilator}"
OBJ=${OBJ:-obj_rdp}; CD="-DRDP_N64"
cd "$HERE"
# (make does not see the model's flags change: a build with other ones starts from an empty directory, or the model's
# object of the last build stays, with another layout of its state)
if [ -z "${DPC:-}" ]; then
    FL="$R $MD $CD ${VDEFS:-} ${TB_CFLAGS:-}"
    [ -f "$OBJ/.flags" ] && [ "$(cat "$OBJ/.flags")" = "$FL" ] || rm -rf "$OBJ"
    mkdir -p "$OBJ"; echo "$FL" > "$OBJ/.flags"
fi
if [ -n "${DPC:-}" ]; then
    S="$HERE/../../rtl/rsp"
    "$V" --cc --exe --build -j 8 -O3 --x-assign fast --x-initial fast -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
        --top-module rsp_dpc_wrap -GDIV_ROM="\"$S/rsp_div_rom.hex\"" --Mdir obj_rdp_dpc \
        "$S/rsp_lutram.v" "$S/rsp_su.sv" "$S/rsp_vu.sv" "$S/rsp_dpram.sv" "$S/rsp_top.sv" "$R/rdp_dpc.sv" rsp_dpc_wrap.sv \
        tb_dpc.cpp "$HERE/../rsp/rsp.c" -CFLAGS "-O2 -I$HERE/../rsp" -o tb_dpc > obj_rdp_dpc.log 2>&1 || { grep -E "%Error|error" obj_rdp_dpc.log | head -30; exit 1; }
    echo "OK -> $HERE/obj_rdp_dpc/tb_dpc"
    exit 0
fi
"$V" --cc --exe --build -j 8 -O3 --x-assign fast --x-initial fast \
    -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-BLKSEQ -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL \
    --top-module rdp_top ${VDEFS:-} --Mdir "$OBJ" \
    "$R/rdp_top.v" "$R/rdp_pix.v" "$R/rdp_mem.v" "$R/rdp_tmem.v" "$M/rdp_sdpram.v" "$M/rdp_rcp_rom.v" \
    tb_rdp.cpp "$HERE/../rsp/rsp.c" "$MD/rdp.c" \
    -CFLAGS "-O2 -I$HERE -I$HERE/../rsp -I$MD $CD ${TB_CFLAGS:-}" -o tb_rdp > "$OBJ.log" 2>&1 || { grep -E "%Error|error" "$OBJ.log" | head -30; tail -5 "$OBJ.log"; exit 1; }
echo "OK -> $HERE/$OBJ/tb_rdp"
