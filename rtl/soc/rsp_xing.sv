// The RSP on the game CPU's clock (RSP_OC, docs/mips.md): what crosses between clk_r, the RSP's (clk_cpu with edges left
// out: rsp_clk.sv), and clk (clk_sys). The two clocks are outputs of one PLL, 3/2 apart: like cpu_port.sv, every
// crossing is a register straight to a register, at every edge, timed at the closest pair of edges (5.3 ns); no
// synchronisers. What makes it work is that nothing counts on the other side seeing a value for exactly one cycle:
//
//   a request   (host registers; DPC writes) a value, held, announced by turning a toggle over; answered by another
//   the DMA     a FIFO of word requests (clk_r writes, clk reads: pointers only count up and are sampled by the other
//               side, as cpu_port's) and a FIFO of the words that come back (clk writes, clk_r reads). The RSP is let
//               make a read only while the words of its reads not yet taken from the second FIFO would all fit in it,
//               so nothing waits for the RSP to take a word -- which it does not while it waits for a DPC write to go
//               across
//
// Ordering: the RSP's DMA writes go to the arbiter in the order it made them, before anything it did after (a DPC write
// is applied only when the FIFO is empty and the arbiter has the last one; the RSP waits meanwhile: rsp_clk.sv).
// The latency is the crossing's two sampling registers each way and the RSP's own enable pattern.
`default_nettype none

module rsp_xing #(
    parameter int QA  = 3,                  // the requests' FIFO: 2**QA words
    parameter int QL  = 6,                  // ... of which the RSP is let fill this many (the room it sees is a cycle or two old)
    parameter int RA  = 4                   // the words back: 2**RA
) (
    // ---- the RSP's side (clk_r)
    input  wire         clk_r, rst_r,
    input  wire         clk_c,              // clk_r with no edge left out: the DPC's registers, as the RSP reads them, are kept on it
    output logic        h_we, h_rd,         // rsp_top's host port
    output logic [3:0]  h_addr,
    output logic [31:0] h_wdata,
    input  wire  [31:0] h_rdata,
    input  wire         h_ready,
    input  wire         r_valid, r_we,      // rsp_top's DMA
    input  wire  [23:2] r_addr,
    input  wire  [31:0] r_wdata,
    output logic        r_ready, r_rvalid,
    output logic [31:0] r_rdata,
    input  wire         dpc_start_we, dpc_end_we, dpc_status_we,   // rsp_top's DPC strobes
    input  wire  [31:0] dpc_wdata,
    output logic [31:0] dpc_start_r, dpc_end_r, dpc_current_r, dpc_status_r,   // the DPC's registers as the RSP reads them
    output logic        dq_t,               // a DPC write was made (turns over): rsp_clk.sv holds the RSP's clock until it is in
    // ---- clk_sys's side
    input  wire         clk, rst,
    // the game CPU's access to the SP registers (mirlo_mips.sv's X_RSP): s_h_req is held, with the rest, until s_h_done
    input  wire         s_h_req, s_h_we,
    input  wire  [3:0]  s_h_addr,
    input  wire  [31:0] s_h_wdata,
    output wire         s_h_done,           // one cycle
    output wire  [31:0] s_h_rdata,          // from then on
    // the DMA, a port of the arbiter
    output wire         p_valid, p_we,
    output wire  [22:2] p_addr,
    output wire  [31:0] p_wdata,
    input  wire         p_ready, p_rvalid,
    input  wire  [31:0] p_rdata,
    // the RSP's writes to the DPC, in one cycle each (s_dp_go), as the CPU's are
    output logic        s_dp_go,
    output logic [2:0]  s_dp_k,             // START, END, STATUS
    output wire  [31:0] s_dp_wdata,
    output logic        dpa_t,              // ... and in, a little after (turns over)
    input  wire  [23:0] s_dpc_start, s_dpc_end, s_dpc_current,
    input  wire  [31:0] s_dpc_status
);

// ---------------------------------------------------------------- the host registers
// clk -> clk_r: the toggle, then the request in hq_* (read from the CPU's registers, which hold until the answer);
// clk_r -> clk: the answer's toggle, with the data in hr_q
logic        h_t;                           // clk: a request is out
logic        h_wait;
logic        ha_t;                          // clk_r: the answer
logic        ha_s;                          // clk: ha_t as the last edge found it
logic        ht_r, h_seen;                  // clk_r: h_t, and the one answered
logic        hq_we;
logic [31:0] hr_q;
wire         h_pend = ht_r != h_seen;
wire         h_op   = h_pend && (!hq_we || h_ready);
assign       h_we = h_pend && hq_we && h_ready;
assign       h_rd = h_pend && !hq_we;
always_ff @(posedge clk_r) begin
    ht_r <= h_t; hq_we <= s_h_we; h_addr <= s_h_addr; h_wdata <= s_h_wdata;
    if (rst_r) begin h_seen <= 1'b0; ha_t <= 1'b0; end
    else if (h_op) begin h_seen <= ht_r; ha_t <= ht_r; end
    if (h_op) hr_q <= h_rdata;
end
assign s_h_rdata = hr_q;
assign s_h_done  = h_wait && ha_s == h_t;
always_ff @(posedge clk) begin
    ha_s <= ha_t;
    if (rst) begin h_t <= 1'b0; h_wait <= 1'b0; end
    else if (s_h_done) h_wait <= 1'b0;
    else if (s_h_req && !h_wait) begin h_t <= !h_t; h_wait <= 1'b1; end
end

// ---------------------------------------------------------------- the DMA: requests
localparam int QW = 1 + 21 + 32;
logic [QW-1:0] qf [1 << QA];
logic [QA:0]   qw;                          // clk_r
logic [QA:0]   qr_r;                        // clk_r: the clk side's read pointer
logic [QA:0]   rp, qw_s;                    // clk
logic          rdy_q;
logic [RA:0]   rd_n;                        // clk_r: reads made
logic [RA:0]   rr_r;                        // clk_r: words taken back
wire           push = r_valid && rdy_q;
wire  [QA:0]   qw_n = qw + (QA + 1)'(push);
wire  [RA:0]   rd_nn = rd_n + (RA + 1)'(push && !r_we);
always_ff @(posedge clk_r) begin
    if (push) qf[qw[QA-1:0]] <= {r_we, r_addr[22:2], r_wdata};
    if (rst_r) begin qw <= '0; qr_r <= '0; rdy_q <= 1'b0; rd_n <= '0; end
    else begin
        qw <= qw_n; qr_r <= rp; rd_n <= rd_nn;
        rdy_q <= (qw_n - qr_r) < (QA + 1)'(QL) && (rd_nn - rr_r) < (RA + 1)'(1 << RA);
    end
end
assign r_ready = rdy_q;

logic          w_valid;
logic [QW-1:0] w_head;
wire           w_wr = w_head[QW-1];
assign p_valid = w_valid;
assign p_we    = w_wr;
assign p_addr  = w_head[52:32];
assign p_wdata = w_head[31:0];
wire           w_pop = w_valid && p_ready;
always_ff @(posedge clk) begin
    qw_s <= qw;
    if (rst) begin rp <= '0; w_valid <= 1'b0; end
    else begin
        if ((!w_valid || w_pop) && rp != qw_s) begin w_head <= qf[rp[QA-1:0]]; rp <= rp + 1'b1; w_valid <= 1'b1; end
        else if (w_pop) w_valid <= 1'b0;
    end
end

// ---------------------------------------------------------------- the DMA: the words back
logic [31:0]   bf [1 << RA];
logic [RA:0]   rw;                          // clk: words written
logic [RA:0]   rw_r;                        // clk_r: ... as it found it
always_ff @(posedge clk) begin
    if (p_rvalid) bf[rw[RA-1:0]] <= p_rdata;
    if (rst) rw <= '0; else if (p_rvalid) rw <= rw + 1'b1;
end
wire           b_av = rw_r != rr_r;
always_ff @(posedge clk_r) begin
    rw_r <= rw;
    if (rst_r) begin rr_r <= '0; r_rvalid <= 1'b0; end
    else begin
        r_rvalid <= b_av;
        if (b_av) rr_r <= rr_r + 1'b1;
    end
    if (b_av) r_rdata <= bf[rr_r[RA-1:0]];
end

// ---------------------------------------------------------------- the DPC
// A write is held in ds_* and announced by dq_t; it is applied when the requests' FIFO is empty and the arbiter has the
// last one (the RSP, held by rsp_clk.sv, has made no request since). dpa_t answers a little after the DPC's registers
// have it. The registers' values come the other way, one register on clk_c.
logic [2:0]  ds_k;
logic [31:0] ds_wd;
logic        dq_s, dp_pre, dp_d1;           // clk: dq_t as found; the apply's toggle; ... delayed
wire         dp_go = dq_s != dp_pre && !w_valid && rp == qw_s;
assign s_dp_wdata = ds_wd;
always_ff @(posedge clk_r) begin
    if (rst_r) dq_t <= 1'b0;
    else if (dpc_start_we || dpc_end_we || dpc_status_we) dq_t <= !dq_t;
    if (dpc_start_we || dpc_end_we || dpc_status_we) begin ds_k <= {dpc_status_we, dpc_end_we, dpc_start_we}; ds_wd <= dpc_wdata; end
end
// (the DPC's values are logic of clk_sys: a register of clk_sys first, so that the path to clk_c is register to register, and
// one cycle later than dpa_t's change at the least; on the whole clk_c: the RSP's wait for the write to come back ends with an
// edge that must find the registers new)
logic [23:0] dq_start, dq_end, dq_current;
logic [31:0] dq_status;
always_ff @(posedge clk) begin dq_start <= s_dpc_start; dq_end <= s_dpc_end; dq_current <= s_dpc_current; dq_status <= s_dpc_status; end
always_ff @(posedge clk_c) begin
    dpc_start_r <= {8'd0, dq_start}; dpc_end_r <= {8'd0, dq_end}; dpc_current_r <= {8'd0, dq_current}; dpc_status_r <= dq_status;
end
always_ff @(posedge clk) begin
    dq_s <= dq_t;
    s_dp_go <= !rst && dp_go;
    s_dp_k  <= ds_k;
    if (rst) begin dp_pre <= 1'b0; dp_d1 <= 1'b0; dpa_t <= 1'b0; end
    else begin
        if (dp_go) dp_pre <= !dp_pre;
        dp_d1 <= dp_pre; dpa_t <= dp_d1;
    end
end
endmodule
`default_nettype wire
