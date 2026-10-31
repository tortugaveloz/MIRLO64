// The real RSP (rtl/rsp_oc/rsp_top_oc.sv) on the gated clock of the game CPU, across rsp_xing.sv, with the RDP's command registers
// and fetch (rtl/rdp/rdp_dpc.sv) on clk_sys, as mirlo_mips.sv wires them with RSP_OC: for tb_ocdpc.cpp, which runs a task and
// checks the command words that come out against the RSP's model (as sim/rdp's tb_dpc does without the crossing).
`default_nettype none
module rsp_oc_wrap #(parameter DIV_ROM = "rsp_div_rom.hex") (
    input  wire         clk, clk_c, rst, rst_c,
    input  wire  [3:0]  duty_n, duty_d,
    // the CPU's access to the SP registers (clk_sys)
    input  wire         s_h_req, s_h_we,
    input  wire  [3:0]  s_h_addr,
    input  wire  [31:0] s_h_wdata,
    output wire         s_h_done,
    output wire  [31:0] s_h_rdata,
    // the arbiter's port (clk_sys)
    output wire         p_valid, p_we,
    output wire  [22:2] p_addr,
    output wire  [31:0] p_wdata,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata,
    // the RDP's command fetch (clk_sys)
    output logic        m_valid,
    output logic [20:0] m_addr,
    input  wire         m_ready, m_rvalid,
    input  wire  [31:0] m_rdata,
    output logic        c_valid,
    output logic [31:0] c_data,
    input  wire         c_ready,
    output logic        halted, unimpl, dpc_busy
);
wire gclk, dpa_t, dq_t;
wire h_we, h_rd; wire [3:0] h_addr; wire [31:0] h_wdata; wire [31:0] h_rdata; wire h_ready;
wire r_valid, r_we; wire [23:2] r_addr; wire [31:0] r_wdata; wire r_ready, r_rvalid; wire [31:0] r_rdata;
wire dpc_start_we, dpc_end_we, dpc_status_we; wire [31:0] dpc_wdata;
wire [31:0] dpc_start_r, dpc_end_r, dpc_current_r, dpc_status_r;
wire irq, halted_i, unimpl_i;
wire s_dp_go; wire [2:0] s_dp_k; wire [31:0] s_dp_wdata;
logic [9:0] dx_addr; logic [31:0] dx_data;
logic [23:0] dpc_start, dpc_end, dpc_end_rd, dpc_current; logic [31:0] dpc_status;
rsp_clk clkgen (.clk_c, .rst_c, .duty_n, .duty_d, .strobe(dpc_start_we || dpc_end_we || dpc_status_we), .dpa_t, .gclk);
rsp_xing xing (.clk_r(gclk), .rst_r(rst_c), .clk_c,
    .h_we, .h_rd, .h_addr, .h_wdata, .h_rdata, .h_ready,
    .r_valid, .r_we, .r_addr, .r_wdata, .r_ready, .r_rvalid, .r_rdata,
    .dpc_start_we, .dpc_end_we, .dpc_status_we, .dpc_wdata, .dpc_start_r, .dpc_end_r, .dpc_current_r, .dpc_status_r, .dq_t,
    .clk, .rst, .s_h_req, .s_h_we, .s_h_addr, .s_h_wdata, .s_h_done, .s_h_rdata,
    .p_valid, .p_we, .p_addr, .p_wdata, .p_ready, .p_rvalid, .p_rdata,
    .s_dp_go, .s_dp_k, .s_dp_wdata, .dpa_t, .s_dpc_start(dpc_start), .s_dpc_end(dpc_end_rd), .s_dpc_current(dpc_current), .s_dpc_status(dpc_status));
rsp_top_oc #(.LANES(8), .MULW(1'b1), .DIV_ROM(DIV_ROM), .DUALCLK(1'b1)) rsp (.clk(gclk), .clk_b(clk), .rst(rst_c),
    .h_we, .h_rd, .h_addr, .h_wdata, .h_rdata, .h_ready, .irq,
    .r_valid, .r_we, .r_addr, .r_wdata, .r_ready, .r_rvalid, .r_rdata,
    .dpc_start_we, .dpc_end_we, .dpc_status_we, .dpc_wdata,
    .dpc_start(dpc_start_r), .dpc_end(dpc_end_r), .dpc_current(dpc_current_r), .dpc_status(dpc_status_r),
    .dx_addr, .dx_data, .halted(halted_i), .unimpl(unimpl_i),
    .tr_e(), .tr_e_pc(), .tr_e2(), .tr_e_we(), .tr_e_wr(), .tr_we(), .tr_wr(), .tr_val(), .tr_mw(), .tr_v(), .tr_vp());
rdp_dpc dpc (.clk, .rst, .start_we(s_dp_go && s_dp_k[0]), .end_we(s_dp_go && s_dp_k[1]), .status_we(s_dp_go && s_dp_k[2]), .wdata(s_dp_wdata),
    .start(dpc_start), .end_(dpc_end), .end_rd(dpc_end_rd), .current(dpc_current), .status(dpc_status), .rdp_busy(c_valid),
    .m_valid, .m_addr, .m_ready, .m_rvalid, .m_rdata, .dx_addr, .dx_data, .c_valid, .c_data, .c_ready);
always_ff @(posedge clk) begin halted <= halted_i; unimpl <= unimpl_i; end
assign dpc_busy = dpc_current != dpc_end || c_valid || dpc_status[10];
endmodule
`default_nettype wire
