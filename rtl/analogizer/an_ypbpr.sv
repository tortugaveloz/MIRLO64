// RGB to Y, Pb, Pr for the Analogizer's component video: ITU-R BT.601, studio levels, 8 bits.
//
//   Y  = 16  + ( 66 R + 129 G +  25 B) / 256            16 .. 235
//   Pb = 128 + (-38 R -  74 G + 112 B) / 256            16 .. 240, 128 for grey
//   Pr = 128 + (112 R -  94 G -  18 B) / 256            16 .. 240, 128 for grey
//
// (the standard's 65.481, 128.553, 24.966 ... / 255 coefficients rounded to eight bits of fraction; the result is
// within 1 LSB of the exact matrix for every one of the 2^24 colours). R, G and B are the full-range 0..255 values
// of rgb = {R, G, B}. Blanking (de low) is black: Y = 16, Pb = Pr = 128. The products are multiplications by constants, which the
// synthesis puts in multiplier blocks (four DSP blocks on a Cyclone V).
//
// Latency: LATENCY clock cycles, fixed: the outputs of cycle n + LATENCY are the inputs of cycle n.
module an_ypbpr (
    input  wire         clk,
    input  wire  [23:0] rgb,        // {R, G, B}
    input  wire         de,         // the pixel is part of the picture (else blanking: black)
    output logic [7:0]  pr,
    output logic [7:0]  y,
    output logic [7:0]  pb
);
localparam int LATENCY = 2;

wire signed [8:0] r = {1'b0, rgb[23:16]}, g = {1'b0, rgb[15:8]}, b = {1'b0, rgb[7:0]};

// stage 1: the three sums (the +128 rounds the shift of stage 2). The products by the constants are written as
// multiplications: the synthesis builds them in multiplier blocks (4), with the adders, and that is smaller than the shifts
// written out. Blanking clears the sums: stage 2 then gives Y = 16, Pb = Pr = 128 with no further logic.
logic signed [16:0] ys, cbs, crs;               // ys 128 .. 56,128; cbs, crs -28,304 .. 28,816
always_ff @(posedge clk) begin
    if (!de) begin
        ys <= 17'sd0; cbs <= 17'sd0; crs <= 17'sd0;
    end else begin
        ys  <= r * 9'sd66  + g * 9'sd129 + b * 9'sd25  + 17'sd128;
        cbs <= b * 9'sd112 - r * 9'sd38  - g * 9'sd74  + 17'sd128;
        crs <= r * 9'sd112 - g * 9'sd94  - b * 9'sd18  + 17'sd128;
    end
end

// stage 2: / 256 and the offsets (the signed sums' bits 15:8 are the 8-bit two's complement, + 128 flips the top bit)
always_ff @(posedge clk) begin
    y  <= 8'd16 + ys[15:8];
    pb <= {~cbs[15], cbs[14:8]};
    pr <= {~crs[15], crs[14:8]};
end
endmodule
