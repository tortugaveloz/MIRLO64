// ooc.sh's "xing" unit: the RSP's clock gate, its enable and its crossings with clk_sys (rtl/soc/rsp_clk.sv, rsp_xing.sv),
// a pseudo-random stand-in for the RSP on the gated clock and one for the SoC around it on clk. Every output is folded
// into one bit, so nothing is removed.
module ooc_xing (input wire clk, clk_c, rst, rst_c, input wire [3:0] duty_n, duty_d, output wire o);
    wire gclk, dq_t, dpa_t;
    wire h_we, h_rd, h_ready, r_ready, r_rvalid, dpc_start_we, dpc_end_we, dpc_status_we;
    wire [3:0] h_addr; wire [31:0] h_wdata, r_rdata, dpc_start_r, dpc_end_r, dpc_current_r, dpc_status_r;
    logic [63:0] lf = 64'h1234_5678_9ABC_DEF1, lg = 64'hDEAD_BEEF_0BAD_F00D;
    logic [31:0] hr, acc, accs;
    // the stand-in RSP, on gclk
    always_ff @(posedge gclk) begin
        lf <= {lf[62:0], lf[63] ^ lf[62] ^ lf[60] ^ lf[59]};
        hr <= hr ^ {28'd0, h_addr} ^ h_wdata ^ {31'd0, h_we} ^ {31'd0, h_rd} ^ lf[31:0];
        acc <= acc ^ r_rdata ^ dpc_start_r ^ dpc_end_r ^ dpc_current_r ^ dpc_status_r ^ {31'd0, r_rvalid};
    end
    assign h_ready = lf[7];
    wire r_valid = lf[0], r_we = lf[1];
    wire [23:2] r_addr = lf[23:2];
    assign dpc_start_we = lf[10:8] == 3'd0, dpc_end_we = lf[10:8] == 3'd1, dpc_status_we = lf[10:8] == 3'd2;
    rsp_clk clkgen (.clk_c, .rst_c, .duty_n, .duty_d, .strobe(dpc_start_we || dpc_end_we || dpc_status_we), .dpa_t, .gclk);
    // the stand-in SoC, on clk
    logic s_h_req; logic s_h_we; logic [3:0] s_h_addr; logic [31:0] s_h_wdata;
    wire s_h_done, p_valid, p_we, s_dp_go; wire [31:0] s_h_rdata, p_wdata, s_dp_wdata; wire [22:2] p_addr; wire [2:0] s_dp_k;
    logic [3:0] rq; logic [31:0] rd [4];
    wire p_ready = lg[3] | lg[9], p_rvalid = rq[3];
    always_ff @(posedge clk) begin
        lg <= {lg[62:0], lg[63] ^ lg[62] ^ lg[60] ^ lg[59]};
        if (!s_h_req) begin s_h_req <= lg[20]; s_h_we <= lg[21]; s_h_addr <= lg[28:25]; s_h_wdata <= lg[63:32]; end
        else if (s_h_done) s_h_req <= 1'b0;
        rq <= {rq[2:0], p_valid && p_ready && !p_we};
        accs <= accs ^ s_h_rdata ^ p_wdata ^ s_dp_wdata ^ {9'd0, p_addr, 1'b0, p_we} ^ {29'd0, s_dp_k} ^ {31'd0, s_dp_go};
    end
    wire [31:0] p_rdata = lg[63:32] ^ lg[31:0];
    rsp_xing xing (.clk_r(gclk), .rst_r(rst_c), .clk_c, .h_we, .h_rd, .h_addr, .h_wdata, .h_rdata(hr), .h_ready, .r_valid, .r_we, .r_addr, .r_wdata(lf[63:32]), .r_ready, .r_rvalid, .r_rdata,
        .dpc_start_we, .dpc_end_we, .dpc_status_we, .dpc_wdata(lf[47:16]), .dpc_start_r, .dpc_end_r, .dpc_current_r, .dpc_status_r, .dq_t,
        .clk, .rst, .s_h_req, .s_h_we, .s_h_addr, .s_h_wdata, .s_h_done, .s_h_rdata, .p_valid, .p_we, .p_addr, .p_wdata, .p_ready, .p_rvalid, .p_rdata,
        .s_dp_go, .s_dp_k, .s_dp_wdata, .dpa_t, .s_dpc_start(lg[23:0]), .s_dpc_end(lg[47:24]), .s_dpc_current(lg[55:32]), .s_dpc_status(lg[63:32]));
    assign o = ^{hr, acc, accs, r_ready};
endmodule

// ... behind the Pocket's PLL (74.25 MHz in; clk_sys and clk_cpu out, as core_top.sv has them)
module ooc_xingpll (input wire refclk, rst, rst_c, input wire [3:0] duty_n, duty_d, output wire o);
    wire clk_sys, clk_cpu, o1, o2, o3, o4, lk;
    mf_pllbase mp1 (.refclk, .rst(1'b0), .outclk_0(clk_sys), .outclk_1(o1), .outclk_2(o2), .outclk_3(o3), .outclk_4(o4), .outclk_5(clk_cpu), .locked(lk));
    wire x;
    ooc_xing xing (.clk(clk_sys), .clk_c(clk_cpu), .rst, .rst_c, .duty_n, .duty_d, .o(x));
    assign o = x ^ o1 ^ o2 ^ o3 ^ o4 ^ lk;
endmodule
