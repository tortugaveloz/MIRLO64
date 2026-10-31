// The RSP's clock enable (RSP_OC, docs/mips.md): the RSP runs on clk_c, the game CPU's 93.75 MHz, with only duty_n of
// every duty_d edges let through (spread evenly: an accumulator, Bresenham's line). duty_n >= duty_d is every edge: the
// overclock. Anything less is the calibration, a rate near what the RSP did on clk_sys: the pipelined RSP takes more
// cycles for the same work, and the duty gives them back. The ratio is MONITOR.RSPDUTY, a level that the runtime sets
// while the RSP is idle (a pattern that changes under a running RSP would only change its speed).
//
// The RSP waits (no edge) while a write it made to the DPC's registers goes across (rsp_xing.sv: dpa_t answers), so
// that its next read of DPC_STATUS finds the write there, as on the console. `strobe` is the RSP's DPC write, in the
// cycle its edge takes it: the next edge is the first to be left out.
// The enable is one register of clk_c straight to the gate: the timing analyser does not time the path into a clock
// control block's enable (checked: a path of several multipliers shows no slack), so it must be short by construction.
// During reset every edge is let by.
`default_nettype none

module rsp_clk (
    input  wire       clk_c, rst_c,
    input  wire [3:0] duty_n, duty_d,       // clk_sys's registers, steady (see above)
    input  wire       strobe,               // the RSP's clock: it writes a DPC register (any of the three) at its next edge
    input  wire       dpa_t,                // clk_sys: the writes made are in the DPC (turns over once for each)
    output wire       gclk
);
logic [3:0] n_q, d_q;
logic [4:0] acc;
logic       ena_q, dq_c;
wire  [4:0] sum  = acc + {1'b0, n_q};
wire        hit  = sum >= {1'b0, d_q};
wire        dq_n = dq_c ^ (strobe && ena_q);        // (an edge the RSP takes: ena_q is the one that let it by)
always_ff @(posedge clk_c) begin
    n_q <= duty_n; d_q <= duty_d;
    dq_c <= !rst_c && dq_n;
    if (rst_c) begin acc <= '0; ena_q <= 1'b1; end
    else begin
        ena_q <= hit && dq_n == dpa_t;
        acc   <= hit ? sum - {1'b0, d_q} : sum;
    end
end
rsp_gate gate (.clk_c, .ena(ena_q), .gclk);
endmodule
`default_nettype wire
