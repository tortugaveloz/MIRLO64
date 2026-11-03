// an_scandbl: the line doubler of the RGBHV mode.
//
// Input: the scan-out's video (rtl/soc/n64_video.sv) on the 25 MHz clk_vid, active-high hs / vs.
//  - woven = 0: a 240p field. A pixel each two clocks (skip marks the second clock of a pixel), lines of 790 (NTSC) or 800
//    (PAL) pixels = 1580 / 1600 clocks, 15.8 kHz, 264 / 313 lines a field. hs rises 656 pixels (1312 clocks) after the line's
//    first DE clock; vs rises 6 pixels before an hs.
//  - woven = 1: a 480-line frame of the same lines, a pixel a clock (skip stays 0), 31.6 kHz. `woven` is constant within
//    a frame (it changes in the vertical blanking).
// Output: VGA 640 x 480 timing, 25 MHz pixel clock, 31.6 kHz: the same clock, a pixel a clock.
//  - woven = 1: the input as it is -- the pixels (all 24 bits), de, the sync positions -- one clock later, hs / vs with the
//    output polarity (parameters), the colour forced to 0 outside DE. No scanlines (they are for the 240p input only).
//  - woven = 0: each input line comes out twice. The pixels of a line are written into a line buffer at the input rate (one
//    every two clocks) and read at one a clock, so the two copies are `OL` clocks long (790, PAL 800) and fill the input
//    line's 1580 / 1600 clocks (the lines of the field's vertical blanking that are shorter than the others by 4 clocks (NTSC),
//    16 (PAL), make the second copy as much shorter). 528 (PAL 626) lines a frame at the input's field rate: 59.94 Hz
//    (50.00 Hz) to the clock, locked to the input: nothing is counted that the input does not restart.
//
// Timing (clocks; E = the clock in which hs is first seen high, T0 = the first DE clock of the line):
//  - the line L is written at T0(L) + 2 i, pixel i (0..639); T0(L) = E(L-1) + 268 (PAL 288).
//  - the pair of output lines for L starts at E(L-1) + D (D = 924, PAL 944; `st`): the first copy reads pixel i at
//    E(L-1) + D + 1 + i, the second one OL clocks later. The read is 656 clocks after T0(L) at pixel 0, so it
//    follows the write with 17 clocks to spare (DLY_ADJ shifts D, for a margin test: -17 is the last value that reads right) and the second
//    copy is done well before the next line's write reaches the pixel (133 clocks).
//  - latency: the output pixel i is shown at T0(L) + 659 + i (and the second copy OL later); 26.4 us, 0.42 of an input line.
//    The woven path: 1 clock.
//  - the line is `act` (has a picture) if the input had DE in it; the output lines of a blank line have no DE.
//  - hs_o: 48 clocks from clock 656 of the output line, de_o: clocks 0..639, as the VGA timing of 640 x 480 has them.
//  - vs_o: the first output line of the pair after the input's vs rise starts it (at the clock of the line's hs), it lasts
//    VS_LINES output lines (the standard has 2; the Pocket's scan-out has 8 input lines, 16 output lines; the default 8 is
//    a compromise), after VS_DELAY lines. With the scan-out's own 240p field this puts vs_o's rise 2 output lines after
//    the last picture line (front porch 2) and the pulse is 8 lines long (back porch 38); VS_DELAY moves it later.
//  - the pair's lines per input line are counted from the input's hs rise, so a changed line length (NTSC <-> PAL) is
//    followed within one line; the TV system (`pal`) is taken from the length of the line just ended (>= 1582 clocks: PAL).
//    No state waits for an input event: the output counter wraps by itself, the delay counter saturates.
//
// Colour: the line buffer keeps the CB top bits of each channel (default 6: the Analogizer's DAC has six; a pixel is
// 18 bits, two M10K), and the output replicates them into the low bits (111111 -> 255). CB = 8 stores all 24 bits (3 M10K).
//
// scanlines (240p input only), on every second output line (the second copy of each input line): 0 off, 1 25 % dimmed
// (v - v/4), 2 50 % (v/2), 3 75 % (v/4), on the CB-bit value, truncating.
`default_nettype none

module an_scandbl #(
    parameter bit HS_ACTIVE_LOW = 1'b1,      // output polarities: the VGA 640 x 480 mode has both low
    parameter bit VS_ACTIVE_LOW = 1'b1,
    parameter int CB            = 6,         // bits of a channel kept in the line buffer (4..8)
    parameter int VS_LINES      = 8,         // vs_o's length in output lines (240p input), 1..40
    parameter int VS_DELAY      = 0,         // output lines from the first one of the pair to vs_o's rise, 0..20
    parameter int DLY_ADJ       = 0          // clocks added to D (a margin test only)
) (
    input  wire         clk,                 // clk_vid, 25 MHz
    input  wire         rst,
    input  wire         woven,               // 1: the input is a 480-line frame (pass through); 0: 240p, doubled
    input  wire  [23:0] rgb,
    input  wire         hs,                  // active high
    input  wire         vs,                  // active high
    input  wire         de,
    input  wire         skip,                // the second clock of a 240p pixel
    input  wire  [1:0]  scanlines,           // 0 off, 1 25 %, 2 50 %, 3 75 %
    output logic [23:0] rgb_o,
    output logic        hs_o,
    output logic        vs_o,
    output logic        de_o
);

localparam int W      = 3 * CB;
localparam int D_N    = 924, D_P = 944;          // see the header
localparam int PAL_LEN = 1582;                   // an input line of this many clocks or more is PAL
localparam int VS_END = VS_DELAY + VS_LINES;     // (vsc counts the output lines since the vs start up to this)
localparam int VW     = $clog2(VS_END + 1);      // vsc's width

// ---------------------------------------------------------------- input: edges, the line's length, the pair's start
logic        hs_q, vs_q;
logic [10:0] lcnt;                  // clocks since the last hs rise (saturating)
logic        pal;
wire         hs_rise = hs && !hs_q;
wire         vs_rise = vs && !vs_q;
wire  [10:0] dsel    = pal ? 11'(D_P + DLY_ADJ) : 11'(D_N + DLY_ADJ);
wire         st      = (lcnt == dsel);      // the pair of output lines starts (oc is 0 in the next clock)
logic        de_seen;               // the input line has had DE
logic        vs_pend;               // an input vs rise waits for the next pair
always_ff @(posedge clk) begin
    hs_q <= hs; vs_q <= vs;
    if (hs_rise) lcnt <= 11'd1;
    else if (lcnt != 11'h7FF) lcnt <= lcnt + 11'd1;
    if (hs_rise) pal <= (lcnt >= 11'(PAL_LEN));
    if (de) de_seen <= 1'b1;
    else if (hs_rise) de_seen <= 1'b0;
    if (vs_rise && !woven) vs_pend <= 1'b1;
    else if ((st && vs_pend) || woven) vs_pend <= 1'b0;
    if (rst) begin
        hs_q <= 1'b0; vs_q <= 1'b0; lcnt <= 11'd0; pal <= 1'b0; de_seen <= 1'b0; vs_pend <= 1'b0;
    end
end

// ---------------------------------------------------------------- the line buffer: written at the input, read at oc
logic [9:0]   wp;                   // the pixel being written
wire          we = de && !skip;
wire  [W-1:0] wd = {rgb[23 -: CB], rgb[15 -: CB], rgb[7 -: CB]};
always_ff @(posedge clk) begin
    if (!de) wp <= 10'd0;
    else if (!skip) wp <= wp + 10'd1;
end
(* ramstyle = "M10K, no_rw_check" *) logic [W-1:0] lb [0:1023];
logic [W-1:0] q;                    // the pixel read at the last clock's oc
logic [9:0]   oc;                   // the clock of the output line (the read address)
always_ff @(posedge clk) begin
    if (we) lb[wp] <= wd;
    q <= lb[oc];
end

// ---------------------------------------------------------------- output: the line's clock, the copy, the sync
logic       pass;                   // 0: the first copy of the input line, 1: the second
logic       act;                    // the input line has a picture
wire  [9:0] ocmax = pal ? 10'd799 : 10'd789;
always_ff @(posedge clk) begin
    if (st) begin oc <= 10'd0; pass <= 1'b0; act <= de_seen; end
    else if (oc == ocmax) begin oc <= 10'd0; pass <= !pass; end
    else oc <= oc + 10'd1;               // (a changed ocmax can leave oc above it: it wraps at 1023)
    if (rst) begin oc <= 10'd0; pass <= 1'b0; act <= 1'b0; end
end

// vs: vsc counts the output lines since the pair that vs_pend started (stops at VS_END)
logic [VW-1:0] vsc;
logic          vs_f;
wire           l655 = (oc == 10'd655);
always_ff @(posedge clk) begin
    if (st && vs_pend) vsc <= '0;
    else if (l655 && vsc != VW'(VS_END)) vsc <= vsc + 1'b1;
    if (l655) vs_f <= (vsc >= VW'(VS_DELAY)) && (vsc < VW'(VS_END));
    if (rst) begin vsc <= VW'(VS_END); vs_f <= 1'b0; end
end

// stage 1 (the RAM's data is q): the sync and the copy, a clock behind oc
logic de1, hs1, vs1, pass1;
always_ff @(posedge clk) begin
    de1 <= act && oc < 10'd640;
    hs1 <= oc >= 10'd656 && oc < 10'd704;
    vs1 <= vs_f;
    pass1 <= pass;
end

// scanline dimming of a CB-bit channel
function automatic logic [CB-1:0] dim(input logic [CB-1:0] v, input logic [1:0] m);
    case (m)
    2'd1:    return v - (v >> 2);
    2'd2:    return v >> 1;
    2'd3:    return v >> 2;
    default: return v;
    endcase
endfunction
// the CB-bit value as eight bits, the top bits repeated below
function automatic logic [7:0] ex(input logic [CB-1:0] c);
    logic [15:0] t;
    t = 16'({c, c});
    return t[2*CB-1 -: 8];
endfunction
wire [1:0]    dm = pass1 ? scanlines : 2'd0;
wire [CB-1:0] qr = q[3*CB-1 -: CB], qg = q[2*CB-1 -: CB], qb = q[CB-1:0];

// stage 2: the pads
always_ff @(posedge clk) begin
    if (woven) begin
        rgb_o <= de ? rgb : 24'd0;
        de_o  <= de;
        hs_o  <= hs ^ HS_ACTIVE_LOW;
        vs_o  <= vs ^ VS_ACTIVE_LOW;
    end else begin
        rgb_o <= de1 ? {ex(dim(qr, dm)), ex(dim(qg, dm)), ex(dim(qb, dm))} : 24'd0;
        de_o  <= de1;
        hs_o  <= hs1 ^ HS_ACTIVE_LOW;
        vs_o  <= vs1 ^ VS_ACTIVE_LOW;
    end
    if (rst) begin
        de_o <= 1'b0; hs_o <= HS_ACTIVE_LOW; vs_o <= VS_ACTIVE_LOW;
    end
end

endmodule
`default_nettype wire
