// The RSP's clock (RSP_OC, docs/mips.md): clk_c, the game CPU's 93.75 MHz, every edge of it but those the RSP waits out while a
// write it made to the DPC's registers goes across (rsp_xing.sv: dpa_t answers), so that its next read of DPC_STATUS finds the
// write there, as on the console. `strobe` is the RSP's DPC write, in the cycle its edge takes it: the next edge is the first
// to be left out.
// The enable is one register of clk_c straight to the gate: the timing analyser does not time the path into a clock control
// block's enable (checked: a path of several multipliers shows no slack), so it must be short by construction.
// During reset every edge is let by.
`default_nettype none

module rsp_clk (
    input  wire       clk_c, rst_c,
    input  wire       strobe,               // the RSP's clock: it writes a DPC register (any of the three) at its next edge
    input  wire       dpa_t,                // clk_sys: the writes made are in the DPC (turns over once for each)
    output wire       gclk
);
logic       ena_q, dq_c;
wire        dq_n = dq_c ^ (strobe && ena_q);        // (an edge the RSP takes: ena_q is the one that let it by)
always_ff @(posedge clk_c) begin
    dq_c <= !rst_c && dq_n;
    ena_q <= rst_c || dq_n == dpa_t;
end
rsp_gate gate (.clk_c, .ena(ena_q), .gclk);
endmodule
`default_nettype wire
