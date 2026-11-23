// RDP pixel pipeline: lang/c/rdp/rdp.c draw_pixel() built with RDP_N64, one
// pixel per clock, no stalls (every operand is on chip: TMEM and the span's
// line buffers). Stage by stage the arithmetic is the model's; the model is
// the contract. (From an earlier rasterizer's pixel pipeline: the texture unit
// reads the N64's TMEM and decodes its formats, the colour image is RGBA5551.)
//
// The N64's coverage (the model's RDP_CVG, lang/c/rdp/rdp.h): each pixel
// comes with its 8-sample mask (rdp_top's walker; all eight for a rectangle
// or a triangle without AA_EN); the alpha, the depth test, the blender and the
// coverage written (the colour image's bit 0, "complete") are the N64's.
//
// Per-primitive state must stay stable while pixels are in flight (the
// sequencer drains the pipe before a state command).
//
// TEXEL1: a 2-cycle pixel whose first combiner cycle reads TEXEL1, or whose second reads TEXEL0 (the N64's second cycle takes the
// first cycle's TEXEL1 there: a golf course's ground, an I8 detail tile times the RGBA16 grass, then the grass itself) (two_pass:
// the skybox's day / night blend, a terrain's second layer) goes through the
// texture stages twice, a clock apart -- the tile's sample, then the next
// tile's (the same S, T) -- and leaves P9 once, with both. rdp_top
// issues such a pixel every second clock (pix_go); P0 repeats it for the
// second pass (dup0). The tile's fields are read for each pass (tl_ra names
// the tile a clock before P5, rdp_top's tile RAMs answer in t_*), and each
// stage takes them from the pass's own read, carried down with it.
//
// LOD (lod_on; lang/c/rdp/rdp.c rdp_lod, docs/lod.md): a 2-cycle primitive
// with TEX_LOD, or whose combiner reads LOD_FRACTION. Its pixels take two
// clocks, and P0's second load is the pixel on the next line (rdp_top's S, T,
// W plus DaDy): its perspective divide is the LOD's next line. Its next pixel
// is the neighbour's own perspective divide -- rdp_top issues the pixels in
// the walk's order and adds a ghost (through the perspective divide, never
// drawn) where the walk's neighbour is not issued in time or is outside the
// span. The distance, the level, the tiles
// and the fraction take three stages after P4 (L1, L2, L3), and only then can
// the pixel's texture passes start: its S, T wait in a RAM (st_ram), and P4
// takes them back D clocks late (D 5 for a walk to the right, 4 to the left).
// What the texture stages do not read (x, the line buffer set, the shade, the
// depth, the coverage, the row) goes from P1 to P8b through a RAM (sb_ram)
// read 9 clocks after it is written, or 9 + D: without the LOD every pixel
// takes the clocks it took before, and only S, T at P4 choose between two
// sources.
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
    input  wire [8:0]  k4, k5,      // SET CONVERT's K4 (the combiner's B 7) and K5 (C 15)
    input  wire [35:0] cv_k03,      // SET CONVERT's K0..K3, 9 bits each (K0 at [35:27]): a YUV texel's conversion
    input  wire [15:0] prim_z,
    input  wire [15:0] z_tol,       // the primitive's depth step (a decal's tolerance; the coverage's dz)
    input  wire [15:0] prim_dz,     // SET_PRIM_DEPTH's delta
    input  wire        need_mem,    // the colour span is read (rdp_top): the memory's coverage is there
    input  wire        c8,          // the colour image is an 8-bit one (rdp_top cimg8): no depth, memory reads 0
    input  wire        c32,         // ... a 32-bit one (rdp_top cimg32): a pixel a word of the old colour buffer
    input  wire        p_aa,        // a triangle: Z moves to the first covered sample
    input  wire [31:0] z_dx, z_dy,  // its DzDx, DzDy
    input  wire [31:0] fill,
    input  wire        p_shade, p_tex, p_zbuf, p_direct,   // p_direct: S,T are s10.5 already
    input  wire [2:0]  t_fmt,
    input  wire [1:0]  t_size,
    input  wire        dither_off,  // the core's "Video settings" 32-bit option: the RGB dither is off whatever RGB_DITHER_SEL says (a quasi-static level)
    input  wire [3:0]  t_pal,
    input  wire [8:0]  t_line,
    input  wire [8:0]  t_tmem,
    input  wire        t_cs, t_ms, t_ct, t_mt,
    input  wire [3:0]  t_masks, t_shifts, t_maskt, t_shiftt,
    input  wire [11:0] t_uls, t_ult, t_lrs, t_lrt,
    // (t_*: the fields of the tile tl_ra named the clock before -- rdp_top's tile RAMs, one read a clock: a pass's tile)
    output wire [2:0]  tl_ra,
    input  wire [2:0]  p_tile,      // the primitive's tile (TEXEL1's: the next)
    input  wire [7:0]  yuv_vec,     // by tile: a YUV 16-bit one (the tile a converting pass checks)
    output wire        two_pass,    // the primitive's pixels go through the texture stages twice (rdp_top issues one every second clock)
    // the LOD (rdp_top: stable for a primitive)
    input  wire        lod_on,      // the primitive's pixels take the LOD (and two clocks each)
    input  wire        lod_lft,     // its walk goes to the right: the next pixel is x + 1 (else x - 1)
    input  wire [2:0]  lod_lvl,     // its level count (the triangle's level field: the last level; a rectangle 0)
    input  wire [4:0]  min_lvl,     // SET_PRIM_COLOR's minimum level
    input  wire        in_ghost,    // (with in_valid) a ghost: through the perspective divide for its neighbour's LOD, never drawn

    // ---- pixels in
    input  wire        in_valid,
    input  wire [9:0]  in_x,
    input  wire        in_set,           // the row's line-buffer set
    input  wire [1:0]  row_y,                  // the row (its low bits), steady while its pixels run
    input  wire [31:0] in_r, in_g, in_b, in_a, in_w, in_z,
    input  wire [15:0] in_s, in_t,           // S and T in 16 bits: the integer halves the perspective divide takes, or a rectangle's s10.5
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
    output reg  [9:0]  clb_raddr,         // the colour buffer's: a 32-bit image's {set, x[8:0]}, a 16-bit one's as lb_raddr
    input  wire [31:0] zlb_rdata,
    input  wire [31:0] clb_rdata,
    output reg  [9:0]  slb_raddr,         // the old shadow buffer's: {set, the colour word's address over two}
    input  wire [31:0] slb_rdata,         // ... its plane word, a byte a pixel (docs/video_options.md; read when sh_rd, for a 16-bit image): the pair's first colour word's pixels in the upper half
    input  wire        sh_rd,             // the core's shadow plane is on: a 16-bit pixel's memory colour takes the shadow byte's low bits where its check is the word's
    // the raw hidden-bit buffer (rdp_mem, lang/c/rdp/rdp.h: the N64's other two
    // coverage bits, a 32-bit word a 16-pixel group, by group mod 128), read
    // as the line buffers: its group's word; the rows' word base bits, by set
    output reg  [7:0]  hl_raddr,
    input  wire [31:0] hl_rdata,
    input  wire [9:0]  rwb0, rwb1,
    output reg         out_valid,        // one pixel result
    output reg         out_set,
    output reg  [9:0]  out_x,
    output wire [15:0] out_color,      // RGBA5551, the RGB dithered (rdp_dither_add); bit 0 the coverage's top bit
    output reg  [7:0]  out_r8,         // an 8-bit image's byte: the red, not dithered (fill: the fill colour's byte)
    output wire [26:0] out_c27,        // a 32-bit image's pixel: {R8 G8 B8 cvg3}, the RGB dithered at 8 bits
    output reg  [1:0]  out_hid,        // the coverage's two hidden bits
    output wire [7:0]  out_sh,         // the shadow plane's byte for a 16-bit image's pixel (docs/video_options.md): {R[2:1], G[2:1], B[2] of the low three bits, the 3-bit check of out_color}
                                       // (with the dither off: out_color is {col_t, col_a}; a fill and a copy write the top bits of each channel again)
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
    // cc_eval1: both cycles' arithmetic, A, B, C as signed numbers (COMBINED, K4 as B, K5 and the LOD fraction as C)
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
        un565 = { v[15:11], 3'd0, v[10:6], 3'd0, v[5:1], 3'd0, 8'hFF };   // (the N64's: zeros below, not the top bits again -- lang/c/rdp/rdp.c unrgb565)
    endfunction

    // the shadow plane's check of a 16-bit colour word: CRC-3 (x^3 + x + 1), the top bit first (lang/c/rdp/rdp.c sh_crc)
    function [2:0] sh_crc;
        input [15:0] w;
        integer i;
        reg [2:0] c;
        reg fb;
        begin
            c = 3'd0;
            for (i = 15; i >= 0; i = i - 1) begin
                fb = c[2] ^ w[i];
                c = {c[1:0], 1'b0};
                if (fb) c = c ^ 3'd3;
            end
            sh_crc = c;
        end
    endfunction

    // the memory colour of a 16-bit image with a shadow plane (lang/c/rdp/rdp.c): the low three bits of each channel from the shadow
    // byte where its check is the colour word's CRC-3 (it keeps two of them for red and green, one for blue; the bit missing repeats the one above)
    function automatic [31:0] un565sh;
        input [15:0] v;
        input [7:0] s;
        reg [8:0] lo;
        begin
            lo = (sh_rd && s[2:0] == sh_crc(v)) ? {s[7], s[6], s[6], s[5], s[4], s[4], s[3], s[3], s[3]} : 9'd0;
            un565sh = { v[15:11], lo[8:6], v[10:6], lo[5:3], v[5:1], lo[2:0], 8'hFF };
        end
    endfunction

    // a 16-bit texel: RGBA5551, or IA 8 8
    function automatic [31:0] tex16;
        input        ia;
        input [15:0] v;
        tex16 = ia ? { v[15:8], v[15:8], v[15:8], v[7:0] }
                   : { v[15:11], v[15:13], v[10:6], v[10:8], v[5:1], v[5:3], {8{v[0]}} };
    endfunction
    // a texel as TMEM gives it (tmem_fetch): the 16-bit word it is in, the
    // upper half's word (32-bit; YUV's Y0 Y1 for its pair's U V), its place in
    // the word (YUV: s[0], Y0 or Y1)
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
                2'd2: texel = (fmt == 3'd1) ? { v, {2{sl[0] ? h[7:0] : h[15:8]}} }      // YUV: U V Y Y
                            : (fmt == 3'd3) ? tex16(1'b1, v) : (fmt <= 3'd2) ? tex16(1'b0, v) : {4{v[15:8]}};
                2'd1: texel = (fmt == 3'd3) ? { {3{b[7:4], b[7:4]}}, b[3:0], b[3:0] } : {4{b}};
                default: texel = (fmt == 3'd3) ? { {3{n[3:1], n[3:1], n[3:2]}}, {8{n[0]}} } : {4{n, n}};
            endcase
        end
    endfunction

    // ======================================================================
    // decoded modes
    // ======================================================================
    wire [1:0] cyc      = other_h[21:20];
    // (the cycle type's tests are registers, one for each use's fanout: cyc is the primitive's, steady while its pixels are in the pipe -- a
    // state command waits for the pipe to drain -- and its tests, with other_h's register behind them and far from the datapath, were the start
    // of the worst paths into bo13a, o11a, cvgmul and the output stage)
    reg        cyc0_q, cyc1_q, cyc2_q, cyc3_q, cyc23_q;     // 1-cycle (0), 2-cycle (1), copy (2), fill (3), copy or fill
    // (and the walk's direction, a copy beside the pixel pipe's stages that take it: rdp_top's lod_lft is a register of the sequencer's fanout,
    // and its path through the LOD's subtractions into mn_m / ml_m was -0.08 ns at 10.667 ns)
    reg        lft_q;
    always @(posedge clk) lft_q <= lod_lft;
    always @(posedge clk) begin
        cyc0_q <= cyc == 2'd0; cyc1_q <= cyc == 2'd1; cyc2_q <= cyc == 2'd2; cyc3_q <= cyc == 2'd3; cyc23_q <= cyc[1];
    end
    wire       persp    = other_h[19];
    wire       tlut_ia  = other_h[14];
    // TEXEL1 read by the first combiner cycle (rdp.c cc0_uses_texel1: the same fields)
    wire       cc0_t1   = cc_hi[23:20] == 4'd2 || cc_lo[31:28] == 4'd2 || cc_hi[19:15] == 5'd2 || cc_hi[19:15] == 5'd9 ||
                          cc_lo[17:15] == 3'd2 || cc_hi[14:12] == 3'd2 || cc_lo[14:12] == 3'd2 || cc_hi[11:9] == 3'd2 ||
                          cc_lo[11:9] == 3'd2;
    // TEXEL0 read by the second combiner cycle (rdp.c cc1_uses_texel0): the N64's second cycle takes the first cycle's TEXEL1 there,
    // the next tile's sample
    wire       cc1_t0   = cc_hi[8:5] == 4'd1 || cc_lo[27:24] == 4'd1 || cc_hi[4:0] == 5'd1 || cc_hi[4:0] == 5'd8 ||
                          cc_lo[8:6] == 3'd1 || cc_lo[23:21] == 3'd1 || cc_lo[5:3] == 3'd1 || cc_lo[20:18] == 3'd1 ||
                          cc_lo[2:0] == 3'd1;
    // (registered: the modes and the combiner are set many clocks before a primitive's first pixel)
    reg        two_pass_q;
    // (a primitive without texture coefficients too: the next tile's texel at its S, T, as the console's)
    always @(posedge clk) two_pass_q <= cyc == 2'd1 && (cc0_t1 || cc1_t0);
    assign two_pass = two_pass_q;
    wire       point    = other_h[13:12] == 2'd0 || other_h[21:20] == 2'd2;    // (copy mode has no filter)
    wire       a_cmp    = other_l[0];
    wire       z_src_p  = other_l[2];
    wire       z_cmp    = other_l[4] && !cyc23_q && !c8;    // (copy and fill mode have no depth, nor an 8-bit image)
    wire       z_upd    = other_l[5] && !cyc23_q && !c8;
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
    reg        bs0, bs9, bs9x, bs10a, bs10, bs10b, bs11a, bs11, bs12a, bs12, bs13a;
    reg [15:0] s0r, t0r;
    reg [31:0] w0r;
    reg [35:0] shade0;
    // the shade, as the interpolator gives it, saturated to a byte -- 0 below 0, 255 above -- and carried as 9 bits (bit 8 clear).
    // A shade past 255 (a highlight's overshoot at a lit surface's edge: a character's tunic lit by a moving light) never goes into the
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
    // (the offsets are 0..3 quarter pixels: the product is a choice among 0, the step, twice it and three times it, the last a register
    // of the clock before, the primitive's DzDx and DzDy being steady long before its pixels -- the adder was in the mask's way)
    reg signed [28:0] zdx3, zdy3;
    always @(posedge clk) begin zdx3 <= $signed(z_dx[31:5]) * 3; zdy3 <= $signed(z_dy[31:5]) * 3; end
    wire signed [28:0] zdx1 = $signed(z_dx[31:5]), zdy1 = $signed(z_dy[31:5]);
    wire signed [28:0] zcx = ox4 == 2'd0 ? 29'sd0 : ox4 == 2'd1 ? zdx1 : ox4 == 2'd2 ? (zdx1 <<< 1) : zdx3;
    wire signed [28:0] zcy = oy4 == 2'd0 ? 29'sd0 : oy4 == 2'd1 ? zdy1 : oy4 == 2'd2 ? (zdy1 <<< 1) : zdy3;
    // (the two products are registers, P0's; their sum, shifted, is added to Z where it is used: the products with the adds behind
    // them did not fit a clock of 10.667 ns)
    wire [3:0] cvg_in = {3'd0, in_mask[0]} + in_mask[1] + in_mask[2] + in_mask[3]
                      + in_mask[4] + in_mask[5] + in_mask[6] + in_mask[7];
    reg  [31:0] z0r;
    reg signed [28:0] zcx0, zcy0;
    wire [31:0] zc0 = ({{3{zcx0[28]}}, zcx0} + {{3{zcy0[28]}}, zcy0}) << 3;
    reg  [3:0]  cvg0;                // the coverage
    reg  [1:0]  y0;                  // the row's low bits (a row's last pixels are still in the pipe when the next row's y is issued)
    reg         g0;                  // a ghost (LOD)
    reg         dup0, ph0;           // two_pass, LOD: the pixel in these registers a second clock (its second pass); ph0 marks that clock
    always @(posedge clk) begin
        dup0 <= in_valid & (two_pass | lod_on) & ~rst;
        v0 <= (in_valid | dup0) & ~rst;
        ph0 <= dup0;
        if (!dup0) begin             // (the second pass repeats the pixel as it is: rdp_top has gone on)
            x0 <= in_x; bs0 <= in_set;
            shade0 <= p_shade ? {sr9, sg9, sb9, sa9} : 36'd0;
            z0r <= in_z;
            zcx0 <= p_aa ? zcx : 29'sd0; zcy0 <= p_aa ? zcy : 29'sd0;
            cvg0 <= cvg_in; y0 <= row_y; g0 <= in_ghost;
        end
        // (a primitive without texture coefficients has S = T = W = 0 and samples the tile's texel (0, 0) all the same: the model's
        // draw_pixel_cvg; a character's collar is one. With the LOD the second clock takes S, T, W on the next line, which rdp_top
        // gives while it waits: S = T = W = 0 there too)
        if (!dup0 || lod_on) begin
            s0r <= p_tex ? in_s : 16'd0; t0r <= p_tex ? in_t : 16'd0; w0r <= p_tex ? in_w : 32'd0;
        end
    end
    wire [31:0] zin_c = z0r + zc0;
    // the depth: the N64's 18 bits. The depth image's word (the model's z_enc / z_dec): 14 of them as a floating-point number --
    // the exponent the count of the depth's leading ones, 0..7, the mantissa the 11 bits under them -- above the top two bits
    // of the pixel's dz code
    wire signed [31:0] zq = $signed(zin_c) >>> 13;
    wire [17:0] dz = zq < 0 ? 18'd0 : (zq > 32'sd262143 ? 18'h3FFFF : zq[17:0]);
    function automatic [13:0] z_enc;
        input [17:0] z;
        casez (z[17:11])
            7'b0??????: z_enc = {3'd0, z[16:6]};
            7'b10?????: z_enc = {3'd1, z[15:5]};
            7'b110????: z_enc = {3'd2, z[14:4]};
            7'b1110???: z_enc = {3'd3, z[13:3]};
            7'b11110??: z_enc = {3'd4, z[12:2]};
            7'b111110?: z_enc = {3'd5, z[11:1]};
            7'b1111110: z_enc = {3'd6, z[10:0]};
            default:    z_enc = {3'd7, z[10:0]};
        endcase
    endfunction
    function automatic [17:0] z_dec;
        input [15:0] w;
        case (w[15:13])
            3'd0: z_dec = {1'b0, w[12:2], 6'd0};
            3'd1: z_dec = {2'b10, w[12:2], 5'd0};
            3'd2: z_dec = {3'b110, w[12:2], 4'd0};
            3'd3: z_dec = {4'b1110, w[12:2], 3'd0};
            3'd4: z_dec = {5'b11110, w[12:2], 2'd0};
            3'd5: z_dec = {6'b111110, w[12:2], 1'd0};
            3'd6: z_dec = {7'b1111110, w[12:2]};
            default: z_dec = {7'b1111111, w[12:2]};
        endcase
    endfunction

    // What the texture stages do not read -- x, the line buffer set, the shade, the depth (P1's), the coverage, the row --
    // from P1 to P8c: a RAM written every clock and read 12 clocks after (as the stages between took), or with the LOD 12 + D
    // (its pixels reach P5 D clocks late: the pointers make the delay, nothing chooses between two paths). Two M10K
    // where the stages' registers were (and Quartus's shift registers made of them).
    wire [17:0] depth_c = z_src_p ? {prim_z, 2'b00} : p_zbuf ? dz : 18'd0;     // (a primitive without Z coefficients -- a rectangle -- has Z 0, the nearest, not the primitive depth: rdp_exact.md)
    reg  [4:0]  sb_wa;
    wire [4:0]  sb_ra = sb_wa - (lod_on ? (lft_q ? 5'd17 : 5'd16) : 5'd12);
    // (the shade's channels are bytes -- sr9 and the others clamp them -- so their ninth bits are not kept)
    (* ramstyle = "M10K, no_rw_check" *) reg [66:0] sb_ram [0:31];
    reg  [66:0] sb_q;
    always @(posedge clk) begin
        sb_ram[sb_wa] <= {x0, bs0, shade0[34:27], shade0[25:18], shade0[16:9], shade0[7:0], depth_c, cvg0, y0};
        sb_q <= sb_ram[sb_ra];
        sb_wa <= sb_wa + 5'd1;
    end
    wire [9:0]  x8c     = sb_q[66:57];
    wire        bs8c    = sb_q[56];
    wire [35:0] shade8c = {1'b0, sb_q[55:48], 1'b0, sb_q[47:40], 1'b0, sb_q[39:32], 1'b0, sb_q[31:24]};
    wire [17:0] depth8c = sb_q[23:6];
    wire [3:0]  cvg8c   = sb_q[5:2];
    wire [1:0]  y8c     = sb_q[1:0];

    // ======================================================================
    // P1: normalise W, the reciprocal's table
    // ======================================================================
    // The console's perspective divide (lang/c/rdp/rdp.c n64_div): the 16-bit integer halves of S, T (s10.5) and W
    // (s.15). W's leading zeros, up to 14, are shifted out of its low 15 bits; its next 6 bits pick a point of the
    // reciprocal and a slope, the 8 after them step along it (rdp_div_rom: the console's two tables)
    wire [14:0] wi0 = w0r[30:16];
    reg  [3:0]  sh_c;
    integer k;
    always @(*) begin
        sh_c = 4'd14;
        for (k = 0; k <= 14; k = k + 1)
            if (wi0[k]) sh_c = 4'd14 - k[3:0];
    end
    wire [14:0] wn = wi0 << sh_c;
    wire [14:0] dv_pt;
    wire [9:0]  dv_sl;
    rdp_div_rom drom (.idx(wn[13:8]), .point(dv_pt), .slope(dv_sl));

    // the pass of the pixel in each stage (0 the tile's, 1 the next tile's: two_pass; with the LOD, up to P4, 1 the next
    // line's perspective divide) and the tile fields the stage takes, carried with it from the pass's read (t_*, at P5a)
    reg        ph1, ph1b, ph2, ph3, ph4, ph5, ph6, ph7, ph8, ph8t, ph8b;
    reg [3:0]  u5_masks, u5_maskt;
    reg [11:0] u5_uls, u5_ult, u5_lrs, u5_lrt;
    reg        u5_cs, u5_ms, u5_ct, u5_mt;
    reg [8:0]  c5_tmem, c5_line, u6_tmem, u6_line;
    reg [1:0]  c5_size, u6_size, c7_size, u8t_size;
    reg [3:0]  c5_pal, c6_pal, c7_pal;
    reg [2:0]  c5_fmt, c6_fmt, c7_fmt, c8_fmt;
    reg        c5_yuv, u6_yuv;             // a YUV 16-bit tile: a pixel pair a word in each half, as a 32-bit one
    // the pass converts YUV instead of filtering (rdp.c texture_pass): copy mode never; its BI_LERP bit (other_h 11 for the
    // tile's pass, 10 for the next tile's) off; the texel a YUV tile's -- the pass's own, or with CONVERT_ONE (other_h 9) the
    // second pass converts the first's texel (tex9, a clock after), and the tile checked is the one before the pass's (the first pass's
    // without the LOD; with it, as the model, the tile before TEXEL1's whatever TEXEL0's is)
    reg        conv5, conv6, conv7, conv8, conv8t, conv8b;
    reg        u8_size0, u8_size1;
    reg [3:0]  u8_pal;
    reg [2:0]  u8t_fmt;
    wire       tlut_on = other_h[15];
    reg        v1, g1, g2, g3, g4;
    reg        wle1, wle2, wle3;           // W's integer half is zero or negative: out of range, over (n64_div)
    reg [14:0] pt1;
    reg [9:0]  sl1;
    reg [7:0]  f1;
    reg [3:0]  sh1;
    reg [15:0] s1r, t1r;
    always @(posedge clk) begin
        v1 <= v0 & ~rst; ph1 <= ph0; g1 <= g0;
        pt1 <= dv_pt; sl1 <= dv_sl; f1 <= wn[7:0]; sh1 <= sh_c;
        s1r <= s0r; t1r <= t0r;
        wle1 <= w0r[31] || w0r[30:16] == 15'd0;
    end

    // ======================================================================
    // P1b: the slope's step, (slope - 1023) * 4 frac, in a register of its own (a DSP block's product with the add behind it
    // did not fit a clock of 10.667 ns)
    // ======================================================================
    wire signed [10:0] dsl = $signed({1'b0, sl1}) - 11'sd1023;
    reg        v1b, g1b, wle1b;
    reg [14:0] pt1b;
    reg signed [21:0] dst1b;
    reg [3:0]  sh1b;
    reg [15:0] s1b, t1b;
    always @(posedge clk) begin
        v1b <= v1 & ~rst; ph1b <= ph1; g1b <= g1; wle1b <= wle1;
        pt1b <= pt1; dst1b <= dsl * $signed({1'b0, f1, 2'b00}); sh1b <= sh1;
        s1b <= s1r; t1b <= t1r;
    end

    // ======================================================================
    // P2: the reciprocal, 15 bits: the point plus the slope's step, ((slope - 1023) * 4 frac) >> 10 (a negative step)
    // ======================================================================
    // It is taken up (sh + 2) mod 3 places, so that the product's way back to s10.5, 13 - sh places, becomes a multiple of
    // 3 (R, P4: 0 to 15); sh 14 is the product taken up 1 (n64_div's prod * 2): R 0 as well. (17 bits at most, 0x4000
    // taken up 2: the products stay 18 x 18, two to a DSP block)
    wire [14:0] rcp1 = pt1b + {{3{dst1b[21]}}, dst1b[21:10]};
    function automatic [1:0] tk3;           // (sh + 2) mod 3
        input [3:0] sh;
        case (sh)
            4'd1, 4'd4, 4'd7, 4'd10, 4'd13: tk3 = 2'd0;
            4'd2, 4'd5, 4'd8, 4'd11, 4'd14: tk3 = 2'd1;
            default:                        tk3 = 2'd2;
        endcase
    endfunction
    wire [1:0]  rm1 = tk3(sh1b);
    reg        v2;
    reg [16:0] r2;
    reg [3:0]  sh2;
    reg [15:0] s2r, t2r;                 // S and T in 16 bits: the integer halves the perspective divide takes, or a rectangle's s10.5
    always @(posedge clk) begin
        v2 <= v1b & ~rst; ph2 <= ph1b; g2 <= g1b; wle2 <= wle1b;
        r2 <= {2'd0, rcp1} << rm1;
        sh2 <= sh1b;
        s2r <= s1b;
        t2r <= t1b;
    end

    // ======================================================================
    // P3: S * r, T * r
    // ======================================================================
    // (and for P4: R / 3, the take-up, and the product's bits n64_div's range takes -- its bits 29 down to the result's
    // top, here 29 + m down to 16 + R -- as a mask of bits 31:16)
    wire [1:0]  rm2 = tk3(sh2);
    wire [4:0]  rr2 = 5'd13 - {1'b0, sh2} + {3'd0, rm2};
    reg  [15:0] rk2;
    integer kk;
    always @(*) for (kk = 0; kk < 16; kk = kk + 1) rk2[kk] = kk >= rr2 && kk <= 13 + rm2;
    reg        v3;
    reg signed [32:0] ps3, pt3;
    reg [2:0]  rq3;
    reg [1:0]  rm3;
    reg [15:0] rk3;
    reg [15:0] s3r, t3r;
    always @(posedge clk) begin
        v3 <= v2 & ~rst; ph3 <= ph2; g3 <= g2; wle3 <= wle2;
        ps3 <= $signed(s2r) * $signed({1'b0, r2});
        pt3 <= $signed(t2r) * $signed({1'b0, r2});
        rq3 <= rr2 == 5'd15 ? 3'd5 : rr2 == 5'd12 ? 3'd4 : rr2 == 5'd9 ? 3'd3 : rr2 == 5'd6 ? 3'd2 : rr2 == 5'd3 ? 3'd1 : 3'd0;
        rm3 <= rm2; rk3 <= rk2;
        s3r <= s2r; t3r <= t2r;
    end

    // ======================================================================
    // P4: s10.5 coordinates (perspective, affine, or direct)
    // ======================================================================
    // the product down by R places, 17 bits (s11.5), and its range: out (2 over, 1 under) when the masked bits are not all
    // one value -- under when the top one, the product's bit 29, is set
    function automatic [18:0] dv17;          // {range 2, value 17}
        input signed [32:0] p;
        input [2:0]  q;
        input [1:0]  m;
        input [15:0] mk;
        reg [16:0] v;
        reg        mix, top;
        begin
            case (q)
                3'd0:    v = p[16:0];
                3'd1:    v = p[19:3];
                3'd2:    v = p[22:6];
                3'd3:    v = p[25:9];
                3'd4:    v = p[28:12];
                default: v = p[31:15];
            endcase
            mix = (|(p[31:16] & mk)) && !(&(p[31:16] | ~mk));
            top = m == 2'd0 ? p[29] : m == 2'd1 ? p[30] : p[31];
            dv17 = {mix && !top, mix && top, v};
        end
    endfunction
    // S / W for the texel (n64_persp): the 17 bits saturated to 16, out of range to the end it went past (a triangle
    // without texture coefficients too: its W is 0, out of range, so S and T are 0x7FFF -- the tile's far texel)
    function automatic [15:0] tx16;
        input [18:0] d;
        input        wle;
        tx16 = (wle || d[18]) ? 16'h7FFF : d[17] ? 16'h8000 : (d[16] != d[15]) ? (d[16] ? 16'h8000 : 16'h7FFF) : d[15:0];
    endfunction
    wire [18:0] dvs = dv17(ps3, rq3, rm3, rk3), dvt = dv17(pt3, rq3, rm3, rk3);
    // the coordinates P5 takes: without the LOD this pass's, from P3; with it a pass's from st_ram, D clocks on (below)
    wire [15:0] s4n = (p_direct || !persp) ? s3r : tx16(dvs, wle3);
    wire [15:0] t4n = (p_direct || !persp) ? t3r : tx16(dvt, wle3);
    // (the LOD's: the perspective divide, but a texture rectangle's S, T are its own s10.5, never out of range, as are an
    // affine triangle's S >> 16; a rectangle without texture is a triangle without coefficients: S = T = W = 0)
    wire        lod_pp = persp && !(p_direct && p_tex);
    reg         v4;
    reg  [15:0] s4, t4;
    reg  [16:0] s17_4, t17_4;       // the LOD's S, T (s11.5) of this perspective divide
    reg         oob4;               // ... out of range (n64_div)
    reg  [3:0]  rw_a;               // st_ram's, sb_ram's and lf_ram's write address: every clock
    reg  [31:0] st_q;
    (* ramstyle = "M10K, no_rw_check" *) reg [31:0] st_ram [0:15];
    wire        l3_v;
    // (P4 loads a pixel's first pass at D, its second at D + 1: read at D - 2 and D - 1, the second in L3's clock)
    wire [3:0]  st_ra = rw_a - (lft_q ? 4'd4 : 4'd3) - {3'd0, l3_v};
    always @(posedge clk) begin
        v4 <= v3 & ~rst; ph4 <= ph3; g4 <= g3;
        s4 <= lod_on ? st_q[31:16] : s4n; t4 <= lod_on ? st_q[15:0] : t4n;
        s17_4 <= lod_pp ? dvs[16:0] : {s3r[15], s3r};
        t17_4 <= lod_pp ? dvt[16:0] : {t3r[15], t3r};
        oob4 <= lod_pp && (wle3 || dvs[18] || dvs[17] || dvt[18] || dvt[17]);
        st_ram[rw_a] <= {s4n, t4n};
        st_q <= st_ram[st_ra];
        rw_a <= rw_a + 4'd1;
    end

    // ======================================================================
    // The LOD (lang/c/rdp/rdp.c rdp_lod, lod_dist): L1 the steps, L2 the
    // distance and its level, L3 the tiles (the first pass's read) and the
    // fraction
    // ======================================================================
    // L1. Each perspective divide's result at P4 is taken from the last pass A's (A: S, T at the pixel; B: on the next
    // line). A pixel's B less its A is its step to the next line. An A less the A before it is a step to the next pixel:
    // the pixel before's when the walk goes to the right (its next pixel follows it two clocks on: rdp_top), this pixel's
    // own, negated, when it goes to the left (the walk's next pixel came before it). A step's magnitude is the model's: a
    // negative one's ones' complement, |d| - 1; of a negated A - prev, -(A - prev) = ~(A - prev - 1), the same of
    // A - prev - 1 (a borrow in). Of each step the larger of S's and T's, in 14 bits, and whether one was 512 texels or
    // more, or a point out of range: the far level.
    reg  [16:0] pa_s, pa_t;          // the last pass A's
    reg         pa_o;
    wire        l1_ci = !ph4 && !lft_q;
    wire [17:0] l1_ds = {s17_4[16], s17_4} - {pa_s[16], pa_s} - {17'd0, l1_ci};
    wire [17:0] l1_dt = {t17_4[16], t17_4} - {pa_t[16], pa_t} - {17'd0, l1_ci};
    wire [16:0] l1_ms = l1_ds[17] ? ~l1_ds[16:0] : l1_ds[16:0];
    wire [16:0] l1_mt = l1_dt[17] ? ~l1_dt[16:0] : l1_dt[16:0];
    wire [13:0] l1_m  = (l1_ms[13:0] > l1_mt[13:0]) ? l1_ms[13:0] : l1_mt[13:0];
    wire        l1_f  = (|l1_ms[16:14]) || (|l1_mt[16:14]) || oob4 || pa_o;
    reg  [13:0] mn_m, ml_m;          // the steps to the next pixel (n) and to the next line (l)
    reg         mn_f, ml_f;
    always @(posedge clk) if (v4 && lod_on) begin
        if (ph4) begin
            ml_m <= l1_m; ml_f <= l1_f;
        end else begin
            mn_m <= l1_m; mn_f <= l1_f;
            pa_s <= s17_4; pa_t <= t17_4; pa_o <= oob4;
        end
    end
    // The drawn pixels' passes A (not ghosts), followed from P4: L3 at D - 1 clocks, the first pass at P5 (its S, T in P4
    // again) at D, the second at D + 1. D is 5 when the walk goes to the right (the next pixel's A is at P4 two clocks
    // after this one's, its step in mn a clock later), 4 to the left (the line's B a clock after, its step a clock later).
    reg  [6:1]  av;
    wire        av0 = v4 && !ph4 && !g4 && lod_on;
    always @(posedge clk) av <= rst ? 6'd0 : {av[5:1], av0};
    assign      l3_v = lft_q ? av[4] : av[3];
    wire        p5_a = lft_q ? av[5] : av[4];
    wire        p5_b = (lft_q ? av[6] : av[5]) && two_pass;
    // L2. The distance, the larger step (the model's lod: under 0x4000 here, else far); its level, the top bit's place in
    // its texels (bits 12:5, the ninth dropped as the model does), taken by the same compare from each step's own; under a
    // texel, magnified
    function automatic [2:0] lg8;
        input [7:0] v;
        lg8 = v[7] ? 3'd7 : v[6] ? 3'd6 : v[5] ? 3'd5 : v[4] ? 3'd4 : v[3] ? 3'd3 : v[2] ? 3'd2 : v[1] ? 3'd1 : 3'd0;
    endfunction
    wire        l2_ge = mn_m >= ml_m;
    reg  [13:0] l2_lod;
    reg  [2:0]  l2_lg;
    reg         l2_far, l2_mag;
    always @(posedge clk) begin
        l2_lod <= l2_ge ? mn_m : ml_m;
        l2_lg  <= l2_ge ? lg8(mn_m[12:5]) : lg8(ml_m[12:5]);
        l2_far <= mn_f || ml_f;
        l2_mag <= mn_m[13:5] == 9'd0 && ml_m[13:5] == 9'd0;
    end
    // L3. The level against the level count (far: the last level), the tiles (TEX_LOD: the primitive's tile plus the
    // level; detail a tile on; TEXEL1's the next, or the same at the far level and magnified without sharpen), and the
    // fraction: the 8 bits under the top bit, 0 or all ones where the next level is not blended, or magnified with sharpen
    // or detail the distance itself (no less than the minimum level; sharpen's negative). The first pass's tile goes to
    // rdp_top's tile RAMs now, the second's next clock.
    wire        tex_lod = other_h[16], sharpen = other_h[17], detail = other_h[18];
    wire        l3_mag = l2_mag && !l2_far;
    wire        l3_dis = l2_far || lod_lvl == 3'd0 || (!l2_mag && (l2_lod[13] || l2_lg >= lod_lvl));
    wire [2:0]  l3_lvl = l3_dis ? lod_lvl : l2_mag ? 3'd0 : l2_lg;
    wire [2:0]  l3_t0 = p_tile + (tex_lod ? l3_lvl + {2'd0, detail && !l3_mag} : 3'd0);
    wire        l3_inc = !tex_lod || (detail ? (l3_mag || !l3_dis) : !(l3_dis || (!sharpen && l3_mag)));
    reg  [2:0]  l3_t1;
    wire [4:0]  l3_mx = (l2_lod[4:0] < min_lvl) ? min_lvl : l2_lod[4:0];
    wire [14:0] l3_sh = {l2_lod[11:0], 3'd0} >> l2_lg;
    wire        l3_pl = !sharpen && !detail;
    wire [8:0]  l3_lf = l2_far ? 9'h0FF : l3_mag ? (l3_pl ? {1'b0, {8{l3_dis}}} : {sharpen, l3_mx, 3'd0})
                      : (l3_pl && l3_dis) ? 9'h0FF : {1'b0, l3_sh[7:0]};
    reg  [8:0]  lf;                  // the pixel's LOD_FRACTION: from its first pass at P5 to the next pixel's
    always @(posedge clk) begin
        l3_t1 <= l3_t0 + {2'd0, l3_inc};
        if (l3_v) lf <= l3_lf;
    end
    // the tile read for P5's pass, a clock ahead: L3's two, or (without the LOD) the pass at P3's
    assign tl_ra = l3_v ? l3_t0 : p5_a ? l3_t1 : ph3 ? p_tile + 3'd1 : p_tile;
    reg  [2:0]  tl_rq;               // (and P5's: its pass and its tile, for the conversion's check)
    reg         tl_p2;
    always @(posedge clk) begin tl_rq <= tl_ra; tl_p2 <= !l3_v && (p5_a || ph3); end

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
    wire signed [33:0] tcp_s = $signed(s4) * $signed({1'b0, tc_pow(t_shifts)});
    wire signed [33:0] tcp_t = $signed(t4) * $signed({1'b0, tc_pow(t_shiftt)});
    wire signed [23:0] tcs_s = tcp_s[33:10];
    wire signed [23:0] tcs_t = tcp_t[33:10];
    // (P5: the products, in registers of their own; P5m: their slices less the tile's origin, and the pass's tile fields a clock on --
    // the tile RAM's output through the power's decode and a DSP product in logic with the subtraction behind it was the worst path of
    // the texture stages at 10.667 ns)
    reg signed [33:0] tcp_sr, tcp_tr;
    wire signed [23:0] vs5 = tcp_sr[33:10] - $signed({9'd0, u5p_uls, 3'd0});
    wire signed [23:0] vt5 = tcp_tr[33:10] - $signed({9'd0, u5p_ult, 3'd0});
    reg        v5p, ph5p, conv5p;
    reg [3:0]  u5p_masks, u5p_maskt;
    reg [11:0] u5p_uls, u5p_ult, u5p_lrs, u5p_lrt;
    reg        u5p_cs, u5p_ms, u5p_ct, u5p_mt;
    reg [8:0]  c5p_tmem, c5p_line;
    reg [1:0]  c5p_size;
    reg [3:0]  c5p_pal;
    reg [2:0]  c5p_fmt;
    reg        c5p_yuv;
    reg        v5;
    reg signed [23:0] vs5r, vt5r;
    reg signed [10:0] mx5s, mx5t;           // each axis's tile's last texel (rdp_tcaxis)
    reg               ng5s, gx5s, ng5t, gx5t;
    // (the clamp's two tests, on the coordinate as P5m has it: the coordinate's sign, and its integer part against the tile's last texel)
    wire signed [10:0] mxn_s = mx_f(u5p_uls, u5p_lrs), mxn_t = mx_f(u5p_ult, u5p_lrt);
    wire ce5s = u5p_cs || u5p_masks == 4'd0, ce5t = u5p_ct || u5p_maskt == 4'd0;       // clamped
    function automatic signed [10:0] mx_f;
        input [11:0] lo, hi;
        reg signed [12:0] df;
        begin df = $signed({1'b0, hi}) - $signed({1'b0, lo}); mx_f = df[12:2]; end
    endfunction
    always @(posedge clk) begin
        // (with the LOD the passes come D clocks after their perspective divides: L3's)
        v5p <= (lod_on ? (p5_a || p5_b) : v4) & ~rst;
        ph5p <= lod_on ? !p5_a : ph4;
        // the clamp, mirror and mask of P5b, and what the later stages take of the pass's tile
        u5p_uls <= t_uls; u5p_ult <= t_ult; u5p_lrs <= t_lrs; u5p_lrt <= t_lrt;
        u5p_cs <= t_cs; u5p_ms <= t_ms; u5p_ct <= t_ct; u5p_mt <= t_mt;
        u5p_masks <= t_masks; u5p_maskt <= t_maskt;
        c5p_tmem <= t_tmem; c5p_line <= t_line; c5p_size <= t_size; c5p_pal <= t_pal; c5p_fmt <= t_fmt;
        c5p_yuv <= t_fmt == 3'd1 && t_size == 2'd2;
        conv5p <= !cyc23_q && !(tl_p2 ? other_h[10] : other_h[11]) && yuv_vec[(tl_p2 && other_h[9]) ? tl_rq - 3'd1 : tl_rq];
        tcp_sr <= tcp_s; tcp_tr <= tcp_t;
        // P5m
        v5 <= v5p & ~rst; ph5 <= ph5p; conv5 <= conv5p;
        u5_uls <= u5p_uls; u5_ult <= u5p_ult; u5_lrs <= u5p_lrs; u5_lrt <= u5p_lrt;
        u5_cs <= u5p_cs; u5_ms <= u5p_ms; u5_ct <= u5p_ct; u5_mt <= u5p_mt;
        u5_masks <= u5p_masks; u5_maskt <= u5p_maskt;
        c5_tmem <= c5p_tmem; c5_line <= c5p_line; c5_size <= c5p_size; c5_pal <= c5p_pal; c5_fmt <= c5p_fmt; c5_yuv <= c5p_yuv;
        vs5r <= vs5; vt5r <= vt5;
        mx5s <= mx_f(u5p_uls, u5p_lrs); mx5t <= mx_f(u5p_ult, u5p_lrt);
        ng5s <= ce5s && vs5[23]; gx5s <= ce5s && !vs5[23] && ($signed(vs5[23:5]) >= $signed({{8{mxn_s[10]}}, mxn_s}));
        ng5t <= ce5t && vt5[23]; gx5t <= ce5t && !vt5[23] && ($signed(vt5[23:5]) >= $signed({{8{mxn_t[10]}}, mxn_t}));
    end

    // one axis: v -> c0, c1 (wrapped) and the 5-bit fraction (rdp_tcaxis, below)
    wire [10:0] s0c, s1c, t0c, t1c;
    wire [4:0]  fsc, ftc;
    rdp_tcaxis tcs (.v(vs5r), .mx(mx5s), .ng_i(ng5s), .gx_i(gx5s), .clampbit(u5_cs), .mirror(u5_ms), .mask(u5_masks), .cpy(1'b0), .cm(3'd0), .c0(s0c), .c1(s1c), .fr(fsc));
    rdp_tcaxis tct (.v(vt5r), .mx(mx5t), .ng_i(ng5t), .gx_i(gx5t), .clampbit(u5_ct), .mirror(u5_mt), .mask(u5_maskt), .cpy(1'b0), .cm(3'd0), .c0(t0c), .c1(t1c), .fr(ftc));
    reg        v6;
    reg [10:0] s06, s16, t06, t16;
    reg [4:0]  fs6, ft6;
    always @(posedge clk) begin
        v6 <= v5 & ~rst; ph6 <= ph5;
        // the TMEM addresses of P7
        u6_tmem <= c5_tmem; u6_line <= c5_line; u6_size <= c5_size; u6_yuv <= c5_yuv;
        conv6 <= conv5; c6_pal <= c5_pal; c6_fmt <= c5_fmt;
        s06 <= s0c; s16 <= s1c; t06 <= t0c; t16 <= t1c; fs6 <= fsc; ft6 <= ftc;
    end

    // ======================================================================
    // P7: TMEM addresses of the three texels the filter takes -- (s1, t0),
    // (s0, t1), and (s0, t0) or, in the 2 x 2's lower triangle, (s1, t1);
    // unfiltered, (s0, t0). A row is `line` 64-bit words:
    // 16-bit word tmem*4 + t*line*4 + s (16- and 32-bit texels), s/2, s/4
    // (and s/2 in each half for a YUV pair). A converted pass takes (s0, t0).
    // On an odd row the halves of each 64-bit word change places (word ^ 2),
    // as the N64's: they did when the row was loaded (rdp_mem).
    // ======================================================================
    wire        upper6 = ({1'b0, fs6} + {1'b0, ft6}) < 6'd32;
    wire        selA6 = upper6 || point || conv6;
    wire [19:0] row0 = t06 * u6_line;
    wire [19:0] row1 = t16 * u6_line;
    function automatic [10:0] taddr;
        input [19:0] row;
        input [10:0] sc;
        input        odd;
        reg [10:0] a;
        begin
            a = {u6_tmem, 2'b00} + {row[8:0], 2'b00}
              + ((u6_size[1] && !u6_yuv) ? sc : (u6_size[0] || u6_yuv) ? {1'b0, sc[10:1]} : {2'b00, sc[10:2]});
            a[1] = a[1] ^ odd;
            taddr = (u6_size == 2'd3 || u6_yuv) ? {1'b0, a[9:0]} : a;
        end
    endfunction
    reg        v7;
    reg [1:0]  sl0_7, sl1_7, sl2_7;         // each texel's place in its 16-bit word
    reg [4:0]  fs7, ft7;
    reg        up7;
    always @(posedge clk) begin
        v7 <= v6 & ~rst; ph7 <= ph6; conv7 <= conv6;
        c7_pal <= c6_pal; c7_fmt <= c6_fmt; c7_size <= u6_size;
        tm_a0 <= selA6 ? taddr(row0, s06, t06[0]) : taddr(row1, s16, t16[0]);
        tm_a1 <= taddr(row0, s16, t06[0]);
        tm_a2 <= taddr(row1, s06, t16[0]);
        sl0_7 <= selA6 ? s06[1:0] : s16[1:0]; sl1_7 <= s16[1:0]; sl2_7 <= s06[1:0];
        fs7 <= fs6; ft7 <= ft6; up7 <= selA6;
    end

    // ======================================================================
    // P8: TMEM's words arrive; with the TLUT on a texel is its address (a 4-bit texel with the tile's palette, an 8-bit one, the
    // first byte of a 16- or 32-bit one's word)
    // ======================================================================
    reg        v8;
    reg [1:0]  sl0_8, sl1_8, sl2_8;
    reg [4:0]  fs8, ft8;
    reg        up8;
    always @(posedge clk) begin
        v8 <= v7 & ~rst; ph8 <= ph7; conv8 <= conv7;
        // the TLUT index
        u8_size0 <= c7_size[0]; u8_size1 <= c7_size[1]; u8_pal <= c7_pal; c8_fmt <= c7_fmt;
        sl0_8 <= sl0_7; sl1_8 <= sl1_7; sl2_8 <= sl2_7;
        fs8 <= fs7; ft8 <= ft7; up8 <= up7;
    end
    function automatic [7:0] tl_idx;
        input [15:0] v;
        input [1:0]  sl;
        reg [3:0] n;
        begin
            n = sl == 2'd0 ? v[15:12] : sl == 2'd1 ? v[11:8] : sl == 2'd2 ? v[7:4] : v[3:0];
            tl_idx = u8_size1 ? v[15:8] : u8_size0 ? (sl[0] ? v[7:0] : v[15:8]) : {u8_pal, n};
        end
    endfunction
    assign tl_a0 = tl_idx(tm_q0, sl0_8);
    assign tl_a1 = tl_idx(tm_q1, sl1_8);
    assign tl_a2 = tl_idx(tm_q2, sl2_8);

    // P8t: the words held while the TLUT is read
    reg        v8t;
    reg [15:0] q0t, q1t, q2t, h0t, h1t, h2t;
    reg [1:0]  sl0_t, sl1_t, sl2_t;
    reg [4:0]  fs8t, ft8t;
    reg        up8t;
    always @(posedge clk) begin
        v8t <= v8 & ~rst; ph8t <= ph8; conv8t <= conv8;
        // the texel's decode
        u8t_fmt <= c8_fmt; u8t_size <= {u8_size1, u8_size0};
        q0t <= tm_q0; q1t <= tm_q1; q2t <= tm_q2; h0t <= tm_h0; h1t <= tm_h1; h2t <= tm_h2;
        sl0_t <= sl0_8; sl1_t <= sl1_8; sl2_t <= sl2_8;
        fs8t <= fs8; ft8t <= ft8; up8t <= up8;
    end
    // (copy mode copies a 16-bit texel's or a TLUT entry's bits as they are: read as RGBA 5551 they are written back
    // unchanged, an IA or I texel and an IA entry too -- the corner, the one texel copy mode takes)
    wire        cp_raw = cyc2_q;
    wire [31:0] tx0 = tlut_on ? tex16(tlut_ia && !cp_raw, tl_q0)
                              : texel((cp_raw && u8t_size == 2'd2 && u8t_fmt != 3'd1) ? 3'd0 : u8t_fmt, u8t_size, q0t, h0t, sl0_t);
    wire [31:0] tx1 = tlut_on ? tex16(tlut_ia, tl_q1) : texel(u8t_fmt, u8t_size, q1t, h1t, sl1_t);
    wire [31:0] tx2 = tlut_on ? tex16(tlut_ia, tl_q2) : texel(u8t_fmt, u8t_size, q2t, h2t, sl2_t);

    // The filter's edges, formed here: the decode leaves this stage slack, the filter's multipliers (P9) none. The edges are
    // always B - A and C - A (texels 1 and 2 less the corner); an upper triangle weights them by the fractions, a lower one
    // crosswise by the complements (below), so that no choice is made among the 32-bit texels
    function automatic [9:0] edge1;
        input [7:0] p, x;
        edge1 = $signed({2'b0, p}) - $signed({2'b0, x});
    endfunction
    function automatic [39:0] edge4;
        input [31:0] p, x;
        edge4 = { edge1(p[31:24], x[31:24]), edge1(p[23:16], x[23:16]), edge1(p[15:8], x[15:8]), edge1(p[7:0], x[7:0]) };
    endfunction
    wire [39:0] e1w8t = edge4(tx1, tx0);
    wire [39:0] e2w8t = edge4(tx2, tx0);

    // P8b: the decoded texels, registered (the memories through the decode
    // into the filter's multipliers is too long a path); x, the set, the shade,
    // the depth come out of sb_ram here
    reg        v8b;
    reg [31:0] ta8b;                        // the corner (A or D)
    reg [39:0] e1v8b, e2v8b;                // the edges from the corner to texels 1 and 2, 10 bits a channel
    reg [5:0]  ws8b, wt8b;                  // the weights of those edges (an upper triangle: S fraction, T fraction; a lower one: 32 - T's, 32 - S's)
    always @(posedge clk) begin
        v8b <= v8t & ~rst; ph8b <= ph8t; conv8b <= conv8t;
        ta8b <= tx0; e1v8b <= e1w8t; e2v8b <= e2w8t;
        ws8b <= up8t ? {1'b0, fs8t} : 6'd32 - {1'b0, ft8t};
        wt8b <= up8t ? {1'b0, ft8t} : 6'd32 - {1'b0, fs8t};
    end

    // P8c: the filter's two products and their sum (the multipliers' stage: the DSP blocks take the edges and the weights
    // from P8b's registers and give the sum in a register of their own: a multiplier with logic after it did not fit a
    // clock of 10.667 ns), the corner, the pass's flags
    reg        v8c, ph8c, conv8c;
    reg [31:0] ta8c;
    reg [59:0] pa8c;                        // the YUV converter's products from the corner (a pass that converts its own texel)
    reg signed [16:0] acc8c [0:3];
    function automatic signed [16:0] tri3_acc;
        input [9:0] e1v, e2v;
        input [5:0] ws, wt;
        reg signed [9:0] e1, e2;
        begin
            e1 = e1v; e2 = e2v;
            tri3_acc = e1 * $signed({1'b0, ws}) + e2 * $signed({1'b0, wt}) + 17'sd16;
        end
    endfunction
    always @(posedge clk) begin
        v8c <= v8b & ~rst; ph8c <= ph8b; conv8c <= conv8b; ta8c <= ta8b;
        pa8c <= yuv_prod(ta8b);
        acc8c[3] <= tri3_acc(e1v8b[39:30], e2v8b[39:30], ws8b, wt8b);
        acc8c[2] <= tri3_acc(e1v8b[29:20], e2v8b[29:20], ws8b, wt8b);
        acc8c[1] <= tri3_acc(e1v8b[19:10], e2v8b[19:10], ws8b, wt8b);
        acc8c[0] <= tri3_acc(e1v8b[9:0],   e2v8b[9:0],   ws8b, wt8b);
    end

    // ======================================================================
    // P9: N64 3-point filter: from the corner x, by the two edges to B and C
    // ======================================================================
    function automatic [7:0] tri3;
        input [7:0] x;
        input signed [16:0] acc;            // (P8c's sum: e1 ws + e2 wt + 16)
        reg signed [19:0] r;
        begin
            // (no clamp: where the filter's result is taken, ws + wt <= 32 -- the weights are those of the triangle the fractions are in -- so it is
            // a rounded mean of three bytes, x (32 - ws - wt) + A ws + B wt over 32, within 0..255; where it is not taken -- the point and the
            // converted passes -- it is not used)
            r = $signed({12'd0, x}) + (acc >>> 5);
            tri3 = r[7:0];
        end
    endfunction
    wire [31:0] filt9 = { tri3(ta8c[31:24], acc8c[3]),
                          tri3(ta8c[23:16], acc8c[2]),
                          tri3(ta8c[15:8],  acc8c[1]),
                          tri3(ta8c[7:0],   acc8c[0]) };
    // the pixel's group in the raw hidden buffer: its word's address (the
    // row's word base + x / 2) by 8; its place there, k
    wire [9:0]  hw8c = (bs8c ? rwb1 : rwb0) + {1'b0, x8c[9:1]};
    // YUV -> RGB (rdp.c yuv_convert): from a texel (U, V, Y, Y), U and V less 128, the K's as 2K + 1 in 1/256ths --
    // R = Y + K0 V, G = Y + K1 U + K2 V, B = Y + K3 U, each clamped to a byte; A = Y. Beside the filter, from the corner
    // (a converted pass's is (s0, t0)), or with CONVERT_ONE the first pass's texel in the second (tex9 the clock after it).
    function automatic [7:0] cv_ch;
        input [7:0]         y;
        input signed [19:0] p;              // the products' sum, + 128
        reg   signed [19:0] q;
        begin
            q = p >>> 8;
            cv_ch = clamp8($signed({12'd0, y}) + q);
        end
    endfunction
    function automatic [59:0] yuv_prod;             // the three channels' products + 128
        input [31:0] t;
        reg signed [8:0]  u, v;
        reg signed [9:0]  k0, k1, k2, k3;
        reg signed [19:0] pr, pg, pb;
        begin
            u = $signed({1'b0, t[31:24]}) - 9'sd128;
            v = $signed({1'b0, t[23:16]}) - 9'sd128;
            k0 = {cv_k03[35:27], 1'b1}; k1 = {cv_k03[26:18], 1'b1}; k2 = {cv_k03[17:9], 1'b1}; k3 = {cv_k03[8:0], 1'b1};
            // (the texel's Y, 8 bits up, is in the sums: (p + Y * 256) >> 8 is (p >> 8) + Y, and the stage after has the clamp only)
            pr = k0 * v + 20'sd128 + $signed({4'd0, t[15:8], 8'd0});
            pg = k1 * u + k2 * v + 20'sd128 + $signed({4'd0, t[15:8], 8'd0});
            pb = k3 * u + 20'sd128 + $signed({4'd0, t[15:8], 8'd0});
            yuv_prod = {pr, pg, pb};
        end
    endfunction
    function automatic [31:0] yuv_fin;               // the texel's Y and the products: R G B and A = Y
        input [7:0]  y;
        input [59:0] pp;
        begin
            yuv_fin = { cv_ch(8'd0, pp[59:40]), cv_ch(8'd0, pp[39:20]), cv_ch(8'd0, pp[19:0]), y };
        end
    endfunction
    wire        dep8c = conv8c && ph8c && other_h[9];       // a second pass that converts the first pass's texel (CONVERT_ONE)
    wire [31:0] conv9 = yuv_fin(ta8c[15:8], pa8c);
    // the texel the pass gives: filtered, or the corner's, or converted; a two_pass pixel's first pass is kept (tex19) for
    // its second, which gives TEXEL1 (tex19; TEXEL0 is tex9)
    wire [31:0] tex_cur = conv8c ? conv9 : point ? ta8c : filt9;      // (a dependent conversion is made in P9x: its texel here is not used)
    reg  [31:0] tex19;
    reg         dep9;
    reg  [3:0]  hk9x, hk10a;
    reg        v9;
    reg [9:0]  x9;
    (* altera_attribute = "-name AUTO_SHIFT_REGISTER_RECOGNITION OFF" *) reg [31:0] tex9;
    reg [35:0] shade9;
    reg [17:0] depth9;
    always @(posedge clk) begin
        v9 <= v8c & ~rst & (!two_pass | ph8c); x9 <= x8c; bs9 <= bs8c;
        tex9 <= (two_pass & ph8c) ? tex19 : tex_cur;       // (the second pass takes the first's texel, tex19 the clock after it: the passes are in consecutive clocks)
        tex19 <= tex_cur; dep9 <= dep8c;
        shade9 <= shade8c; depth9 <= depth8c;
    end
    // The dependent conversion (a second pass that converts the first pass's texel): the products are made from tex9's
    // register (the first pass's texel, taken as the second pass's TEXEL0) in the pass's own P9 clock and the sum and the clamp in the clock after, P9x. The other conversions have their
    // source in P8b and are made in P8c (pa8c) and P9. P9x also holds the pixel's other registers a clock: the combiner's
    // selection takes them from here, and the line buffers' addresses are issued from P9's, a clock later than they were.
    reg [59:0] pb9;
    reg [7:0]  yb9;
    reg        v9x;
    reg [9:0]  x9x;
    reg [31:0] tex9x, tex19x;
    reg [35:0] shade9x;
    reg [17:0] depth9x;
    reg [8:0]  lf9x, nz9x;
    reg [3:0]  cv9x;
    reg [1:0]  y9x;
    wire [9:0] hw9 = (bs9 ? rwb1 : rwb0) + {1'b0, x9[9:1]};
    always @(posedge clk) begin
        pb9 <= yuv_prod(tex9); yb9 <= tex9[15:8];     // (tex9 the clock after the second pass has the first pass's texel)
        v9x <= v9 & ~rst; x9x <= x9; bs9x <= bs9;
        lb_raddr <= {bs9, x9[9:1]};             // line buffers: data after the next edge, captured at P10b
        clb_raddr <= {bs9, c32 ? x9[8:0] : x9[9:1]};
        slb_raddr <= {bs9, hw9[9:1]};           // (the colour word's address over two: its pair's place)
        hl_raddr <= {bs9, hw9[9:3]};            // the hidden word of the pixel's group
        hk9x <= {hw9[2:0], x9[0]};
        tex9x <= tex9; tex19x <= dep9 ? yuv_fin(yb9, pb9) : tex19;
        shade9x <= shade9; depth9x <= depth9; lf9x <= lf9; nz9x <= nz9;
    end
    // the LOD fraction, from L3 (lf) to P9 and on to P10 for the second cycle: a RAM read 8 clocks after its write (the
    // first pass at P5 to P9, with P1b, P5m and P8c between; lf holds for both passes)
    reg  [8:0]  lf_q, lf9, lf10a, lf10;
    (* ramstyle = "M10K, no_rw_check" *) reg [8:0] lf_ram [0:15];
    always @(posedge clk) begin
        lf_ram[rw_a] <= lf;
        lf_q <= lf_ram[rw_a - 4'd7];
        lf9 <= lf_q; lf10a <= lf9x; lf10 <= lf10a;
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
        rgb_b = (sel == 4'd7) ? cc_xab(k4) : (sel >= 4'd6) ? 10'd0 : rgb_a(sel, i, tex, tex1, sh, cm);
    endfunction
    function automatic [9:0] rgb_c_com;   // C's choices that are the same for the three channels (the alphas, the fraction, K5), made once
        input [4:0]  sel;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        input [9:0]  fr;
        case (sel)
            5'd7:  rgb_c_com = cc_xc(ch9(cm, 2'd3));
            5'd8:  rgb_c_com = {2'b00, tex[7:0]};
            5'd9:  rgb_c_com = {2'b00, tex1[7:0]};
            5'd10: rgb_c_com = {2'b00, prim[7:0]};
            5'd11: rgb_c_com = {1'b0, ch9(sh, 2'd3)};
            5'd12: rgb_c_com = {2'b00, env[7:0]};
            5'd13, 5'd14: rgb_c_com = fr;
            5'd15: rgb_c_com = cc_xc(k5);
            default: rgb_c_com = 10'd0;
        endcase
    endfunction
    function automatic [9:0] rgb_c;        // (fr: the LOD fraction or the primitive's, 13 or 14, chosen once for the three; com: rgb_c_com)
        input [4:0]  sel;
        input [1:0]  i;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [35:0] cm;
        input [9:0]  com;
        case (sel)
            5'd0:  rgb_c = cc_xc(ch9(cm, i));
            5'd1, 5'd2, 5'd3, 5'd4, 5'd5: rgb_c = rgb_a(sel[3:0], i, tex, tex1, sh, cm);
            default: rgb_c = com;
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
    function automatic [9:0] al_c;         // (fr: the LOD fraction or the primitive's, 0 or 6)
        input [2:0]  sel;
        input [31:0] tex, tex1;
        input [35:0] sh;
        input [9:0]  fr;
        case (sel)
            3'd1: al_c = {2'b00, tex[7:0]};
            3'd2: al_c = {2'b00, tex1[7:0]};
            3'd3: al_c = {2'b00, prim[7:0]};
            3'd4: al_c = {1'b0, ch9(sh, 2'd3)};
            3'd5: al_c = {2'b00, env[7:0]};
            3'd0, 3'd6: al_c = fr;
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
    // (the 1-cycle mode's LOD fraction without the LOD: all ones where there is a level count of 0 and neither sharpen nor detail, a register --
    // the primitive's modes are steady while its pixels are in the pipe -- not four compares in front of the combiner's C choices:
    // other_h -> o11a, -0.23 ns at 10.667 ns)
    reg         lf_one_q;
    always @(posedge clk) lf_one_q <= other_h[21:20] == 2'd0 && lod_lvl == 3'd0 && !other_h[17] && !other_h[18];
    function automatic [155:0] cc_sel;
        input        cy;              // 0: cycle 0 settings, 1: cycle 1
        input [31:0] tex, tex1;           // TEXEL0, TEXEL1
        input [35:0] sh;                  // SHADE: 9 bits a channel
        input [35:0] cm;
        input [8:0]  lf;                  // the LOD fraction (a signed 9-bit as C, as K5; 0 without the LOD)
        input [8:0]  nz;                  // NOISE (the colour's A 7), read as COMBINED is there
        reg [3:0] sa_r, sb_r;
        reg [4:0] mul_r;
        reg [2:0] sa_a, mul_a, sb_a, add_a, add_r;
        reg [9:0] lfx, fr_c, fr_a, c_com;
        begin
            if (!cy) begin
                sa_r = cc_hi[23:20]; mul_r = cc_hi[19:15]; sa_a = cc_hi[14:12]; mul_a = cc_hi[11:9];
                sb_r = cc_lo[31:28]; add_r = cc_lo[17:15]; sb_a = cc_lo[14:12]; add_a = cc_lo[11:9];
            end else begin
                sa_r = cc_hi[8:5];   mul_r = cc_hi[4:0];   sa_a = cc_lo[23:21]; mul_a = cc_lo[20:18];
                sb_r = cc_lo[27:24]; add_r = cc_lo[8:6];   sb_a = cc_lo[5:3];   add_a = cc_lo[2:0];
            end
            // (C's two fractions, the colour's 13 and 14, the alpha's 0 and 6, are one input of each C select: which of the
            // two is chosen here, once for the three channels)
            // (1-cycle mode: a level count of 0, without sharpen and detail, gives all ones whatever the steps)
            lfx = lod_on ? {lf[8], lf} : lf_one_q ? 10'h0FF : 10'd0;
            fr_c = mul_r[0] ? lfx : {2'b00, prim_lod_frac};
            fr_a = mul_a[1] ? {2'b00, prim_lod_frac} : lfx;
            c_com = rgb_c_com(mul_r, tex, tex1, sh, cm, fr_c);
            cc_sel = {
                (sa_r == 4'd7) ? cc_xab(nz) : rgb_a(sa_r, 2'd0, tex, tex1, sh, cm), (sa_r == 4'd7) ? cc_xab(nz) : rgb_a(sa_r, 2'd1, tex, tex1, sh, cm),
                (sa_r == 4'd7) ? cc_xab(nz) : rgb_a(sa_r, 2'd2, tex, tex1, sh, cm), al_abd(sa_a, tex, tex1, sh, cm),
                rgb_b(sb_r, 2'd0, tex, tex1, sh, cm), rgb_b(sb_r, 2'd1, tex, tex1, sh, cm), rgb_b(sb_r, 2'd2, tex, tex1, sh, cm), al_abd(sb_a, tex, tex1, sh, cm),
                rgb_c(mul_r, 2'd0, tex, tex1, sh, cm, c_com), rgb_c(mul_r, 2'd1, tex, tex1, sh, cm, c_com), rgb_c(mul_r, 2'd2, tex, tex1, sh, cm, c_com), al_c(mul_a, tex, tex1, sh, fr_a),
                rgb_d(add_r, 2'd0, tex, tex1, sh, cm), rgb_d(add_r, 2'd1, tex, tex1, sh, cm), rgb_d(add_r, 2'd2, tex, tex1, sh, cm), al_d(add_a, tex, tex1, sh, cm) };
        end
    endfunction
    // the four channels' raw 9 bits (cycle 0: operands 0..511, and its colours' K4 and K5, and its alpha's LOD fraction,
    // a signed C; cycle 1: COMBINED too)
    function automatic [35:0] cc_arith0;
        input [155:0] o;
        cc_arith0 = { cc_eval1(o[155:146], o[115:106], o[75:66], o[35:27]),
                      cc_eval1(o[145:136], o[105:96],  o[65:56], o[26:18]),
                      cc_eval1(o[135:126], o[95:86],   o[55:46], o[17:9]),
                      cc_eval1(o[125:116], o[85:76],   o[45:36], o[8:0]) };
    endfunction
    // The second arithmetic in two steps: the differences A - B (11 bits), with C and D, are what the stage before registers (cc_pre: the
    // subtractions after the operands' choice, which has the clock for them), and the product and the sum are the stage's (cc_fin: with
    // the subtraction in front of the multiplier it did not fit 10.667 ns: o11a -> c1m, -0.2 to -0.74 ns)
    function automatic [119:0] cc_pre;
        input [155:0] o;
        reg signed [10:0] d0, d1, d2, d3;
        begin
            d0 = $signed(o[155:146]) - $signed(o[115:106]);
            d1 = $signed(o[145:136]) - $signed(o[105:96]);
            d2 = $signed(o[135:126]) - $signed(o[95:86]);
            d3 = $signed(o[125:116]) - $signed(o[85:76]);
            cc_pre = { d0, o[75:66], o[35:27], d1, o[65:56], o[26:18], d2, o[55:46], o[17:9], d3, o[45:36], o[8:0] };
        end
    endfunction
    function automatic [8:0] cc_fin1;
        input signed [10:0] diff;
        input signed [9:0]  c;
        input [8:0]         d;
        reg signed [20:0] prod;
        reg [16:0] sum;
        begin
            prod = diff * c;
            sum = prod[16:0] + {d, 8'h80};
            cc_fin1 = sum[16:8];
        end
    endfunction
    function automatic [35:0] cc_fin;
        input [119:0] q;
        cc_fin = { cc_fin1(q[119:109], q[108:99], q[98:90]), cc_fin1(q[89:79], q[78:69], q[68:60]),
                   cc_fin1(q[59:49],   q[48:39],  q[38:30]), cc_fin1(q[29:19], q[18:9],  q[8:0]) };
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
    // NOISE, the colour combiner's A 7: ((three random bits) << 6) | 0x20 for each pixel, the bits the alpha dither's
    // noise takes (as the console's, Angrylion's get_dither_noise: the one random number serves both). From a second
    // copy of the LFSR, stepped here at P9 for the same pixels as the first at P11, so that a pixel's value is the one
    // the model takes for it (the state before its own step); the second cycle's a stage on.
    reg  [22:0] nlfsr;
    reg  [8:0]  nz10;
    wire [8:0]  nz9 = {nlfsr[10:8], 6'h20};
    always @(posedge clk) begin
        if (rst) nlfsr <= 23'd0;
        else if (v9 && cv9 != 4'd0 && !cyc3_q) nlfsr <= {nlfsr[21:0], ~(nlfsr[22] ^ nlfsr[18])};
        nz10 <= nz9x;
    end
    // The one combiner (the console's: a 2-cycle pixel goes through it twice, RDP_SHARE of the faithful build). A 2-cycle pixel's first
    // cycle takes its operands from P9x (early: u_e), its second cycle's -- or a 1-cycle pixel's only one -- are those of the same pixel
    // three clocks on (late: P10b), when the first cycle's result, cm_r, has come round. 2-cycle pixels are issued every second clock on one
    // parity (rdp_top), so a first pass never meets another pixel's second. The late operands have TEXEL0 and TEXEL1 changed places (the
    // N64's second cycle takes the first cycle's TEXEL1 as TEXEL0).
    reg         v10a;
    reg [9:0]   x10a;
    reg [31:0]  tex10a, tex1_10a;     // (tex1_*: the next tile's sample, the second cycle's TEXEL0)
    reg [35:0]  shade10a;
    reg [17:0]  depth10a;
    always @(posedge clk) begin
        v10a <= v9x & ~rst; x10a <= x9x; bs10a <= bs9x; hk10a <= hk9x;
        tex10a <= tex9x; tex1_10a <= tex19x; shade10a <= shade9x; depth10a <= depth9x;
    end
    reg        v10;
    reg [9:0]  x10;
    reg [31:0] tex10, tex1_10, mem10;
    reg [15:0] mv10;                  // a 16-bit image's memory word and its shadow byte, kept as they come out of the RAMs: their check and the low bits they give are made in the next clock (P10b)
    reg [7:0]  ms10;
    reg [35:0] shade10;
    reg [17:0] depth10;
    reg [15:0] zold10;               // the depth image's word, as it is
    reg        memb10;                // the colour image's bit 0 (the coverage's top bit)
    reg [1:0]  hid10;                 // its hidden two
    always @(posedge clk) begin
        v10 <= v10a & ~rst; x10 <= x10a; bs10 <= bs10a;
        tex10 <= tex10a; tex1_10 <= tex1_10a; shade10 <= shade10a; depth10 <= depth10a;
        zold10 <= x10a[0] ? zlb_rdata[15:0] : zlb_rdata[31:16];     // the even pixel first
        // (a 32-bit image: the word's R G B at eight bits, and the coverage from the alpha byte's top three bits, where
        // a 16-bit one has its bit 0 and the hidden two; lang/c/rdp/rdp.c draw_pixel_cvg)
        mem10 <= {clb_rdata[31:8], 8'hFF};                          // (a 32-bit image's; a 16-bit one's is made in P10b)
        mv10 <= c8 ? 16'd0 : x10a[0] ? clb_rdata[15:0] : clb_rdata[31:16];
        ms10 <= c8 ? 8'd0 : hk10a[1] ? (x10a[0] ? slb_rdata[7:0] : slb_rdata[15:8]) : (x10a[0] ? slb_rdata[23:16] : slb_rdata[31:24]);
        memb10 <= c32 ? clb_rdata[7] : x10a[0] ? clb_rdata[0] : clb_rdata[16];
        hid10 <= c32 ? clb_rdata[6:5] : hl_rdata[{hk10a, 1'b0} +: 2];
    end
    reg         v10b;
    reg [9:0]   x10b;
    reg [31:0]  tex10b, tex1_10b, mem10b;
    reg [35:0]  shade10b;
    reg [17:0]  depth10b;
    reg [15:0]  zold10b;
    reg [8:0]   lf10b, nz10b;
    reg         memb10b;
    reg [1:0]   hid10b;
    always @(posedge clk) begin
        v10b <= v10 & ~rst; x10b <= x10; bs10b <= bs10;
        tex10b <= tex10; tex1_10b <= tex1_10; shade10b <= shade10; depth10b <= depth10;
        mem10b <= c32 ? mem10 : un565sh(mv10, ms10); zold10b <= zold10; memb10b <= memb10; hid10b <= hid10;
        lf10b <= lf10; nz10b <= nz10;
    end
    // (cyc is the primitive's, steady while its pixels are in the pipe: its test is a register, and the choice between the first cycle's
    // operands and the second's -- 300 bits of multiplexers in front of the subtractions -- is a register too: u_e is v9x && 2-cycle,
    // made a stage ahead. other_h -> o11a was -0.24 to -0.41 ns at 10.667 ns)
    reg          u_e;
    always @(posedge clk) u_e <= v9 & ~rst & cyc1_q;
    reg  [35:0]  cm_r;                                  // the first cycle's result, 9 raw bits a channel (cc_eval1), held
    wire [35:0]  cm10 = cyc1_q ? cm_r : 36'd0;   // COMBINED for the second cycle (0 in the other modes)
    wire [155:0] o1_w = cc_sel(!u_e, u_e ? tex9x : tex1_10b, u_e ? tex19x : tex10b, u_e ? shade9x : shade10b,
                               u_e ? 36'd0 : cm10, u_e ? lf9x : lf10b, u_e ? nz9x : nz10b);
    reg         v11a, ue11a;
    reg [9:0]   x11a;
    reg [119:0] o11a;
    reg [31:0]  tex11a, mem11a;
    reg [35:0]  shade11a;
    reg [17:0]  depth11a, zold11a;    // (zold11a: the word's 18 bits, z_dec)
    reg [6:0]   zfl11a;               // what a word of the exponents 0, 1, 2 has lost: the least tolerance
    reg         memb11a, memb11;
    reg [1:0]   hid11a, hid11;
    reg [7:0]   c0a11a, c0a11;       // 2-cycle: the first cycle's alpha (the alpha compare's)
    reg         c0f11a, c0f11;       // ... and whether it counts 256 (an alpha of 255 does: the coverage's multiply)
    always @(posedge clk) begin
        v11a <= v10b & ~rst; x11a <= x10b; bs11a <= bs10b; ue11a <= u_e;
        o11a <= cc_pre(o1_w);
        tex11a <= tex10b; shade11a <= shade10b; mem11a <= mem10b;
        depth11a <= depth10b; zold11a <= z_dec(zold10b);
        zfl11a <= zold10b[15:13] == 3'd0 ? 7'h7F : zold10b[15:13] == 3'd1 ? 7'h3F : zold10b[15:13] == 3'd2 ? 7'h1F : 7'h00;
        memb11a <= memb10b; c0a11a <= cc_clamp(cm10[8:0]); c0f11a <= cc_a256(cm10[8:0]); hid11a <= hid10b;
    end
    // P11m: the second cycle's arithmetic (the combiner's multipliers and adders) in a register of its own; the clamp, the
    // choice and the rest of the stage's work come after it (the two together did not fit a clock of 10.667 ns)
    wire [35:0] c1_w = cc_fin(o11a);
    always @(posedge clk) if (ue11a) cm_r <= c1_w;
    reg         v11m, bs11m;
    reg [9:0]   x11m;
    reg [35:0]  c1m;
    reg [31:0]  tex11m, mem11m;
    reg [35:0]  shade11m;
    reg [17:0]  depth11m, zold11m, dlo11m, dhi11m;
    reg         memb11m;
    reg [1:0]   hid11m;
    reg [7:0]   c0a11m;
    reg         c0f11m;
    // the tolerance: the primitive's step (16-bit units: its two low bits set) and no less than what the memory's word has lost
    wire [17:0] ztol11a = {z_tol, 2'b11} | {11'd0, zfl11a};
    wire [18:0] dsum11a = {1'b0, depth11a} + {1'b0, ztol11a};
    wire [18:0] ddif11a = {1'b0, depth11a} - {1'b0, ztol11a};     // (the borrow: the depth is below the tolerance)
    always @(posedge clk) begin
        v11m <= v11a & ~rst; x11m <= x11a; bs11m <= bs11a;
        c1m <= c1_w;
        tex11m <= tex11a; shade11m <= shade11a; depth11m <= depth11a;
        dlo11m <= ddif11a[18] ? 18'd0 : ddif11a[17:0];
        dhi11m <= (dsum11a[18]) ? 18'h3FFFF : dsum11a[17:0];
        zold11m <= zold11a; mem11m <= mem11a;
        memb11m <= memb11a; c0a11m <= c0a11a; c0f11m <= c0f11a; hid11m <= hid11a;
    end
    reg        v11;
    reg [9:0]  x11;
    reg [31:0] comb11;
    reg [35:0] shade11;
    reg        af11;                // the combined alpha counts 256 for the coverage multiply (CVG_X_ALPHA)
    reg [17:0] depth11, zold11;
    reg [17:0] dlo11, dhi11;       // the depth less and plus the decal tolerance
    reg [31:0] mem11;
    always @(posedge clk) begin
        v11 <= v11m & ~rst; x11 <= x11m; bs11 <= bs11m;
        comb11 <= cyc2_q ? tex11m : { cc_clamp(c1m[35:27]), cc_clamp(c1m[26:18]), cc_clamp(c1m[17:9]), cc_clamp(c1m[8:0]) };
        af11 <= !cyc2_q && cc_a256(c1m[8:0]);
        shade11 <= shade11m; depth11 <= depth11m;
        dlo11 <= dlo11m; dhi11 <= dhi11m;
        zold11 <= zold11m; mem11 <= mem11m;
        memb11 <= memb11m; c0a11 <= c0a11m; c0f11 <= c0f11m; hid11 <= hid11m;
    end
    // The coverage and the row's low bits (a row's last pixels are still in
    // the pipe when the next row's y is issued), out of sb_ram at P8b, on to
    // P11 (the coverage, the alpha dither) and P13b (the RGB dither)
    reg [3:0] cv9, cv10a, cv10, cv10b, cv11a, cv11m, cv11;
    reg [1:0] y9, y10a, y10, y10b, y11a, y11m, y11, y11b, y12a, y12, y12b, y13a, y13b;
    always @(posedge clk) begin
        cv9 <= cvg8c; cv9x <= cv9; cv10a <= cv9x; cv10 <= cv10a; cv10b <= cv10; cv11a <= cv10b; cv11m <= cv11a; cv11 <= cv11m;
        y9 <= y8c; y9x <= y9; y10a <= y9x; y10 <= y10a; y10b <= y10; y11a <= y10b; y11m <= y11a; y11 <= y11m;
        y11b <= y11; y12a <= y11b; y12 <= y12a; y12b <= y12; y13a <= y12b; y13b <= y13a;
    end
    wire [3:0] cvg11 = cv11;
    wire [1:0] dy11 = y11;
    wire [1:0] dy13 = y13b;

    // ======================================================================
    // the N64's alpha, coverage and depth (lang/c/rdp/rdp.c draw_pixel_cvg)
    // ======================================================================
    // the alpha dither: the matrix the RGB dither's RGB_DITHER_SEL names for
    // the alpha (the Bayer matrix for 1 and 3, the magic square for 0 and 2:
    // Angrylion's get_dither_noise, the console's dither fetch) as
    // ALPHA_DITHER_SEL takes it -- the pattern, its inverse, noise (three
    // random bits), none. Copy mode: none.
    // The noise: a 23-bit LFSR (the console's taps), one step for each pixel
    // with coverage that passes this stage (lang/c/rdp/rdp.c steps it at the
    // same pixels): the alpha dither's noise and the alpha compare's random
    // threshold (DITHER_ALPHA_EN: the N64 compares with a random 8-bit
    // number, not with a pattern -- a pattern drew a regular grid of a
    // translucent surface's texels of alpha 0)
    reg  [22:0] lfsr;
    always @(posedge clk)
        if (rst) lfsr <= 23'd0;
        else if (v11 && cvg11 != 4'd0 && !cyc3_q) lfsr <= {lfsr[21:0], ~(lfsr[22] ^ lfsr[18])};
    wire [2:0] ad_d;
    rdp_dither_tab adtab (.off(1'b0), .bayer(other_h[6]), .y(dy11), .x(x11[1:0]), .dt(ad_d));
    wire [2:0] adith = cyc2_q ? 3'd0 : (other_h[5:4] == 2'd0) ? ad_d : (other_h[5:4] == 2'd1) ? ~ad_d
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
    // (the two products, alpha x coverage + 4, are registers made in the stage before, from the alpha as it is made -- the combiner's
    // clamp or the copy mode's texel -- and the coverage that comes next (cv11m): the multiply in front of the alpha's choices and of
    // the registers of P11b was -0.2 to -0.24 ns at 10.667 ns in 0 C: comb11 -> pa11b, cvg_sb and ca11b)
    reg  [11:0] cvgmul, cvgmul0;
    always @(posedge clk) begin
        cvgmul  <= (cyc2_q ? tex11m[7:0] : cc_clamp(c1m[8:0])) * cv11m + 12'd4;
        cvgmul0 <= c0a11m * cv11m + 12'd4;
    end
    wire        cx_m    = cvg_x_a & ~af11;
    wire        cx_m0   = cvg_x_a & ~c0f11;
    wire [3:0]  cvg_s   = cx_m ? cvgmul[11:8] : cvg11;
    wire [7:0]  pa11 = !a_cvg_s ? sat9({1'b0, comb11[7:0]} + {6'd0, adith})
                     : cx_m ? sat9(cvgmul[11:3]) : sat9({cvg11, 5'd0});
    wire [7:0]  ca11 = !cyc1_q ? pa11
                     : !a_cvg_s ? sat9({1'b0, c0a11} + {6'd0, adith})
                     : cx_m0 ? sat9(cvgmul0[11:3]) : sat9({cvg11, 5'd0});
    // the shade's alpha is 9 bits (0..511): plus the dither, 255 from 256 up
    wire [9:0]  sha_s = {1'b0, shade11[8:0]} + {7'd0, adith};
    wire [7:0]  sha11 = (sha_s[9] | sha_s[8]) ? 8'hFF : sha_s[7:0];
    wire [7:0]  athr  = cyc2_q ? 8'd128 : a_dith ? lfsr[7:0] : blend[7:0];   // (copy: the texel's alpha bit)
    // the memory's coverage: 6 where the colour image's pixel is complete, 7
    // where it is not read, else what this pixel leaves uncovered (rdp.h: storage)
    wire [2:0]  memcvg = !(im_rd && need_mem) ? 3'd7 : {memb11, hid11};
    wire        ovf    = ({1'b0, memcvg} + cvg_s) >= 4'd8;
    // depth: in front; within the primitive's step nearer / farther; the far plane
    wire        zmax   = zold11 == 18'h3FFFF;
    wire        zfront = depth11 < zold11;
    wire        znear  = zold11 >= dlo11;            // depth - dz <= old
    wire        zfar   = zold11 <= dhi11;            // depth + dz >= old
    // interpenetrating: the coverage scaled by how far in front it is, in
    // steps of 2 dz: ((old >> s) - (depth >> s)) & 15, s = the step's top bit + 1 (the step in 16-bit units: + 3 in the depth's 18)
    reg  [4:0]  zsft;                // (static: the primitive's)
    integer zb;
    always @(posedge clk) begin
        zsft <= 5'd3;
        for (zb = 1; zb < 16; zb = zb + 1) if (z_tol[zb]) zsft <= zb[4:0] + 5'd3;
    end
    wire [21:0] zdif  = ({4'd0, zold11} >> zsft) - ({4'd0, depth11} >> zsft);

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
    reg [13:0] depth11b;           // (the depth as its word's 14 bits from here on: z_enc)
    reg [3:0]  cvg_sb, zdifb;
    reg [7:0]  pa11b, ca11b, sha11b, athr11b;
    reg [2:0]  mcvg11b;
    reg        ovf11b, zmax11b, zfront11b, znear11b, zfar11b, k0_11b;
    always @(posedge clk) begin
        v11b <= v11 & ~rst; x11b <= x11; bs11b <= bs11;
        comb11b <= comb11; mem11b <= mem11; shade11b <= shade11; depth11b <= z_enc(depth11);
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
    wire        blend_en = force_bl || (!ovf11b && aa_en && (!z_cmp || zfar11b));
    // without a depth compare the memory coverage's weight shifts by the
    // primitive's dz (the N64's dzpixenc, ours + 3, up to 15): (static). The code's top two bits go into the depth's word
    reg  [2:0]  shb;
    reg  [1:0]  dzc;
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
        dzc <= e[3:2];
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
    reg        v12a;
    reg [9:0]  x12a;
    reg [31:0] comb12a, mem12a;
    reg [7:0]  pa12a, sha12a;
    reg [2:0]  mcvg12a;
    reg [13:0] depth12a;
    reg        ben12a, ovf12a;
    reg [2:0]  cv312, cv313, cv313b;
    // (the coverage's multiply by the depth's inter-penetration weight, its choice, its sum with the memory's, the written coverage and
    // the pixel's rejects are made from the registers of P12a and land in P12: the multiply, the choice, the sum and the rest were
    // one path from cvg_sb to the next register, -0.69 ns at 10.667 ns)
    reg [7:0]  zcm12a;
    reg [3:0]  cvg12a;
    reg        zinter12a, zpass12a, arej12a, k012a, blend12a;
    wire [3:0] cvg_i12 = (z_cmp && zinter12a) ? zcm12a[6:3] : cvg12a;
    wire       kill12 = k012a || (cvg_i12 == 4'd0 && aa_en && !cyc2_q);
    wire       pass12w = cyc3_q ? !k012a : (zpass12a && !arej12a && !kill12);
    wire [4:0] csum12 = {1'b0, cvg_i12} + {2'd0, mcvg12a};
    reg  [2:0] cvo12;
    always @(*) begin
        case (cvg_dst)
            2'd0: cvo12 = blend12a ? ((csum12 >= 5'd8) ? 3'd7 : csum12[2:0]) : (cvg_i12[2:0] - 3'd1);
            2'd1: cvo12 = csum12[2:0];
            2'd2: cvo12 = 3'd7;
            default: cvo12 = mcvg12a;
        endcase
    end
    wire [2:0] cv312w = cyc2_q ? {3{comb12a[7]}} : cvo12;
    always @(posedge clk) begin
        v12a <= v11b & ~rst; x12a <= x11b; bs12a <= bs11b;
        comb12a <= comb11b; mem12a <= mem11b; pa12a <= pa11b; sha12a <= sha11b; mcvg12a <= mcvg11b;
        depth12a <= depth11b;
        zcm12a <= cvg_sb * zdifb; cvg12a <= cvg_sb; zinter12a <= zinter; zpass12a <= zpass11; arej12a <= arej11; k012a <= k0_11b;
        blend12a <= blend_en;
        ben12a <= blend_en; ovf12a <= ovf11b;
    end
    reg        v12;
    reg [9:0]  x12;
    reg [31:0] comb12, mem12;
    reg [7:0]  pa12, sha12;
    reg [2:0]  mcvg12;
    reg [13:0] depth12;
    reg        pass12, ben12, ovf12;
    always @(posedge clk) begin
        v12 <= v12a & ~rst; x12 <= x12a; bs12 <= bs12a;
        comb12 <= comb12a; mem12 <= mem12a; pa12 <= pa12a; sha12 <= sha12a; mcvg12 <= mcvg12a;
        depth12 <= depth12a; pass12 <= pass12w; ben12 <= ben12a; ovf12 <= ovf12a; cv312 <= cv312w;
    end
    // The one blender, as the combiner (above): a 2-cycle pixel's first cycle takes the pixel as P11b has it (early), its second -- or a
    // 1-cycle pixel's only one -- the same pixel's three clocks on (late: P12b), with the first cycle's result (b0_r) in P's place.
    reg        v12b, bs12b, pass12b, ben12b, ovf12b;
    reg [9:0]  x12b;
    reg [31:0] comb12b, mem12b;
    reg [7:0]  pa12b, sha12b;
    reg [2:0]  mcvg12b, cv312b;
    reg [13:0] depth12b;
    always @(posedge clk) begin
        v12b <= v12 & ~rst; x12b <= x12; bs12b <= bs12;
        comb12b <= comb12; mem12b <= mem12; pa12b <= pa12; sha12b <= sha12; mcvg12b <= mcvg12;
        depth12b <= depth12; pass12b <= pass12; ben12b <= ben12; ovf12b <= ovf12; cv312b <= cv312;
    end
    reg        bu_e;
    always @(posedge clk) bu_e <= v11 & ~rst & cyc1_q;
    reg [23:0] b0_r;                         // 2-cycle: the first cycle always blends, >> 5 (8 bits: the sum wraps)
    wire [63:0] bo1_w = bl_sel(!bu_e && cyc1_q, bu_e ? comb11b : cyc1_q ? { b0_r, comb12b[7:0] } : comb12b,
                               bu_e ? mem11b : mem12b, bu_e ? pa11b : pa12b, bu_e ? sha11b : sha12b, bu_e ? mcvg11b : mcvg12b, shb);
    reg        be13a;

    // ======================================================================
    // P13a: the last blender cycle's operands -- 2-cycle's second on the
    // first's result, or 1-cycle's (cycle 0's settings) on the pixel; P13b:
    // its sums; out: the result
    // ======================================================================
    reg        v13a;
    reg [9:0]  x13a;
    reg [63:0] bo13a;
    reg [31:0] comb13;
    reg [13:0] depth13;
    reg        pass13, ben13, ovf13;
    always @(posedge clk) begin
        v13a <= v12b & ~rst; x13a <= x12b; bs13a <= bs12b; be13a <= bu_e;
        bo13a <= bo1_w;
        comb13 <= comb12b;
        depth13 <= depth12b; pass13 <= pass12b; ben13 <= ben12b; ovf13 <= ovf12b; cv313 <= cv312b;
    end
    wire [41:0] s1_w = bl_sum(bo13a);
    always @(posedge clk) if (be13a) b0_r <= { s1_w[40:33], s1_w[26:19], s1_w[12:5] };
    reg        v13b;
    reg [9:0]  x13b;
    reg        bs13b;
    reg [41:0] fsum;
    reg [47:0] fpm;                 // the last cycle's P, M
    reg [17:0] frc;                 // the divisor's reciprocal
    reg        fblend, fdiv, fclr;
    reg [31:0] comb13b;
    reg [13:0] depth13b;
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
    wire [31:0] fin = cyc3_q ? 32'd0 : cyc2_q ? comb13b : {bl_w, 8'd0};
    // The RGB dither (the N64's: RGB_DITHER_SEL, other_h[7:6]): the blender's
    // 8 bits go to 5 by truncation, after the three low bits are compared with
    // a 3-bit threshold -- where the threshold is below them the channel goes
    // up a step. The threshold: a 4 x 4 matrix of the pixel's place on the
    // screen (0: the magic square, 1: the Bayer matrix: a pixel an eighth of
    // the way to the next step goes up in one of eight places of the matrix),
    // or noise (2: three random bits for each channel, the LFSR's [2:0], [5:3]
    // and [8:6] as the pixel's alpha dither sees it, carried here in nz1..nz5:
    // the console's dither calculation -- a game's dust, G_CD_NOISE, was
    // drawn as an ordered checker screen with the matrix in its place); 3: off.
    // Without it a smooth shaded or lit surface is cut into flat bands of one
    // of 32 levels. Copy and fill write their colors untouched. The three low
    // bits and the thresholds go on to the output (out_color is their sum, a
    // 5-bit step and a compare behind the registers: not on the blender's path).
    wire [2:0] dt13;
    rdp_dither_tab dtab (.off(other_h[7:6] == 2'd3 || cyc23_q || dither_off), .bayer(other_h[6]),
                         .y(dy13), .x(x13b[1:0]), .dt(dt13));
    // the noise: the LFSR at the pixel's P11 (the sample before its step), six clocks to P13b (P12b is one of them)
    reg [8:0] nz1, nz2, nz3, nz4, nz5, nz6;
    always @(posedge clk) begin nz1 <= lfsr[8:0]; nz2 <= nz1; nz3 <= nz2; nz4 <= nz3; nz5 <= nz4; nz6 <= nz5; end
    wire dnoise = other_h[7:6] == 2'd2 && !cyc23_q && !dither_off;
    reg [14:0] col_t;                           // R5 G5 B5, truncated
    reg [8:0]  col_lo;                          // the low three bits of R, G, B
    reg        col_rep;                         // a copy or a fill: the plane's bits are the pixel's own top ones
    reg [2:0]  col_dt, col_dg, col_db;
    reg        col_a;
    wire [4:0] dr, dg, db;
    rdp_dither_add dar (.c(col_t[14:10]), .lo(col_lo[8:6]), .d(col_dt), .o(dr));
    rdp_dither_add dag (.c(col_t[9:5]),   .lo(col_lo[5:3]), .d(col_dg), .o(dg));
    rdp_dither_add dab (.c(col_t[4:0]),   .lo(col_lo[2:0]), .d(col_db), .o(db));
    assign out_color = { dr, dg, db, col_a };
    assign out_sh = { col_rep ? {col_t[14:13], col_t[9:8], col_t[4]} : {col_lo[8:7], col_lo[5:4], col_lo[2]}, sh_crc({col_t, col_a}) };
    // A 32-bit image keeps the eight bits (docs/cimg32.md): the same compare, and where it takes the channel up, the
    // next multiple of 8 (255 from 248 up), else the eight bits as they were. The coverage goes whole, no hidden plane.
    function automatic [7:0] dith8;
        input [4:0] c, o;
        input [2:0] lo, d;
        dith8 = !(d < lo) ? {c, lo} : (c == 5'd31) ? 8'hFF : {o, 3'd0};
    endfunction
    assign out_c27 = { dith8(col_t[14:10], dr, col_lo[8:6], col_dt), dith8(col_t[9:5], dg, col_lo[5:3], col_dg),
                       dith8(col_t[4:0], db, col_lo[2:0], col_db), col_a, out_hid };
    always @(posedge clk) begin
        out_valid <= v13b & ~rst;
        out_x <= x13b; out_set <= bs13b;
        col_t  <= cyc3_q ? (x13b[0] ? fill[15:1] : fill[31:17])
                                : { fin[31:27], fin[23:19], fin[15:11] };
        col_a  <= cyc3_q ? (x13b[0] ? fill[0] : fill[16]) : cv313b[2];
        out_hid <= cyc3_q ? {2{x13b[0] ? fill[0] : fill[16]}} : cv313b[1:0];
        col_rep <= cyc23_q;
        col_lo <= cyc3_q ? 9'd0 : { fin[26:24], fin[18:16], fin[10:8] };
        out_r8 <= cyc3_q ? fill[{~x13b[1:0], 3'd0} +: 8] : fin[31:24];
        col_dt <= dnoise ? nz6[2:0] : dt13;
        col_dg <= dnoise ? nz6[5:3] : dt13;
        col_db <= dnoise ? nz6[8:6] : dt13;
        out_depth <= {depth13b, dzc};
        out_cpass <= pass13b;
        out_zpass <= pass13b && z_upd;
    end

    assign busy = v0 | v1 | v1b | v2 | v3 | v4 | (|av) | v5p | v5 | v6 | v7 | v8 | v8t | v8b | v8c | v9 | v9x | v10a | v10 | v10b | v11a | v11m | v11 | v11b | v12a | v12 | v12b | v13a | v13b | out_valid;
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

// The console's reciprocal for the perspective divide (lang/c/rdp/rdp.c n64_div, its two 64-entry tables): the point
// (15 bits) and the slope's low 10 bits (its upper ones are all ones). In logic, a LUT for each bit of the 25 (as a
// ROM block its read is on P2's path, and it takes an M10K).
(* altera_attribute = "-name AUTO_ROM_RECOGNITION OFF" *)
module rdp_div_rom (
    input  wire [5:0]  idx,
    output wire [14:0] point,
    output wire [9:0]  slope
);
    (* romstyle = "logic" *) reg [24:0] e;
    always @(*) begin
        case (idx)
            6'd0: e = {15'h4000, 10'h303};
            6'd1: e = {15'h3f04, 10'h30b};
            6'd2: e = {15'h3e10, 10'h311};
            6'd3: e = {15'h3d22, 10'h319};
            6'd4: e = {15'h3c3c, 10'h320};
            6'd5: e = {15'h3b5d, 10'h325};
            6'd6: e = {15'h3a83, 10'h32d};
            6'd7: e = {15'h39b1, 10'h332};
            6'd8: e = {15'h38e4, 10'h337};
            6'd9: e = {15'h381c, 10'h33d};
            6'd10: e = {15'h375a, 10'h342};
            6'd11: e = {15'h369d, 10'h347};
            6'd12: e = {15'h35e5, 10'h34c};
            6'd13: e = {15'h3532, 10'h350};
            6'd14: e = {15'h3483, 10'h355};
            6'd15: e = {15'h33d9, 10'h359};
            6'd16: e = {15'h3333, 10'h35d};
            6'd17: e = {15'h3291, 10'h362};
            6'd18: e = {15'h31f4, 10'h364};
            6'd19: e = {15'h3159, 10'h369};
            6'd20: e = {15'h30c3, 10'h36c};
            6'd21: e = {15'h3030, 10'h370};
            6'd22: e = {15'h2fa1, 10'h373};
            6'd23: e = {15'h2f15, 10'h376};
            6'd24: e = {15'h2e8c, 10'h379};
            6'd25: e = {15'h2e06, 10'h37c};
            6'd26: e = {15'h2d83, 10'h37f};
            6'd27: e = {15'h2d03, 10'h382};
            6'd28: e = {15'h2c86, 10'h384};
            6'd29: e = {15'h2c0b, 10'h387};
            6'd30: e = {15'h2b93, 10'h38a};
            6'd31: e = {15'h2b1e, 10'h38c};
            6'd32: e = {15'h2aab, 10'h38e};
            6'd33: e = {15'h2a3a, 10'h391};
            6'd34: e = {15'h29cc, 10'h393};
            6'd35: e = {15'h2960, 10'h395};
            6'd36: e = {15'h28f6, 10'h397};
            6'd37: e = {15'h288e, 10'h399};
            6'd38: e = {15'h2828, 10'h39b};
            6'd39: e = {15'h27c4, 10'h39d};
            6'd40: e = {15'h2762, 10'h39f};
            6'd41: e = {15'h2702, 10'h3a1};
            6'd42: e = {15'h26a4, 10'h3a3};
            6'd43: e = {15'h2648, 10'h3a4};
            6'd44: e = {15'h25ed, 10'h3a6};
            6'd45: e = {15'h2594, 10'h3a8};
            6'd46: e = {15'h253d, 10'h3a9};
            6'd47: e = {15'h24e7, 10'h3aa};
            6'd48: e = {15'h2492, 10'h3ac};
            6'd49: e = {15'h243f, 10'h3ae};
            6'd50: e = {15'h23ee, 10'h3af};
            6'd51: e = {15'h239e, 10'h3b0};
            6'd52: e = {15'h234f, 10'h3b2};
            6'd53: e = {15'h2302, 10'h3b3};
            6'd54: e = {15'h22b6, 10'h3b5};
            6'd55: e = {15'h226c, 10'h3b5};
            6'd56: e = {15'h2222, 10'h3b7};
            6'd57: e = {15'h21da, 10'h3b8};
            6'd58: e = {15'h2193, 10'h3b9};
            6'd59: e = {15'h214d, 10'h3ba};
            6'd60: e = {15'h2108, 10'h3bc};
            6'd61: e = {15'h20c5, 10'h3bc};
            6'd62: e = {15'h2082, 10'h3be};
            default: e = {15'h2041, 10'h3be};
        endcase
    end
    assign point = e[24:10];
    assign slope = e[9:0];
endmodule

// One axis of the texture coordinate's clamp, mirror and mask (the model's tc_axis): the texel (v >> 5) and its neighbour (+ 1) in the tile
// -- c0, c1, the wrapped coordinates -- and the fraction fr. The two are chosen first (0 below the tile's start, the tile's last texel
// from there up, where the tile clamps), then each is masked (a mirror turns a texel in an odd repeat) by the mask's thermometer.
module rdp_tcaxis (
    input  wire signed [23:0] v,
    input  wire signed [10:0] mx,            // the tile's last texel: (hi - lo) >> 2, signed (found a clock ahead, with the tile's other fields)
    input  wire               ng_i, gx_i,    // below the start (clamped: 0), at or past the end (clamped: the last): found a clock ahead, with v
    input  wire               clampbit, mirror,
    input  wire [3:0]         mask,
    input  wire               cpy,           // copy mode (a texture rectangle's): the texel is the group's plus the pixel's place, never clamped, its mask at most 10 bits
    input  wire [2:0]         cm,
    output wire [10:0]        c0, c1,
    output wire [4:0]         fr
);
    wire [18:0] i0 = v[23:5];
    wire [18:0] ia = i0 + {16'd0, cpy ? cm : 3'd0};
    wire [18:0] ja = cpy ? ia : ia + 19'd1;
    wire        ng = ng_i;                                         // below the start: 0
    wire        gx = gx_i;                                         // at or past the end: the last
    wire [3:0]  mq = (cpy && mask > 4'd10) ? 4'd10 : mask;
    wire [15:0] t0 = ng ? 16'd0 : gx ? {{5{mx[10]}}, mx} : ia[15:0];
    wire [15:0] t1 = ng ? 16'd0 : gx ? {{5{mx[10]}}, mx} : ja[15:0];
    wire        m0 = mirror && mask != 4'd0 && t0[mq];
    wire        m1 = mirror && mask != 4'd0 && t1[mq];
    genvar b;
    generate for (b = 0; b < 11; b = b + 1) begin : bb
        wire mb = mask == 4'd0 || b < mq;
        assign c0[b] = mb & (t0[b] ^ m0);
        assign c1[b] = mb & (t1[b] ^ m1);
    end endgenerate
    assign fr = (cpy || ng || gx) ? 5'd0 : v[4:0];
endmodule
