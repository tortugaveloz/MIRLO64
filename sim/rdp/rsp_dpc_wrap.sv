// The RSP with the RDP's command registers and fetch (rtl/rsp/rsp_top.sv,
// rtl/rdp/rdp_dpc.sv), as the SoC wires them: for tb_dpc.cpp, which checks
// the command words that come out against the RSP's model.
`default_nettype none
module rsp_dpc_wrap #(parameter DIV_ROM = "rsp_div_rom.hex") (
    input  wire         clk, rst,
    input  wire         h_we,
    input  wire  [3:0]  h_addr,
    input  wire  [31:0] h_wdata,
    output logic [31:0] h_rdata,
    output logic        r_valid, r_we,
    output logic [23:2] r_addr,
    output logic [31:0] r_wdata,
    input  wire         r_ready, r_rvalid,
    input  wire  [31:0] r_rdata,
    output logic        m_valid,
    output logic [20:0] m_addr,
    input  wire         m_ready, m_rvalid,
    input  wire  [31:0] m_rdata,
    output logic        c_valid,
    output logic [31:0] c_data,
    input  wire         c_ready,
    output logic        halted, unimpl, dpc_busy
);
logic        dpc_start_we, dpc_end_we, dpc_status_we, h_ready, irq;
logic [31:0] dpc_wdata, dpc_status, dx_data;
logic [23:0] dpc_start, dpc_end, dpc_end_rd, dpc_current;
logic [9:0]  dx_addr;
rsp_top #(.LANES(4), .DIV_ROM(DIV_ROM)) rsp (.clk, .rst,
    .h_we, .h_rd(1'b0), .h_addr, .h_wdata, .h_rdata, .h_ready, .irq,
    .r_valid, .r_we, .r_addr, .r_wdata, .r_ready, .r_rvalid, .r_rdata,
    .dpc_start_we, .dpc_end_we, .dpc_status_we, .dpc_wdata,
    .dpc_start({8'd0, dpc_start}), .dpc_end({8'd0, dpc_end_rd}), .dpc_current({8'd0, dpc_current}), .dpc_status,
    .dx_addr, .dx_data, .halted, .unimpl,
    .tr_e(), .tr_e_pc(), .tr_e_we(), .tr_e_wr(), .tr_we(), .tr_wr(), .tr_val(), .tr_v(), .tr_vp());
rdp_dpc dpc (.clk, .rst, .start_we(dpc_start_we), .end_we(dpc_end_we), .status_we(dpc_status_we), .wdata(dpc_wdata),
    .start(dpc_start), .end_(dpc_end), .end_rd(dpc_end_rd), .current(dpc_current), .status(dpc_status), .rdp_busy(c_valid),
    .m_valid, .m_addr, .m_ready, .m_rvalid, .m_rdata, .dx_addr, .dx_data, .c_valid, .c_data, .c_ready);
assign dpc_busy = dpc_current != dpc_end || c_valid || dpc_status[10];
endmodule
`default_nettype wire
