// mirlo_mips for tb_soc.cpp: the SDRAM's DQ split for the chip model, the
// Pocket's side as inputs, a few internals out for the bench.
`default_nettype none
module soc_sim_top (
    input  wire         clk_sys, clk_sys2x, clk_vid, reset,
    input  wire         clk_cpu,
    input  wire         host_reset_n, host_loaded,
    input  wire  [31:0] file_size,
    output wire         file_size_wr,       // the program sets the selected slot's size (what the Pocket writes back)
    output wire  [31:0] file_size_new,
    input  wire         br_complete,
    output wire         br_req_read,
    output wire  [15:0] br_slot,
    output wire  [31:0] br_offset, br_length, br_addr,
    input  wire  [31:0] cont1_key, cont2_key, cont3_key, cont4_key,     // the dock's pads: KEY_TYPE in [31:28] (0: none)
    input  wire  [31:0] cont1_joy, cont2_joy, cont3_joy, cont4_joy,
    input  wire   [3:0] ia_address,         // the Pocket's interact bus (the core's settings: interact.json), INTERACT=<address>:<value>
    input  wire  [31:0] ia_data,
    input  wire         ia_wr,
    output wire  [12:0] sdram_a,
    output wire  [1:0]  sdram_ba, sdram_dm,
    output wire         sdram_ras_n, sdram_cas_n, sdram_we_n, sdram_cke,
    output wire  [15:0] dq_out,
    output wire         dq_oe,
    input  wire  [15:0] dq_in,
    output wire  [7:0]  vga_r, vga_b,
    output wire  [7:0]  vga_g,
    output wire         vga_de, vga_hsync, vga_vsync, vga_skip,
    output wire  [11:0] fb_width,
    output wire         fb_tall,
    output wire  [31:0] vi_dbg0, vi_dbg1,   // (VGAT) the VI's: {hs_zero, black, mon_vi, tv_pal, vs_set, vs_pal, origin}; {thr, WIDTH}
    output wire  [7:0]  fb_y0,
    output wire  [8:0]  fb_rows,
    output wire  [9:0]  fb_px0, fb_pw,
    output wire  [10:0] fb_xs, fb_ys, fb_nw,
    output wire         boot_ready,
    output wire         uart_we,
    output wire  [7:0]  uart_byte,
    output wire  [31:0] cpu_pc, cpu_cause, cpu_epc, geom_pc, geom_msg, fb_base, rdp_sync, audio_pc, audio_state,
    output wire         fb_mode, fb_on, fb_gamma, vblank, audio_wr,
    // (RSPSTAT) the RSP this cycle: 0 running, 1 a DMA under way, 2 its request
    // not taken, 3 an instruction leaves E, 4 a command run waits for the one
    // being fetched, 5 commands being fetched or drawn
    output wire  [5:0]  rsp_stat,
    output wire  [7:0]  rsp_sig,            // SP_STATUS's signals (libultra: 0 yield asked, 1 yielded, 2 task done)
    // (MEMSTAT) the SDRAM this cycle: [5:0] the port whose request was taken
    // (0 scan-out, 1 CPU, 2 RDP, 3 RSP DMA, 4 bridge, 5 command fetch), [8:6]
    // the controller's command {ras_n, cas_n, we_n} and [10:9] its bank, [11]
    // its request taken, [12] a request at it, [13] a port asking, [14] the
    // RDP at work, [15] the RSP running
    output wire  [15:0] mem_stat,
    output wire         mem_hit,        // (the controller's request is of an open row)
    // (CPUSTAT) the game CPU this cycle of its clock: [0] an instruction
    // retires, [1] M waits, [4:2] M's slow-path state, [7:5] F's state, [8] F
    // has an instruction, [9] D waits (a hazard), [10] the FPU at work, [11]
    // a multiply or divide, [12] a store at M with the write buffer full,
    // [13] a flush, [16:14] the memory port's state, [17] D holds an instruction
    output wire  [17:0] cpu_stat,
    // (CPUTRACE) what the game CPU retires this cycle of its clock, and an exception it takes
    output wire         tr_retire, tr_wr, tr_exc,
    output wire  [31:0] tr_pc, tr_insn, tr_val, tr_exc_pc,
    output wire  [4:0]  tr_rd, tr_exc_code,
    output wire  [31:0] audio_out,
    output wire         x_req, x_we, x_ack,
    output wire  [31:0] x_addr, x_rdata, x_wdata,
    // the Pocket's bridge as the SoC sees it (apf_wishbone_master's word
    // addresses): the bench's Pocket writes a game file and the slots through
    // it, and reads the nonvolatile ones back
    input  wire         wbm_cyc, wbm_we,
    input  wire  [29:0] wbm_adr,
    input  wire  [31:0] wbm_dat_w,
    output wire         wbm_ack,
    output wire  [31:0] wbm_dat_r,
    // the PSRAMs' pads as the PHY's registers hold them (the chips' model is the bench's)
    output wire  [5:0]  ps_a,
    output wire  [31:0] ps_dq_out,
    output wire         ps_dq_oe, ps_adv_n, ps_oe_n, ps_we_n, ps_cre, ps_clk_en,
    output wire  [1:0]  ps_ce_n,
    output wire  [3:0]  ps_bm_n,
    input  wire  [31:0] ps_dq_in,
    output wire         ps_ready
);
wire [15:0] dq;
wire [15:0] c0dq, c1dq;
wire c0clk, c1clk;
// the Pocket's cartridge port (MIRLO_GB_CART: a Game Boy cartridge on it, +gbcart=<rom>)
wire        gb_on, gb_a_dir, gb_a_oe, gb_d_dir, gb_d_oe, gb_phi, gb_wr_n, gb_rd_n, gb_cs_n;
wire [15:0] gb_a;
wire [7:0]  gb_d_o, gb_d_i;
gb_cart_model gbcart (.clk(clk_sys), .res_n(gb_on), .a_dir(gb_a_dir), .a_oe(gb_a_oe), .a(gb_a), .d_dir(gb_d_dir),
    .d_oe(gb_d_oe), .d_o(gb_d_o), .d_i(gb_d_i), .phi(gb_phi), .wr_n(gb_wr_n), .rd_n(gb_rd_n), .cs_n(gb_cs_n));
// the APF audio FIFO (core_top's): its fill, flushed, drained at 48 kHz while playing
logic [11:0] aud_fill = 0;
logic [10:0] aud_div = 0;
wire         aud_flush, aud_play;
always_ff @(posedge clk_sys) begin
    aud_div <= aud_div == 11'd1308 ? 11'd0 : aud_div + 11'd1;          // 62.832 MHz / 48 kHz
    if (aud_flush) aud_fill <= 0;
    else aud_fill <= aud_fill + {11'd0, audio_wr && aud_fill != 12'hFFF} - {11'd0, aud_play && aud_div == 0 && aud_fill != 0};
end
mirlo_mips #(.SDR_T_INIT(100)) soc (
    .altera_reserved_tck(1'b0), .altera_reserved_tdi(1'b0), .altera_reserved_tdo(), .altera_reserved_tms(1'b0),
    .apf_audio_buffer_fill(aud_fill), .apf_audio_bus_out(audio_out), .apf_audio_bus_wr(audio_wr), .apf_audio_flush(aud_flush),
    .apf_audio_playback_en(aud_play),
    .apf_bridge_boot_ready(boot_ready), .apf_bridge_command_result_code(3'd0), .apf_bridge_complete_trigger(br_complete),
    .apf_bridge_current_address(32'd0), .apf_bridge_data_offset(br_offset), .apf_bridge_file_size(file_size),
    .apf_bridge_file_size_wr(file_size_wr), .apf_bridge_host_loaded(host_loaded), .apf_bridge_host_reset_n(host_reset_n),
    .apf_bridge_length(br_length), .apf_bridge_new_file_size_data(file_size_new), .apf_bridge_ram_data_address(br_addr),
    .apf_bridge_request_getfile(), .apf_bridge_request_openfile(), .apf_bridge_request_read(br_req_read), .apf_bridge_request_write(),
    .apf_bridge_slot_id(br_slot), .apf_id_chip_id(64'd0),
    .apf_input_cont1_joy(cont1_joy), .apf_input_cont1_key(cont1_key), .apf_input_cont1_trig(32'd0),
    .apf_input_cont2_joy(cont2_joy), .apf_input_cont2_key(cont2_key), .apf_input_cont2_trig(32'd0),
    .apf_input_cont3_joy(cont3_joy), .apf_input_cont3_key(cont3_key), .apf_input_cont3_trig(32'd0),
    .apf_input_cont4_joy(cont4_joy), .apf_input_cont4_key(cont4_key), .apf_input_cont4_trig(32'd0),
    .apf_interact_address(ia_address), .apf_interact_data(ia_data), .apf_interact_q(), .apf_interact_wr(ia_wr),
    .apf_rtc_date_bcd(32'd0), .apf_rtc_time_bcd(32'd0), .apf_rtc_unix_seconds(32'd0),
    .clk_sys, .clk_sys2x, .clk_sys2x_90deg(clk_sys2x), .clk_vid, .reset,
    .clk_cpu,
    .sdram_a, .sdram_ba, .sdram_cas_n, .sdram_cke, .sdram_clock(), .sdram_dm, .sdram_dq(dq), .sdram_ras_n, .sdram_we_n,
    .cram0_a(), .cram1_a(), .cram0_dq(c0dq), .cram1_dq(c1dq),
    .cram0_clk(c0clk), .cram1_clk(c1clk), .cram0_adv_n(), .cram1_adv_n(), .cram0_cre(), .cram1_cre(),
    .cram0_ce0_n(), .cram0_ce1_n(), .cram1_ce0_n(), .cram1_ce1_n(),
    .cram0_oe_n(), .cram1_oe_n(), .cram0_we_n(), .cram1_we_n(), .cram0_ub_n(), .cram0_lb_n(), .cram1_ub_n(), .cram1_lb_n(),
    .serial_rx(1'b1), .serial_tx(), .use_jtag(1'b0),
    .gb_on, .gb_a_dir, .gb_a_oe, .gb_a, .gb_d_o, .gb_d_dir, .gb_d_oe, .gb_d_i, .gb_phi, .gb_wr_n, .gb_rd_n, .gb_cs_n,
    .vga_b, .vga_de, .vga_g, .vga_hsync, .vga_r, .vga_skip, .vga_vsync,
    .wishbone_ack(1'b0), .wishbone_adr(), .wishbone_bte(), .wishbone_cti(), .wishbone_cyc(), .wishbone_dat_r(32'd0),
    .wishbone_dat_w(), .wishbone_err(1'b0),
    .wishbone_master_ack(wbm_ack), .wishbone_master_adr(wbm_adr), .wishbone_master_bte(2'd0), .wishbone_master_cti(3'd0),
    .wishbone_master_cyc(wbm_cyc), .wishbone_master_dat_r(wbm_dat_r), .wishbone_master_dat_w(wbm_dat_w), .wishbone_master_err(),
    .wishbone_master_sel(4'hF), .wishbone_master_stb(wbm_cyc), .wishbone_master_we(wbm_we),
    .wishbone_sel(), .wishbone_stb(), .wishbone_we()
);
assign c0dq = soc.ps_o_dq_oe ? 16'bz : ps_dq_in[31:16];
assign c1dq = soc.ps_o_dq_oe ? 16'bz : ps_dq_in[15:0];
assign ps_a = soc.ps_o_a;           assign ps_dq_out = soc.ps_o_dq;   assign ps_dq_oe = soc.ps_o_dq_oe;
assign ps_adv_n = soc.ps_o_adv_n;   assign ps_oe_n = soc.ps_o_oe_n;   assign ps_we_n = soc.ps_o_we_n;
assign ps_cre = soc.ps_o_cre;       assign ps_clk_en = c0clk & c1clk;       // (the pins: the bench looks at them each quarter it steps in)
assign ps_ce_n = soc.ps_o_ce_n;     assign ps_bm_n = soc.ps_o_bm_n;
assign ps_ready = soc.ps_ready;
assign dq_oe  = soc.phy.oe_reg;
assign dq_out = soc.phy.out_reg;
assign dq = dq_oe ? 16'bz : dq_in;
assign uart_we   = soc.r_uart_we;
assign uart_byte = soc.r_uart_byte;
assign cpu_pc    = soc.sys.cpu.f_pc;
assign cpu_cause = soc.dbg_cause;
assign cpu_epc   = soc.dbg_epc;
assign geom_pc   = soc.dbg_geom_pc;
assign geom_msg  = soc.dbg_geom_msg;
assign fb_base   = soc.fb_base;
assign fb_mode   = soc.fb_mode;
assign fb_on     = soc.fb_en;    // (off: the picture is black)
assign fb_width  = soc.fb_width;
assign fb_tall   = soc.fb_tall;
assign vi_dbg0   = {soc.vi.hs_zero, soc.vi.black, soc.mon_vi, soc.tv_pal, soc.vi.vs_set, soc.vi.vs_pal, 3'd0, soc.vi.org_b};
assign vi_dbg1   = {3'd0, soc.vi_thr, soc.vi.wid};
assign fb_px0    = soc.fb_px0; assign fb_pw = soc.fb_pw; assign fb_xs = soc.fb_xs; assign fb_ys = soc.fb_ys; assign fb_nw = soc.fb_nw;
assign fb_y0     = soc.fb_y0;
assign fb_rows   = soc.fb_rows;
assign fb_gamma  = soc.fb_gamma;                              // (the scan-out's gamma correction is on)
assign mem_hit   = soc.sdram.open[soc.sdram.q_bank] && soc.sdram.row[soc.sdram.q_bank] == soc.sdram.q_row;
assign mem_stat  = {!soc.rsp_halted, soc.cmd_valid || !soc.rdp_idle, |soc.p_valid, soc.sdram.req_valid, soc.sdram.req_ready,
                    soc.sdram.cmd_ba, soc.sdram.cmd, 6'(soc.p_valid & soc.p_ready)};
assign rsp_sig   = soc.rsp.st_sig;
assign rsp_stat  = {soc.dpc_status[3], soc.dpc.end_valid, soc.rsp.tr_e, soc.rsp_r_valid && !soc.p_ready[3],
                    soc.rsp.ds != 0, !soc.rsp_halted};
assign cpu_stat  = {soc.sys.cpu.d_valid, 3'(soc.sys.cpu.port), soc.sys.cpu.flush, soc.sys.cpu.m_live && soc.sys.cpu.m_st && soc.sys.cpu.wb_full,
                    soc.sys.cpu.md_busy, soc.sys.cpu.fpu_busy, soc.sys.cpu.d_stall, soc.sys.cpu.f_have, 3'(soc.sys.cpu.fs), 3'(soc.sys.cpu.ms),
                    soc.sys.cpu.m_stall, soc.sys.cpu.w_valid};
assign tr_retire = soc.sys.cpu.tr_retire; assign tr_wr = soc.sys.cpu.tr_wr; assign tr_exc = soc.sys.cpu.tr_exc;
assign tr_pc = soc.sys.cpu.tr_pc; assign tr_insn = soc.sys.cpu.tr_insn; assign tr_val = soc.sys.cpu.tr_val[31:0];
assign tr_rd = soc.sys.cpu.tr_rd; assign tr_exc_pc = soc.sys.cpu.tr_exc_pc; assign tr_exc_code = soc.sys.cpu.tr_exc_code;
assign vblank    = soc.vblank;
assign rdp_sync = soc.rdp_sync;
assign audio_pc  = soc.dbg_aud_pc;
assign x_req = soc.x_req; assign x_we = soc.x_we; assign x_ack = soc.x_ack;
assign x_addr = soc.x_addr; assign x_rdata = soc.x_rdata; assign x_wdata = soc.x_wdata;
assign audio_state = soc.dbg_aud_state;
endmodule
`default_nettype wire
