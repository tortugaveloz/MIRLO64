// RDP pixel pipeline: lang/c/mrdp/mrdp.c draw_pixel() built with MRDP_N64, one
// pixel per clock, no stalls (every operand is on chip: TMEM and the span's
// line buffers). Stage by stage the arithmetic is the model's; the model is
// the contract. (From rtl/mrdp/mrdp_pix.v: the texture unit reads the N64's
// TMEM and decodes its formats, the colour image is RGBA5551.)
//
// The N64's coverage (the model's MRDP_CVG, lang/c/mrdp/mrdp.h): each pixel
// comes with its 8-sample mask (rdp_top's walker; all eight for a rectangle
// or a triangle without AA_EN); the alpha, the depth test, the blender and the
// coverage written (the colour image's bit 0, "complete") are the N64's.
//
// Per-primitive state must stay stable while pixels are in flight (the
// sequencer drains the pipe before a state command).
//
// TEXEL1: a 2-cycle pixel whose first combiner cycle reads TEXEL1, or whose second reads TEXEL0 (the N64's second cycle takes the
// first cycle's TEXEL1 there: Mario Golf's ground, an I8 detail tile times the RGBA16 grass, then the grass itself) (two_pass:
// the skybox's day / night blend, a terrain's second layer) goes through the
// texture stages twice, a clock apart -- the tile's sample, then the next
// tile's (t1_*, the same S, T) -- and leaves P9 once, with both. rdp_top
// issues such a pixel every second clock (pix_go); P0 repeats it for the
// second pass (dup0). Each stage takes the tile's fields of its pass's tile:
// the registers u<n>_*, loaded with the stage's data from the pass of the
// stage before (ph<n>).
module rdp_pix (
    input  wire        clk,
    input  wire        rst,

    // ---- per-primitive state
    input  wire [23:0] other_h,
    input  wire [31:0] other_l,
    input  wire [23:0] cc_hi,
    input  wire [31:0] cc_lo,
    input  wire [31:0] prim, env, fog, blend,   // RGBA, R in [31:24]
    input  wire [7:0]  prim_lod_frac,
    input  wire [15:0] prim_z,
    input  wire [15:0] z_tol,       // the primitive's depth step (a decal's tolerance; the coverage's dz)
    input  wire [15:0] prim_dz,     // SET_PRIM_DEPTH's delta
    input  wire        need_mem,    // the colour span is read (rdp_top): the memory's coverage is there
    input  wire        p_aa,        // a triangle: Z moves to the first covered sample
    input  wire [31:0] z_dx, z_dy,  // its DzDx, DzDy
    input  wire [31:0] fill,
    input  wire        p_shade, p_tex, p_zbuf, p_direct,   // p_direct: S,T are s10.5 already
    input  wire [2:0]  t_fmt,
    input  wire [1:0]  t_size,
    input  wire [3:0]  t_pal,
    input  wire [8:0]  t_line,
    input  wire [8:0]  t_tmem,
    input  wire        t_cs, t_ms, t_ct, t_mt,
    input  wire [3:0]  t_masks, t_shifts, t_maskt, t_shiftt,
    input  wire [11:0] t_uls, t_ult, t_lrs, t_lrt,
    // the next tile's (TEXEL1, the second pass of a two_pass pixel)
    input  wire [2:0]  t1_fmt,
    input  wire [1:0]  t1_size,
    input  wire [3:0]  t1_pal,
    input  wire [8:0]  t1_line,
    input  wire [8:0]  t1_tmem,
    input  wire        t1_cs, t1_ms, t1_ct, t1_mt,
    input  wire [3:0]  t1_masks, t1_shifts, t1_maskt, t1_shiftt,
    input  wire [11:0] t1_uls, t1_ult, t1_lrs, t1_lrt,
    output wire        two_pass,    // the primitive's pixels take two clocks each (rdp_top issues one every second clock)

    // ---- pixels in
    input  wire        in_valid,
    input  wire [9:0]  in_x,
    input  wire        in_set,           // the row's line-buffer set
    input  wire [1:0]  row_y,                  // the row (its low bits), steady while its pixels run
    input  wire [31:0] in_r, in_g, in_b, in_a, in_s, in_t, in_w, in_z,
    input  wire [7:0]  in_mask,          // the pixel's samples (bit 7 the top-left; 0xFF: all)

    // ---- TMEM, a copy for each of the filter's three texels (rdp_tmem):
    // the data one cycle after the address
    output reg  [10:0] tm_a0, tm_a1, tm_a2,
    input  wire [15:0] tm_q0, tm_q1, tm_q2,
    input  wire [15:0] tm_h0, tm_h1, tm_h2,
    output wire [7:0]  tl_a0, tl_a1, tl_a2,
    input  wire [15:0] tl_q0, tl_q1, tl_q2,

    // ---- span line buffers (512 x 32: two pixels a word), registered read
    output reg  [9:0]  lb_raddr,          // {set, word}
    input  wire [31:0] zlb_rdata,
    input  wire [31:0] clb_rdata,
    // the raw hidden-bit buffer (rdp_mem, lang/c/mrdp/mrdp.h: the N64's other two
    // coverage bits, a 32-bit word a 16-pixel group, by group mod 128), read
    // as the line buffers: its group's word; the rows' word base bits, by set
    output reg  [7:0]  hl_raddr,
    input  wire [31:0] hl_rdata,
    input  wire [9:0]  rwb0, rwb1,
    output reg         out_valid,        // one pixel result
    output reg         out_set,
    output reg  [9:0]  out_x,
    output wire [15:0] out_color,      // RGBA5551, the RGB dithered (rdp_dither_add); bit 0 the coverage's top bit
    output reg  [1:0]  out_hid,        // the coverage's two hidden bits
    output reg  [15:0] out_depth,
    output reg         out_cpass,        // write the color
    output reg         out_zpass,        // write the depth

    output wire        busy
);
    // ======================================================================
    // helpers
    // ======================================================================
    function automatic [7:0] clamp8;
        input signed [19:0] v;
        clamp8 = v < 0 ? 8'd0 : (v > 255 ? 8'd255 : v[7:0]);
    endfunction

    // cc_begin (sim/rdp/cc_unit.sh extracts these functions)
    // The N64's combiner arithmetic (Angrylion's; the model's cc_eval): ((A - B) * C + D * 256 + 0x80) >> 8
    // kept to 9 bits. "1" is 256 (A and D), C a plain 0..255 (255 is 255/256).
    // The operands arrive extended, by their source (cc_xab, cc_xc below and
    // the selects): A, B and C 10 bits signed, D 9 bits (only its low 9 count
    // in the sum). TEXEL, PRIMITIVE, ENV, ONE are 0..256; the SHADE is 0..255
    // (saturated to a byte by the interpolator, below), a plain positive number as A, B, C and D
    // (zero-extended); COMBINED, the first
    // cycle's result, is its 9 raw bits: as A, B, D 0..383, or -128..-1 when
    // bits 8:7 are both set; as C a signed 9-bit.
    // Only the low 17 bits of the sum matter. The 0x80 only carries out of
    // the product's bit 7 into bit 8: a carry-in of the adder.
    function automatic [9:0] cc_xab;       // COMBINED as A or B
        input [8:0] v;
        cc_xab = {v[8] & v[7], v};
    endfunction
    function automatic [9:0] cc_xc;        // COMBINED as C
        input [8:0] v;
        cc_xc = {v[8], v};
    endfunction
    // cc_eval0: the first cycle, whose operands are 0..511 (COMBINED is 0 there; bit 9 of A, B, C is clear).
    function automatic [8:0] cc_eval0;
        input [9:0] a, b, c;
        input [8:0] d;
        reg signed [9:0]  diff;
        reg signed [19:0] prod;
        reg [16:0] sum;
        begin
            diff = $signed({1'b0, a[8:0]}) - $signed({1'b0, b[8:0]});
            prod = diff * $signed({1'b0, c[8:0]});
            sum = prod[16:0] + {d, 8'h80};
            cc_eval0 = sum[16:8];
        end
    endfunction
    function automatic [8:0] cc_eval1;
        input [9:0] a, b, c;
        input [8:0] d;
        reg signed [10:0] diff;
        reg signed [20:0] prod;
        reg [16:0] sum;
        begin
            diff = $signed(a) - $signed(b);
            prod = diff * $signed(c);
            sum = prod[16:0] + {d, 8'h80};
            cc_eval1 = sum[16:8];
        end
    endfunction
    // a result's 9 bits: a colour (and the alpha's 8 bits) clamped -- bits 8:7
    // both set: 0, bit 8: 255; an alpha's 256 flag -- bit 8 alone, or exactly 255
    function automatic [7:0] cc_clamp;
        input [8:0] v;
        cc_clamp = (v[8] & v[7]) ? 8'd0 : v[8] ? 8'hFF : v[7:0];
    endfunction
    function automatic cc_a256;
        input [8:0] v;
        cc_a256 = v[8] ? ~v[7] : (v[7:0] == 8'hFF);
    endfunction
    // cc_end

    // the colour image's RGBA5551 -> RGBA8888 (memory alpha reads as 1)
    function automatic [31:0] un565;
        input [15:0] v;
        un565 = { v[15:11], 3'd0, v[10:6], 3'd0, v[5:1], 3'd0, 8'hFF };   // (the N64's: zeros below, not the top bits again -- lang/c/mrdp/mrdp.c unrgb565)
    endfunction

    // a 16-bit texel: RGBA5551, or IA 8 8
    function automatic [31:0] tex16;
        input        ia;
        input [15:0] v;
        tex16 = ia ? { v[15:8], v[15:8], v[15:8], v[7:0] }
                   : { v[15:11], v[15:13], v[10:6], v[10:8], v[5:1], v[5:3], {8{v[0]}} };
    endfunction
    // a texel as TMEM gives it (tmem_fetch): the 16-bit word it is in, the
    // upper half's word (32-bit), its place in the word
    function automatic [31:0] texel;
        input [2:0]  fmt;
        input [1:0]  size;
        input [15:0] v, h;
        input [1:0]  sl;
        reg [3:0] n;
        reg [7:0] b;
        begin
            n = sl == 2'd0 ? v[15:12] : sl == 2'd1 ? v[11:8] : sl == 2'd2 ? v[7:4] : v[3:0];
            b = sl[0] ? v[7:0] : v[15:8];
            case (size)
                2'd3: texel = { v, h };
                2'd2: texel = (fmt == 3'd3) ? tex16(1'b1, v) : (fmt <= 3'd2) ? tex16(1'b0, v) : {4{v[15:8]}};
                2'd1: texel = (fmt == 3'd3) ? { {3{b[7:4], b[7:4]}}, b[3:0], b[3:0] } : {4{b}};
                default: texel = (fmt == 3'd3) ? { {3{n[3:1], n[3:1], n[3:2]}}, {8{n[0]}} } : {4{n, n}};
            endcase
        end
    endfunction

    // ======================================================================
    // decoded modes
    // ======================================================================
    wire [1:0] cyc      = other_h[21:20];
    wire       persp    = other_h[19];
    wire       tlut_ia  = other_h[14];
    // TEXEL1 read by the first combiner cycle (mrdp.c cc0_uses_texel1: the same fields)
    wire       cc0_t1   = cc_hi[23:20] == 4'd2 || cc_lo[31:28] == 4'd2 || cc_hi[19:15] == 5'd2 || cc_hi[19:15] == 5'd9 ||
                          cc_lo[17:15] == 3'd2 || cc_hi[14:12] == 3'd2 || cc_lo[14:12] == 3'd2 || cc_hi[11:9] == 3'd2 ||
                          cc_lo[11:9] == 3'd2;
    // TEXEL0 read by the second combiner cycle (mrdp.c cc1_uses_texel0): the N64's second cycle takes the first cycle's TEXEL1 there,
    // the next tile's sample
    wire       cc1_t0   = cc_hi[8:5] == 4'd1 || cc_lo[27:24] == 4'd1 || cc_hi[4:0] == 5'd1 || cc_hi[4:0] == 5'd8 ||
                          cc_lo[8:6] == 3'd1 || cc_lo[23:21] == 3'd1 || cc_lo[5:3] == 3'd1 || cc_lo[20:18] == 3'd1 ||
                          cc_lo[2:0] == 3'd1;
    // (registered: the modes and the combiner are set many clocks before a primitive's first pixel)
    reg        two_pass_q;
    always @(posedge clk) two_pass_q <= cyc == 2'd1 && p_tex && (cc0_t1 || cc1_t0);
    assign two_pass = two_pass_q;
    wire       point    = other_h[13:12] == 2'd0;
    wire       a_cmp    = other_l[0];
    wire       z_src_p  = other_l[2];
    wire       z_cmp    = other_l[4] && !cyc[1];    // (copy and fill mode have no depth)
    wire       z_upd    = other_l[5] && !cyc[1];
    wire       decal    = other_l[11:10] == 2'd3;
    wire       cvg_x_a  = other_l[12];
    wire       force_bl = other_l[14];
    wire       a_dith   = other_l[1];
    wire       aa_en    = other_l[3];
    wire       im_rd    = other_l[6];
    wire       clr_cvg  = other_l[7];
    wire [1:0] cvg_dst  = other_l[9:8];
    wire [1:0] zmode    = other_l[11:10];
    wire       a_cvg_s  = other_l[13];

    // ======================================================================
    // P0: input registers
    // ======================================================================
    reg        v0;
    reg [9:0]  x0;
    // the line buffers' set of the pixel's row (rdp_top), along with x
    reg        bs0, bs1, bs2, bs3, bs4, bs5, bs6, bs7, bs8, bs8t, bs8b, bs9, bs10a, bs10, bs11a, bs11, bs12a, bs12, bs13a;
    reg [31:0] s0r, t0r, w0r;
    reg [35:0] shade0;
    // the shade, as the interpolator gives it, saturated to a byte -- 0 below 0, 255 above -- and carried as 9 bits (bit 8 clear).
    // A shade past 255 (a highlight's overshoot at a lit surface's edge: Link's tunic with Navi's light) never goes into the
    // combiner as it is: with 9 bits TEXEL0 * SHADE overshot 255 and, read as COMBINED in the second cycle of a 2-cycle
    // combiner (ENV * COMBINED), it became negative: black pixels.
    wire [8:0] sr9 = {1'b0, clamp8($signed(in_r[31:16]))}, sg9 = {1'b0, clamp8($signed(in_g[31:16]))};
    wire [8:0] sb9 = {1'b0, clamp8($signed(in_b[31:16]))}, sa9 = {1'b0, clamp8($signed(in_a[31:16]))};
    // the samples: the coverage (their count), and -- a triangle -- the first
    // covered one (the lowest subscanline, its leftmost sample) in quarter
    // pixels from the pixel's top-left corner, where Z is taken (the attributes
    // are the plane's there): Z += (DzDx>>5 * ox + DzDy>>5 * oy) << 3. Sample 7
    // (the only one a pixel without AA_EN is drawn for) is (0, 0).
    // The mask's nibbles are two subscanlines each, 0xA the even one's samples
    // and 0x5 the odd one's.
    wire [3:0] cj0 = in_mask[7:4] & 4'hA, cj1 = in_mask[7:4] & 4'h5;
    wire [3:0] cj2 = in_mask[3:0] & 4'hA, cj3 = in_mask[3:0] & 4'h5;
    wire [3:0] cjf = (cj0 != 4'd0) ? cj0 : (cj1 != 4'd0) ? cj1 : (cj2 != 4'd0) ? cj2 : cj3;
    wire [1:0] oy4 = (cj0 != 4'd0) ? 2'd0 : (cj1 != 4'd0) ? 2'd1 : (cj2 != 4'd0) ? 2'd2 : 2'd3;
    wire [1:0] ox4 = cjf[3] ? 2'd0 : cjf[2] ? 2'd1 : cjf[1] ? 2'd2 : 2'd3;
    wire signed [28:0] zcx = $signed(z_dx[31:5]) * $signed({1'b0, ox4});
    wire signed [28:0] zcy = $signed(z_dy[31:5]) * $signed({1'b0, oy4});
    wire [31:0] zcorr = ({{3{zcx[28]}}, zcx} + {{3{zcy[28]}}, zcy}) << 3;
    wire [3:0] cvg_in = {3'd0, in_mask[0]} + in_mask[1] + in_mask[2] + in_mask[3]
                      + in_mask[4] + in_mask[5] + in_mask[6] + in_mask[7];
    reg  [31:0] z0r, zc0;
    reg  [63:0] cvd;                 // the coverage, along with the pixel to P11 (16 x 4)
    reg         dup0, ph0;           // two_pass: the pixel in these registers a second clock (its second pass); ph0 marks that clock
    always @(posedge clk) begin
        dup0 <= in_valid & two_pass & ~rst;
        v0 <= (in_valid | dup0) & ~rst;
        ph0 <= dup0;
        if (!dup0) begin             // (the second pass repeats the pixel as it is: rdp_top has gone on)
            x0 <= in_x; bs0 <= in_set;
            // (a primitive without texture coefficients has S = T = W = 0 and samples the tile's texel (0, 0) all the same: the model's
            // draw_pixel_cvg; Link's collar is one)
            s0r <= p_tex ? in_s : 32'd0; t0r <= p_tex ? in_t : 32'd0; w0r <= p_tex ? in_w : 32'd0;
            shade0 <= p_shade ? {sr9, sg9, sb9, sa9} : 36'd0;
            z0r <= in_z;
            zc0 <= p_aa ? zcorr : 32'd0;
        end
        cvd <= {cvd[59:0], dup0 ? cvd[3:0] : cvg_in};
    end
    wire [31:0] zin_c = z0r + zc0;
    wire signed [31:0] zq = $signed(zin_c) >>> 15;
    wire [15:0] dz = zq < 0 ? 16'd0 : (zq > 32'sd65535 ? 16'hFFFF : zq[15:0]);

    // ======================================================================
    // P1: normalise W, table lookup
    // ======================================================================
    wire [31:0] wpos = ($signed(w0r) <= 0) ? 32'd1 : w0r;
    reg  [4:0]  sh_c;
    integer k;
    always @(*) begin
        sh_c = 5'd30;
        for (k = 0; k <= 30; k = k + 1)
            if (wpos[k]) sh_c = 5'd30 - k[4:0];
    end
    wire [31:0] wn = wpos << sh_c;
    wire [16:0] rom_p;
    wire [10:0] rom_d;
    mrdp_rcp_rom rom (.idx(wn[29:24]), .p(rom_p), .d(rom_d));

    // the pass of the pixel in each stage (0 the tile's, 1 the next tile's: two_pass) and the tile fields the stage
    // takes, registered with it
    reg        ph1, ph2, ph3, ph4, ph5, ph6, ph7, ph8, ph8t, ph8b;
    reg [3:0]  u4_shifts, u4_shiftt, u5_masks, u5_maskt;
    reg [11:0] u4_uls, u4_ult, u5_uls, u5_ult, u5_lrs, u5_lrt;
    reg        u5_cs, u5_ms, u5_ct, u5_mt;
    reg [8:0]  u6_tmem, u6_line;
    reg [1:0]  u6_size, u8t_size;
    reg        u8_size0;
    reg [3:0]  u8_pal;
    reg [2:0]  u8t_fmt;
    wire       tlut_on = other_h[15] && !u8t_size[1];
    reg        v1;
    reg [9:0]  x1;
    reg [16:0] p1;
    reg [10:0] d1;
    reg [8:0]  f1;
    reg [4:0]  sh1;
    reg [31:0] s1r, t1r;
    reg [35:0] shade1;
    reg [15:0] depth1;
    always @(posedge clk) begin
        v1 <= v0 & ~rst; x1 <= x0; bs1 <= bs0; ph1 <= ph0;
        p1 <= rom_p; d1 <= rom_d; f1 <= wn[23:15]; sh1 <= sh_c;
        s1r <= s0r; t1r <= t0r; shade1 <= shade0;
        depth1 <= (z_src_p || !p_zbuf) ? prim_z : dz;
    end

    // ======================================================================
    // P2: r = P - (D * frac >> 9); S, T clamped to the 27-bit multiplier input
    // ======================================================================
    // The products S * r and T * r go down by 27 - sh places (W is 2.30, lang/c/mrdp/mrdp_setup.h mrdp_slope30: 41 - 14;
    // none above sh 27). r is taken up by the low part of that here, sh mod 8 places, so that what is left for the two
    // 45-bit products is one of four shifts, 27, 19, 11 or 3 (sq: sh / 8) -- sh 28..30: up 3 and down 3
    wire [19:0] dfrac = d1 * f1;
    wire [16:0] r1 = p1 - {6'd0, dfrac[19:9]};
    wire        sh_top = sh1 > 5'd27;
    wire [2:0]  sj1 = sh_top ? 3'd3 : sh1[2:0];
    function automatic [26:0] sat27;
        input [31:0] v;
        begin
            if ($signed(v) > 32'sd67108863) sat27 = 27'h3FFFFFF;
            else if ($signed(v) < -32'sd67108864) sat27 = 27'h4000000;
            else sat27 = v[26:0];
        end
    endfunction
    reg        v2;
    reg [9:0]  x2;
    reg [23:0] r2;
    reg [1:0]  sq2;
    reg [26:0] sc2, tc2;
    reg [15:0] s2r, t2r;                 // S and T as the affine and the direct paths take them (s10.5 in 16 bits), decided here: only the perspective path wants more
    reg [35:0] shade2;
    reg [15:0] depth2;
    always @(posedge clk) begin
        v2 <= v1 & ~rst; x2 <= x1; bs2 <= bs1; ph2 <= ph1;
        r2 <= {7'd0, r1} << sj1;
        sq2 <= sh_top ? 2'd3 : sh1[4:3];
        sc2 <= sat27(s1r); tc2 <= sat27(t1r);
        s2r <= p_direct ? s1r[15:0] : sat16_32($signed(s1r) >>> 11);
        t2r <= p_direct ? t1r[15:0] : sat16_32($signed(t1r) >>> 11);
        shade2 <= shade1; depth2 <= depth1;
    end

    // ======================================================================
    // P3: S * r, T * r
    // ======================================================================
    reg        v3;
    reg [9:0]  x3;
    reg signed [51:0] ps3, pt3;
    reg [1:0]  sq3;
    reg [15:0] s3r, t3r;
    reg [35:0] shade3;
    reg [15:0] depth3;
    always @(posedge clk) begin
        v3 <= v2 & ~rst; x3 <= x2; bs3 <= bs2; ph3 <= ph2;
        ps3 <= $signed(sc2) * $signed({1'b0, r2});
        pt3 <= $signed(tc2) * $signed({1'b0, r2});
        sq3 <= sq2;
        s3r <= s2r; t3r <= t2r; shade3 <= shade2; depth3 <= depth2;
    end

    // ======================================================================
    // P4: s10.5 coordinates (perspective, affine, or direct)
    // ======================================================================
    // a product down by 27, 19, 11 or 3 places (q: 0..3), saturated to 16 bits: the 16 bits from there, when all above
    // them are the sign
    function automatic [15:0] sat16_q;
        input signed [51:0] v;
        input [1:0] q;
        reg [15:0] lo;
        reg        in0, in1, in2, in3, ok;
        begin
            in0 = v[51:42] == {10{v[51]}};
            in1 = in0 && v[41:34] == {8{v[51]}};
            in2 = in1 && v[33:26] == {8{v[51]}};
            in3 = in2 && v[25:18] == {8{v[51]}};
            case (q)
                2'd0:    begin lo = v[42:27]; ok = in0; end
                2'd1:    begin lo = v[34:19]; ok = in1; end
                2'd2:    begin lo = v[26:11]; ok = in2; end
                default: begin lo = v[18:3];  ok = in3; end
            endcase
            sat16_q = ok ? lo : v[51] ? 16'h8000 : 16'h7FFF;
        end
    endfunction
    function automatic [15:0] sat16_32;
        input signed [31:0] v;
        sat16_32 = (v > 32'sd32767) ? 16'h7FFF : ((v < -32'sd32768) ? 16'h8000 : v[15:0]);
    endfunction
    reg        v4;
    reg [9:0]  x4;
    reg [15:0] s4, t4;
    reg [35:0] shade4;
    reg [15:0] depth4;
    always @(posedge clk) begin
        v4 <= v3 & ~rst; x4 <= x3; bs4 <= bs3; ph4 <= ph3;
        // the tile fields P5's shift and origin take (the pass's tile)
        u4_shifts <= ph3 ? t1_shifts : t_shifts; u4_shiftt <= ph3 ? t1_shiftt : t_shiftt;
        u4_uls <= ph3 ? t1_uls : t_uls; u4_ult <= ph3 ? t1_ult : t_ult;
        if (p_direct || !persp) begin
            s4 <= s3r; t4 <= t3r;
        end else begin
            s4 <= sat16_q(ps3, sq3); t4 <= sat16_q(pt3, sq3);
        end
        shade4 <= shade3; depth4 <= depth3;
    end

    // ======================================================================
    // P5: tile shift, origin (P5a); clamp, mirror, mask (P5b)
    // ======================================================================
    // v shifted right by `shift` (up to 10) or left by 16 - shift (11..15): a product with a power of two
    // on a DSP block -- v * 2^k >>> 10, k = 10 - shift, or 26 - shift -- instead of a barrel shifter
    function automatic [15:0] tc_pow;
        input [3:0] shift;
        reg [3:0] k;
        begin
            k = (shift <= 4'd10) ? 4'd10 - shift : 4'd10 + 4'd16 - shift;
            tc_pow = 16'd1 << k;
        end
    endfunction
    wire signed [33:0] tcp_s = $signed(s4) * $signed({1'b0, tc_pow(u4_shifts)});
    wire signed [33:0] tcp_t = $signed(t4) * $signed({1'b0, tc_pow(u4_shiftt)});
    wire signed [23:0] tcs_s = tcp_s[33:10];
    wire signed [23:0] tcs_t = tcp_t[33:10];
    wire signed [23:0] vs5 = tcs_s - $signed({9'd0, u4_uls, 3'd0});
    wire signed [23:0] vt5 = tcs_t - $signed({9'd0, u4_ult, 3'd0});
    reg        v5;
    reg [9:0]  x5;
    reg signed [23:0] vs5r, vt5r;
    reg [35:0] shade5;
    reg [15:0] depth5;
    always @(posedge clk) begin
        v5 <= v4 & ~rst; x5 <= x4; bs5 <= bs4; ph5 <= ph4;
        // and the clamp, mirror and mask of P5b
        u5_uls <= u4_uls; u5_ult <= u4_ult;
        u5_lrs <= ph4 ? t1_lrs : t_lrs; u5_lrt <= ph4 ? t1_lrt : t_lrt;
        u5_cs <= ph4 ? t1_cs : t_cs; u5_ms <= ph4 ? t1_ms : t_ms; u5_ct <= ph4 ? t1_ct : t_ct; u5_mt <= ph4 ? t1_mt : t_mt;
        u5_masks <= ph4 ? t1_masks : t_masks; u5_maskt <= ph4 ? t1_maskt : t_maskt;
        vs5r <= vs5; vt5r <= vt5;
        shade5 <= shade4; depth5 <= depth4;
    end

    // one axis: v -> c0, c1 (wrapped) and the 5-bit fraction
    task automatic tc_axis;
        input  signed [23:0] v;
        input  [11:0] lo, hi;
        input  clampbit, mirror;
        input  [3:0] mask;
        output [10:0] c0, c1;
        output [4:0]  fr;
        reg signed [18:0] i, j, mx, m;
        reg [4:0] f;
        begin
            i = v >>> 5;
            f = v[4:0];
            j = i + 19'sd1;
            if (clampbit || mask == 4'd0) begin
                mx = ($signed({7'd0, hi}) - $signed({7'd0, lo})) >>> 2;
                if (i < 0) begin i = 0; j = 0; f = 0; end
                else if (i >= mx) begin i = mx; j = mx; f = 0; end
            end
            if (mask != 4'd0) begin
                m = (19'sd1 <<< mask) - 19'sd1;
                if (mirror && ((i >>> mask) & 19'sd1)) i = ~i;
                if (mirror && ((j >>> mask) & 19'sd1)) j = ~j;
                i = i & m;
                j = j & m;
            end
            c0 = i[10:0];
            c1 = j[10:0];
            fr = f;
        end
    endtask

    reg [10:0] s0c, s1c, t0c, t1c;
    reg [4:0]  fsc, ftc;
    always @(*) begin
        tc_axis(vs5r, u5_uls, u5_lrs, u5_cs, u5_ms, u5_masks, s0c, s1c, fsc);
        tc_axis(vt5r, u5_ult, u5_lrt, u5_ct, u5_mt, u5_maskt, t0c, t1c, ftc);
    end
    reg        v6;
    reg [9:0]  x6;
    reg [10:0] s06, s16, t06, t16;
    reg [4:0]  fs6, ft6;
    reg [35:0] shade6;
    reg [15:0] depth6;
    always @(posedge clk) begin
        v6 <= v5 & ~rst; x6 <= x5; bs6 <= bs5; ph6 <= ph5;
        // the TMEM addresses of P7
        u6_tmem <= ph5 ? t1_tmem : t_tmem; u6_line <= ph5 ? t1_line : t_line; u6_size <= ph5 ? t1_size : t_size;
        s06 <= s0c; s16 <= s1c; t06 <= t0c; t16 <= t1c; fs6 <= fsc; ft6 <= ftc;
        shade6 <= shade5; depth6 <= depth5;
    end

    // ======================================================================
    // P7: TMEM addresses of the three texels the filter takes -- (s1, t0),
    // (s0, t1), and (s0, t0) or, in the 2 x 2's lower triangle, (s1, t1);
    // unfiltered, (s0, t0). A row is `line` 64-bit words:
    // 16-bit word tmem*4 + t*line*4 + s (16- and 32-bit texels), s/2, s/4.
    // On an odd row the halves of each 64-bit word change places (word ^ 2),
    // as the N64's: they did when the row was loaded (rdp_mem).
    // ======================================================================
    wire        upper6 = ({1'b0, fs6} + {1'b0, ft6}) < 6'd32;
    wire        selA6 = upper6 || point;
    wire [19:0] row0 = t06 * u6_line;
    wire [19:0] row1 = t16 * u6_line;
    function automatic [10:0] taddr;
        input [19:0] row;
        input [10:0] sc;
        input        odd;
        reg [10:0] a;
        begin
            a = {u6_tmem, 2'b00} + {row[8:0], 2'b00}
              + (u6_size[1] ? sc : u6_size[0] ? {1'b0, sc[10:1]} : {2'b00, sc[10:2]});
            a[1] = a[1] ^ odd;
            taddr = (u6_size == 2'd3) ? {1'b0, a[9:0]} : a;
        end
    endfunction
    reg        v7;
    reg [9:0]  x7;
    reg [1:0]  sl0_7, sl1_7, sl2_7;         // each texel's place in its 16-bit word
    reg [4:0]  fs7, ft7;
    reg        up7;
    reg [35:0] shade7;
    reg [15:0] depth7;
    always @(posedge clk) begin
        v7 <= v6 & ~rst; x7 <= x6; bs7 <= bs6; ph7 <= ph6;
        tm_a0 <= selA6 ? taddr(row0, s06, t06[0]) : taddr(row1, s16, t16[0]);
        tm_a1 <= taddr(row0, s16, t06[0]);
        tm_a2 <= taddr(row1, s06, t16[0]);
        sl0_7 <= selA6 ? s06[1:0] : s16[1:0]; sl1_7 <= s16[1:0]; sl2_7 <= s06[1:0];
        fs7 <= fs6; ft7 <= ft6; up7 <= selA6;
        shade7 <= shade6; depth7 <= depth6;
    end

    // ======================================================================
    // P8: TMEM's words arrive; a 4- or 8-bit texel is the TLUT's address
    // ======================================================================
    reg        v8;
    reg [9:0]  x8;
    reg [1:0]  sl0_8, sl1_8, sl2_8;
    reg [4:0]  fs8, ft8;
    reg        up8;
    reg [35:0] shade8;
    reg [15:0] depth8;
    always @(posedge clk) begin
        v8 <= v7 & ~rst; x8 <= x7; bs8 <= bs7; ph8 <= ph7;
        // the TLUT index (4- and 8-bit texels)
        u8_size0 <= ph7 ? t1_size[0] : t_size[0]; u8_pal <= ph7 ? t1_pal : t_pal;
        sl0_8 <= sl0_7; sl1_8 <= sl1_7; sl2_8 <= sl2_7;
        fs8 <= fs7; ft8 <= ft7; up8 <= up7;
        shade8 <= shade7; depth8 <= depth7;
    end
    function automatic [7:0] tl_idx;
        input [15:0] v;
        input [1:0]  sl;
        reg [3:0] n;
        begin
            n = sl == 2'd0 ? v[15:12] : sl == 2'd1 ? v[11:8] : sl == 2'd2 ? v[7:4] : v[3:0];
            tl_idx = u8_size0 ? (sl[0] ? v[7:0] : v[15:8]) : {u8_pal, n};
        end
    endfunction
    assign tl_a0 = tl_idx(tm_q0, sl0_8);
    assign tl_a1 = tl_idx(tm_q1, sl1_8);
    assign tl_a2 = tl_idx(tm_q2, sl2_8);

    // P8t: the words held while the TLUT is read
    reg        v8t;
    reg [9:0]  x8t;
    reg [15:0] q0t, q1t, q2t, h0t, h1t, h2t;
    reg [1:0]  sl0_t, sl1_t, sl2_t;
    reg [4:0]  fs8t, ft8t;
    reg        up8t;
    reg [35:0] shade8t;
    reg [15:0] depth8t;
    always @(posedge clk) begin
        v8t <= v8 & ~rst; x8t <= x8; bs8t <= bs8; ph8t <= ph8;
        // the texel's decode
        u8t_fmt <= ph8 ? t1_fmt : t_fmt; u8t_size <= ph8 ? t1_size : t_size;
        q0t <= tm_q0; q1t <= tm_q1; q2t <= tm_q2; h0t <= tm_h0; h1t <= tm_h1; h2t <= tm_h2;
        sl0_t <= sl0_8; sl1_t <= sl1_8; sl2_t <= sl2_8;
        fs8t <= fs8; ft8t <= ft8; up8t <= up8;
        shade8t <= shade8; depth8t <= depth8;
    end
    wire [31:0] tx0 = tlut_on ? tex16(tlut_ia, tl_q0) : texel(u8t_fmt, u8t_size, q0t, h0t, sl0_t);
    wire [31:0] tx1 = tlut_on ? tex16(tlut_ia, tl_q1) : texel(u8t_fmt, u8t_size, q1t, h1t, sl1_t);
    wire [31:0] tx2 = tlut_on ? tex16(tlut_ia, tl_q2) : texel(u8t_fmt, u8t_size, q2t, h2t, sl2_t);

    // The filter's edges, formed here: the decode leaves this stage slack, the filter's multipliers (P9) none. The first
    // edge goes to B for an upper triangle and to C for a lower one, the second to the other
    function automatic [9:0] edge1;
        input [7:0] p, x;
        edge1 = $signed({2'b0, p}) - $signed({2'b0, x});
    endfunction
    function automatic [39:0] edge4;
        input [31:0] p, x;
        edge4 = { edge1(p[31:24], x[31:24]), edge1(p[23:16], x[23:16]), edge1(p[15:8], x[15:8]), edge1(p[7:0], x[7:0]) };
    endfunction
    wire [39:0] e1w8t = edge4(up8t ? tx1 : tx2, tx0);
    wire [39:0] e2w8t = edge4(up8t ? tx2 : tx1, tx0);

    // P8b: the decoded texels, registered (the memories through the decode
    // into the filter's multipliers is too long a path)
    reg        v8b;
    reg [9:0]  x8b;
    reg [31:0] ta8b;                        // the corner (A or D)
    reg [39:0] e1v8b, e2v8b;                // the edges from the corner to the other two texels, 10 bits a channel
    reg [5:0]  ws8b, wt8b;                  // the weights of those edges
    reg [35:0] shade8b;
    reg [15:0] depth8b;
    always @(posedge clk) begin
        v8b <= v8t & ~rst; x8b <= x8t; bs8b <= bs8t; ph8b <= ph8t;
        ta8b <= tx0; e1v8b <= e1w8t; e2v8b <= e2w8t;
        ws8b <= up8t ? {1'b0, fs8t} : 6'd32 - {1'b0, fs8t};
        wt8b <= up8t ? {1'b0, ft8t} : 6'd32 - {1'b0, ft8t};
        shade8b <= shade8t; depth8b <= depth8t;
    end

    // ======================================================================
    // P9: N64 3-point filter: from the corner x, by the two edges to B and C
    // ======================================================================
    function automatic [7:0] tri3;
        input [7:0] x;
        input [9:0] e1v, e2v;
        input [5:0] ws, wt;
        reg signed [9:0]  e1, e2;
        reg signed [16:0] acc;
        begin
            e1 = e1v; e2 = e2v;
            acc = e1 * $signed({1'b0, ws}) + e2 * $signed({1'b0, wt}) + 17'sd16;
            tri3 = clamp8($signed({12'd0, x}) + (acc >>> 5));
        end
    endfunction
    wire [31:0] filt9 = { tri3(ta8b[31:24], e1v8b[39:30], e2v8b[39:30], ws8b, wt8b),
                          tri3(ta8b[23:16], e1v8b[29:20], e2v8b[29:20], ws8b, wt8b),
                          tri3(ta8b[15:8],  e1v8b[19:10], e2v8b[19:10], ws8b, wt8b),
                          tri3(ta8b[7:0],   e1v8b[9:0],   e2v8b[9:0],   ws8b, wt8b) };
    // the pixel's group in the raw hidden buffer: its word's address (the
    // row's word base + x / 2) by 8; its place there, k
    wire [9:0]  hw8b = (bs8b ? rwb1 : rwb0) + {1'b0, x8b[9:1]};
    // the texel the pass gives: filtered, or the corner's; a two_pass pixel's first pass is kept (texA9) for its second,
    // which gives TEXEL1 (tex19; TEXEL0 is tex9)
    wire [31:0] tex_cur = point ? ta8b : filt9;
    reg  [31:0] texA9, tex19;
    reg  [3:0]  hk9, hk10a;
    reg        v9;
    reg [9:0]  x9;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *) reg [31:0] tex9;
    reg [35:0] shade9;
    reg [15:0] depth9;
    always @(posedge clk) begin
        v9 <= v8b & ~rst & (!two_pass | ph8b); x9 <= x8b; bs9 <= bs8b;
        lb_raddr <= {bs8b, x8b[9:1]};           // line buffers: data after the next edge, captured at P10b
        hl_raddr <= {bs8b, hw8b[9:3]};          // the hidden word of the pixel's group
        hk9 <= {hw8b[2:0], x8b[0]};
        if (!ph8b) texA9 <= tex_cur;
        tex9 <= (two_pass & ph8b) ? texA9 : tex_cur;
        tex19 <= tex_cur;
        shade9 <= shade8b; depth9 <= depth8b;
    end

    // ======================================================================
    // combiner selections (N64 SetCombine)
    // ======================================================================
    function automatic [7:0] ch;           // byte ch of an RGBA word: 0 r, 1 g, 2 b, 3 a
        input [31:0] c;
        input [1:0]  i;
        ch = c[31 - 8*i -: 8];
    endfunction
    function automatic [8:0] ch9;          // the first cycle's result (COMBINED): 9 bits a channel
        input [35:0] c;
        input [1:0]  i;
        ch9 = c[35 - 9*i -: 9];
    endfunction
    function automatic [9:0] rgb_a;        // sub A (and the RGB core of B, C, D); 6 is 1 (256); 10 bits, extended, see cc_xab
        input [3:0]  sel;
        input [1:0]  i;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        case (sel)
            4'd0: rgb_a = cc_xab(ch9(cm, i));
            4'd1: rgb_a = {2'b00, ch(tex, i)};
            4'd2: rgb_a = {2'b00, ch(tex1, i)};
            4'd3: rgb_a = {2'b00, ch(prim, i)};
            4'd4: rgb_a = {1'b0, ch9(sh, i)};
            4'd5: rgb_a = {2'b00, ch(env, i)};
            4'd6: rgb_a = 10'd256;
            default: rgb_a = 10'd0;
        endcase
    endfunction
    function automatic [9:0] rgb_b;
        input [3:0]  sel;
        input [1:0]  i;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        rgb_b = (sel >= 4'd6) ? 10'd0 : rgb_a(sel, i, tex, tex1, sh, cm);
    endfunction
    function automatic [9:0] rgb_c;
        input [4:0]  sel;
        input [1:0]  i;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        case (sel)
            5'd0:  rgb_c = cc_xc(ch9(cm, i));
            5'd1, 5'd2, 5'd3, 5'd4, 5'd5: rgb_c = rgb_a(sel[3:0], i, tex, tex1, sh, cm);
            5'd7:  rgb_c = cc_xc(ch9(cm, 2'd3));
            5'd8:  rgb_c = {2'b00, tex[7:0]};
            5'd9:  rgb_c = {2'b00, tex1[7:0]};
            5'd10: rgb_c = {2'b00, prim[7:0]};
            5'd11: rgb_c = {1'b0, ch9(sh, 2'd3)};
            5'd12: rgb_c = {2'b00, env[7:0]};
            5'd14: rgb_c = {2'b00, prim_lod_frac};
            default: rgb_c = 10'd0;
        endcase
    endfunction
    function automatic [8:0] rgb_d;        // (D's low 9 bits are all that count)
        input [2:0]  sel;
        input [1:0]  i;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        reg [9:0] t;
        begin
            t = rgb_a({1'b0, sel}, i, tex, tex1, sh, cm);
            rgb_d = (sel == 3'd7) ? 9'd0 : t[8:0];
        end
    endfunction
    function automatic [9:0] al_abd;
        input [2:0]  sel;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        case (sel)
            3'd0: al_abd = cc_xab(ch9(cm, 2'd3));
            3'd1: al_abd = {2'b00, tex[7:0]};
            3'd2: al_abd = {2'b00, tex1[7:0]};
            3'd3: al_abd = {2'b00, prim[7:0]};
            3'd4: al_abd = {1'b0, ch9(sh, 2'd3)};
            3'd5: al_abd = {2'b00, env[7:0]};
            3'd6: al_abd = 10'd256;
            default: al_abd = 10'd0;
        endcase
    endfunction
    function automatic [8:0] al_d;
        input [2:0]  sel;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        reg [9:0] t;
        begin
            t = al_abd(sel, tex, tex1, sh, cm);
            al_d = t[8:0];
        end
    endfunction
    function automatic [9:0] al_c;
        input [2:0]  sel;
        input [31:0] tex, tex1;
        input [35:0] sh;
        case (sel)
            3'd1: al_c = {2'b00, tex[7:0]};
            3'd2: al_c = {2'b00, tex1[7:0]};
            3'd3: al_c = {2'b00, prim[7:0]};
            3'd4: al_c = {1'b0, ch9(sh, 2'd3)};
            3'd5: al_c = {2'b00, env[7:0]};
            3'd6: al_c = {2'b00, prim_lod_frac};
            default: al_c = 10'd0;
        endcase
    endfunction

    // (functions are evaluated in continuous assignments and only their
    // results registered: Verilator 5.020 mis-evaluated the nested automatic
    // functions inside the clocked blocks -- comb11 came out with RGB 0)
    //
    // Each combiner and blender cycle is two stages: the operand muxes, then
    // the arithmetic. As one stage the path from the mode registers through
    // the muxes, the multiplier and the clamp missed the clock by ~1 ns.

    // one combiner cycle's operands {A, B, C, D}, each RGBA: A, B, C of 10 bits (extended, see cc_xab; R in [155:146], [115:106], [75:66]),
    // D of 9 (R in [35:27])
    function automatic [155:0] cc_sel;
        input        cy;              // 0: cycle 0 settings, 1: cycle 1
        input [31:0] tex, tex1;           // TEXEL0, TEXEL1
        input [35:0] sh;                  // SHADE: 9 bits a channel
        input [35:0] cm;
        reg [3:0] sa_r, sb_r;
        reg [4:0] mul_r;
        reg [2:0] sa_a, mul_a, sb_a, add_a, add_r;
        begin
            if (!cy) begin
                sa_r = cc_hi[23:20]; mul_r = cc_hi[19:15]; sa_a = cc_hi[14:12]; mul_a = cc_hi[11:9];
                sb_r = cc_lo[31:28]; add_r = cc_lo[17:15]; sb_a = cc_lo[14:12]; add_a = cc_lo[11:9];
            end else begin
                sa_r = cc_hi[8:5];   mul_r = cc_hi[4:0];   sa_a = cc_lo[23:21]; mul_a = cc_lo[20:18];
                sb_r = cc_lo[27:24]; add_r = cc_lo[8:6];   sb_a = cc_lo[5:3];   add_a = cc_lo[2:0];
            end
            cc_sel = {
                rgb_a(sa_r, 2'd0, tex, tex1, sh, cm), rgb_a(sa_r, 2'd1, tex, tex1, sh, cm), rgb_a(sa_r, 2'd2, tex, tex1, sh, cm), al_abd(sa_a, tex, tex1, sh, cm),
                rgb_b(sb_r, 2'd0, tex, tex1, sh, cm), rgb_b(sb_r, 2'd1, tex, tex1, sh, cm), rgb_b(sb_r, 2'd2, tex, tex1, sh, cm), al_abd(sb_a, tex, tex1, sh, cm),
                rgb_c(mul_r, 2'd0, tex, tex1, sh, cm), rgb_c(mul_r, 2'd1, tex, tex1, sh, cm), rgb_c(mul_r, 2'd2, tex, tex1, sh, cm), al_c(mul_a, tex, tex1, sh),
                rgb_d(add_r, 2'd0, tex, tex1, sh, cm), rgb_d(add_r, 2'd1, tex, tex1, sh, cm), rgb_d(add_r, 2'd2, tex, tex1, sh, cm), al_d(add_a, tex, tex1, sh, cm) };
        end
    endfunction
    // the four channels' raw 9 bits (cycle 0: operands 0..511; cycle 1: COMBINED too)
    function automatic [35:0] cc_arith0;
        input [155:0] o;
        cc_arith0 = { cc_eval0(o[155:146], o[115:106], o[75:66], o[35:27]),
                      cc_eval0(o[145:136], o[105:96],  o[65:56], o[26:18]),
                      cc_eval0(o[135:126], o[95:86],   o[55:46], o[17:9]),
                      cc_eval0(o[125:116], o[85:76],   o[45:36], o[8:0]) };
    endfunction
    function automatic [35:0] cc_arith1;
        input [155:0] o;
        cc_arith1 = { cc_eval1(o[155:146], o[115:106], o[75:66], o[35:27]),
                      cc_eval1(o[145:136], o[105:96],  o[65:56], o[26:18]),
                      cc_eval1(o[135:126], o[95:86],   o[55:46], o[17:9]),
                      cc_eval1(o[125:116], o[85:76],   o[45:36], o[8:0]) };
    endfunction

    // ======================================================================
    // P10a/b: combiner cycle 0; P11a/b: cycle 1 (COMBINED = cycle 0 in
    // 2-cycle, 0 in 1-cycle); copy mode takes the texel. The line-buffer
    // words addressed at P9 are captured at P10b.
    // ======================================================================
    wire [155:0] o0_w = cc_sel(1'b0, tex9, tex19, shade9, 36'd0);
    reg         v10a;
    reg [9:0]   x10a;
    reg [155:0] o10a;
    reg [31:0]  tex10a, tex1_10a;     // (tex1_*: the next tile's sample, the second cycle's TEXEL0)
    reg [35:0]  shade10a;
    reg [15:0]  depth10a;
    always @(posedge clk) begin
        v10a <= v9 & ~rst; x10a <= x9; bs10a <= bs9; hk10a <= hk9;
        o10a <= o0_w;
        tex10a <= tex9; tex1_10a <= tex19; shade10a <= shade9; depth10a <= depth9;
    end
    wire [35:0] c0_w = cc_arith0(o10a);
    reg        v10;
    reg [9:0]  x10;
    reg [35:0] c010;                   // the first cycle's result: 9 raw bits a channel (cc_eval1)
    reg [31:0] tex10, tex1_10, mem10;
    reg [35:0] shade10;
    reg [15:0] depth10, zold10;
    reg        memb10;                // the colour image's bit 0 (the coverage's top bit)
    reg [1:0]  hid10;                 // its hidden two
    always @(posedge clk) begin
        v10 <= v10a & ~rst; x10 <= x10a; bs10 <= bs10a;
        c010 <= c0_w;
        tex10 <= tex10a; tex1_10 <= tex1_10a; shade10 <= shade10a; depth10 <= depth10a;
        zold10 <= x10a[0] ? zlb_rdata[15:0] : zlb_rdata[31:16];     // the even pixel first
        mem10 <= un565(x10a[0] ? clb_rdata[15:0] : clb_rdata[31:16]);
        memb10 <= x10a[0] ? clb_rdata[0] : clb_rdata[16];
        hid10 <= hl_rdata[{hk10a, 1'b0} +: 2];
    end
    wire [35:0]  cm10 = (cyc == 2'd1) ? c010 : 36'd0;   // COMBINED for cycle 1
    wire [155:0] o1_w = cc_sel(1'b1, tex1_10, tex10, shade10, cm10);   // (the second cycle: TEXEL0 the next tile's sample, TEXEL1 the tile's)
    reg         v11a;
    reg [9:0]   x11a;
    reg [155:0] o11a;
    reg [31:0]  tex11a, mem11a;
    reg [35:0]  shade11a;
    reg [15:0]  depth11a, zold11a;
    reg         memb11a, memb11;
    reg [1:0]   hid11a, hid11;
    reg [7:0]   c0a11a, c0a11;       // 2-cycle: the first cycle's alpha (the alpha compare's)
    reg         c0f11a, c0f11;       // ... and whether it counts 256 (an alpha of 255 does: the coverage's multiply)
    always @(posedge clk) begin
        v11a <= v10 & ~rst; x11a <= x10; bs11a <= bs10;
        o11a <= o1_w;
        tex11a <= tex10; shade11a <= shade10; mem11a <= mem10;
        depth11a <= depth10; zold11a <= zold10;
        memb11a <= memb10; c0a11a <= cc_clamp(c010[8:0]); c0f11a <= cc_a256(c010[8:0]); hid11a <= hid10;
    end
    wire [35:0] c1_w = cc_arith1(o11a);
    reg        v11;
    reg [9:0]  x11;
    reg [31:0] comb11;
    reg [35:0] shade11;
    reg        af11;                // the combined alpha counts 256 for the coverage multiply (CVG_X_ALPHA)
    reg [15:0] depth11, zold11;
    reg [15:0] dlo11, dhi11;       // the depth less and plus the decal tolerance
    wire [16:0] dsum11a = {1'b0, depth11a} + {1'b0, z_tol};
    reg [31:0] mem11;
    always @(posedge clk) begin
        v11 <= v11a & ~rst; x11 <= x11a; bs11 <= bs11a;
        comb11 <= (cyc == 2'd2) ? tex11a : { cc_clamp(c1_w[35:27]), cc_clamp(c1_w[26:18]), cc_clamp(c1_w[17:9]), cc_clamp(c1_w[8:0]) };
        af11 <= (cyc != 2'd2) && cc_a256(c1_w[8:0]);
        shade11 <= shade11a; depth11 <= depth11a;
        dlo11 <= (depth11a < z_tol) ? 16'd0 : depth11a - z_tol;
        dhi11 <= (dsum11a[16]) ? 16'hFFFF : dsum11a[15:0];
        zold11 <= zold11a; mem11 <= mem11a;
        memb11 <= memb11a; c0a11 <= c0a11a; c0f11 <= c0f11a; hid11 <= hid11a;
    end
    wire [3:0] cvg11 = cvd[63:60];

    // ======================================================================
    // the N64's alpha, coverage and depth (lang/c/mrdp/mrdp.c draw_pixel_cvg)
    // ======================================================================
    // The row's low bits, carried along with its pixels (a row's last pixels
    // are still in the pipe when the next row's y is issued): a 20 x 2 shift
    // register. ysh[2k+1:2k] is row_y k + 1 clocks on: P11 is 16 (the alpha
    // dither), the output stage's x13b 21 (the RGB dither).
    reg [41:0] ysh;
    always @(posedge clk) ysh <= {ysh[39:0], dup0 ? ysh[1:0] : row_y};     // (the second pass of a pixel: its row, not the next one's)
    wire [1:0] dy11 = ysh[31:30];
    wire [1:0] dy13 = ysh[41:40];
    // the alpha dither: the matrix the RGB dither's RGB_DITHER_SEL names for
    // the alpha (the Bayer matrix for 1 and 3, the magic square for 0 and 2:
    // Angrylion's get_dither_noise, the console's dither fetch) as
    // ALPHA_DITHER_SEL takes it -- the pattern, its inverse, noise (three
    // random bits), none. Copy mode: none.
    // The noise: a 23-bit LFSR (the console's taps), one step for each pixel
    // with coverage that passes this stage (lang/c/mrdp/mrdp.c steps it at the
    // same pixels): the alpha dither's noise and the alpha compare's random
    // threshold (DITHER_ALPHA_EN: the N64 compares with a random 8-bit
    // number, not with a pattern -- a pattern drew a regular grid of a
    // translucent surface's texels of alpha 0)
    reg  [22:0] lfsr;
    always @(posedge clk)
        if (rst) lfsr <= 23'd0;
        else if (v11 && cvg11 != 4'd0 && cyc != 2'd3) lfsr <= {lfsr[21:0], ~(lfsr[22] ^ lfsr[18])};
    wire [2:0] ad_d;
    rdp_dither_tab adtab (.off(1'b0), .bayer(other_h[6]), .y(dy11), .x(x11[1:0]), .dt(ad_d));
    wire [2:0] adith = (cyc == 2'd2) ? 3'd0 : (other_h[5:4] == 2'd0) ? ad_d : (other_h[5:4] == 2'd1) ? ~ad_d
                     : (other_h[5:4] == 2'd2) ? lfsr[10:8] : 3'd0;
    function automatic [7:0] sat9;
        input [8:0] v;
        sat9 = v[8] ? 8'hFF : v[7:0];
    endfunction
    // CVG_X_ALPHA: the coverage scaled by the alpha, (a * cvg + 4) >> 8;
    // ALPHA_CVG_SEL: the alpha is the coverage (times the alpha); else the
    // alpha plus its dither. The alpha compare (2-cycle: the first cycle's).
    // An alpha of 255 counts 256 in the multiply: (256 * cvg + 4) >> 8 = cvg, >> 3 = cvg << 5 --
    // the coverage unscaled; so the product is used only below 255 (af11,
    // c0f11: the flags are registered, the select is not behind the multiplier).
    wire [11:0] cvgmul  = comb11[7:0] * cvg11 + 12'd4;
    wire [11:0] cvgmul0 = c0a11 * cvg11 + 12'd4;
    wire        cx_m    = cvg_x_a & ~af11;
    wire        cx_m0   = cvg_x_a & ~c0f11;
    wire [3:0]  cvg_s   = cx_m ? cvgmul[11:8] : cvg11;
    wire [7:0]  pa11 = !a_cvg_s ? sat9({1'b0, comb11[7:0]} + {6'd0, adith})
                     : cx_m ? sat9(cvgmul[11:3]) : sat9({cvg11, 5'd0});
    wire [7:0]  ca11 = (cyc != 2'd1) ? pa11
                     : !a_cvg_s ? sat9({1'b0, c0a11} + {6'd0, adith})
                     : cx_m0 ? sat9(cvgmul0[11:3]) : sat9({cvg11, 5'd0});
    // the shade's alpha is 9 bits (0..511): plus the dither, 255 from 256 up
    wire [9:0]  sha_s = {1'b0, shade11[8:0]} + {7'd0, adith};
    wire [7:0]  sha11 = (sha_s[9] | sha_s[8]) ? 8'hFF : sha_s[7:0];
    wire [7:0]  athr  = (cyc == 2'd2) ? 8'd128 : a_dith ? lfsr[7:0] : blend[7:0];   // (copy: the texel's alpha bit)
    // the memory's coverage: 6 where the colour image's pixel is complete, 7
    // where it is not read, else what this pixel leaves uncovered (mrdp.h: storage)
    wire [2:0]  memcvg = !(im_rd && need_mem) ? 3'd7 : {memb11, hid11};
    wire        ovf    = ({1'b0, memcvg} + cvg_s) >= 4'd8;
    // depth: in front; within the primitive's step nearer / farther; the far plane
    wire        zmax   = zold11 == 16'hFFFF;
    wire        zfront = depth11 < zold11;
    wire        znear  = zold11 >= dlo11;            // depth - dz <= old
    wire        zfar   = zold11 <= dhi11;            // depth + dz >= old
    // interpenetrating: the coverage scaled by how far in front it is, in
    // steps of 2 dz: ((old >> s) - (depth >> s)) & 15, s = the step's top bit + 1
    reg  [4:0]  zsft;                // (static: the primitive's)
    integer zb;
    always @(posedge clk) begin
        zsft <= 5'd1;
        for (zb = 1; zb < 16; zb = zb + 1) if (z_tol[zb]) zsft <= zb[4:0] + 5'd1;
    end
    wire [19:0] zdif  = ({4'd0, zold11} >> zsft) - ({4'd0, depth11} >> zsft);

    // ======================================================================
    // P11b: the first half registered -- the coverage, the alphas, the
    // memory's coverage and the overflow, the depth compares. The second half
    // (the depth rules, the interpenetration, the blend enable, the coverage
    // written, the blender's operands) is P12a's: as one stage, from the
    // combiner's alpha to the coverage bit, it missed the 62.5 MHz clock by
    // up to 1.3 ns (fits of integ 633246f).
    // ======================================================================
    reg        v11b, bs11b;
    reg [9:0]  x11b;
    reg [31:0] comb11b, mem11b;
    reg [35:0] shade11b;
    reg [15:0] depth11b;
    reg [3:0]  cvg_sb, zdifb;
    reg [7:0]  pa11b, ca11b, sha11b, athr11b;
    reg [2:0]  mcvg11b;
    reg        ovf11b, zmax11b, zfront11b, znear11b, zfar11b, k0_11b;
    always @(posedge clk) begin
        v11b <= v11 & ~rst; x11b <= x11; bs11b <= bs11;
        comb11b <= comb11; mem11b <= mem11; shade11b <= shade11; depth11b <= depth11;
        cvg_sb <= cvg_s; zdifb <= zdif[3:0];
        pa11b <= pa11; ca11b <= ca11; sha11b <= sha11; athr11b <= athr;
        mcvg11b <= memcvg; ovf11b <= ovf;
        zmax11b <= zmax; zfront11b <= zfront; znear11b <= znear; zfar11b <= zfar;
        k0_11b <= cvg11 == 4'd0;
    end
    wire        zinter = zmode == 2'd1 && zfront11b && zfar11b && ovf11b;
    wire        zp     = zinter ? 1'b1 : (zmode == 2'd2) ? (zfront11b || zmax11b)
                       : (zmode == 2'd3) ? (zfar11b && znear11b && !zmax11b)
                       : ovf11b ? (zfront11b || zmax11b) : (znear11b || zmax11b);
    wire        zpass11 = !z_cmp || zp;
    wire [7:0]  zcm   = cvg_sb * zdifb;
    wire [3:0]  cvg_i = (z_cmp && zinter) ? zcm[6:3] : cvg_sb;
    wire        blend_en = force_bl || (!ovf11b && aa_en && (!z_cmp || zfar11b));
    // no covered sample: not drawn (AA_EN, or CVG_X_ALPHA's cut-outs)
    wire        kill11 = k0_11b || (cvg_i == 4'd0 && (aa_en || cvg_x_a) && cyc != 2'd2);
    // the coverage written (CVG_DST: clamp, wrap, zap, save) and so the bit 0
    wire [4:0]  csum = {1'b0, cvg_i} + {2'd0, mcvg11b};
    reg  [2:0]  cvo;
    always @(*) begin
        case (cvg_dst)
            2'd0: cvo = blend_en ? ((csum >= 5'd8) ? 3'd7 : csum[2:0]) : (cvg_i[2:0] - 3'd1);
            2'd1: cvo = csum[2:0];
            2'd2: cvo = 3'd7;
            default: cvo = mcvg11b;
        endcase
    end
    // (copy mode: the texel's alpha bit, 7 or 0)
    wire [2:0]  cv311 = (cyc == 2'd2) ? {3{comb11b[7]}} : cvo;
    // without a depth compare the memory coverage's weight shifts by the
    // primitive's dz (the N64's dzpixenc, ours + 3, up to 15): (static)
    reg  [2:0]  shb;
    always @(posedge clk) begin : shb_calc
        reg [4:0] e, d;
        integer q;
        e = 5'd0;
        if (z_src_p) begin
            for (q = 1; q < 16; q = q + 1) if (prim_dz[q]) e = q[4:0];
        end else if (p_zbuf) begin
            e = 5'd3;
            for (q = 1; q < 16; q = q + 1) if (z_tol[q]) e = q[4:0] + 5'd3;
        end
        if (e > 5'd15) e = 5'd15;
        d = 5'd15 - e;
        shb <= z_cmp ? 3'd0 : (e >= 5'd12) ? d[2:0] : 3'd4;
    end

    // ======================================================================
    // blender: P * A5 + M * (B5 + 1), the N64's
    // ======================================================================
    function automatic [31:0] bl_pm;
        input [1:0]  sel;
        input [31:0] pix, mem;
        case (sel)
            2'd0: bl_pm = pix;
            2'd1: bl_pm = mem;
            2'd2: bl_pm = blend;
            default: bl_pm = fog;
        endcase
    endfunction
    // one cycle's operands {P rgb 24, M rgb 24, A5 5, B5 + 1 6, divisor 4,
    // partial reject 1}: A5 the A alpha's top five bits, B5 the B's (B =
    // 1 - A: ~A5; memory alpha, the coverage: {a[7:5], 00} and {memcvg >>
    // shb, 11}); the divisor A5 / 4 + B5 / 4 + 1; the partial reject: P with
    // 1 - its own alpha at 255 is P
    function automatic [63:0] bl_sel;
        input        cy;
        input [31:0] pix, mem;
        input [7:0]  pa, sha;
        input [2:0]  mcvg, sb;
        reg [1:0] p, a, m, b;
        reg [7:0] av;
        reg [4:0] a5, b5;
        reg [2:0] mc;
        reg [31:0] P, M;
        begin
            p = cy ? other_l[29:28] : other_l[31:30];
            a = cy ? other_l[25:24] : other_l[27:26];
            m = cy ? other_l[21:20] : other_l[23:22];
            b = cy ? other_l[17:16] : other_l[19:18];
            case (a)
                2'd0: av = pa;
                2'd1: av = fog[7:0];
                2'd2: av = sha;
                default: av = 8'd0;
            endcase
            mc = mcvg >> sb;
            a5 = (b == 2'd1) ? {av[7:5], 2'b00} : av[7:3];
            case (b)
                2'd0: b5 = ~av[7:3];
                2'd1: b5 = {mc, 2'b11};
                2'd2: b5 = 5'd31;
                default: b5 = 5'd0;
            endcase
            P = bl_pm(p, pix, mem);
            M = bl_pm(m, pix, mem);
            bl_sel = { P[31:8], M[31:8], a5, {1'b0, b5} + 6'd1,
                       {1'b0, a5[4:2]} + {1'b0, b5[4:2]} + 4'd1, (a == 2'd0 && b == 2'd0 && av == 8'hFF) };
        end
    endfunction
    function automatic [13:0] bl_mac;        // P * A5 + M * (B5 + 1) (< 2^14)
        input [7:0] p, m;
        input [4:0] a;
        input [5:0] b;
        reg [13:0] pp, mm;
        begin
            pp = p * a;
            mm = m * b;
            bl_mac = pp + mm;
        end
    endfunction
    function automatic [41:0] bl_sum;        // three channels, 14 bits each
        input [63:0] o;
        bl_sum = { bl_mac(o[63:56], o[39:32], o[15:11], o[10:5]),
                   bl_mac(o[55:48], o[31:24], o[15:11], o[10:5]),
                   bl_mac(o[47:40], o[23:16], o[15:11], o[10:5]) };
    endfunction
    // ~2^17 / k (the blender's multipliers)
    function automatic [17:0] bl_recip;
        input [3:0] k;
        case (k)
            4'd0: bl_recip = 18'h3ffff;  4'd1: bl_recip = 18'h20001;  4'd2: bl_recip = 18'h10001;  4'd3: bl_recip = 18'h0aaab;
            4'd4: bl_recip = 18'h08001;  4'd5: bl_recip = 18'h06667;  4'd6: bl_recip = 18'h05556;  4'd7: bl_recip = 18'h04925;
            4'd8: bl_recip = 18'h04001;  4'd9: bl_recip = 18'h038e4;  4'd10: bl_recip = 18'h03334; 4'd11: bl_recip = 18'h02e8c;
            4'd12: bl_recip = 18'h02aab; 4'd13: bl_recip = 18'h02763; 4'd14: bl_recip = 18'h02493; default: bl_recip = 18'h02223;
        endcase
    endfunction

    // ======================================================================
    // P12a: tests, blender cycle 0 operands; P12b: its sums
    // ======================================================================
    wire        door11 = 1'b0;
    wire arej11 = (a_cmp && ca11b < athr11b) || door11;
    wire [63:0] bo0_w = bl_sel(1'b0, comb11b, mem11b, pa11b, sha11b, mcvg11b, shb);
    reg        v12a;
    reg [9:0]  x12a;
    reg [63:0] bo12a;
    reg [31:0] comb12a, mem12a;
    reg [7:0]  pa12a, sha12a;
    reg [2:0]  mcvg12a;
    reg [15:0] depth12a;
    reg        pass12a, ben12a, ovf12a;
    reg [2:0]  cv312a, cv312, cv313, cv313b;
    always @(posedge clk) begin
        v12a <= v11b & ~rst; x12a <= x11b; bs12a <= bs11b;
        bo12a <= bo0_w;
        comb12a <= comb11b; mem12a <= mem11b; pa12a <= pa11b; sha12a <= sha11b; mcvg12a <= mcvg11b;
        depth12a <= depth11b;
        pass12a <= (cyc == 2'd3) ? !k0_11b : (zpass11 && !arej11 && !kill11);
        ben12a <= blend_en; ovf12a <= ovf11b; cv312a <= cv311;
    end
    wire [41:0] s0_w = bl_sum(bo12a);
    reg        v12;
    reg [9:0]  x12;
    reg [23:0] b0rgb12;
    reg [31:0] comb12, mem12;
    reg [7:0]  pa12, sha12;
    reg [2:0]  mcvg12;
    reg [15:0] depth12;
    reg        pass12, ben12, ovf12;
    always @(posedge clk) begin
        v12 <= v12a & ~rst; x12 <= x12a; bs12 <= bs12a;
        b0rgb12 <= { s0_w[40:33], s0_w[26:19], s0_w[12:5] };
        comb12 <= comb12a; mem12 <= mem12a; pa12 <= pa12a; sha12 <= sha12a; mcvg12 <= mcvg12a;
        depth12 <= depth12a; pass12 <= pass12a; ben12 <= ben12a; ovf12 <= ovf12a; cv312 <= cv312a;
    end
    // 2-cycle: the first cycle always blends, >> 5 (8 bits: the sum wraps)
    wire [31:0] b012 = { b0rgb12, comb12[7:0] };

    // ======================================================================
    // P13a: the last blender cycle's operands -- 2-cycle's second on the
    // first's result, or 1-cycle's (cycle 0's settings) on the pixel; P13b:
    // its sums; out: the result
    // ======================================================================
    wire [63:0] bo1_w = bl_sel(cyc == 2'd1, (cyc == 2'd1) ? b012 : comb12, mem12, pa12, sha12, mcvg12, shb);
    reg        v13a;
    reg [9:0]  x13a;
    reg [63:0] bo13a;
    reg [31:0] comb13;
    reg [15:0] depth13;
    reg        pass13, ben13, ovf13;
    always @(posedge clk) begin
        v13a <= v12 & ~rst; x13a <= x12; bs13a <= bs12;
        bo13a <= bo1_w;
        comb13 <= comb12;
        depth13 <= depth12; pass13 <= pass12; ben13 <= ben12; ovf13 <= ovf12; cv313 <= cv312;
    end
    wire [41:0] s1_w = bl_sum(bo13a);
    reg        v13b;
    reg [9:0]  x13b;
    reg        bs13b;
    reg [41:0] fsum;
    reg [47:0] fpm;                 // the last cycle's P, M
    reg [17:0] frc;                 // the divisor's reciprocal
    reg        fblend, fdiv, fclr;
    reg [31:0] comb13b;
    reg [15:0] depth13b;
    reg        pass13b;
    always @(posedge clk) begin
        v13b <= v13a & ~rst; x13b <= x13a; bs13b <= bs13a;
        fsum <= s1_w;
        fpm <= bo13a[63:16];
        frc <= bl_recip(bo13a[4:1]);
        fblend <= ben13 && !bo13a[0];
        fdiv <= ben13 && !bo13a[0] && !force_bl;
        fclr <= clr_cvg && !ovf13;
        comb13b <= comb13; depth13b <= depth13; pass13b <= pass13; cv313b <= cv313;
    end
    // CLR_ON_CVG without overflow: M; not blending: P; blending: the sum >> 5,
    // or (not FORCE_BL) divided -- its bits [13:2] by the reciprocal, [24:17]
    function automatic [7:0] bl_out;
        input [13:0] sum;
        input [7:0]  p, m;
        reg   [29:0] q;
        begin
            q = sum[13:2] * frc;
            bl_out = fclr ? m : !fblend ? p : fdiv ? q[24:17] : sum[12:5];
        end
    endfunction
    wire [23:0] bl_w = { bl_out(fsum[41:28], fpm[47:40], fpm[23:16]),
                         bl_out(fsum[27:14], fpm[39:32], fpm[15:8]),
                         bl_out(fsum[13:0],  fpm[31:24], fpm[7:0]) };
    wire [31:0] fin = (cyc == 2'd3) ? 32'd0 : (cyc == 2'd2) ? comb13b : {bl_w, 8'd0};
    // The RGB dither (the N64's: RGB_DITHER_SEL, other_h[7:6]): the blender's
    // 8 bits go to 5 by truncation, after the three low bits are compared with
    // a 3-bit threshold -- where the threshold is below them the channel goes
    // up a step. The threshold: a 4 x 4 matrix of the pixel's place on the
    // screen (0: the magic square, 1: the Bayer matrix: a pixel an eighth of
    // the way to the next step goes up in one of eight places of the matrix),
    // or noise (2: three random bits for each channel, the LFSR's [2:0], [5:3]
    // and [8:6] as the pixel's alpha dither sees it, carried here in nz1..nz5:
    // the console's dither calculation -- OoT's dust, SETUPDL_0, G_CD_NOISE, was
    // drawn as an ordered checker screen with the matrix in its place); 3: off.
    // Without it a smooth shaded or lit surface is cut into flat bands of one
    // of 32 levels. Copy and fill write their colors untouched. The three low
    // bits and the thresholds go on to the output (out_color is their sum, a
    // 5-bit step and a compare behind the registers: not on the blender's path).
    wire [2:0] dt13;
    rdp_dither_tab dtab (.off(other_h[7:6] == 2'd3 || cyc[1]), .bayer(other_h[6]),
                         .y(dy13), .x(x13b[1:0]), .dt(dt13));
    // the noise: the LFSR at the pixel's P11 (the sample before its step), five clocks to P13b
    reg [8:0] nz1, nz2, nz3, nz4, nz5;
    always @(posedge clk) begin nz1 <= lfsr[8:0]; nz2 <= nz1; nz3 <= nz2; nz4 <= nz3; nz5 <= nz4; end
    wire dnoise = other_h[7:6] == 2'd2 && !cyc[1];
    reg [14:0] col_t;                           // R5 G5 B5, truncated
    reg [8:0]  col_lo;                          // the low three bits of R, G, B
    reg [2:0]  col_dt, col_dg, col_db;
    reg        col_a;
    wire [4:0] dr, dg, db;
    rdp_dither_add dar (.c(col_t[14:10]), .lo(col_lo[8:6]), .d(col_dt), .o(dr));
    rdp_dither_add dag (.c(col_t[9:5]),   .lo(col_lo[5:3]), .d(col_dg), .o(dg));
    rdp_dither_add dab (.c(col_t[4:0]),   .lo(col_lo[2:0]), .d(col_db), .o(db));
    assign out_color = { dr, dg, db, col_a };
    always @(posedge clk) begin
        out_valid <= v13b & ~rst;
        out_x <= x13b; out_set <= bs13b;
        col_t  <= (cyc == 2'd3) ? (x13b[0] ? fill[15:1] : fill[31:17])
                                : { fin[31:27], fin[23:19], fin[15:11] };
        col_a  <= (cyc == 2'd3) ? (x13b[0] ? fill[0] : fill[16]) : cv313b[2];
        out_hid <= (cyc == 2'd3) ? {2{x13b[0] ? fill[0] : fill[16]}} : cv313b[1:0];
        col_lo <= (cyc == 2'd3) ? 9'd0 : { fin[26:24], fin[18:16], fin[10:8] };
        col_dt <= dnoise ? nz5[2:0] : dt13;
        col_dg <= dnoise ? nz5[5:3] : dt13;
        col_db <= dnoise ? nz5[8:6] : dt13;
        out_depth <= depth13b;
        out_cpass <= pass13b;
        out_zpass <= pass13b && z_upd;
    end

    assign busy = v0 | v1 | v2 | v3 | v4 | v5 | v6 | v7 | v8 | v8t | v8b | v9 | v10a | v10 | v11a | v11 | v11b | v12a | v12 | v13a | v13b | out_valid;
endmodule

// the dither's threshold at a place of the 4 x 4 matrix; 7 turns it off (no
// three-bit value is above 7)
module rdp_dither_tab (
    input  wire       off, bayer,
    input  wire [1:0] y, x,
    output wire [2:0] dt
);
    reg [2:0] m, b;
    always @(*) begin
        case ({y, x})
            4'h0: m = 3'd0;  4'h1: m = 3'd6;  4'h2: m = 3'd1;  4'h3: m = 3'd7;
            4'h4: m = 3'd4;  4'h5: m = 3'd2;  4'h6: m = 3'd5;  4'h7: m = 3'd3;
            4'h8: m = 3'd3;  4'h9: m = 3'd5;  4'hA: m = 3'd2;  4'hB: m = 3'd4;
            4'hC: m = 3'd7;  4'hD: m = 3'd1;  4'hE: m = 3'd6;  default: m = 3'd0;
        endcase
        case ({y, x})
            4'h0: b = 3'd0;  4'h1: b = 3'd4;  4'h2: b = 3'd1;  4'h3: b = 3'd5;
            4'h4: b = 3'd4;  4'h5: b = 3'd0;  4'h6: b = 3'd5;  4'h7: b = 3'd1;
            4'h8: b = 3'd3;  4'h9: b = 3'd7;  4'hA: b = 3'd2;  4'hB: b = 3'd6;
            4'hC: b = 3'd7;  4'hD: b = 3'd3;  4'hE: b = 3'd6;  default: b = 3'd2;
        endcase
    end
    assign dt = off ? 3'd7 : bayer ? b : m;
endmodule

// a channel's top five bits, one step up where the threshold is below the
// low three bits -- except at the top (the N64 adds 7 - low, not 8 - low,
// above 247: 255 stays 255)
module rdp_dither_add (
    input  wire [4:0] c,
    input  wire [2:0] lo, d,
    output wire [4:0] o
);
    assign o = (d < lo && c != 5'd31) ? c + 5'd1 : c;
endmodule
