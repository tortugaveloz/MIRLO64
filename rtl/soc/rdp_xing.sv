// The RDP on the game CPU's clock (RDP_OC): what crosses between clk_r (clk_cpu), where rdp_top runs, and clk (clk_sys), where
// the command source (rdp_dpc), the memory's port (port_rdp, the arbiter) and the status registers are. As rsp_xing.sv, the
// two clocks are outputs of one PLL, 3/2 apart: every crossing is a register straight to a register, timed at the closest
// pair of edges, with no synchronisers; nothing counts on the other side seeing a value for exactly one cycle. The RDP sees
// the interface it always had (the command words with a ready, the memory's request / write data / read data channels) and
// the SoC sees the one rdp_top gave it, so neither is changed:
//
//   commands    a FIFO of command words, clk writes and clk_r reads (the pointers count up and the other side samples them)
//   the memory  a FIFO of requests {we, word address} and a FIFO of write data, clk_r writes, clk reads: the requests are
//               given to port_rdp in order, a write's data from the second FIFO when its request is at the head, as
//               port_rdp has them from rdp_top. A read's word comes back through a third (clk writes, clk_r reads); the RDP
//               is let issue a request only while the words of its reads not yet taken would all fit in it, so that the
//               words it does not hold back are never lost (it has no way to stop them)
//   status      busy: a command in the FIFO, or one the RDP has and is not idle with (the RDP's own `cmd_valid || !idle`, taken
//               with the FIFO's read pointer in one register, so that a command is never in neither); idle, as the SoC reads
//               it, is also the memory's: nothing in the crossing or in port_rdp's queue. The DP interrupt (sync_ev) comes
//               out when its SYNC FULL has been seen and the writes before it have gone through the crossing and port_rdp's
//               queue, as the console's has the data in memory before the interrupt
`default_nettype none

module rdp_xing #(
    parameter int CA = 2,                   // commands: 2**CA words
    parameter int QA = 3,                   // memory requests: 2**QA
    parameter int WA = 3,                   // write data: 2**WA
    parameter int RA = 5                    // the words back: 2**RA (the reads in flight at most)
) (
    // ---- the RDP's side (clk_r)
    input  wire         clk_r, rst_r,
    output wire         c_valid,
    output wire  [31:0] c_data,
    input  wire         c_ready,
    input  wire         rm_cmd_valid, rm_cmd_we,
    input  wire  [24:2] rm_cmd_addr,
    output wire         rm_cmd_ready,
    input  wire         rm_wdata_valid,
    input  wire  [31:0] rm_wdata,
    input  wire  [3:0]  rm_wdata_we,
    output wire         rm_wdata_ready,
    output logic        rm_rdata_valid,
    output logic [31:0] rm_rdata,
    input  wire         r_idle, r_tmem_busy, r_sync_ev,
    input  wire  [15:0] r_sync_count,
    input  wire  [31:0] r_load_count,
    input  wire  [15:0] r_unknown_ops,
    // ---- clk's side
    input  wire         clk, rst,
    input  wire         cmd_valid,
    input  wire  [31:0] cmd_data,
    output wire         cmd_ready,
    output wire         m_cmd_valid, m_cmd_we,
    output wire  [24:2] m_cmd_addr,
    input  wire         m_cmd_ready,
    output wire         m_wdata_valid,
    output wire  [31:0] m_wdata,
    output wire  [3:0]  m_wdata_we,
    input  wire         m_wdata_ready,
    input  wire         m_rdata_valid,
    input  wire  [31:0] m_rdata,
    input  wire         q_empty,            // port_rdp's queue holds nothing
    output wire         idle,
    output logic        tmem_busy,
    output logic        sync_ev,            // one cycle
    output logic [15:0] sync_count,
    output logic [31:0] load_count,
    output logic [15:0] unknown_ops
);

localparam int CD = 1 << CA, QD = 1 << QA, WD = 1 << WA, RD = 1 << RA;

// ---------------------------------------------------------------- the commands: clk -> clk_r
logic [31:0]   cf [CD];
logic [CA:0]   cw;                          // clk: written
logic [CA:0]   cr_s;                        // clk: the read pointer as clk_r's register had it
logic [CA:0]   cr, cw_r;                    // clk_r: read; the write pointer as found
logic          crdy;
wire           cpush = cmd_valid && crdy;
wire  [CA:0]   cw_n = cw + (CA + 1)'(cpush);
always_ff @(posedge clk) begin
    if (cpush) cf[cw[CA-1:0]] <= cmd_data;
    if (rst) begin cw <= '0; crdy <= 1'b0; end
    else begin cw <= cw_n; crdy <= (cw_n - cr_s) < (CA + 1)'(CD - 1); end
end
assign cmd_ready = crdy;

logic          ch_v;
logic [31:0]   ch_d;
assign c_valid = ch_v;
assign c_data  = ch_d;
always_ff @(posedge clk_r) begin
    cw_r <= cw;
    if (rst_r) begin cr <= '0; ch_v <= 1'b0; end
    else if ((!ch_v || c_ready) && cr != cw_r) begin ch_d <= cf[cr[CA-1:0]]; cr <= cr + 1'b1; ch_v <= 1'b1; end
    else if (c_ready) ch_v <= 1'b0;
end

// ---------------------------------------------------------------- the memory's requests: clk_r -> clk
localparam int QW = 1 + 23;
logic [QW-1:0] qf [QD];
logic [QA:0]   qw;                          // clk_r
logic [QA:0]   qr_r;                        // clk_r: the clk side's read pointer
logic [RA:0]   rd_n;                        // clk_r: reads made
logic [RA:0]   rr_r;                        // clk_r: words taken back
logic [QA:0]   rp, qw_s;                    // clk
logic          qrdy;
wire           qpush = rm_cmd_valid && qrdy;
wire  [QA:0]   qw_n = qw + (QA + 1)'(qpush);
wire  [RA:0]   rd_nn = rd_n + (RA + 1)'(qpush && !rm_cmd_we);
always_ff @(posedge clk_r) begin
    if (qpush) qf[qw[QA-1:0]] <= {rm_cmd_we, rm_cmd_addr};
    if (rst_r) begin qw <= '0; qr_r <= '0; qrdy <= 1'b0; rd_n <= '0; end
    else begin
        qw <= qw_n; qr_r <= rp; rd_n <= rd_nn;
        qrdy <= (qw_n - qr_r) < (QA + 1)'(QD - 1) && (rd_nn - rr_r) < (RA + 1)'(RD - 1);
    end
end
assign rm_cmd_ready = qrdy;

logic          w_valid;
logic [QW-1:0] w_head;
assign m_cmd_valid = w_valid;
assign m_cmd_we    = w_head[QW-1];
assign m_cmd_addr  = w_head[QW-2:0];
wire           w_pop = w_valid && m_cmd_ready;
always_ff @(posedge clk) begin
    qw_s <= qw;
    if (rst) begin rp <= '0; w_valid <= 1'b0; end
    else begin
        if ((!w_valid || w_pop) && rp != qw_s) begin w_head <= qf[rp[QA-1:0]]; rp <= rp + 1'b1; w_valid <= 1'b1; end
        else if (w_pop) w_valid <= 1'b0;
    end
end

// ---------------------------------------------------------------- the write data: clk_r -> clk
logic [35:0]   wf [WD];
logic [WA:0]   ww;                          // clk_r
logic [WA:0]   wr_r;                        // clk_r: the clk side's read pointer
logic [WA:0]   wr, ww_s;                    // clk
logic          wrdy;
wire           wpush = rm_wdata_valid && wrdy;
wire  [WA:0]   ww_n = ww + (WA + 1)'(wpush);
always_ff @(posedge clk_r) begin
    if (wpush) wf[ww[WA-1:0]] <= {rm_wdata_we, rm_wdata};
    if (rst_r) begin ww <= '0; wr_r <= '0; wrdy <= 1'b0; end
    else begin ww <= ww_n; wr_r <= wr; wrdy <= (ww_n - wr_r) < (WA + 1)'(WD - 1); end
end
assign rm_wdata_ready = wrdy;

logic          wd_valid;
logic [35:0]   wd_head;
assign m_wdata_valid = wd_valid;
assign m_wdata       = wd_head[31:0];
assign m_wdata_we    = wd_head[35:32];
always_ff @(posedge clk) begin
    ww_s <= ww;
    if (rst) begin wr <= '0; wd_valid <= 1'b0; end
    else begin
        if ((!wd_valid || m_wdata_ready) && wr != ww_s) begin wd_head <= wf[wr[WA-1:0]]; wr <= wr + 1'b1; wd_valid <= 1'b1; end
        else if (m_wdata_ready) wd_valid <= 1'b0;
    end
end

// ---------------------------------------------------------------- the words back: clk -> clk_r
logic [31:0]   bf [RD];
logic [RA:0]   rw;                          // clk: words written
logic [RA:0]   rw_r;                        // clk_r: ... as it found it
always_ff @(posedge clk) begin
    if (m_rdata_valid) bf[rw[RA-1:0]] <= m_rdata;
    if (rst) rw <= '0; else if (m_rdata_valid) rw <= rw + 1'b1;
end
wire           b_av = rw_r != rr_r;
always_ff @(posedge clk_r) begin
    rw_r <= rw;
    if (rst_r) begin rr_r <= '0; rm_rdata_valid <= 1'b0; end
    else begin
        rm_rdata_valid <= b_av;
        if (b_av) rr_r <= rr_r + 1'b1;
    end
    if (b_av) rm_rdata <= bf[rr_r[RA-1:0]];
end

// ---------------------------------------------------------------- the status: clk_r -> clk
// {the command pointer, busy} are one register, so that what clk sees of them is of one cycle of the RDP's
logic [CA:0]   st_cr;
logic          st_busy, st_tm, st_ev_t;
logic          sev_t;                       // clk_r: turns over at each SYNC FULL
logic          busy_x, tm_x, sev_x;
always_ff @(posedge clk_r) begin
    st_cr <= cr; st_busy <= ch_v || !r_idle; st_tm <= r_tmem_busy;
    if (rst_r) sev_t <= 1'b0; else if (r_sync_ev) sev_t <= !sev_t;
end
logic          sev_seen, sev_pend;
wire           drained = !w_valid && rp == qw_s && !wd_valid && wr == ww_s && q_empty;
assign         idle = cw == cr_s && !busy_x && drained;     // (not a register: the cycle after a command is taken is not idle)
always_ff @(posedge clk) begin
    cr_s <= st_cr; busy_x <= st_busy; tm_x <= st_tm; sev_x <= sev_t;
    tmem_busy <= tm_x;
    sync_count <= r_sync_count; load_count <= r_load_count; unknown_ops <= r_unknown_ops;
    if (rst) begin sev_seen <= 1'b0; sev_pend <= 1'b0; sync_ev <= 1'b0; end
    else begin
        sync_ev <= 1'b0;
        if (sev_x != sev_seen) begin sev_seen <= sev_x; sev_pend <= 1'b1; end
        if (sev_pend && drained && sev_x == sev_seen) begin sync_ev <= 1'b1; sev_pend <= 1'b0; end
    end
end
endmodule
`default_nettype wire
