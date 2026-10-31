#!/bin/sh
# Builds tb_dma (tb_dma.cpp: the RSP's host DMA queue, SP_SEMAPHORE, and a host DMA against a running RSP) on rtl/rsp.
#   ./build_dma.sh [rtl dir] [old]  -> obj_dma/tb_dma    (the arguments: another checkout's rtl/rsp, to run the bench on the old RTL -- with `old`,
#   the RTL before the h_rd port)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=${1:-$(cd "$HERE/../.." && pwd)/rtl/rsp}
OBJ=${OBJ:-$HERE/obj_dma}
verilator --cc --exe --build -j 8 -O1 -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    --x-assign fast --x-initial fast -Mdir "$OBJ" --top-module rsp_top -GLANES=4 -GMULW=1 \
    -GDIV_ROM="\"$R/rsp_div_rom.hex\"" -CFLAGS "-O2 -I$HERE ${2:+-DOLD_RTL}" \
    "$R/rsp_lutram.v" "$R/rsp_su.sv" "$R/rsp_vu.sv" "$R/rsp_dpram.sv" "$R/rsp_top.sv" \
    "$HERE/tb_dma.cpp" -o tb_dma > "$OBJ.log" 2>&1 || { grep -E "%Error|error:" "$OBJ.log" | head -30; exit 1; }
echo "OK -> $OBJ/tb_dma"
