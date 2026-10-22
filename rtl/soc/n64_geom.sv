// The geometry core: mips_geom (MIPS, rtl/mips/mips_geom.sv). It has no
// I-cache (it fetches from its ROM through a port of its own) and has the
// Vpu4DFixed CFU, whose PUSH port feeds RDP's command FIFO.
//   ROM A  0x2000_0000  16 KiB  iBus + dBus (.text.start, .rodata, .data's image)
//   RAM    0x2000_8000  16 KiB  the core's TCM (GeomTcmRam port A)
//   ROM B  0x2001_0000  64 KiB  iBus only (code)
// Anything else on the dBus goes out (n64_bus): SDRAM, the registers.
// The ROMs are loaded by the game CPU, each game its own firmware
// (lang/c/game/geom_load.c), through write ports (l_*) that only take
// effect while the core is held in reset (run = 0, MAILBOX.GEOM_RUN).
// ROM A is a true dual-port M10K (GeomTcmRam): port A the iBus, port B the
// dBus -- or the loader, while the core is in reset. ROM B: one write port,
// one read port.
`default_nettype none

module n64_geom (
    input  wire         clk,
    input  wire         rst,
    // the core out of reset, and the loader's word writes into its ROMs
    // (l_b: ROM B, else ROM A; word address)
    input  wire         run,
    input  wire         l_we,
    input  wire         l_b,
    input  wire  [13:0] l_adr,
    input  wire  [31:0] l_dat,
    // out: SDRAM and the registers (Wishbone classic)
    output logic        o_cyc, o_stb, o_we,
    output logic [29:0] o_adr,
    output logic [31:0] o_dat_w,
    output logic [3:0]  o_sel,
    output logic [2:0]  o_cti,
    input  wire         o_ack,
    input  wire  [31:0] o_dat_r,
    // the CFU's PUSH stream (to RDP's command FIFO)
    output logic        su_valid,
    input  wire         su_ready,
    output logic [31:0] su_data,
    // the TCM's second port (the game CPU's window on the geom RAM: word
    // address, byte write enables, the read a cycle later), and an interrupt
    input  wire  [11:0] t_adr,
    input  wire  [3:0]  t_we,
    input  wire  [31:0] t_dat_w,
    output logic [31:0] t_dat_r,
    input  wire         irq,
    output logic [31:0] dbg_pc
);

// ---- ROMs (ROM A: GeomTcmRam below)
logic [31:0] rom_b [16384];
wire core_rst = rst || !run;
wire load = l_we && !run;

// ---- the core
logic        ib_valid, ib_rsp, ib_sel;
logic [31:0] ib_pc, ib_ia, ib_ib;
logic        cfu_cmd_valid, cfu_cmd_ready, cfu_rsp_valid, cfu_rsp_ready;
logic [9:0]  cfu_fid;
logic [31:0] cfu_in0, cfu_in1, cfu_out0;
logic        tcm_en, tcm_we;
logic [31:0] tcm_adr, tcm_wdat, tcm_rdat;
logic [3:0]  tcm_mask;
logic        d_cyc, d_stb, d_we, d_ack, d_err;
logic [29:0] d_adr;
logic [31:0] d_dat_w, d_dat_r;
logic [3:0]  d_sel;
logic [2:0]  d_cti;
logic [1:0]  d_bte;

// (mips_lite + a write-through data cache, rtl/mips/mips_geom.sv)
mips_geom cpu (
    .clk, .reset(core_rst), .externalResetVector(32'h2000_0000),
    .timerInterrupt(1'b0), .softwareInterrupt(1'b0), .externalInterruptArray({31'd0, irq}),
    .CfuPlugin_bus_cmd_valid(cfu_cmd_valid), .CfuPlugin_bus_cmd_ready(cfu_cmd_ready),
    .CfuPlugin_bus_cmd_payload_function_id(cfu_fid),
    .CfuPlugin_bus_cmd_payload_inputs_0(cfu_in0), .CfuPlugin_bus_cmd_payload_inputs_1(cfu_in1),
    .CfuPlugin_bus_rsp_valid(cfu_rsp_valid), .CfuPlugin_bus_rsp_ready(cfu_rsp_ready),
    .CfuPlugin_bus_rsp_payload_outputs_0(cfu_out0),
    .iBus_cmd_valid(ib_valid), .iBus_cmd_ready(1'b1), .iBus_cmd_payload_pc(ib_pc),
    .iBus_rsp_valid(ib_rsp), .iBus_rsp_payload_error(1'b0), .iBus_rsp_payload_inst(ib_sel ? ib_ib : ib_ia),
    .dTcm_enable(tcm_en), .dTcm_address(tcm_adr), .dTcm_write_enable(tcm_we),
    .dTcm_write_data(tcm_wdat), .dTcm_write_mask(tcm_mask), .dTcm_read_data(tcm_rdat),
    .dBusWishbone_CYC(d_cyc), .dBusWishbone_STB(d_stb), .dBusWishbone_ACK(d_ack), .dBusWishbone_WE(d_we),
    .dBusWishbone_ADR(d_adr), .dBusWishbone_DAT_MISO(d_dat_r), .dBusWishbone_DAT_MOSI(d_dat_w),
    .dBusWishbone_SEL(d_sel), .dBusWishbone_ERR(d_err), .dBusWishbone_CTI(d_cti), .dBusWishbone_BTE(d_bte)
);

Vpu4DFixed #(.DATA_WIDTH(27), .FRAC_BITS(10)) cfu (
    .clk, .resetn(!core_rst),
    .cmd_valid(cfu_cmd_valid), .cmd_ready(cfu_cmd_ready), .cmd_function_id(cfu_fid),
    .cmd_inputs_0(cfu_in0), .cmd_inputs_1(cfu_in1),
    .rsp_valid(cfu_rsp_valid), .rsp_ready(cfu_rsp_ready), .rsp_outputs_0(cfu_out0),
    .out_valid(su_valid), .out_ready(su_ready), .out_data(su_data)
);

// (registered: mips_geom's iBus PC is combinational through its stall logic,
// and the OSD reads dbg_pc from the video clock domain)
always_ff @(posedge clk) dbg_pc <= ib_pc;

// ---- iBus: a command every cycle, its instruction the next (the ROMs' registered read)
always_ff @(posedge clk) begin
    ib_rsp <= ib_valid && !core_rst;
    ib_sel <= ib_pc[16];
    if (load && l_b) rom_b[l_adr] <= l_dat;
    ib_ib  <= rom_b[ib_pc[15:2]];
end

// ---- TCM
GeomTcmRam tcm (
    .clk, .a_en(tcm_en), .a_adr(tcm_adr[13:2]), .a_we({4{tcm_we}} & tcm_mask), .a_dat_w(tcm_wdat), .a_dat_r(tcm_rdat),
    .b_adr(t_adr), .b_we(t_we), .b_dat_w(t_dat_w), .b_dat_r(t_dat_r)
);

// ---- dBus: ROM A's data port, or out
wire  in_rom = d_adr[29:13] == 17'h4000;             // 0x2000_0000 - 0x2000_7FFF
logic rom_ack;
logic [31:0] rom_q;
always_ff @(posedge clk) rom_ack <= d_cyc && d_stb && in_rom && !rom_ack && !core_rst;
GeomTcmRam rom_a (
    .clk, .a_en(1'b1), .a_adr(ib_pc[13:2]), .a_we(4'd0), .a_dat_w(32'd0), .a_dat_r(ib_ia),
    .b_adr(run ? d_adr[11:0] : l_adr[11:0]), .b_we({4{load && !l_b}}), .b_dat_w(l_dat), .b_dat_r(rom_q)
);
assign o_cyc = d_cyc && !in_rom;
assign o_stb = d_stb && !in_rom;
assign o_we = d_we; assign o_adr = d_adr; assign o_dat_w = d_dat_w; assign o_sel = d_sel; assign o_cti = d_cti;
assign d_ack = in_rom ? rom_ack : o_ack;
assign d_dat_r = in_rom ? rom_q : o_dat_r;
assign d_err = 1'b0;

endmodule
`default_nettype wire
