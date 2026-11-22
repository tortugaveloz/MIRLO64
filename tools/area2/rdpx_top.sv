// ooc.sh's "rdpx" unit: rdp_xing alone, every port a pin of the unit (the fit makes them virtual)
module ooc_rdpx (
    input  wire clk_r, rst_r, output wire c_valid, output wire [31:0] c_data, input wire c_ready,
    input  wire rm_cmd_valid, rm_cmd_we, input wire [23:2] rm_cmd_addr, output wire rm_cmd_ready,
    input  wire rm_wdata_valid, input wire [31:0] rm_wdata, input wire [3:0] rm_wdata_we, output wire rm_wdata_ready,
    output wire rm_rdata_valid, output wire [31:0] rm_rdata,
    input  wire r_idle, r_tmem_busy, r_sync_ev, input wire [15:0] r_sync_count, input wire [31:0] r_load_count, input wire [15:0] r_unknown_ops,
    input  wire clk, rst, cmd_valid, input wire [31:0] cmd_data, output wire cmd_ready,
    output wire m_cmd_valid, m_cmd_we, output wire [23:2] m_cmd_addr, input wire m_cmd_ready,
    output wire m_wdata_valid, output wire [31:0] m_wdata, output wire [3:0] m_wdata_we, input wire m_wdata_ready,
    input  wire m_rdata_valid, input wire [31:0] m_rdata, input wire q_empty,
    output wire idle, tmem_busy, sync_ev, output wire [15:0] sync_count, output wire [31:0] load_count, output wire [15:0] unknown_ops);
    rdp_xing x (.*);
endmodule
