#!/bin/bash
# Verilator benches for MIRLO's MIPS SoC (rtl/soc/mirlo_mips.sv):
#   ./build.sh sdr   -> obj_sdr/tb_sdr   (SDRAM controller + PHY + arbiter vs a chip model)
#   ./build.sh soc   -> obj_soc/tb_soc   (the whole SoC; needs tools/mips_inits.py's images)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
case "${1:-soc}" in
sdr)
    "$VL" --cc --exe --build -j 8 -O2 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC --public-flat-rw -Mdir "$HERE/obj_sdr" \
        --top-module sdr_test_top "$R/rtl/soc/sdr_phy.sv" "$R/rtl/soc/sdr_ctrl.sv" "$R/rtl/soc/sdr_arb.sv" \
        "$HERE/sdr_test_top.sv" "$HERE/tb_sdr.cpp" -o tb_sdr > "$HERE/obj_sdr.log" 2>&1 || { tail -30 "$HERE/obj_sdr.log"; exit 1; }
    echo "OK -> $HERE/obj_sdr/tb_sdr" ;;
soc)
    # the Quartus project's macros (projects/mirlo.qsf) but MIRLO_NO_JTAG_UART (the JTAG UART is built, its chain idle)
    DEFS="+define+CMDQ_AW=12 ${EXTRA_DEFS:-}"
    # (the closing set's MIRLO_* macros are always on in the RTL now; EXTRA_DEFS is for the value macros: MIRLO_CPU_IC_LOG2,
    # MIRLO_CPU_DC_LOG2, MIRLO_RSP_LANES. The bench's clocks are the closing set's: the SoC at 62.5 MHz, the game CPU at 3/2 of it.)
    S="$R/rtl/soc"
    "$VL" --cc --exe --build -j 8 -O3 ${VL_THREADS:+--threads $VL_THREADS} -Wno-fatal -Wno-lint -Wno-style -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-MULTIDRIVEN \
        --x-assign fast --x-initial fast -Mdir "${OBJ:-$HERE/obj_soc}" --top-module soc_sim_top $DEFS ${TBC:+-CFLAGS "$TBC"} -I"$S" \
        "$S/sdr_phy.sv" "$S/sdr_ctrl.sv" "$S/sdr_arb.sv" "$S/psram.sv" "$S/afifo.sv" "$S/fifo_fwft.sv" "$S/n64_video.sv" \
        "$S/mirlo_regs.sv" "$S/rcp_mi.sv" "$S/rcp_vi.sv" "$S/rcp_dpc_view.sv" "$S/rcp_ai.sv" "$S/rcp_si.sv" "$S/n64_ports.sv" "$S/cpu_port.sv" "$S/n64_uart.sv" "$S/jtag_uart_bridge.v" \
        "$HERE/sim_cyclonev_jtag.v" "$S/mips_sys.sv" "$S/mirlo_mips.sv" "$S/gb_cart.sv" "$HERE/gb_cart_model.sv" \
        "$R/rtl/mips/fpu_n64.sv" "$R/rtl/mips/mips_core.sv" \
        "$R/rtl/rsp/rsp_lutram.v" "$R/rtl/rsp/rsp_su.sv" "$R/rtl/rsp/rsp_vu.sv" "$R/rtl/rsp/rsp_dpram.sv" "$R/rtl/rsp/rsp_top.sv" \
        "$R/rtl/rdp/rdp_sdpram.v" "$R/rtl/rdp/rdp_rcp_rom.v" "$R"/rtl/rdp/rdp_*.v "$R/rtl/rdp/rdp_dpc.sv" \
        "$HERE/soc_sim_top.sv" "$HERE/tb_soc.cpp" -o tb_soc > "${OBJ:-$HERE/obj_soc}.log" 2>&1 || { grep -E "%Error" "${OBJ:-$HERE/obj_soc}.log" | head -30; tail -5 "${OBJ:-$HERE/obj_soc}.log"; exit 1; }
    echo "OK -> ${OBJ:-$HERE/obj_soc}/tb_soc" ;;
esac
