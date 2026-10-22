#!/bin/bash
# Builds the geom-core RTL simulation (the real mips_geom + Vpu4DFixed) into
# obj_dir/tb_geom_cpu, plus the host-side comparison tools into tools/.
# The firmware: lang/c/geom/build/geom.bin (make in lang/c/geom).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VERILATOR="${VERILATOR:-$HOME/local/usr/bin/verilator}"
# GeomSetupUnit options: default = what projects/mirlo.qsf
# deploys (SU_NO_EDGE, SU_NO_BLEND). SU_DEFINES="..." builds another.
MDIR="${MDIR:-$HERE/obj_dir}"
CSR_H="$R/lang/mips/include/generated/csr.h"      # tools/mirlo_regs.py
csr_ofs() { sed -n "s/^#define $1 (CSR_BASE + \(0x[0-9a-fA-F]*\)L\{0,1\})/\1/p" "$CSR_H"; }
MB_OFS=$(csr_ofs CSR_MAILBOX_BASE)
# RDP's command registers, and the RDP C model linked into the bench (TB_RDP)
RDP_OFS=$(csr_ofs CSR_RDP_BASE)
RDP_DEF="-DTB_RDP -DGEOM_RDP"; RDP_SRC="$R/lang/c/rdp/rdp.c"
SU_DEFINES="${SU_DEFINES-+define+SU_NO_EDGE +define+SU_NO_BLEND}"
[ -n "$MB_OFS" ] && [ -n "$RDP_OFS" ] || { echo "csr.h: mailbox/rdp base not found" >&2; exit 1; }
# Layout inside each block is checked too (tb assumes these exact offsets).
for chk in "CSR_MAILBOX_STATUS_ADDR 0x18" "CSR_MAILBOX_GEOM_MSG_ADDR 0xc" "CSR_RDP_CMD_STATUS_ADDR 0x4" \
           "CSR_RDP_SYNC_COUNT_ADDR 0xc" "CSR_RDP_LOAD_COUNT_ADDR 0x10" "CSR_RDP_STATUS_ADDR 0x14"; do
    set -- $chk; base=$([ "${1#CSR_MAILBOX}" != "$1" ] && echo $MB_OFS || echo $RDP_OFS)
    [ $(( $(csr_ofs $1) - base )) -eq $(( $2 )) ] || { echo "csr.h: $1 moved inside its block" >&2; exit 1; }
done
echo "tb: mailbox @ CSR+$MB_OFS, RDP @ CSR+$RDP_OFS"
# the geom core's memories (rtl/soc/n64_geom.sv): ROM A 16 KiB, ROM B 64 KiB at
# 0x2001_0000 (fetch only), the RAM on its tightly coupled data port
TB_CF="-DTB_ROM_SIZE=0x20000u -DTB_ROM_B=0x10000u"
"$VERILATOR" --cc -CFLAGS "-DTB_MB_OFS=${MB_OFS}u -DTB_RDP_OFS=${RDP_OFS}u $RDP_DEF $TB_CF" \
    --exe --build -O2 -Wno-fatal -Wno-lint -Wno-style --public-flat-rw \
    --top-module geom_cpu_top $SU_DEFINES -I"$R/rtl/vpu" -Mdir "$MDIR" -o tb_geom_cpu -j 8 \
    "$HERE/geom_cpu_top.v" "$R/rtl/mips/mips_geom.sv" "$R/rtl/mips/mips_lite.sv" "$R/rtl/vpu/Vpu4DFixed.v" "$R/rtl/vpu/GeomSetupUnit.v" \
    "$HERE/tb_geom_cpu.cpp" $RDP_SRC
CF="-O2 -w -std=gnu11 -fno-strict-aliasing -m64 -fcommon -ffp-contract=off -fsigned-char \
    -DGEOM_HOST_TEST -DGEOM_FB_HRES=268 -DGEOM_FB_VRES=240 ${RDP_DEF:+-DGEOM_RDP}"
INC="-I$R/lang/c/geom -I$R/lang/c/game"
# shellcheck disable=SC2086
gcc $CF $INC -o "$HERE/tools/hostreplay" "$HERE/tools/hostreplay.c" \
    "$R/lang/c/geom/geom_pipeline.c" "$R/lang/c/geom/geom_triangle.c" \
    "$R/lang/c/geom/geom_fixed.c" "$R/lang/c/geom/geom_vecmath.c" $RDP_SRC -lm
gcc -O1 -w -I"$R/lang/c/geom" -I"$R/lang/c/game" -o "$HERE/tools/mkgdl" "$HERE/tools/mkgdl.c" -lm
echo "OK -> $MDIR/tb_geom_cpu"
