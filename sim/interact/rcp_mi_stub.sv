// rcp_mi with nothing inside: mirlo_regs instantiates the MI and the PI's engine, which interact_test.sv does not exercise (and
// whose enum ternaries Icarus rejects as needing a cast). Same ports as rtl/soc/rcp_mi.sv; every output is 0.
`default_nettype none
module rcp_mi #(parameter bit PI = 0, parameter bit PI2 = 0) (
    input  wire         clk, rst,
    input  wire         sp_lvl, vi_pulse, dp_pulse, vi_clr, ai_pulse, ai_clr, si_pulse, si_clr,
    input  wire         raise_we, clr_we,
    input  wire  [5:0]  csr_bits,
    input  wire         dst_we, src_we, len_we,
    input  wire  [24:0] csr_wd,
    output wire  [5:0]  intr, mask_o,
    output wire         pi_busy,
    output wire  [20:0] pi_cnt,
    input  wire         w_mi, w_pi, w_we,
    input  wire  [1:0]  w_a,
    input  wire  [31:0] w_wd,
    output logic [31:0] w_rd,
    output wire         line,
    output wire         d_req, d_we,
    output wire  [23:0] d_adr,
    output wire  [3:0]  d_sel,
    output wire  [31:0] d_wdata,
    input  wire  [31:0] d_rdata,
    input  wire         d_ack,
    input  wire         io_req, io_we,
    input  wire  [23:0] io_adr,
    input  wire  [31:0] io_wd,
    output wire         io_ack
);
assign intr = 0; assign mask_o = 0; assign pi_busy = 0; assign pi_cnt = 0; assign line = 0;
assign d_req = 0; assign d_we = 0; assign d_adr = 0; assign d_sel = 0; assign d_wdata = 0; assign io_ack = 0;
always_comb w_rd = 0;
endmodule
`default_nettype wire
