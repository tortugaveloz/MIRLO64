#!/bin/bash
# ooc.sh <tag> <unit> [tree]: Analysis & Synthesis of one unit alone -- cpu (mips_sys), rdp (rdp_top), rsp (rsp_top),
# video (n64_video), regs (mirlo_regs), and the SoC's smaller blocks -- soc (mirlo_mips whole: its entity table), glue
# (mirlo_mips with the three cores as stubs), ai, si, vi
# (rcp_ai, rcp_si, rcp_vi), arb (sdr_arb, six ports), sdr (sdr_ctrl), dpc (rdp_dpc with rcp_dpc_view), cport (cpu_port),
# ports (port_rdp and port_apf), psram, gbc (gb_cart) -- with the project's synthesis settings and fit.sh's macros, in
# projects_ooc_<tag>/ of this worktree (OOC_ROOT=<dir>: under that directory instead; the unit of another tree: its path as
# the third argument). FIT="1 2 3": then the unit fitted alone, every pin virtual, once a seed (fit_<seed>.rpt, its
# ALMs). A minute or two, and steady: the
# whole design's synthesis moves a unit that was not touched by tens of ALUTs (rsp_vu by a hundred) from one run to
# the next; a unit alone gives the same count for the same source. Prints its ALUTs, registers, memory ALUTs, DSPs.
# The RSP and the video are instantiated as rtl/soc/mirlo_mips.sv does (MIRLO_RSP_LANES lanes with MULW; the
# video's 24-bit origin mask). In the Quartus container: QUARTUS=/opt/intelFPGA_lite/23.1/quartus/bin.
set -e
W=$(cd "$(dirname "$0")/../.." && pwd)
tag=$1; unit=$2; T=${3:-$W}
P=${OOC_ROOT:-$W}/projects_ooc_$tag; rm -rf $P; mkdir -p $P/rtl; cp -r $T/rtl/. $P/rtl/; cd $P
# (the init header names its files by their absolute paths: this copy's)
sed -i "s#\"[^\"]*/rtl/soc/init/#\"$P/rtl/soc/init/#; s#\"[^\"]*/rtl/rsp/rsp_div_rom.hex#\"$P/rtl/rsp/rsp_div_rom.hex#" rtl/soc/mirlo_mips_init.svh
top=ooc_$unit
case $unit in
cpu) cat > top.sv <<SV
\`include "mirlo_mips_init.svh"
module ooc_cpu (input wire clk, input wire rst, output wire s_req, s_line, output wire [31:0] s_addr, input wire s_rvalid, input wire [31:0] s_rdata,
    output wire x_req, output wire [31:0] x_addr, input wire x_ack, input wire [31:0] x_rdata,
    output wire st_valid, st_dev, output wire [31:0] st_addr, st_data, output wire [3:0] st_strb, input wire st_full, st_done, input wire irq, irq_mon,
    input wire sn_t, input wire [20:0] sn_line, output wire sn_hold);
    wire s_we, s_wrun, x_we; wire [31:0] s_wdata, x_wdata, dbg_pc, dbg_cause, dbg_epc; wire [3:0] s_strb, x_strb;
    mips_sys #(.BOOTROM(\`MIPS_INIT_BOOTROM), .FAST(1'b1), .XWB(1'b1)) sys (.clk, .rst, .s_req, .s_we, .s_addr, .s_wdata, .s_strb, .s_line, .s_wrun, .s_ack(1'b0),
        .s_rvalid, .s_rdata, .s_busy(1'b0), .x_req, .x_we, .x_addr, .x_wdata, .x_strb, .x_ack, .x_rdata, .st_valid, .st_dev, .st_addr, .st_data, .st_strb,
        .st_full, .st_done, .irq, .irq_mon, .sn_t, .sn_line, .sn_hold, .dbg_pc, .dbg_cause, .dbg_epc);
endmodule
SV
;;
rsp) cat > top.sv <<SV
\`include "mirlo_mips_init.svh"
module ooc_rsp (input wire clk, rst, h_we, input wire [3:0] h_addr, input wire [31:0] h_wdata, output wire [31:0] h_rdata, input wire h_rd, output wire h_ready, irq,
    output wire r_valid, r_we, output wire [23:2] r_addr, output wire [31:0] r_wdata, input wire r_ready, r_rvalid, input wire [31:0] r_rdata,
    output wire dpc_start_we, dpc_end_we, dpc_status_we, output wire [31:0] dpc_wdata, input wire [31:0] dpc_start, dpc_end, dpc_current, dpc_status,
    input wire [9:0] dx_addr, output wire [31:0] dx_data, output wire halted, unimpl);
    rsp_top #(.LANES(\`MIRLO_RSP_LANES), .MULW(1'b1), .DIV_ROM(\`MIPS_INIT_RSP_DIV)) rsp (.tr_e(), .tr_e_pc(), .tr_e2(), .tr_e_we(), .tr_e_wr(), .tr_we(), .tr_wr(), .tr_val(), .tr_v(), .tr_vp(), .*);
endmodule
SV
;;
video) cat > top.sv <<SV
module ooc_video (input wire clk, rst, clk_vid, rst_vid, enable, input wire [25:0] vi_origin, input wire vi_il, output wire vblank, input wire [11:0] vi_width, input wire vi_px32,
    input wire vi_black, vi_pal, output wire tv_pal, input wire gamma, vi_aa, vi_divot, vi_dedither, input wire [4:0] crop_v, input wire [1:0] crop_r,
    input wire [8:0] vi_lines, input wire [9:0] vi_px0, vi_pw, input wire [7:0] vi_y0, input wire [8:0] vi_rows, input wire [10:0] vi_xs, input wire [11:0] vi_ys, input wire [10:0] vi_nw,
    output wire m_valid, output wire [23:0] m_addr, output wire m_urgent, input wire m_ready, m_rvalid, input wire [31:0] m_rdata, output wire vi_line, vi_frame, vi_latch,
    output wire [7:0] vga_r, vga_g, vga_b, output wire vga_hsync, vga_vsync, vga_de, vga_skip);
    n64_video #(.ORIGIN_MASK(24'hFFFFFF), .N64FB(1'b1)) video (.osd_en(1'b0), .fps_en(1'b0), .osd_val('0), .*);
endmodule
SV
;;
rdp)  top=rdp_top ;;
regs) top=mirlo_regs ;;
soc)  top=mirlo_mips ;;
glue) top=mirlo_mips                    # (mirlo_mips with the CPU, the RSP and the RDP as glue_stubs.sv's stubs: minutes, not twenty)
      cp $W/tools/area2/glue_stubs.sv rtl/soc/mips_sys.sv; : > rtl/rsp/rsp_top.sv; : > rtl/rdp/rdp_top.v ;;
ai)   top=rcp_ai ;;
si)   top=rcp_si ;;
vi)   top=rcp_vi ;;
arb)  top=sdr_arb; params="N=6" ;;
sdr)  top=sdr_ctrl ;;
cport) top=cpu_port ;;
psram) top=psram ;;
gbc)  top=gb_cart ;;
dpc) cat > top.sv <<SV
module ooc_dpc (input wire clk, rst, start_we0, end_we, status_we, input wire [31:0] wdata, output wire [23:0] start, end_, current, end_rd,
    output wire [31:0] st, input wire rdp_busy, sync_full, output wire m_valid, output wire [20:0] m_addr, input wire m_ready, m_rvalid, input wire [31:0] m_rdata,
    output wire [9:0] dx_addr, input wire [31:0] dx_data, output wire c_valid, output wire [31:0] c_data, input wire c_ready);
    wire start_ok; wire [31:0] st_raw;
    rcp_dpc_view view (.clk, .rst, .end_we, .status_we, .wdata, .sync_full, .st_raw, .cur(current), .end_, .start_ok, .st);
    rdp_dpc dpc (.clk, .rst, .start_we(start_we0 && start_ok), .end_we, .status_we, .wdata, .start, .end_, .end_rd, .current, .status(st_raw),
        .rdp_busy, .m_valid, .m_addr, .m_ready, .m_rvalid, .m_rdata, .dx_addr, .dx_data, .c_valid, .c_data, .c_ready);
endmodule
SV
;;
ports) cat > top.sv <<SV
module ooc_ports (input wire clk, rst,
    input wire m_cmd_valid, m_cmd_we, input wire [31:0] m_cmd_addr, output wire m_cmd_ready, input wire m_wdata_valid, input wire [31:0] m_wdata,
    input wire [3:0] m_wdata_we, output wire m_wdata_ready, m_rdata_valid, output wire [31:0] m_rdata, output wire p_valid, p_we, output wire [23:0] p_addr,
    output wire [31:0] p_wdata, output wire [3:0] p_be, input wire p_ready, p_rvalid, input wire [31:0] p_rdata,
    input wire cyc, stb, we, input wire [29:0] adr, input wire [31:0] dat_w, input wire [3:0] sel, output wire ack, output wire [31:0] dat_r,
    input wire x_sel, output wire x_valid, input wire x_ack, input wire [31:0] x_rdata, output wire a_valid, a_we, output wire [23:0] a_addr,
    output wire [31:0] a_wdata, output wire [3:0] a_be, input wire a_ready, a_rvalid);
    port_rdp pm (.clk, .rst, .m_cmd_valid, .m_cmd_we, .m_cmd_addr, .m_cmd_ready, .m_wdata_valid, .m_wdata, .m_wdata_we, .m_wdata_ready, .m_rdata_valid,
        .m_rdata, .p_valid, .p_we, .p_addr, .p_wdata, .p_be, .p_ready, .p_rvalid, .p_rdata);
    port_apf pa (.clk, .rst, .cyc, .stb, .we, .adr, .dat_w, .sel, .ack, .dat_r, .x_sel, .x_valid, .x_ack, .x_rdata,
        .p_valid(a_valid), .p_we(a_we), .p_addr(a_addr), .p_wdata(a_wdata), .p_be(a_be), .p_ready(a_ready), .p_rvalid(a_rvalid), .p_rdata);
endmodule
SV
;;
*) echo "unit: cpu rdp rsp video regs soc ai si vi arb sdr dpc cport ports psram gbc"; exit 2 ;;
esac
{ grep -E "^set_global_assignment -name (FAMILY|DEVICE|MUX_RESTRUCTURE|ALLOW_ANY_R[AO]M_SIZE_FOR_RECOGNITION|OPTIMIZATION_TECHNIQUE|PRE_MAPPING_RESYNTHESIS|SAFE_STATE_MACHINE|ADV_NETLIST_OPT_SYNTH_WYSIWYG_REMAP|VERILOG_MACRO) " $W/projects/mirlo.qsf $W/platform/pocket/pocket.tcl | sed 's/^[^:]*://'
  echo "set_global_assignment -name TOP_LEVEL_ENTITY $top"
  echo "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY out"
  echo "set_global_assignment -name NUM_PARALLEL_PROCESSORS 2"
  echo "set_global_assignment -name SEARCH_PATH rtl/soc"
  echo "set_global_assignment -name QIP_FILE rtl/mips.qip"
  for m in RSP_LANES=8 CPU_IC_LOG2=10 CPU_DC_LOG2=10; do
    echo "set_global_assignment -name VERILOG_MACRO \"MIRLO_$m\""; done
  [ $unit = cpu ] && echo 'set_instance_assignment -name MUX_RESTRUCTURE OFF -to "mips_sys:sys"'
  [ $unit = soc ] && echo 'set_instance_assignment -name MUX_RESTRUCTURE OFF -to "mips_sys:sys"'
  for pv in ${params:-}; do echo "set_parameter -name ${pv%%=*} ${pv#*=}"; done
  [ -f top.sv ] && echo "set_global_assignment -name SYSTEMVERILOG_FILE top.sv"
  # (FIT: its pins virtual -- an assignment synthesis takes -- and a 16 ns clock for each of its clocks)
  [ -n "${FIT:-}" ] && { echo "set_instance_assignment -name VIRTUAL_PIN ON -to *"; echo "set_global_assignment -name SDC_FILE ooc.sdc"; }
  true; } > ooc.qsf
for c in clk clk_vid clk_c; do echo "if {[get_collection_size [get_ports -nowarn $c]] > 0} { create_clock -name $c -period 16.000 [get_ports $c] }"; done > ooc.sdc
echo 'PROJECT_REVISION = "ooc"' > ooc.qpf
${QUARTUS:-$HOME/altera/25.1std/quartus/bin}/quartus_map ooc > map.log 2>&1 || { grep "Error (" map.log | head -5; exit 1; }
f() { grep -h "$1" out/ooc.map.rpt | head -1 | awk -F';' '{gsub(/ /,"",$3); print $3}'; }
echo "$tag [$unit]: ALUTs $(f 'Combinational ALUT usage for logic'), registers $(f 'Dedicated logic registers'), memory ALUTs $(f 'Memory ALUT usage'), DSP $(f 'Total DSP Blocks'), block memory bits $(f 'Total block memory bits')"
# FIT="<seeds>": the unit placed alone, once a seed
[ -n "${FIT:-}" ] || exit 0
cp ooc.qsf ooc.map.qsf
for seed in $FIT; do
  { cat ooc.map.qsf; echo "set_global_assignment -name SEED $seed"; } > ooc.qsf
  ${QUARTUS:-$HOME/altera/25.1std/quartus/bin}/quartus_fit ooc > fit_$seed.log 2>&1 || { grep "Error (" fit_$seed.log | head -5; exit 1; }
  cp out/ooc.fit.rpt fit_$seed.rpt
  g() { grep -m1 "$1" fit_$seed.rpt | awk -F';' '{gsub(/ /,"",$3); print $3}'; }
  echo "$tag [$unit] seed $seed: ALMs $(g 'Logic utilization (ALMs needed'), registers $(g '; Total registers')"
done
