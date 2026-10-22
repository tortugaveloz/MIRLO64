// Mirlo's SoC on MIPS (docs/mips.md): what core_top.sv instantiates as the
// SoC of the core. Its fabric is Mirlo-N64's (MIRLO64: rtl/soc/sdr_*,
// n64_ports, n64_video); the SDRAM's masters:
//
//   the game CPU (mips_sys: mips_core, big-endian) -----\
//   the RDP (rtl/rdp): its pixels, its commands --------+-- sdr_arb -- sdr_ctrl -- sdr_phy -- SDRAM
//   the RSP's DMA (rtl/rsp) ----------------------------|
//   the scan-out ---------------------------------------|
//   the Pocket's bridge --------------------------------/
//
//   the bridge's port (a ROM's first 32 MiB) -- psram -- psram_phy -- the two PSRAMs
//
// The map (the game CPU's device port below has the details):
//   0x0430_0000.. the RCP's MI, PI, VI, AI and SI registers, the cartridge's
//                domain 2 and ROM, PIF RAM: the N64's addresses
//   0x2000_0000  the RSP's and the DPC's registers
//   0x3000_0000  the runtime's window on the ROM
//   0x4000_0000  SDRAM, 64 MiB
//   0x8000_0000  KSEG0/KSEG1: an N64 game's RDRAM (8 MiB of SDRAM, mips_core's N64WIN)
//   0xF000_0000  the registers (mirlo_regs, tools/mirlo_regs.py)
//   0xFFFF_8000  the monitor level's memory (mon_ram)
//   0xBFC0_0000  the game CPU's boot ROM (lang/mips/boot: waits for the Pocket
//                to load the game into SDRAM, then jumps to 0x4000_0000)
// The SDRAM initialises itself; boot_ready tells the Pocket it may load.
`default_nettype none

`include "mirlo_mips_init.svh"

module mirlo_mips #(
    parameter BOOTROM = `MIPS_INIT_BOOTROM,
    parameter int SDR_T_INIT = 12600
) (
    input  wire          altera_reserved_tck,
    input  wire          altera_reserved_tdi,
    output wire          altera_reserved_tdo,
    input  wire          altera_reserved_tms,
    input  wire   [11:0] apf_audio_buffer_fill,
    output wire   [31:0] apf_audio_bus_out,
    output wire          apf_audio_bus_wr,
    output wire          apf_audio_flush,
    output wire          apf_audio_playback_en,
    output wire          apf_bridge_boot_ready,
    input  wire    [2:0] apf_bridge_command_result_code,
    input  wire          apf_bridge_complete_trigger,
    input  wire   [31:0] apf_bridge_current_address,
    output wire   [31:0] apf_bridge_data_offset,
    input  wire   [31:0] apf_bridge_file_size,
    output wire          apf_bridge_file_size_wr,
    input  wire          apf_bridge_host_loaded,
    input  wire          apf_bridge_host_reset_n,
    output wire   [31:0] apf_bridge_length,
    output wire   [31:0] apf_bridge_new_file_size_data,
    output wire   [31:0] apf_bridge_ram_data_address,
    output wire          apf_bridge_request_getfile,
    output wire          apf_bridge_request_openfile,
    output wire          apf_bridge_request_read,
    output wire          apf_bridge_request_write,
    output wire   [15:0] apf_bridge_slot_id,
    input  wire   [63:0] apf_id_chip_id,
    input  wire   [31:0] apf_input_cont1_joy,
    input  wire   [31:0] apf_input_cont1_key,
    input  wire   [31:0] apf_input_cont1_trig,
    input  wire   [31:0] apf_input_cont2_joy,
    input  wire   [31:0] apf_input_cont2_key,
    input  wire   [31:0] apf_input_cont2_trig,
    input  wire   [31:0] apf_input_cont3_joy,
    input  wire   [31:0] apf_input_cont3_key,
    input  wire   [31:0] apf_input_cont3_trig,
    input  wire   [31:0] apf_input_cont4_joy,
    input  wire   [31:0] apf_input_cont4_key,
    input  wire   [31:0] apf_input_cont4_trig,
    input  wire    [3:0] apf_interact_address,
    input  wire   [31:0] apf_interact_data,
    output logic  [31:0] apf_interact_q,
    input  wire          apf_interact_wr,
    input  wire   [31:0] apf_rtc_date_bcd,
    input  wire   [31:0] apf_rtc_time_bcd,
    input  wire   [31:0] apf_rtc_unix_seconds,
    input  wire          clk_sys,
    input  wire          clk_cpu,            // the game CPU's own: a ratio of clk_sys, from the same PLL
    input  wire          clk_sys2x,
    input  wire          clk_sys2x_90deg,
    input  wire          clk_vid,
    input  wire          reset,
    output logic  [12:0] sdram_a,
    output logic   [1:0] sdram_ba,
    output logic         sdram_cas_n,
    output logic         sdram_cke,
    output wire          sdram_clock,
    output logic   [1:0] sdram_dm,
    inout  wire   [15:0] sdram_dq,
    output logic         sdram_ras_n,
    output logic         sdram_we_n,
    // the Pocket's two PSRAMs (rtl/soc/psram.sv)
    output wire   [21:16] cram0_a, cram1_a,
    inout  wire   [15:0] cram0_dq, cram1_dq,
    output wire          cram0_clk, cram1_clk, cram0_adv_n, cram1_adv_n, cram0_cre, cram1_cre,
    output wire          cram0_ce0_n, cram0_ce1_n, cram1_ce0_n, cram1_ce1_n,
    output wire          cram0_oe_n, cram1_oe_n, cram0_we_n, cram1_we_n,
    output wire          cram0_ub_n, cram0_lb_n, cram1_ub_n, cram1_lb_n,
    input  wire          serial_rx,
    output wire          serial_tx,
    // a Game Boy cartridge in the Pocket's slot (gb_cart.sv; MIRLO_GB_CART -- without it: off)
    output wire          gb_on, gb_a_dir, gb_a_oe, gb_d_dir, gb_d_oe,
    output wire   [15:0] gb_a,
    output wire    [7:0] gb_d_o,
    input  wire    [7:0] gb_d_i,
    output wire          gb_phi, gb_wr_n, gb_rd_n, gb_cs_n,
    input  wire          use_jtag,
    output logic   [7:0] vga_b,
    output logic         vga_de,
    output logic         vga_skip,      // (the pixel is not this clock's: the Pocket's video_skip)
    output logic   [7:0] vga_g,
    output logic         vga_hsync,
    output logic   [7:0] vga_r,
    output logic         vga_vsync,
    input  wire          wishbone_ack,
    output wire   [29:0] wishbone_adr,
    output wire    [1:0] wishbone_bte,
    output wire    [2:0] wishbone_cti,
    output wire          wishbone_cyc,
    input  wire   [31:0] wishbone_dat_r,
    output wire   [31:0] wishbone_dat_w,
    input  wire          wishbone_err,
    output wire          wishbone_master_ack,
    input  wire   [29:0] wishbone_master_adr,
    input  wire    [1:0] wishbone_master_bte,
    input  wire    [2:0] wishbone_master_cti,
    input  wire          wishbone_master_cyc,
    output wire   [31:0] wishbone_master_dat_r,
    input  wire   [31:0] wishbone_master_dat_w,
    output wire          wishbone_master_err,
    input  wire    [3:0] wishbone_master_sel,
    input  wire          wishbone_master_stb,
    input  wire          wishbone_master_we,
    output wire    [3:0] wishbone_sel,
    output wire          wishbone_stb,
    output wire          wishbone_we
);

// ---------------------------------------------------------------- clocks, resets
wire clk = clk_sys;
logic rst, rst2x, rst_vid, rv1;
always_ff @(posedge clk) rst <= reset;
always_ff @(posedge clk_sys2x) rst2x <= reset;
always_ff @(posedge clk_vid) begin rv1 <= reset; rst_vid <= rv1; end

// ---------------------------------------------------------------- unused pads
assign serial_tx = 1'b1;
assign wishbone_adr = 0; assign wishbone_bte = 0; assign wishbone_cti = 0; assign wishbone_cyc = 0;
assign wishbone_dat_w = 0; assign wishbone_sel = 0; assign wishbone_stb = 0; assign wishbone_we = 0;

// ---------------------------------------------------------------- SDRAM
localparam int NP = 6;              // 0 scan-out, 1 the game CPU, 2 the RDP, 3 the RSP's DMA, 4 the bridge, 5 the RDP's commands
logic [NP-1:0]       p_valid, p_we, p_urgent, p_ready, p_rvalid;
logic [NP-1:0][23:0] p_addr;
logic [NP-1:0][31:0] p_wdata;
logic [NP-1:0][3:0]  p_be;
logic [31:0]         p_rdata;
logic        rq_valid, rq_we, rq_ready, rd_valid, sdr_ready;
logic [23:0] rq_addr;
logic [31:0] rq_wdata, rd_data;
logic [3:0]  rq_be;
logic [2:0]  rq_tag, rd_tag;
logic        la_valid;
logic [23:0] la_addr;
// The other masters' writes, for the game CPU's D-cache (mips_core's SNOOP: it drops the line of each): sn_t turns over
// when the arbiter grants the RDP, the RSP's DMA or the bridge (the PI's engine, the SI's DMA) a write, whose word is
// then in its register stage (rq_addr). Both are taken across to the CPU's clock as cpu_port's read words are, a value
// lasting one cycle of clk. While lines to drop wait in the CPU, sn_hold holds those ports' writes.
localparam logic [NP-1:0] SN_PORTS = 6'b01_1100;
logic        sn_t, sn_hold, sn_hold_s;
always_ff @(posedge clk) begin
    sn_hold_s <= !rst && sn_hold;
    if (rst) sn_t <= 1'b0; else if (|(p_ready & p_we & SN_PORTS)) sn_t <= !sn_t;
end
sdr_arb #(.N(NP)) arb (.clk, .rst, .p_valid(p_valid & ~(p_we & SN_PORTS & {NP{sn_hold_s}})), .p_we, .p_addr, .p_wdata, .p_be, .p_urgent, .p_ready, .p_rvalid, .p_rdata, .la_valid, .la_addr,
    .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag),
    .req_ready(rq_ready), .rd_valid, .rd_data, .rd_tag);
logic [1:0]  dfi_ras_n, dfi_cas_n, dfi_we_n;
logic [25:0] dfi_addr;
logic [3:0]  dfi_ba, dfi_wm;
logic        dfi_cke, dfi_wren, dfi_rden, dfi_rv;
logic [31:0] dfi_wd, dfi_rd;
// (the rows are opened ahead on phase 1 -- sdr_ctrl.sv)
localparam bit SDR_AHEAD = 1;
logic        rd_next;
logic [2:0]  rd_next_tag;
sdr_ctrl #(.T_INIT(SDR_T_INIT), .AHEAD(SDR_AHEAD)) sdram (.clk, .rst, .ready(sdr_ready),
    .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag),
    .req_ready(rq_ready), .la_valid, .la_addr, .rd_valid, .rd_data, .rd_tag, .rd_next, .rd_next_tag,
    .dfi_ras_n, .dfi_cas_n, .dfi_we_n, .dfi_addr, .dfi_ba, .dfi_cke, .dfi_wrdata_en(dfi_wren), .dfi_wrdata(dfi_wd),
    .dfi_wrdata_mask(dfi_wm), .dfi_rddata_en(dfi_rden), .dfi_rddata(dfi_rd), .dfi_rddata_valid(dfi_rv));
sdr_phy phy (.clk, .rst, .clk2x(clk_sys2x), .rst2x, .clk2x_90(clk_sys2x_90deg),
    .ras_n(dfi_ras_n), .cas_n(dfi_cas_n), .we_n(dfi_we_n), .addr(dfi_addr), .ba(dfi_ba), .cke(dfi_cke),
    .wrdata_en(dfi_wren), .wrdata(dfi_wd), .wrdata_mask(dfi_wm), .rddata_en(dfi_rden), .rddata(dfi_rd), .rddata_valid(dfi_rv),
    .sdram_a, .sdram_ba, .sdram_ras_n, .sdram_cas_n, .sdram_we_n, .sdram_cke, .sdram_dm, .sdram_dq, .sdram_clock);
assign apf_bridge_boot_ready = sdr_ready;           // the Pocket may write the slots

// ---------------------------------------------------------------- the game CPU
logic        s_req, s_we, s_line, s_ack, s_rvalid, s_busy, s_wrun;
logic [31:0] s_addr, s_wdata, s_rdata;
logic [3:0]  s_strb;
logic        x_req, x_we, x_ack;
logic [31:0] x_addr, x_wdata, x_rdata;
logic [3:0]  x_strb;
logic [31:0] dbg_cpu_pc, dbg_cause, dbg_epc;
logic        game_irq, geom_irq, geom_run;
// (with the RSP, its SP interrupt -- a task's BREAK -- is the game CPU's too)
logic        cpu_irq, mon_irq, mon_own, mon_rcp, mon_sp, mon_field, mon_dp;
// mips_core's FAST -- its longest paths cut for a faster clock -- and the
// game CPU on clk_cpu, a ratio of
// clk_sys out of the same PLL (target/pocket: 3/2 of it, 94.248 MHz), its
// memory port taken across by cpu_port (docs/mips.md)
localparam bit CPU_FAST = 1'b1;
logic        rst_cpu, irq_s, irq_mon_s, c_irq, c_irq_mon;
logic        c_x_req, c_x_ack, st_valid, st_dev, st_full, st_done;
logic [31:0] c_x_addr, c_x_rdata, st_addr, st_data;
logic [3:0]  st_strb;
always_ff @(posedge clk_cpu) rst_cpu <= reset;
always_ff @(posedge clk) begin irq_s <= cpu_irq; irq_mon_s <= mon_irq; end
mips_sys #(.BOOTROM(BOOTROM), .FAST(1'b1), .XWB(1'b1)) sys (.clk(clk_cpu), .rst(rst_cpu),
    .s_req, .s_we, .s_addr, .s_wdata, .s_strb, .s_line, .s_wrun, .s_ack(1'b0), .s_rvalid, .s_rdata, .s_busy(1'b0),
    .x_req(c_x_req), .x_we(), .x_addr(c_x_addr), .x_wdata(), .x_strb(), .x_ack(c_x_ack), .x_rdata(c_x_rdata),
    .st_valid, .st_dev, .st_addr, .st_data, .st_strb, .st_full, .st_done,
    .irq(c_irq), .irq_mon(c_irq_mon), .sn_t, .sn_line(rq_addr[23:3]), .sn_hold, .dbg_pc(dbg_cpu_pc), .dbg_cause, .dbg_epc);
cpu_port #(.PORT(1)) pc (.clk_c(clk_cpu), .rst_c(rst_cpu),
    .st_valid, .st_dev, .st_addr, .st_data, .st_strb, .st_full, .st_done,
    .s_req, .s_line, .s_addr, .s_rvalid, .s_rdata,
    .c_x_req, .c_x_addr, .c_x_ack, .c_x_rdata, .c_irq, .c_irq_mon,
    .clk, .rst,
    .p_valid(p_valid[1]), .p_we(p_we[1]), .p_addr(p_addr[1]), .p_wdata(p_wdata[1]), .p_be(p_be[1]),
    .p_ready(p_ready[1]), .p_rdata, .rd_next, .rd_next_tag,
    .x_req, .x_we, .x_addr, .x_wdata, .x_strb, .x_ack, .x_rdata, .irq(irq_s), .irq_mon(irq_mon_s));
assign s_busy = 1'b0;
// (the CPU's reads go first, with the RSP's DMA: it waits for each line it misses)
assign p_urgent[1] = 1;

// the CPU's device port: one access at a time, acknowledged for a cycle (a
// request still up in the cycle after its ack is the same one: ignored)
wire x_regs  = x_addr[31:16] == 16'hF000;
// MIRLO_RSP: the RSP (rtl/rsp) in place of the geom and audio cores. Its
// registers, as on the N64 but here: 0x2000_0000 SP_MEM_ADDR, +4 SP_DRAM_ADDR,
// +8 SP_RD_LEN, +C SP_WR_LEN, +10 SP_STATUS, +14 SP_DMA_FULL, +18 SP_DMA_BUSY,
// +1C SP_SEMAPHORE, +20 SP_PC; 0x2000_0040 DPC_START, +44 DPC_END,
// +48 DPC_CURRENT, +4C DPC_STATUS (read).
wire x_rsp   = x_addr[31:8] == 24'h2000_00;
wire x_audio = 1'b0;
wire x_tcm   = 1'b0;
wire x_grom  = 1'b0;
typedef enum logic [4:0] { X_IDLE, X_REGS, X_REGD, X_AUDIO, X_TCM, X_TCMD, X_ACK, X_RSP, X_MON, X_MOND, X_MIW1, X_MIW2, X_VIR, X_VIW, X_PIF, X_PIFR, X_CART } xst_t;
// the monitor level's memory (MIRLO_MON: mon_ram.sv): 2 KiB at 0xFFFF_8000
wire x_mon = x_addr[31:11] == 21'h1FFFF0;
logic [31:0] mon_rd;
xst_t xs;
// MIRLO_MI: the MI's block (0x0430_0000: its registers repeat through the 1 MiB) and, MIRLO_PI, PI_STATUS (0x0460_0010) --
// physical, or KSEG0 as the core's address leaves it (rcp_mi.sv); the bridge's addresses are not these (0x4xxx_xxxx and up)
wire x_mi = !x_addr[30] && x_addr[28:20] == 9'h043;
wire x_pi = !x_addr[30] && x_addr[28:2] == 27'h1180004;
// MIRLO_VI: the VI's block (0x0440_0000, rcp_vi.sv), a block RAM's registers: a read waits a cycle, a write is done in one
// and ends as the MI's (two cycles for its effect on the interrupt's line)
wire x_vi = !x_addr[30] && x_addr[28:20] == 9'h044;
// MIRLO_AI: the AI's block (0x0450_0000, rcp_ai.sv): registers in flip-flops, answered as the MI's
wire x_ai = !x_addr[30] && x_addr[28:20] == 9'h045;
// MIRLO_SI: the SI's registers (0x0480_0000, rcp_si.sv: answered as the MI's) and PIF RAM (0x1FC0_07C0..07FF, KSEG0 or KSEG1: the
// compare of mips_core's SIW), a block RAM: a read waits a cycle, and both wait while the SI's DMA owns it
wire x_si  = !x_addr[30] && x_addr[28:20] == 9'h048;
wire x_pif = !x_addr[30] && x_addr[28:20] == 9'h1FC && x_addr[10:6] == 5'h1F;
// MIRLO_PI2: the cartridge's bus as the game CPU's lw / sw reach it (mips_core's PI2W): domain 2, 0x0800_0000..0x0FFF_FFFF (the
// save: SRAM or FlashRAM; the core leaves its command register, 0x0801_xxxx, to the runtime), and lw of the ROM,
// 0x1000_0000..0x13FF_FFFF -- KSEG0 or KSEG1, as the MI's. The PI's engine does the access (rcp_mi.sv: between copies): a ROM
// offset o below 32 MiB at the PSRAM's word ((1 MiB + o) / 4) mod 8 M (io_ps: the word the bridge wrote it to), from 32 MiB at
// the SDRAM's 1 MiB + o - 32 MiB; domain 2's at the save's memory (0x0332_0000 + the offset's 17 bits) -- or, with
// MONITOR.PI2's FlashRAM bit, a read is the status word the runtime keeps at 0x0334_0100 and a write goes nowhere (a command is
// the runtime's: it traps)
wire x_d2  = !x_addr[30] && x_addr[28:27] == 2'b01;
wire x_rom = !x_addr[30] && x_addr[28:26] == 3'b100;
// The runtime's window on the ROM, 0x3000_0000..0x33FF_FFFF (physical, uncached: its byte o is the ROM's, the Pocket's file's at
// 1 MiB + o): as the cartridge's reads; a word written to the PSRAM's part is written (a check of the chips)
wire x_psw = x_addr[31:26] == 6'b001100;
wire io_ps = !x_addr[25];
logic        d2_flash, io_ack;
logic [31:0] pa_dat_r /*verilator public_flat_rd*/;      // (the bridge's port's read data: below)
wire  [23:0] io_adr = (x_rom || x_psw) ? {x_addr[25:20] + (x_addr[25] ? 6'd33 : 6'd1), x_addr[19:2]} : d2_flash ? 24'hCD_0040 : {9'h199, x_addr[16:2]};
logic [31:0] mi_w_rd;
logic        mi_line, mon_mi;
logic        vi_line, vi_latch, vi_irq, vi_chg, vi_clr, vi_blk, vi_gm, mon_vi;
logic        vi_aa, vi_dv, vi_dd;
wire  [4:0]  vid_crop_v;           // the core's video options (interact.json): crop lines at the top and bottom,
wire  [1:0]  vid_crop_r;           // and pixels at the right (n64_video.sv)
logic [25:0] vi_base;
logic        vi_il;
logic [31:0] vi_rd, ai_rd;
logic        mon_ai, ai_clr, ai_chg, ai_req;
logic        ai_irq /*verilator public_flat_rd*/, ai_fw /*verilator public_flat_rd*/, ai_step_we /*verilator public_flat_rd*/, ai_ack /*verilator public_flat_rd*/;   // (the bench's AILOG)
logic [13:0] ai_dac;
logic [19:0] ai_step_wd /*verilator public_flat_rd*/;
logic [31:0] ai_fd, aud_out_r;
logic        aud_wr_r;
logic [23:0] ai_adr;
logic [16:0] vi_thr;

wire         mi_op = xs == X_IDLE && x_req && !x_ack && (x_mi || x_pi);
wire         ai_op = xs == X_IDLE && x_req && !x_ack && x_ai;
wire         si_op = xs == X_IDLE && x_req && !x_ack && x_si;
logic [31:0] si_rd, si_q, si_wdata;
logic        si_irq, si_clr, si_ev, si_done, si_busy, si_req, si_we, si_ack;
logic [23:0] si_adr;
logic        d_req, d_we, d_ack, d_ps;
logic        ps_ready;                                   // (the PSRAMs are set up: below)
logic [23:0] d_adr;
logic [3:0]  d_sel;
logic [31:0] d_wdata;
mon_ram monram (.clk, .we(xs == X_MON && x_we), .a(x_addr[10:2]), .wd(x_wdata), .be(x_strb), .rd(mon_rd));
wire geom_load = xs == X_IDLE && x_req && !x_ack && x_grom && x_we && x_strb == 4'hF;
logic        xr_go;                                  // the CPU's register access, this cycle
logic        bus_r_req;
logic [15:0] bus_errors;
logic [31:0] r_rdata, a_s_dat_r, t_dat_r;
logic        a_s_ack;
// the RSP's registers: the access is taken into registers first (the CPU's
// store data is late in its cycle, and a write of a DMA length goes through a
// multiplier), then done -- a write waits while the microcode writes a register itself
logic [31:0] rsp_x_rdata, rx_wdata;
logic [4:0]  rx_addr;                                // x_addr[6:2]
logic        rx_we;
logic        rsp_h_ready;
wire         rsp_x_go = xs == X_RSP && (!rx_we || rsp_h_ready);
always_ff @(posedge clk) begin
    x_ack <= 0;
    if (rst) begin xs <= X_IDLE; bus_errors <= 0; end
    else case (xs)
    X_IDLE: if (x_req && !x_ack) begin
        if (x_regs) xs <= X_REGS;
        else if (x_audio) xs <= X_AUDIO;
        else if (x_tcm) xs <= X_TCM;
        else if (x_mon) xs <= X_MON;
        else if (x_mi || x_pi) begin x_rdata <= mi_w_rd; if (x_we) xs <= X_MIW1; else x_ack <= 1; end
        else if (x_vi) xs <= x_we ? X_VIW : X_VIR;
        else if (x_ai) begin x_rdata <= ai_rd; if (x_we) xs <= X_MIW1; else x_ack <= 1; end
        else if (x_si) begin x_rdata <= si_rd; if (x_we) xs <= X_MIW1; else x_ack <= 1; end
        else if (x_pif) xs <= X_PIF;
        else if (x_d2 || x_rom || x_psw) begin if (x_we && (x_rom || d2_flash || (x_psw && !io_ps))) begin x_rdata <= 0; x_ack <= 1; end else xs <= X_CART; end
        else if (x_grom) begin x_rdata <= 0; x_ack <= 1; end
        else if (x_rsp) begin rx_wdata <= x_wdata; rx_addr <= x_addr[6:2]; rx_we <= x_we; xs <= X_RSP; end
        else begin x_rdata <= 0; x_ack <= 1; if (bus_errors != 16'hFFFF) bus_errors <= bus_errors + 16'd1; end
    end
    X_REGS: if (!bus_r_req) xs <= X_REGD;            // (the geom and audio cores' accesses go first)
    X_REGD: begin x_rdata <= r_rdata; x_ack <= 1; xs <= X_IDLE; end
    X_AUDIO: if (a_s_ack) begin x_rdata <= a_s_dat_r; x_ack <= 1; xs <= X_IDLE; end
    X_MON:  xs <= X_MOND;
    X_MOND: begin x_rdata <= mon_rd; x_ack <= 1; xs <= X_IDLE; end
    // (a store to the MI waits two cycles for its effect on the line to be in the CPU's clock: rcp_mi.sv)
    X_VIR: begin x_rdata <= vi_rd; x_ack <= 1; xs <= X_IDLE; end
    X_PIF:  if (!si_busy) xs <= x_we ? X_MIW2 : X_PIFR;          // (the write is done in this cycle: rcp_si.sv's c_wr_s)
    X_PIFR: begin x_rdata <= si_q; x_ack <= 1; xs <= X_IDLE; end
    X_CART: if (io_ack) begin x_rdata <= pa_dat_r; x_ack <= 1; xs <= X_IDLE; end
    X_VIW: xs <= X_MIW1;                            // (the write is done in this cycle: rcp_vi.sv's wr_s)
    X_MIW1: xs <= X_MIW2;
    X_MIW2: begin x_ack <= 1; xs <= X_IDLE; end
    X_TCM:  xs <= X_TCMD;
    X_TCMD: begin x_rdata <= t_dat_r; x_ack <= 1; xs <= X_IDLE; end
    X_RSP:  if (rsp_x_go) begin x_rdata <= rsp_x_rdata; x_ack <= 1; xs <= X_IDLE; end
    default: xs <= X_IDLE;
    endcase
end
assign xr_go = xs == X_REGS && !bus_r_req;

// ---------------------------------------------------------------- registers, bus
logic        br_we;
logic [15:0] br_addr, stray;
logic [31:0] br_wdata;
logic        su_valid, su_ready;
logic [31:0] su_data;
logic        cmd_valid, cmd_ready;
logic [31:0] cmd_data, mrdp_sync, mrdp_load;
logic [15:0] mrdp_unknown;
logic        mrdp_idle;
logic        r_uart_we, uart_rx_pop, uart_rxempty, uart_txfull;
logic [7:0]  r_uart_byte, uart_rx_byte;
logic [31:0] fb_base_csr, dbg_geom_msg;
logic [11:0] fb_width;
logic [7:0]  fb_y0;
logic [8:0]  fb_rows;
logic [9:0]  fb_px0, fb_pw;
logic [10:0] fb_xs, fb_nw;
logic [11:0] fb_ys;                 // (2.10, up to 2.0: a serrated picture's field steps two rows)
logic        fb_gamma, fb_tall, fb_pal, tv_pal, vi_pal_rq;
logic        fb_dma_enable, fb_vtg_enable, fb_mode, vblank;
logic        gbc_on, gbc_trust, gbc_start, gbc_we, gbc_busy;
logic [15:0] gbc_addr;
logic [7:0]  gbc_wdata, gbc_rdata;
mirlo_regs regs (.clk, .rst,
    .r_req(bus_r_req || xr_go), .r_we(bus_r_req ? br_we : x_we), .r_addr(bus_r_req ? br_addr : x_addr[15:0]),
    .r_wdata(bus_r_req ? br_wdata : x_wdata), .r_rdata,
    .aud_out(aud_out_r), .aud_wr(aud_wr_r), .aud_playback_en(apf_audio_playback_en),
    .aud_flush(apf_audio_flush), .aud_fill(apf_audio_buffer_fill),
    .br_request_read(apf_bridge_request_read), .br_request_write(apf_bridge_request_write),
    .br_request_getfile(apf_bridge_request_getfile), .br_request_openfile(apf_bridge_request_openfile),
    .br_slot_id(apf_bridge_slot_id), .br_data_offset(apf_bridge_data_offset), .br_length(apf_bridge_length),
    .br_ram_data_address(apf_bridge_ram_data_address), .br_file_size_wr(apf_bridge_file_size_wr),
    .br_new_file_size(apf_bridge_new_file_size_data), .br_file_size(apf_bridge_file_size),
    .br_complete_trigger(apf_bridge_complete_trigger), .br_current_address(apf_bridge_current_address),
    .br_command_result_code(apf_bridge_command_result_code),
    .br_host_reset_n(apf_bridge_host_reset_n), .br_host_loaded(apf_bridge_host_loaded),
    .cont_key('{apf_input_cont4_key, apf_input_cont3_key, apf_input_cont2_key, apf_input_cont1_key}),
    .cont_joy('{apf_input_cont4_joy, apf_input_cont3_joy, apf_input_cont2_joy, apf_input_cont1_joy}),
    .cont_trig('{apf_input_cont4_trig, apf_input_cont3_trig, apf_input_cont2_trig, apf_input_cont1_trig}),
    .ia_address(apf_interact_address), .ia_data(apf_interact_data), .ia_wr(apf_interact_wr), .ia_q(apf_interact_q),
    .ia_crop_v(vid_crop_v), .ia_crop_r(vid_crop_r),
    .vblank, .fb_base(fb_base_csr), .fb_width, .fb_y0, .fb_rows, .fb_px0, .fb_pw, .fb_xs, .fb_ys, .fb_nw, .fb_tall, .fb_gamma, .fb_pal, .tv_pal, .fb_dma_enable, .fb_vtg_enable, .fb_mode,
    .bus_errors, .geom_irq, .game_irq, .geom_run, .dbg_geom_msg,
    .su_valid, .su_ready, .su_data, .cmd_valid, .cmd_ready, .cmd_data,
    .mrdp_sync_count(mrdp_sync), .mrdp_load_count(mrdp_load), .mrdp_unknown_ops(mrdp_unknown), .mrdp_idle,
    .uart_we(r_uart_we), .uart_byte(r_uart_byte), .uart_txfull, .uart_rx_pop, .uart_rx_byte, .uart_rxempty,
    .mon_sp, .mon_field, .mon_dp, .mon_own, .mon_rcp, .mon_irq,
    .mon_mi, .mi_line, .mi_w_mi(mi_op && x_mi), .mi_w_pi(mi_op && x_pi), .mi_w_we(x_we), .mi_w_a(x_addr[3:2]), .mi_w_wd(x_wdata), .mi_w_rd,
    .d_req, .d_we, .d_adr, .d_ps, .d_sel, .d_wdata, .d_rdata(pa_dat_r), .d_ack,
    .io_req(xs == X_CART), .io_we(x_we), .io_adr, .io_ps(io_ps && (x_rom || x_psw)), .io_wd(x_wdata), .io_ack, .d2_flash, .ps_ready,
    .mon_vi, .vi_thr, .vi_irq, .vi_chg, .vi_clr,
    .mon_ai, .ai_irq, .ai_clr, .ai_chg, .ai_dac, .ai_step_we, .ai_step_wd,
    .si_irq, .si_clr, .si_ev, .si_done,
    .gbc_on, .gbc_trust, .gbc_start, .gbc_we, .gbc_addr, .gbc_wdata, .gbc_busy, .gbc_rdata);

// ---------------------------------------------------------------- the Game Boy cartridge slot
gb_cart gbcart (.clk, .rst, .on(gbc_on), .trust(gbc_trust), .start(gbc_start), .we(gbc_we), .addr(gbc_addr),
    .wdata(gbc_wdata), .busy(gbc_busy), .rdata(gbc_rdata),
    .gb_on, .gb_a_dir, .gb_a_oe, .gb_a, .gb_d_o, .gb_d_dir, .gb_d_oe, .gb_d_i, .gb_phi, .gb_wr_n, .gb_rd_n, .gb_cs_n);

// ---------------------------------------------------------------- the JTAG UART
`ifdef MIRLO_NO_JTAG_UART
assign uart_txfull = 1'b0;
assign uart_rxempty = 1'b1;
assign uart_rx_byte = 8'd0;
// the JTAG pins still end in the hard JTAG block, idle: without an atom on
// them Quartus places altera_reserved_* as four more user I/O (228 > 224)
`ifndef VERILATOR
cyclonev_jtag jtag_idle (.tck(altera_reserved_tck), .tdi(altera_reserved_tdi), .tms(altera_reserved_tms),
    .tdo(altera_reserved_tdo), .tdouser(1'b0), .clkdruser(), .runidleuser(), .shiftuser(), .tckutap(),
    .tdiutap(), .tmsutap(), .updateuser(), .usr1user());
`endif
`else
n64_uart uart (.clk, .rst,
    .tck(altera_reserved_tck), .tdi(altera_reserved_tdi), .tms(altera_reserved_tms), .tdo(altera_reserved_tdo),
    .r_we(r_uart_we), .r_byte(r_uart_byte), .w_we(1'b0), .w_byte(8'd0), .txfull(uart_txfull),
    .rx_pop(uart_rx_pop), .rx_byte(uart_rx_byte), .rxempty(uart_rxempty), .dump_en(1'b0), .dbg('0));
`endif

// ---------------------------------------------------------------- the RSP
// The game's own microcode runs here (the game CPU loads a task as libultra
// does: the header and the boot microcode by DMA, the PC, halt cleared). Its
// DMA is a port of the SDRAM arbiter: the N64's RDRAM is SDRAM 0x4380_0000.
logic        rsp_r_valid, rsp_r_we, rsp_halted, rsp_unimpl, rsp_irq;
logic [23:2] rsp_r_addr;
logic [31:0] rsp_r_wdata, rsp_h_rdata;
logic        dpc_start_we, dpc_end_we, dpc_status_we;      // the writes that reach rdp_dpc: the microcode's (rsp_*) and the CPU's (dpc_h*)
logic        rsp_dpc_start_we, rsp_dpc_end_we, rsp_dpc_status_we;
logic [31:0] dpc_wdata, rsp_dpc_wdata, dpc_status, dpc_st_raw;
logic [23:0] dpc_start, dpc_end, dpc_end_rd, dpc_current;
logic        dpc_start_ok;
// (MIRLO_RSP_LANES: 8, or 4 -- rsp.md)
`ifndef MIRLO_RSP_LANES
`define MIRLO_RSP_LANES 8
`endif
// (MIRLO_RSP_MULW, with 4 lanes: the multiplies on eight)
localparam bit RSP_MULW = 1'b1;
rsp_top #(.LANES(`MIRLO_RSP_LANES), .MULW(RSP_MULW), .DIV_ROM(`MIPS_INIT_RSP_DIV)) rsp (.clk, .rst,
    .h_we(rsp_x_go && rx_we && !rx_addr[4]), .h_rd(rsp_x_go && !rx_we && !rx_addr[4]), .h_addr(rx_addr[3:0]), .h_wdata(rx_wdata), .h_rdata(rsp_h_rdata),
    .h_ready(rsp_h_ready), .irq(rsp_irq),
    .r_valid(rsp_r_valid), .r_we(rsp_r_we), .r_addr(rsp_r_addr), .r_wdata(rsp_r_wdata),
    .r_ready(p_ready[3]), .r_rvalid(p_rvalid[3]), .r_rdata(p_rdata),
    .dpc_start_we(rsp_dpc_start_we), .dpc_end_we(rsp_dpc_end_we), .dpc_status_we(rsp_dpc_status_we), .dpc_wdata(rsp_dpc_wdata),
    .dpc_start({8'd0, dpc_start}), .dpc_end({8'd0, dpc_end_rd}), .dpc_current({8'd0, dpc_current}), .dpc_status,
    .dx_addr(dpc_dx_addr), .dx_data(dpc_dx_data),
    .halted(rsp_halted), .unimpl(rsp_unimpl),
    .tr_e(), .tr_e_pc(), .tr_e2(), .tr_e_we(), .tr_e_wr(), .tr_we(), .tr_wr(), .tr_val(), .tr_v(), .tr_vp());
assign cpu_irq     = mon_own ? (mon_mi ? mi_line : mon_rcp) : (game_irq | rsp_irq);       // (the monitor's: the game's own IP2 is the monitor's line)
assign mon_sp      = rsp_irq;
assign p_valid[3]  = rsp_r_valid;
assign p_we[3]     = rsp_r_we;
assign p_addr[3]   = 24'hE0_0000 + {3'd0, rsp_r_addr[22:2]};  // 8 MiB of RDRAM at 0x4380_0000
assign p_wdata[3]  = rsp_r_wdata;
assign p_be[3]     = 4'hF;
// (the RSP's DMA goes first, with the CPU: the microcode waits for each transfer it starts -- a display list's next
// commands, the vertices, the RDP's commands out -- while the RDP and the scan-out ride out a wait in their buffers)
assign p_urgent[3] = 1;
// the RDP's registers and its command fetch: the commands the microcode
// leaves in RDRAM, into the RDP's command FIFO (the port the geom core pushed its own through)
logic [20:0] dpc_m_addr;
logic [9:0]  dpc_dx_addr;
logic [31:0] dpc_dx_data;
// The CPU's writes to DPC_START / DPC_END / DPC_STATUS (the monitor forwards them to 0x2000_0040..4C): a write is taken in the
// cycle the microcode is not writing a register itself (rsp_h_ready), so the two never meet. A START while START is valid is
// ignored; the status is the console's layout (rcp_dpc_view.sv).
wire         dpc_h_w = rsp_x_go && rx_we && rx_addr[4];
assign dpc_start_we  = (rsp_dpc_start_we || (dpc_h_w && rx_addr[1:0] == 2'd0)) && dpc_start_ok;
assign dpc_end_we    = rsp_dpc_end_we    || (dpc_h_w && rx_addr[1:0] == 2'd1);
assign dpc_status_we = rsp_dpc_status_we || (dpc_h_w && rx_addr[1:0] == 2'd3);
assign dpc_wdata     = dpc_h_w ? rx_wdata : rsp_dpc_wdata;
rcp_dpc_view dpc_view (.clk, .rst, .end_we(dpc_end_we), .status_we(dpc_status_we), .wdata(dpc_wdata), .sync_full(mon_dp),
    .st_raw(dpc_st_raw), .cur(dpc_current), .end_(dpc_end), .start_ok(dpc_start_ok), .st(dpc_status));
rdp_dpc dpc (.clk, .rst, .start_we(dpc_start_we), .end_we(dpc_end_we), .status_we(dpc_status_we), .wdata(dpc_wdata),
    .start(dpc_start), .end_(dpc_end), .end_rd(dpc_end_rd), .current(dpc_current), .status(dpc_st_raw),
    .rdp_busy(cmd_valid || !mrdp_idle),
    .m_valid(p_valid[5]), .m_addr(dpc_m_addr), .m_ready(p_ready[5]), .m_rvalid(p_rvalid[5]), .m_rdata(p_rdata),
    .dx_addr(dpc_dx_addr), .dx_data(dpc_dx_data),
    .c_valid(su_valid), .c_data(su_data), .c_ready(su_ready));
assign p_we[5]     = 1'b0;
assign p_addr[5]   = 24'hE0_0000 + {3'd0, dpc_m_addr};
assign p_wdata[5]  = '0;
assign p_be[5]     = 4'hF;
assign p_urgent[5] = 0;
always_comb
    if (!rx_addr[4]) rsp_x_rdata = rsp_h_rdata;
    else unique case (rx_addr[1:0])
        2'd0: rsp_x_rdata = {8'd0, dpc_start};
        2'd1: rsp_x_rdata = {8'd0, dpc_end_rd};
        2'd2: rsp_x_rdata = {8'd0, dpc_current};
        default: rsp_x_rdata = dpc_status;
    endcase
// what the geom and audio cores drove
assign bus_r_req = 1'b0; assign br_we = 1'b0; assign br_addr = '0; assign br_wdata = '0; assign stray = '0;
assign a_s_ack = 1'b0; assign a_s_dat_r = '0; assign t_dat_r = '0;
logic [31:0] dbg_geom_pc, dbg_aud_state, dbg_aud_pc;
assign dbg_geom_pc = '0; assign dbg_aud_state = {30'd0, rsp_unimpl, rsp_halted}; assign dbg_aud_pc = '0;

// ---------------------------------------------------------------- MRDP
logic        m_cmd_valid, m_cmd_ready, m_cmd_we, m_wdata_valid, m_wdata_ready, m_rdata_valid;
logic [31:0] m_cmd_addr, m_wdata, m_rdata;
logic [3:0]  m_wdata_we;
// the RDP for the N64's command stream (rtl/rdp): its addresses are the
// N64's, in the 8 MiB of RDRAM at SDRAM 0x4380_0000
logic [31:0] rdp_addr;
rdp_top rdp (.clk, .rst,
    .cmd_valid, .cmd_ready, .cmd_data,
    .m_cmd_valid, .m_cmd_ready, .m_cmd_we, .m_cmd_addr(rdp_addr),
    .m_wdata_valid, .m_wdata_ready, .m_wdata, .m_wdata_we,
    .m_rdata_valid, .m_rdata,
    .sync_count(mrdp_sync), .sync_ev(mon_dp), .load_count(mrdp_load), .unknown_ops(mrdp_unknown), .idle(mrdp_idle));
// (bit 23: the RDP's hidden-bit plane, 1 MiB below the RDRAM -- lang/c/mrdp/mrdp.h, layout.h)
assign m_cmd_addr = {6'd0, rdp_addr[23] ? 24'hDC_0000 + {6'd0, rdp_addr[19:2]} : 24'hE0_0000 + {3'd0, rdp_addr[22:2]}, 2'b00};
port_mrdp pm (.clk, .rst, .m_cmd_valid, .m_cmd_we, .m_cmd_addr, .m_cmd_ready, .m_wdata_valid, .m_wdata, .m_wdata_we,
    .m_wdata_ready, .m_rdata_valid, .m_rdata,
    .p_valid(p_valid[2]), .p_we(p_we[2]), .p_addr(p_addr[2]), .p_wdata(p_wdata[2]), .p_be(p_be[2]),
    .p_ready(p_ready[2]), .p_rvalid(p_rvalid[2]), .p_rdata);
assign p_urgent[2] = 0;

// ---------------------------------------------------------------- the Pocket's bridge
// Its addresses (apf_wishbone_master's words; 0 is the SDRAM's first), as
// docs/mips.md has them ("The memory of an N64 game"), by their megabyte. A
// write:
//   below 1 MiB         to the SDRAM, at that address: the program
//   1 MiB to 33 MiB     to the PSRAM, at the address mod 32 MiB: the ROM's
//                       first 32 MiB (the die's word: the address's word mod
//                       8 M, as the PI's engine and the CPU's cartridge reads
//                       have it -- no adder)
//   33 MiB to 65 MiB    to the SDRAM at the address less 32 MiB: the rest of
//                       a ROM of up to 64 MiB, at 1 MiB to 33 MiB
//   anything else       to the SDRAM, at the address's low 26 bits
// A read is always the SDRAM's, at the address's low 26 bits: the Pocket
// reads back the nonvolatile slots, and nothing of a ROM.
// So no byte of a ROM reaches the SDRAM from 33 MiB on, where the
// accessories' and the saves' slots are: data.json has them at their SDRAM
// address with bit 26 set (113 MiB up), and the Pocket reads a nonvolatile
// one back from there when the core quits.
// What is written from 56 MiB to 64 MiB, the RDRAM's place, goes to the SDRAM
// at that address as well, first. A ROM's bytes do no harm in the RDRAM
// before the game starts, and the save slots of a data.json older than this
// map (they were at the RDRAM's top) are read back as they were loaded --
// with any game that leaves that memory alone.
logic        pa_ack, pa_done;
wire  [6:0]  br_mib = wishbone_master_adr[24:18];
wire         br_lo  = br_mib != 7'd0 && (br_mib[6:5] == 2'd0 || br_mib == 7'd32);       // 1 to 32: the PSRAM's
wire         br_hi  = br_mib[6:5] == 2'd1 ? br_mib != 7'd32 : br_mib == 7'd64;            // 33 to 64
wire         br_rd  = br_mib[6:3] == 4'b0111;                                       // 56 to 63: the SDRAM's own write first
wire         br_two = wishbone_master_we && br_hi && br_rd;
wire         br_mv  = wishbone_master_we && br_hi && (!br_rd || pa_done);          // to the SDRAM less 32 MiB
wire  [6:0]  br_mm  = br_mib - 7'd32;
wire  [29:0] br_adr = br_mv ? {wishbone_master_adr[29:24], br_mm[5:0], wishbone_master_adr[17:0]} : wishbone_master_adr;
// MIRLO_PI: the PI's engine (rcp_mi.sv) is a second master of this port, while the bridge is idle (pa_dma: its access)
logic        pa_dma;
always_ff @(posedge clk) begin
    if (rst) pa_dma <= 1'b0;
    else if (pa_dma) begin if (pa_ack) pa_dma <= 1'b0; end
    else if ((d_req || ai_req || si_req) && !wishbone_master_cyc) pa_dma <= 1'b1;
end
// MIRLO_AI: the AI's reader (rcp_ai.sv) is a third: a read, first when both ask (it is rare and must not wait for a copy),
// but not between the engine's read and its write of a word (pa_dat_r)
// MIRLO_SI: the SI's DMA (rcp_si.sv) is a fourth, after the AI: it moves a word between the SDRAM and the PIF RAM, a write from the
// PIF RAM's own output (no data held in pa_dat_r), a read into the PIF RAM in the cycle of the ack -- but, as the AI's, never
// between the PI engine's read and write of a word
logic        pa_ai, pa_si;             // the access under way is the AI's / the SI's
always_ff @(posedge clk) begin
    if (rst) begin pa_ai <= 1'b0; pa_si <= 1'b0; end
    else if (!pa_dma) begin                           // (the PI's engine holds its word in pa_dat_r from its read to its write: no AI read between)
        pa_ai <= ai_req && !d_we;
        pa_si <= !ai_req && si_req && !d_we;
    end
end
assign d_ack  = pa_dma && pa_ack && !pa_ai && !pa_si;
assign ai_ack = pa_dma && pa_ack && pa_ai;
assign si_ack = pa_dma && pa_ack && pa_si;
wire         pd_req = pa_si ? si_req : pa_ai ? ai_req : d_req;
wire  [23:0] pd_adr = pa_si ? si_adr : pa_ai ? ai_adr : d_adr;
// The port is the PSRAM's only master (x_*): the bridge's writes from 1 MiB to 33 MiB, and the engine's reads of a ROM's first
// 32 MiB (its copies, the game CPU's words)
logic        pa_x_valid, pa_x_ack;
logic [31:0] ps_x_rdata;
wire         pa_x_sel = pa_dma ? !pa_ai && !pa_si && d_ps : wishbone_master_we && br_lo;
port_apf pa (.clk, .rst, .cyc(pa_dma ? pd_req : wishbone_master_cyc), .stb(pa_dma || wishbone_master_stb), .we(pa_dma ? (pa_si ? si_we : d_we && !pa_ai) : wishbone_master_we),
    .adr(pa_dma ? {6'd0, pd_adr} : br_adr), .dat_w(pa_dma ? (pa_si ? si_wdata : d_wdata) : wishbone_master_dat_w), .sel(pa_dma ? d_sel : wishbone_master_sel),
    .ack(pa_ack), .dat_r(pa_dat_r),
    .x_sel(pa_x_sel), .x_valid(pa_x_valid), .x_ack(pa_x_ack), .x_rdata(ps_x_rdata),
    .p_valid(p_valid[4]), .p_we(p_we[4]), .p_addr(p_addr[4]), .p_wdata(p_wdata[4]), .p_be(p_be[4]),
    .p_ready(p_ready[4]), .p_rvalid(p_rvalid[4]), .p_rdata);
always_ff @(posedge clk) begin
    if (!wishbone_master_cyc) pa_done <= 1'b0;          // (a write to both: the SDRAM's own first)
    else if (pa_ack && !pa_dma && br_two) pa_done <= 1'b1;
end
assign wishbone_master_ack   = pa_ack && !pa_dma && (!br_two || pa_done);
assign wishbone_master_dat_r = pa_dat_r;
assign wishbone_master_err = 1'b0;
assign p_urgent[4] = 0;

// ---------------------------------------------------------------- the PSRAMs
// a word at a time, the bridge's port's accesses: it holds one until its ack; the word is the port's SDRAM word's 23 low bits
logic [5:0]  ps_o_a;
logic [31:0] ps_o_dq, ps_i_dq;
logic        ps_o_dq_oe, ps_o_adv_n, ps_o_oe_n, ps_o_we_n, ps_o_cre;
logic [1:0]  ps_o_ce_n;
logic [3:0]  ps_o_bm_n;
// (the chips' 150 us are the SDRAM's own wait, 200 us)
psram psram (.clk, .rst, .wake(sdr_ready),
    .x_valid(pa_x_valid), .x_we(p_we[4]), .x_addr(p_addr[4][22:0]), .x_wdata(p_wdata[4]), .x_be(p_be[4]),
    .x_ack(pa_x_ack), .x_rdata(ps_x_rdata), .ready(ps_ready),
    .o_a(ps_o_a), .o_dq(ps_o_dq), .o_dq_oe(ps_o_dq_oe), .o_adv_n(ps_o_adv_n), .o_oe_n(ps_o_oe_n), .o_we_n(ps_o_we_n),
    .o_cre(ps_o_cre), .o_ce_n(ps_o_ce_n), .o_bm_n(ps_o_bm_n), .i_dq(ps_i_dq));
psram_phy psphy (.clk, .o_a(ps_o_a), .o_dq(ps_o_dq), .o_dq_oe(ps_o_dq_oe), .o_adv_n(ps_o_adv_n), .o_oe_n(ps_o_oe_n),
    .o_we_n(ps_o_we_n), .o_cre(ps_o_cre), .o_ce_n(ps_o_ce_n), .o_bm_n(ps_o_bm_n), .i_dq(ps_i_dq),
    .cram0_a, .cram1_a, .cram0_dq, .cram1_dq, .cram0_clk, .cram1_clk,
    .cram0_adv_n, .cram1_adv_n, .cram0_cre, .cram1_cre, .cram0_ce0_n, .cram0_ce1_n, .cram1_ce0_n, .cram1_ce1_n,
    .cram0_oe_n, .cram1_oe_n, .cram0_we_n, .cram1_we_n, .cram0_ub_n, .cram0_lb_n, .cram1_ub_n, .cram1_lb_n);

// ---------------------------------------------------------------- the VI (MIRLO_VI: rcp_vi.sv)
// With MONITOR.CTL's VI bit the scan-out's base, gamma and black are the VI registers'; else the CSRs'
rcp_vi vi (.clk, .rst, .sc_line(vi_line), .sc_latch(vi_latch), .tv_pal, .tv_def(fb_pal), .vi_pal(vi_pal_rq), .thr(vi_thr),
    .a(x_addr[5:2]), .wd(x_wdata), .wr_s(xs == X_VIW), .rd(vi_rd), .vi_clr,
    .irq(vi_irq), .chg(vi_chg), .base(vi_base), .il(vi_il), .black(vi_blk), .gamma(vi_gm), .aa(vi_aa), .divot(vi_dv), .dedither(vi_dd));
// ---------------------------------------------------------------- the AI (MIRLO_AI: rcp_ai.sv)
// The Pocket's audio FIFO has two writers: the CSR's (the runtime's software AI, a program's) and the AI's
rcp_ai ai (.clk, .rst, .a(x_addr[4:2]), .wd(x_wdata), .wr_s(ai_op && x_we), .rd(ai_rd), .clr(ai_clr), .irq(ai_irq), .chg(ai_chg),
    .step_we(ai_step_we), .step_wd(ai_step_wd), .dac(ai_dac), .fill(apf_audio_buffer_fill[9:0]), .fw(ai_fw), .fd(ai_fd),
    .req(ai_req), .adr(ai_adr), .ack(ai_ack), .rdata(pa_dat_r));
// ---------------------------------------------------------------- the SI (MIRLO_SI: rcp_si.sv)
rcp_si si (.clk, .rst, .a(x_addr[4:2]), .wd(x_wdata), .wr_s(si_op && x_we), .rd(si_rd), .clr(si_clr), .irq(si_irq),
    .ca(x_addr[5:2]), .c_wr_s(xs == X_PIF && x_we), .c_wd(x_wdata), .c_q(si_q), .c_busy(si_busy),
    .done_we(si_done), .done_wd(ai_step_wd[17:0]), .ev(si_ev),          // (the registers' write data, as the AI's step: SIDONE's)
    .req(si_req), .we(si_we), .adr(si_adr), .wdata(si_wdata), .ack(si_ack), .rdata(pa_dat_r));
assign apf_audio_bus_wr  = aud_wr_r || ai_fw;
assign apf_audio_bus_out = ai_fw ? ai_fd : aud_out_r;

wire [31:0] fb_base = mon_vi ? {6'd0, vi_base} : fb_base_csr;
wire        fb_en   = fb_dma_enable && fb_vtg_enable && !(mon_vi && vi_blk);
wire        vi_gam  = mon_vi ? vi_gm : fb_gamma;

// ---------------------------------------------------------------- the scan-out
localparam bit VIDEO_N64FB = 1'b1;  // the framebuffer is the game's: RGBA5551, the left pixel in the high half
n64_video #(.ORIGIN_MASK(24'hFFFFFF), .N64FB(VIDEO_N64FB)) video (.clk, .rst, .clk_vid, .rst_vid,
    .enable(fb_en), .vi_origin(fb_base[25:0]), .vi_il(mon_vi && vi_il), .vblank,
    .vi_width(fb_width), .vi_black(1'b0), .vi_pal(mon_vi ? vi_pal_rq : fb_pal), .tv_pal, .gamma(vi_gam), .vi_aa(mon_vi && vi_aa), .vi_divot(mon_vi && vi_dv), .vi_dedither(mon_vi && vi_dd), .crop_v(vid_crop_v), .crop_r(vid_crop_r), .vi_lines(9'd240), .vi_px0(fb_px0), .vi_pw(fb_pw), .vi_y0(fb_y0), .vi_rows(fb_rows), .vi_xs(fb_xs), .vi_ys(fb_ys), .vi_nw(fb_nw), .osd_en(1'b0), .fps_en(1'b0), .osd_val('0),
    .m_valid(p_valid[0]), .m_addr(p_addr[0]), .m_urgent(p_urgent[0]), .m_ready(p_ready[0]),
    .m_rvalid(p_rvalid[0]), .m_rdata(p_rdata),
    .vi_line(vi_line), .vi_frame(mon_field), .vi_latch,
    .vga_r, .vga_g, .vga_b, .vga_hsync, .vga_vsync, .vga_de, .vga_skip);
assign p_we[0] = 0; assign p_wdata[0] = 0; assign p_be[0] = 4'hF;

endmodule
`default_nettype wire
