#!/bin/bash
# ooc.sh <tag> <unit> [tree]: Analysis & Synthesis of one unit alone -- cpu (mips_sys), rdp (rdp_top), rsp (rsp_top),
# video (n64_video), regs (mirlo_regs) -- with the project's synthesis settings and the closing macros, in projects_ooc_<tag>/
# of this worktree (the unit of another tree: its path as the third argument). A minute or two, and steady: the
# whole design's synthesis moves a unit that was not touched by tens of ALUTs (rsp_vu by a hundred) from one run to
# the next; a unit alone gives the same count for the same source. Prints its ALUTs, registers, memory ALUTs, DSPs.
set -e
W=$(cd "$(dirname "$0")/../.." && pwd)
tag=$1; unit=$2; T=${3:-$W}
P=$W/projects_ooc_$tag; rm -rf $P; mkdir -p $P/rtl; cp -r $T/rtl/. $P/rtl/; cd $P
# (the init header names its files by their absolute paths: this copy's)
sed -i "s#\"[^\"]*/rtl/soc/init/#\"$P/rtl/soc/init/#; s#\"[^\"]*/rtl/rsp/rsp_div_rom.hex#\"$P/rtl/rsp/rsp_div_rom.hex#" rtl/soc/mirlo_mips_init.svh
top=ooc_$unit
case $unit in
cpu) cat > top.sv <<SV
\`include "mirlo_mips_init.svh"
module ooc_cpu (input wire clk, input wire rst, output wire s_req, s_line, output wire [31:0] s_addr, input wire s_rvalid, input wire [31:0] s_rdata,
    output wire x_req, output wire [31:0] x_addr, input wire x_ack, input wire [31:0] x_rdata,
    output wire st_valid, st_dev, output wire [31:0] st_addr, st_data, output wire [3:0] st_strb, input wire st_full, st_done, input wire irq, irq_mon);
    wire s_we, s_wrun, x_we; wire [31:0] s_wdata, x_wdata, dbg_pc, dbg_cause, dbg_epc; wire [3:0] s_strb, x_strb;
    mips_sys #(.BOOTROM(\`MIPS_INIT_BOOTROM), .FAST(1'b1), .XWB(1'b1)) sys (.clk, .rst, .s_req, .s_we, .s_addr, .s_wdata, .s_strb, .s_line, .s_wrun, .s_ack(1'b0),
        .s_rvalid, .s_rdata, .s_busy(1'b0), .x_req, .x_we, .x_addr, .x_wdata, .x_strb, .x_ack, .x_rdata, .st_valid, .st_dev, .st_addr, .st_data, .st_strb,
        .st_full, .st_done, .irq, .irq_mon, .dbg_pc, .dbg_cause, .dbg_epc);
endmodule
SV
;;
rsp) cat > top.sv <<SV
\`include "mirlo_mips_init.svh"
module ooc_rsp (input wire clk, rst, h_we, input wire [3:0] h_addr, input wire [31:0] h_wdata, output wire [31:0] h_rdata, input wire h_rd, output wire h_ready, irq,
    output wire r_valid, r_we, output wire [23:2] r_addr, output wire [31:0] r_wdata, input wire r_ready, r_rvalid, input wire [31:0] r_rdata,
    output wire dpc_start_we, dpc_end_we, dpc_status_we, output wire [31:0] dpc_wdata, input wire [31:0] dpc_start, dpc_end, dpc_current, dpc_status,
    input wire [9:0] dx_addr, output wire [31:0] dx_data, output wire halted, unimpl);
    rsp_top #(.LANES(4), .MULW(1'b1), .DIV_ROM(\`MIPS_INIT_RSP_DIV)) rsp (.tr_e(), .tr_e_pc(), .tr_e_we(), .tr_e_wr(), .tr_we(), .tr_wr(), .tr_val(), .tr_v(), .tr_vp(), .*);
endmodule
SV
;;
video) cat > top.sv <<SV
module ooc_video (input wire clk, rst, clk_vid, rst_vid, enable, input wire [25:0] vi_origin, input wire vi_avg, output wire vblank, input wire [11:0] vi_width,
    input wire vi_black, vi_pal, output wire tv_pal, input wire gamma, vi_aa, vi_divot, vi_dedither, input wire [4:0] crop_v, input wire [1:0] crop_r,
    input wire [8:0] vi_lines, input wire [9:0] vi_px0, vi_pw, input wire [7:0] vi_y0, input wire [8:0] vi_rows, input wire [10:0] vi_xs, vi_ys, vi_nw,
    output wire m_valid, output wire [23:0] m_addr, output wire m_urgent, input wire m_ready, m_rvalid, input wire [31:0] m_rdata, output wire vi_line, vi_frame, vi_latch,
    output wire [7:0] vga_r, vga_g, vga_b, output wire vga_hsync, vga_vsync, vga_de, vga_skip);
    n64_video #(.N64FB(1'b1)) video (.osd_en(1'b0), .fps_en(1'b0), .osd_val('0), .*);
endmodule
SV
;;
rdp)  top=rdp_top ;;
regs) top=mirlo_regs ;;
*) echo "unit: cpu rdp rsp video regs"; exit 2 ;;
esac
{ grep -E "^set_global_assignment -name (FAMILY|DEVICE|MUX_RESTRUCTURE|ALLOW_ANY_R[AO]M_SIZE_FOR_RECOGNITION|OPTIMIZATION_TECHNIQUE|PRE_MAPPING_RESYNTHESIS|SAFE_STATE_MACHINE|ADV_NETLIST_OPT_SYNTH_WYSIWYG_REMAP|VERILOG_MACRO) " $W/projects/mirlo.qsf $W/platform/pocket/pocket.tcl | sed 's/^[^:]*://'
  echo "set_global_assignment -name TOP_LEVEL_ENTITY $top"
  echo "set_global_assignment -name PROJECT_OUTPUT_DIRECTORY out"
  echo "set_global_assignment -name NUM_PARALLEL_PROCESSORS 2"
  echo "set_global_assignment -name SEARCH_PATH rtl/soc"
  echo "set_global_assignment -name QIP_FILE rtl/mips.qip"
  for m in RSP=1 RSP_LANES=4 RSP_MULW=1 TLB_RAM=1 GB_CART=1 CPU_CLK=32 CPU_IC_LOG2=10 CPU_DC_LOG2=10 CLK_N64=1 MON=1 CPU_64ALU=1 CPU_64SH=1 CPU_64MD=1 MI=1 PI=1 VI=1 AI=1 SI=1 VI_AA=1 PI2=1; do
    echo "set_global_assignment -name VERILOG_MACRO \"MIRLO_$m\""; done
  [ $unit = cpu ] && echo 'set_instance_assignment -name MUX_RESTRUCTURE OFF -to "mips_sys:sys"'
  [ -f top.sv ] && echo "set_global_assignment -name SYSTEMVERILOG_FILE top.sv"; } > ooc.qsf
echo 'PROJECT_REVISION = "ooc"' > ooc.qpf
${QUARTUS:-$HOME/altera/25.1std/quartus/bin}/quartus_map ooc > map.log 2>&1 || { grep "Error (" map.log | head -5; exit 1; }
f() { grep -h "$1" out/ooc.map.rpt | head -1 | awk -F';' '{gsub(/ /,"",$3); print $3}'; }
echo "$tag [$unit]: ALUTs $(f 'Combinational ALUT usage for logic'), registers $(f 'Dedicated logic registers'), memory ALUTs $(f 'Memory ALUT usage'), DSP $(f 'Total DSP Blocks'), block memory bits $(f 'Total block memory bits')"
