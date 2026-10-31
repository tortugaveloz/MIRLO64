#!/bin/sh
# Builds tb_dma (tb_dma.cpp: the RSP's host DMA queue, SP_SEMAPHORE, and a host DMA against a running RSP) on rtl/rsp_oc (the overclocked build's RSP).
#   ./build_dma.sh [rtl dir] -> obj_dma_oc/tb_dma
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
R=${1:-$(cd "$HERE/../.." && pwd)/rtl/rsp_oc}
D="$HERE/../../rtl/rsp"
OBJ=${OBJ:-$HERE/obj_dma_oc}
verilator --cc --exe --build -j 8 -O1 -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
    --x-assign fast --x-initial fast -Mdir "$OBJ" --top-module rsp_top_oc -GLANES=${LANES:-8} -GMULW=1 \
    -GDIV_ROM="\"$D/rsp_div_rom.hex\"" -CFLAGS "-O2 -I$HERE ${2:+-DOLD_RTL}" \
    "$R/rsp_lutram_oc.v" "$R/rsp_su_oc.sv" "$R/rsp_vu_oc.sv" "$R/rsp_dpram_oc.sv" "$R/rsp_top_oc.sv" \
    "$HERE/tb_dma.cpp" -o tb_dma > "$OBJ.log" 2>&1 || { grep -E "%Error|error:" "$OBJ.log" | head -30; exit 1; }
echo "OK -> $OBJ/tb_dma"
