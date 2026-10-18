// N requesters onto one sdr_ctrl. Round-robin; a grant is kept while its
// port goes on asking, up to MAXB words (a line read, a burst: row hits), and
// an `urgent` port (the SoC's: the CPU and the RSP's DMA, which wait for each
// word they ask for, and the scan-out while its FIFO is low) goes first,
// round-robin among the urgent ones -- bounded
// by what it asks for, so it cannot starve anyone for long. The chosen request
// is registered before it reaches the controller.
//
// `la_*`: the request of the port that is next after the one being served
// (registered: a cycle old). The controller opens its row meanwhile, where
// that costs the one being served nothing.
//
// Read data comes back to every port with rd_valid[i] for the one that asked,
// in the order each port asked.
`default_nettype none

module sdr_arb #(
    parameter int N    = 5,
    parameter int MAXB = 16
) (
    input  wire              clk,
    input  wire              rst,
    input  wire  [N-1:0]     p_valid,
    input  wire  [N-1:0]     p_we,
    input  wire  [N-1:0][23:0] p_addr,
    input  wire  [N-1:0][31:0] p_wdata,
    input  wire  [N-1:0][3:0]  p_be,
    input  wire  [N-1:0]     p_urgent,
    output logic [N-1:0]     p_ready,
    output logic [N-1:0]     p_rvalid,
    output logic [31:0]      p_rdata,
    // to sdr_ctrl
    output logic             req_valid,
    output logic             req_we,
    output logic [23:0]      req_addr,
    output logic [31:0]      req_wdata,
    output logic [3:0]       req_be,
    output logic [2:0]       req_tag,
    input  wire              req_ready,
    output logic             la_valid,
    output logic [23:0]      la_addr,
    input  wire              rd_valid,
    input  wire  [31:0]      rd_data,
    input  wire  [2:0]       rd_tag
);

localparam int IW = $clog2(N);
logic [IW-1:0] cur;             // the port holding the grant
logic          held;            // cur keeps it (it asked last cycle and got served)
logic [4:0]    beats;

// the first port of v round the ring from cur + 1 (cur itself the last): {found, the port; cur when none}.
// Two priority chains by the port's place against cur -- those above it, then those up to it -- and no modulo.
function automatic logic [IW:0] ring(input logic [N-1:0] v, input logic [IW-1:0] c);
    logic f; logic [IW-1:0] p;
    f = 0; p = c;
    for (int i = N - 1; i >= 0; i--) if (v[i] && IW'(i) <= c) begin p = IW'(i); f = 1; end
    for (int i = N - 1; i >= 0; i--) if (v[i] && IW'(i) > c)  begin p = IW'(i); f = 1; end
    return {f, p};
endfunction
// the next port: cur if held and still asking, else urgent first, else round-robin after cur
logic [IW-1:0] pick, pu, pn;
logic          any, fu;
logic [IW:0]   rn;
always_comb begin
    any = |p_valid;
    {fu, pu} = ring(p_valid & p_urgent, cur);
    rn = ring(p_valid, cur);
    pn = rn[IW-1:0];
    if (held && p_valid[cur] && beats < 5'(MAXB)) pick = cur;
    else pick = fu ? pu : pn;
end
// the next other port: as above, cur left out
logic [IW-1:0] ou, on;
logic          fou, fon;
logic [N-1:0]  p_oth;
always_comb begin
    p_oth = p_valid;
    p_oth[cur] = 0;
    {fou, ou} = ring(p_oth & p_urgent, cur);
    {fon, on} = ring(p_oth, cur);
end
always_ff @(posedge clk) begin
    la_valid <= !rst && fon;
    la_addr  <= p_addr[fou ? ou : on];
end

// the register stage
wire load = any && (!req_valid || req_ready);
always_comb begin
    p_ready = '0;
    if (load) p_ready[pick] = 1;
end
always_ff @(posedge clk) begin
    if (rst) begin req_valid <= 0; cur <= '0; held <= 0; beats <= '0; end
    else begin
        if (req_valid && req_ready) req_valid <= 0;
        if (load) begin
            req_valid <= 1; req_we <= p_we[pick]; req_addr <= p_addr[pick];
            req_wdata <= p_wdata[pick]; req_be <= p_be[pick]; req_tag <= 3'(pick);
            beats <= (held && pick == cur) ? beats + 5'd1 : 5'd1;
            cur <= pick; held <= 1;
        end else if (!p_valid[cur]) held <= 0;
    end
end

always_comb begin
    p_rvalid = '0;
    if (rd_valid) p_rvalid[rd_tag[IW-1:0]] = 1;
end
assign p_rdata = rd_data;

endmodule
`default_nettype wire
