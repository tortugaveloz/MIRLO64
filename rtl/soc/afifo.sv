// A dual-clock FIFO: Gray-coded pointers through two-flop synchronisers, the
// data in a RAM (M10K) read show-ahead on the read clock: r_data is the head
// whenever !r_empty, r_en pops it. A word is readable only once its write
// pointer has crossed (two rclk edges), so the RAM's registered read never
// meets a word being written.
`default_nettype none
module afifo #(parameter int W = 32, parameter int AW = 9) (
    input  wire          wclk, wrst,
    input  wire          w_en,              // (no full check: the writer keeps the level in bounds)
    input  wire  [W-1:0] w_data,
    output logic [AW:0]  w_level,           // entries, as the write side sees them (registered: a cycle late)
    input  wire          rclk, rrst,
    input  wire          r_en,
    output logic [W-1:0] r_data,
    output logic         r_empty
);
logic [W-1:0] mem [1 << AW];
function automatic logic [AW:0] g(input logic [AW:0] b); return b ^ (b >> 1); endfunction
function automatic logic [AW:0] b(input logic [AW:0] gg);
    logic [AW:0] r; r[AW] = gg[AW];
    for (int i = AW - 1; i >= 0; i--) r[i] = r[i + 1] ^ gg[i];
    return r;
endfunction
// write side
logic [AW:0] wptr, wptr_g, rptr_g_w1, rptr_g_w2, rptr_w;
always_ff @(posedge wclk) begin
    if (wrst) begin wptr <= 0; wptr_g <= 0; rptr_g_w1 <= 0; rptr_g_w2 <= 0; rptr_w <= 0; w_level <= 0; end
    else begin
        if (w_en) wptr <= wptr + 1'b1;
        wptr_g <= g(w_en ? wptr + 1'b1 : wptr);
        rptr_g_w1 <= rptr_g; rptr_g_w2 <= rptr_g_w1;
        // the Gray -> binary chain and the subtraction each get a register:
        // a timing path of their own (they fed the SDRAM arbiter's grant)
        rptr_w <= b(rptr_g_w2);
        w_level <= wptr - rptr_w;
    end
end
always_ff @(posedge wclk) if (w_en) mem[wptr[AW-1:0]] <= w_data;
// read side
logic [AW:0] rptr, rptr_g, wptr_g_r1, wptr_g_r2;
wire  [AW:0] rptr_n = rptr + (r_en && !r_empty);
always_ff @(posedge rclk) begin
    if (rrst) begin rptr <= 0; rptr_g <= 0; wptr_g_r1 <= 0; wptr_g_r2 <= 0; end
    else begin
        rptr <= rptr_n;
        rptr_g <= g(rptr_n);
        wptr_g_r1 <= wptr_g; wptr_g_r2 <= wptr_g_r1;
    end
end
always_ff @(posedge rclk) r_data <= mem[rptr_n[AW-1:0]];
assign r_empty = rptr_g == wptr_g_r2;          // (the Gray codes of the two pointers are equal as the pointers are: no conversion back)
endmodule
`default_nettype wire
