// S-Video / composite encoder for the Analogizer, NTSC and PAL, from the scan-out's 25 MHz video.
//
// Outputs: y, the luma, and c, the chroma, both unsigned 8-bit; csync_n, the composite sync (an_csync). The adapter
// adds y and c (and takes the sync down) to make the composite signal.
//
//  - Luma is BT.601 at studio levels (an_ypbpr's matrix): 16 is blanking and black (no 7.5 IRE setup), 235 white. Sync is not in y.
//  - Chroma is the subcarrier modulated by U and V, c = 128 + (U sin(w t) +- V cos(w t)) with U and V the colour
//    differences B-Y and R-Y weighted by 0.492 and 0.877 (SMPTE 170M, ITU-R BT.470) and scaled like y (219 codes are
//    100 % white), times the chroma gain CGAIN/16 (12/16 = 0.75; see below). 128 is no chroma: blanking, grey, black
//    and white all give c = 128. U and V are filtered by a one-pole low-pass at about 1.1 MHz (25 MHz clock, a pole at 1/4), so the
//    pixel steps (12.5 MHz) do not put sidebands into the luma's band.
//      NTSC: fsc = 315/88 MHz = 3.579545 MHz, c = U sin + V cos.
//      PAL:  fsc = 4.43361875 MHz, c = U sin +- V cos, the sign of V switching each line.
//  - The colour burst follows the horizontal sync, 15 clock cycles (0.6 us) after the sync pulse ends (csync_n) in NTSC, 22 (0.9 us)
//    in PAL (the scan-out's hs is high from pixel clock 656 to 704 of a line of 790 / 800, 48 pixels of 2 clocks;
//    the picture, de, ends at 640 and starts again at 0 of the next line, so burst and sync are in the line's long back
//    porch). NTSC: 9 cycles of -sin, i.e. 180 degrees to the U axis, amplitude 20 IRE (40 IRE peak to peak). PAL: 10 cycles of
//    -U +- V, 135 and 225 degrees, alternating with the line (the swing the receiver uses to
//    find the V switch), 300 mV peak to peak on a 700 mV white (46.9 codes of peak amplitude). The burst has the amplitude of its standard
//    scaled by CGAIN/16 as the picture's chroma is, so a receiver that references its chroma gain to the burst sees the
//    standard saturation. No burst while vs is high or in the vertical interval of an_csync.
//  - The subcarrier is a 22-bit phase accumulator at the clock (the step is within 0.3 ppm of the subcarrier: 0.4 Hz NTSC,
//    1.2 Hz PAL, 0.03 degrees over a line), and two 1024-entry sine tables in an M10K block each, one for U and one for V,
//    that already carry the weights and the gain (so no multiplier scales U and V); V reads the same phase a quarter turn
//    on (cos), or three quarters on when PAL reverses it. The phase is set to zero at the start of each burst, so every
//    line has the same burst phase and a burst of whole cycles, and then runs on through the picture of the line that
//    follows: the chroma phase is the picture's colour phase against the burst, which is what a receiver measures.
//
// Chroma gain: with the weights above a full-saturation red or cyan has a chroma amplitude of 138 codes, more than the
// 127 an 8-bit c centred on 128 can swing; yellow, at 100 % saturation, puts composite at 133 IRE. CGAIN = 12 (0.75)
// keeps the largest chroma at 104 codes and the composite within -22 .. 122 IRE for every colour. CGAIN is a hardware choice
// (the adapter's weights for y and c): 1 .. 13 (the V table has ten bits).
//
// rgb may be held for two clock cycles per pixel. The inputs hs, vs, de and rgb are those of the scan-out (n64_video's
// vga_hsync, vga_vsync, vga_de and vga_r/g/b).
//
// Latency: LATENCY clock cycles for y, c and csync_n alike: the outputs of cycle n + LATENCY are the inputs of cycle n.
module an_yc #(
    parameter int CLK_HZ           = 25_000_000,
    parameter bit CSYNC_ACTIVE_LOW = 1'b1,
    parameter int CGAIN            = 12,            // chroma gain in sixteenths
    parameter int PH               = 22             // bits of the subcarrier's phase accumulator (a turn is 2^PH)
) (
    input  wire         clk,
    input  wire         rst,
    input  wire         pal,            // 1: PAL (4.43361875 MHz, V switched each line), 0: NTSC
    input  wire         hs,             // active high
    input  wire         vs,             // active high
    input  wire         de,
    input  wire  [23:0] rgb,            // {R, G, B}
    output wire  [7:0]  c,              // chroma, 128 = none
    output logic [7:0]  y,              // luma
    output logic        csync_n,
    // the matrix and the sync a cycle-2 YPbPr output takes (an_out.sv), so that the top level builds one matrix and one an_csync:
    output wire  [7:0]  m_pr, m_y, m_pb,    // Pr, Y, Pb, MAT_LATENCY cycles behind rgb
    output wire         m_csync_n           // the composite sync, as many cycles behind hs
);
localparam int LATENCY = 5;
localparam int MAT_LATENCY = 2;               // m_pr, m_y, m_pb and m_csync_n (an_ypbpr's and an_csync's)

// ------------------------------------------------------------------ constants
// phase step: fsc / CLK_HZ of a turn, PH bits (NTSC 315/88 MHz; PAL 17,734,475 / 4 Hz)
localparam logic [63:0] DEN_N = 64'd88 * 64'(CLK_HZ), DEN_P = 64'd4 * 64'(CLK_HZ);
localparam logic [PH-1:0] STEP_N = PH'(((64'd315_000_000 << PH) + (DEN_N >> 1)) / DEN_N);
localparam logic [PH-1:0] STEP_P = PH'(((64'd17_734_475 << PH) + (DEN_P >> 1)) / DEN_P);
// The weights from Pb, Pr (+-112) to U, V in codes, 0.8523 and 1.2021 times the gain CGAIN/16, are in the sine tables: the table
// for U is sin(w t) times 0.8523 CGAIN/16 x 2^TS / 8 (the filter's output has 3 fractional bits), the one for V the same with
// 1.2021: amplitudes TAU, TAV in ten bits (CGAIN up to 13), x16 for the table's rounding.
localparam int TS = 12;                             // the products are divided by 2^TS
localparam longint TAU16 = (64'(CGAIN) * 64'd8523 * 64'd512 + 64'd5000) / 64'd10000;
localparam longint TAV16 = (64'(CGAIN) * 64'd12021 * 64'd512 + 64'd5000) / 64'd10000;
// the burst, in the units of the filter's output (Pb, Pr codes x 8) so that it goes through the same tables and gain as the
// picture's chroma: NTSC 43.8 codes (20 IRE) peak, PAL 46.9 codes peak, 33.2 on each of the two axes, divided by the weights
localparam int BURST_N  = 411;                      // 43.8 x 8 / 0.8523, on the U axis (NTSC)
localparam int BURST_PU = 312;                      // 33.2 x 8 / 0.8523, on the U axis (PAL)
localparam int BURST_PV = 221;                      // 33.2 x 8 / 1.2021, on the V axis (PAL)
// the burst starts DELAY cycles after csync_n rises: the picture path to c is 3 cycles after the phase register (ROM, product,
// rounding) against 3 of csync_n's delay line beyond its own 2, hence the count from hs falling is DELAY + 2
localparam int BSTART_N = 15 + 2, BSTART_P = 22 + 2;
localparam int NCYC_N = 9, NCYC_P = 10;

// ------------------------------------------------------------------ luma, Pb, Pr (2 cycles)
logic [7:0] y2, pb, pr;
an_ypbpr u_mat (.clk(clk), .rgb(rgb), .de(de), .pr(pr), .y(y2), .pb(pb));

// ------------------------------------------------------------------ composite sync
logic csync_i, vint;
an_csync #(.CSYNC_ACTIVE_LOW(CSYNC_ACTIVE_LOW)) u_cs (.clk(clk), .hs(hs), .vs(vs), .csync_n(csync_i), .vint(vint));
assign m_pr = pr; assign m_y = y2; assign m_pb = pb; assign m_csync_n = csync_i;

// ------------------------------------------------------------------ U, V and their low-pass (cycle 3)
// The filter states are 8 x (Pb - 128), (Pr - 128): ul <= ul - ul / 4 + 2 x input (the quarter rounded down), one adder of three
// terms. At rest it stops 0 .. 3/8 above the input (a Pb code), and the chroma of a grey, which is rounded, is 0.
wire signed [7:0]  cbs = {~pb[7], pb[6:0]};
wire signed [7:0]  crs = {~pr[7], pr[6:0]};
logic signed [10:0] ul, vl;
always_ff @(posedge clk) begin
    if (rst) begin ul <= 11'sd0; vl <= 11'sd0; end
    else begin
        ul <= ul - (ul >>> 2) + 11'($signed({cbs, 1'b0}));
        vl <= vl - (vl >>> 2) + 11'($signed({crs, 1'b0}));
    end
end

// ------------------------------------------------------------------ the burst and the subcarrier
logic        hs_q;
logic [4:0]  bcnt;                       // cycles since hs fell (stops at 31, so b_go is true once a line)
logic        burst_on, vsw;
logic [3:0]  ncyc;
logic [PH-1:0] phase;
wire  [PH:0]   phase_n = {1'b0, phase} + {1'b0, pal ? STEP_P : STEP_N};
wire         hs_fall = hs_q && !hs;
wire         b_go    = bcnt == (pal ? 5'(BSTART_P) : 5'(BSTART_N));
always_ff @(posedge clk) begin
    hs_q <= hs;
    if (rst) begin
        burst_on <= 1'b0; bcnt <= 5'd31; vsw <= 1'b0; ncyc <= 4'd0; phase <= '0;
    end else begin
        phase <= phase_n[PH-1:0];
        if (bcnt != 5'd31) bcnt <= bcnt + 5'd1;
        if (hs_fall) begin
            bcnt <= 5'd1; vsw <= !vsw;
        end
        if (b_go) begin
            if (!vs && !vint) begin
                burst_on <= 1'b1; phase <= '0; ncyc <= 4'd0;
            end
        end
        if (burst_on && phase_n[PH]) begin                       // a whole cycle done
            if (ncyc == (pal ? 4'(NCYC_P - 1) : 4'(NCYC_N - 1))) burst_on <= 1'b0;
            else ncyc <= ncyc + 4'd1;
        end
    end
end

// ------------------------------------------------------------------ sine tables: sin(2 pi i / 1024) scaled by the weights of U and V, one M10K block each
// (built by the recurrence s[i+1] = 2 cos(t) s[i] - s[i-1] in 2^-30 fixed point, which the synthesis tools evaluate
// without a math library; the error at the end of the turn is below 2^-14 of full scale)
(* ramstyle = "M10K", romstyle = "M10K" *) logic signed [9:0] tbu [0:1023];
(* ramstyle = "M10K", romstyle = "M10K" *) logic signed [9:0] tbv [0:1023];
initial begin
    longint sp, sq, sn;
    sp = 0; sq = 6588356;                                       // sin(0), sin(2 pi / 1024), 30 fractional bits
    for (int i = 0; i < 1024; i++) begin
        tbu[i] = 10'((sp * TAU16 + (64'sd1 << 33)) >>> 34);
        tbv[i] = 10'((sp * TAV16 + (64'sd1 << 33)) >>> 34);
        sn = ((64'sd2147443222 * sq) >>> 30) - sp;              // 2 cos(2 pi / 1024) = 2147443222 / 2^30
        sp = sq; sq = sn;
    end
end

// ------------------------------------------------------------------ cycle 4: burst or picture into the modulator, table read
logic signed [10:0] um, vm;
logic signed [9:0]  sn, cs;
wire  [9:0] idx = phase[PH-1:PH-10];
wire signed [10:0] bn = 11'(BURST_N), bpu = 11'(BURST_PU), bpv = 11'(BURST_PV);
wire        vneg = pal && vsw;                           // PAL: the V axis is reversed on every other line
always_ff @(posedge clk) begin
    sn <= tbu[idx];
    cs <= tbv[idx + (vneg ? 10'd768 : 10'd256)];         // cos, or -cos (half a turn on) when V is reversed
    if (burst_on) begin
        um <= pal ? -bpu : -bn;
        vm <= pal ? bpv : 11'sd0;
    end else begin
        um <= ul;
        vm <= vl;
    end
end

// ------------------------------------------------------------------ cycle 5: the products (two multipliers); / 2^TS, + 128 (not registered: the top level's pins are)
// (the largest chroma is 104 codes, the burst less: it never reaches the 127 an eight-bit c can swing, for CGAIN up to 13)
logic signed [21:0] prod;
wire  signed [21:0] q = (prod + 22'(64'sd1 << (TS - 1))) >>> TS;
always_ff @(posedge clk) prod <= um * sn + vm * cs;
assign c = {~q[7], q[6:0]};

// ------------------------------------------------------------------ delay y and csync_n to c
(* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *) logic [7:0] yd [0:2];     // (flip-flops, not a RAM block)
(* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *) logic [2:0] sd;
always_ff @(posedge clk) begin
    yd[0] <= y2; yd[1] <= yd[0]; yd[2] <= yd[1];
    sd    <= {sd[1:0], csync_i};
end
assign y       = yd[2];
assign csync_n = sd[2];
endmodule
