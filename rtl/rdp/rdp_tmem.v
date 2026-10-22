// TMEM for the texture filter's three texels (rdp_pix reads three texels a
// pixel; rdp_mem's loads write every copy alike): two copies, each in true
// dual port memories. TMEM is written only by loads, and a load runs only
// with the pixel pipeline empty (rdp_top's S_LOAD waits for all_quiet), so a
// copy's port A is the loads' write port while one runs and a read port while
// drawing: copy 0 gives texels 0 (port A) and 1 (port B). Copy 1 gives texel
// 2 by port B, its port A only writes (RA 0: no address choice in front of
// it). Three copies of simple dual port memories took 15 M10K, these 10.
//
// TMEM is the N64's 4 KiB as 2048 16-bit words (lang/c/mrdp/mrdp.c,
// MRDP_N64): four memories of 512, {half, lane} -- the lower and upper 2 KiB,
// the even and odd 16-bit words. A read at 16-bit word `ra` gives that word,
// and with it the same word of the upper half (a 32-bit texel's B and A).
// The TLUT's entries -- the 16-bit words 4 * index of the upper half -- are
// kept a second time in a memory of their own, for the lookup's read.
module rdp_tmem #(
    parameter RA = 1              // port A reads too (else only port B reads)
) (
    input  wire        clk,
    // write (loads): the memories enabled, {upper lane 1, upper lane 0, lower lane 1, lower lane 0}
    input  wire [3:0]  we,
    input  wire [8:0]  wa,
    input  wire [15:0] wd0, wd1, wd2, wd3,
    // reads, port A (RA; not while a load writes) and port B: the data one cycle after the address
    input  wire [10:0] ra_a, ra_b,
    output wire [15:0] q_a, q_b,        // word ra
    output wire [15:0] qh_a, qh_b,      // the upper half's word at ra[9:0]
    input  wire [7:0]  ta_a, ta_b,
    output wire [15:0] tq_a, tq_b       // TLUT entry ta
);
    wire        ld = we != 4'd0;
    wire [8:0]  aa = (ld || !RA) ? wa : ra_a[9:1];
    wire [7:0]  ta = (ld || !RA) ? wa[8:1] : ta_a;
    wire [15:0] a0, a1, a2, a3, b0, b1, b2, b3;
    rdp_tdpram #(.AW(9)) m0 (.clk(clk), .we(we[0]), .addr_a(aa), .wdata(wd0), .q_a(a0), .addr_b(ra_b[9:1]), .q_b(b0));
    rdp_tdpram #(.AW(9)) m1 (.clk(clk), .we(we[1]), .addr_a(aa), .wdata(wd1), .q_a(a1), .addr_b(ra_b[9:1]), .q_b(b1));
    rdp_tdpram #(.AW(9)) m2 (.clk(clk), .we(we[2]), .addr_a(aa), .wdata(wd2), .q_a(a2), .addr_b(ra_b[9:1]), .q_b(b2));
    rdp_tdpram #(.AW(9)) m3 (.clk(clk), .we(we[3]), .addr_a(aa), .wdata(wd3), .q_a(a3), .addr_b(ra_b[9:1]), .q_b(b3));
    rdp_tdpram #(.AW(8)) tl (.clk(clk), .we(we[2] && !wa[0]), .addr_a(ta), .wdata(wd2), .q_a(tq_a), .addr_b(ta_b), .q_b(tq_b));
    reg half_a, lane_a, half_b, lane_b;
    always @(posedge clk) begin half_a <= ra_a[10]; lane_a <= ra_a[0]; half_b <= ra_b[10]; lane_b <= ra_b[0]; end
    assign q_a  = half_a ? (lane_a ? a3 : a2) : (lane_a ? a1 : a0);
    assign qh_a = lane_a ? a3 : a2;
    assign q_b  = half_b ? (lane_b ? b3 : b2) : (lane_b ? b1 : b0);
    assign qh_b = lane_b ? b3 : b2;
endmodule

// A 16-bit memory in M10K's true dual port mode: port A writes or reads,
// port B reads; the data one cycle after the address. What port A reads in
// the clock it writes is never used (no_rw_check)
module rdp_tdpram #(
    parameter AW = 9
) (
    input  wire          clk,
    input  wire          we,
    input  wire [AW-1:0] addr_a,
    input  wire [15:0]   wdata,
    output reg  [15:0]   q_a,
    input  wire [AW-1:0] addr_b,
    output reg  [15:0]   q_b
);
    (* ramstyle = "M10K, no_rw_check" *) reg [15:0] mem [0:(1<<AW)-1];
    always @(posedge clk) begin
        if (we) mem[addr_a] <= wdata;
        q_a <= mem[addr_a];
    end
    always @(posedge clk) q_b <= mem[addr_b];
endmodule
