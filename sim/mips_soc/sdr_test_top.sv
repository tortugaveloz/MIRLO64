// sdr_arb + sdr_ctrl + sdr_phy with four request ports, for tb_sdr.cpp
`define RL 5
`default_nettype none
module sdr_test_top (
    input  wire         clk, clk2x, rst,
    output wire         ready,
    input  wire  [3:0]  p_valid, p_we, p_urgent,
    input  wire  [3:0][23:0] p_addr,
    input  wire  [3:0][31:0] p_wdata,
    input  wire  [3:0][3:0]  p_be,
    output wire  [3:0]  p_ready, p_rvalid,
    output wire  [31:0] p_rdata,
    output wire  [12:0] sdram_a,
    output wire  [1:0]  sdram_ba, sdram_dm,
    output wire         sdram_ras_n, sdram_cas_n, sdram_we_n, sdram_cke,
    output wire  [15:0] dq_out,
    output wire         dq_oe,
    input  wire  [15:0] dq_in
);
logic rst2x;
always_ff @(posedge clk2x) rst2x <= rst;
wire rq_valid, rq_we, rq_ready, rd_valid;
wire [23:0] rq_addr; wire [31:0] rq_wdata, rd_data; wire [3:0] rq_be; wire [2:0] rq_tag, rd_tag;
wire la_valid; wire [23:0] la_addr;
`define AHEAD 1
sdr_arb #(.N(4)) arb (.clk, .rst, .p_valid, .p_we, .p_addr, .p_wdata, .p_be, .p_urgent, .apf_force(1'b0), .p_ready, .p_rvalid, .p_rdata, .la_valid, .la_addr,
    .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag),
    .req_ready(rq_ready), .rd_valid, .rd_data, .rd_tag);
wire [1:0] ras_n, cas_n, we_n; wire [25:0] a; wire [3:0] ba, wm; wire cke, wren, rden, phy_rv; wire [31:0] wd, phy_rd;
sdr_ctrl #(.T_INIT(100), .RL(`RL), .AHEAD(`AHEAD)) ctrl (.clk, .rst, .ready, .req_valid(rq_valid), .req_we(rq_we), .req_addr(rq_addr), .la_valid, .la_addr,
    .req_wdata(rq_wdata), .req_be(rq_be), .req_tag(rq_tag), .req_ready(rq_ready), .rd_valid, .rd_data, .rd_tag,
    .dfi_ras_n(ras_n), .dfi_cas_n(cas_n), .dfi_we_n(we_n), .dfi_addr(a), .dfi_ba(ba), .dfi_cke(cke),
    .dfi_wrdata_en(wren), .dfi_wrdata(wd), .dfi_wrdata_mask(wm), .dfi_rddata_en(rden),
    .dfi_rddata(phy_rd), .dfi_rddata_valid(phy_rv));
// the PHY with its DQ pad split for the model (the tristate is the model's)
wire [15:0] dq;
sdr_phy phy (.clk, .rst, .clk2x, .rst2x, .clk2x_90(clk2x), .ras_n, .cas_n, .we_n, .addr(a), .ba, .cke,
    .wrdata_en(wren), .wrdata(wd), .wrdata_mask(wm), .rddata_en(rden), .rddata(phy_rd), .rddata_valid(phy_rv),
    .sdram_a, .sdram_ba, .sdram_ras_n, .sdram_cas_n, .sdram_we_n, .sdram_cke, .sdram_dm, .sdram_dq(dq), .sdram_clock());
assign dq_oe = phy.oe_reg;
assign dq_out = phy.out_reg;
assign dq = dq_oe ? 16'bz : dq_in;
endmodule
`default_nettype wire
