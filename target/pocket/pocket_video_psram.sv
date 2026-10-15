// Pocket display-only burst controller, derived from upstream a92f021's
// rtl/soc/psram.sv. The two 16-bit PSRAMs form a 32-bit RGB frame store on
// die 0. Die 1 remains deselected; there is no game/SoC port.
// Consecutive commands form a burst until z_last, a gap, or a 128-word row
// boundary. Fixed-latency bursts never cross a chip row or rely on WAIT.
//
// The chips' clock is gated (low outside a burst) and rises three quarters
// into clk's cycle, out of a DDR register on the doubled clock. What this
// drives at a rising edge of clk a chip takes 11.9 ns later and has 4 ns
// more of (tSP 3 ns, tHD 2 ns). What a chip puts out after its edge (tACLK
// <= 9 ns, held tKOH 2 ns past its next) comes back behind the clock's own
// way out to the pad, 3.8 ns: the pads' registers take it at clk's rising
// edge a period and a quarter after the chip's, the middle of the time it
// is there (some 3 ns either side: the timing analyser's figures, with
// projects/mirlo.sdc). A quarter of a period less and it is not there yet.
//
// A burst, in cycles of clk from the one its address goes out in (the
// CellularRAM's fixed latency, code 4 at up to 66 MHz: BCR 0x641F):
//   0        CE#, ADV# low, the address, WE# low for a write
//   1        ADV# high (a read: the bus released; 2: OE# low)
//   L - 1    the run's first command taken; then one a cycle
//   a write  L = 5 + (cfg[1:0] - 1): word i's data and lane enables in cycle
//            L + i
//   a read   L = 4 + (cfg[3:2] - 1): word i's data back in cycle L + i + 2
//   then CE# high for two cycles
// The display uses cfg=0101 (write latency 5, read latency 4). A read
// whose data cycle finds WAIT asserted sets wait_err.
//
// At reset, wait for power-up, then write die 0's burst-control and
// refresh registers with CRE high. No asynchronous data accesses are exposed.
`default_nettype none

module pocket_video_psram #(
    parameter int T_INIT = 12000            // clk cycles before the first access (150 us)
) (
    input  wire         clk,
    input  wire         rst,
    // ---- die 0: display frames (runs of words)
    input  wire         z_valid,
    output logic        z_ready,
    input  wire         z_we,
    input  wire  [21:0] z_addr,             // word
    input  wire  [31:0] z_wdata,
    input  wire  [3:0]  z_wmask,            // [3] the first byte, bits 31:24
    input  wire         z_last,
    output logic        z_rvalid,
    output wire  [31:0] z_rdata,
    output wire         z_idle,
    // ---- settings, state
    input  wire  [3:0]  cfg,                // [1:0] a write's latency adjustment + 1, [3:2] a read's
    input  wire         cfg_wr,             // (written: wait_err is cleared)
    output logic        ready,              // the chips are set up
    output logic        wait_err,
    // ---- the pads' registers (psram_phy)
    output logic [5:0]  o_a,                // A[21:16]
    output logic [31:0] o_dq,               // {cram0, cram1}
    output logic        o_dq_oe,
    output logic        o_adv_n, o_oe_n, o_we_n, o_cre,
    output logic [1:0]  o_ce_n,             // [0] die 0, [1] die 1
    output logic [3:0]  o_bm_n,             // {cram0 UB#, LB#, cram1 UB#, LB#}
    output logic        o_clk_en,           // the chips' clock rises in this cycle
    input  wire  [31:0] i_dq,               // the pads at the last rising edge
    input  wire  [1:0]  i_wait
);
localparam logic [15:0] BCR_BURST = 16'h641F;   // synchronous, fixed latency code 4, WAIT high during the delay, no wrap, continuous
localparam logic [15:0] RCR_ALL   = 16'h0010;   // the whole array refreshed, no deep power-down (the power-up value)
localparam logic [5:0]  A_BCR     = 6'b001000;  // A[19:18] = 10 (the RCR: 00)

typedef enum logic [3:0] { S_INIT, S_IDLE, S_BURST, S_RE1, S_GAP, S_ASYNC, S_AEND } st_t;
st_t         st;
logic [14:0] t;                             // the cycle within an access; the wait at reset
logic [2:0]  ph;                            // a burst's cycle, up to 7
logic        dir, stop;                     // a burst: a write; nothing more is taken
logic [6:0]  nxt;                           // a burst: the next word, in its row
logic        cfg_n;                         // die 0's BCR, then RCR
wire  [15:0] cfg_val = cfg_n ? RCR_ALL : BCR_BURST;
logic [2:0]  rsr;                           // a read word's data is back 3 cycles after its command
wire  [2:0]  LW = 3'd4 + {1'b0, cfg[1:0]};  // 5 with cfg[1:0] = 1
wire  [2:0]  LR = 3'd3 + {1'b0, cfg[3:2]};  // 4 with cfg[3:2] = 1
wire  [2:0]  L  = dir ? LW : LR;
wire         run  = ph >= L - 3'd1;         // the latency is over: a word a cycle
wire         slot = st == S_BURST && run && !stop;                      // a command may be taken this cycle
wire         take = slot && z_valid && z_we == dir;
assign z_ready = take;
assign z_rdata = i_dq;
assign z_rvalid = rsr[2];
assign z_idle = st != S_BURST && st != S_RE1 && rsr == 3'd0;

logic n_ce;
always_ff @(posedge clk) o_clk_en <= n_ce;

always_comb begin
    // the chips' clock through the cycle that follows: a burst's latency, and while its words go
    n_ce = 1'b0;
    if (st == S_IDLE && ready && z_valid) n_ce = 1'b1;
    if (st == S_BURST) n_ce = !run || take;
end

always_ff @(posedge clk) begin
    rsr <= {rsr[1:0], take && !dir};
    if (cfg_wr) wait_err <= 1'b0;
    else if (rsr[2] && i_wait != 2'b00) wait_err <= 1'b1;
    if (rst) begin
        st <= S_INIT; t <= '0; ready <= 1'b0; wait_err <= 1'b0; rsr <= '0; cfg_n <= 1'b0;
        o_ce_n <= 2'b11; o_adv_n <= 1'b1; o_oe_n <= 1'b1; o_we_n <= 1'b1; o_cre <= 1'b0; o_dq_oe <= 1'b0;
        o_bm_n <= 4'h0; o_a <= '0; o_dq <= '0;
    end else case (st)
    // ---- reset: the chips' power-up time, then die 0's BCR/RCR
    S_INIT: begin
        t <= t + 15'd1;
        if (t == 15'(T_INIT)) begin
            o_ce_n <= 2'b10; o_adv_n <= 1'b0; o_cre <= 1'b1;
            o_a <= cfg_n ? 6'd0 : A_BCR; o_dq <= {2{cfg_val}}; o_dq_oe <= 1'b1;
            t <= 15'd0; st <= S_ASYNC;
        end
    end
    S_IDLE: begin
        t <= 15'd0; ph <= 3'd0;
        o_bm_n <= 4'h0;
        if (!ready) begin st <= S_INIT; t <= 15'(T_INIT); end
        else if (z_valid) begin
            dir <= z_we; nxt <= z_addr[6:0]; stop <= 1'b0;
            o_ce_n <= 2'b10; o_adv_n <= 1'b0; o_we_n <= !z_we;
            o_a <= z_addr[21:16]; o_dq <= {2{z_addr[15:0]}}; o_dq_oe <= 1'b1;
            st <= S_BURST;
        end
    end
    // ---- a burst
    S_BURST: begin
        if (ph != 3'd7) ph <= ph + 3'd1;
        o_adv_n <= 1'b1; o_we_n <= 1'b1;
        if (!dir) begin
            o_dq_oe <= 1'b0;
            if (ph != 3'd0) o_oe_n <= 1'b0;
        end
        if (take) begin
            o_dq <= z_wdata; o_bm_n <= ~z_wmask;
            nxt <= nxt + 7'd1;
            if (z_last || nxt == 7'd127) stop <= 1'b1;
        end else if (run) begin                        // nothing more: the clock has stopped
            if (dir) begin o_ce_n <= 2'b11; o_dq_oe <= 1'b0; t <= 15'd0; st <= S_GAP; end
            else st <= S_RE1;                          // (a read's last word is still on its way in)
        end
    end
    S_RE1: begin o_ce_n <= 2'b11; o_oe_n <= 1'b1; t <= 15'd0; st <= S_GAP; end
    S_GAP: begin t <= t + 15'd1; if (t[0]) st <= S_IDLE; end
    // ---- configuration-register write: address/value held with CRE high
    S_ASYNC: begin
        t <= t + 15'd1;
        o_adv_n <= 1'b1;
        if (t == 15'd1) o_we_n <= 1'b0;
        if (t == 15'd5) begin o_we_n <= 1'b1; st <= S_AEND; end
    end
    S_AEND: begin
        o_ce_n <= 2'b11; o_dq_oe <= 1'b0; o_cre <= 1'b0; o_bm_n <= 4'h0;
        cfg_n <= 1'b1;
        if (cfg_n) ready <= 1'b1;
        st <= S_IDLE;
    end
    default: st <= S_IDLE;
    endcase
end

endmodule

// The pads: every output a register at its pad (the project's fast output /
// input register assignments on cram*), the bus's two directions, the clock
// from a DDR register on the doubled clock: high from three quarters into a
// cycle of clk in which clk_en is set to a quarter into the next.
module pocket_video_psram_phy (
    input  wire         clk,
    input  wire         clk2x,              // clk doubled, their rising edges together
    input  wire  [5:0]  o_a,
    input  wire  [31:0] o_dq,
    input  wire         o_dq_oe, o_adv_n, o_oe_n, o_we_n, o_cre,
    input  wire  [1:0]  o_ce_n,
    input  wire  [3:0]  o_bm_n,
    input  wire         clk_en,
    output logic [31:0] i_dq,
    output logic [1:0]  i_wait,
    output wire  [21:16] cram0_a, cram1_a,
    inout  wire  [15:0] cram0_dq, cram1_dq,
    input  wire         cram0_wait, cram1_wait,
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
always_ff @(posedge clk) begin i_dq <= {cram0_dq, cram1_dq}; i_wait <= {cram0_wait, cram1_wait}; end
// The half of clk's cycle, as the doubled clock's edges see it: tg_d is tg
// from the middle of a cycle to its end. The DDR register's two halves: at
// the edge in the middle of a cycle, low then (clk_en) high; at the edge that
// ends it, (the clk_en of the cycle just over) high then low.
logic tg, tg_d;
always_ff @(posedge clk)   tg   <= ~tg;
always_ff @(posedge clk2x) tg_d <= tg;
wire second = tg_d == tg;
wire ck_h = clk_en && second;
wire ck_l = clk_en && !second;
`ifdef VERILATOR
logic q_h, q_l, q_ln;
initial tg = 1'b0;
always_ff @(posedge clk2x) begin q_h <= ck_h; q_l <= ck_l; end
always_ff @(negedge clk2x) q_ln <= q_l;
assign cram0_clk = clk2x ? q_h : q_ln;
assign cram1_clk = cram0_clk;
`else
ALTDDIO_OUT #(.WIDTH(2)) clk_out (.datain_h({2{ck_h}}), .datain_l({2{ck_l}}), .outclock(clk2x), .dataout({cram1_clk, cram0_clk}));
`endif
endmodule
`default_nettype wire
