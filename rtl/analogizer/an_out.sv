// The Analogizer's video output: the scan-out's signal (rtl/soc/n64_video.sv) in the mode the setting chose, on the
// cartridge port's three banks as the adapter's DAC reads them (docs/analogizer.md has the pin map and the modes).
//
//   mode 1 RGBS            R G B, csync on the HS pin
//   mode 2 RGsB            R G B, csync on the VS pin (the DAC's SYNC, for sync on green)
//   mode 3 YPbPr           Pr Y Pb on R G B, csync on the VS pin
//   mode 4 / 5 Y/C         chroma on R, luma on G, 0 on B, csync on the HS pin (NTSC / PAL)
//   mode 6 RGBHV           the lines doubled (or a woven frame as it comes), hsync on the HS pin, vsync on the VS pin
//
// Everything is registered on clk (the 25 MHz clk_vid): the three banks are what the DAC takes on the next clock edge.
// Outside the picture (`de`) the colours are 0, and in the modes with a blanking input it is low. The colours of the RGB
// modes (1, 2) come through an_scandbl's pass-through, as RGBHV's do, one clock behind the scan-out's.
`default_nettype none
module an_out (
    input  wire         clk,
    input  wire         rst,
    input  wire  [2:0]  mode,           // 0 off, 1 RGBS, 2 RGsB, 3 YPbPr, 4 Y/C NTSC, 5 Y/C PAL, 6 RGBHV
    input  wire  [1:0]  scanlines,      // RGBHV: 0 off, 1..3 the odd lines 25, 50, 75 % darker
    input  wire         woven,          // the frame is a woven 480-line one (31 kHz): RGBHV passes it as it is
    input  wire  [23:0] rgb,            // the scan-out's pads: 8:8:8, hs and vs high pulses, de, skip
    input  wire         hs, vs, de, skip,
    output logic [7:0]  bank3,          // {R5..R0, HS pin, VS pin}
    output logic [7:0]  bank2,          // {B0, /BLANK, G5..G0}
    output logic [4:0]  bank1_b         // {B5..B1} (bank 1's bits 4:0; its bit 5 is the DAC's clock, the top level's)
);

// ---- the encoders: Y/C, and with it the YPbPr matrix and the composite sync (an_yc builds them: the modes never run together)
wire  [7:0]  yc_c, yc_y;
wire         yc_cs_n;
wire  [7:0]  pr, yy, pb;
wire         csync_n;               // the composite sync, two clocks behind hs, as the YPbPr matrix is behind the colour
an_yc yc (.clk(clk), .rst(rst), .pal(mode == 3'd5), .hs(hs), .vs(vs), .de(de), .rgb(rgb),
          .c(yc_c), .y(yc_y), .csync_n(yc_cs_n), .m_pr(pr), .m_y(yy), .m_pb(pb), .m_csync_n(csync_n));
wire         cs_ypbpr = csync_n;

// The colour of the RGB modes and RGBHV: an_scandbl, which doubles the lines in RGBHV (mode 6) and passes the scan-out's
// picture on a clock later otherwise (its `woven` input is high), with the colour 0 outside the picture
wire  [23:0] sd_rgb;
wire         sd_hs_n, sd_vs_n, sd_de;
an_scandbl scandbl (.clk(clk), .rst(rst), .woven(woven || mode != 3'd6), .rgb(rgb), .hs(hs), .vs(vs), .de(de), .skip(skip),
                    .scanlines(scanlines), .rgb_o(sd_rgb), .hs_o(sd_hs_n), .vs_o(sd_vs_n), .de_o(sd_de));

// ---- the pins, by mode
logic [5:0] r6, g6, b6;
logic       hs_pin, vs_pin, blank_n;
always_comb begin
    r6 = sd_rgb[23:18]; g6 = sd_rgb[15:10]; b6 = sd_rgb[7:2];
    hs_pin = 1'b1; vs_pin = 1'b1; blank_n = sd_de;
    case (mode)
    3'd1: begin hs_pin = csync_n; end
    3'd2: begin vs_pin = csync_n; end
    3'd3: begin r6 = pr[7:2]; g6 = yy[7:2]; b6 = pb[7:2]; vs_pin = cs_ypbpr; blank_n = 1'b1; end
    3'd4, 3'd5: begin r6 = yc_c[7:2]; g6 = yc_y[7:2]; b6 = 6'd0; hs_pin = yc_cs_n; blank_n = 1'b1; end
    3'd6: begin hs_pin = sd_hs_n; vs_pin = sd_vs_n; blank_n = 1'b1; end
    default: begin r6 = 6'd0; g6 = 6'd0; b6 = 6'd0; blank_n = 1'b0; end
    endcase
end

always_ff @(posedge clk) begin
    bank3   <= {r6, hs_pin, vs_pin};
    bank2   <= {b6[0], blank_n, g6};
    bank1_b <= b6[5:1];
end

endmodule
