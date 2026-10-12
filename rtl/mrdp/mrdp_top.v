// MRDP -- Mirlo's N64-style rasterizer. The contract is the C
// model, lang/c/mrdp/mrdp.c: for the same command stream the RTL writes the
// same memory.
//
// Commands are executed one at a time. A primitive is drawn row by row: the
// row's Z and color spans are read into on-chip line buffers, the pixels run
// through mrdp_pix at one a clock, then the span is written back (only the
// pixels that passed). State commands wait for the previous primitive to
// finish, so the pixel pipeline never sees a state change mid-flight.
module mrdp_top (
    input  wire        clk,
    input  wire        rst,

    // command words (the wrapper's FIFO)
    input  wire        cmd_valid,
    output wire        cmd_ready,
    input  wire [31:0] cmd_data,

    // SDRAM (LiteDRAM native style, byte addresses)
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
    output reg  [31:0] load_count,      // LOAD TILEs completed
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
    reg [15:0] prim_z;
    reg [11:0] sc_xh, sc_yh, sc_xl, sc_yl;
    reg [31:0] cimg, zimg, timg;
    reg [10:0] cimg_width, timg_width;

    // The 8 tiles, in two small RAMs (SET_TILE writes the format half,
    // SET_TILESIZE and LOAD_TILE the size half): as flip-flops they were 688
    // registers plus their write decode and an 8-way read mux. Read through
    // one registered port at p_tile (ts_* below).
    //   tl_attr: {fmt 3, line 9, tmem 10, cs, ms, ct, mt, masks 4, shifts 4, maskt 4, shiftt 4}
    //   tl_size: {uls 12, ult 12, lrs 12, lrt 12}
    (* ramstyle = "MLAB, no_rw_check" *) reg [41:0] tl_attr [0:7];
    (* ramstyle = "MLAB, no_rw_check" *) reg [47:0] tl_size [0:7];
    integer ti;
    initial for (ti = 0; ti < 8; ti = ti + 1) begin tl_attr[ti] = 42'd0; tl_size[ti] = 48'd0; end

    wire [1:0] cyc = other_h[21:20];
    wire [9:0] sc_x0 = sc_xh[11:2], sc_x1 = sc_xl[11:2];
    wire [9:0] sc_y0 = sc_yh[11:2], sc_y1 = sc_yl[11:2];

    // does the blender read memory (the color span must be loaded)
    wire rd_mem0 = other_l[31:30] == 2'd1 || other_l[23:22] == 2'd1;
    wire rd_mem1 = other_l[29:28] == 2'd1 || other_l[21:20] == 2'd1;
    wire need_mem = (cyc == 2'd1) ? (rd_mem0 | rd_mem1) : (cyc == 2'd0) ? rd_mem0 : 1'b0;
    wire z_cmp = other_l[4] && cyc != 2'd3;
    wire z_upd = other_l[5] && cyc != 2'd3;

    // ======================================================================
    // command intake
    // ======================================================================
    reg [31:0] w0;                 // first word of the command
    reg [5:0]  nwords, widx;       // total words, index of the next
    reg        collecting;
    reg        exec_go;            // command complete: execute
    reg        busy_exec;          // executing (FSM not idle)
    reg [31:0] w1;                 // second word

    // triangle coefficients, decoded as they arrive
    reg        t_lft;
    // TRI_V: the attributes' vertex values (3 words each) at 0..23 of a
    // staging RAM, the plane factors F0..F3 and kx, ky at 24..29 -- read
    // back as the multiplier needs them, not held in flip-flops (area)
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
        reg [5:0] op, t;
        begin
            op = w[29:24];
            if (op == 6'h20) begin                           // TRI_R: 7 + 3 per attribute, even
                t = 6'd7 + (w[21] ? 6'd12 : 6'd0) + (w[20] ? 6'd9 : 6'd0) + (w[19] ? 6'd3 : 6'd0);
                cmd_len = t + {5'd0, t[0]};
            end else if (op[5:3] == 3'b001)
                cmd_len = 6'd8 + (op[2] ? 6'd16 : 6'd0) + (op[1] ? 6'd16 : 6'd0) + (op[0] ? 6'd4 : 6'd0);
            else if (op[5:4] == 2'b01) begin                  // TRI_V 14, TRI_G 8, + 3 per attribute, even
                t = (op[3] ? 6'd8 : 6'd14) + (op[2] ? 6'd12 : 6'd0) + (op[1] ? 6'd9 : 6'd0) + (op[0] ? 6'd3 : 6'd0);
                cmd_len = t + {5'd0, t[0]};
            end else if (op == 6'h24 || op == 6'h25)
                cmd_len = 6'd4;
            else
                cmd_len = 6'd2;
        end
    endfunction

    wire [5:0] op0 = cmd_data[29:24];
    wire [5:0] op  = w0[29:24];
    // Triangles are TRI_V (gradients computed here) and TRI_G (gradients
    // sent) -- lang/c/mrdp/mrdp_setup.h. N64-form triangles (0x08..0x0F) are
    // skipped as unknown: their half-word attribute layout cost more logic
    // than both of these.
    wire       is_triv = op[5:3] == 3'b010;
    wire       is_trig = op[5:3] == 3'b011;
    wire       is_trir = op == 6'h20;           // TRI_R: the whole setup here (S_RSET)
    wire       is_tri = is_triv | is_trig | is_trir;
    assign cmd_ready = !busy_exec && !exec_go;


    // ======================================================================
    // RAMs
    // ======================================================================
    // TMEM: 4 banks of 1024 texels
    wire [9:0]  tm_ra0, tm_ra1, tm_ra2, tm_ra3;
    wire [15:0] tm_rd0, tm_rd1, tm_rd2, tm_rd3;
    wire [3:0]  tm_we;
    wire [9:0]  tm_wa0, tm_wa1, tm_wa2, tm_wa3;
    wire [15:0] tm_wd0, tm_wd1, tm_wd2, tm_wd3;
    mrdp_sdpram #(.AW(10), .DW(16), .NB(1)) tmem0 (.clk(clk), .we(tm_we[0]), .waddr(tm_wa0), .wdata(tm_wd0), .wbe(1'b1), .raddr(tm_ra0), .rdata(tm_rd0));
    mrdp_sdpram #(.AW(10), .DW(16), .NB(1)) tmem1 (.clk(clk), .we(tm_we[1]), .waddr(tm_wa1), .wdata(tm_wd1), .wbe(1'b1), .raddr(tm_ra1), .rdata(tm_rd1));
    mrdp_sdpram #(.AW(10), .DW(16), .NB(1)) tmem2 (.clk(clk), .we(tm_we[2]), .waddr(tm_wa2), .wdata(tm_wd2), .wbe(1'b1), .raddr(tm_ra2), .rdata(tm_rd2));
    mrdp_sdpram #(.AW(10), .DW(16), .NB(1)) tmem3 (.clk(clk), .we(tm_we[3]), .waddr(tm_wa3), .wdata(tm_wd3), .wbe(1'b1), .raddr(tm_ra3), .rdata(tm_rd3));

    // span line buffers and their pass masks (512 words of two pixels)
    reg         zlb_we, clb_we;
    reg  [8:0]  lb_wa;
    reg  [31:0] zlb_wd, clb_wd;
    reg  [1:0]  zlb_be, clb_be;
    wire [8:0]  lb_ra;
    wire [31:0] zlb_rd, clb_rd;
    mrdp_sdpram #(.AW(9), .DW(32), .NB(2)) zlb (.clk(clk), .we(zlb_we), .waddr(lb_wa), .wdata(zlb_wd), .wbe(zlb_be), .raddr(lb_ra), .rdata(zlb_rd));
    mrdp_sdpram #(.AW(9), .DW(32), .NB(2)) clb (.clk(clk), .we(clb_we), .waddr(lb_wa), .wdata(clb_wd), .wbe(clb_be), .raddr(lb_ra), .rdata(clb_rd));
    reg         msk_we;
    reg  [8:0]  msk_wa;
    reg  [1:0]  zm_wd, cm_wd, msk_be;
    wire [1:0]  zm_rd, cm_rd;
    mrdp_sdpram #(.AW(9), .DW(2), .NB(2)) zmsk (.clk(clk), .we(msk_we), .waddr(msk_wa), .wdata(zm_wd), .wbe(msk_be), .raddr(lb_ra), .rdata(zm_rd));
    // TRI_V's vertex values: word 14 + k of the command at k (<= 24 words)
    // TRI_G: word 8 + k at k
    // TRI_R: vertices x0 y0 x1 y1 x2 y2 at 32..37, attributes from 0; the
    // engine writes the factors, kx, ky at 24..29 and the converted S, T, W
    wire        vr_we = cmd_valid && cmd_ready && collecting && is_tri && (is_trir ? widx >= 6'd1 : widx >= 6'd8);
    wire [5:0]  vr_wa6 = is_trir ? (widx <= 6'd6 ? widx + 6'd31 : widx - 6'd7)
                       : is_trig ? widx - 6'd8 : widx >= 6'd14 ? widx - 6'd14 : widx + 6'd16;   // TRI_V 8..13 -> 24..29
    reg         vw_we;                  // the engine's writes (never while a command arrives)
    reg  [5:0]  vw_wa;
    reg  [31:0] vw_wd;
    reg  [5:0]  vr_ra;
    wire [5:0]  vram_ra;            // vr_ra, or the microcode's read address
    wire [31:0] vr_q;
    mrdp_sdpram #(.AW(6), .DW(32), .NB(1)) vram (.clk(clk), .we(vr_we | vw_we), .waddr(vw_we ? vw_wa : vr_wa6),
                                                 .wdata(vw_we ? vw_wd : cmd_data), .wbe(1'b1), .raddr(vram_ra), .rdata(vr_q));
    mrdp_sdpram #(.AW(9), .DW(2), .NB(2)) cmsk (.clk(clk), .we(msk_we), .waddr(msk_wa), .wdata(cm_wd), .wbe(msk_be), .raddr(lb_ra), .rdata(cm_rd));

    // ======================================================================
    // the pixel pipeline
    // ======================================================================
    reg         p_shade, p_tex, p_zbuf, p_direct;
    reg  [2:0]  p_tile;
    reg         pix_valid;
    reg  [9:0]  pix_x;
    // AA_EN: the N64's coverage (lang/c/mrdp/mrdp.c draw_triangle_aa)
    // AA_EN: the crack grow
    localparam COVERAGE = 1'b0;
    reg         t_aa;               // this triangle has AA_EN
    reg  [2:0]  t_grow;             // TRI_R: the edges that grow (H, M, L; microcode GRW)
    reg  [15:0] g_h, g_m, g_l;      // their grow along a row, 16.16 (0: none)
    reg  [31:0] dzdy;               // dZ/dy at fixed x (DzDe - DzDx DxHDy)
    reg         pix_kill;           // no covered sample: through the pipe, no write
    reg         pix_part, pix_minor;  // < 8 / < 4 of the 8 samples covered
    reg  signed [2:0] pix_sx8, pix_sy8;   // the first covered sample, 1/8 px from the centre
    reg  [31:0] cur [0:7];          // iterated attributes (triangles)
    // texrects step S and T in cur[4], cur[5] by adx[4], adx[5] -- the
    // triangle's own S/T adders -- and pass s = cur[4] >>> shS, t = cur[5] >>> 5
    reg  [2:0]  shS;
    wire [31:0] pr_s = p_direct ? asr(cur[4], {2'b0, shS}) : cur[4];
    wire [31:0] pr_t = p_direct ? asr(cur[5], 5'd5) : cur[5];
    // The drawing tile's fields, registered: indexing tile[p_tile] straight
    // into the pixel pipe was the worst path of the first fit (-4 ns). p_tile
    // and the tiles change only between primitives, many cycles before the
    // first pixel.
    reg [2:0]  ts_fmt;
    reg [8:0]  ts_line;
    reg [9:0]  ts_tmem;
    reg        ts_cs, ts_ms, ts_ct, ts_mt;
    reg [3:0]  ts_masks, ts_shifts, ts_maskt, ts_shiftt;
    reg [11:0] ts_uls, ts_ult, ts_lrs, ts_lrt;
    always @(posedge clk) begin
        {ts_fmt, ts_line, ts_tmem, ts_cs, ts_ms, ts_ct, ts_mt, ts_masks, ts_shifts, ts_maskt, ts_shiftt} <= tl_attr[p_tile];
        {ts_uls, ts_ult, ts_lrs, ts_lrt} <= tl_size[p_tile];
    end
    wire [8:0]  pix_lb_ra;
    wire        px_valid, px_cpass, px_zpass, pix_busy;
    wire [9:0]  px_x;
    wire [15:0] px_color, px_depth;
    mrdp_pix pix (
        .clk(clk), .rst(rst),
        .other_h(other_h), .other_l(other_l), .cc_hi(cc_hi), .cc_lo(cc_lo),
        .prim(prim), .env(env), .fog(fog), .blend(blend),
        .prim_lod_frac(prim_lod_frac), .prim_z(prim_z), .fill(fill),
        .p_shade(p_shade), .p_tex(p_tex), .p_zbuf(p_zbuf), .p_direct(p_direct),
        .t_fmt(ts_fmt), .t_line(ts_line), .t_tmem(ts_tmem),
        .t_cs(ts_cs), .t_ms(ts_ms), .t_ct(ts_ct), .t_mt(ts_mt),
        .t_masks(ts_masks), .t_shifts(ts_shifts), .t_maskt(ts_maskt), .t_shiftt(ts_shiftt),
        .t_uls(ts_uls), .t_ult(ts_ult), .t_lrs(ts_lrs), .t_lrt(ts_lrt),
        .in_valid(pix_valid), .in_x(pix_x),
        .in_r(cur[0]), .in_g(cur[1]), .in_b(cur[2]), .in_a(cur[3]),
        .in_s(pr_s), .in_t(pr_t), .in_w(cur[6]), .in_z(cur[7]),
        .in_kill(pix_kill), .in_part(pix_part), .in_minor(pix_minor), .in_sx8(pix_sx8), .in_sy8(pix_sy8), .dzdx(adx[7]), .dzdy(dzdy),
        .tm_a0(tm_ra0), .tm_a1(tm_ra1), .tm_a2(tm_ra2), .tm_a3(tm_ra3),
        .tm_d0(tm_rd0), .tm_d1(tm_rd1), .tm_d2(tm_rd2), .tm_d3(tm_rd3),
        .lb_raddr(pix_lb_ra), .zlb_rdata(zlb_rd), .clb_rdata(clb_rd),
        .out_valid(px_valid), .out_x(px_x), .out_color(px_color), .out_depth(px_depth),
        .out_cpass(px_cpass), .out_zpass(px_zpass), .busy(pix_busy));

    // ======================================================================
    // the memory engine
    // ======================================================================
    reg         mr_valid;
    reg  [2:0]  mr_op;
    reg  [31:0] mr_base;
    reg  [9:0]  mr_x0;
    reg  [10:0] mr_x1;
    reg  [9:0]  mr_trow;
    wire        mr_ready, mem_busy, mem_wpend;
    wire        mw_z_we, mw_c_we;
    wire [8:0]  mw_addr, mem_lb_ra;
    wire [31:0] mw_data;
    mrdp_mem mem (
        .clk(clk), .rst(rst),
        .req_valid(mr_valid), .req_ready(mr_ready), .req_op(mr_op), .req_base(mr_base),
        .req_x0(mr_x0), .req_x1(mr_x1), .req_fill(fill), .req_trow(mr_trow),
        .req_tmem(ts_tmem), .req_line(ts_line),     // LOAD TILE: p_tile is its tile
        .lbw_z_we(mw_z_we), .lbw_c_we(mw_c_we), .lbw_addr(mw_addr), .lbw_data(mw_data),
        .lbr_addr(mem_lb_ra), .lbr_zdata(zlb_rd), .lbr_cdata(clb_rd), .lbr_zmask(zm_rd), .lbr_cmask(cm_rd),
        .tm_we(tm_we), .tm_waddr0(tm_wa0), .tm_waddr1(tm_wa1), .tm_waddr2(tm_wa2), .tm_waddr3(tm_wa3),
        .tm_wdata0(tm_wd0), .tm_wdata1(tm_wd1), .tm_wdata2(tm_wd2), .tm_wdata3(tm_wd3),
        .m_cmd_valid(m_cmd_valid), .m_cmd_ready(m_cmd_ready), .m_cmd_we(m_cmd_we), .m_cmd_addr(m_cmd_addr),
        .m_wdata_valid(m_wdata_valid), .m_wdata_ready(m_wdata_ready), .m_wdata(m_wdata), .m_wdata_we(m_wdata_we),
        .m_rdata_valid(m_rdata_valid), .m_rdata(m_rdata),
        .busy(mem_busy), .writes_pending(mem_wpend));

    // ======================================================================
    // shared multiplier: 32 x 32 signed, two cycles
    // ======================================================================
    reg  [31:0] mul_a, mul_b;
    reg  signed [63:0] mul_p;
    // the same operands as unsigned: a_u b_u = a_s b_s + 2^32 (a31 b_u + b31 a_u),
    // the correction registered with the product (one adder after it, not two)
    reg  [31:0] mul_corr;
    always @(posedge clk) begin
        mul_p <= $signed(mul_a) * $signed(mul_b);
        mul_corr <= (mul_a[31] ? mul_b : 32'd0) + (mul_b[31] ? mul_a : 32'd0);
    end
    wire [63:0] mul_pu = {mul_p[63:32] + mul_corr, mul_p[31:0]};

    // ======================================================================
    // the sequencer
    // ======================================================================
    localparam S_IDLE = 5'd0,  S_TRI_INIT = 5'd1, S_ROW = 5'd2,  S_SETUP = 5'd3,
               S_RDZ = 5'd4,   S_RDC = 5'd5,      S_PIX = 5'd6,  S_DRAIN = 5'd7,
               S_WRC = 5'd8,   S_WRZ = 5'd9,      S_NEXT = 5'd10, S_MEMWAIT = 5'd11,
               S_FILL = 5'd12, S_LOAD = 5'd13,    S_SYNC = 5'd14, S_RECT_INIT = 5'd15,
               S_LOAD_INIT = 5'd16, S_SPAN = 5'd17, S_VSET = 5'd18,
               S_RSET = 5'd19, S_AASUB = 5'd20;
    reg [4:0]  st, st_after;
    reg [4:0]  mi;                  // multiplier sequence index
    reg        kind_tri, kind_texrect, kind_fillmode, kind_rect;   // what is being drawn
    reg signed [11:0] y, y0r, ymr, yb;
    reg signed [11:0] ry0;         // rect / texrect top row
    reg [9:0]  rx0, xa, xb;        // rect columns [xa, xb), texrect origin rx0
    reg [31:0] exh, exm, exl;
    reg signed [32:0] xs_r, xe_r;   // this row's span before the scissor (S_ROW -> S_SPAN)
    // S_VSET (TRI_V): attribute vi from its vertex values va0..va2, the
    // arithmetic of mrdp_vattr() / mrdp_decal_z() in lang/c/mrdp/mrdp_setup.h
    reg [2:0]  vi;
    reg [3:0]  vs;
    reg [4:0]  vbase;
    reg [31:0] va0, vda1, vda2, vgx, vgy, vde, vval;
    reg signed [63:0] vacc;
    reg [5:0]  vexp;                // the gradients' exponent: TRI_V 30, TRI_R its own
    reg [2:0]  tflags;              // the triangle's shade / texture / z
    reg signed [63:0] vsh;          // vacc >> vexp, registered
    // saturated to +-0x7FFFFFFF (mrdp_fgrad / mrdp_vgrad)
    function automatic [31:0] sat64;
        input signed [63:0] v;
        begin
            if (v > 64'sh07FFFFFFF)       sat64 = 32'h7FFFFFFF;
            else if (v < -64'sh07FFFFFFF) sat64 = 32'h80000001;
            else                          sat64 = v[31:0];
        end
    endfunction
    // TRI_R's raw fields as they leave the RAM (mrdp_tri_r_fields): colour
    // x 255, Z x 65535 / 2; S, T, W were converted in place by S_RSET
    function automatic [31:0] vconv;
        input [31:0] q;
        input [2:0]  a;
        input        r;
        reg signed [48:0] z;
        begin
            z = $signed({q[31], q, 16'd0}) - $signed({{17{q[31]}}, q});
            if (!r)            vconv = q;
            else if (a < 3'd4) vconv = (q << 8) - q;
            else if (a == 3'd7) vconv = z[32:1];
            else               vconv = q;
        end
    endfunction
    wire [31:0] vq = vconv(vr_q, vi, is_trir);

    // ---- TRI_R: the whole setup (mrdp_setup.h mrdp_tri_r, _fields) ------
    // A microcoded engine (tools/mrdp_ucode.py: the program, its assembler,
    // a cycle-accurate model and a differential test against the C). The
    // FSM it replaced loaded ~30 registers from ~70 distinct expressions and
    // cost 1,840 ALMs; this datapath is 8 registers, one adder-based ALU,
    // the multiplier and the 64-bit shifter, and the vertex RAM.
    wire [2:0]  u_seq, u_cond, u_sa, u_ra, u_rb, u_rd;
    wire [7:0]  u_tgt;
    wire [1:0]  u_sb, u_shl;
    wire [15:0] u_imm;
    wire [4:0]  u_alu;
    wire [0:0]  u_we, u_rrel, u_wv, u_wrel, u_mul, u_fl;
    wire [5:0]  u_rad, u_wad;
    wire [3:0]  u_sp;
    reg  [7:0]  upc, uret, upc_n;
    mrdp_ucode_rom ucode (.clk(clk), .a(upc_n), .u_seq(u_seq), .u_cond(u_cond), .u_tgt(u_tgt),
        .u_sa(u_sa), .u_ra(u_ra), .u_sb(u_sb), .u_rb(u_rb), .u_imm(u_imm), .u_alu(u_alu),
        .u_we(u_we), .u_rd(u_rd), .u_rad(u_rad), .u_rrel(u_rrel), .u_wv(u_wv), .u_wad(u_wad),
        .u_wrel(u_wrel), .u_mul(u_mul), .u_shl(u_shl), .u_sp(u_sp), .u_fl(u_fl));
    reg [31:0] ur [0:7];
    reg [31:0] ufreg;               // the result the flags were taken from
    reg        ufn, ufc, ufneg;     // less-than / sign, borrow, the area's sign
    reg [8:0]  ue_sa;               // the reciprocal ROM's address (the RSP's VRCP ROM)
    reg [7:0]  ve_raw;              // the gradients' exponent before its clamp
    reg [63:0] sh_in;
    reg [5:0]  sh_k;
    // one 64-bit arithmetic right shifter: S_VSET's gradients, S_RSET's
    // products (all below 2^62, so arithmetic = logical there) and shifts
    wire [63:0] sh_src = (st == S_VSET) ? vacc : sh_in;
    wire [5:0]  sh_amt = (st == S_VSET) ? vexp : sh_k;
    wire [63:0] sh_out = $signed(sh_src) >>> sh_amt;
    // the microcode reads it through a register, and its saturation to
    // 0x7FFFFFFF through a second: shifter -> operand -> adder in one cycle
    // missed by 5 ns
    reg  [63:0] sh_r;
    reg  [31:0] sh_rs;
    always @(posedge clk) begin
        sh_r <= sh_out;
        sh_rs <= (sh_r[63:31] != 33'd0) ? 32'h7FFFFFFF : sh_r[31:0];
    end
    wire [15:0] seed_q;
    mrdp_seed_rom seed (.clk(clk), .a(ue_sa), .q(seed_q));
    function automatic [5:0] clz32;          // 32 for 0; binary search (log depth)
        input [31:0] v;
        reg [31:0] x;
        reg [5:0]  n;
        begin
            x = v; n = 6'd0;
            if (x == 32'd0) clz32 = 6'd32;
            else begin
                if (x[31:16] == 16'd0) begin n = n + 6'd16; x = x << 16; end
                if (x[31:24] == 8'd0)  begin n = n + 6'd8;  x = x << 8;  end
                if (x[31:28] == 4'd0)  begin n = n + 6'd4;  x = x << 4;  end
                if (x[31:30] == 2'd0)  begin n = n + 6'd2;  x = x << 2;  end
                if (x[31] == 1'b0)     n = n + 6'd1;
                clz32 = n;
            end
        end
    endfunction
    wire [5:0]  tbase = tflags[2] ? 6'd12 : 6'd0;   // the texture triplets
    // operands
    wire [31:0] u_opa = (u_sa == 3'd0) ? ur[u_ra] : (u_sa == 3'd1) ? vr_q
                      : (u_sa == 3'd2) ? sh_r[31:0] : (u_sa == 3'd3) ? sh_r[63:32]
                      : (u_sa == 3'd4) ? sh_rs
                      : (u_sa == 3'd5) ? mul_p[31:0] : mul_p[63:32];
    wire [31:0] u_opb = (u_sb == 2'd0) ? ur[u_rb] : (u_sb == 2'd1) ? vr_q
                      : (u_sb == 2'd2) ? {16'd0, u_imm} : {u_imm, 16'd0};
    // the adder: X + (Y ^ inv) + cin. SUB/SBB X - Y, RSB/RSBB Y - X, ADD,
    // and the conditional negations ABS/NEGB/NEGD (0 + (Y ^ c) + c)
    wire        u_sub = (u_alu >= 5'd3 && u_alu <= 5'd6) || u_alu == 5'd13;
    wire        u_rsb = (u_alu == 5'd5 || u_alu == 5'd6);
    wire        u_bor = (u_alu == 5'd4 || u_alu == 5'd6) && ufc;
    wire        u_ncnd = (u_alu == 5'd7) ? u_opa[31] : (u_alu == 5'd8) ? u_opb[31]
                       : (u_alu == 5'd9) ? (ufneg ^ u_opb[31]) : 1'b0;
    wire [31:0] u_x = u_sub ? (u_rsb ? u_opb : u_opa) : (u_alu == 5'd2) ? u_opa : 32'd0;
    wire [31:0] u_y = (u_sub && !u_rsb) || u_alu == 5'd2 ? u_opb : u_opa;
    wire        u_inv = u_sub | u_ncnd;
    wire [32:0] u_sum = {1'b0, u_x} + {1'b0, u_y ^ {32{u_inv}}} + {32'd0, u_sub ? !u_bor : u_ncnd};
    wire        u_lt = u_x[31] ^ ~u_y[31] ^ u_sum[32];     // signed X < Y (+ borrow)
    reg  [31:0] u_res;
    always @(*) begin
        case (u_alu)
            5'd0:    u_res = u_opa;
            5'd1:    u_res = u_opb;
            5'd10:   u_res = u_opa & u_opb;
            5'd11:   u_res = u_opa | u_opb;
            5'd12:   u_res = {26'd0, clz32(u_opa)};
            5'd13:   u_res = u_lt ? u_opa : u_opb;
            5'd14:   u_res = mul_p[47:16];
            5'd15:   u_res = mul_p[61:30];
            5'd16:   u_res = {2'b01, seed_q, 14'd0};    // 1 + rom / 2^16, Q30
            default: u_res = u_sum[31:0];
        endcase
    end
    // sequencing: the branch sees the flags of earlier instructions
    wire        u_fz = ufreg == 32'd0;
    reg         u_c;
    always @(*) begin
        case (u_cond)
            3'd0: u_c = u_fz;          3'd1: u_c = !u_fz;
            3'd2: u_c = ufn;           3'd3: u_c = ufn | u_fz;
            3'd4: u_c = !ufneg;        3'd5: u_c = tflags[1];
            3'd6: u_c = !tflags[1];    default: u_c = !ufn;
        endcase
        if (st != S_RSET) upc_n = 8'd0;
        else case (u_seq)
            3'd0:    upc_n = upc + 8'd1;
            3'd1:    upc_n = u_tgt;
            3'd2:    upc_n = u_c ? u_tgt : upc + 8'd1;
            3'd3:    upc_n = u_tgt;
            3'd4:    upc_n = uret;
            default: upc_n = 8'd0;         // DONE, EXIT
        endcase
    end
    always @(posedge clk) begin
        upc <= upc_n;
        if (st == S_RSET) begin
            if (u_seq == 3'd3) uret <= upc + 8'd1;
            if (u_we) ur[u_rd] <= u_res;
            if (u_fl) begin
                ufreg <= u_res;
                ufn <= u_sub ? u_lt : u_res[31];
                if (u_sub && u_alu != 5'd13) ufc <= !u_sum[32];
            end
            if (u_sp == 4'd4) ufneg <= u_res[31];
            if (u_sp == 4'd12) ue_sa <= u_res[30:22];
        end
    end
    assign vram_ra = (st == S_RSET) ? u_rad + (u_rrel ? tbase : 6'd0) : vr_ra;
    function automatic attr_on;             // mrdp_attr_on()
        input [2:0] fl;
        input [2:0] a;
        attr_on = a < 3'd4 ? fl[2] : a < 3'd7 ? fl[1] : fl[0];
    endfunction
    reg [31:0] row_off;            // y * width * 2 (color / Z images)
    reg [9:0]  xs, xe_m1;
    reg [10:0] xe;
    reg [9:0]  px_cnt;
    reg [9:0]  tt, tt0, tt1;        // load rows
    reg [31:0] trow_addr;

    wire [4:0] mul_n = 5'd0;

    // sign helpers
    wire signed [13:0] yh_s = t_yh, ym_s = t_ym, yl_s = t_yl;
    // the last row + 1 (see S_IDLE); other_l is this triangle's here
    wire signed [13:0] yl_end = (COVERAGE && other_l[3]) ? ($signed(t_yl) + 14'sd3) >>> 2 : ($signed(t_yl) + 14'sd1) >>> 2;
    wire signed [11:0] yb_c = (yl_end < $signed({2'b0, sc_y1})) ? yl_end[11:0] : $signed({1'b0, sc_y1});
    wire signed [13:0] y4 = {y, 2'b10};     // 4y + 2

    // edge x values of this row
    wire [31:0] minor_x = (y4 < ym_s) ? exm : exl;
    wire [31:0] xleft  = t_lft ? exh : minor_x;
    wire [31:0] xright = t_lft ? minor_x : exh;
    wire signed [32:0] xl33 = $signed({xleft[31], xleft}), xr33 = $signed({xright[31], xright});
    // the crack grow: each edge out by its g_* (lang/c/mrdp/mrdp.c draw_triangle)
    wire [15:0] g_minor = (y4 < ym_s) ? g_m : g_l;
    wire [15:0] g_left = t_lft ? g_h : g_minor, g_right = t_lft ? g_minor : g_h;
    // (0x7FFF - g_left is 0x7FFF..-0x8001; 0x7FFF + g_right below 2^17)
    wire signed [32:0] xs_c = (xl33 + ($signed({17'd0, 16'h7FFF}) - $signed({17'd0, g_left}))) >>> 16;
    wire signed [32:0] xe_c = (xr33 + $signed({16'd0, {1'b0, 16'h7FFF} + {1'b0, g_right}})) >>> 16;
    // mrdp_setup.h mrdp_grow_amount: 1/8 + |DxDy| / 8, capped at 1/4 px
    function automatic [15:0] grow_amt;
        input [31:0] d;
        reg [31:0] a;
        begin
            a = d[31] ? -d : d;
            grow_amt = (a[31:3] >= 29'h2000) ? 16'h4000 : 16'h2000 + {3'd0, a[15:3]};
        end
    endfunction


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
    wire [31:0] e_q = e_ram[ea], ade_q = ade_ram[ea];
    // e + DaDe / 2 (TRI_INIT, two clocks ahead of its product) or e + DaDe (the row step)
    wire [31:0] e_add = e_q + ((st == S_TRI_INIT) ? asr(ade_q, 5'd1) : ade_q);
    reg  [31:0] e_add1, e_add2;
    always @(posedge clk) begin
        e_add1 <= e_add; e_add2 <= e_add1;
    end
    wire vset_w = st == S_VSET && (vs == 4'd15 || (vs == 4'd0 && !attr_on(tflags, vi)));
    wire vset_on = vs == 4'd15;
    wire tinit_w = st == S_TRI_INIT && mi >= 5'd5 && mi <= 5'd12;
    wire step_w = !es[3];
    wire [2:0]  e_wa = vset_w ? vi : tinit_w ? mi[2:0] - 3'd5 : es[2:0];
    wire [31:0] e_wd = vset_w ? (vset_on ? vval + (is_trig ? 32'd0 : mul_p[47:16]) : 32'd0)
                     : tinit_w ? e_add2 + mul_p[31:0] : e_add;
    always @(posedge clk) begin
        if (vset_w || tinit_w || step_w) e_ram[e_wa] <= e_wd;
        if (vset_w) ade_ram[vi] <= vset_on ? vde : 32'd0;
    end

    integer i;
    reg [31:0] dxs;          // span start offset from the H edge, 16.16
    always @(posedge clk) begin
        zlb_we <= 1'b0; clb_we <= 1'b0; msk_we <= 1'b0;
        vw_we <= 1'b0;
        pix_valid <= 1'b0;
        if (rst) begin
            st <= S_IDLE;
            es <= 4'd8; es_done <= 1'b0;
            collecting <= 1'b0;
            exec_go <= 1'b0;
            busy_exec <= 1'b0;
            mr_valid <= 1'b0;
            sync_count <= 0; load_count <= 0; unknown_ops <= 0;
            other_h <= 0; other_l <= 0; cc_hi <= 0; cc_lo <= 0;
            fill <= 0; fog <= 0; blend <= 0; prim <= 0; env <= 0; prim_lod_frac <= 0; prim_z <= 0;
            sc_xh <= 0; sc_yh <= 0; sc_xl <= 12'd4092; sc_yl <= 12'd4092;
            cimg <= 0; zimg <= 0; timg <= 0; cimg_width <= 11'd1; timg_width <= 11'd1;
        end else begin
            // ---- memory engine handshake
            if (mr_valid && mr_ready) mr_valid <= 1'b0;

            // ---- pixel results into the line buffers
            if (px_valid) begin
                msk_we <= 1'b1;
                msk_wa <= px_x[9:1];
                msk_be <= px_x[0] ? 2'b10 : 2'b01;
                cm_wd <= {2{px_cpass}};
                zm_wd <= {2{px_zpass}};
                lb_wa <= px_x[9:1];
                clb_wd <= {px_color, px_color};
                zlb_wd <= {px_depth, px_depth};
                clb_be <= px_x[0] ? 2'b10 : 2'b01;
                zlb_be <= px_x[0] ? 2'b10 : 2'b01;
                clb_we <= px_cpass;
                zlb_we <= px_zpass;
            end else if (mw_z_we || mw_c_we) begin
                lb_wa <= mw_addr;
                zlb_wd <= mw_data; clb_wd <= mw_data;
                zlb_be <= 2'b11; clb_be <= 2'b11;
                zlb_we <= mw_z_we; clb_we <= mw_c_we;
            end

            // ---- command intake
            if (cmd_valid && cmd_ready) begin
                if (!collecting) begin
                    w0 <= cmd_data;
                    nwords <= cmd_len(cmd_data);
                    widx <= 6'd1;
                    collecting <= 1'b1;
                    if (op0[5:4] == 2'b01 || op0 == 6'h20) begin
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
                if (is_tri) begin
                    kind_tri <= 1'b1; kind_texrect <= 1'b0; kind_rect <= 1'b0; kind_fillmode <= 1'b0;
                    tflags <= is_trir ? w0[21:19] : op[2:0];
                    p_shade <= is_trir ? w0[21] : op[2]; p_tex <= is_trir ? w0[20] : op[1];
                    p_zbuf <= is_trir ? w0[19] : op[0]; p_direct <= 1'b0; p_tile <= t_tile;
                    vexp <= 6'd30;
                    y0r <= $signed(t_yh) >>> 2;
                    ymr <= $signed(t_ym) >>> 2;
                    // first row: max(y0, scissor); rows end at min((yl + 1) >> 2, scissor),
                    // with AA_EN at min((yl + 3) >> 2, scissor): every row a subscanline reaches
                    t_aa <= other_l[3];
                    t_grow <= 3'd0;             // TRI_R: the microcode's GRW
                    y <= (($signed(t_yh) >>> 2) > $signed({2'b0, sc_y0})) ? ($signed(t_yh) >>> 2) : $signed({2'b0, sc_y0});
                    yb <= yb_c;
                    mi <= 5'd0;
                    vi <= 3'd0; vs <= 4'd0; vbase <= 5'd0;
                    st <= is_trir ? S_RSET : S_VSET;
                end else begin
                    case (op)
                    6'h00, 6'h26, 6'h27, 6'h28: begin busy_exec <= 1'b0; end
                    6'h29: st <= S_SYNC;
`ifdef MRDP_NO_TEXRECT
                    6'h36: begin                  // (texrects skipped as unknown)
`else
                    6'h24, 6'h25, 6'h36: begin
`endif
                        // rectangles: texrect (w0: XL YL, w1: tile XH YH, then S T, DsDx DtDy in t_xl, t_dxl)
                        kind_tri <= 1'b0;
                        kind_texrect <= op != 6'h36;
                        kind_fillmode <= op == 6'h36 && cyc == 2'd3;
                        kind_rect <= 1'b1;
                        p_shade <= 1'b0; p_tex <= op != 6'h36; p_zbuf <= 1'b0; p_direct <= 1'b1;
                        p_tile <= w1[26:24];
                        rx0 <= w1[23:14];
                        ry0 <= {2'b00, w1[11:2]};
                        begin : rect_bounds
                            reg [10:0] x1c, y1c;
                            reg [9:0]  xa_c, ya_c;
                            reg incl;
                            incl = (op == 6'h36) ? (cyc == 2'd3) : (cyc == 2'd2);
                            x1c = {1'b0, w0[23:14]} + (incl ? 11'd1 : 11'd0);
                            y1c = {1'b0, w0[11:2]}  + (incl ? 11'd1 : 11'd0);
                            xa_c = w1[23:14] > sc_x0 ? w1[23:14] : sc_x0;
                            ya_c = w1[11:2]  > sc_y0 ? w1[11:2]  : sc_y0;
                            xa <= xa_c;
                            xb <= (x1c < {1'b0, sc_x1}) ? x1c[9:0] : sc_x1;
                            y <= {2'b00, ya_c};
                            yb <= (y1c < {1'b0, sc_y1}) ? {1'b0, y1c} : {2'b00, sc_y1};
                            if (((x1c < {1'b0, sc_x1}) ? x1c : {1'b0, sc_x1}) <= {1'b0, xa_c})
                                yb <= {2'b00, ya_c};   // empty
                        end
                        mi <= 5'd0;
                        st <= S_RECT_INIT;
                    end
                    6'h2D: begin sc_xh <= w0[23:12]; sc_yh <= w0[11:0]; sc_xl <= w1[23:12]; sc_yl <= w1[11:0]; busy_exec <= 1'b0; end
                    6'h2E: begin prim_z <= w1[31:16]; busy_exec <= 1'b0; end
                    6'h2F: begin other_h <= w0[23:0]; other_l <= w1; busy_exec <= 1'b0; end
                    6'h32: begin
                        tl_size[w1[26:24]] <= {w0[23:12], w0[11:0], w1[23:12], w1[11:0]};
                        busy_exec <= 1'b0;
                    end
                    6'h34: begin
                        tl_size[w1[26:24]] <= {w0[23:12], w0[11:0], w1[23:12], w1[11:0]};
                        p_tile <= w1[26:24];      // its tmem / line reach the engine through ts_*
                        tt0 <= w0[11:2]; tt1 <= w1[11:2];
                        mi <= 5'd0;
                        st <= S_LOAD_INIT;
                    end
                    6'h35: begin
                        tl_attr[w1[26:24]] <= {w0[23:21], w0[17:9], w0[18], w0[8:0],
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
                    6'h3D: begin timg <= w1; timg_width <= {1'b0, w0[9:0]} + 11'd1; busy_exec <= 1'b0; end
                    6'h3E: begin zimg <= w1; busy_exec <= 1'b0; end
                    6'h3F: begin cimg <= w1; cimg_width <= {1'b0, w0[9:0]} + 11'd1; busy_exec <= 1'b0; end
                    default: begin unknown_ops <= unknown_ops + 16'd1; busy_exec <= 1'b0; end
                    endcase
                end
            end

            // ---- TRI_V: gradients of each attribute on (mrdp_vattr): 15 steps
            // on the shared multiplier (products 2 cycles after issue)
            // ---- TRI_R: one microinstruction per cycle (tools/mrdp_ucode.py)
            S_RSET: begin
                if (u_mul) begin mul_a <= u_opa; mul_b <= u_opb; end
                case (u_shl)
                    2'd1: sh_in <= mul_pu;
                    2'd2: sh_in <= mul_p;
                    2'd3: sh_in <= {u_opa, u_opb};
                    default: ;
                endcase
                if (u_wv) begin
                    vw_we <= 1'b1; vw_wa <= u_wad + (u_wrel ? tbase : 6'd0); vw_wd <= u_res;
                end
                case (u_sp)
                    4'd1:  t_yh <= u_opa[27:14];
                    4'd2:  t_ym <= u_opa[27:14];
                    4'd3:  t_yl <= u_opa[27:14];
                    4'd4:  t_lft <= !u_res[31];
                    4'd5:  t_dxh <= u_res;
                    4'd6:  t_dxm <= u_res;
                    4'd7:  t_dxl <= u_res;
                    4'd8:  t_xh <= u_res;
                    4'd9:  t_xm <= u_res;
                    4'd10: t_xl <= u_res;
                    4'd11: sh_k <= u_res[5:0];
                    4'd13: ve_raw <= u_res[7:0];            // e in -18..77: clamped at DONE
                    4'd14: t_grow <= u_res[2:0];            // GRW: the crack-grow edges
                    default: ;
                endcase
                if (u_seq == 3'd5) begin                 // DONE: the rows, then the attributes
                    vexp <= ve_raw[7] ? 6'd0 : ve_raw[6] ? 6'd63 : ve_raw[5:0];
                    y0r <= $signed(t_yh) >>> 2;
                    ymr <= $signed(t_ym) >>> 2;
                    y <= (($signed(t_yh) >>> 2) > $signed({2'b0, sc_y0})) ? ($signed(t_yh) >>> 2) : $signed({2'b0, sc_y0});
                    yb <= yb_c;
                    st <= S_VSET;
                end else if (u_seq == 3'd6) begin        // EXIT: degenerate, nothing drawn
                    st <= S_IDLE; busy_exec <= 1'b0;
                end
            end

            S_VSET: begin
                case (vs)
                    4'd0: if (!attr_on(tflags, vi)) begin
                              vi <= vi + 3'd1;
                              if (vi == 3'd7) st <= S_TRI_INIT;
                          end else begin
                              vr_ra <= {1'b0, vbase}; vs <= 4'd1;
                          end
                    // RAM data arrive two steps after the address
                    4'd1: begin vr_ra <= {1'b0, vbase} + 6'd1; vs <= 4'd2; end
                    4'd2: begin va0 <= vq; vr_ra <= {1'b0, vbase} + 6'd2; vs <= 4'd3; end
                    // TRI_G: value, DaDx, DaDe as sent -- straight to the write
                    4'd3: if (is_trig) begin vgx <= vr_q; vs <= 4'd4; end
                          else begin vda1 <= vq - va0; vr_ra <= 6'd24; vs <= 4'd4; end   // F0
                    4'd4: if (is_trig) begin vde <= vr_q; vval <= va0; vs <= 4'd15; end
                          else begin vda2 <= vq - va0; vr_ra <= 6'd25; vs <= 4'd5; end   // F1
                    4'd5: begin mul_a <= vda1; mul_b <= vr_q; vr_ra <= 6'd26; vs <= 4'd6; end  // da1 F0; F2
                    4'd6: begin mul_a <= vda2; mul_b <= vr_q; vr_ra <= 6'd27; vs <= 4'd7; end  // da2 F1; F3
                    4'd7: begin mul_a <= vda2; mul_b <= vr_q; vacc <= mul_p; vs <= 4'd8; end   // da2 F2
                    4'd8: begin mul_a <= vda1; mul_b <= vr_q; vacc <= vacc - mul_p; vs <= 4'd9; end  // da1 F3
                    4'd9: begin vsh <= sh_out; vacc <= mul_p; vs <= 4'd10; end                 // gx >> e
                    4'd10: begin vgx <= sat64(vsh); vacc <= vacc - mul_p; vr_ra <= 6'd28; vs <= 4'd11; end  // kx
                    4'd11: begin vsh <= sh_out; mul_a <= vgx; mul_b <= t_dxh; vr_ra <= 6'd29; vs <= 4'd12; end  // ky
                    4'd12: begin vgy <= sat64(vsh); mul_a <= vgx; mul_b <= vr_q; vs <= 4'd13; end   // gx kx
                    4'd13: begin mul_a <= vgy; mul_b <= vr_q; vde <= vgy + mul_p[47:16]; vs <= 4'd14; end  // gy ky
                    4'd14: begin vval <= va0 + mul_p[47:16]; vs <= 4'd15; end
                    default: begin                   // 15: the attribute's registers
                        adx[vi] <= vgx;             // e[vi], ade[vi]: the RAM writes below
                        vbase <= vbase + 5'd3;
                        vs <= 4'd0; vi <= vi + 3'd1;
                        if (vi == 3'd7) st <= S_TRI_INIT;
                    end
                endcase
            end

            // ---- triangle: edge and attribute values at the first row, and
            // that row's offset into the images (y * width * 2)
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
                        5'd12: begin mul_a <= adx[7]; mul_b <= t_dxh; end
                        default: ;
                    endcase
                    // products come back two cycles later
                    case (mi)
                        5'd2:  exh <= t_xh + asr(t_dxh, 5'd1) + mul_p[31:0];
                        5'd3:  exm <= t_xm + asr(t_dxm, 5'd1) + mul_p[31:0];
                        5'd4:  exl <= t_xl + asr(t_dxl, 5'd1) + mul_p[31:0];
                        // 5..12: e[mi - 5] = e + DaDe / 2 + k DaDe (the RAM writes below)
                        5'd13: row_off <= {mul_p[30:0], 1'b0};
                        5'd14: dzdy <= ade_q - mul_p[47:16];                // ea = 7
                        default: ;
                    endcase
                    // the crack grow's amounts (the slopes are final here)
                    g_h <= (t_aa && !COVERAGE && t_grow[0]) ? grow_amt(t_dxh) : 15'd0;
                    g_m <= (t_aa && !COVERAGE && t_grow[1]) ? grow_amt(t_dxm) : 15'd0;
                    g_l <= (t_aa && !COVERAGE && t_grow[2]) ? grow_amt(t_dxl) : 15'd0;
                    mi <= mi + 5'd1;
                    if (mi == 5'd14) st <= S_ROW;
                end
            end

            // ---- a row: its span, or the next row
            S_ROW: begin
                if (y >= yb) begin
                    st <= S_IDLE;
                    busy_exec <= 1'b0;
                end else if (kind_tri && y4 < yh_s) begin
                    st <= S_NEXT;          // its centre is above YH
                end else if (kind_tri) begin
                    xs_r <= xs_c;          // the scissor in S_SPAN: one path was too long
                    xe_r <= xe_c;
                    st <= S_SPAN;
                end else begin
                    // rectangles: columns fixed
                    xs <= xa;
                    xe <= {1'b0, xb};
                    mi <= 5'd0;
                    st <= kind_fillmode ? S_FILL : S_SETUP;
                end
            end


            S_SPAN: begin
                begin : span
                    reg signed [32:0] a, b;
                    a = xs_r < $signed({23'd0, sc_x0}) ? $signed({23'd0, sc_x0}) : xs_r;
                    // an AA_EN span's end is inclusive (S_AASUB): the + 1 beside the
                    // compare, not in front of it
                    if (COVERAGE && t_aa) b = xe_r >= $signed({23'd0, sc_x1}) ? $signed({23'd0, sc_x1}) : xe_r + 33'sd1;
                    else      b = xe_r > $signed({23'd0, sc_x1}) ? $signed({23'd0, sc_x1}) : xe_r;
                    if (a >= b) st <= S_NEXT;
                    else begin
                        xs <= a[9:0];
                        xe <= b[10:0];
                        mi <= 5'd0;
                        st <= S_SETUP;
                    end
                end
            end

            // ---- span setup: cur = e + DaDx * (xs + 0.5 - xh) (triangles),
            // the S/T accumulators (texrects)
            S_SETUP: begin
                if (kind_tri) begin
                    dxs = ({xs, 16'd0} + 32'h8000) - exh;
                    if (mi < 5'd8) begin mul_a <= adx[mi[2:0]]; mul_b <= dxs; end
                    if (mi >= 5'd2 && mi < 5'd10)
                        cur[mi - 5'd2] <= e_q + mul_p[47:16];                // ea = mi - 2
                    mi <= mi + 5'd1;
                    if (mi == 5'd9) begin
                        st <= z_cmp ? S_RDZ : (need_mem ? S_RDC : S_PIX);
                        es <= 4'd0;                 // e[] no longer needed this row: step it
                    end
                end else begin
`ifdef MRDP_NO_TEXRECT
                    st <= z_cmp ? S_RDZ : (need_mem ? S_RDC : S_PIX);   // fill rectangles: no S, T
`else
                    // texrect: t_xl = S T, t_dxl = DsDx DtDy; copy mode steps 4x
                    begin : rect_setup
                        reg [2:0] ds;
                        reg signed [31:0] dsdx, dtdy, sv, tv, dcol, drow;
                        ds = (cyc == 2'd2) ? 3'd7 : 3'd5;
                        sv = {{16{t_xl[31]}}, t_xl[31:16]};   tv = {{16{t_xl[15]}}, t_xl[15:0]};
                        dsdx = {{16{t_dxl[31]}}, t_dxl[31:16]}; dtdy = {{16{t_dxl[15]}}, t_dxl[15:0]};
                        dcol = $signed({22'd0, xs}) - $signed({22'd0, rx0});
                        drow = $signed({{20{y[11]}}, y}) - $signed({{20{ry0[11]}}, ry0});
                        case (mi)
                            5'd0: begin mul_a <= dsdx; mul_b <= (op == 6'h25) ? drow : dcol; end
                            5'd1: begin mul_a <= dtdy; mul_b <= (op == 6'h25) ? dcol : drow; end
                            default: ;
                        endcase
                        if (mi == 5'd2) cur[4] <= (sv <<< ds) + mul_p[31:0];
                        if (mi == 5'd3) cur[5] <= (tv <<< 5) + mul_p[31:0];
                        shS <= ds;
                        adx[4] <= (op == 6'h25 || op == 6'h36) ? 32'd0 : dsdx;
                        adx[5] <= (op == 6'h25) ? dtdy : 32'd0;
                        mi <= mi + 5'd1;
                        if (mi == 5'd3) st <= z_cmp ? S_RDZ : (need_mem ? S_RDC : S_PIX);
                    end
`endif
                end
                px_cnt <= 10'd0;
            end

            // ---- span reads
            S_RDZ: if (!mr_valid && mr_ready) begin
                mr_valid <= 1'b1; mr_op <= 3'd0; mr_base <= zimg + row_off; mr_x0 <= xs; mr_x1 <= xe;
                st <= S_MEMWAIT; st_after <= need_mem ? S_RDC : S_PIX;
            end
            S_RDC: if (!mr_valid && mr_ready) begin
                mr_valid <= 1'b1; mr_op <= 3'd1; mr_base <= cimg + row_off; mr_x0 <= xs; mr_x1 <= xe;
                st <= S_MEMWAIT; st_after <= S_PIX;
            end
            S_MEMWAIT: if (!mr_valid && mr_ready) st <= st_after;

            // ---- pixels, one a clock
            S_PIX: begin
                pix_valid <= 1'b1;
                pix_x <= xs + px_cnt;
                pix_kill <= 1'b0; pix_part <= 1'b0; pix_minor <= 1'b0;
                pix_sx8 <= 3'sd0; pix_sy8 <= 3'sd0;
                if (px_cnt != 10'd0) begin
                    for (i = 0; i < 8; i = i + 1) cur[i] <= cur[i] + adx[i];
                end
                px_cnt <= px_cnt + 10'd1;
                if ({1'b0, xs} + {1'b0, px_cnt} + 11'd1 == xe) st <= S_DRAIN;
            end
            S_DRAIN: if (!pix_busy && !pix_valid && !px_valid) st <= S_WRC;

            // ---- span write-back
            S_WRC: if (!mr_valid && mr_ready) begin
                mr_valid <= 1'b1; mr_op <= 3'd3; mr_base <= cimg + row_off; mr_x0 <= xs; mr_x1 <= xe;
                st <= S_MEMWAIT; st_after <= z_upd ? S_WRZ : S_NEXT;
            end
            S_WRZ: if (!mr_valid && mr_ready) begin
                mr_valid <= 1'b1; mr_op <= 3'd2; mr_base <= zimg + row_off; mr_x0 <= xs; mr_x1 <= xe;
                st <= S_MEMWAIT; st_after <= S_NEXT;
            end

            // ---- fill mode rectangles: straight to memory
            S_FILL: if (!mr_valid && mr_ready) begin
                mr_valid <= 1'b1; mr_op <= 3'd4; mr_base <= cimg + row_off; mr_x0 <= xs; mr_x1 <= xe;
                st <= S_MEMWAIT; st_after <= S_NEXT;
            end

            // ---- next row
            // (a triangle row waits for e[] += ade[]: behind the span since
            // its setup, or here when the row had none)
            S_NEXT: if (!kind_tri || es_done) begin
                y <= y + 12'sd1;
                row_off <= row_off + {20'd0, cimg_width, 1'b0};
                exh <= exh + t_dxh;
                exm <= exm + t_dxm;
                exl <= exl + t_dxl;
                es_done <= 1'b0;
                st <= S_ROW;
            end else if (es == 4'd8) es <= 4'd0;

            // ---- rectangles: row offset of the first row
            S_RECT_INIT: begin
                if (mi == 5'd0) begin mul_a <= sx12(y); mul_b <= {21'd0, cimg_width}; end
                if (mi == 5'd2) begin row_off <= {mul_p[30:0], 1'b0}; st <= S_ROW; end
                mi <= mi + 5'd1;
            end

            // ---- LOAD TILE: rows t0..t1 of the texture image into TMEM
            S_LOAD_INIT: begin
                if (mi == 5'd0) begin mul_a <= {22'd0, tt0}; mul_b <= {21'd0, timg_width}; end
                if (mi == 5'd2) begin trow_addr <= timg + {mul_p[30:0], 1'b0}; tt <= tt0; st <= S_LOAD; end
                mi <= mi + 5'd1;
            end
            S_LOAD: if (!mr_valid && mr_ready) begin
                if (tt > tt1) begin
                    load_count <= load_count + 32'd1;
                    st <= S_IDLE;
                    busy_exec <= 1'b0;
                end else begin
                    mr_valid <= 1'b1; mr_op <= 3'd5; mr_base <= trow_addr;
                    mr_x0 <= w0[23:14]; mr_x1 <= {1'b0, w1[23:14]} + 11'd1;
                    mr_trow <= tt - tt0;
                    trow_addr <= trow_addr + {20'd0, timg_width, 1'b0};
                    tt <= tt + 10'd1;
                end
            end

            // ---- SYNC FULL: every earlier write accepted by the memory
            S_SYNC: if (!mr_valid && mr_ready && !mem_wpend) begin
                sync_count <= sync_count + 32'd1;
                st <= S_IDLE;
                busy_exec <= 1'b0;
            end

            default: st <= S_IDLE;
            endcase
        end
    end


    // line-buffer read port: the pixel pipe while pixels run, else the engine
    assign lb_ra = (st == S_PIX || st == S_DRAIN) ? pix_lb_ra : mem_lb_ra;

    assign idle = !busy_exec && !exec_go && !collecting && !mem_busy && !mem_wpend && !mr_valid;
endmodule
