// Top-level for the "add every component" incremental simulation: the REAL
// VexRiscvGeom RISC-V core (rtl/geom/VexRiscvGeom.v) wired directly to the
// REAL Vpu4DFixed CFU (rtl/vpu/Vpu4DFixed.v), exactly matching
// the SoC's geometry-subsystem wiring. The Wishbone iBus/
// dBus are exposed raw to the Verilator C++ testbench, which behaviorally
// models geom_rom/geom_ram/the mailbox CSRs/a GDL-holding SDRAM region --
// this file only wires real RTL, no behavioral modeling lives here.
module geom_cpu_top (
    input  wire        clk,
    input  wire        reset,

    output wire         iBus_cmd_valid,
    input  wire         iBus_cmd_ready,
    output wire [31:0]  iBus_cmd_payload_pc,
    input  wire         iBus_rsp_valid,
    input  wire         iBus_rsp_payload_error,
    input  wire [31:0]  iBus_rsp_payload_inst,

    output wire         dTcm_enable,
    output wire [31:0]  dTcm_address,
    output wire         dTcm_write_enable,
    output wire [31:0]  dTcm_write_data,
    output wire [3:0]   dTcm_write_mask,
    input  wire [31:0]  dTcm_read_data,

    output wire         dBusWishbone_CYC,
    output wire         dBusWishbone_STB,
    input  wire         dBusWishbone_ACK,
    output wire         dBusWishbone_WE,
    output wire [29:0]  dBusWishbone_ADR,
    input  wire [31:0]  dBusWishbone_DAT_MISO,
    output wire [31:0]  dBusWishbone_DAT_MOSI,
    output wire [3:0]   dBusWishbone_SEL,
    input  wire         dBusWishbone_ERR,
    output wire [2:0]   dBusWishbone_CTI,
    output wire [1:0]   dBusWishbone_BTE,

    // the CFU's PUSH port (GeomSetupUnit 0x61) toward RDP's command FIFO
    output wire         su_out_valid,
    input  wire         su_out_ready,
    output wire [31:0]  su_out_data
);

    localparam GEOM_SRAM_BASE = 32'h2000_0000;

    wire        cfu_cmd_valid, cfu_cmd_ready;
    wire [9:0]  cfu_cmd_fid;
    wire [31:0] cfu_cmd_in0, cfu_cmd_in1;
    wire        cfu_rsp_valid, cfu_rsp_ready;
    wire [31:0] cfu_rsp_out0;

    // the geom core on MIPS (rtl/mips/mips_geom.sv): the same ports
    mips_geom cpu (
        .externalResetVector(GEOM_SRAM_BASE),
        .timerInterrupt(1'b0),
        .softwareInterrupt(1'b0),
        .externalInterruptArray(32'h0),

        .CfuPlugin_bus_cmd_valid(cfu_cmd_valid),
        .CfuPlugin_bus_cmd_ready(cfu_cmd_ready),
        .CfuPlugin_bus_cmd_payload_function_id(cfu_cmd_fid),
        .CfuPlugin_bus_cmd_payload_inputs_0(cfu_cmd_in0),
        .CfuPlugin_bus_cmd_payload_inputs_1(cfu_cmd_in1),
        .CfuPlugin_bus_rsp_valid(cfu_rsp_valid),
        .CfuPlugin_bus_rsp_ready(cfu_rsp_ready),
        .CfuPlugin_bus_rsp_payload_outputs_0(cfu_rsp_out0),

        .iBus_cmd_valid(iBus_cmd_valid),
        .iBus_cmd_ready(iBus_cmd_ready),
        .iBus_cmd_payload_pc(iBus_cmd_payload_pc),
        .iBus_rsp_valid(iBus_rsp_valid),
        .iBus_rsp_payload_error(iBus_rsp_payload_error),
        .iBus_rsp_payload_inst(iBus_rsp_payload_inst),

        .dTcm_enable(dTcm_enable),
        .dTcm_address(dTcm_address),
        .dTcm_write_enable(dTcm_write_enable),
        .dTcm_write_data(dTcm_write_data),
        .dTcm_write_mask(dTcm_write_mask),
        .dTcm_read_data(dTcm_read_data),
        .dBusWishbone_CYC(dBusWishbone_CYC),
        .dBusWishbone_STB(dBusWishbone_STB),
        .dBusWishbone_ACK(dBusWishbone_ACK),
        .dBusWishbone_WE(dBusWishbone_WE),
        .dBusWishbone_ADR(dBusWishbone_ADR),
        .dBusWishbone_DAT_MISO(dBusWishbone_DAT_MISO),
        .dBusWishbone_DAT_MOSI(dBusWishbone_DAT_MOSI),
        .dBusWishbone_SEL(dBusWishbone_SEL),
        .dBusWishbone_ERR(dBusWishbone_ERR),
        .dBusWishbone_CTI(dBusWishbone_CTI),
        .dBusWishbone_BTE(dBusWishbone_BTE),

        .clk(clk),
        .reset(reset)
    );

    // what the tb's PCPROF samples: the last instruction to reach W
    wire [31:0] prof_pc = cpu.cpu.w_pc;
    wire        prof_ret = cpu.cpu.w_valid;

    Vpu4DFixed #(.DATA_WIDTH(27), .FRAC_BITS(10)) vpu (
        .clk(clk),
        .resetn(~reset),
        .cmd_valid(cfu_cmd_valid),
        .cmd_ready(cfu_cmd_ready),
        .cmd_function_id(cfu_cmd_fid),
        .cmd_inputs_0(cfu_cmd_in0),
        .cmd_inputs_1(cfu_cmd_in1),
        .rsp_valid(cfu_rsp_valid),
        .rsp_ready(cfu_rsp_ready),
        .rsp_outputs_0(cfu_rsp_out0),
        .out_valid(su_out_valid),
        .out_ready(su_out_ready),
        .out_data(su_out_data)
    );

endmodule
