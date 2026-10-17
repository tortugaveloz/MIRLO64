// RDP -- Mirlo's rasterizer for the N64's own command stream, as a game's
// microcode on the RSP (rtl/rsp) gives it. The contract is the C model,
// lang/c/mrdp/mrdp.c built with MRDP_N64: for the same command stream the RTL
// writes the same memory. From MRDP (rtl/mrdp), which draws the
// geometry core's triangles: here the triangles are the N64's (0x08..0x0F),
// TMEM is the N64's 4 KiB loaded by LOAD BLOCK / LOAD TILE / LOAD TLUT and
// decoded by the tile's format when sampled, texture rectangles, 2-cycle and
// copy mode are in, and the images are the game's (RGBA5551, the N64's byte
// order). The coverage (anti-aliasing) is the N64's (lang/c/mrdp/mrdp.h,
// MRDP_CVG): with AA_EN a triangle is walked four subscanlines a row, and its
// pixels carry their 8-sample masks. YUV textures are converted (SET_CONVERT).
// Not done: LOD.
//
// Commands are executed one at a time. A primitive is drawn row by row: the
// row's Z and color spans are read into on-chip line buffers, the pixels run
// through mrdp_pix at one a clock, then the span is written back (only the
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
    output wire        idle             // no command in progress, nothing pending
);
    // ======================================================================
    // state (lang/c/mrdp/mrdp.h mrdp_t)
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
    reg [31:0] cimg, zimg, timg;
    reg [10:0] cimg_width, timg_width;
    reg [1:0]  timg_size;
    reg        cimg_ok;             // the colour image is a 16-bit or an 8-bit one (what is drawn to)
    // An 8-bit colour image (lang/c/mrdp/mrdp.c wr8): a pixel is a byte at cimg + y * width + x, the blender's red as it
    // is (copy: the texel's; fill: the fill colour's byte for the address). Written only: the pixels read neither it nor
    // a depth, and no hidden coverage goes with it. The pixel pipe's results go through the new buffers as for a 16-bit
    // image, the byte in a pixel's colour bits 15:8; the write engine puts a word's two pixels in their half of the
    // memory word (rdp_mem wr_c8). The rows must start on a 4-byte boundary, as a 16-bit image's.
    reg        cimg8;

    // The 8 tiles, in two small RAMs (SET_TILE writes the format half,
    // SET_TILESIZE and LOAD_TILE the size half): as flip-flops they were 688
    // registers plus their write decode and an 8-way read mux. Read through
    // one registered port at p_tile (ts_* below).
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
    // (and the memory's coverage: IM_RD, with AA_EN or CVG_X_ALPHA)
    wire need_mem = (cyc[1] || cimg8) ? 1'b0 : (other_l[6] && (other_l[3] || other_l[12])) ? 1'b1
                  : !other_l[14] ? rd_p : (cyc == 2'd1) ? (rd_mem0 | rd_mem1) : rd_mem0;
    wire z_cmp = other_l[4] && !cyc[1] && !cimg8;       // (copy and fill mode have no depth, nor an 8-bit image)
    wire z_upd = other_l[5] && !cyc[1] && !cimg8;

    // ======================================================================
    // command intake
    // ======================================================================
    reg [31:0] w0;                 // first word of the command
    reg [5:0]  nwords, widx;       // total words, index of the next
    reg        collecting;
    reg        exec_go;            // command complete: execute
    reg        busy_exec;          // executing (FSM not idle)
    reg [31:0] w1;                 // second word

    // triangle coefficients, decoded as they arrive; the attributes' words
    // (from the ninth on) wait in a staging RAM and are put together -- each
    // value, DaDx and DaDe from its integer and fraction halves -- in S_VSET
    reg        t_lft;
    reg [2:0]  t_tile;
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
    (* ramstyle = "MLAB, no_rw_check" *) reg [31:0] ade_ram [0:7];
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
    wire [5:0] op  = w0[29:24];
    wire       is_tri = op[5:3] == 3'b001;      // the N64's triangles, 0x08..0x0F
    assign cmd_ready = !busy_exec && !exec_go;


    // ======================================================================
    // RAMs
    // ======================================================================
    // TMEM: a copy for each of the filter's three texels
    wire [10:0] tm_ra0, tm_ra1, tm_ra2;
    wire [15:0] tm_q0, tm_q1, tm_q2, tm_h0, tm_h1, tm_h2, tl_q0, tl_q1, tl_q2;
    wire [7:0]  tl_a0, tl_a1, tl_a2;
    wire [3:0]  tm_we;
    wire [8:0]  tm_wa;
    wire [15:0] tm_wd0, tm_wd1, tm_wd2, tm_wd3;
    rdp_tmem tmem0 (.clk(clk), .we(tm_we), .wa(tm_wa), .wd0(tm_wd0), .wd1(tm_wd1), .wd2(tm_wd2), .wd3(tm_wd3),
                    .ra(tm_ra0), .q(tm_q0), .qh(tm_h0), .ta(tl_a0), .tq(tl_q0));
    rdp_tmem tmem1 (.clk(clk), .we(tm_we), .wa(tm_wa), .wd0(tm_wd0), .wd1(tm_wd1), .wd2(tm_wd2), .wd3(tm_wd3),
                    .ra(tm_ra1), .q(tm_q1), .qh(tm_h1), .ta(tl_a1), .tq(tl_q1));
    rdp_tmem tmem2 (.clk(clk), .we(tm_we), .wa(tm_wa), .wd0(tm_wd0), .wd1(tm_wd1), .wd2(tm_wd2), .wd3(tm_wd3),
                    .ra(tm_ra2), .q(tm_q2), .qh(tm_h2), .ta(tl_a2), .tq(tl_q2));

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
    wire [9:0]  pix_lb_ra;
    wire [31:0] zlb_rd, clb_rd;
    mrdp_sdpram #(.AW(10), .DW(32), .NB(2)) zold (.clk(clk), .we(mw_z_we), .waddr(mw_addr), .wdata(mw_data), .wbe(2'b11), .raddr(pix_lb_ra), .rdata(zlb_rd));
    mrdp_sdpram #(.AW(10), .DW(32), .NB(2)) cold (.clk(clk), .we(mw_c_we), .waddr(mw_addr), .wdata(mw_data), .wbe(2'b11), .raddr(pix_lb_ra), .rdata(clb_rd));
    reg         zn_we, cn_we, msk_we;
    reg  [9:0]  nb_wa;
    reg  [31:0] zn_wd;
    reg  [35:0] cn_wd;                  // {colour 16, hidden 2} x 2, the even pixel first
    reg  [1:0]  nb_be;
    reg  [1:0]  zm_wd, cm_wd, msk_be;
    wire [9:0]  mem_lb_ra;
    wire [31:0] znew_rd;
    wire [35:0] cnew_rd;
    wire [1:0]  zm_rd, cm_rd;
    wire [9:0]  zn_ra = mem_lb_ra;
    mrdp_sdpram #(.AW(10), .DW(32), .NB(2)) znew (.clk(clk), .we(zn_we), .waddr(nb_wa), .wdata(zn_wd), .wbe(nb_be), .raddr(zn_ra), .rdata(znew_rd));
    mrdp_sdpram #(.AW(10), .DW(36), .NB(2)) cnew (.clk(clk), .we(cn_we), .waddr(nb_wa), .wdata(cn_wd), .wbe(nb_be), .raddr(mem_lb_ra), .rdata(cnew_rd));
    // the raw hidden-bit buffers (rdp_mem): the span's groups' words, by {set,
    // group mod 128}, a copy for the pixels and one for the write engine
    wire        hl_we;
    wire [7:0]  hl_wa, pix_hl_ra, mem_hl_ra;
    wire [31:0] pix_hl_rd, mem_hl_rd;
    mrdp_sdpram #(.AW(8), .DW(32), .NB(1)) hraw0 (.clk(clk), .we(hl_we), .waddr(hl_wa), .wdata(mw_data), .wbe(1'b1), .raddr(pix_hl_ra), .rdata(pix_hl_rd));
    mrdp_sdpram #(.AW(8), .DW(32), .NB(1)) hraw1 (.clk(clk), .we(hl_we), .waddr(hl_wa), .wdata(mw_data), .wbe(1'b1), .raddr(mem_hl_ra), .rdata(mem_hl_rd));
    reg  [9:0]  rwb [0:1];              // the rows' colour word base (bits 9:0), by set
    mrdp_sdpram #(.AW(10), .DW(2), .NB(2)) zmsk (.clk(clk), .we(msk_we), .waddr(nb_wa), .wdata(zm_wd), .wbe(msk_be), .raddr(zn_ra), .rdata(zm_rd));
    mrdp_sdpram #(.AW(10), .DW(2), .NB(2)) cmsk (.clk(clk), .we(msk_we), .waddr(nb_wa), .wdata(cm_wd), .wbe(msk_be), .raddr(mem_lb_ra), .rdata(cm_rd));
    // a triangle's attribute words: word 8 + k of the command at k
    wire        vr_we = cmd_valid && cmd_ready && collecting && is_tri && widx >= 6'd8;
    wire [5:0]  vr_wa6 = widx - 6'd8;
    reg  [5:0]  vr_ra;
    wire [31:0] vr_q;
    mrdp_sdpram #(.AW(6), .DW(32), .NB(1)) vram (.clk(clk), .we(vr_we), .waddr(vr_wa6),
                                                 .wdata(cmd_data), .wbe(1'b1), .raddr(vr_ra), .rdata(vr_q));

    // ======================================================================
    // the pixel pipeline
    // ======================================================================
    reg         p_shade, p_tex, p_zbuf, p_direct;
    reg  [2:0]  p_tile;
    reg         pix_valid;
    reg  [9:0]  pix_x;
    reg  [7:0]  pix_mask;           // its samples (0xFF: all)
    reg         p_aa;               // a triangle: the N64's walker
    reg         p_aaen;             // ... with AA_EN (the pixel is drawn for any sample; else for sample 7 only)
    reg  [31:0] cur [0:7];          // iterated attributes (triangles)
    // texrects step S and T in cur[4], cur[5] by adx[4], adx[5] -- the
    // triangle's own S/T adders -- and pass s = cur[4] >>> shS, t = cur[5] >>> 5
    reg  [2:0]  shS;
    // a triangle's S and T are the N64's -- s10.5 in the integer half, against
    // a W of s.15 -- taken to the pipeline's units (lang/c/mrdp/mrdp.c draw_pixel)
    wire [4:0]  shN = other_h[19] ? 5'd4 : 5'd5;
    wire [31:0] pr_s = p_direct ? asr(cur[4], {2'b0, shS}) : asr(cur[4], shN);
    wire [31:0] pr_t = p_direct ? asr(cur[5], 5'd5) : asr(cur[5], shN);
    // The drawing tile's fields, registered: indexing tile[p_tile] straight
    // into the pixel pipe was the worst path of the first fit (-4 ns). p_tile
    // and the tiles change only between primitives, many cycles before the
    // first pixel.
    reg [2:0]  ts_fmt;
    reg [1:0]  ts_sz;
    reg [3:0]  ts_pal;
    reg [8:0]  ts_line;
    reg [8:0]  ts_tmem;
    reg        ts_cs, ts_ms, ts_ct, ts_mt;
    reg [3:0]  ts_masks, ts_shifts, ts_maskt, ts_shiftt;
    reg [11:0] ts_uls, ts_ult, ts_lrs, ts_lrt;
    always @(posedge clk) begin
        {ts_fmt, ts_sz, ts_pal, ts_line, ts_tmem, ts_cs, ts_ms, ts_ct, ts_mt, ts_masks, ts_shifts, ts_maskt, ts_shiftt} <= tl_attr[p_tile];
        {ts_uls, ts_ult, ts_lrs, ts_lrt} <= tl_size[p_tile];
    end
    // and the next tile's (TEXEL1: tile + 1, wrapping at 8), for a pixel's second pass (rdp_pix two_pass)
    wire [2:0] p_tile1 = p_tile + 3'd1;
    reg [2:0]  ts1_fmt;
    reg [1:0]  ts1_sz;
    reg [3:0]  ts1_pal;
    reg [8:0]  ts1_line;
    reg [8:0]  ts1_tmem;
    reg        ts1_cs, ts1_ms, ts1_ct, ts1_mt;
    reg [3:0]  ts1_masks, ts1_shifts, ts1_maskt, ts1_shiftt;
    reg [11:0] ts1_uls, ts1_ult, ts1_lrs, ts1_lrt;
    always @(posedge clk) begin
        {ts1_fmt, ts1_sz, ts1_pal, ts1_line, ts1_tmem, ts1_cs, ts1_ms, ts1_ct, ts1_mt, ts1_masks, ts1_shifts, ts1_maskt, ts1_shiftt} <= tl_attr[p_tile1];
        {ts1_uls, ts1_ult, ts1_lrs, ts1_lrt} <= tl_size[p_tile1];
    end
    wire two_pass;
    wire        px_valid, px_cpass, px_zpass, pix_busy, px_set;
    reg         set_cur;            // the line-buffer set of the row being drawn
    wire [9:0]  px_x;
    wire [15:0] px_color, px_depth;
    wire [7:0]  px_r8;
    wire [1:0]  px_hid;
    rdp_pix pix (
        .clk(clk), .rst(rst),
        .other_h(other_h), .other_l(other_l), .cc_hi(cc_hi), .cc_lo(cc_lo),
        .prim(prim), .env(env), .fog(fog), .blend(blend),
        .prim_lod_frac(prim_lod_frac), .k4(k4), .k5(k5), .cv_k03(cv_k03), .prim_z(prim_z), .z_tol(p_zbuf ? z_tol : 16'd1), .fill(fill),
        .prim_dz(prim_dz), .need_mem(need_mem), .c8(cimg8), .p_aa(p_aa), .z_dx(adx[7]), .z_dy(zdy),
        .p_shade(p_shade), .p_tex(p_tex), .p_zbuf(p_zbuf), .p_direct(p_direct),
        .t_fmt(ts_fmt), .t_size(ts_sz), .t_pal(ts_pal), .t_line(ts_line), .t_tmem(ts_tmem),
        .t_cs(ts_cs), .t_ms(ts_ms), .t_ct(ts_ct), .t_mt(ts_mt),
        .t_masks(ts_masks), .t_shifts(ts_shifts), .t_maskt(ts_maskt), .t_shiftt(ts_shiftt),
        .t_uls(ts_uls), .t_ult(ts_ult), .t_lrs(ts_lrs), .t_lrt(ts_lrt),
        .t1_fmt(ts1_fmt), .t1_size(ts1_sz), .t1_pal(ts1_pal), .t1_line(ts1_line), .t1_tmem(ts1_tmem),
        .t1_cs(ts1_cs), .t1_ms(ts1_ms), .t1_ct(ts1_ct), .t1_mt(ts1_mt),
        .t1_masks(ts1_masks), .t1_shifts(ts1_shifts), .t1_maskt(ts1_maskt), .t1_shiftt(ts1_shiftt),
        .t1_uls(ts1_uls), .t1_ult(ts1_ult), .t1_lrs(ts1_lrs), .t1_lrt(ts1_lrt), .two_pass(two_pass),
        .in_valid(pix_valid), .in_x(pix_x), .in_set(set_cur), .row_y(y[1:0]),
        .in_r(cur[0]), .in_g(cur[1]), .in_b(cur[2]), .in_a(cur[3]),
        .in_s(pr_s), .in_t(pr_t), .in_w(cur[6]), .in_z(cur[7]), .in_mask(pix_mask),
        .tm_a0(tm_ra0), .tm_a1(tm_ra1), .tm_a2(tm_ra2),
        .tm_q0(tm_q0), .tm_q1(tm_q1), .tm_q2(tm_q2), .tm_h0(tm_h0), .tm_h1(tm_h1), .tm_h2(tm_h2),
        .tl_a0(tl_a0), .tl_a1(tl_a1), .tl_a2(tl_a2), .tl_q0(tl_q0), .tl_q1(tl_q1), .tl_q2(tl_q2),
        .lb_raddr(pix_lb_ra), .zlb_rdata(zlb_rd), .clb_rdata(clb_rd),
        .hl_raddr(pix_hl_ra), .hl_rdata(pix_hl_rd), .rwb0(rwb[0]), .rwb1(rwb[1]), .out_hid(px_hid),
        .out_valid(px_valid), .out_set(px_set), .out_x(px_x), .out_color(px_color), .out_r8(px_r8), .out_depth(px_depth),
        .out_cpass(px_cpass), .out_zpass(px_zpass), .busy(pix_busy));

    // ======================================================================
    // the memory engine
    // ======================================================================
    // a read request (a row's spans, or a load) and a write request (a row's
    // results, or a fill): each waits here for its engine
    reg         rq_pend, rq_load, rq_z, rq_c, rq_h, rq_set;
    reg  [31:0] rq_zbase, rq_cbase;
    reg  [9:0]  rq_x0;
    reg  [10:0] rq_x1;
    reg  [10:0] rq_ldst, rq_ln;
    reg  [1:0]  rq_lmode;
    reg  [11:0] rq_ldxt;
    reg         rq_lodd, l_odd;
    reg         wq_pend, wq_fill, wq_z, wq_set;
    reg  [31:0] wq_cbase, wq_zbase;
    reg  [9:0]  wq_x0;
    reg  [10:0] wq_x1;
    wire        rd_ready, wr_ready, rd_busy, wr_busy, mem_wpend, h_done, wr_seti;
    // a span's hidden words are read into its set only when the write engine
    // is not merging from that set -- and, when rows can share a group (the
    // image's rows not 16 pixels wide), only when nothing of the row before is
    // still to be written
    wire        hz_ok = !rq_h || (!(wr_busy && wr_seti == rq_set)
                                  && (cimg_width[3:0] == 4'd0 || (!wr_busy && !wq_pend)));
    wire [9:0]  z_have, c_have;
    // a row's results in the new buffers so far, by set -- as the write engine
    // may count on them: two cycles behind (the buffers' write, then its read)
    reg  [10:0] res_n [0:1];
    reg  [10:0] res_d [0:1];
    reg  [10:0] res_q [0:1];
    // a set's old buffers are not written while the last pixels of the row
    // that had them are still ahead of the pipe's read of them
    reg  [3:0]  old_hold [0:1];
    rdp_mem mem (
        .clk(clk), .rst(rst),
        .rd_valid(rq_pend && old_hold[rq_set] == 4'd0 && hz_ok), .rd_ready(rd_ready), .rd_load(rq_load), .rd_z(rq_z), .rd_c(rq_c), .rd_h(rq_h),
        .rd_zbase(rq_zbase), .rd_cbase(rq_cbase), .rd_x0(rq_x0), .rd_x1(rq_x1), .rd_set(rq_set),
        .rd_ldst(rq_ldst), .rd_ln(rq_ln), .rd_lmode(rq_lmode), .rd_ldxt(rq_ldxt), .rd_lodd(rq_lodd),
        .z_have(z_have), .c_have(c_have), .h_done(h_done), .hlbw_we(hl_we), .hlbw_addr(hl_wa),
        .lbw_z_we(mw_z_we), .lbw_c_we(mw_c_we), .lbw_addr(mw_addr), .lbw_data(mw_data),
        .wr_valid(wq_pend), .wr_ready(wr_ready), .wr_fill(wq_fill), .wr_z(wq_z), .wr_c8(cimg8),
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
    localparam S_IDLE = 5'd0,  S_TRI_INIT = 5'd1, S_ROW = 5'd2,  S_SETUP = 5'd3,
               S_AAINIT = 5'd4, S_AASUB = 5'd5, S_AASPAN = 5'd8,
               S_PIX = 5'd6,   S_FIN = 5'd7,      S_NEXT = 5'd10,
               S_FILL = 5'd12, S_LOAD = 5'd13,    S_SYNC = 5'd14, S_RECT_INIT = 5'd15,
               S_LOAD_INIT = 5'd16, S_VSET = 5'd18,
               S_XL = 5'd19;
    reg [4:0]  st;
    reg        row_wq;              // the row's write-back has been asked for
    // the row's next pixel: its word in the span, and whether what it reads of
    // memory is in the old buffers
    wire [9:0] pix_wi = ({9'd0, xs[0]} + px_cnt) >> 1;
    wire       pix_ok = !rq_pend && h_done && (!z_cmp || z_have > pix_wi) && (!need_mem || c_have > pix_wi);
    // a pixel that goes through the texture stages twice (rdp_pix two_pass) is issued every second clock:
    // pix_valid is still up the clock after an issue
    wire       pix_go = pix_ok && !(two_pass && pix_valid);
    // nothing of the primitive is under way any more
    wire       all_quiet = !pix_busy && !pix_valid && !px_valid && !msk_we
                         && !rq_pend && !rd_busy && !wq_pend && !wr_busy;
    reg [4:0]  mi;                  // multiplier sequence index
    reg        kind_tri, kind_texrect, kind_fillmode, kind_rect;   // what is being drawn
    reg signed [11:0] y, y0r, ymr, yb;
    reg signed [11:0] ry0;         // rect / texrect top row
    reg [9:0]  rx0, xa, xb;        // rect columns [xa, xb), texrect origin rx0
    reg [1:0]  rxf;                // XH's quarter pixels (outside copy mode: the column-stepped attribute's origin)
    reg [31:0] exh;                 // the major edge's x at the row's top (the attributes' reference)
    // S_VSET: attribute vi (R G B A, S T W, Z) out of the staging RAM. The
    // N64 sends a group's four (shade, texture) as 16 words -- value, DaDx,
    // DaDe, DaDy, each as two words of four 16-bit integer halves and, four
    // words on, their fractions; Z as four whole words.
    reg [2:0]  vi;
    reg [3:0]  vs;
    reg [31:0] vgx, vde, vval;
    // Z's DzDy too (the walker has no use for it): with DzDx, the depth a
    // pixel's step moves it -- all ones under the top bit of |DzDx| + |DzDy|
    // in the stored depth's units -- is a decal's tolerance (rdp_pix)
    reg [31:0] zdy;
    reg [15:0] z_tol;
    wire [16:0] zt_x = vgx[31] ? ~vgx[31:15] : vgx[31:15];
    wire [16:0] zt_y = zdy[31] ? ~zdy[31:15] : zdy[31:15];
    wire [17:0] zt_s = {1'b0, zt_x} + {1'b0, zt_y};
    reg  [15:0] zt_m;
    integer zi;
    always @* begin
        zt_m[15] = |zt_s[17:15];
        for (zi = 14; zi >= 0; zi = zi - 1) zt_m[zi] = zt_m[zi + 1] | zt_s[zi];
        zt_m[0] = 1'b1;
    end
    reg [2:0]  tflags;              // the triangle's shade / texture / z
    wire [5:0]  vgb = (vi < 3'd4) ? 6'd0 : (vi < 3'd7) ? (tflags[2] ? 6'd16 : 6'd0)
                    : (tflags[2] ? 6'd16 : 6'd0) + (tflags[1] ? 6'd16 : 6'd0);
    wire        vz = vi == 3'd7;
    wire [15:0] vhalf = vi[0] ? vr_q[15:0] : vr_q[31:16];      // (R, B, S, W: the first half)
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
    function automatic attr_on;             // mrdp_attr_on()
        input [2:0] fl;
        input [2:0] a;
        attr_on = a < 3'd4 ? fl[2] : a < 3'd7 ? fl[1] : fl[0];
    endfunction
    reg [31:0] row_off;            // y * width * 2 (color / Z images; an 8-bit colour image's y * width)
    reg [9:0]  xs, xe_m1;
    reg [10:0] xe;
    reg [9:0]  px_cnt;
    // loads (lang/c/mrdp/mrdp.c load_block, load_tlut, load_tile_n64)
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
        reg [15:0] bytes, b7;
        reg [12:0] w;
        begin
            bytes = (({3'd0, n} << sz) + 16'd1) >> 1;
            b7 = bytes + 16'd7;
            w = b7[15:3];
            load_words = (w >= 13'd512) ? 11'd1024 : {w[9:0], 1'b0};
        end
    endfunction

    // sign helpers
    wire signed [13:0] yh_s = t_yh, ym_s = t_ym, yl_s = t_yl;
    // the last row + 1: every row a subscanline above YL reaches
    wire signed [13:0] yl_end_aa = ($signed(t_yl) + 14'sd3) >>> 2;
    wire signed [11:0] yb_aa = (yl_end_aa < $signed({2'b0, sc_y1})) ? yl_end_aa[11:0] : $signed({1'b0, sc_y1});

    // ---- AA_EN: the N64's walker (lang/c/mrdp/mrdp.c draw_triangle_aa). The
    // edges at subscanline aa_j: three running edges, set up at the first
    // row's top (S_AAINIT) and stepped (DxDy >> 2) & ~1 a subscanline. A
    // subscanline a clock (S_AASUB): its edges in eighths with the sticky
    // bit, clamped to the scissor (stage 1), then shifted into the row's four
    // (stage 2) with the span's union as a running min / max.
    reg  [31:0] ax_h, ax_m, ax_l;
    reg  [2:0]  aa_j;
    wire [31:0] aq_h = asr(t_dxh, 5'd2) & ~32'd1, aq_m = asr(t_dxm, 5'd2) & ~32'd1, aq_l = asr(t_dxl, 5'd2) & ~32'd1;
    // S_AAINIT: an edge's start, X & ~1 (XL as S_XL left it) plus the product (one adder for the three)
    wire [31:0] aa_init = ((mi == 5'd2) ? (t_xh & ~32'd1) : (mi == 5'd3) ? (t_xm & ~32'd1) : t_xl) + mul_p[31:0];
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
            aa_eql[jj] = aa_px == aa_lp[10*jj +: 10];
            aa_eqr[jj] = aa_px == aa_rp[10*jj +: 10];
            aa_cj[jj] = 4'd0;
            if (aa_v[jj]) begin
                if (aa_eql[jj]) aa_cj[jj] = aa_eqr[jj] ? (aa_lm[4*jj +: 4] & aa_rm[4*jj +: 4]) : aa_lm[4*jj +: 4];
                else if (aa_eqr[jj]) aa_cj[jj] = aa_rm[4*jj +: 4];
                else if (aa_pl[jj] && !aa_pr[jj]) aa_cj[jj] = jj[0] ? 4'h5 : 4'hA;
            end
        end
    end
    wire [7:0]  aa_mask = {aa_cj[0] | aa_cj[1], aa_cj[2] | aa_cj[3]};
    // the span: the subscanlines' union, its end to the scissor
    wire [10:0] aa_xe = ({1'b0, aa_max} + 11'd1 < {1'b0, sc_x1}) ? {1'b0, aa_max} + 11'd1 : {1'b0, sc_x1};

    // arithmetic shift right, evaluated on its own: inside a wider expression
    // with any unsigned operand, $signed(v) >>> n is a LOGICAL shift
    function automatic [31:0] asr;
        input [31:0] v;
        input [4:0]  n;
        asr = $signed(v) >>> n;
    endfunction

    function automatic [31:0] sx12;   // sign-extend a 12-bit row / column index
        input [11:0] v;
        sx12 = {{20{v[11]}}, v};
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
    reg         es_done;            // this row's step finished
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
    wire tinit_w = st == S_TRI_INIT && mi >= 5'd5 && mi <= 5'd12;
    wire step_w = !es[3];
    wire [2:0]  e_wa = vset_w ? vi : tinit_w ? mi[2:0] - 3'd5 : es[2:0];
    // (TRI_INIT's write of e + k DaDe reads e where it writes it, as the product comes -- two clocks after DaDe was
    // read at ea for it; one adder for that and for the row step's e + DaDe)
    wire [31:0] e_q = e_ram[tinit_w ? e_wa : ea], ade_q = ade_ram[ea];
    wire [31:0] e_add = e_q + (tinit_w ? mul_p[31:0] : ade_q);
    wire [31:0] e_wd = vset_w ? (vset_on ? vval : 32'd0) : e_add;
    always @(posedge clk) begin
        if (vset_w || tinit_w || step_w) e_ram[e_wa] <= e_wd;
        if (vset_w) ade_ram[vi] <= vset_on ? vde : 32'd0;
    end

    // AA_EN: the subscanline stages
    always @(posedge clk) begin
        aa_s1 <= st == S_AASUB && !aa_j[2];
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
        end else if (st == S_ROW) aa_any <= 1'b0;
        // the span's progress past the ends (S_PIX: a pixel issued)
        if (st == S_AASPAN) begin aa_pl <= 4'd0; aa_pr <= 4'd0; end
        else if (st == S_PIX && row_wq && pix_go) begin aa_pl <= aa_pl | aa_eql; aa_pr <= aa_pr | aa_eqr; end
    end

    integer i;
    reg [31:0] dxs;          // span start offset from the H edge, 16.16
    always @(posedge clk) begin
        zn_we <= 1'b0; cn_we <= 1'b0; msk_we <= 1'b0;
        pix_valid <= 1'b0;
        sync_ev <= 1'b0;
        if (rst) begin
            st <= S_IDLE;
            es <= 4'd8; es_done <= 1'b0;
            collecting <= 1'b0;
            exec_go <= 1'b0;
            busy_exec <= 1'b0;
            rq_pend <= 1'b0; wq_pend <= 1'b0; row_wq <= 1'b0; set_cur <= 1'b0;
            res_n[0] <= 11'd0; res_n[1] <= 11'd0; res_d[0] <= 11'd0; res_d[1] <= 11'd0; res_q[0] <= 11'd0; res_q[1] <= 11'd0;
            old_hold[0] <= 4'd0; old_hold[1] <= 4'd0;
            sync_count <= 0; load_count <= 0; unknown_ops <= 0;
            other_h <= 0; other_l <= 0; cc_hi <= 0; cc_lo <= 0;
            fill <= 0; fog <= 0; blend <= 0; prim <= 0; env <= 0; prim_lod_frac <= 0; k4 <= 0; k5 <= 0; cv_k03 <= 0; prim_z <= 0;
            sc_xh <= 0; sc_yh <= 0; sc_xl <= 12'd4092; sc_yl <= 12'd4092;
            cimg <= 0; zimg <= 0; timg <= 0; cimg_width <= 11'd1; timg_width <= 11'd1;
            timg_size <= 2'd0; cimg_ok <= 1'b0; cimg8 <= 1'b0;
        end else begin
            // ---- the engines take their requests
            if (rq_pend && rd_ready && old_hold[rq_set] == 4'd0 && hz_ok) rq_pend <= 1'b0;
            if (wq_pend && wr_ready) wq_pend <= 1'b0;
            if (old_hold[0] != 4'd0) old_hold[0] <= old_hold[0] - 4'd1;
            if (old_hold[1] != 4'd0) old_hold[1] <= old_hold[1] - 4'd1;

            // ---- pixel results into the new buffers
            res_d[0] <= res_n[0]; res_d[1] <= res_n[1];
            res_q[0] <= res_d[0]; res_q[1] <= res_d[1];
            if (px_valid) begin
                msk_we <= 1'b1;
                nb_wa <= {px_set, px_x[9:1]};
                msk_be <= px_x[0] ? 2'b10 : 2'b01;
                cm_wd <= {2{px_cpass}};
                zm_wd <= {2{px_zpass}};
                cn_wd <= cimg8 ? {2{px_r8, 10'd0}} : {px_color, px_hid, px_color, px_hid};
                zn_wd <= {px_depth, px_depth};
                nb_be <= px_x[0] ? 2'b01 : 2'b10;       // the even pixel in [31:16]
                cn_we <= px_cpass;
                zn_we <= px_zpass;
                res_n[px_set] <= res_n[px_set] + 11'd1;
            end

            // ---- command intake
            if (cmd_valid && cmd_ready) begin
                if (!collecting) begin
                    w0 <= cmd_data;
                    nwords <= cmd_len(cmd_data);
                    widx <= 6'd1;
                    collecting <= 1'b1;
                    if (op0[5:3] == 3'b001) begin
                        t_lft <= cmd_data[23];
                        t_tile <= cmd_data[18:16];
                        t_yl <= cmd_data[13:0];
                        for (i = 0; i < 8; i = i + 1) adx[i] <= 0;
                    end
                end else begin
                    widx <= widx + 6'd1;
                    if (widx == 6'd1) w1 <= cmd_data;
                    if (widx == 6'd2) t_xl <= cmd_data;     // triangle XL, or texrect S T
                    if (widx == 6'd3) t_dxl <= cmd_data;    // triangle DxLDy, or DsDx DtDy
                    if (is_tri) begin
                        case (widx)
                            6'd1: begin t_ym <= cmd_data[29:16]; t_yh <= cmd_data[13:0]; end
                            6'd4: t_xh <= cmd_data;  6'd5: t_dxh <= cmd_data;
                            6'd6: t_xm <= cmd_data;  6'd7: t_dxm <= cmd_data;
                        endcase
                    end
                    if (widx + 6'd1 == nwords) begin
                        collecting <= 1'b0;
                        exec_go <= 1'b1;
                    end
                end
            end

            // ---- the row step of e[] (one attribute a clock, see es_*)
            if (!es[3]) begin
                es <= es + 4'd1;
                if (es == 4'd7) es_done <= 1'b1;
            end

            // ---- execute
            case (st)
            S_IDLE: if (exec_go) begin
                exec_go <= 1'b0;
                busy_exec <= 1'b1;
                if ((is_tri || op == 6'h24 || op == 6'h25 || op == 6'h36) && !cimg_ok) begin
                    busy_exec <= 1'b0;          // a 32-bit colour image: nothing drawn
                end else if (is_tri) begin
                    kind_tri <= 1'b1; kind_texrect <= 1'b0; kind_rect <= 1'b0; kind_fillmode <= 1'b0;
                    tflags <= op[2:0];
                    p_shade <= op[2]; p_tex <= op[1]; p_zbuf <= op[0]; p_direct <= 1'b0; p_tile <= t_tile;
                    y0r <= $signed(t_yh) >>> 2;
                    ymr <= $signed(t_ym) >>> 2;
                    p_aa <= 1'b1;               // every triangle is walked as the N64 walks it
                    p_aaen <= other_l[3];       // ... drawn where any sample is covered (AA_EN), or sample 7 is
                    // first row: max(y0, scissor); rows end at min((yl + 3) >> 2, scissor)
                    y <= (($signed(t_yh) >>> 2) > $signed({2'b0, sc_y0})) ? ($signed(t_yh) >>> 2) : $signed({2'b0, sc_y0});
                    yb <= yb_aa;
                    mi <= 5'd0;
                    vi <= 3'd0; vs <= 4'd0;
                    st <= S_XL;
                end else begin
                    case (op)
                    6'h00, 6'h26, 6'h27, 6'h28: begin busy_exec <= 1'b0; end
                    6'h29: st <= S_SYNC;
                    6'h24, 6'h25, 6'h36: begin
                        // rectangles: texrect (w0: XL YL, w1: tile XH YH, then S T, DsDx DtDy in t_xl, t_dxl)
                        kind_tri <= 1'b0;
                        p_aa <= 1'b0; p_aaen <= 1'b0;
                        kind_texrect <= op != 6'h36;
                        kind_fillmode <= op == 6'h36 && cyc == 2'd3;
                        kind_rect <= 1'b1;
                        p_shade <= 1'b0; p_tex <= op != 6'h36; p_zbuf <= 1'b0; p_direct <= 1'b1;
                        p_tile <= w1[26:24];
                        rx0 <= w1[23:14];
                        rxf <= (cyc != 2'd2) ? w1[13:12] : 2'd0;
                        ry0 <= {2'b00, w1[11:2]};
                        begin : rect_bounds
                            // copy mode (texrect), fill mode (fill rectangle): the corners' whole pixels, the lower
                            // right included. Otherwise the pixels whose top-left sample is inside, 4x in [XH, XL)
                            // and 4y in [YH, YL): each corner rounded up to a whole pixel
                            reg [10:0] x0c, y0c, x1c, y1c;
                            reg [10:0] xa_c, ya_c;
                            reg incl;
                            incl = (op == 6'h36) ? (cyc == 2'd3) : (cyc == 2'd2);
                            x0c = incl ? {1'b0, w1[23:14]} : ({1'b0, w1[23:12]} + 13'd3) >> 2;
                            y0c = incl ? {1'b0, w1[11:2]}  : ({1'b0, w1[11:0]}  + 13'd3) >> 2;
                            x1c = incl ? {1'b0, w0[23:14]} + 11'd1 : ({1'b0, w0[23:12]} + 13'd3) >> 2;
                            y1c = incl ? {1'b0, w0[11:2]}  + 11'd1 : ({1'b0, w0[11:0]}  + 13'd3) >> 2;
                            xa_c = x0c > {1'b0, sc_x0} ? x0c : {1'b0, sc_x0};
                            ya_c = y0c > {1'b0, sc_y0} ? y0c : {1'b0, sc_y0};
                            xa <= xa_c[9:0];
                            xb <= (x1c < {1'b0, sc_x1}) ? x1c[9:0] : sc_x1;
                            y <= {1'b0, ya_c};
                            yb <= (y1c < {1'b0, sc_y1}) ? {1'b0, y1c} : {2'b00, sc_y1};
                            if (((x1c < {1'b0, sc_x1}) ? x1c : {1'b0, sc_x1}) <= xa_c)
                                yb <= {1'b0, ya_c};   // empty
                        end
                        mi <= 5'd0;
                        st <= S_RECT_INIT;
                    end
                    6'h2C: begin cv_k03 <= {w0[21:0], w1[31:18]}; k4 <= w1[17:9]; k5 <= w1[8:0]; busy_exec <= 1'b0; end
                    6'h2D: begin sc_xh <= w0[23:12]; sc_yh <= w0[11:0]; sc_xl <= w1[23:12]; sc_yl <= w1[11:0]; busy_exec <= 1'b0; end
                    6'h2E: begin prim_z <= {w1[30:16], 1'b0}; prim_dz <= w1[15:0]; busy_exec <= 1'b0; end      // the N64's: 15 bits, and its delta
                    6'h2F: begin other_h <= w0[23:0]; other_l <= w1; busy_exec <= 1'b0; end
                    6'h32: begin
                        tl_size[w1[26:24]] <= {w0[23:12], w0[11:0], w1[23:12], w1[11:0]};
                        busy_exec <= 1'b0;
                    end
                    // loads: the tile's TMEM address and format reach S_LOAD_INIT through ts_*
                    6'h30, 6'h33, 6'h34: begin
                        if (op == 6'h34) tl_size[w1[26:24]] <= {w0[23:12], w0[11:0], w1[23:12], w1[11:0]};
                        p_tile <= w1[26:24];
                        l_kind <= (op == 6'h33) ? 2'd0 : (op == 6'h34) ? 2'd1 : 2'd2;
                        mi <= 5'd0;
                        st <= S_LOAD_INIT;
                    end
                    6'h35: begin
                        tl_attr[w1[26:24]] <= {w0[23:21], w0[20:19], w1[23:20], w0[17:9], w0[8:0],
                                               w1[9], w1[8], w1[19], w1[18],
                                               w1[7:4], w1[3:0], w1[17:14], w1[13:10]};
                        busy_exec <= 1'b0;
                    end
                    6'h37: begin fill <= w1; busy_exec <= 1'b0; end
                    6'h38: begin fog <= w1; busy_exec <= 1'b0; end
                    6'h39: begin blend <= w1; busy_exec <= 1'b0; end
                    6'h3A: begin prim <= w1; prim_lod_frac <= w0[7:0]; busy_exec <= 1'b0; end
                    6'h3B: begin env <= w1; busy_exec <= 1'b0; end
                    6'h3C: begin cc_hi <= w0[23:0]; cc_lo <= w1; busy_exec <= 1'b0; end
                    6'h3D: begin timg <= w1; timg_width <= {1'b0, w0[9:0]} + 11'd1; timg_size <= w0[20:19]; busy_exec <= 1'b0; end
                    6'h3E: begin zimg <= w1; busy_exec <= 1'b0; end
                    6'h3F: begin
                        cimg <= w1; cimg_width <= {1'b0, w0[9:0]} + 11'd1;
                        cimg_ok <= w0[20:19] == 2'd2 || w0[20:19] == 2'd1; cimg8 <= w0[20:19] == 2'd1;
                        busy_exec <= 1'b0;
                    end
                    default: begin unknown_ops <= unknown_ops + 16'd1; busy_exec <= 1'b0; end
                    endcase
                end
            end

            // ---- the N64's XL is the low edge at YM itself, not at YM's
            // scanline as XH and XM are at YH's: back by the quarter rows between
            // (AA_EN: XL & ~1, back by the subscanline step (DxLDy >> 2) & ~1)
            S_XL: begin
                if (mi == 5'd0) begin mul_a <= {30'd0, t_ym[1:0]}; mul_b <= aq_l; end
                if (mi == 5'd2) begin t_xl <= (t_xl & ~32'd1) - mul_p[31:0]; st <= S_VSET; end
                mi <= (mi == 5'd2) ? 5'd0 : mi + 5'd1;
            end

            // ---- an attribute's value, DaDx, DaDe from the staging RAM (the
            // data two steps after the address)
            S_VSET: begin
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
                        if (vz) vval <= vr_q; else vval[31:16] <= vhalf;
                    end
                    4'd3: begin
                        vr_ra <= vz ? vgb + 6'd3 : vgb + voff(3'd3, vz) + {5'd0, vi[1]}; vs <= 4'd4;
                        if (vz) vgx <= vr_q; else vval[15:0] <= vhalf;
                    end
                    4'd4: begin
                        vr_ra <= vgb + voff(3'd4, vz) + {5'd0, vi[1]};
                        if (vz) begin vde <= vr_q; vs <= 4'd5; end
                        else begin vgx[31:16] <= vhalf; vs <= 4'd5; end
                    end
                    4'd5: if (vz) begin zdy <= vr_q; vs <= 4'd15; end
                          else begin vr_ra <= vgb + voff(3'd5, vz) + {5'd0, vi[1]}; vgx[15:0] <= vhalf; vs <= 4'd6; end
                    4'd6: begin vde[31:16] <= vhalf; vs <= 4'd7; end
                    4'd7: begin vde[15:0] <= vhalf; vs <= 4'd15; end
                    default: begin                   // 15: the attribute's registers
                        adx[vi] <= vgx;             // e[vi], ade[vi]: the RAM writes below
                        if (vz) z_tol <= zt_m;
                        vs <= 4'd0; vi <= vi + 3'd1;
                        if (vi == 3'd7) st <= S_TRI_INIT;
                    end
                endcase
            end

            // ---- triangle: edge and attribute values at the first row, and
            // that row's offset into the images (y * width * 2; an 8-bit image's y * width)
            S_TRI_INIT: begin
                begin : tri_init
                    reg [31:0] k;
                    k = rdiff(y, y0r);
                    case (mi)
                        5'd0: begin mul_a <= k; mul_b <= t_dxh; end
                        5'd1: begin mul_a <= k; mul_b <= t_dxm; end
                        5'd2: begin mul_a <= rdiff(y, ymr); mul_b <= t_dxl; end
                        5'd3, 5'd4, 5'd5, 5'd6, 5'd7, 5'd8, 5'd9, 5'd10:
                              begin mul_a <= k; mul_b <= ade_q; end       // ea = mi - 3
                        5'd11: begin mul_a <= sx12(y); mul_b <= {21'd0, cimg_width}; end
                        default: ;
                    endcase
                    // products come back two cycles later
                    case (mi)
                        5'd2:  exh <= t_xh + mul_p[31:0];
                        // 5..12: e[mi - 5] = e + k DaDe (the RAM writes below)
                        5'd13: row_off <= cimg8 ? mul_p[31:0] : {mul_p[30:0], 1'b0};
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
                    5'd0: begin mul_a <= rdiff(y, y0r); mul_b <= t_dxh & ~32'd7; end
                    5'd1: begin mul_a <= rdiff(y, y0r); mul_b <= t_dxm & ~32'd7; end
                    5'd2: begin mul_a <= rdiff(y, ymr); mul_b <= t_dxl & ~32'd7; ax_h <= aa_init; end
                    5'd3: ax_m <= aa_init;
                    default: begin ax_l <= aa_init; st <= S_ROW; end
                endcase
                mi <= mi + 5'd1;
            end

            // ---- a row: its span, or the next row
            S_ROW: begin
                if (y >= yb) begin
                    st <= S_FIN;
                end else if (kind_tri) begin
                    aa_j <= 3'd0;
                    aa_lo <= $signed({y, 2'b00}) < ym_s;
                    st <= S_AASUB;
                end else begin
                    // rectangles: columns fixed
                    xs <= xa;
                    xe <= {1'b0, xb};
                    mi <= 5'd0;
                    st <= kind_fillmode ? S_FILL : S_SETUP;
                    if (!kind_fillmode) begin
                        set_cur <= !set_cur;
                        rq_pend <= 1'b1; rq_load <= 1'b0; rq_z <= z_cmp; rq_c <= need_mem; rq_h <= !cimg8; rq_set <= !set_cur;
                        rq_zbase <= zimg + row_off; rq_cbase <= cimg + row_off; rq_x0 <= xa; rq_x1 <= {1'b0, xb};
                        rwb[!set_cur] <= cimg[11:2] + row_off[11:2];
                    end
                end
            end

            // ---- AA_EN: the row's four subscanlines (and a clock for the last's stage 2)
            S_AASUB: begin
                if (!aa_j[2]) begin
                    ax_h <= ax_h + aq_h; ax_m <= ax_m + aq_m; ax_l <= ax_l + aq_l;
                end
                aa_j <= aa_j + 3'd1;
                aa_lo <= (aa_yq + 14'sd1) < ym_s;
                if (aa_j == 3'd4) st <= S_AASPAN;
            end
            S_AASPAN: begin
                if (!aa_any || {1'b0, aa_min} >= aa_xe) st <= S_NEXT;
                else begin
                    xs <= aa_min;
                    xe <= aa_xe;
                    mi <= 5'd0;
                    st <= S_SETUP;
                    set_cur <= !set_cur;
                    rq_pend <= 1'b1; rq_load <= 1'b0; rq_z <= z_cmp; rq_c <= need_mem; rq_h <= !cimg8; rq_set <= !set_cur;
                    rq_zbase <= zimg + row_off; rq_cbase <= cimg + row_off; rq_x0 <= aa_min; rq_x1 <= aa_xe;
                    rwb[!set_cur] <= cimg[11:2] + row_off[11:2];
                end
            end

            // ---- span setup: cur = e + DaDx * (xs - xh), the plane at the pixel's
            // top-left corner (triangles),
            // the S/T accumulators (texrects)
            S_SETUP: begin
                if (kind_tri) begin
                    dxs = {xs, 16'd0} - exh;
                    if (mi < 5'd8) begin mul_a <= adx[mi[2:0]]; mul_b <= dxs; end
                    if (mi >= 5'd2 && mi < 5'd10)
                        cur[mi - 5'd2] <= e_q + mul_p[47:16];                // ea = mi - 2
                    mi <= mi + 5'd1;
                    if (mi == 5'd9) begin
                        st <= S_PIX;
                        es <= 4'd0;                 // e[] no longer needed this row: step it
                    end
                end else begin
                    // texrect: t_xl = S T, t_dxl = DsDx DtDy; copy mode steps 4x
                    begin : rect_setup
                        reg [2:0] ds;
                        reg signed [31:0] dsdx, dtdy, sv, tv, dcol, drow;
                        reg signed [10:0] dc11;
                        ds = (cyc == 2'd2) ? 3'd7 : 3'd5;
                        sv = {{16{t_xl[31]}}, t_xl[31:16]};   tv = {{16{t_xl[15]}}, t_xl[15:0]};
                        dsdx = {{16{t_dxl[31]}}, t_dxl[31:16]}; dtdy = {{16{t_dxl[15]}}, t_dxl[15:0]};
                        dc11 = $signed({1'b0, xs}) - $signed({1'b0, rx0});      // (11 and 13 bits, then extended)
                        // in quarter pixels, the products then over 4: the rows from YH's whole row, the columns
                        // from XH itself (4 (xs - XH's column) - XH's fraction; none in copy mode) -- the
                        // column-stepped attribute is the plane at XH
                        dcol = {{19{dc11[10]}}, dc11, 2'b00} - {30'd0, rxf};
                        drow = {rdiff(y, ry0), 2'b00};
                        case (mi)
                            5'd0: begin mul_a <= dsdx; mul_b <= (op == 6'h25) ? drow : dcol; end
                            5'd1: begin mul_a <= dtdy; mul_b <= (op == 6'h25) ? dcol : drow; end
                            default: ;
                        endcase
                        if (mi == 5'd2) cur[4] <= (sv <<< ds) + mul_p[33:2];
                        if (mi == 5'd3) cur[5] <= (tv <<< 5) + mul_p[33:2];
                        shS <= ds;
                        adx[4] <= (op == 6'h25 || op == 6'h36) ? 32'd0 : dsdx;
                        adx[5] <= (op == 6'h25) ? dtdy : 32'd0;
                        mi <= mi + 5'd1;
                        if (mi == 5'd3) st <= S_PIX;
                    end
                end
                px_cnt <= 10'd0;
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
                        wq_cbase <= cimg + row_off; wq_zbase <= zimg + row_off; wq_x0 <= xs; wq_x1 <= xe;
                        res_n[set_cur] <= 11'd0; res_d[set_cur] <= 11'd0; res_q[set_cur] <= 11'd0;
                        row_wq <= 1'b1;
                    end
                end else if (pix_go) begin
                    pix_valid <= 1'b1;
                    pix_x <= xs + px_cnt;
                    pix_mask <= !p_aa ? 8'hFF : (p_aaen || aa_mask[7]) ? aa_mask : 8'h00;   // (no AA_EN: sample 7 or nothing)
                    if (px_cnt != 10'd0) begin
                        for (i = 0; i < 8; i = i + 1) cur[i] <= cur[i] + adx[i];
                    end
                    px_cnt <= px_cnt + 10'd1;
                    if ({1'b0, xs} + {1'b0, px_cnt} + 11'd1 == xe) begin
                        st <= S_NEXT;
                        row_wq <= 1'b0;
                        old_hold[set_cur] <= two_pass ? 4'd15 : 4'd14;
                    end
                end
            end

            // ---- fill mode rectangles: straight to memory
            S_FILL: if (!wq_pend) begin
                wq_pend <= 1'b1; wq_fill <= 1'b1; wq_z <= 1'b0; wq_set <= 1'b0;
                wq_cbase <= cimg + row_off; wq_x0 <= xs; wq_x1 <= xe;
                st <= S_NEXT;
            end

            // ---- the primitive's end: its pixels out of the pipe and back
            // in memory before anything else (the next may change the state
            // they are drawn with, or read what they write)
            S_FIN: if (all_quiet) begin
                st <= S_IDLE;
                busy_exec <= 1'b0;
            end

            // ---- next row
            // (a triangle row waits for e[] += ade[]: behind the span since
            // its setup, or here when the row had none)
            S_NEXT: if (!kind_tri || es_done) begin
                y <= y + 12'sd1;
                row_off <= row_off + (cimg8 ? {21'd0, cimg_width} : {20'd0, cimg_width, 1'b0});
                exh <= exh + t_dxh;
                es_done <= 1'b0;
                st <= S_ROW;
            end else if (es == 4'd8) es <= 4'd0;

            // ---- rectangles: row offset of the first row
            S_RECT_INIT: begin
                if (mi == 5'd0) begin mul_a <= sx12(y); mul_b <= {21'd0, cimg_width}; end
                if (mi == 5'd2) begin row_off <= cimg8 ? mul_p[31:0] : {mul_p[30:0], 1'b0}; st <= S_ROW; end
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
                    if (mi == 5'd0) begin
                        mul_a <= (l_kind == 2'd0) ? {20'd0, w0[11:0]} : {22'd0, t0};
                        mul_b <= {21'd0, timg_width};
                    end
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
                    rq_pend <= 1'b1; rq_load <= 1'b1; rq_z <= 1'b0; rq_c <= 1'b0; rq_h <= 1'b0; rq_set <= 1'b0;
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
        end
    end


    assign idle = !busy_exec && !exec_go && !collecting && all_quiet && !mem_wpend;
endmodule
