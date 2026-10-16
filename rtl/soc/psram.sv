// The Pocket's two PSRAMs (cram0, cram1: AS1C8M16PL-70, each two 64 Mbit
// CellularRAM dies, address / data multiplexed) as one 32-bit memory of 8 M
// words, 32 MiB: a word's upper half on cram0, its lower half on cram1, both
// chips driven alike; the word's bit 22 is the die (CE0# or CE1#). It holds a
// ROM's first 32 MiB (docs/mips.md): single words, asynchronous -- the chips'
// power-up mode -- at the X port, one at a time.
//
// An asynchronous access (70 ns parts; clk's period 16 ns), in cycles of clk:
// the address with ADV# low for a cycle, held another; a read's OE# low from
// cycle 2 and its data taken 6 periods (96 ns) after the address; a write's
// WE# low for four cycles (64 ns) with the data; then CE# high for two
// cycles. The chips' clock is never started. When `wake` rises (the SDRAM's
// own power-up wait, 200 us: more than the chips' 150) the two dies' BCRs are
// written with their power-up value (another core may have left them
// synchronous) and their refresh registers likewise (it may have left a part
// of the array without refresh), CRE high.
`default_nettype none

module psram (
    input  wire         clk,
    input  wire         rst,
    input  wire         wake,               // the chips' power-up time is over
    input  wire         x_valid,            // held until x_ack
    input  wire         x_we,
    input  wire  [22:0] x_addr,             // word
    input  wire  [31:0] x_wdata,
    input  wire  [3:0]  x_be,
    output logic        x_ack,              // a cycle; a read's data with it, in that cycle only
    output wire  [31:0] x_rdata,
    output logic        ready,              // the chips are set up
    // ---- the pads' registers (psram_phy)
    output logic [5:0]  o_a,                // A[21:16]
    output logic [31:0] o_dq,               // {cram0, cram1}
    output logic        o_dq_oe,
    output logic        o_adv_n, o_oe_n, o_we_n, o_cre,
    output logic [1:0]  o_ce_n,             // [0] die 0, [1] die 1
    output logic [3:0]  o_bm_n,             // {cram0 UB#, LB#, cram1 UB#, LB#}
    input  wire  [31:0] i_dq                // the pads at the last rising edge
);
localparam logic [15:0] BCR_ASYNC = 16'h9D1F;   // the power-up value
localparam logic [15:0] RCR_ALL   = 16'h0010;   // the whole array refreshed, no deep power-down (the power-up value)
localparam logic [5:0]  A_BCR     = 6'b001000;  // A[19:18] = 10 (the RCR: 00)

typedef enum logic [2:0] { S_INIT, S_IDLE, S_ASYNC, S_AEND, S_GAP } st_t;
st_t         st;
logic [2:0]  t;                             // the cycle within an access
logic        a_cfg;                         // the access is a register's write (else the X port's)
wire         a_we = a_cfg || x_we;
logic [1:0]  cfg_n;                         // the registers written so far: BCR die 0, die 1, RCR die 0, die 1
assign x_rdata = i_dq;

always_ff @(posedge clk) begin
    x_ack <= 1'b0;
    if (rst) begin
        st <= S_INIT; t <= '0; ready <= 1'b0; cfg_n <= 2'd0; a_cfg <= 1'b0;
        o_ce_n <= 2'b11; o_adv_n <= 1'b1; o_oe_n <= 1'b1; o_we_n <= 1'b1; o_cre <= 1'b0; o_dq_oe <= 1'b0;
        o_bm_n <= 4'h0; o_a <= '0; o_dq <= '0;
    end else case (st)
    // ---- reset: the chips' power-up time, then the four registers
    S_INIT: if (wake) begin
        a_cfg <= 1'b1;
        o_ce_n <= cfg_n[0] ? 2'b01 : 2'b10; o_adv_n <= 1'b0; o_cre <= 1'b1;
        o_a <= cfg_n[1] ? 6'd0 : A_BCR; o_dq <= {2{cfg_n[1] ? RCR_ALL : BCR_ASYNC}}; o_dq_oe <= 1'b1;
        t <= '0; st <= S_ASYNC;
    end
    S_IDLE: if (x_valid) begin
        a_cfg <= 1'b0;
        o_ce_n <= x_addr[22] ? 2'b01 : 2'b10; o_adv_n <= 1'b0;
        o_a <= x_addr[21:16]; o_dq <= {2{x_addr[15:0]}}; o_dq_oe <= 1'b1;
        t <= '0; st <= S_ASYNC;
    end
    // ---- an asynchronous access (and a register's write: CRE high, its value on the address's lines throughout)
    S_ASYNC: begin
        t <= t + 3'd1;
        o_adv_n <= 1'b1;
        if (t == 3'd1) begin
            if (a_we) begin if (!a_cfg) begin o_dq <= x_wdata; o_bm_n <= ~x_be; end o_we_n <= 1'b0; end
            else begin o_dq_oe <= 1'b0; o_oe_n <= 1'b0; end
        end
        if (t == 3'd5) o_we_n <= 1'b1;
        if (t == 3'd5 && !a_we) x_ack <= 1'b1;         // (a read: its data is the pads' 6 periods after the address, i_dq
                                                       // in the cycle of the ack)
        if (t == (a_we ? 3'd5 : 3'd6)) begin
            if (!a_we) begin o_oe_n <= 1'b1; o_ce_n <= 2'b11; end
            st <= S_AEND;
        end
    end
    S_AEND: begin
        o_ce_n <= 2'b11; o_dq_oe <= 1'b0; o_cre <= 1'b0; o_bm_n <= 4'h0; t <= '0;
        if (a_cfg) begin
            cfg_n <= cfg_n + 2'd1;
            if (cfg_n == 2'd3) ready <= 1'b1;
        end else x_ack <= a_we;
        st <= S_GAP;
    end
    S_GAP: begin t <= t + 3'd1; if (t[0]) st <= ready ? S_IDLE : S_INIT; end
    default: st <= S_INIT;
    endcase
end

endmodule

// The pads: every output a register at its pad (the project's fast output /
// input register assignments on cram*), the bus's two directions; the chips'
// clock low.
module psram_phy (
    input  wire         clk,
    input  wire  [5:0]  o_a,
    input  wire  [31:0] o_dq,
    input  wire         o_dq_oe, o_adv_n, o_oe_n, o_we_n, o_cre,
    input  wire  [1:0]  o_ce_n,
    input  wire  [3:0]  o_bm_n,
    output logic [31:0] i_dq,
    output wire  [21:16] cram0_a, cram1_a,
    inout  wire  [15:0] cram0_dq, cram1_dq,
    output wire         cram0_clk, cram1_clk,
    output wire         cram0_adv_n, cram1_adv_n,
    output wire         cram0_cre, cram1_cre,
    output wire         cram0_ce0_n, cram0_ce1_n, cram1_ce0_n, cram1_ce1_n,
    output wire         cram0_oe_n, cram1_oe_n,
    output wire         cram0_we_n, cram1_we_n,
    output wire         cram0_ub_n, cram0_lb_n, cram1_ub_n, cram1_lb_n
);
assign cram0_a = o_a;            assign cram1_a = o_a;
assign cram0_adv_n = o_adv_n;    assign cram1_adv_n = o_adv_n;
assign cram0_cre = o_cre;        assign cram1_cre = o_cre;
assign cram0_ce0_n = o_ce_n[0];  assign cram1_ce0_n = o_ce_n[0];
assign cram0_ce1_n = o_ce_n[1];  assign cram1_ce1_n = o_ce_n[1];
assign cram0_oe_n = o_oe_n;      assign cram1_oe_n = o_oe_n;
assign cram0_we_n = o_we_n;      assign cram1_we_n = o_we_n;
assign {cram0_ub_n, cram0_lb_n, cram1_ub_n, cram1_lb_n} = o_bm_n;
assign cram0_dq = o_dq_oe ? o_dq[31:16] : 16'bz;
assign cram1_dq = o_dq_oe ? o_dq[15:0]  : 16'bz;
assign cram0_clk = 1'b0;         assign cram1_clk = 1'b0;
always_ff @(posedge clk) i_dq <= {cram0_dq, cram1_dq};
endmodule
`default_nettype wire
