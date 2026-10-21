// Mirlo's game CPU on the MIPS SoC: mips_core (rtl/mips/mips_core.sv) with
// MIRLO's memory map -- no TLB, big-endian (the N64's order), every address
// physical but the N64 window:
//   0x4000_0000-0x43FF_FFFF  SDRAM, cached (the s_* port: lines and words)
//   0x1FC0_0000-0x1FC0_0FFF  the boot ROM (KSEG1 0xBFC0_0000: the reset
//                            vector, and the exception vectors while BEV)
//   anything else            the device port (x_*): single words, uncached
//                            -- the registers at 0xF000_0000, the audio
//                            core at 0x6000_0000, the geom RAM at 0x2000_8000
//   0x8000_0000 / 0xA000_0000 + 8 MiB  an N64 game's RDRAM: SDRAM 0x4380_0000
//                            (KSEG0 cached, KSEG1 not; mips_core's N64WIN)
// The exception vector with BEV 0 is 0x4000_0180 (the program's crt0).
`default_nettype none

module mips_sys #(
    parameter              BOOTROM = "",         // $readmemh file, 32-bit words
    parameter int          BOOT_WORDS = 1024,
    parameter bit          FAST = 0,            // mips_core's FAST; the port's answers by registers (below)
    parameter bit          XWB = 0              // mips_core's XWB: the stores leave by st_*, s_* and x_* only read
) (
    input  wire         clk,
    input  wire         rst,
    // SDRAM: byte addresses (0x4xxx_xxxx); line reads (8 words) and single words
    output logic        s_req,
    output logic        s_we,
    output logic [31:0] s_addr,
    output logic [31:0] s_wdata,
    output logic [3:0]  s_strb,
    output logic        s_line,
    output logic        s_wrun,             // a write of a run: may be taken as the last one is granted
    input  wire         s_ack,
    input  wire         s_rvalid,
    input  wire  [31:0] s_rdata,
    input  wire         s_busy,             // a write taken from here is not in the SDRAM yet
    // devices: single words, held until x_ack (reads: data with the ack)
    output logic        x_req,
    output logic        x_we,
    output logic [31:0] x_addr,
    output logic [31:0] x_wdata,
    output logic [3:0]  x_strb,
    input  wire         x_ack,
    input  wire  [31:0] x_rdata,
    // (XWB) the stores: to the SDRAM, or (st_dev) to a device, which answers with st_done
    output wire         st_valid, st_dev,
    output wire  [31:0] st_addr, st_data,
    output wire  [3:0]  st_strb,
    input  wire         st_full, st_done,
    input  wire         irq,                // Cause.IP2
    input  wire         irq_mon,            // MIRLO_MON: the monitor level's interrupt
    // the other masters' writes into the SDRAM, for the D-cache to drop the lines they write (mips_core's SNOOP)
    input  wire         sn_t,
    input  wire  [20:0] sn_line,
    output wire         sn_hold,
    output logic [31:0] dbg_pc, dbg_cause, dbg_epc
);

logic        c_req, c_we, c_line, c_ack, c_rvalid;
logic [31:0] c_addr, c_wdata, c_rdata, dbg_status;
logic [3:0]  c_strb;
logic        hwx_on;

// The TLB. An N64 game's mapped addresses are those below 0x2000_0000 (Super
// Mario 64's title-screen head at 0x0400_0000); a miss is the N64's
// exception, at Mirlo's vector. mips_core's translation table is in block
// RAM, which the runtime fills from what the game maps (~75 ALMs, 4 M10K).
localparam bit CPU_TLB = 1'b0;
localparam bit CPU_TLB_RAM = 1'b1;
// mips_core's FPG: two page registers on the fetch side, so that code run from a mapped page whose table entry is
// cacheable (COP0 23's bit 5) goes through the I-cache. MIRLO_NO_FPG: a mapped fetch is the slow path (A/B in the sim)
`ifdef MIRLO_NO_FPG
localparam bit CPU_FPG = 1'b0;
`else
localparam bit CPU_FPG = 1'b1;
`endif
// The VR4300's 64-bit integers (an N64 game's own code has them: its long
// long helpers, a -mips3 compiler's register saves and block moves), by
// mips_core's levels. What the CPU leaves out is a Reserved Instruction, for
// the runtime to do. In: the registers, ld and sd, the unit (dadd, dsub, logic
// and compares on 64-bit values), its d-shifts, dmult (on the DSP) and 64-bit
// HI and LO; not ddiv: it is the runtime's.
localparam bit CPU_W64 = 1'b1;
localparam bit CPU_W64_ALU = 1'b1;
localparam bit CPU_W64_SH = 1'b1;
localparam bit CPU_W64_MD = 1'b1;
localparam bit CPU_W64_DIV = 1'b0;
localparam bit CPU_FAST = FAST;
// MIRLO_CPU_IC_LOG2, MIRLO_CPU_DC_LOG2: the caches' lines (the VR4300's are 512
// and 256: 16 KiB and 8 KiB; more are block RAM only: mips_core)
// MIRLO_MON: mips_core's MON -- the monitor level and the game's own exception vector, for a game that
// runs its own libultra (docs/mips.md)
localparam bit CPU_MON = 1'b1;
// MIRLO_MI, MIRLO_PI: mips_core's MI, PIW -- the MI and PI_STATUS in the SoC's hardware (rcp_mi.sv)
localparam bit CPU_MI = 1'b1;
localparam bit CPU_PIW = 1'b1;
localparam bit CPU_VIW = 1'b1;
localparam bit CPU_AIW = 1'b1;
// MIRLO_SI: mips_core's SIW -- the SI's registers and PIF RAM (rcp_si.sv). PIF RAM is 0x1FC0_07C0, inside the boot ROM's 4 KiB:
// while the pass-through is on (hwx_on: the runtime has set MCTL's bit 8) the boot ROM is not there for those 64 bytes
localparam bit CPU_SIW = 1'b1;
// MIRLO_PI2: mips_core's PI2W -- the cartridge's bus, domain 2 and the ROM's reads (rcp_mi.sv's PI2: the PI's engine)
localparam bit CPU_PI2W = CPU_PIW;
// and the ROM's last 16 MiB (0x1300_0000..: a ROM of up to 64 MiB, mirlo_mips.sv)
localparam bit CPU_ROM64W = CPU_PI2W;
// mips_core's SNOOP: the D-cache drops a line the RDP, the RSP's DMA or the bridge (the PI's engine, the SI's DMA)
// writes into: its 32-byte lines are two of the VR4300's (mirlo_mips.sv takes those writes from the arbiter)
localparam bit CPU_SNOOP = 1'b1;
`ifndef MIRLO_CPU_IC_LOG2
`define MIRLO_CPU_IC_LOG2 10
`endif
`ifndef MIRLO_CPU_DC_LOG2
`define MIRLO_CPU_DC_LOG2 10
`endif
mips_core #(.TLB_EN(CPU_TLB), .TLB_RAM(CPU_TLB_RAM), .FLAT(1), .BIG(1), .N64WIN(1), .MON(CPU_MON), .MI(CPU_MI), .PIW(CPU_PIW), .VIW(CPU_VIW), .AIW(CPU_AIW), .SIW(CPU_SIW), .PI2W(CPU_PI2W), .ROM64W(CPU_ROM64W), .EXC_BASE(32'h4000_0000), .W64(CPU_W64), .W64_ALU(CPU_W64_ALU), .W64_SH(CPU_W64_SH), .W64_MD(CPU_W64_MD), .W64_DIV(CPU_W64_DIV), .FAST(CPU_FAST), .FPG(CPU_FPG),
            .IC_LOG2(`MIRLO_CPU_IC_LOG2), .DC_LOG2(`MIRLO_CPU_DC_LOG2), .XWB(XWB), .SNOOP(CPU_SNOOP)) cpu (
    .clk, .rst, .reset_pc(32'hBFC0_0000),
    .mem_req(c_req), .mem_addr(c_addr), .mem_we(c_we), .mem_wdata(c_wdata), .mem_wstrb(c_strb), .mem_line(c_line), .mem_wrun(s_wrun),
    .mem_ack(c_ack), .mem_rvalid(c_rvalid), .mem_rdata(c_rdata),
    .st_valid, .st_dev, .st_addr, .st_data, .st_strb, .st_full, .st_done,
    .irq_rcp(irq), .irq_mon(irq_mon), .sn_t, .sn_line, .sn_hold,
    .tr_retire(), .tr_pc(), .tr_insn(), .tr_wr(), .tr_rd(), .tr_val(), .tr_exc(), .tr_exc_int(), .tr_exc_pc(),
    .tr_exc_code(), .tr_fwe_even(), .tr_fwe_odd(), .tr_fwidx(), .tr_fweven(), .tr_fwodd(), .tr_fcr31(),
    .dbg_pc, .dbg_cause, .dbg_status, .dbg_epc, .hwx_on
);

wire is_sd   = c_addr[31:26] == 6'b010000;
wire is_boot = c_addr[31:12] == 20'h1FC00 && !(CPU_SIW && hwx_on && c_addr[11:6] == 6'h1F);

// the boot ROM: a read a cycle later
logic [31:0] boot [BOOT_WORDS];
initial if (BOOTROM != "") $readmemh(BOOTROM, boot);
logic [31:0] boot_q; logic boot_ack;
always_ff @(posedge clk) begin
    boot_q <= boot[c_addr[$clog2(BOOT_WORDS)+1:2]];
    boot_ack <= c_req && is_boot && !boot_ack && !rst;
end

// FAST: whose the answer is comes from registers -- the request's kind as it
// was a cycle ago (k_*: nothing answers sooner, but the SDRAM's port a write,
// which then waits that cycle unless it is the write buffer's: s_wrun)
logic k_boot, k_sd;
always_ff @(posedge clk) begin k_boot <= is_boot; k_sd <= is_sd; end
assign s_req = c_req && is_sd && (!FAST || !c_we || s_wrun || k_sd);
assign s_we = c_we; assign s_addr = c_addr; assign s_wdata = c_wdata;
assign s_strb = c_strb; assign s_line = c_line;
// (a device access waits for the last SDRAM write before it: what it sets
// off -- a DMA, a task -- may read that word)
assign x_req = c_req && !is_sd && !is_boot && !s_busy; assign x_we = c_we; assign x_addr = c_addr;
assign x_wdata = c_wdata; assign x_strb = c_strb;

always_comb begin
    if (FAST) begin
        c_ack = s_wrun ? s_ack : k_boot ? boot_ack && c_we : k_sd ? s_ack : x_ack && c_we;
        c_rvalid = k_boot ? boot_ack && !c_we : k_sd ? s_rvalid : x_ack && !c_we;
        c_rdata = k_boot ? boot_q : k_sd ? s_rdata : x_rdata;
    end else if (is_boot) begin                              // (writes: ignored, acknowledged)
        c_ack = boot_ack && c_we; c_rvalid = boot_ack && !c_we; c_rdata = boot_q;
    end else if (is_sd) begin
        c_ack = s_ack; c_rvalid = s_rvalid; c_rdata = s_rdata;
    end else begin
        c_ack = x_ack && c_we; c_rvalid = x_ack && !c_we; c_rdata = x_rdata;
    end
end

endmodule
`default_nettype wire
