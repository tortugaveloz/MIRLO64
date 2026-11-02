// RDP -- Mirlo's rasterizer for the N64's own command stream, as a game's
// microcode on the RSP (rtl/rsp) gives it. The contract is the C model,
// lang/c/rdp/rdp.c built with RDP_N64: for the same command stream the RTL
// writes the same memory. From RDP (rtl/rdp), which draws the
// geometry core's triangles: here the triangles are the N64's (0x08..0x0F),
// TMEM is the N64's 4 KiB loaded by LOAD BLOCK / LOAD TILE / LOAD TLUT and
// decoded by the tile's format when sampled, texture rectangles, 2-cycle and
// copy mode are in, and the images are the game's (RGBA5551, the N64's byte
// order). The coverage (anti-aliasing) is the N64's (lang/c/rdp/rdp.h,
// RDP_CVG): with AA_EN a triangle is walked four subscanlines a row, and its
// pixels carry their 8-sample masks. YUV textures are converted (SET_CONVERT).
// The texture LOD of a 2-cycle primitive is the model's (docs/lod.md): rdp_pix
// computes it; this module issues the pixels in the walk's order, with the
// ghosts that needs, and gives the next line's S, T, W. A colour image may be a
// 32-bit one (docs/cimg32.md): cimg32 below.
//
// Commands are executed one at a time. A primitive is drawn row by row: the
// row's Z and color spans are read into on-chip line buffers, the pixels run
// through rdp_pix at one a clock, then the span is written back (only the
// pixels that passed). State commands wait for the previous primitive to
// finish, so the pixel pipeline never sees a state change mid-flight.
module rdp_top (
    input  wire        clk,
    input  wire        rst,

    // command words (the wrapper's FIFO)
    input  wire        cmd_valid,
    output wire        cmd_ready,
    input  wire [31:0] cmd_data,

    // the N64's memory (LiteDRAM native style, byte addresses as the commands give them)
    output wire        m_cmd_valid,
    input  wire        m_cmd_ready,
    output wire        m_cmd_we,
    output wire [31:0] m_cmd_addr,
    output wire        m_wdata_valid,
    input  wire        m_wdata_ready,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wdata_we,
    input  wire        m_rdata_valid,
    input  wire [31:0] m_rdata,


    // status
    output reg  [31:0] sync_count,      // SYNC FULLs completed (everything before in DRAM)
    output reg         sync_ev,         // (a cycle) one has just completed: the DP interrupt
    output reg  [31:0] load_count,      // loads completed
    output reg  [15:0] unknown_ops,
    output wire        idle,            // no command in progress, nothing pending
    output wire        tmem_busy        // a texture load is running (DPC_STATUS' TMEM busy)
);
    // ======================================================================
    // state (lang/c/rdp/rdp.h rdp_t)
    // ======================================================================
    reg [23:0] other_h;
    reg [31:0] other_l;
    reg [23:0] cc_hi;
    reg [31:0] cc_lo;
    reg [31:0] fill, fog, blend, prim, env;
    reg [7:0]  prim_lod_frac;
    reg [8:0]  k4, k5;              // SET CONVERT's two constants of the combiner
    reg [35:0] cv_k03;              // SET CONVERT's K0..K3, 9 bits each (K0 at [35:27]): a YUV texel's conversion
    reg [15:0] prim_z, prim_dz;
    reg [11:0] sc_xh, sc_yh, sc_xl, sc_yl;
    reg        sc_field, sc_odd;    // SET_SCISSOR's field mode: only the rows of the field kept (odd or even) are drawn
    reg [31:0] cimg, zimg, timg;
    reg [10:0] cimg_width, timg_width;
    reg [1:0]  timg_size;
    reg        cimg_ok;             // the colour image is a 16-bit, an 8-bit or a 32-bit one (what is drawn to)
    // An 8-bit colour image (lang/c/rdp/rdp.c wr8): a pixel is a byte at cimg + y * width + x, the blender's red as it
    // is (copy: the texel's; fill: the fill colour's byte for the address). Written only: the pixels read neither it nor
    // a depth, and no hidden coverage goes with it. The pixel pipe's results go through the new buffers as for a 16-bit
    // image, the byte in a pixel's colour bits 15:8; the write engine puts a word's two pixels in their half of the
    // memory word (rdp_mem wr_c8). The rows must start on a 4-byte boundary, as a 16-bit image's.
    reg        cimg8;
    // A 32-bit colour image (docs/cimg32.md; lang/c/rdp/rdp.c draw_pixel_cvg): a pixel is a word at cimg + (y * width + x)
    // * 4, R G B and the coverage in the alpha byte's top three bits; the depth image keeps its 16-bit layout, and no
    // hidden coverage goes with it. The line buffers keep their blocks: the old colour buffer holds a pixel a word, the
    // new one {R8 G8 B8 cvg3} an entry (rdp_mem, rdp_pix), so a set holds 512 pixels of a row and a longer span is drawn
    // in two halves (S_SPL). Copy mode into one draws nothing, as the model.
    reg        cimg32;

    // The 8 tiles, in two small RAMs (SET_TILE writes the format half,
    // SET_TILESIZE and LOAD_TILE the size half): as flip-flops they were 688
    // registers plus their write decode and an 8-way read mux. Read through
    // one registered port (ts_* below), at the tile of the pass about to enter
    // rdp_pix's P5 (tl_ra): the primitive's when nothing draws (the loads').
    //   tl_attr: {fmt 3, size 2, pal 4, line 9, tmem 9, cs, ms, ct, mt, masks 4, shifts 4, maskt 4, shiftt 4}
    //   tl_size: {uls 12, ult 12, lrs 12, lrt 12}
    (* ramstyle = "M10K, no_rw_check" *) reg [46:0] tl_attr [0:7];
    (* ramstyle = "M10K, no_rw_check" *) reg [47:0] tl_size [0:7];
    integer ti;
    initial for (ti = 0; ti < 8; ti = ti + 1) begin tl_attr[ti] = 47'd0; tl_size[ti] = 48'd0; end

    wire [1:0] cyc = other_h[21:20];
    wire [9:0] sc_x0 = sc_xh[11:2], sc_x1 = sc_xl[11:2];
    wire [9:0] sc_y0 = sc_yh[11:2], sc_y1 = sc_yl[11:2];

    // does the pixel need what is in memory (the color span must be loaded):
    // with FORCE_BL, a blender cycle in use that selects it; without, the
    // pixel is the last cycle's P alone -- memory itself, or (two cycles)
    // the first cycle's blend of it. An opaque surface's mode names memory
    // in a blender that never runs (the N64 blends its edges by coverage),
    // and its spans were read for nothing: half a frame's reads.
    wire rd_mem0 = other_l[31:30] == 2'd1 || other_l[23:22] == 2'd1;
    wire rd_mem1 = other_l[29:28] == 2'd1 || other_l[21:20] == 2'd1;
    wire rd_p    = (cyc == 2'd1) ? (other_l[29:28] == 2'd1 || (other_l[29:28] == 2'd0 && rd_mem0))
                                 : other_l[31:30] == 2'd1;
    // (and the memory's coverage: IM_RD, with AA_EN or CVG_X_ALPHA; and its
    // colour for CLR_ON_CVG with CVG_X_ALPHA and without AA_EN, where a pixel
    // with no scaled coverage is written as the blender's M)
    wire need_mem = (cyc[1] || cimg8) ? 1'b0
                  : ((other_l[6] && (other_l[3] || other_l[12])) || (other_l[7] && other_l[12] && !other_l[3])) ? 1'b1
                  : !other_l[14] ? rd_p : (cyc == 2'd1) ? (rd_mem0 | rd_mem1) : rd_mem0;
    wire z_cmp = other_l[4] && !cyc[1] && !cimg8;       // (copy and fill mode have no depth, nor an 8-bit image)
    wire z_upd = other_l[5] && !cyc[1] && !cimg8;

    // ======================================================================
    // command intake
    // ======================================================================
    localparam S_IDLE = 5'd0,  S_TRI_INIT = 5'd1, S_ROW = 5'd2,  S_SETUP = 5'd3,
               S_AAINIT = 5'd4,
               S_PIX = 5'd6,   S_FIN = 5'd7,      S_NEXT = 5'd10,
               S_FILL = 5'd12, S_LOAD = 5'd13,    S_SYNC = 5'd14, S_RECT_INIT = 5'd15,
               S_LOAD_INIT = 5'd16, S_VSET = 5'd18,
               S_XL = 5'd19,     S_SPL = 5'd20,    S_TLOAD = 5'd9, S_GO = 5'd11;
    reg [4:0]  st;
    reg [31:0] w0;                 // first word of the command being executed
    reg [5:0]  nwords, widx;       // total words, index of the next
    reg        collecting;
    reg        exec_go;            // command complete: execute
    reg        busy_exec;          // executing (FSM not idle)
    reg [31:0] w1;                 // second word
    // The next command's first two words (nw0, nw1) come in while the one before still executes; every word goes to the
    // staging RAM too (word k at k), where a triangle's edges (words 2..7) and a texture rectangle's S, T and steps (2, 3)
    // are read back at its start (S_TLOAD). In S_IDLE the command about to start is the next one (nw0, nw1, nop).
    reg [31:0] nw0, nw1;

    // triangle coefficients, decoded as they arrive; the attributes' words
    // (from the ninth on) wait in a staging RAM and are put together -- each
    // value, DaDx and DaDe from its integer and fraction halves -- in S_VSET
    reg        t_lft;
    reg [2:0]  t_tile, t_lvl;      // (t_lvl: the level count, the LOD's)
    reg [13:0] t_yl, t_ym, t_yh;
    reg [31:0] t_xl, t_dxl, t_xh, t_dxh, t_xm, t_dxm;
    // the attribute start values go straight into e[] (row-start values):
    // commands are taken only while nothing draws, and TRI_INIT turns them
    // into the first row's in place
    // adx[] steps every pixel's eight attributes at once: flip-flops. e[]
    // and ade[] are read one at a time (TRI_INIT, the span setup, the row
    // step): LUT RAM, one read and one write a clock -- as flip-flops their
    // read muxes, write muxes and eight row-step adders cost ~ALMs
    reg [31:0] adx [0:7];
    (* ramstyle = "MLAB, no_rw_check" *) reg [31:0] e_ram [0:7];
    // ade_ram's rows 8..15 hold a copy of adx[] (DaDx), for the span setup's
    // products: read there, they keep an 8-way choice of adx out of the shared
    // multiplier's operand select. The MLAB is 32 rows deep: they cost nothing
    (* ramstyle = "MLAB, no_rw_check" *) reg [31:0] ade_ram [0:15];
    // (a texrect's second 64-bit word, S T / DsDx DtDy, lands in t_xl, t_dxl)

    function automatic [5:0] cmd_len;        // in 32-bit words, from the first word
        input [31:0] w;
        reg [5:0] op;
        begin
            op = w[29:24];
            if (op[5:3] == 3'b001)
                cmd_len = 6'd8 + (op[2] ? 6'd16 : 6'd0) + (op[1] ? 6'd16 : 6'd0) + (op[0] ? 6'd4 : 6'd0);
            else if (op == 6'h24 || op == 6'h25)
                cmd_len = 6'd4;
            else
                cmd_len = 6'd2;
        end
    endfunction

    wire [5:0] op0 = cmd_data[29:24];
    wire [5:0] op  = w0[29:24];       // (the command being executed; nop: the one about to start, in S_IDLE)
    wire [5:0] nop = nw0[29:24];
    wire       n_tri = nop[5:3] == 3'b001;      // the N64's triangles, 0x08..0x0F
    // The next command's words are taken while the one before executes, once that has read its own words out of the
    // staging RAM (S_TLOAD, S_XL, S_VSET): a triangle's up to 44 words, a word a clock, come in under the rows of the
    // primitive before it. It executes when that is done (a triangle while it drains: tri_early).
    assign cmd_ready = !exec_go && (!busy_exec || (st != S_TLOAD && st != S_XL && st != S_VSET));


    // ======================================================================
    // RAMs
    // ======================================================================
    // TMEM: two copies of two read ports for the filter's three texels (rdp_tmem)
    wire [10:0] tm_ra0, tm_ra1, tm_ra2;
    wire [15:0] tm_q0, tm_q1, tm_q2, tm_h0, tm_h1, tm_h2, tl_q0, tl_q1, tl_q2;
    wire [7:0]  tl_a0, tl_a1, tl_a2;
    wire [3:0]  tm_we;
    wire [8:0]  tm_wa;
    wire [15:0] tm_wd0, tm_wd1, tm_wd2, tm_wd3;
    rdp_tmem tmem0 (.clk(clk), .we(tm_we), .wa(tm_wa), .wd0(tm_wd0), .wd1(tm_wd1), .wd2(tm_wd2), .wd3(tm_wd3),
                    .ra_a(tm_ra0), .q_a(tm_q0), .qh_a(tm_h0), .ta_a(tl_a0), .tq_a(tl_q0),
                    .ra_b(tm_ra1), .q_b(tm_q1), .qh_b(tm_h1), .ta_b(tl_a1), .tq_b(tl_q1));
    rdp_tmem #(.RA(0)) tmem1 (.clk(clk), .we(tm_we), .wa(tm_wa), .wd0(tm_wd0), .wd1(tm_wd1), .wd2(tm_wd2), .wd3(tm_wd3),
                    .ra_a(11'd0), .q_a(), .qh_a(), .ta_a(8'd0), .tq_a(),
                    .ra_b(tm_ra2), .q_b(tm_q2), .qh_b(tm_h2), .ta_b(tl_a2), .tq_b(tl_q2));

    // span line buffers: 512 words of two pixels, and two sets of each -- a
    // row is on the other set than the row before, so that its reads, its
    // pixels and the write-back of the row before overlap.
    //   old   what memory holds: the read engine fills them, the pixel pipe
    //         reads them (Z to compare, colour to blend)
    //   new   the pixels' results and their pass masks: the write engine
    //         takes them back to memory
    wire        mw_z_we, mw_c_we;
    wire [9:0]  mw_addr;
    wire [31:0] mw_data;
    wire [9:0]  pix_lb_ra, pix_clb_ra;      // (the depth's and the colour's: a 32-bit image's colour word is the pixel's)
    wire [31:0] zlb_rd, clb_rd;
    rdp_sdpram #(.AW(10), .DW(32), .NB(2)) zold (.clk(clk), .we(mw_z_we), .waddr(mw_addr), .wdata(mw_data), .wbe(2'b11), .raddr(pix_lb_ra), .rdata(zlb_rd));
    rdp_sdpram #(.AW(10), .DW(32), .NB(2)) cold (.clk(clk), .we(mw_c_we), .waddr(mw_addr), .wdata(mw_data), .wbe(2'b11), .raddr(pix_clb_ra), .rdata(clb_rd));
    reg         zn_we, cn_we, msk_we;
    reg  [9:0]  nb_wa;                  // the depth buffers' {set, word}
    reg         nb_x0;                  // the pixel's x[0]: the new colour buffer's address, a 32-bit image's {set, x[8:0]}
    wire [9:0]  cn_wa = cimg32 ? {nb_wa[9], nb_wa[7:0], nb_x0} : nb_wa;
    reg  [35:0] zn_wd;                  // {depth word 16, hidden 2} x 2, the even pixel first (the dz code's low two are the hidden ones)
    reg  [35:0] cn_wd;                  // {colour 16, hidden 2} x 2, the even pixel first (a 32-bit image: {R8 G8, 0, pass, B8 cvg3, 0})
    reg  [1:0]  nb_be, cn_be;
    reg  [1:0]  zm_wd, cm_wd, msk_be;
    wire [9:0]  mem_lb_ra;
    wire [35:0] znew_rd;
    wire [35:0] cnew_rd;
    wire [1:0]  zm_rd, cm_rd;
    wire [9:0]  zn_ra = mem_lb_ra;
    rdp_sdpram #(.AW(10), .DW(36), .NB(2)) znew (.clk(clk), .we(zn_we), .waddr(nb_wa), .wdata(zn_wd), .wbe(nb_be), .raddr(zn_ra), .rdata(znew_rd));
    rdp_sdpram #(.AW(10), .DW(36), .NB(2)) cnew (.clk(clk), .we(cn_we), .waddr(cn_wa), .wdata(cn_wd), .wbe(cn_be), .raddr(mem_lb_ra), .rdata(cnew_rd));
    // the raw hidden-bit buffers (rdp_mem): the span's groups' words, by {set,
    // group mod 128}, a copy for the pixels and one for the write engine
    // The depth image has its own (its dz code's low two bits, rule 8 of docs/rdp_exact.md): a copy for the pixels, and in the
    // write engine's the two images' words, the colour image's in the lower half
    wire        hl_we, hl_z;
    wire [7:0]  hl_wa, pix_hl_ra, pix_hz_ra;
    wire [8:0]  mem_hl_ra;
    wire [31:0] pix_hl_rd, pix_hz_rd, mem_hl_rd;
    rdp_sdpram #(.AW(8), .DW(32), .NB(1)) hraw0 (.clk(clk), .we(hl_we && !hl_z), .waddr(hl_wa), .wdata(mw_data), .wbe(1'b1), .raddr(pix_hl_ra), .rdata(pix_hl_rd));
    rdp_sdpram #(.AW(8), .DW(32), .NB(1)) hrawz (.clk(clk), .we(hl_we && hl_z), .waddr(hl_wa), .wdata(mw_data), .wbe(1'b1), .raddr(pix_hz_ra), .rdata(pix_hz_rd));
    rdp_sdpram #(.AW(9), .DW(32), .NB(1)) hraw1 (.clk(clk), .we(hl_we), .waddr({hl_wa[7], hl_z, hl_wa[6:0]}), .wdata(mw_data), .wbe(1'b1), .raddr(mem_hl_ra), .rdata(mem_hl_rd));
    reg  [9:0]  rwb [0:1];              // the rows' colour word base (bits 9:0), by set
    reg  [9:0]  rzb [0:1];              // ... and the depth's
    // (a 32-bit image's pass bits are in cnew's entries, so that cmsk keeps zmsk's addresses: the fit packs the two)
    rdp_sdpram #(.AW(10), .DW(2), .NB(2)) zmsk (.clk(clk), .we(msk_we), .waddr(nb_wa), .wdata(zm_wd), .wbe(msk_be), .raddr(zn_ra), .rdata(zm_rd));
    rdp_sdpram #(.AW(10), .DW(2), .NB(2)) cmsk (.clk(clk), .we(msk_we), .waddr(nb_wa), .wdata(cm_wd), .wbe(msk_be), .raddr(mem_lb_ra), .rdata(cm_rd));
    // every command's words: word k at k (a triangle's attributes from 8 on)
    wire        vr_we = cmd_valid && cmd_ready;
    wire [5:0]  vr_wa6 = collecting ? widx : 6'd0;
    reg  [5:0]  vr_ra;
    wire [31:0] vr_q;
    rdp_sdpram #(.AW(6), .DW(32), .NB(1)) vram (.clk(clk), .we(vr_we), .waddr(vr_wa6),
                                                 .wdata(cmd_data), .wbe(1'b1), .raddr(vr_ra), .rdata(vr_q));

    // ======================================================================
    // the pixel pipeline
    // ======================================================================
    reg         p_shade, p_tex, p_zbuf, p_direct;
    reg  [2:0]  p_tile;
    reg         pix_valid;
    reg  [9:0]  pix_x;             // (the pixel's place in the row's words: its column plus the row's odd halfword, par)
    wire        par;                // the row (the one the row walker is at) starts on an odd halfword (a 16-bit image of an odd width)
    reg         par_c;              // ... the row being drawn
    reg  [2:0]  cy;                 // ... its row, for the dither
    wire [9:0]  xsp;                // the span's columns in the row's words: xs + par_c, xe + par_c
    wire [10:0] xep;
    reg  [7:0]  pix_mask;           // its samples (0xFF: all)
    reg         p_aa;               // a triangle: the N64's walker
    reg         p_aaen;             // ... with AA_EN (the pixel is drawn for any sample; else for sample 7 only)
    reg  [31:0] cur [0:7];          // iterated attributes (triangles)
    // texrects step S and T in cur[4], cur[5] by adx[4], adx[5] -- the
    // triangle's own S/T adders -- and pass s = cur[4] >>> shS, t = cur[5] >>> 5
    reg  [2:0]  shS;
    // a triangle's S and T are the N64's -- s10.5 in the integer half, against
    // a W of s.15 -- and go to the pipeline as they are: its perspective divide, and the
    // affine path, take the integer halves (lang/c/rdp/rdp.c draw_pixel_cvg)
    // The LOD (docs/lod.md): a 2-cycle primitive with TEX_LOD, or whose combiner reads LOD_FRACTION (C 13 in a colour, C
    // 0 in an alpha, either cycle). (Registered: the modes are set many clocks before a primitive's first pixel.)
    reg         lod_on;
    always @(posedge clk) lod_on <= cyc == 2'd1 && (other_h[16] || cc_hi[19:15] == 5'd13 || cc_hi[4:0] == 5'd13
                                                         || cc_hi[11:9] == 3'd0 || cc_lo[20:18] == 3'd0);
    reg         lod_lft;            // the walk goes to the right (a triangle's lft; a rectangle's always): x + 1 is next
    reg  [2:0]  lod_lvl;            // the level count (the triangle's level field; a rectangle's 0)
    reg  [4:0]  min_lvl;            // SET_PRIM_COLOR's minimum level
    reg         pix_ghost;          // the pixel issued is a ghost: through the perspective divide for its neighbour's LOD, never drawn
    // The next line's S, T, W, which rdp_pix's P0 takes the clock after a pixel's (pix_valid down): plus DaDy, of which a
    // triangle keeps the top 17 bits (ldy_*, from its texture block), or a texture rectangle's row step (t_dxl: DsDx
    // flipped, DtDy not), the exact next row as the model takes it (the console keeps 17 bits of it as of a 16.16 DaDy,
    // which would drop t_dxl's low 4 bits). On the whole 32 bits, as the model adds them: a sum that
    // wraps wraps there too.
    reg  [16:0] ldy_s, ldy_t, ldy_w;
    // The console moves a pixel without full coverage to its first covered sample by the shade's DaDx and DaDy >> 14 (13 bits;
    // adx's bits 26:14, sdy), and takes the console's dz code from the integer halves of DzDx and DzDy: |DzDx| + |DzDy|
    // (a negative one's complement, 15 bits), the place of its top set bit + 1 (docs/rdp_exact.md, rule 4)
    reg  [12:0] sdy [0:3];
    reg  [3:0]  dzc_r;
    wire [14:0] dzc_a = vgx[31] ? ~vgx[30:16] : vgx[30:16];
    wire [14:0] dzc_b = zdy[31] ? ~zdy[30:16] : zdy[30:16];
    wire [15:0] dzc_s = {1'b0, dzc_a} + {1'b0, dzc_b};
    reg  [3:0]  dzc_p;
    integer dzi;
    always @* begin
        dzc_p = 4'd0;
        if (dzc_s[15:14] != 2'b00) dzc_p = 4'd15;
        else for (dzi = 13; dzi >= 0; dzi = dzi - 1) if (dzc_s[dzi] && dzc_p == 4'd0) dzc_p = dzi[3:0] + 4'd1;
    end
    wire        lpass = !pix_valid;
    wire        rflip = op == 6'h25;
    // (the console keeps a rectangle's row step to the same 17 bits of a 16.16 DaDy as a triangle's: DsDx's or DtDy's low 4 bits are 0)
    wire [31:0] dy_s = p_direct ? (rflip ? {{16{t_dxl[31]}}, t_dxl[31:20], 4'd0} : 32'd0) : {ldy_s, 15'd0};
    wire [31:0] dy_t = p_direct ? (rflip ? 32'd0 : {{16{t_dxl[15]}}, t_dxl[15:4], 4'd0}) : {ldy_t, 15'd0};
    wire [31:0] cur_s = cur[4] + (lpass ? dy_s : 32'd0);
    wire [31:0] cur_t = cur[5] + (lpass ? dy_t : 32'd0);
    wire [31:0] cur_w = {cur[6][31:15] + ((lpass && !p_direct) ? ldy_w : 17'd0), cur[6][14:0]};
    // (rdp_pix takes the 16 bits of each: a rectangle's s10.5 after the shift, a triangle's integer half)
    wire [31:0] pr_sd = asr(cur_s, {2'b0, shS}), pr_td = asr(cur_t, 5'd5);
    wire [15:0] pr_s = p_direct ? pr_sd[15:0] : cur_s[31:16];
    wire [15:0] pr_t = p_direct ? pr_td[15:0] : cur_t[31:16];
    // The pass's tile's fields, registered: indexing tile[p_tile] straight
    // into the pixel pipe was the worst path of the first fit (-4 ns). rdp_pix
    // names the tile a clock before its P5 takes the fields (tl_ra): the
    // primitive's, the next (TEXEL1's), or with the LOD the pixel's level's.
    // One read a clock serves both passes (it was a second copy of each RAM
    // for the next tile); the tiles change only between primitives.
    wire [2:0] tl_ra;
    reg [2:0]  ts_fmt;
    reg [1:0]  ts_sz;
    reg [3:0]  ts_pal;
    reg [8:0]  ts_line;
    reg [8:0]  ts_tmem;
    reg        ts_cs, ts_ms, ts_ct, ts_mt;
    reg [3:0]  ts_masks, ts_shifts, ts_maskt, ts_shiftt;
    reg [11:0] ts_uls, ts_ult, ts_lrs, ts_lrt;
    always @(posedge clk) begin
        {ts_fmt, ts_sz, ts_pal, ts_line, ts_tmem, ts_cs, ts_ms, ts_ct, ts_mt, ts_masks, ts_shifts, ts_maskt, ts_shiftt} <= tl_attr[tl_ra];
        {ts_uls, ts_ult, ts_lrs, ts_lrt} <= tl_size[tl_ra];
    end
    // which tiles are YUV 16-bit ones (a pass that converts checks its tile's, or with CONVERT_ONE the one before)
    reg [7:0]  yuv_vec;
    initial yuv_vec = 8'd0;
    wire two_pass;
    wire        px_valid, px_cpass, px_zpass, pix_busy, px_set;
    reg         set_cur;            // the line-buffer set of the row being drawn
    wire [9:0]  px_x;
    wire [15:0] px_color;
    wire [17:0] px_depth;
    wire [7:0]  px_r8;
    wire [26:0] px_c27;                 // a 32-bit image's pixel: {R8 G8 B8 cvg3}
    wire [1:0]  px_hid;
    rdp_pix pix (
        .clk(clk), .rst(rst),
        .other_h(other_h), .other_l(other_l), .cc_hi(cc_hi), .cc_lo(cc_lo),
        .prim(prim), .env(env), .fog(fog), .blend(blend),
        .prim_lod_frac(prim_lod_frac), .k4(k4), .k5(k5), .cv_k03(cv_k03), .prim_z(prim_z), .fill(fill),
        .prim_dz(prim_dz), .need_mem(need_mem), .c8(cimg8), .c32(cimg32), .p_aa(p_aa), .z_dx(adx[7]), .z_dy(zdy),
        .sh_dx({adx[3][26:14], adx[2][26:14], adx[1][26:14], adx[0][26:14]}), .sh_dy({sdy[3], sdy[2], sdy[1], sdy[0]}), .dzc_x(dzc_r),
        .p_shade(p_shade), .p_tex(p_tex), .p_zbuf(p_zbuf), .p_direct(p_direct),
        .t_fmt(ts_fmt), .t_size(ts_sz), .t_pal(ts_pal), .t_line(ts_line), .t_tmem(ts_tmem),
        .t_cs(ts_cs), .t_ms(ts_ms), .t_ct(ts_ct), .t_mt(ts_mt),
        .t_masks(ts_masks), .t_shifts(ts_shifts), .t_maskt(ts_maskt), .t_shiftt(ts_shiftt),
        .t_uls(ts_uls), .t_ult(ts_ult), .t_lrs(ts_lrs), .t_lrt(ts_lrt),
        .tl_ra(tl_ra), .p_tile(p_tile), .yuv_vec(yuv_vec), .two_pass(two_pass),
        .lod_on(lod_on), .lod_lft(lod_lft), .lod_lvl(lod_lvl), .min_lvl(min_lvl), .in_ghost(pix_ghost),
        .in_valid(pix_valid), .in_x(pix_x), .in_par(par_c), .in_set(set_cur), .row_y(sc_field ? cy[2:1] : cy[1:0]),
        .in_r(cur[0]), .in_g(cur[1]), .in_b(cur[2]), .in_a(cur[3]),
        .in_s(pr_s), .in_t(pr_t), .in_m(cp_m_q), .in_w(cur_w), .in_z(cur[7]), .in_mask(pix_mask),
        .tm_a0(tm_ra0), .tm_a1(tm_ra1), .tm_a2(tm_ra2),
        .tm_q0(tm_q0), .tm_q1(tm_q1), .tm_q2(tm_q2), .tm_h0(tm_h0), .tm_h1(tm_h1), .tm_h2(tm_h2),
        .tl_a0(tl_a0), .tl_a1(tl_a1), .tl_a2(tl_a2), .tl_q0(tl_q0), .tl_q1(tl_q1), .tl_q2(tl_q2),
        .lb_raddr(pix_lb_ra), .clb_raddr(pix_clb_ra), .zlb_rdata(zlb_rd), .clb_rdata(clb_rd),
        .hl_raddr(pix_hl_ra), .hl_rdata(pix_hl_rd), .rwb0(rwb[0]), .rwb1(rwb[1]), .hz_raddr(pix_hz_ra), .hz_rdata(pix_hz_rd), .rzb0(rzb[0]), .rzb1(rzb[1]), .out_hid(px_hid),
        .out_valid(px_valid), .out_set(px_set), .out_x(px_x), .out_color(px_color), .out_r8(px_r8), .out_c27(px_c27), .out_depth(px_depth),
        .out_cpass(px_cpass), .out_zpass(px_zpass), .busy(pix_busy));

    // ======================================================================
    // the memory engine
    // ======================================================================
    // a read request (a row's spans, or a load) and a write request (a row's
    // results, or a fill): each waits here for its engine
    reg         rq_pend, rq_load, rq_z, rq_c, rq_h, rq_hz, rq_set, rq_half;
    reg  [31:0] rq_zbase, rq_cbase;
    // (a span's read takes its columns from xs, xe: they stay as they are while the read waits, since the row's pixels
    // wait for it)
    reg  [9:0]  xs, xe_m1;
    reg  [10:0] xe;
    reg  [9:0]  rq_x0;                  // the request's columns, in the row's words (the read engine takes them when it takes the request, which can be
    reg  [10:0] rq_x1;                  // after the row walker has gone on)
    reg  [10:0] rq_ldst, rq_ln;
    reg  [1:0]  rq_lmode;
    reg  [11:0] rq_ldxt;
    reg         rq_lodd, l_odd;
    reg         wq_pend, wq_fill, wq_z, wq_set;
    reg  [31:0] wq_cbase, wq_zbase;
    reg  [9:0]  wq_x0;
    reg  [10:0] wq_x1;
    wire        rd_ready, wr_ready, rd_busy, wr_busy, mem_wpend, h_done0, h_done1, wr_seti;
    // a span's hidden words are read into its set only when the write engine
    // is not merging from that set -- and, when rows can share a group (the
    // image's rows not 16 pixels wide), only when nothing of the row before is
    // still to be written -- as for the second half of a 32-bit image's wide span, whose first group may be the first half's last
    wire        hz_ok = !(rq_h || rq_hz) || (!(wr_busy && wr_seti == rq_set)
                                  && ((cimg_width[3:0] == 4'd0 && !rq_half) || (!wr_busy && !wq_pend)));
    wire [9:0]  z_have0, z_have1, c_have0, c_have1;
    wire [9:0]  z_have = set_cur ? z_have1 : z_have0, c_have = set_cur ? c_have1 : c_have0;      // (the row being drawn's)
    wire        h_done = set_cur ? h_done1 : h_done0;
    wire        rq_cur = rq_pend && rq_set == set_cur;          // the row being drawn's own request is still to be taken
    // a row's results in the new buffers so far, by set -- as the write engine
    // may count on them: two cycles behind (the buffers' write, then its read)
    reg  [10:0] res_n [0:1];
    reg  [10:0] res_d [0:1];
    reg  [10:0] res_q [0:1];
    // a set's old buffers are not written while the last pixels of the row
    // that had them are still ahead of the pipe's read of them
    reg  [4:0]  old_hold [0:1];
    rdp_mem mem (
        .clk(clk), .rst(rst),
        .rd_valid(rq_pend && old_hold[rq_set] == 5'd0 && hz_ok), .rd_ready(rd_ready), .rd_load(rq_load), .rd_z(rq_z), .rd_c(rq_c), .rd_h(rq_h), .rd_hz(rq_hz),
        .rd_zbase(rq_zbase), .rd_cbase(rq_cbase), .rd_x0(rq_x0), .rd_x1(rq_x1), .rd_set(rq_set), .rd_c32(cimg32),
        .rd_ldst(rq_ldst), .rd_ln(rq_ln), .rd_lmode(rq_lmode), .rd_ldxt(rq_ldxt), .rd_lodd(rq_lodd),
        .z_have0(z_have0), .z_have1(z_have1), .c_have0(c_have0), .c_have1(c_have1), .h_done0(h_done0), .h_done1(h_done1), .hlbw_we(hl_we), .hlbw_z(hl_z), .hlbw_addr(hl_wa),
        .lbw_z_we(mw_z_we), .lbw_c_we(mw_c_we), .lbw_addr(mw_addr), .lbw_data(mw_data),
        .wr_valid(wq_pend), .wr_ready(wr_ready), .wr_fill(wq_fill), .wr_z(wq_z), .wr_c8(cimg8), .wr_c32(cimg32), .wr_fmode(cyc == 2'd3),
        .wr_cbase(wq_cbase), .wr_zbase(wq_zbase), .wr_x0(wq_x0), .wr_x1(wq_x1), .wr_fillc(fill), .wr_set(wq_set),
        .res_a(res_q[0]), .res_b(res_q[1]),
        .lbr_addr(mem_lb_ra), .lbr_zdata(znew_rd), .lbr_cdata(cnew_rd), .lbr_zmask(zm_rd), .lbr_cmask(cm_rd),
        .hlbr_addr(mem_hl_ra), .hlbr_data(mem_hl_rd),
        .tm_we(tm_we), .tm_wa(tm_wa), .tm_wd0(tm_wd0), .tm_wd1(tm_wd1), .tm_wd2(tm_wd2), .tm_wd3(tm_wd3),
        .m_cmd_valid(m_cmd_valid), .m_cmd_ready(m_cmd_ready), .m_cmd_we(m_cmd_we), .m_cmd_addr(m_cmd_addr),
        .m_wdata_valid(m_wdata_valid), .m_wdata_ready(m_wdata_ready), .m_wdata(m_wdata), .m_wdata_we(m_wdata_we),
        .m_rdata_valid(m_rdata_valid), .m_rdata(m_rdata),
        .rd_busy(rd_busy), .wr_busy(wr_busy), .wr_seti(wr_seti), .writes_pending(mem_wpend));

    // ======================================================================
    // shared multiplier: 32 x 32 signed, two cycles
    // ======================================================================
    reg  [31:0] mul_a, mul_b;
    reg  signed [63:0] mul_p;
    always @(posedge clk) mul_p <= $signed(mul_a) * $signed(mul_b);

    // ======================================================================
    // the sequencer
    // ======================================================================
    reg        row_wq;              // the row's write-back has been asked for
    // The row walker (wk): the next row's span is walked (a triangle's subscanlines) and its reads asked for while the
    // pixels of the row before issue; its place waits (W_RDY: in aa_min, aa_max and the subscanlines' registers aa_* for a
    // triangle, in xa, xb for a rectangle; y and row_h as they are) for the pixels to be done, and S_GO takes it. The walker goes on to the row after that when the row taken has
    // set itself up (W_HOLD), and its write-back has been asked for, since the row's base address moves on then.
    localparam W_OFF = 4'd0, W_ROW = 4'd1, W_SUB = 4'd2, W_SPAN = 4'd3, W_RDY = 4'd4, W_HOLD = 4'd5, W_ADV = 4'd6, W_END = 4'd7, W_SKIP = 4'd8;
    reg [3:0]  wk;
    wire       hold_ok = cimg32 ? st == S_GO : (st == S_PIX && row_wq);
    // the row's next pixel: its word in the span, and whether what it reads of
    // memory is in the old buffers
    wire [9:0] pix_wi = ({9'd0, xsp[0]} + px_cnt) >> 1;
    // (a 32-bit image's colour span: a word a pixel)
    wire       pix_ok = !rq_cur && h_done && (!z_cmp || z_have > pix_wi) && (!need_mem || c_have > (cimg32 ? px_cnt : pix_wi));
    // a pixel that goes through the texture stages twice (rdp_pix two_pass), or takes the LOD, is issued every second
    // clock: pix_valid is still up the clock after an issue
    wire       pix_slot = !((two_pass || lod_on) && pix_valid);
    // The LOD: a pixel's next pixel goes through the perspective divide two clocks after it when the walk goes to the
    // right (pix_fol: rdp_pix takes its step from the next pass A), so the pixel issued then is the next one, or -- its
    // memory not in, or the span ended -- a ghost of it (through the perspective divide, never drawn; the pixel itself
    // follows when it can). Walking left, the span's first pixel is preceded by a ghost of the pixel before it (pix_g0).
    reg        pix_fol, pix_g0, pix_stp;    // (pix_stp: cur steps to the next place at the next issue)
    wire [10:0] pix_pos = {1'b0, xs} + {1'b0, px_cnt};
    wire       pix_end = pix_pos == xe;
    wire       pix_gh = lod_on && pix_slot && (pix_g0 || (pix_fol && (pix_end || !pix_ok)));
    wire       pix_go = pix_ok && pix_slot && !pix_gh && !(lod_on && pix_end);
    // nothing of the primitive is under way any more
    wire       all_quiet = !pix_busy && !pix_valid && !px_valid && !msk_we
                         && !rq_pend && !rd_busy && !wq_pend && !wr_busy;
    // the command waiting is a triangle that draws: it may start while the primitive before drains
    wire       tri_early = n_tri && cimg_ok && !(cimg32 && cyc == 2'd2);
    reg [4:0]  mi;                  // multiplier sequence index
    reg  [2:0] cp_m_q;           // the place in its group of the pixel issued (copy mode)
    reg        kind_tri, kind_texrect, kind_fillmode, kind_rect;   // what is being drawn
    reg signed [11:0] y, y0r, ymr, yb;
    reg signed [11:0] ry0;         // rect / texrect top row
    reg [9:0]  rx0, xa, xb;        // rect columns [xa, xb), texrect origin rx0
    reg [1:0]  rxf;                // XH's quarter pixels (outside copy mode: the column-stepped attribute's origin)
    // S_VSET: attribute vi (R G B A, S T W, Z) out of the staging RAM. The
    // N64 sends a group's four (shade, texture) as 16 words -- value, DaDx,
    // DaDe, DaDy, each as two words of four 16-bit integer halves and, four
    // words on, their fractions; Z as four whole words.
    reg [2:0]  vi;
    reg [3:0]  vs;
    reg [31:0] vgx, vde, vval;
    // Z's DzDy too (the walker has no use for it): with DzDx, the console's dz code of the primitive (dzc_p), which the
    // pixel's depth compare and the depth word keep (rdp_pix)
    reg [31:0] zdy;
    reg [2:0]  tflags;              // the triangle's shade / texture / z
    // A triangle's setup runs while the primitive before drains (S_FIN): what the pixel pipeline reads of it -- the flags,
    // the tile, the LOD's walk and level count, the depth's dz code -- waits here (n_*, dzc_n) until its first span (cmt:
    // to be committed), which waits for the drain
    reg        n_shade, n_tex, n_zbuf, n_lodlft, n_aaen, cmt;
    reg        tl_tri;              // S_TLOAD reads a triangle's six words (else a texture rectangle's two)
    reg [2:0]  n_tile, n_lodlvl;
    reg [3:0]  dzc_n;
    wire [5:0]  vgb = (vi < 3'd4) ? 6'd8 : (vi < 3'd7) ? (tflags[2] ? 6'd24 : 6'd8)
                    : (tflags[2] ? 6'd24 : 6'd8) + (tflags[1] ? 6'd16 : 6'd0);
    wire        vz = vi == 3'd7;
    wire [15:0] vhalf = vi[0] ? vr_q[15:0] : vr_q[31:16];      // (R, B, S, W: the first half)
    // The value, DaDx and DaDe come from the words in halves (Z: whole words); each register is loaded, a half at a time, from
    // the one choice of the half an attribute takes, or a Z's, at the step it takes them -- a source a register, not two.
    wire [15:0] vsrc_hi = vz ? vr_q[31:16] : vhalf, vsrc_lo = vz ? vr_q[15:0] : vhalf;
    wire        vs_on = !rst && st == S_VSET;
    always @(posedge clk) begin
        if (vs_on && vs == 4'd2) vval[31:16] <= vsrc_hi;
        if (vs_on && ((vs == 4'd2 && vz) || (vs == 4'd3 && !vz))) vval[15:0] <= vsrc_lo;
        if (vs_on && ((vs == 4'd3 && vz) || (vs == 4'd4 && !vz))) vgx[31:16] <= vsrc_hi;
        if (vs_on && ((vs == 4'd3 && vz) || (vs == 4'd5 && !vz))) vgx[15:0] <= vsrc_lo;
        if (vs_on && ((vs == 4'd4 && vz) || (vs == 4'd6 && !vz))) vde[31:16] <= vsrc_hi;
        if (vs_on && ((vs == 4'd4 && vz) || (vs == 4'd7 && !vz))) vde[15:0] <= vsrc_lo;
    end
    function automatic [5:0] voff;           // the word read at step k
        input [2:0] k;
        input       z;
        case (k)
            3'd0: voff = 6'd0;
            3'd1: voff = z ? 6'd1 : 6'd4;
            3'd2: voff = 6'd2;
            3'd3: voff = 6'd6;
            3'd4: voff = 6'd8;
            default: voff = 6'd12;
        endcase
    endfunction
    function automatic attr_on;             // rdp_attr_on()
        input [2:0] fl;
        input [2:0] a;
        attr_on = a < 3'd4 ? fl[2] : a < 3'd7 ? fl[1] : fl[0];
    endfunction
    // The row's offset into the images, y * width pixels (row_h), and in bytes (row_off): twice that for the 16- and
    // 32-bit images, as they are (an 8-bit one's is the pixels'). The first row's is the product of the span setup, as 0 +
    // it through the step's adder (the next rows' step: + the width), so that the register has the one source.
    reg [31:0] row_h;
    wire [31:0] row_off = cimg8 ? row_h : {row_h[30:0], 1'b0};
    // the row's colour base: a 32-bit image's row is twice the bytes of the depth image's
    wire [31:0] crow = cimg + (cimg32 ? {row_off[30:0], 1'b0} : row_off);
    wire [31:0] zrow = zimg + row_off;            // the depth image's row
    // (the hidden words' row bases, rwb and rzb, are those addresses' bits 11:2; a 32-bit image has no hidden words, so the
    // colour image's, where crow differs from cimg + row_off, is not read)
    // A 16-bit image whose width is odd has rows that start on the odd halfword of a memory word: the row's pixels sit in
    // its words from halfword `par` on, so a span's columns are counted in words (xsp, xep: the buffers', the reads' and the
    // write-back's) from there; the pixel pipeline keeps the column itself for what depends on it (the dither). The depth
    // image is taken to have the same alignment (an image's base is a multiple of 8 bytes: its rows' halfwords are the
    // colour image's for the same width).
    assign par = !cimg8 && !cimg32 && crow[1];
    assign xsp = xs + {9'd0, par_c};
    assign xep = xe + {10'd0, par_c};
    reg [9:0]  px_cnt;
    // loads (lang/c/rdp/rdp.c load_block, load_tlut, load_tile_n64)
    reg [9:0]  tt, tt1;             // LOAD TILE's rows
    reg [22:0] l_ti;                // the row's first texel in the image (t * width + s)
    reg [10:0] l_dst;               // its 16-bit word of TMEM
    reg [10:0] l_n;                 // 32-bit words a row (TLUT: entries)
    reg [1:0]  l_kind;              // 0 LOAD BLOCK, 1 LOAD TILE, 2 LOAD TLUT
    reg        l_none;              // nothing to load (lrs < uls, lrt < ult)
    // the 64-bit words n texels of the image take, as 32-bit ones (load_words)
    function automatic [10:0] load_words;
        input [12:0] n;
        input [1:0]  sz;
        reg [16:0] m;
        reg [12:0] w;
        begin
            // (the 64-bit words of n texels: ceil(ceil(n << sz / 2) / 8) = ceil((n << sz) / 16))
            m = ({4'd0, n} << sz) + 17'd15;
            w = m[16:4];
            load_words = (w >= 13'd512) ? 11'd1024 : {w[9:0], 1'b0};
        end
    endfunction

    // sign helpers
    wire signed [13:0] yh_s = t_yh, ym_s = t_ym, yl_s = t_yl;
    // the scissor's field mode: the row is not of the field kept
    wire sc_skip = sc_field && (y[0] != sc_odd);
    // (the triangle about to start: from its first word)
    wire signed [13:0] yl_end_i = ($signed(nw0[13:0]) + 14'sd3) >>> 2;
    wire signed [11:0] yb_aa = (yl_end_i < $signed({2'b0, sc_y1})) ? yl_end_i[11:0] : $signed({1'b0, sc_y1});

    // ---- AA_EN: the N64's walker (lang/c/rdp/rdp.c draw_triangle_aa). The
    // edges at subscanline aa_j: three running edges, set up at the first
    // row's top (S_AAINIT) and stepped (DxDy >> 2) & ~1 a subscanline. A
    // subscanline a clock (the row walker's W_SUB): its edges in eighths with the sticky
    // bit, clamped to the scissor (stage 1), then shifted into the row's four
    // (stage 2) with the span's union as a running min / max.
    reg  [31:0] ax_h, ax_m, ax_l;
    reg  [2:0]  aa_j;
    wire [31:0] aq_h = asr(t_dxh, 5'd2) & ~32'd1, aq_m = asr(t_dxm, 5'd2) & ~32'd1, aq_l = asr(t_dxl, 5'd2) & ~32'd1;
    // S_AAINIT: an edge's start, X & ~1 (XL as S_XL left it) plus the product (one adder for the three)
    // (and S_XL's XL - product, TRI_INIT's XH + product: one adder for the five)
    wire        aa_sub = st == S_XL;
    wire [31:0] aa_x = (st == S_TRI_INIT) ? t_xh : aa_sub ? (t_xl & ~32'd1)
                     : (mi == 5'd2) ? (t_xh & ~32'd1) : (mi == 5'd3) ? (t_xm & ~32'd1) : t_xl;
    wire [31:0] aa_init = aa_x + (mul_p[31:0] ^ {32{aa_sub}}) + {31'd0, aa_sub};
    wire signed [13:0] aa_yq = {y, aa_j[1:0]};
    // (the minor edge -- M above YM, L from it -- chosen a subscanline ahead, aa_lo)
    reg         aa_lo;
    wire [31:0] aa_xa = ax_h, aa_xb = aa_lo ? ax_m : ax_l;
    // (each edge in eighths with its sticky bit, then left / right)
    wire signed [18:0] aa_a8 = {aa_xa[31:14], |aa_xa[13:1]}, aa_b8 = {aa_xb[31:14], |aa_xb[13:1]};
    wire signed [18:0] aa_l8 = t_lft ? aa_a8 : aa_b8, aa_r8 = t_lft ? aa_b8 : aa_a8;
    wire        aa_cross = $signed(aa_r8[18:1]) < $signed(aa_l8[18:1]);
    wire [12:0] aa_cxh = {sc_xh, 1'b0}, aa_cxl = {sc_xl, 1'b0};
    function automatic [12:0] aa_clamp;       // max(v, lo), then min(., hi)
        input signed [18:0] v;
        input [12:0] lo, hi;
        reg signed [18:0] t;
        begin
            t = (v < $signed({6'd0, lo})) ? $signed({6'd0, lo}) : v;
            aa_clamp = (t > $signed({6'd0, hi})) ? hi : t[12:0];
        end
    endfunction
    wire        aa_vj = !aa_cross && aa_yq >= yh_s && aa_yq < yl_s
                     && aa_yq >= $signed({2'b00, sc_yh}) && aa_yq < $signed({2'b00, sc_yl});
    reg         aa_s1;                  // stage 1 holds a subscanline
    reg  [12:0] aa_l1, aa_r1;
    reg         aa_v1;
    reg  [39:0] aa_lp, aa_rp;           // 4 x pixel (subscanline 0 lowest)
    reg  [15:0] aa_lm, aa_rm;           // 4 x its samples left of / right of the edge's eighth
    reg  [3:0]  aa_v;                   // subscanline valid, its edges not crossed
    // (the row being drawn has copies of the subscanlines' registers: the walker goes on to the next row meanwhile)
    reg  [39:0] pc_lp, pc_rp;
    reg  [15:0] pc_lm, pc_rm;
    reg  [3:0]  pc_v;
    reg  [3:0]  aa_pl, aa_pr;           // the span's pixels so far have passed its left / right end
    reg  [9:0]  aa_min, aa_max;
    reg         aa_any;
    wire [3:0]  aa_fm = aa_j[0] ? 4'hA : 4'h5;   // (stage 2: the previous subscanline's)
    wire [3:0]  aa_lmj = (4'hF >> (({1'b0, aa_l1[2:0]} + 4'd1) >> 1)) & aa_fm;
    wire [7:0]  aa_rm8 = 8'hF0 >> (({1'b0, aa_r1[2:0]} + 4'd1) >> 1);
    wire [3:0]  aa_rmj = aa_rm8[3:0] & aa_fm;
    // a pixel's samples: two a subscanline, a checkerboard. The span starts
    // at the leftmost valid left end and a valid subscanline's left end is
    // never after its right one (its edges are not crossed; the sticky bit
    // stays in the pixel): so each pixel need only be compared for equality
    // with the ends, and two flags say whether the span has passed them.
    wire [9:0]  aa_px = xs + px_cnt;
    reg  [3:0]  aa_cj [0:3];
    reg  [3:0]  aa_eql, aa_eqr;
    integer jj;
    always @(*) begin
        for (jj = 0; jj < 4; jj = jj + 1) begin
            aa_eql[jj] = aa_px == pc_lp[10*jj +: 10];
            aa_eqr[jj] = aa_px == pc_rp[10*jj +: 10];
            aa_cj[jj] = 4'd0;
            if (pc_v[jj]) begin
                if (aa_eql[jj]) aa_cj[jj] = aa_eqr[jj] ? (pc_lm[4*jj +: 4] & pc_rm[4*jj +: 4]) : pc_lm[4*jj +: 4];
                else if (aa_eqr[jj]) aa_cj[jj] = pc_rm[4*jj +: 4];
                else if (aa_pl[jj] && !aa_pr[jj]) aa_cj[jj] = jj[0] ? 4'h5 : 4'hA;
            end
        end
    end
    wire [7:0]  aa_mask = {aa_cj[0] | aa_cj[1], aa_cj[2] | aa_cj[3]};
    // the span: the subscanlines' union, its end to the scissor
    wire [10:0] aa_xe = ({1'b0, aa_max} + 11'd1 < {1'b0, sc_x1}) ? {1'b0, aa_max} + 11'd1 : {1'b0, sc_x1};
    // A 32-bit image's span longer than a set holds (512 pixels) is drawn in two halves (spl): xe is the first half's
    // end, and the second half, from there to the span's end (xe2: a rectangle's xb, a triangle's aa_xe, both as they
    // were when the row began), is a row of its own on the other set (S_SPL) that goes on with the walk. (The cut: a
    // rectangle's columns; a triangle's min(aa_max + 1, scissor) - aa_min, as two compares side by side.)
    reg        spl;
    wire       r_cut = cimg32 && ({1'b0, xb} - {1'b0, xa}) > 11'd512;
    wire [10:0] aa_m512 = {1'b0, aa_min} + 11'd512;
    wire       a_cut = cimg32 && {1'b0, aa_max} >= aa_m512 && {1'b0, sc_x1} > aa_m512;
    wire [10:0] xe2 = kind_tri ? aa_xe : {1'b0, xb};

    // arithmetic shift right, evaluated on its own: inside a wider expression
    // with any unsigned operand, $signed(v) >>> n is a LOGICAL shift
    function automatic [31:0] asr;
        input [31:0] v;
        input [4:0]  n;
        asr = $signed(v) >>> n;
    endfunction

    // a - b of two 12-bit rows, sign-extended: 13 bits (a row above -2048
    // + 1023 is 3071 rows away; 12 bits wrapped it, seed 3140)
    function automatic [31:0] rdiff;
        input [11:0] a, b;
        reg [12:0] d;
        begin
            d = {a[11], a} - {b[11], b};
            rdiff = {{19{d[12]}}, d};
        end
    endfunction

    // ---- e[] / ade[] RAM ports
    reg  [3:0]  es;                 // row step: attribute es, 8 when idle
    wire        rw_ld  = (st == S_TRI_INIT && mi == 5'd13) || (st == S_RECT_INIT && mi == 5'd2);
    wire        rw_stp = st == S_NEXT || wk == W_ADV || (wk == W_SKIP && (!kind_tri || es[3]));
    always @(posedge clk) if (rw_ld || rw_stp) row_h <= (rw_ld ? 32'd0 : row_h) + (rw_ld ? mul_p[31:0] : {21'd0, cimg_width});
    reg  [2:0]  ea;                 // read address
    always @(*) begin
        case (st)
            S_TRI_INIT: ea = (mi == 5'd14) ? 3'd7 : mi[2:0] - 3'd3;
            S_SETUP:    ea = mi[2:0] - 3'd2;
            default:    ea = es[2:0];
        endcase
    end
    wire vset_w = st == S_VSET && (vs == 4'd15 || (vs == 4'd0 && !attr_on(tflags, vi)));
    wire vset_on = vs == 4'd15;
    wire vgx_w = st == S_VSET && vs == (vz ? 4'd5 : 4'd7);
    wire tinit_w = st == S_TRI_INIT && mi >= 5'd5 && mi <= 5'd12;
    wire step_w = !es[3];
    wire [2:0]  e_wa = vset_w ? vi : tinit_w ? mi[2:0] - 3'd5 : es[2:0];
    // (TRI_INIT's write of e + k DaDe reads e where it writes it, as the product comes -- two clocks after DaDe was
    // read at ea for it; one adder for that and for the row step's e + DaDe)
    // (the span setup reads DaDx at mi, its rows; the rest DaDe at ea)
    wire [3:0]  ada = (st == S_SETUP) ? {1'b1, mi[2:0]} : {1'b0, ea};
    wire [31:0] e_q = e_ram[tinit_w ? e_wa : ea], ade_q = ade_ram[ada];
    wire [31:0] e_add = e_q + (tinit_w ? mul_p[31:0] : ade_q);
    wire [31:0] e_wd = vset_w ? (vset_on ? vval : 32'd0) : e_add;
    // The span's start value, as the console takes it (docs/rdp_exact.md, rule 4): ((e & ~0x1FF) + dif - xfrac * ((DaDx >> 8) & ~1)) &
    // ~0x3FF + (the pixel's distance from the major edge's pixel) * DaDx (low 5 bits cleared, Z whole). dif: 0, or when the edge
    // leans away from the walk (the attributes are taken on the bottom subscanline) 3/4 of DaDe - DaDy, each with its low 9 bits
    // cleared, one value an attribute for the whole triangle (dif_ram, written as the attribute's DaDy comes in).
    reg  [11:0] unscrx;             // the major edge's pixel and 8-bit fraction on that subscanline, from the walker
    reg  [7:0]  xfrac;
    wire        aa_ld3 = !(t_dxh[31] ^ t_lft);
    always @(posedge clk) if (wk == W_SUB && !aa_j[2] && aa_j[1:0] == (aa_ld3 ? 2'd3 : 2'd0)) begin
        unscrx <= ax_h[27:16]; xfrac <= ax_h[15:8];
    end
    (* ramstyle = "MLAB, no_rw_check" *) reg [31:0] dif_ram [0:7];
    reg  [15:0] vgy_hi;
    wire        dz_s = vs == 4'd15 && vz;            // (Z's own step: its DaDy is zdy; at step 0 of vi = 7 it is W's that arrives)
    wire [31:0] dy_c = dz_s ? zdy : {vgy_hi, vi[0] ? vr_q[31:16] : vr_q[15:0]};
    // (de - de >> 2 - dy + dy >> 2 with 32-bit wrap, as the console's: two terms that each fit, not (de - dy) >> 2)
    wire [31:0] dq   = (vde & 32'hFFFFFE00) - (dy_c & 32'hFFFFFE00);
    wire [31:0] dq2  = asr(vde & 32'hFFFFFE00, 5'd2) - asr(dy_c & 32'hFFFFFE00, 5'd2);
    wire [31:0] dif_d = dq - dq2;
    wire [2:0]  dif_a = dz_s ? 3'd7 : vi - 3'd1;
    wire        dif_w = st == S_VSET && (dz_s || (vs == 4'd0 && vi != 3'd0));
    always @(posedge clk) begin
        if (st == S_VSET && vs == 4'd15 && !vz) vgy_hi <= vhalf;
        // (one write port: Z's, when it is off, is zeroed at the triangle's first step after S_VSET, which writes none of it)
        if (dif_w || (st == S_TRI_INIT && mi == 5'd0 && !tflags[0]))
            dif_ram[dif_w ? dif_a : 3'd7] <= (dif_w && aa_ld3 && attr_on(tflags, dif_a)) ? dif_d : 32'd0;
    end
    wire [31:0] dif_q = dif_ram[ea];
    reg  signed [23:0] pf_a;
    reg  signed [8:0]  pf_b;
    (* multstyle = "dsp" *) reg signed [32:0] pf_p;
    reg  [31:0] s1r, p_d;
    always @(posedge clk) begin
        pf_a <= (cyc == 2'd2 || !attr_on(tflags, mi[2:0])) ? 24'sd0 : $signed({ade_q[31:9], 1'b0});
        pf_b <= {1'b0, xfrac};
        pf_p <= pf_a * pf_b;
        s1r <= ((e_q & 32'hFFFFFE00) + dif_q - pf_p[31:0]) & 32'hFFFFFC00;
        p_d <= mul_p[31:0];
    end
    always @(posedge clk) begin
        if (vset_w || tinit_w || step_w) e_ram[e_wa] <= e_wd;
        // DaDx goes in at a step of S_VSET after vgx is complete and before
        // the step that writes DaDe (Z: 5, the others: 7); an attribute that
        // is off is not written, and its product is 0 (S_SETUP)
        if (vset_w) ade_ram[{1'b0, vi}] <= vset_on ? vde : 32'd0;
        else if (vgx_w) ade_ram[{1'b1, vi}] <= vz ? vgx : {vgx[31:5], 5'd0};     // (the pixels step by DaDx without its low 5 bits, Z whole)
    end

    // ---- the shared multiplier's operands, by state and step. A product is
    // read exactly two clocks after its operands were chosen and never later,
    // so the operands need no hold: at the steps whose product nobody reads
    // they are don't-care, and the select is left with the sources alone. The
    // row differences y - YH, y - YM and y - 0 (the image row) are one
    // subtractor; AA_EN's start slopes are TRI_INIT's with bits 2..0 cleared;
    // a texrect's DsDx / DtDy and its row / column distances are one choice each
    // the span's start from the H edge, 16.16 (the LOD walking left: from its ghost, a pixel before)
    wire [10:0] xs_g = {1'b0, xs} - {10'd0, lod_on && !lod_lft};
    // (the pixels' distance from the major edge's pixel on the subscanline the attributes are taken on: 13 bits)
    wire signed [12:0] dist13 = $signed({{2{xs_g[10]}}, xs_g}) - $signed({unscrx[11], unscrx});
    wire [31:0] dxs = {{19{dist13[12]}}, dist13};
    wire [31:0] r_dsdx = {{16{t_dxl[31]}}, t_dxl[31:16]}, r_dtdy = {{16{t_dxl[15]}}, t_dxl[15:0]};
    wire signed [10:0] r_dc11 = $signed({1'b0, xs}) - $signed({1'b0, rx0});      // (11 and 13 bits, then extended)
    // in quarter pixels, the products then over 4: the rows from YH's whole row, the columns from XH itself
    // (4 (xs - XH's column) - XH's fraction; none in copy mode): the column-stepped attribute is the plane at XH
    wire [31:0] r_dcol = (cyc == 2'd2) ? 32'd0 : {{19{r_dc11[10]}}, r_dc11, 2'b00} - {30'd0, rxf};     // (copy: the group count starts at the first pixel drawn)
    wire [31:0] r_drow = {rdiff(y, ry0), 2'b00};
    wire [11:0] md_b = (mi == 5'd2) ? ymr : ((st == S_TRI_INIT && mi == 5'd11) || st == S_RECT_INIT) ? 12'd0 : y0r;
    wire [31:0] md_k = rdiff(y, md_b);
    wire [31:0] md_dx = ((mi == 5'd0) ? t_dxh : (mi == 5'd1) ? t_dxm : t_dxl) & {29'h1FFFFFFF, {3{st != S_AAINIT}}};
    reg  [31:0] mul_ad, mul_bd;
    always @* begin
        mul_ad = 32'bx; mul_bd = 32'bx;
        case (st)
        // XL - (YM's quarter rows) x the subscanline step (that in mul_a, as the other wide operands)
        S_XL:        begin mul_ad = aq_l; mul_bd = {30'd0, t_ym[1:0]}; end
        // the edges (0..2), e + k DaDe (3..10, ea = mi - 3), the first row's offset (11)
        S_TRI_INIT:  begin mul_ad = md_k; mul_bd = (mi < 5'd3) ? md_dx : (mi == 5'd11) ? {21'd0, cimg_width} : ade_q; end
        S_AAINIT:    begin mul_ad = md_k; mul_bd = md_dx; end
        // DaDx (its rows of ade_ram) x the span's offset; an attribute that is off: 0 (the offset forced to it)
        S_SETUP: if (kind_tri) begin mul_ad = attr_on(tflags, mi[2:0]) ? dxs : 32'd0; mul_bd = ade_q; end
                 else begin mul_ad = mi[0] ? r_dtdy : r_dsdx; mul_bd = (mi[0] ^ (op == 6'h25)) ? r_drow : r_dcol; end
        S_RECT_INIT: begin mul_ad = md_k; mul_bd = {21'd0, cimg_width}; end
        // LOAD BLOCK's whole texels, the others' 10.2
        S_LOAD_INIT: begin mul_ad = (l_kind == 2'd0) ? {20'd0, w0[11:0]} : {22'd0, w0[11:2]}; mul_bd = {21'd0, timg_width}; end
        default: ;
        endcase
    end
    always @(posedge clk) begin mul_a <= mul_ad; mul_b <= mul_bd; end

    // AA_EN: the subscanline stages
    always @(posedge clk) begin
        aa_s1 <= wk == W_SUB && !aa_j[2];
        aa_l1 <= aa_clamp(aa_l8, aa_cxh, aa_cxl);
        aa_r1 <= aa_clamp(aa_r8, aa_cxh, aa_cxl);
        aa_v1 <= aa_vj;
        if (aa_s1) begin
            aa_lp <= {aa_l1[12:3], aa_lp[39:10]};
            aa_rp <= {aa_r1[12:3], aa_rp[39:10]};
            aa_lm <= {aa_lmj, aa_lm[15:4]};
            aa_rm <= {aa_rmj, aa_rm[15:4]};
            aa_v <= {aa_v1, aa_v[3:1]};
            if (aa_v1) begin
                if (!aa_any || aa_l1[12:3] < aa_min) aa_min <= aa_l1[12:3];
                if (!aa_any || aa_r1[12:3] > aa_max) aa_max <= aa_r1[12:3];
                aa_any <= 1'b1;
            end
        end else if (wk == W_ROW) aa_any <= 1'b0;
        // the span's progress past the ends (S_PIX: a pixel issued)
        if (st == S_GO && wk == W_RDY) begin
            aa_pl <= 4'd0; aa_pr <= 4'd0;
            pc_lp <= aa_lp; pc_rp <= aa_rp; pc_lm <= aa_lm; pc_rm <= aa_rm; pc_v <= aa_v;
        end else if (st == S_PIX && row_wq && pix_go) begin aa_pl <= aa_pl | aa_eql; aa_pr <= aa_pr | aa_eqr; end
    end

    // cur: the span setup's first values (a triangle's e + the product, one attribute a clock; a texrect's S and T) and the
    // pixels' step. A triangle's load goes through the step's adder, as 0 + the value in place of cur + adx: each attribute
    // has one adder and nothing behind it (the fitter put a second source of a register's behind its own adder in a
    // route-through LUT a bit: 170 of them, and a mux).
    // copy mode (a texture rectangle's): S or T steps once a group of 4 pixels (8 in an 8-bit image) -- the issue of a group's first
    // pixel -- and the pixel's place in its group goes down the pipe to the texel (docs/rdp_exact.md, rule 6)
    reg  [2:0]  cp_m;
    wire        cp_on = p_direct && cyc == 2'd2;
    wire        cp_end = cp_m == (cimg8 ? 3'd7 : 3'd3);
    wire        cur_stp = st == S_PIX && row_wq && (pix_gh || pix_go) && pix_stp && (!cp_on || cp_m == 3'd0);
    // (a texrect's S and T are t_xl's halves, shifted, plus the product: the same adder as a triangle's e + the product)
    wire [2:0]  rs_ds = (cyc == 2'd2) ? 3'd7 : 3'd5;
    wire signed [31:0] rs_sv = {{16{t_xl[31]}}, t_xl[31:16]}, rs_tv = {{16{t_xl[15]}}, t_xl[15:0]};
    wire [31:0] cur_ldv = (kind_tri ? s1r : mi[0] ? (rs_tv <<< 5) : (rs_sv <<< rs_ds)) + (kind_tri ? p_d : (cyc == 2'd2 && op == 6'h25 && !mi[0]) ? mul_p[31:0] : mul_p[33:2]);
    wire [7:0]  cur_ld;
    genvar cg;
    generate for (cg = 0; cg < 8; cg = cg + 1) begin : cur_ldg
        assign cur_ld[cg] = st == S_SETUP && (kind_tri ? mi == cg + 3 : (cg == 4 && mi == 5'd2) || (cg == 5 && mi == 5'd3));     // (e[ea] with ea = mi - 2)
    end endgenerate
    // The AA edges (ax_*: their start in S_AAINIT, a subscanline's step in the row walker's W_SUB) the same way: the start is 0 + it in place of x + the step.
    wire        ax_ldh = st == S_AAINIT && mi == 5'd2, ax_ldm = st == S_AAINIT && mi == 5'd3, ax_ldl = st == S_AAINIT && mi >= 5'd4;
    wire        ax_stp = wk == W_SUB && !aa_j[2];
    integer ci;
    // The shade and W (cur[0..3], cur[6]) step by a DaDx whose low five bits are 0 (adx, above: only Z's are whole), and the pixel
    // pipeline reads the shade from bit 14 up and W from bit 16: the low five bits of those cur[] cannot carry into the bits above
    // and nothing reads them. They are made 0 at the load (the choice of ci is a constant of the unrolled loop), and the registers,
    // the adders' bits and adx's are gone. (S and T, cur[4] and cur[5], are a texture rectangle's too, whose steps are whole; Z, cur[7].)
    function automatic cur_whole;
        input integer c;
        cur_whole = c == 4 || c == 5 || c == 7;
    endfunction
    always @(posedge clk) begin
        if (ax_ldh || ax_stp) ax_h <= (ax_ldh ? 32'd0 : ax_h) + (ax_ldh ? aa_init : aq_h);
        if (ax_ldm || ax_stp) ax_m <= (ax_ldm ? 32'd0 : ax_m) + (ax_ldm ? aa_init : aq_m);
        if (ax_ldl || ax_stp) ax_l <= (ax_ldl ? 32'd0 : ax_l) + (ax_ldl ? aa_init : aq_l);
        for (ci = 0; ci < 8; ci = ci + 1) begin
            if (cur_ld[ci] || cur_stp)
                cur[ci] <= (cur_ld[ci] ? 32'd0 : cur[ci]) + (cur_ld[ci] ? (cur_whole(ci) ? cur_ldv : (cur_ldv & ~32'h1F)) : adx[ci]);
        end
    end

    integer i, ai;
    always @(posedge clk) begin
        zn_we <= 1'b0; cn_we <= 1'b0; msk_we <= 1'b0;
        pix_valid <= 1'b0;
        sync_ev <= 1'b0;
        if (rst) begin
            st <= S_IDLE;
            es <= 4'd8;
            collecting <= 1'b0;
            exec_go <= 1'b0;
            busy_exec <= 1'b0;
            rq_pend <= 1'b0; wq_pend <= 1'b0; row_wq <= 1'b0; set_cur <= 1'b0; cmt <= 1'b0; wk <= W_OFF; par_c <= 1'b0;
            res_n[0] <= 11'd0; res_n[1] <= 11'd0; res_d[0] <= 11'd0; res_d[1] <= 11'd0; res_q[0] <= 11'd0; res_q[1] <= 11'd0;
            old_hold[0] <= 5'd0; old_hold[1] <= 5'd0;
            sync_count <= 0; load_count <= 0; unknown_ops <= 0;
            other_h <= 0; other_l <= 0; cc_hi <= 0; cc_lo <= 0;
            fill <= 0; fog <= 0; blend <= 0; prim <= 0; env <= 0; prim_lod_frac <= 0; k4 <= 0; k5 <= 0; cv_k03 <= 0; prim_z <= 0;
            sc_xh <= 0; sc_yh <= 0; sc_xl <= 12'd4092; sc_yl <= 12'd4092; sc_field <= 1'b0; sc_odd <= 1'b0;
            cimg <= 0; zimg <= 0; timg <= 0; cimg_width <= 11'd1; timg_width <= 11'd1;
            timg_size <= 2'd0; cimg_ok <= 1'b0; cimg8 <= 1'b0; cimg32 <= 1'b0;
        end else begin
            // ---- the engines take their requests
            if (rq_pend && rd_ready && old_hold[rq_set] == 5'd0 && hz_ok) begin
                rq_pend <= 1'b0;
                if (!rq_load) begin rwb[rq_set] <= rq_cbase[11:2]; rzb[rq_set] <= rq_zbase[11:2]; end     // (the hidden words' row bases, by set)
            end
            if (wq_pend && wr_ready) wq_pend <= 1'b0;
            if (old_hold[0] != 5'd0) old_hold[0] <= old_hold[0] - 5'd1;
            if (old_hold[1] != 5'd0) old_hold[1] <= old_hold[1] - 5'd1;

            // ---- pixel results into the new buffers
            res_d[0] <= res_n[0]; res_d[1] <= res_n[1];
            res_q[0] <= res_d[0]; res_q[1] <= res_d[1];
            if (px_valid) begin
                msk_we <= 1'b1;
                nb_wa <= {px_set, px_x[9:1]};
                msk_be <= px_x[0] ? 2'b10 : 2'b01;
                cm_wd <= {2{px_cpass}};
                zm_wd <= {2{px_zpass}};
                zn_wd <= {px_depth, px_depth};
                nb_be <= px_x[0] ? 2'b01 : 2'b10;       // the even pixel in [31:16]
                // (a 32-bit image: the pixel's entry whole, written whether it passed or not, its pass bit in bit 27)
                // (laid out so that the write engine's word, {[35:20], [17:2]}, is {R G B, cvg << 5}: the pass bit in [19])
                nb_x0 <= px_x[0];
                cn_wd <= cimg8 ? {2{px_r8, 10'd0}} : cimg32 ? {px_c27[26:11], px_cpass, 1'b0, px_c27[10:0], 7'd0}
                                                            : {px_color, px_hid, px_color, px_hid};
                cn_be <= cimg32 ? 2'b11 : px_x[0] ? 2'b01 : 2'b10;
                cn_we <= px_cpass || cimg32;
                zn_we <= px_zpass;
                res_n[px_set] <= res_n[px_set] + 11'd1;
            end

            // ---- command intake
            if (cmd_valid && cmd_ready) begin
                if (!collecting) begin
                    nw0 <= cmd_data;
                    nwords <= cmd_len(cmd_data);
                    widx <= 6'd1;
                    collecting <= 1'b1;
                end else begin
                    widx <= widx + 6'd1;
                    if (widx == 6'd1) nw1 <= cmd_data;
                    if (widx + 6'd1 == nwords) begin
                        collecting <= 1'b0;
                        exec_go <= 1'b1;
                    end
                end
            end

            // ---- the row step of e[] (one attribute a clock, see es_*)
            if (!es[3]) es <= es + 4'd1;

            // ---- execute
            case (st)
            S_IDLE: if (exec_go && (all_quiet || tri_early)) begin
                exec_go <= 1'b0;
                busy_exec <= 1'b1;
                w0 <= nw0; w1 <= nw1;
                if (n_tri) begin       // (a triangle's first two words; its edges come out of the staging RAM, S_TLOAD)
                    t_lft <= nw0[23]; t_tile <= nw0[18:16]; t_lvl <= nw0[21:19]; t_yl <= nw0[13:0];
                    t_ym <= nw1[29:16]; t_yh <= nw1[13:0];
                end
                if ((n_tri || nop == 6'h24 || nop == 6'h25 || nop == 6'h36) && (!cimg_ok || (cimg32 && cyc == 2'd2))) begin
                    busy_exec <= 1'b0;          // a 4-bit colour image, or copy mode into a 32-bit one: nothing drawn
                end else if (n_tri) begin
                    for (i = 0; i < 8; i = i + 1) adx[i] <= 0;     // (S_VSET writes those of the attributes it has)
                    kind_tri <= 1'b1; kind_texrect <= 1'b0; kind_rect <= 1'b0; kind_fillmode <= 1'b0;
                    tflags <= nop[2:0];
                    // (what the pixel pipeline reads of the primitive waits in n_*, for the primitive before to drain:
                    // committed where the first span's reads are asked for)
                    n_shade <= nop[2]; n_tex <= nop[1]; n_zbuf <= nop[0]; n_tile <= nw0[18:16];
                    n_lodlft <= nw0[23]; n_lodlvl <= nw0[21:19];
                    n_aaen <= other_l[3];       // (drawn where any sample is covered (AA_EN), or sample 7 is)
                    cmt <= 1'b1;
                    y0r <= $signed(nw1[13:0]) >>> 2;
                    ymr <= $signed(nw1[29:16]) >>> 2;
                    // first row: max(y0, scissor); rows end at min((yl + 3) >> 2, scissor)
                    y <= (($signed(nw1[13:0]) >>> 2) > $signed({2'b0, sc_y0})) ? ($signed(nw1[13:0]) >>> 2) : $signed({2'b0, sc_y0});
                    yb <= yb_aa;
                    mi <= 5'd0;
                    vi <= 3'd0; vs <= 4'd0;
                    st <= S_TLOAD; tl_tri <= 1'b1;
                end else begin
                    case (nop)
                    6'h00, 6'h26, 6'h27, 6'h28: begin busy_exec <= 1'b0; end
                    6'h29: st <= S_SYNC;
                    6'h24, 6'h25, 6'h36: begin
                        // rectangles: texrect (nw0: XL YL, nw1: tile XH YH, then S T, DsDx DtDy in t_xl, t_dxl)
                        kind_tri <= 1'b0;
                        p_aa <= 1'b0; p_aaen <= 1'b0;
                        kind_texrect <= nop != 6'h36;
                        kind_fillmode <= nop == 6'h36 && cyc == 2'd3;
                        kind_rect <= 1'b1;
                        p_shade <= 1'b0; p_tex <= nop != 6'h36; p_zbuf <= 1'b0; p_direct <= 1'b1;
                        p_tile <= nw1[26:24];
                        lod_lft <= 1'b1; lod_lvl <= 3'd0;       // (walked left to right; the far level is level 0)
                        rx0 <= nw1[23:14];
                        rxf <= (cyc != 2'd2) ? nw1[13:12] : 2'd0;
                        ry0 <= {2'b00, nw1[11:2]};
                        begin : rect_bounds
                            // copy mode (texrect), fill mode (fill rectangle): the corners' whole pixels, the lower
                            // right included. Otherwise the pixels whose top-left sample is inside, 4x in [XH, XL)
                            // and 4y in [YH, YL): each corner rounded up to a whole pixel
                            reg [10:0] x0c, y0c, x1c, y1c;
                            reg [10:0] xa_c, ya_c;
                            reg incl;
                            incl = (nop == 6'h36) ? (cyc == 2'd3) : (cyc == 2'd2);
                            x0c = incl ? {1'b0, nw1[23:14]} : ({1'b0, nw1[23:12]} + 13'd3) >> 2;
                            y0c = incl ? {1'b0, nw1[11:2]}  : ({1'b0, nw1[11:0]}  + 13'd3) >> 2;
                            x1c = incl ? {1'b0, nw0[23:14]} + 11'd1 : ({1'b0, nw0[23:12]} + 13'd3) >> 2;
                            y1c = incl ? {1'b0, nw0[11:2]}  + 11'd1 : ({1'b0, nw0[11:0]}  + 13'd3) >> 2;
                            xa_c = x0c > {1'b0, sc_x0} ? x0c : {1'b0, sc_x0};
                            ya_c = y0c > {1'b0, sc_y0} ? y0c : {1'b0, sc_y0};
                            // (copy and fill mode write the scissor's last column too, as the console's: the span
                            // runs to the edge clamped to the scissor, that pixel included)
                            xa <= xa_c[9:0];
                            xb <= (x1c < {1'b0, sc_x1} + {10'd0, incl}) ? x1c[9:0] : sc_x1 + {9'd0, incl};
                            y <= {1'b0, ya_c};
                            yb <= (y1c < {1'b0, sc_y1}) ? {1'b0, y1c} : {2'b00, sc_y1};
                            if (((x1c < {1'b0, sc_x1} + {10'd0, incl}) ? x1c : {1'b0, sc_x1} + {10'd0, incl}) <= xa_c)
                                yb <= {1'b0, ya_c};   // empty
                        end
                        mi <= 5'd0;
                        st <= (nop == 6'h36) ? S_RECT_INIT : S_TLOAD; tl_tri <= 1'b0;     // (a fill rectangle has no S, T)
                    end
                    6'h2C: begin cv_k03 <= {nw0[21:0], nw1[31:18]}; k4 <= nw1[17:9]; k5 <= nw1[8:0]; busy_exec <= 1'b0; end
                    6'h2D: begin
                        sc_xh <= nw0[23:12]; sc_yh <= nw0[11:0]; sc_xl <= nw1[23:12]; sc_yl <= nw1[11:0];
                        sc_field <= nw1[25]; sc_odd <= nw1[24];
                        busy_exec <= 1'b0;
                    end
                    6'h2E: begin prim_z <= {nw1[30:16], 1'b0}; prim_dz <= nw1[15:0]; busy_exec <= 1'b0; end      // the N64's: 15 bits, and its delta
                    6'h2F: begin other_h <= nw0[23:0]; other_l <= nw1; busy_exec <= 1'b0; end
                    6'h32: begin
                        tl_size[nw1[26:24]] <= {nw0[23:12], nw0[11:0], nw1[23:12], nw1[11:0]};
                        busy_exec <= 1'b0;
                    end
                    // loads: the tile's TMEM address and format reach S_LOAD_INIT through ts_*
                    6'h30, 6'h33, 6'h34: begin
                        // (each load sets its tile's size from its coordinates, as the console's: LOAD BLOCK's lrt is
                        // its dxt)
                        tl_size[nw1[26:24]] <= {nw0[23:12], nw0[11:0], nw1[23:12], nw1[11:0]};
                        p_tile <= nw1[26:24];
                        l_kind <= (nop == 6'h33) ? 2'd0 : (nop == 6'h34) ? 2'd1 : 2'd2;
                        mi <= 5'd0;
                        st <= S_LOAD_INIT;
                    end
                    6'h35: begin
                        yuv_vec[nw1[26:24]] <= nw0[23:21] == 3'd1 && nw0[20:19] == 2'd2;
                        tl_attr[nw1[26:24]] <= {nw0[23:21], nw0[20:19], nw1[23:20], nw0[17:9], nw0[8:0],
                                               nw1[9], nw1[8], nw1[19], nw1[18],
                                               nw1[7:4], nw1[3:0], nw1[17:14], nw1[13:10]};
                        busy_exec <= 1'b0;
                    end
                    6'h37: begin fill <= nw1; busy_exec <= 1'b0; end
                    6'h38: begin fog <= nw1; busy_exec <= 1'b0; end
                    6'h39: begin blend <= nw1; busy_exec <= 1'b0; end
                    6'h3A: begin prim <= nw1; prim_lod_frac <= nw0[7:0]; min_lvl <= nw0[12:8]; busy_exec <= 1'b0; end
                    6'h3B: begin env <= nw1; busy_exec <= 1'b0; end
                    6'h3C: begin cc_hi <= nw0[23:0]; cc_lo <= nw1; busy_exec <= 1'b0; end
                    6'h3D: begin timg <= nw1; timg_width <= {1'b0, nw0[9:0]} + 11'd1; timg_size <= nw0[20:19]; busy_exec <= 1'b0; end
                    6'h3E: begin zimg <= nw1; busy_exec <= 1'b0; end
                    6'h3F: begin
                        cimg <= nw1; cimg_width <= {1'b0, nw0[9:0]} + 11'd1;
                        cimg_ok <= nw0[20:19] == 2'd2 || nw0[20:19] == 2'd1 || nw0[20:19] == 2'd3;
                        cimg8 <= nw0[20:19] == 2'd1; cimg32 <= nw0[20:19] == 2'd3;
                        busy_exec <= 1'b0;
                    end
                    default: begin unknown_ops <= unknown_ops + 16'd1; busy_exec <= 1'b0; end
                    endcase
                end
            end

            // ---- a triangle's edges (words 2..7) or a texture rectangle's S, T and steps (2, 3) out of the staging RAM:
            // word mi asked for at step mi - 2, there at step mi
            S_TLOAD: begin
                vr_ra <= {1'b0, mi} + 6'd2;
                case (mi)
                    5'd2: t_xl <= vr_q;  5'd3: t_dxl <= vr_q;
                    5'd4: t_xh <= vr_q;  5'd5: t_dxh <= vr_q;
                    5'd6: t_xm <= vr_q;  5'd7: t_dxm <= vr_q;
                    default: ;
                endcase
                mi <= mi + 5'd1;
                if (mi == (tl_tri ? 5'd7 : 5'd3)) begin mi <= 5'd0; st <= tl_tri ? S_XL : S_RECT_INIT; end
            end

            // ---- the N64's XL is the low edge at YM itself, not at YM's
            // scanline as XH and XM are at YH's: back by the quarter rows between
            // (AA_EN: XL & ~1, back by the subscanline step (DxLDy >> 2) & ~1)
            S_XL: begin
                if (mi == 5'd2) begin t_xl <= aa_init; st <= S_VSET; end
                mi <= (mi == 5'd2) ? 5'd0 : mi + 5'd1;
            end

            // ---- an attribute's value, DaDx, DaDe from the staging RAM (the
            // data two steps after the address)
            S_VSET: begin
                // (the LOD's DaDy of S, T, W: read at steps 6 and 7, free, its integer half there at 15 and its fraction's
                // top bit at the next attribute's step 0 -- the model keeps the top 17 bits)
                if (vs == 4'd0 && tflags[2] && vi >= 3'd1 && vi <= 3'd4)         // (the previous shade attribute's DaDy fraction's top bits)
                    sdy[vi - 3'd1][1:0] <= vi[0] ? vr_q[31:30] : vr_q[15:14];
                if (vs == 4'd0 && tflags[1] && (vi == 3'd5 || vi == 3'd6 || vi == 3'd7)) begin
                    case (vi)
                        3'd5: ldy_s[0] <= vr_q[31];
                        3'd6: ldy_t[0] <= vr_q[15];
                        default: ldy_w[0] <= vr_q[31];
                    endcase
                end
                case (vs)
                    4'd0: if (!attr_on(tflags, vi)) begin
                              vi <= vi + 3'd1;
                              if (vi == 3'd7) st <= S_TRI_INIT;
                          end else begin
                              vr_ra <= vgb + (vz ? 6'd0 : {5'd0, vi[1]}); vs <= 4'd1;
                          end
                    4'd1: begin vr_ra <= vgb + voff(3'd1, vz) + (vz ? 6'd0 : {5'd0, vi[1]}); vs <= 4'd2; end
                    4'd2: begin
                        vr_ra <= vgb + voff(3'd2, vz) + (vz ? 6'd0 : {5'd0, vi[1]}); vs <= 4'd3;
                    end
                    4'd3: begin
                        vr_ra <= vz ? vgb + 6'd3 : vgb + voff(3'd3, vz) + {5'd0, vi[1]}; vs <= 4'd4;
                    end
                    4'd4: begin
                        vr_ra <= vgb + voff(3'd4, vz) + {5'd0, vi[1]};
                        vs <= 4'd5;
                    end
                    4'd5: if (vz) begin zdy <= vr_q; vs <= 4'd15; end
                          else begin vr_ra <= vgb + voff(3'd5, vz) + {5'd0, vi[1]}; vs <= 4'd6; end
                    4'd6: begin vs <= 4'd7; vr_ra <= vgb + 6'd10 + {5'd0, vi[1]}; end
                    4'd7: begin vs <= 4'd15; vr_ra <= vgb + 6'd14 + {5'd0, vi[1]}; end
                    default: begin                   // 15: the attribute's registers
                        for (ai = 0; ai < 8; ai = ai + 1) if (vi == ai) adx[ai] <= (ai == 7) ? vgx : {vgx[31:5], 5'd0};
                        if (vi < 3'd4) sdy[vi][12:2] <= vhalf[10:0];
                        if (!vz) case (vi)
                            3'd4: ldy_s[16:1] <= vhalf;
                            3'd5: ldy_t[16:1] <= vhalf;
                            3'd6: ldy_w[16:1] <= vhalf;
                            default: ;
                        endcase             // e[vi], ade[vi]: the RAM writes below
                        if (vz) dzc_n <= dzc_p;
                        vs <= 4'd0; vi <= vi + 3'd1;
                        if (vi == 3'd7) st <= S_TRI_INIT;
                    end
                endcase
            end

            // ---- triangle: edge and attribute values at the first row, and
            // that row's offset into the images (y * width * 2; an 8-bit image's y * width)
            S_TRI_INIT: begin
                begin : tri_init
                    // (the operands: mul_ad / mul_bd)
                    // products come back two cycles later
                    case (mi)
                        // 5..12: e[mi - 5] = e + k DaDe (the RAM writes below)
                        default: ;
                    endcase
                    mi <= mi + 5'd1;
                    if (mi == 5'd14) begin
                        st <= S_AAINIT;
                        mi <= 5'd0;
                    end
                end
            end

            // ---- AA_EN: the edges at the first row's top: X & ~1 plus the
            // rows down times four subscanline steps (DxDy & ~7)
            S_AAINIT: begin
                case (mi)
                    5'd0, 5'd1: ;
                    5'd2, 5'd3: ;
                    default: begin st <= S_GO; wk <= W_ROW; end
                endcase
                mi <= mi + 5'd1;
            end

            // ---- a fill-mode rectangle's row (the other primitives' rows are walked by wk, below)
            S_ROW: begin
                if (y >= yb) begin
                    st <= S_FIN;
                end else if (sc_skip) begin
                    st <= S_NEXT;                   // (a row of the field not kept)
                end else begin
                    xs <= xa;
                    xe <= {1'b0, xb};
                    spl <= 1'b0;
                    par_c <= par;
                    mi <= 5'd0;
                    st <= S_FILL;
                end
            end

            // ---- the row walker has the next row's place ready: take it, and set up its attributes
            S_GO: begin
                if (wk == W_RDY && (!kind_tri || es[3])) begin
                    xs <= kind_tri ? aa_min : xa;
                    xe <= kind_tri ? (a_cut ? aa_m512 : aa_xe) : r_cut ? {1'b0, xa} + 11'd512 : {1'b0, xb};
                    spl <= kind_tri ? a_cut : r_cut;
                    par_c <= par; cy <= y[2:0];
                    set_cur <= !set_cur;
                    mi <= 5'd0;
                    st <= S_SETUP;
                    wk <= W_HOLD;
                end else if (wk == W_END && (!kind_tri || es[3])) begin
                    st <= S_FIN;
                    wk <= W_OFF;
                end
            end

            // ---- span setup: cur = e + DaDx * (xs - xh), the plane at the pixel's
            // top-left corner (triangles),
            // the S/T accumulators (texrects)
            S_SETUP: begin
                if (kind_tri) begin
                    mi <= mi + 5'd1;
                    if (mi == 5'd10) begin
                        st <= S_PIX;
                        es <= 4'd0;                 // e[] no longer needed this row: step it
                    end
                end else begin
                    // texrect: t_xl = S T, t_dxl = DsDx DtDy; copy mode steps 4x
                    begin : rect_setup
                        shS <= rs_ds;
                        adx[4] <= (op == 6'h25 || op == 6'h36) ? 32'd0 : cyc == 2'd2 ? {r_dsdx[29:0], 2'd0} : r_dsdx;     // (copy: a group's step, /128)
                        adx[5] <= (op == 6'h25) ? r_dtdy : 32'd0;
                        mi <= mi + 5'd1;
                        if (mi == 5'd3) st <= S_PIX;
                    end
                end
                px_cnt <= 10'd0; cp_m <= 3'd0;
                pix_stp <= 1'b0; pix_fol <= 1'b0; pix_g0 <= lod_on && !lod_lft;
            end

            // ---- pixels, one a clock as what they read of memory comes in.
            // The row's write-back is asked for first (it follows the pixels'
            // results out of the pipe); the row is left with its last pixel
            // issued -- the next row's setup and reads run under this one's
            // pipeline and write-back.
            S_PIX: begin
                if (!row_wq) begin
                    // (not before the row's read is taken: with an image whose rows
                    // share hidden groups, hz_ok holds that read until the write
                    // engine is idle and nothing waits for it -- a write request of
                    // this row's, waiting on pixels the read gates, would hold it forever)
                    if (!wq_pend && !rq_pend) begin
                        wq_pend <= 1'b1; wq_fill <= 1'b0; wq_z <= z_upd; wq_set <= set_cur;
                        wq_cbase <= crow; wq_zbase <= zimg + row_off; wq_x0 <= xsp; wq_x1 <= xep;
                        res_n[set_cur] <= 11'd0; res_d[set_cur] <= 11'd0; res_q[set_cur] <= 11'd0;
                        row_wq <= 1'b1;
                    end
                end else if (pix_gh) begin
                    // a ghost: cur goes to its place but the pixel is not counted (walking right, the pixel itself
                    // follows it there: no step then; walking left, the span's first pixel is the next place)
                    pix_valid <= 1'b1; pix_ghost <= 1'b1; pix_mask <= 8'h00;
                    if (pix_stp) begin
                    end
                    pix_stp <= pix_g0; pix_g0 <= 1'b0; pix_fol <= 1'b0;
                    if (pix_end) begin
                        st <= spl ? S_SPL : S_GO;
                        row_wq <= 1'b0;
                        old_hold[set_cur] <= 5'd20;
                    end
                end else if (pix_go) begin
                    pix_valid <= 1'b1; pix_ghost <= 1'b0;
                    pix_x <= xsp + px_cnt;
                    cp_m_q <= cp_m;
                    pix_mask <= !p_aa ? 8'hFF : (p_aaen || aa_mask[7]) ? aa_mask : 8'h00;   // (no AA_EN: sample 7 or nothing)
                    pix_stp <= 1'b1;
                    pix_fol <= lod_on && lod_lft;
                    px_cnt <= px_cnt + 10'd1;
                    cp_m <= cp_end ? 3'd0 : cp_m + 3'd1;
                    // (walking right with the LOD the row ends with the last pixel's ghost)
                    if ({1'b0, xs} + {1'b0, px_cnt} + 11'd1 == xe && !(lod_on && lod_lft)) begin
                        st <= spl ? S_SPL : S_GO;
                        row_wq <= 1'b0;
                        // (its last line buffer read: the pixel's P9, D clocks later with the LOD)
                        old_hold[set_cur] <= lod_on ? 5'd20 : two_pass ? 5'd15 : 5'd14;
                    end
                end
            end

            // ---- fill mode rectangles: straight to memory
            S_FILL: if (!wq_pend) begin
                wq_pend <= 1'b1; wq_fill <= 1'b1; wq_z <= 1'b0; wq_set <= 1'b0;
                wq_cbase <= crow; wq_x0 <= xsp; wq_x1 <= xep;
                st <= S_NEXT;
            end

            // ---- a 32-bit image's span's second half: its own reads on the other set, then its pixels, the walk going
            // on from the first half's last pixel (cur holds it, and steps at the next issue; the LOD walking left first
            // issues a ghost of that pixel, as at a span's start)
            S_SPL: begin
                xs <= xe[9:0]; xe <= xe2; spl <= 1'b0;
                px_cnt <= 10'd0;
                pix_g0 <= lod_on && !lod_lft;
                if (lod_on && !lod_lft) pix_stp <= 1'b0;
                st <= S_PIX;
                set_cur <= !set_cur;
                rq_pend <= 1'b1; rq_load <= 1'b0; rq_z <= z_cmp; rq_c <= need_mem; rq_h <= 1'b0; rq_hz <= z_cmp || z_upd; rq_half <= 1'b1; rq_set <= !set_cur;
                rq_zbase <= zimg + row_off; rq_cbase <= crow;
                rq_x0 <= xe[9:0]; rq_x1 <= xe2;      // (a 32-bit image's: par is 0)
            end

            // ---- the primitive's end: its pixels out of the pipe and back
            // in memory before anything else (the next may change the state
            // they are drawn with, or read what they write)
            // (a triangle that comes next starts under the drain: its setup and its first row's walk need nothing the
            // drain uses, and it waits for the drain where its first span's reads are asked for)
            S_FIN: if (all_quiet || (exec_go && tri_early)) begin
                st <= S_IDLE;
                busy_exec <= 1'b0;
            end

            // ---- next row (a fill-mode rectangle's)
            S_NEXT: begin
                y <= y + 12'sd1;
                st <= S_ROW;
            end

            // ---- rectangles: row offset of the first row
            S_RECT_INIT: begin
                if (mi == 5'd2) begin
                    if (kind_fillmode) st <= S_ROW;
                    else begin st <= S_GO; wk <= W_ROW; end
                end
                mi <= mi + 5'd1;
            end

            // ---- loads: the first row's texel in the image and its place in
            // TMEM (the load tile's ts_* are in by step 1), then a request a row
            S_LOAD_INIT: begin
                begin : load_init
                    reg [9:0]  s0, t0, s1, t1;
                    reg [12:0] nt;
                    // LOAD BLOCK's coordinates are whole texels, the others' 10.2
                    s0 = w0[23:14]; t0 = w0[11:2]; s1 = w1[23:14]; t1 = w1[11:2];
                    if (mi == 5'd2) begin
                        l_ti <= mul_p[22:0] + ((l_kind == 2'd0) ? {11'd0, w0[23:12]} : {13'd0, s0});
                        l_dst <= {ts_tmem, 2'b00};
                        l_odd <= 1'b0;
                        if (l_kind == 2'd0) begin
                            nt = {1'b0, w1[23:12]} - {1'b0, w0[23:12]} + 13'd1;
                            l_n <= load_words(nt, timg_size);
                            l_none <= w1[23:12] < w0[23:12];
                            tt <= 10'd0; tt1 <= 10'd0;
                        end else begin
                            nt = {3'd0, s1} - {3'd0, s0} + 13'd1;
                            l_n <= (l_kind == 2'd2) ? ((nt > 13'd256) ? 11'd256 : nt[10:0]) : load_words(nt, timg_size);
                            l_none <= s1 < s0 || (l_kind == 2'd1 && t1 < t0);
                            tt <= (l_kind == 2'd1) ? t0 : 10'd0; tt1 <= (l_kind == 2'd1) ? t1 : 10'd0;
                        end
                        st <= S_LOAD;
                    end
                    mi <= mi + 5'd1;
                end
            end
            S_LOAD: if (all_quiet) begin
                if (l_none || tt > tt1) begin
                    load_count <= load_count + 32'd1;
                    st <= S_IDLE;
                    busy_exec <= 1'b0;
                end else begin
                    rq_pend <= 1'b1; rq_load <= 1'b1; rq_z <= 1'b0; rq_c <= 1'b0; rq_h <= 1'b0; rq_hz <= 1'b0; rq_set <= 1'b0;
                    rq_cbase <= timg + ((l_kind == 2'd2) ? {8'd0, l_ti, 1'b0}
                                                         : ({9'd0, l_ti} << timg_size) >> 1);
                    rq_ldst <= l_dst; rq_ln <= l_n;
                    rq_lmode <= (l_kind == 2'd2) ? 2'd2 : (ts_fmt == 3'd0 && ts_sz == 2'd3) ? 2'd1
                              : (ts_fmt == 3'd1 && ts_sz == 2'd2) ? 2'd3 : 2'd0;   // (3: YUV 16)
                    // (the odd rows' words go in swapped: by dxt, or a LOAD TILE's by its rows)
                    rq_ldxt <= (l_kind == 2'd0) ? w1[11:0] : 12'd0;
                    rq_lodd <= l_kind == 2'd1 && l_odd;
                    l_odd <= ~l_odd;
                    l_ti <= l_ti + {12'd0, timg_width};
                    l_dst <= l_dst + {ts_line, 2'b00};
                    if (tt == tt1) l_none <= 1'b1;
                    tt <= tt + 10'd1;
                end
            end

            // ---- SYNC FULL: every earlier write accepted by the memory
            S_SYNC: if (all_quiet && !mem_wpend) begin
                sync_count <= sync_count + 32'd1;
                sync_ev <= 1'b1;
                st <= S_IDLE;
                busy_exec <= 1'b0;
            end

            default: st <= S_IDLE;
            endcase

            // ---- the row walker
            case (wk)
            W_ROW: begin
                if (y >= yb) wk <= W_END;
                else if (kind_tri) begin
                    aa_j <= 3'd0;
                    aa_lo <= $signed({y, 2'b00}) < ym_s;
                    wk <= W_SUB;
                end else if (sc_skip) wk <= W_SKIP;               // (a row of the field not kept)
                else if (!rq_pend) begin
                    // rectangles: columns fixed (a 32-bit image's span of more than 512 pixels: its first half)
                    rq_x0 <= xa + {9'd0, par}; rq_x1 <= (r_cut ? {1'b0, xa} + 11'd512 : {1'b0, xb}) + {10'd0, par};
                    rq_pend <= 1'b1; rq_load <= 1'b0; rq_z <= z_cmp; rq_c <= need_mem; rq_h <= !cimg8 && !cimg32; rq_hz <= z_cmp || z_upd; rq_half <= 1'b0; rq_set <= !set_cur;
                    rq_zbase <= zimg + row_off; rq_cbase <= crow;
                    wk <= W_RDY;
                end
            end
            // ---- AA_EN: the row's four subscanlines (and a clock for the last's stage 2)
            W_SUB: begin
                aa_j <= aa_j + 3'd1;
                aa_lo <= (aa_yq + 14'sd1) < ym_s;
                if (aa_j == 3'd4) wk <= W_SPAN;
            end
            W_SPAN: begin
                if (!aa_any || {1'b0, aa_min} >= aa_xe || sc_skip) wk <= W_SKIP;
                else if (rq_pend || (cmt && !all_quiet)) ;      // (the primitive before still draining: its pixels, its writes)
                else begin
                    if (cmt) begin
                        p_shade <= n_shade; p_tex <= n_tex; p_zbuf <= n_zbuf; p_direct <= 1'b0; p_tile <= n_tile;
                        lod_lft <= n_lodlft; lod_lvl <= n_lodlvl; p_aa <= 1'b1; p_aaen <= n_aaen; dzc_r <= dzc_n;
                        cmt <= 1'b0;
                    end
                    rq_x0 <= aa_min + {9'd0, par}; rq_x1 <= (a_cut ? aa_m512 : aa_xe) + {10'd0, par};
                    rq_pend <= 1'b1; rq_load <= 1'b0; rq_z <= z_cmp; rq_c <= need_mem; rq_h <= !cimg8 && !cimg32; rq_hz <= z_cmp || z_upd; rq_half <= 1'b0; rq_set <= !set_cur;
                    rq_zbase <= zimg + row_off; rq_cbase <= crow;
                    wk <= W_RDY;
                end
            end
            // (W_RDY: S_GO takes the row. W_HOLD: the row has set itself up and its write-back has been asked for)
            // (the next row: the step of the attributes' row values (es, a clock each) runs under its walk, and S_GO waits for it;
            // a row with no span has its own step, after the one before)
            W_HOLD: if (hold_ok) wk <= W_ADV;
            W_ADV: begin y <= y + 12'sd1; wk <= W_ROW; end
            W_SKIP: if (!kind_tri || es[3]) begin
                if (kind_tri) es <= 4'd0;
                y <= y + 12'sd1;
                wk <= W_ROW;
            end
            default: ;
            endcase
        end
    end


    assign idle = !busy_exec && !exec_go && !collecting && all_quiet && !mem_wpend;
    assign tmem_busy = st == S_LOAD_INIT || st == S_LOAD;
endmodule
