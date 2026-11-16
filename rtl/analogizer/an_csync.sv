// Composite sync for the Analogizer, from the scan-out's hsync and vsync (both active HIGH pulses).
//
// The picture is a 240p (non-interlaced) signal: every field has the same number of lines, so there is no half line
// and no equalising pulse; the vertical interval is the usual non-interlaced one:
//
//  - a line outside the vertical interval carries one sync pulse, the hs pulse (sync tip low, or high if
//    CSYNC_ACTIVE_LOW is 0), with the width hs has (3.84 us at the scan-out's 12.5 MHz pixels);
//  - during the vertical interval the pulse is inverted: the output is the XOR of hs and the vertical level, so the
//    sync stays active for the whole line except the hs-wide gap (the serration), one gap each line, where the
//    line's pulse would have been. Those are the broad pulses of the standard's vertical interval, at the line
//    rate. A sync separator that integrates the signal (the CRT's vertical separator) sees the average go to the
//    sync level for the interval and fires, while a slicer that looks at the edges sees an edge each line at the hs
//    position, so the horizontal oscillator stays locked through the interval.
//
// The scan-out's vs rises and falls six pixels before an hs pulse, which would put a spurious 0.5 us gap at the
// start and the end of the interval. The vertical level is therefore taken where hs FALLS (the one point where the
// XOR does not change the output): the interval starts with the hs pulse of the line on which vs went high, whose
// pulse is stretched into the broad pulse, and every serration is exactly hs wide.
//
// The scan-out holds vs for 8 lines. A 240p source sends 3 lines of vertical sync (as the interlaced
// standards do): VS_LINES limits the interval to the first VS_LINES lines of vs (1 to 7), 0 follows vs for as long as it is
// high. A CRT's vertical separator fires in about the first line and a half of the interval, so the picture does not
// move with the length; the standard 3 is the one every set tolerates.
//
// Latency: csync_n is two clock cycles behind hs (one for hs_q, one for the output register). `vint` is the vertical level
// (high from the hs fall of the first interval line to the hs fall of the line that ends it), one cycle ahead of csync_n.
module an_csync #(
    parameter bit CSYNC_ACTIVE_LOW = 1'b1,      // 1: the sync tip is low (csync_n low); 0: the sync is a high pulse
    parameter int VS_LINES         = 3          // lines of the vertical interval (1..7); 0: as long as vs is high
) (
    input  wire  clk,
    input  wire  hs,                            // active high
    input  wire  vs,                            // active high
    output logic csync_n,                       // the composite sync (the polarity of CSYNC_ACTIVE_LOW)
    output logic vint                           // the vertical interval, for the encoder
);
localparam int LATENCY = 2;                     // clock cycles from hs to csync_n

logic       hs_q = 1'b0;
logic [2:0] nl   = 3'd0;                        // the lines of this vs counted so far
initial begin vint = 1'b0; csync_n = CSYNC_ACTIVE_LOW; end

always_ff @(posedge clk) begin
    hs_q <= hs;
    if (hs_q && !hs) begin                      // hs falls: vs is steady here, and the XOR below does not change
        if (!vs) begin
            nl <= 3'd0; vint <= 1'b0;
        end else if (VS_LINES == 0 || nl != 3'(VS_LINES)) begin
            if (nl != 3'd7) nl <= nl + 3'd1;
            vint <= 1'b1;
        end else begin
            vint <= 1'b0;
        end
    end
    csync_n <= CSYNC_ACTIVE_LOW ? !(hs_q ^ vint) : (hs_q ^ vint);
end
endmodule
