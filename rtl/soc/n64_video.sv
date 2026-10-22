// Mirlo-N64's scan-out: the N64's VI_ORIGIN framebuffer (RGBA5551 as the RDP writes it, two pixels a word, the
// left one in the high half; or RDP's RGB565) to the Pocket's video pads, as the N64's VI sends it: a 640-wide
// line (the VI's 640 pixel clocks between H_START 108 and H_END 748), 240 lines a field, progressive, 59.94 Hz
// (PAL: 400 x 313 at 50 Hz), a pixel each two clocks of clk_vid, 25 MHz (sys / 2.5): 12.5 MHz; a serrated picture's two
// fields woven, 480 lines a frame, a pixel a clock (below).
//
//  - vid: a timing generator, never stalling -- a pixel the FIFO does not have in time is black. Lines of 790
//    clocks and 264 lines make 208,560 clocks: nine of the lines in vblank lose two clocks (1,042,710 sys clocks,
//    59.9400 Hz to 1.5 ppm), see below. The Pocket's scaler slot 0 (video.json) is 640 x 240.
//  - the picture is the VI's own: a window of the line (VI_H_START / H_END: `vi_px0`, `vi_pw`, in pixel clocks from
//    the VI's 108 -- one 597 wide, 20 in, is not centred) and of the field (`vi_y0`, `vi_rows`: V_START / V_END), black
//    around it. The window's pixel i (clock i) shows the framebuffer's pixel floor(i X_SCALE / 1024) (`vi_xs`, 2.10:
//    0x200 doubles a 320-wide picture, 0x2C0 shows a 440-wide one whole, 0x400 is a hi-res 640), its line j the row
//    floor(j Y_SCALE / 1024) (`vi_ys`: 0x400 a row a line, 0x800 every other row: a hi-res picture's even rows),
//    the rows `vi_width` pixels apart (the VI's WIDTH: a hi-res picture's 1280 is two rows of 640). Nearest pixel and
//    row, the VI's filters at the framebuffer's own pixels, as the VI has them (below). One path for every size.
//    A SERRATED hi-res picture (VI_CTRL's SERRATE: 640 x 480 as two fields of 240 rows, `vi_il`) is sent woven: its 480
//    rows in order, a frame each field of the VI's, 640 x 480 progressive (docs/resolution.md) -- the picture two fields
//    make; the Pocket's scaler shows interlaced fields torn.
//  - a 32-bit picture (VI_STATUS type 3, `vi_px32`: RGBA8888, R the lowest byte) is a pixel a word: the pitch is
//    VI_WIDTH words, a row reads twice the words (the runtime's `vi_nw`), and the coverage is the alpha byte's top three
//    bits (no hidden plane read). The FIFO's read side converts each pixel to what a 16-bit picture's carries -- each
//    channel's top five bits, the coverage as bit 0 and the two hidden bits -- so the line buffers and the filters are
//    the 16-bit picture's, unchanged: the picture is shown at five bits a channel (the low three are dropped).
//  - sys: at the frame boundary (the first line of vblank, from vid) the base is latched from VI_ORIGIN (a new
//    origin never tears the frame being sent) and the frame's rows are read in
//    order into the FIFO: 64 KiB (16,384 words), which is what rides out the DRAM contention -- the arbiter puts
//    the scan-out first only while that FIFO is below a quarter full (`m_urgent`, below). A row is
//    `vi_nw` words from the row's first, the next row the one Y_SCALE's accumulator reaches (0, 1 or 2 pitches on;
//    a hi-res picture reads 320 words a row: 18.4 MB/s as a 320-wide one did; a woven frame twice the rows: 36.8 MB/s).
//  - the FIFO crosses sys -> vid; each word carries the frame's epoch bit (what is left of a frame is dropped by
//    the vid side instead of resetting a dual-clock FIFO) and a row-start bit (the vid side starts each row at one,
//    dropping what the last row left).
//  - `vblank` is the timing's vblank as a sys level (APF_VIDEO).
//  - vi_latch: the frame boundary as a sys pulse, on the cycle of the vi_line of the line it is on (rcp_vi.sv counts
//    the lines from it).
//  - vi_line / vi_frame: each line's start and the frame's, as sys pulses --
//    the frame's four lines (a quarter of a millisecond) before the frame
//    boundary, when the base is latched: the VI interrupt, which a game answers
//    with its new framebuffer.
//  - the pads outside DE: after each line's DE the scaler slot of the next frame (video.json: 0 640 x 240, 1 640 x 480 for a
//    woven frame; rgb[14:13]), else 0.
//
// The picture's pixels come through a pipeline whose steps are not clocks but source pixels (`adv`): a pixel a
// step, so that the VI's filters see the framebuffer's pixels and not the output's, however it is scaled. A line's
// steps are the priming, in the blanking before it (a few columns of nothing, then the window's first pixels, so
// that its first filtered pixel is out), and in the window one at each clock the X_SCALE accumulator carries.
// The pads show what the pipeline's last register holds, which a step changes just before the window's clock the
// new pixel is for.
`default_nettype none

module n64_video #(
    parameter logic [23:0] ORIGIN_MASK = 24'h1FFFFF,   // word address: the N64's 8 MiB RDRAM
    // the framebuffer is the N64's own -- RGBA5551, the left pixel in the
    // word's high half (rtl/rdp) -- not RDP's RGB565 with it in the low half
    parameter bit N64FB = 1'b0
) (
    input  wire         clk,            // sys
    input  wire         rst,
    input  wire         clk_vid,
    input  wire         rst_vid,
    input  wire         enable,         // sys: scan out (else black, no DMA)
    input  wire  [25:0] vi_origin,      // sys: the framebuffer's byte address (ORIGIN_MASK: its span)
    input  wire         vi_il,          // sys: the picture is serrated (VI_CTRL's SERRATE, a hi-res picture): sent woven
    output logic        vblank,         // sys
    input  wire  [11:0] vi_width,       // sys: VI_WIDTH, the rows' pitch in pixels (even; 512 or more: a hi-res picture, no filters)
    input  wire         vi_px32,        // sys: the pixels are 32-bit (VI_STATUS type 3): RGBA8888, a pixel a word
    input  wire         vi_black,       // sys: H_START 0 (osViBlack): black
    input  wire         vi_pal,         // sys: the TV system asked for: 1 PAL (625 lines, 50 Hz), 0 NTSC (and MPAL: 525 lines, 59.94 Hz)
    output logic        tv_pal,         // sys: the field being scanned out is PAL (vi_pal, taken at the frame boundary)
    input  wire         gamma,          // sys: the VI's gamma correction (VI_CTRL's GAMMA: a game's choice)
    input  wire         vi_aa,          // sys: the VI's anti-alias filter (VI_CTRL's AA_MODE 0 or 1; MIRLO_VI_AA)
    input  wire         vi_divot,       // sys: and its divot filter (VI_CTRL bit 4)
    input  wire         vi_dedither,    // sys: and its dither filter (VI_CTRL bit 16)
    input  wire  [4:0]  crop_v,         // sys: the core's "Crop Vertical": lines of the picture blanked at its top and its
                                        //      bottom
    input  wire  [1:0]  crop_r,         // sys: its "Crop Right": pixels (of the 320 wide frame: two clocks each) blanked at the picture's right edge
    input  wire  [8:0]  vi_lines,       // sys: the lines the VI's window shows (the rest black)
    input  wire  [9:0]  vi_px0,         // sys: the picture's first clock in the line (H_START - 108, VI pixels)
    input  wire  [9:0]  vi_pw,          // sys: its width in clocks (H_END - H_START)
    input  wire  [7:0]  vi_y0,          // sys: its first line in the field
    input  wire  [8:0]  vi_rows,        // sys: its lines (0: the whole field)
    input  wire  [10:0] vi_xs,          // sys: the VI's X_SCALE (2.10, at most 1.0)
    input  wire  [11:0] vi_ys,          // sys: its Y_SCALE (2.10, at most 2.0: 0x800, a serrated picture's field)
    input  wire  [10:0] vi_nw,          // sys: the words a row reads (the runtime: the window's last pixel and the priming)
    input  wire         osd_en,         // (the OSD of the 320 x 240 frame is gone: unused)
    input  wire         fps_en,
    input  wire  [15:0][31:0] osd_val,
    // the DMA's SDRAM port (sys): word addresses
    output logic        m_valid,
    output logic [23:0] m_addr,
    output logic        m_urgent,       // the FIFO is low: the arbiter's priority, with the CPU's and the RSP's
    input  wire         m_ready,
    input  wire         m_rvalid,
    input  wire  [31:0] m_rdata,
    // VI clocks (sys pulses)
    output logic        vi_line,
    output logic        vi_frame,
    output logic        vi_latch,       // the frame boundary (the base's latch), in step with vi_line
    // pads (vid)
    output logic [7:0]  vga_r,          // 8:8:8 (the N64 VI's eight bits: its filters' results as they are)
    output logic [7:0]  vga_g,
    output logic [7:0]  vga_b,
    output logic        vga_hsync,
    output logic        vga_vsync,
    output logic        vga_de,
    output logic        vga_skip        // a progressive frame's pixel's second clock
);

// The field's timing: 790 clocks a line (the N64's 3094 VI clocks of 48.68 MHz x 12.5 / 48.68 ... = 395 pixels of the
// 320 x 240 frame this system has always had, whose 1,042,710 system clocks a field the runtime's VI_PERIOD counts),
// 264 lines, nine of them (253..261, no picture, no sync) two clocks shorter: 208,542 clocks of clk_vid, 1,042,710 of
// clk_sys, 59.9400 Hz to 1.5 ppm. PAL (libultra's PAL modes write V_SYNC 0x271, H_SYNC 0xC69: 312.5 lines of 3178 clocks of the
// VI's 49.6565 MHz = 20.000 ms): 800-clock lines, 313 of them, the 50 lines 253..302 eight clocks shorter: 263 x 800 + 50 x 792
// = 250,000 clocks of clk_vid, 1,250,000 of clk_sys, 50.000 Hz exactly. A PAL mode shows 237 lines too. vi_pal is taken at the
// frame boundary (tv_pal, sys) and the timing changes at the end of that field, so the change never cuts a field short.
localparam logic [9:0] HRES = 10'd640, HSYNC_S = 10'd656, HSYNC_E = 10'd704;
// VS rises and falls six clocks before an HS (the Pocket's video interface: no HS in the three clocks after VS)
localparam logic [9:0] VSYNC_X = 10'd650;
localparam logic [9:0] SHORT_V = 10'd253;                // the first short line (both systems; a woven frame's: 506)
localparam logic [9:0] HSCAN_N = 10'd789, HSCAN_P = 10'd799;     // the line's last clock (the count is one more)
localparam int         SHORT_LINES_N = 9, SHORT_LINES_P = 50;      // (a woven frame: twice as many)
localparam logic [9:0] SHORT_PX_N = 10'd2, SHORT_PX_P = 10'd8;   // clocks
localparam logic [9:0] VSCAN_N = 10'd263, VSCAN_P = 10'd312;     // (a woven frame: 527, 625)
// the line's schedule of the picture's pipeline (hcount): from PR_SYNC the row's start is looked for in the FIFO, from PR0 the
// priming steps (NF empty columns, then pixels), as the line ends
localparam logic [9:0] PR_SYNC = 10'd690, PR0 = 10'd740;
localparam int         NF = 2;                           // the steps of nothing before the first pixel (the filters' left taps)
localparam int         NPE = 9;                          // the priming's steps: until the first pixel's filtered value is out

// ---------------------------------------------------------------- vid: timing
// clk_vid is 25 MHz. A progressive picture's frame is the timing above at 12.5 MHz: everything on the vid side steps on every other
// clock (ce), and the pads hold each pixel two clocks, the second skipped (vga_skip). A WOVEN frame -- a serrated hi-res picture's
// two fields, the rows of its 640 x 480 picture in order (wv) -- steps on every clock: twice the lines of the same length, 528
// (PAL 626), 480 of them the picture's, the short lines twice as many: the same 1,042,710 (1,250,000) system clocks a frame. The
// Pocket shows it as the progressive frame it is (video.json's 640 x 480): its scaler keeps a frame whole while it shows the one
// before -- not so for interlaced fields, which it writes over the picture it is showing: a tear wherever its output, 60 Hz on
// the Dock and on its screen, overtakes the fields.
logic       wv_v;                   // the frame being generated is a woven one (taken from wv_f, sys, where the frame ends)
logic       ce_t;
wire        ce = wv_v || ce_t;
always_ff @(posedge clk_vid) ce_t <= rst_vid ? 1'b0 : !ce_t;
logic [9:0] hcount;
logic [9:0] vcount;
// the TV system of the field being generated: taken from tv_pal (sys; two flip-flops) where the field ends, so a field is whole
logic       pal_v;
logic [1:0] pal_s;
always_ff @(posedge clk_vid) pal_s <= {pal_s[0], tv_pal};
wire  [9:0] HSCAN       = pal_v ? HSCAN_P : HSCAN_N;
wire  [9:0] SHORT_PX    = pal_v ? SHORT_PX_P : SHORT_PX_N;
wire  [9:0] VSCAN       = wv_v ? (pal_v ? 10'd625 : 10'd527) : pal_v ? VSCAN_P : VSCAN_N;
wire  [9:0] VRES        = wv_v ? 10'd480 : 10'd240;
wire  [9:0] SHORT_S     = wv_v ? 2 * SHORT_V : SHORT_V;
// (four constants: as a product of the two selects it took a DSP block)
localparam logic [9:0] SHORT_END_N = SHORT_V + 10'(SHORT_LINES_N) - 10'd1, SHORT_END_P = SHORT_V + 10'(SHORT_LINES_P) - 10'd1;
localparam logic [9:0] SHORT_END_WN = 2 * SHORT_V + 10'(2 * SHORT_LINES_N) - 10'd1, SHORT_END_WP = 2 * SHORT_V + 10'(2 * SHORT_LINES_P) - 10'd1;
wire  [9:0] SHORT_END   = wv_v ? (pal_v ? SHORT_END_WP : SHORT_END_WN) : (pal_v ? SHORT_END_P : SHORT_END_N);
logic       hactive, vactive, hsync, vsync;
logic       ann;
logic       wvf_v1, wvf_v2;         // (wv_f, two flip-flops)

logic       short_ln;               // the line vcount names (the lines SHORT_V .. SHORT_V + SHORT_LINES - 1) is shorter
always_ff @(posedge clk_vid) begin
    if (rst_vid) begin
        hcount <= 0; vcount <= 0; hactive <= 0; vactive <= 0; hsync <= 0; vsync <= 0; short_ln <= 0; pal_v <= 0; wv_v <= 0;
    end else if (ce) begin
        hcount <= hcount + 10'd1;
        if (hcount == 0) hactive <= 1;
        if (hcount == HRES) hactive <= 0;
        if (hcount == HSYNC_S) hsync <= 1;
        if (hcount == HSYNC_E) hsync <= 0;
        if (hcount == (short_ln ? HSCAN - SHORT_PX : HSCAN)) hcount <= 0;
        if (hcount == VSYNC_X) begin
            if (vcount == VRES + 10'd1) vsync <= 1;
            if (vcount == VRES + (wv_v ? 10'd17 : 10'd9)) vsync <= 0;
        end
        if (hcount == HSYNC_S) begin
            vcount <= vcount + 10'd1;
            if (vcount == 0) vactive <= 1;
            if (vcount == VRES) vactive <= 0;
            if (vcount == VSCAN) begin vcount <= 0; pal_v <= pal_s[1]; wv_v <= wvf_v2; end
            if (vcount == SHORT_S - 10'd1) short_ln <= 1;                // (vcount is the line before: it counts up now)
            if (vcount == SHORT_END) short_ln <= 0;
        end
    end
end
wire de = hactive & vactive;
// The frame announced (ann: the next frame is a woven one -- what the frame now shown is, taken at its line 0 and held, so that its
// lines' slot words say it). A frame that is not what was announced (the first of a mode) is sent black, and the next is right.
logic       wv_f;                    // sys: the frame being read is a woven one (taken at the frame boundary)
always_ff @(posedge clk_vid) begin
    wvf_v1 <= wv_f; wvf_v2 <= wvf_v1;
    if (rst_vid) ann <= 0;
    else if (ce && hcount == 0 && vcount == 0) ann <= wv_v;
end
// the frame boundary (vid): where the next frame's base is latched and its read begins, and the vid side's epoch.
// LATCH_EARLY lines before the last line of the frame -- not vblank's first line: the VI interrupt that the game answers with
// its new framebuffer (vi_frame, 4 lines before the end of the picture) has all of vblank, less LATCH_EARLY lines, to
// be answered in (the game's own libultra writes VI_ORIGIN in its retrace handler, many monitor entries long); the
// FIFO is read ahead from the latch, and the first row is needed LATCH_EARLY + 1 lines on.
// (A woven frame's lines are half as long: its figures in lines are twice the progressive one's, and the VI's line -- rcp_vi's
// count -- is every other one, the odd ones: the latch's line is one of them.)
localparam int LATCH_EARLY = 8;
wire  fb_vid = ce && hcount == 0 && vcount == VSCAN - (wv_v ? 10'd16 : 10'(LATCH_EARLY));
logic epoch_vid;
always_ff @(posedge clk_vid) if (rst_vid) epoch_vid <= 0; else if (fb_vid) epoch_vid <= ~epoch_vid;

// ------------------------------------------------------------ vid -> sys pulses
// the VI interrupt comes every field (264 lines)
logic t_line, t_frame, t_fb;
wire  fld_pulse = ce && hcount == 0 && vcount == VRES - (wv_v ? 10'd8 : 10'd4);
always_ff @(posedge clk_vid)
    if (rst_vid) begin t_line <= 0; t_frame <= 0; t_fb <= 0; end
    else begin
        if (ce && hcount == 0 && (!wv_v || vcount[0])) t_line <= ~t_line;
        if (fld_pulse) t_frame <= ~t_frame;     // 4 lines (a quarter of a millisecond) before the frame boundary: the VI interrupt that a game answers with its new origin
        if (fb_vid) t_fb <= ~t_fb;
    end
logic [2:0] s_line, s_frame, s_fb;
always_ff @(posedge clk)
    if (rst) begin s_line <= 0; s_frame <= 0; s_fb <= 0; end
    else begin s_line <= {s_line[1:0], t_line}; s_frame <= {s_frame[1:0], t_frame}; s_fb <= {s_fb[1:0], t_fb}; end
wire fb_sys /*verilator public_flat_rd*/ = s_fb[2] ^ s_fb[1];   // (the bench's VITRACE)
// vblank (sys): the frame's blank
logic [2:0] s_vb;
always_ff @(posedge clk) s_vb <= {s_vb[1:0], !vactive};
assign vblank = s_vb[2];
always_ff @(posedge clk) begin vi_line <= s_line[2] ^ s_line[1]; vi_frame <= s_frame[2] ^ s_frame[1]; vi_latch <= fb_sys; end

// ------------------------------------------------------------------ sys: DMA
logic        epoch_sys, running;
logic [9:0]  issued, got;          // (the reads in flight are their difference)
// the picture, latched at the boundary: its rows, their pitch, the steps from one to the next, and what the vid side
// draws it with (the window and the X_SCALE: static for the frame)
logic        all;
logic [8:0]  orow, nr, nrm1;          // (rows: a woven frame's up to 480)
logic [10:0] nwm1_f, stride_f;      // (the words a row reads, less one)
logic [11:0] ys_f;
logic [9:0]  px0_f, pw_f;
logic [8:0]  y0_f;
logic [10:0] xs_f;
logic        nofilt_f;             // a hi-res picture (512 pixels or more a row): the filters are off
logic        px32_f;               // a 32-bit picture (N64FB only: the RGB565 picture has no such mode)
logic [1:0]  ann_s;                // the frame announced, from vid
always_ff @(posedge clk) ann_s <= {ann_s[0], ann};
// a woven frame: a serrated picture of the two pitches of its fields -- WIDTH 640 with Y_SCALE 2.0 (a field every other row; or
// down to 1.5: a picture the VI scales), or WIDTH 1280 with 1.0 (a pitch of two rows): the two fields' lines in turn, from the
// lower origin (rcp_vi's base, the first field's), its window twice the field's lines. The frame's line 2 j + p is the field
// p's line j, as the VI shows it: the row Y_SCALE's accumulator reaches at the field's line j, from the base, and for the
// second field one 640-pixel row (320 words) on. (A serrated 240-row buffer, or another pitch, is not a pair of fields: shown
// as any picture is.)
wire         wv_rq  = vi_il && (vi_width[11:1] == 11'd640 || (vi_width[11:1] == 11'd320 && vi_ys >= 12'h600));
wire  [9:0]  vi_nr2 = {vi_nr, 1'b0};
logic        hid_f;                // the hidden coverage plane is wanted (a filter is on)
logic [10:0] col;                  // the read's word in its row
logic [23:0] raddr;                // the next read's word address
logic [23:0] rowp;                 // the row's first word
logic [9:0]  yacc;                 // the row step's fraction
logic        rs_next;              // the next colour word is its row's first
logic        wodd;                 // (a woven frame) the row being read is the second field's line
localparam int FAW = 14;           // the FIFO: 2^14 words
logic [9:0]  drop;                 // the last frame's reads still to come back (not written)
logic [FAW:0] wlevel;              // the FIFO's fill as sys sees it (words)
wire         keep = m_rvalid && !fb_sys && drop == 0;
wire         hires  = |vi_width[11:9];                       // 512 or more: a hi-res picture
wire         px32_n = vi_px32 && N64FB;
wire  [8:0]  vi_nr  = (vi_rows == 0 || vi_rows > 9'd240) ? 9'd240 : vi_rows;
// the next row: this one and the pitches Y_SCALE's accumulator steps (0 for an enlarged line: the row again, 1, 2)
wire  [11:0] yan    = {2'b00, yacc} + ys_f;                    // (at most 0x3FF + 0x800: never both carries)
// (a woven frame: the second field's line is one 640-pixel row on, 320 words or a 32-bit picture's 640: the same adder)
wire         w2nd   = wv_f && !wodd;
wire  [23:0] rowp_n = rowp + (w2nd ? (px32_f ? 24'd640 : 24'd320) : yan[11] ? {12'd0, stride_f, 1'b0} : yan[10] ? {13'd0, stride_f} : 24'd0);
// a word as the FIFO takes it: two pixels, the left one in the low half --
// RGBA5551 as the game's (N64FB: its bit 0 the RDP's coverage, "complete"),
// or RGB565 without N64FB. A 32-bit picture's word goes through the same
// swap, one pixel: {B, A} in the high half, {R, G} in the low (the vid side
// takes it so).
wire  [31:0] rd_w = N64FB ? {m_rdata[15:0], m_rdata[31:16]} : m_rdata;
// MIRLO_VI_AA: each word goes with its pixels' hidden coverage bits (the RDP's
// plane, 1 MiB below the RDRAM: lang/c/rdp/rdp.h, layout.h) -- a group's
// word (16 pixels) is read before the group's first colour word (and at each
// row's start), and a tag a read tells the words back apart: the hidden
// word is held, a colour word takes its pair's four bits (the even pixel's in
// [1:0]). The FIFO word: {epoch, row start, the pair's hidden bits, the two pixels}.
localparam int FW = 38;
logic        h_next;                // the next read is the group's hidden word
logic [31:0] hreg;
logic [4:0]  tq [0:31];             // the reads in flight: {row start, hidden, the colour word's place in its group}
logic [4:0]  tq_w, tq_r;
wire  [5:0]  tq_n = 6'(tq_w - tq_r);
wire  [4:0]  tq_h = tq[tq_r];
wire  [3:0]  h_nib = hreg[{tq_h[2:0], 2'b00} +: 4];
wire         m_hid = h_next;
always_ff @(posedge clk) begin
    if (rst) begin epoch_sys <= 0; running <= 0; issued <= 0; got <= 0; drop <= 0;
                   col <= 0; raddr <= 0; all <= 1; tv_pal <= 0; h_next <= 0; wv_f <= 0; end
    else begin
        if (fb_sys) begin
            epoch_sys <= ~epoch_sys;
            // (a frame that is not what the frame before announced -- the first of a mode, woven or progressive --: black)
            running <= enable && wv_rq == ann_s[1];
            wv_f <= wv_rq;
            tv_pal <= vi_pal;
            raddr <= vi_origin[25:2] & ORIGIN_MASK;   // SDRAM word address
            rowp  <= vi_origin[25:2] & ORIGIN_MASK;
            orow <= 0; all <= vi_nr == 0; yacc <= 0; rs_next <= 1'b1;
            nr <= wv_rq ? vi_nr2[8:0] : vi_nr; nrm1 <= (wv_rq ? vi_nr2[8:0] : vi_nr) - 9'd1;
            nwm1_f <= vi_nw - 11'd1; ys_f <= vi_ys; wodd <= 1'b0;
            // (a program that never set the pitch: 320 pixels; a 32-bit picture's pitch is a word a pixel)
            stride_f <= vi_width[11:1] == 0 ? (px32_n ? 11'd320 : 11'd160) : px32_n ? vi_width[10:0] : 11'(vi_width[11:1]);
            px0_f <= vi_px0; pw_f <= vi_pw; y0_f <= wv_rq ? {vi_y0, 1'b0} : {1'b0, vi_y0}; xs_f <= vi_xs; nofilt_f <= hires;
            px32_f <= px32_n;
            // (a 32-bit picture's coverage is in its own words: no hidden plane)
            hid_f <= vi_aa && !hires && N64FB && !px32_n;
            issued <= 0; got <= 0; col <= 0;
            drop <= issued - got - 10'(m_rvalid);
            h_next <= vi_aa && !hires && N64FB && !px32_n;
        end else begin
            if (m_valid && m_ready && m_hid) begin
                issued <= issued + 10'd1;
                h_next <= 1'b0;
            end else if (m_valid && m_ready) begin
                issued <= issued + 10'd1;
                rs_next <= 1'b0;
                raddr <= raddr + 24'd1;
                if (col == nwm1_f) begin                  // a row's last word: the next row
                    col <= 0;
                    raddr <= rowp_n;
                    if (w2nd) wodd <= 1'b1;               // (the second field's line)
                    else begin rowp <= rowp_n; yacc <= yan[9:0]; wodd <= 1'b0; end
                    orow <= orow + 9'd1; all <= orow == nrm1;
                    rs_next <= 1'b1; h_next <= hid_f;
                end else begin
                    col <= col + 11'd1;
                    if (raddr[2:0] == 3'd7) h_next <= hid_f;   // the next word starts a group
                end
            end
            if (keep) got <= got + 10'd1;
            if (m_rvalid && drop != 0) drop <= drop - 10'd1;
        end
    end
end
// the tags: pushed with each read, popped with each word back (kept or dropped)
always_ff @(posedge clk) begin
    if (rst) begin tq_w <= 0; tq_r <= 0; end
    else begin
        if (m_valid && m_ready) begin tq[tq_w] <= {rs_next, m_hid, raddr[2:0]}; tq_w <= tq_w + 5'd1; end
        if (m_rvalid) tq_r <= tq_r + 5'd1;
    end
    if (m_rvalid && tq_h[3]) hreg <= m_rdata;
end
// (a registered stage in front of the FIFO, with the epoch of the word's arrival)
logic        f_we, f_ep;
logic [36:0] f_wd;
always_ff @(posedge clk) begin f_we <= !rst && keep && !tq_h[3]; f_wd <= {tq_h[4], h_nib, rd_w}; f_ep <= epoch_sys; end
// ask while the frame is not all asked for and the FIFO has room for what is in flight
// (room and urgency registered: the FIFO's level must not reach the
// arbiter's grant combinationally; the level is a few cycles late anyway, and
// the margin to 512 covers)
wire [15:0] inflight = 16'(issued - got);                // (below 1024)
logic       room, low;
always_ff @(posedge clk) room <= (16'(wlevel) + inflight) < 16'((1 << FAW) - 64);
// urgent below a quarter of the FIFO -- at every frame's start, and wherever the other ports have left it behind: the CPU's and the
// RSP's requests go first (mirlo_mips.sv), and a stream of them (a loop of uncached stores, a long DMA) kept the scan-out from the
// memory while a frame's first rows were due: the lines found the FIFO empty and were black, the picture a few dozen lines lower
// for a frame. Now it goes round with them while it is low; above the mark, as the others.
always_ff @(posedge clk) low <= (16'(wlevel) + inflight) < 16'(1 << (FAW - 2));
assign m_urgent = low;
assign m_valid  = running && !fb_sys && drop == 0 && room
                  && !all && tq_n < 6'd30;
assign m_addr   = m_hid ? 24'hDC_0000 + {6'd0, raddr[20:3]} : raddr;   // (the plane: SDRAM 0x0370_0000)

// ------------------------------------------------------------------ the FIFO
logic        f_rd, f_empty;
logic [FW-1:0] f_q;
afifo #(.W(FW), .AW(FAW)) fifo (
    .wclk(clk), .wrst(rst), .w_en(f_we), .w_data({f_ep, f_wd}), .w_level(wlevel),
    .rclk(clk_vid), .rrst(rst_vid), .r_en(f_rd), .r_data(f_q), .r_empty(f_empty)
);

// --------------------------------------------------------------- vid: the picture
// the frame's parameters (static: latched by sys at the boundary)
logic [51:0] win_s;
always_ff @(posedge clk_vid) win_s <= {px32_f, nofilt_f, nr, y0_f, px0_f, pw_f, xs_f};
wire  [10:0] xs_v    = win_s[10:0];
wire  [9:0]  pw_v    = win_s[20:11];
wire  [9:0]  px0_v   = win_s[30:21];
wire  [9:0]  py0_v   = {1'b0, win_s[39:31]};
wire  [9:0]  nr_v    = {1'b0, win_s[48:40]};
wire         nofilt_v = win_s[49];
wire         px32_v   = win_s[50];
// the window's rows (the fetch lines: vcount py0 .. py0 + nr - 1; line d shows the row of line d - 1: the filters' row below
// is the one just fetched), by line, from the priming on: nv this line fetches a row, cv the one before did (and is shown
// now), av the one before that (the filters' row above)
logic       nv, cv, av;
logic [10:0] win_end;
always_ff @(posedge clk_vid) win_end <= {1'b0, px0_v} + {1'b0, pw_v};
wire        nv_n = vcount >= py0_v && vcount < py0_v + nr_v;
always_ff @(posedge clk_vid)
    if (rst_vid) begin nv <= 0; cv <= 0; av <= 0; end
    else if (ce && hcount == PR_SYNC) begin nv <= nv_n; cv <= nv; av <= cv; end
// the steps of the pipeline
logic       wpos;                   // the next pixel is the FIFO head word's right one
logic [9:0] sx;                     // the slot (the framebuffer's pixel) the next step loads
logic [9:0] xa;                     // X_SCALE's accumulator: its fraction (a step when it carries)
wire  [10:0] xsum = {1'b0, xa} + xs_v;
wire        win_act = hcount > px0_v && hcount < win_end[9:0] && (nv || cv);   // the clocks before the window's last: a step where the accumulator carries
wire        win_adv = win_act && xsum[10];
wire        pr_act  = hcount >= PR0 && hcount < PR0 + 10'(NPE) && (nv || cv);
wire        flush   = pr_act && hcount < PR0 + 10'(NF);
wire        adv     = pr_act || win_adv;
always_ff @(posedge clk_vid) if (ce) begin
    if (hcount == px0_v) xa <= 0;
    else if (win_act) xa <= xsum[9:0];
end
// the FIFO head: a word of the row (the epoch's, not a stale one's)
wire         stale = !f_empty && f_q[FW-1] != epoch_vid;
wire         hd_ok = !f_empty && !stale;
wire         sync_ph = nv && hcount > PR_SYNC && hcount < PR0;
wire         rs_drop = sync_ph && hd_ok && !f_q[FW-2];
wire         pop_ev  = adv && !flush && nv && (wpos || px32_v) && hd_ok;   // (a 32-bit picture: a word a step)
always_comb f_rd = ce && (stale || rs_drop || pop_ev);
logic [15:0] pix;
logic [1:0]  phid;                 // its hidden coverage bits (MIRLO_VI_AA)
logic        pv;                   // pix is the picture's (else black)
logic        lwe;                  // pix is to be written to the line buffers
always_ff @(posedge clk_vid) if (ce) begin
    if (hcount == PR_SYNC) begin wpos <= 0; sx <= 0; end
    else if (adv && !flush) begin
        sx <= sx + 10'd1;
        if (nv) wpos <= ~wpos;
    end
end
// a word's two pixels (the hidden bits of the one) -- the pixel the step loads. A 32-bit word's pixel (R f_q[15:8],
// G [7:0], B [31:24], A [23:16]) as a 16-bit one: each channel's top five bits, the coverage (alpha[7:5]) as bit 0 and
// the hidden two
wire [15:0] p32_hi = {f_q[15:11], f_q[7:3], f_q[31:27], f_q[23]};
wire [15:0] nx_pix = !(nv && hd_ok) || flush ? 16'd0 : px32_v ? p32_hi : wpos ? f_q[31:16] : f_q[15:0];
wire [1:0]  nx_hid = !(nv && hd_ok) || flush ? 2'd0 : px32_v ? f_q[22:21] : wpos ? f_q[35:34] : f_q[33:32];
wire        nx_pv  = nv && hd_ok && !flush;

// the filters' line buffers and windows (the column of the slot, and its two rows above)
logic       aa_on, dd_on, dv_on;
(* ramstyle = "M10K, no_rw_check" *) logic [18:0] lb1 [0:1023];
(* ramstyle = "M10K, no_rw_check" *) logic [18:0] lb2 [0:1023];
logic [18:0] lq1r, lq2r;           // the line buffers' reads
logic        lq_c, lq_a;           // ... that were of rows in the window
wire  [18:0] lq1 = lq_c ? lq1r : 19'd0, lq2 = lq_a ? lq2r : 19'd0;
logic [9:0]  lwa;
// the window: {valid, hidden 2, RGBA5551} (the coverage: {bit 0, hidden})
logic [18:0] wa [0:3], wc [0:4], wb [0:3];
integer wi;
always_ff @(posedge clk_vid) if (ce && adv) begin
    // the pixel this step loads
    pix <= nx_pix; phid <= nx_hid; pv <= nx_pv; lwe <= nx_pv;
    lq1r <= lb1[sx]; lq2r <= lb2[sx];
    lq_c <= !flush && cv; lq_a <= !flush && av;
    lwa <= sx;
    // the column the last step loaded: written to the line buffers and into the windows
    if (lwe) begin lb1[lwa] <= {pv, phid, pix}; lb2[lwa] <= lq1; end
    wa[0] <= lq2; wc[0] <= lq1; wb[0] <= {pv, phid, pix};
    for (wi = 1; wi < 4; wi = wi + 1) begin wa[wi] <= wa[wi - 1]; wb[wi] <= wb[wi - 1]; end
    for (wi = 1; wi < 5; wi = wi + 1) wc[wi] <= wc[wi - 1];
end
// ---- MIRLO_VI_AA: the N64 VI's anti-alias and divot filters (the console's
// VI_filter / VI_filter_pen, Angrylion's), on a pixel whose coverage is not
// complete (the RDP's three bits: bit 0 and the hidden two, fetched with it).
// The rows above and below: two line buffers of the rows fetched (a row is
// shown the line after it is fetched), a pixel, its hidden bits and its valid
// bit, read at its slot and written back on the next step: lb1 the row just fetched, lb2 the
// one before. The window: three rows by five columns, column k the pixel k steps back (the
// centre is k = 2). Taps: (x-1, x+1) above and below, (x-2, x+2) beside; one that is
// not complete or not in the picture is the centre itself. Per channel, the
// second smallest and second largest of the six, widened to take the centre
// in: the N64's c + ((min + max - 2c) * (7 - cvg) + 4) >> 3 on the channels'
// eight bits (c << 3: exactly 8c + (min + max - 2c)(7 - cvg)), its top six to
// the pads. A complete pixel, with the dither filter on (VI_CTRL bit 16; with
// AA off every pixel counts as complete, as the N64 takes c = 7): the dedither
// -- each channel's eight neighbours on the 3 x 3 ring, +1 for each above the
// centre and -1 for each below (on the five bits), added to its eight: the
// smooth colour the RDP's dither stood for. Then the divot filter: where
// any of three neighbours on the line is not complete, the median of the
// three. Delay: six steps.
// A 32-bit picture: the same filters on its pixels' top five bits and the coverage of its alpha byte (the VI's own work
// on eight bits; the line buffers have no room for the low three, so they are dropped at the FIFO's read side).
logic [1:0]  aa_s, dv_s, dd_s;
always_ff @(posedge clk_vid) begin aa_s <= {aa_s[0], vi_aa}; dv_s <= {dv_s[0], vi_divot}; dd_s <= {dd_s[0], vi_dedither}; end
// (a hi-res picture: the filters are off; its pixels go through the line buffers all the same -- 1024 slots: a 640-pixel line and the priming's)
assign aa_on = aa_s[1] && N64FB && !nofilt_v;
assign dd_on = dd_s[1] && N64FB && !nofilt_v;
assign dv_on = dv_s[1] && aa_on;
function automatic logic one6(input logic [5:0] x);               // exactly one of six
    return x == 6'b000001 || x == 6'b000010 || x == 6'b000100 || x == 6'b001000 || x == 6'b010000 || x == 6'b100000;
endfunction
// second smallest and second largest of six, by rank: each pair compared once (15 compares; equal values in the order
// of their numbers), then the value that exactly one of the others comes before, and the one that comes before exactly one
function automatic logic [9:0] mm2(input logic [4:0] a, b, c, d, e, f);
    logic [4:0] v [6];
    logic [5:0] bf [6], af [6];                     // bf[i][j]: v[j] comes before v[i]; af[i][j]: after it
    logic [4:0] lo, hi;
    v[0] = a; v[1] = b; v[2] = c; v[3] = d; v[4] = e; v[5] = f;
    for (int i = 0; i < 6; i++) begin
        bf[i] = 6'd0;
        for (int j = i + 1; j < 6; j++) bf[i][j] = v[j] < v[i];
    end
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < i; j++) bf[i][j] = !bf[j][i];
    for (int i = 0; i < 6; i++) begin
        af[i] = ~bf[i];
        af[i][i] = 1'b0;
    end
    lo = 5'd0; hi = 5'd0;
    for (int i = 0; i < 6; i++) begin
        if (one6(bf[i])) lo = lo | v[i];
        if (one6(af[i])) hi = hi | v[i];
    end
    return {lo, hi};
endfunction
function automatic logic [7:0] pen(input logic [4:0] c, input logic [9:0] m, input logic [2:0] w);
    logic [4:0] mn, mx;
    logic signed [6:0] d;
    logic signed [10:0] o;
    mn = m[9:5] < c ? m[9:5] : c;
    mx = m[4:0] > c ? m[4:0] : c;
    d = $signed({2'b0, mn}) + $signed({2'b0, mx}) - $signed({1'b0, c, 1'b0});
    o = $signed({3'b0, c, 3'b0}) + d * $signed({1'b0, w});
    return o[7:0];
endfunction
function automatic logic complete(input logic [18:0] t);   // coverage 7
    return t[0] && t[17:16] == 2'b11;
endfunction
// a tap: its colour if it is in the picture and complete, else the centre's
function automatic logic [14:0] tap(input logic [18:0] t, input logic [18:0] ctr);
    return (t[18] && complete(t)) ? t[15:1] : ctr[15:1];
endfunction
wire [18:0] ctr = wc[2];
wire [2:0]  cw  = ~{ctr[0], ctr[17:16]};        // 7 - the coverage
wire [14:0] n0 = tap(wa[1], ctr), n1 = tap(wa[3], ctr), n2 = tap(wc[0], ctr),
            n3 = tap(wc[4], ctr), n4 = tap(wb[1], ctr), n5 = tap(wb[3], ctr);
wire [9:0]  mr = mm2(n0[14:10], n1[14:10], n2[14:10], n3[14:10], n4[14:10], n5[14:10]);
wire [9:0]  mg = mm2(n0[9:5],   n1[9:5],   n2[9:5],   n3[9:5],   n4[9:5],   n5[9:5]);
wire [9:0]  mb = mm2(n0[4:0],   n1[4:0],   n2[4:0],   n3[4:0],   n4[4:0],   n5[4:0]);
wire        aa_px = aa_on && ctr[18] && !complete(ctr);
// a 5551 / 565 pixel as R8 G8 B8. The N64's VI takes a 5551 channel as its
// five bits << 3 (the console's VI_lineProcess, angrylion's & 0xf8: white is
// 248), the base its dedither and AA filter work on too: a pixel left as it is
// must match its dedithered and anti-aliased neighbours (with the top bits
// again below, a partial pixel among complete ones came out c >> 2 brighter).
// A 565 picture (not the N64's): the top bits again below.
function automatic logic [23:0] rgb8(input logic [15:0] p);
    if (N64FB) return {p[15:11], 3'd0, p[10:6], 3'd0, p[5:1], 3'd0};
    else       return {p[15:11], p[15:13], p[10:5], p[10:9], p[4:0], p[4:2]};
endfunction
// the dedither of one channel (bits [hi:hi-4] of the 5551 word): a ring tap
// not in the picture counts as equal
function automatic logic [7:0] ddch(input logic [4:0] c, input logic [39:0] n, input logic [7:0] v);
    logic [3:0] up, dn;
    logic [9:0] o;
    up = 4'd0; dn = 4'd0;
    for (int i = 0; i < 8; i++) begin
        up = up + {3'd0, v[i] && n[5*i +: 5] > c};
        dn = dn + {3'd0, v[i] && n[5*i +: 5] < c};
    end
    o = {2'b0, c, 3'b0} + {6'd0, up} - {6'd0, dn};
    return o[8] ? 8'hFF : o[7:0];                 // (above 255: 255; never below 0)
endfunction
wire [7:0]  rv = {wa[3][18], wa[2][18], wa[1][18], wc[3][18], wc[1][18], wb[3][18], wb[2][18], wb[1][18]};
`define DDN(h) {wa[3][h -: 5], wa[2][h -: 5], wa[1][h -: 5], wc[3][h -: 5], wc[1][h -: 5], wb[3][h -: 5], wb[2][h -: 5], wb[1][h -: 5]}
wire [23:0] dd8 = {ddch(ctr[15:11], `DDN(15), rv), ddch(ctr[10:6], `DDN(10), rv), ddch(ctr[5:1], `DDN(5), rv)};
`undef DDN
// the filtered pixel, its partial coverage (for divot) and whether it is the picture's
logic [23:0] fa0, fa1, fa2;
logic [2:0]  fe, fv;                   // [0] the newest
always_ff @(posedge clk_vid) if (ce && adv) begin
    fa0 <= !ctr[18] ? 24'd0 : aa_px ? {pen(ctr[15:11], mr, cw), pen(ctr[10:6], mg, cw), pen(ctr[5:1], mb, cw)}
         : dd_on ? dd8 : rgb8(ctr[15:0]);
    fe[0] <= aa_px; fv[0] <= ctr[18];
    fa1 <= fa0; fa2 <= fa1; fe[2:1] <= fe[1:0]; fv[2:1] <= fv[1:0];
end
// divot: the median of three, by channel (a neighbour not in the picture is the centre)
function automatic logic [7:0] med3(input logic [7:0] a, b, c);
    logic ba, ca, cb;                       // (b before a, c before a, c before b: the one that exactly one of the others comes before)
    ba = b < a; ca = c < a; cb = c < b;
    return ({8{ba ^ ca}} & a) | ({8{!ba ^ cb}} & b) | ({8{ca ^ cb}} & c);
endfunction
wire [23:0] dl = fv[2] ? fa2 : fa1, dr = fv[0] ? fa0 : fa1;
wire        dv_px = dv_on && fv[1] && (fe[0] || fe[1] || fe[2]);
logic [23:0] pq;
always_ff @(posedge clk_vid) if (ce && adv)
    pq <= dv_px ? {med3(dl[23:16], fa1[23:16], dr[23:16]), med3(dl[15:8], fa1[15:8], dr[15:8]), med3(dl[7:0], fa1[7:0], dr[7:0])} : fa1;

// ---- the output's window and crop
// the VI's window (slow-changing, from sys): black while H_START is 0 --
// libultra's osViBlack, the game's level changes, when the framebuffer is
// being rewritten -- and below its last line (V_START..V_END: SM64 shows
// 237 lines from its origin, one line into the framebuffer: the 240th
// would be the next framebuffer's first, or the memory past the last one)
logic [1:0] blk_s; logic [8:0] lin_s1, lin_s2;
always_ff @(posedge clk_vid) begin blk_s <= {blk_s[0], vi_black}; lin_s1 <= vi_lines; lin_s2 <= lin_s1; end
// The core's video options: the picture's first and last crop_v lines and its last crop_r columns (of the 320-wide
// frame this was: two clocks each), blanked -- relative to the picture (a window too), the frame and its timing unchanged. Thresholds
// in the timing's own counts, registered: static.
logic [6:0] cv_s1, cv_s2;
logic [9:0] crop_top, crop_bot, crop_rgt;
wire  [9:0] cvl = wv_v ? {4'd0, cv_s2[4:0], 1'b0} : {5'd0, cv_s2[4:0]};   // (a field's lines, as the N64's: a woven frame's two each)
wire  [9:0] lin_v = wv_v ? {lin_s2, 1'b0} : {1'b0, lin_s2};
always_ff @(posedge clk_vid) begin
    cv_s1 <= {crop_r, crop_v}; cv_s2 <= cv_s1;
    crop_top <= py0_v + cvl;
    crop_bot <= py0_v + nr_v - cvl;
    crop_rgt <= win_end[9:0] - {7'd0, cv_s2[6:5], 1'b0};
end
// the display line d shows the row fetched at d - 1, so its window is the lines py0 + 1 .. py0 + nr
wire  crop = blk_s[1] || vcount > lin_v                 // (active lines are vcount 1..240, a woven frame's 1..480)
          || vcount <= crop_top || vcount > crop_bot || hcount > crop_rgt || hcount <= px0_v || !cv;

// The VI's gamma correction: a channel's square root, the N64's 2 sqrt(64 v)
// on its eight bits (the console's VI_sqrt without the gamma dither's noise),
// a table read as the pixel goes into pix_d. A game with it on draws that much
// darker (Star Fox 64).
logic [1:0] gam_s;
always_ff @(posedge clk_vid) gam_s <= {gam_s[0], gamma};
(* romstyle = "M10K" *) logic [7:0] gtab [0:255];
function automatic logic [7:0] gsqrt(input int v);        // 2 floor(sqrt(64 v))
    int r;
    r = 0;
    for (int k = 0; k < 128; k++) if (k * k <= 64 * v) r = k;
    return 8'(2 * r);
endfunction
initial for (int v = 0; v < 256; v++) gtab[v] = gsqrt(v);
logic [7:0] gq_r, gq_g, gq_b;          // pq through the table (pix_d's clock)
// outputs, a register behind pq
logic de_d, hs_d, vs_d, cr_d;
// the word for the Pocket outside DE: the slot of the next frame after each line's DE (green's bits 6:5, rgb[14:13]; core_top
// passes them on): 1 (640 x 480) for a woven frame, 0 (640 x 240) for any other; no frame features (every frame is progressive)
wire  [1:0] slot_v = ann ? 2'd1 : 2'd0;
logic [23:0] pix_d;
wire  [7:0]  o_r = gam_s[1] ? gq_r : pix_d[23:16];
wire  [7:0]  o_g = gam_s[1] ? gq_g : pix_d[15:8];
wire  [7:0]  o_b = gam_s[1] ? gq_b : pix_d[7:0];
always_ff @(posedge clk_vid) begin
    if (ce) begin
        de_d <= de; hs_d <= hsync; vs_d <= vsync; cr_d <= crop;
        pix_d <= pq;
        gq_r <= gtab[pq[23:16]]; gq_g <= gtab[pq[15:8]]; gq_b <= gtab[pq[7:0]];
        vga_de <= de_d; vga_hsync <= hs_d; vga_vsync <= vs_d;
        vga_r <= !de_d || cr_d ? 8'd0 : o_r;
        vga_g <= !de_d ? (vs_d ? 8'd0 : {1'b0, slot_v, 5'd0}) : cr_d ? 8'd0 : o_g;
        vga_b <= !de_d || cr_d ? 8'd0 : o_b;
    end
    // (a progressive frame's pixel stays on the pads two clocks: the second is skipped)
    vga_skip <= !ce && vga_de;
end

endmodule
`default_nettype wire
