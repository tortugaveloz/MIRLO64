// Mirlo-N64's scan-out: the N64's VI_ORIGIN framebuffer (RGB565 as MRDP
// writes it: two pixels a word, the left one in the low half) to the
// Pocket's video pads: 320 x 240 at 59.94 Hz (and the other sizes, below), clk_vid
// 12.5 MHz = sys / 5.
//
//  - vid: a timing generator (the 320 x 240 set is the fixed one
//    the Pocket's scaler has been fed with); it never stalls -- a word the FIFO does not have in
//    time shows black.
//  - sys: at the frame boundary (the first line of vblank, from vid) the base
//    is latched from VI_ORIGIN (a new
//    origin never tears the frame being sent) and the frame's 38,400 words
//    are read in order into the FIFO: 64 KiB (16,384 words), which is what rides out the DRAM contention -- the
//    arbiter gives the scan-out no priority (a priority scheme was tried on
//    the Pocket and went badly).
//  - a hi-res framebuffer (VI_WIDTH 512 or more: 640 wide -- Pokemon Stadium's menus) is always shown in this 320 x 240
//    frame, reduced without a buffer: a word (two pixels) is read for each pixel shown and the two are averaged (2 x 1 box;
//    the left pixel alone, point sampling, was the first try: `havg` below); one that is 480 rows (vi_tall: VI yScale 2,
//    both fields' rows are in the buffer from the first field's origin) shows that first field alone, the buffer's even rows (every other row is skipped by the
//    reads). The VI's filters (below) are off for it. One pixel clock, 12.5 MHz, with video_skip.
//  - a framebuffer smaller than the screen (VI_WIDTH under 320: Banjo-Kazooie's 292 x 213) is shown as it is, a pixel a
//    pixel, black around it: its vi_rows rows of VI_WIDTH / 2 words are read
//    one after the other, and on the screen the picture starts vi_x0 pixels
//    in and vi_y0 lines down.
//  - the FIFO crosses sys -> vid; each word carries the frame's epoch bit, so
//    what is left of a frame is dropped by the vid side instead of resetting
//    a dual-clock FIFO.
//  - one frame size, the Pocket's scaler slot 0 (320 x 240): outside DE the pads are 0. `vblank` is the timing's vblank
//    as a sys level (APF_VIDEO).
//  - vi_latch: the frame boundary as a sys pulse, on the cycle of the vi_line of the line it is on (rcp_vi.sv counts
//    the lines from it).
//  - vi_line / vi_frame: each line's start and the frame's, as sys pulses --
//    the frame's four lines (a quarter of a millisecond) before the frame
//    boundary, when the base is latched: the VI interrupt, which a game answers
//    with its new framebuffer.
//  - OSD (a debug build: osd_en): 16 words (osd_val, sys) as hex in a box at
//    the top left, 8 lines of two -- the only window into the running SoC
//    without a UART, read through the HDMI capture. Read live across the
//    clock domains (no snapshot: 512 flip-flops the device does not have):
//    a value that changes while it is drawn may show torn for a frame.
`default_nettype none

module n64_video #(
    parameter logic [23:0] ORIGIN_MASK = 24'h1FFFFF,   // word address: the N64's 8 MiB RDRAM
    // the framebuffer is the N64's own -- RGBA5551, the left pixel in the
    // word's high half (rtl/rdp) -- not MRDP's RGB565 with it in the low half
    parameter bit N64FB = 1'b0
) (
    input  wire         clk,            // sys
    input  wire         rst,
    input  wire         clk_vid,
    input  wire         rst_vid,
    input  wire         enable,         // sys: scan out (else black, no DMA)
    input  wire  [25:0] vi_origin,      // sys: the framebuffer's byte address (ORIGIN_MASK: its span)
    output logic        vblank,         // sys
    input  wire  [11:0] vi_width,       // sys: VI_WIDTH (512 or more: hi-res, 640 wide, shown at half its width)
    input  wire         vi_tall,        // sys: the hi-res picture is 480 rows (interlaced: its first field is shown), else 240
    input  wire         vi_black,       // sys: H_START 0 (osViBlack): black
    input  wire         vi_pal,         // sys: the TV system asked for: 1 PAL (625 lines, 50 Hz), 0 NTSC (and MPAL: 525 lines, 59.94 Hz)
    output logic        tv_pal,         // sys: the field being scanned out is PAL (vi_pal, taken at the frame boundary)
    input  wire         gamma,          // sys: the VI's gamma correction (VI_CTRL's GAMMA: a game's choice)
    input  wire         vi_aa,          // sys: the VI's anti-alias filter (VI_CTRL's AA_MODE 0 or 1; MIRLO_VI_AA)
    input  wire         vi_divot,       // sys: and its divot filter (VI_CTRL bit 4)
    input  wire         vi_dedither,    // sys: and its dither filter (VI_CTRL bit 16)
    input  wire  [4:0]  crop_v,         // sys: the core's "Crop Vertical": lines of the picture blanked at its top and its
                                        //      bottom
    input  wire  [1:0]  crop_r,         // sys: its "Crop Right": pixels blanked at the picture's right edge
    input  wire  [8:0]  vi_lines,       // sys: the lines the VI's window shows (the rest black)
    input  wire  [5:0]  vi_x0,          // sys: the picture's left edge on the screen, pixels
    input  wire  [5:0]  vi_y0,          // sys: its first line
    input  wire  [8:0]  vi_rows,        // sys: its lines (0: the whole frame)
    input  wire         osd_en,
    input  wire         fps_en,         // the frame counter in the corner (when the OSD is off)
    input  wire  [15:0][31:0] osd_val,  // sys
    // the DMA's SDRAM port (sys): word addresses
    output logic        m_valid,
    output logic [23:0] m_addr,
    output logic        m_urgent,       // (always 0: no priority)
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
    output logic        vga_skip        // the pixel is not this clock's (a pixel of two clocks: its second)
);

// The frame the scan-out sends is the Pocket's scaler slot 0 (video.json), on a pixel clock clk_vid = sys / 5
// (12.5 MHz): a pixel lasts two clocks (video_skip tells the scaler which one is its).
//   320 x 240, 2 clocks a pixel, 790 clocks a line, 264 lines (nine of them 788), 59.940 Hz
// The field is the N64's: every NTSC mode of libultra writes V_SYNC 0x20D, H_SYNC 0xC15 -- 262.5 lines of 3094 clocks of
// the VI's 48.6818 MHz = 59.9400 Hz, which is 1,042,708.5 clocks of a 62.5 MHz clk_sys. A line of 790 clocks (395 pixels)
// and 264 lines make 1,042,800 (59.935 Hz, 80 ppm slow: 4.8 ms a minute); SHORT_LINES of the lines in vblank (lines
// 253..261, no picture, no sync) lose one pixel (2 clocks) -- 9 of them: 208,542 clocks of clk_vid, 1,042,710 of clk_sys,
// 59.9400 Hz to 1.5 ppm. (SHORT_LINES = 0: the plain 790; the Pocket's scaler takes either, 47..61 Hz.) The field
// does not move with the size, nor do the scan-out's reads (a field is 240 rows of 320 words: 18.4 MB/s). It is a
// fixed rate whatever the registers say beyond the TV system (the Pocket's slot is one fixed 320 x 240 frame; the
// registers of every NTSC mode are these).
//   PAL (vi_pal: libultra's PAL modes write V_SYNC 0x271, H_SYNC 0xC69: 312.5 lines of 3178 clocks of the VI's 49.6565 MHz =
// 20.000 ms, 50 Hz, which is 1,250,000 clocks of a 62.5 MHz clk_sys): the same slot, 400-pixel lines (800 clocks, 64 us as the
// console's) and 313 of them; the 50 lines SHORT_V..SHORT_V+49, in vblank, lose 4 pixels (396): 263 x 800 + 50 x 792 =
// 250,000 clocks of clk_vid, 1,250,000 of clk_sys, 50.000 Hz exactly. The picture is the same 240 lines of the same slot: a
// PAL mode of libultra shows 237 lines too (V_START 95..569 half-lines, as NTSC's 37..511). vi_pal is taken at the frame
// boundary (tv_pal, sys) and the timing changes at the end of that field, so the change never cuts a field short.
logic       sub;                   // vid: the clock within a two-clock pixel
wire        ce   = sub;            // a pixel's clock
always_ff @(posedge clk_vid) sub <= rst_vid ? 1'b0 : ~sub;
localparam logic [8:0] HRES = 9'd320, HSYNC_S = 9'd328, HSYNC_E = 9'd352;
localparam logic [9:0] SHORT_V = 10'd253;                // the first short line (both systems)
localparam logic [9:0] VRES = 10'd240;
// the two TV systems' constants (pal_v selects, below): the line's last pixel (the pixel count is one more), the lines that are
// shorter (the lines of vblank from SHORT_V), by how many pixels, and the field's last line
localparam logic [8:0] HSCAN_N = 9'd394, HSCAN_P = 9'd399;
localparam int         SHORT_LINES_N = 9, SHORT_LINES_P = 50;
localparam logic [8:0] SHORT_PX_N = 9'd1, SHORT_PX_P = 9'd4;
localparam logic [9:0] VSCAN_N = 10'd263, VSCAN_P = 10'd312;

// ---------------------------------------------------------------- vid: timing
logic [8:0] hcount;
logic [9:0] vcount;
// the TV system of the field being generated: taken from tv_pal (sys; two flip-flops) where the field ends, so a field is whole
logic       pal_v;
logic [1:0] pal_s;
always_ff @(posedge clk_vid) pal_s <= {pal_s[0], tv_pal};
wire  [8:0] HSCAN       = pal_v ? HSCAN_P : HSCAN_N;
wire  [8:0] SHORT_PX    = pal_v ? SHORT_PX_P : SHORT_PX_N;
wire  [9:0] VSCAN       = pal_v ? VSCAN_P : VSCAN_N;
wire  [9:0] SHORT_END   = SHORT_V + (pal_v ? 10'(SHORT_LINES_P) : 10'(SHORT_LINES_N)) - 10'd1;
logic       hactive, vactive, hsync, vsync;
logic       short_ln;               // the line vcount names (the lines SHORT_V .. SHORT_V + SHORT_LINES - 1) is a pixel short
always_ff @(posedge clk_vid) begin
    if (rst_vid) begin
        hcount <= 0; vcount <= 0; hactive <= 0; vactive <= 0; hsync <= 0; vsync <= 0; short_ln <= 0; pal_v <= 0;
    end else if (ce) begin
        hcount <= hcount + 9'd1;
        if (hcount == 0) hactive <= 1;
        if (hcount == HRES) hactive <= 0;
        if (hcount == HSYNC_S) hsync <= 1;
        if (hcount == HSYNC_E) hsync <= 0;
        if (hcount == (short_ln ? HSCAN - SHORT_PX : HSCAN)) hcount <= 0;
        if (hcount == HSYNC_S) begin
            vcount <= vcount + 10'd1;
            if (vcount == 0) vactive <= 1;
            if (vcount == VRES) vactive <= 0;
            if (vcount == VRES + 10'd1) vsync <= 1;
            if (vcount == VRES + 10'd9) vsync <= 0;
            if (vcount == VSCAN) begin vcount <= 0; pal_v <= pal_s[1]; end
            if (vcount == SHORT_V - 10'd1) short_ln <= 1;                // (vcount is the line before: it counts up now)
            if (vcount == SHORT_END) short_ln <= 0;
        end
    end
end
wire de = hactive & vactive;
// MIRLO_VI_AA: the picture is fetched a line ahead of the display (the
// filter needs the row below the one shown): fetch lines are vcount 0..VRES-1,
// shown 1..VRES (vact_f leads vactive by a line)
logic vact_f;
always_ff @(posedge clk_vid)
    if (rst_vid) vact_f <= 1'b0;
    else if (ce && hcount == HSYNC_S) begin
        if (vcount == VSCAN) vact_f <= 1'b1;
        if (vcount == VRES - 10'd1) vact_f <= 1'b0;
    end
wire       de_f = hactive & vact_f;
wire [9:0] vc_f = vcount + 10'd1;

// the frame boundary (vid): where the next frame's base is latched and its read begins, and the vid side's epoch.
// LATCH_EARLY lines before the last line of the frame -- not vblank's first line: the VI interrupt that the game answers with
// its new framebuffer (vi_frame, 4 lines before the end of the picture) has all of vblank, less LATCH_EARLY lines, to
// be answered in (the game's own libultra writes VI_ORIGIN in its retrace handler, many monitor entries long); the
// FIFO is read ahead from the latch, and the first row is needed LATCH_EARLY + 1 lines on. (Was VRES + 1: 5 lines.)
localparam int LATCH_EARLY = 8;
wire  fb_vid = ce && hcount == 0 && vcount == VSCAN - 10'(LATCH_EARLY);
logic epoch_vid;
always_ff @(posedge clk_vid) if (rst_vid) epoch_vid <= 0; else if (fb_vid) epoch_vid <= ~epoch_vid;

// ------------------------------------------------------------ vid -> sys pulses
// the VI interrupt comes every field (264 lines, 208,542 clocks of clk_vid)
logic t_line, t_frame, t_fb;
wire  fld_pulse = ce && hcount == 0 && vcount == VRES - 10'd4;
always_ff @(posedge clk_vid)
    if (rst_vid) begin t_line <= 0; t_frame <= 0; t_fb <= 0; end
    else begin
        if (ce && hcount == 0) t_line <= ~t_line;
        if (fld_pulse) t_frame <= ~t_frame;     // 4 lines (a quarter of a millisecond) before the frame boundary: the VI interrupt that a game answers with its new origin
        if (fb_vid) t_fb <= ~t_fb;
    end
logic [2:0] s_line, s_frame, s_fb;
always_ff @(posedge clk)
    if (rst) begin s_line <= 0; s_frame <= 0; s_fb <= 0; end
    else begin s_line <= {s_line[1:0], t_line}; s_frame <= {s_frame[1:0], t_frame}; s_fb <= {s_fb[1:0], t_fb}; end
wire fb_sys = s_fb[2] ^ s_fb[1];
// vblank (sys): the frame's blank
logic [2:0] s_vb;
always_ff @(posedge clk) s_vb <= {s_vb[1:0], !vactive};
assign vblank = s_vb[2];
always_ff @(posedge clk) begin vi_line <= s_line[2] ^ s_line[1]; vi_frame <= s_frame[2] ^ s_frame[1]; vi_latch <= fb_sys; end

// sys: the frame counter's count -- VI origin changes (a frame the game
// showed) in BCD, taken every 60 VI frames (50 in PAL: a second; the OSD's corner shows it)
logic [25:0] org_d;
logic [7:0]  fps_cnt, fps_bcd;
logic [5:0]  fps_v;
always_ff @(posedge clk) begin
    org_d <= vi_origin;
    if (rst) begin fps_cnt <= 0; fps_bcd <= 0; fps_v <= 0; end
    else if (vi_frame && fps_v == (tv_pal ? 6'd49 : 6'd59)) begin fps_v <= 0; fps_bcd <= fps_cnt; fps_cnt <= 0; end
    else begin
        if (vi_frame) fps_v <= fps_v + 6'd1;
        if (org_d != vi_origin)
            fps_cnt <= fps_cnt[3:0] == 4'd9 ? {fps_cnt[7:4] + 4'd1, 4'd0} : fps_cnt + 8'd1;
    end
end

// ------------------------------------------------------------------ sys: DMA
logic        epoch_sys, running;
logic [9:0]  issued, got;          // (the reads in flight are their difference)
// the picture: its rows of `rw` words, and its place in the frame (a
// framebuffer smaller than the frame is shown whole, a pixel a pixel, black
// around it) -- latched at the boundary, for the vid side's window too
logic        all;
logic [8:0]  rw;
logic [8:0]  orow, nr, x0_f, y0_f;
logic        dec_f;                // the picture is a hi-res one shown at half its width (its pixel pairs averaged)
logic [8:0]  nrm1;
logic [8:0]  col;                  // the read's word in its row
logic [23:0] raddr;                // the next read's word address
localparam int FAW = 14;           // the FIFO: 2^14 words
logic [9:0]  drop;                 // the last frame's reads still to come back (not written)
logic [FAW:0] wlevel;              // the FIFO's fill as sys sees it (words)
wire         keep = m_rvalid && !fb_sys && drop == 0;
// the next frame's picture
wire         hires  = |vi_width[11:9];                       // 512 or more: 640 wide (vi_tall: 480 rows, else 240)
wire         dec    = hires;                                 // every other pixel, in the 320 x 240 frame
wire  [8:0]  vi_rw  = hires ? 9'd320 : (vi_width[8:1] == 0 || vi_width[8:1] > 8'd160) ? 9'd160 : 9'(vi_width[8:1]);
wire  [8:0]  vi_nr  = (vi_rows == 0 || vi_rows > 9'd240) ? 9'd240 : vi_rows;   // (a 480-row picture's: a field's rows)
logic        il_f;                 // the picture is a 480-row one: its first field only, the buffer's even rows
// a word as the FIFO takes it: two pixels, the left one in the low half --
// RGBA5551 as the game's (N64FB: its bit 0 the RDP's coverage, "complete"),
// or MRDP's RGB565
wire  [31:0] rd_w = N64FB ? {m_rdata[15:0], m_rdata[31:16]} : m_rdata;
// MIRLO_VI_AA: each word goes with its pixels' hidden coverage bits (the RDP's
// plane, 1 MiB below the RDRAM: lang/c/mrdp/mrdp.h, layout.h) -- a group's
// word (16 pixels) is read before the group's first colour word (and at the
// frame's start), and a tag a read tells the words back apart: the hidden
// word is held, a colour word takes its pair's four bits (the even pixel's in
// [1:0]). The FIFO word: {epoch, the pair's hidden bits, the two pixels}.
localparam int FW = 37;
logic        h_next;                // the next read is the group's hidden word
logic [31:0] hreg;
logic [3:0]  tq [0:31];             // the reads in flight: {hidden, the colour word's place in its group}
logic [4:0]  tq_w, tq_r;
wire  [5:0]  tq_n = 6'(tq_w - tq_r);
wire  [3:0]  tq_h = tq[tq_r];
wire  [3:0]  h_nib = hreg[{tq_h[2:0], 2'b00} +: 4];
wire         m_hid = h_next;
always_ff @(posedge clk) begin
    if (rst) begin epoch_sys <= 0; running <= 0; issued <= 0; got <= 0; drop <= 0;
                   col <= 0; raddr <= 0; all <= 0; tv_pal <= 0; end
    else begin
        if (fb_sys) begin
            epoch_sys <= ~epoch_sys;
            running <= enable;
            tv_pal <= vi_pal;
            raddr <= vi_origin[25:2] & ORIGIN_MASK;   // SDRAM word address
            il_f <= vi_tall;
            orow <= 0; all <= 0;
            rw <= vi_rw; nr <= vi_nr; nrm1 <= vi_nr - 9'd1;
            x0_f <= 9'(vi_x0); y0_f <= 9'(vi_y0); dec_f <= dec;
            issued <= 0; got <= 0; col <= 0;
            drop <= issued - got - 10'(m_rvalid);
            h_next <= 1'b1;
        end else begin
            if (m_valid && m_ready && m_hid) begin
                issued <= issued + 10'd1;
                h_next <= 1'b0;
            end else if (m_valid && m_ready) begin
                issued <= issued + 10'd1;
                raddr <= raddr + 24'd1;
                if (raddr[2:0] == 3'd7) h_next <= 1'b1;   // the next word starts a group
                if (col == rw - 9'd1) begin               // a row's last word
                    col <= 0;
                    if (il_f) raddr <= raddr + 24'd321;   // (a 480-row picture: the next row but one)
                    orow <= orow + 9'd1; all <= orow == nrm1;
                end
                else col <= col + 9'd1;
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
        if (m_valid && m_ready) begin tq[tq_w] <= {m_hid, raddr[2:0]}; tq_w <= tq_w + 5'd1; end
        if (m_rvalid) tq_r <= tq_r + 5'd1;
    end
    if (m_rvalid && tq_h[3]) hreg <= m_rdata;
end
wire        f_we = keep && !tq_h[3];
wire [35:0] f_wd = {h_nib, rd_w};
// ask while the frame is not all asked for and the FIFO has room for what is in flight
// (room and urgency registered: the FIFO's level must not reach the
// arbiter's grant combinationally; the level is a few cycles late anyway, and
// the margin to 512 covers)
wire [15:0] inflight = 16'(issued - got);                // (below 1024)
logic       room;
always_ff @(posedge clk) room <= (16'(wlevel) + inflight) < 16'((1 << FAW) - 64);
assign m_urgent = 1'b0;
assign m_valid  = running && !fb_sys && drop == 0 && room
                  && !all && tq_n < 6'd30;
assign m_addr   = m_hid ? 24'hDC_0000 + {6'd0, raddr[20:3]} : raddr;   // (the plane: SDRAM 0x0370_0000)

// ------------------------------------------------------------------ the FIFO
logic        f_rd, f_empty;
logic [FW-1:0] f_q;
afifo #(.W(FW), .AW(FAW)) fifo (
    .wclk(clk), .wrst(rst), .w_en(f_we), .w_data({epoch_sys, f_wd}), .w_level(wlevel),
    .rclk(clk_vid), .rrst(rst_vid), .r_en(f_rd), .r_data(f_q), .r_empty(f_empty)
);

// --------------------------------------------------------------- vid: pixels
// a word: two pixels, low half first. Stale words (the last frame's epoch)
// are dropped whenever they come up. A word not there by its second pixel is
// black and owed (debt): it is dropped when it arrives, so the rest of the
// frame stays where it belongs.
// The picture's place on the screen (latched by sys at the boundary): pixel
// hcount - 1 of line vcount (1 up); outside it nothing is taken and the pixel
// is black.
logic [30:0] win_s1, win_s2;
always_ff @(posedge clk_vid) begin win_s1 <= {dec_f, nr, y0_f[5:0], x0_f[5:0], rw}; win_s2 <= win_s1; end
wire  [9:0]  px0 = 10'(win_s2[14:9]), py0 = 10'(win_s2[20:15]);
wire         dec_v = win_s2[30];                       // a hi-res picture in 320 x 240: a word a pixel, its left one
wire         inwin = (dec_v || (hcount > px0 && hcount <= px0 + {win_s2[8:0], 1'b0}))
                     && vc_f > py0 && vc_f <= py0 + 10'(win_s2[29:21]);
logic        half;                 // the next pixel is the word's high half
logic [15:0] pix;
logic [1:0]  phid;                 // its hidden coverage bits (MIRLO_VI_AA)
logic        pv;                   // pix is the picture's (else black)
logic [15:0] debt;
wire         stale = !f_empty && f_q[FW-1] != epoch_vid;
wire         owed  = !f_empty && !stale && debt != 0;
// a hi-res picture shown at half its width: the word's two pixels averaged, per channel (a + b + 1) >> 1 -- a 2 x 1 box on
// what the word already has (no buffer); a 565 picture takes the left one
function automatic logic [15:0] havg(input logic [15:0] a, b);
    if (!N64FB) return a;
    else return {5'((6'(a[15:11]) + 6'(b[15:11]) + 6'd1) >> 1), 5'((6'(a[10:6]) + 6'(b[10:6]) + 6'd1) >> 1),
                 5'((6'(a[5:1]) + 6'(b[5:1]) + 6'd1) >> 1), a[0] & b[0]};
endfunction
wire         take  = de_f && inwin && (half || dec_v);  // the word's second pixel (or its only one)
always_comb f_rd = ce && (stale || owed || (take && !f_empty));
always_ff @(posedge clk_vid) begin
    if (rst_vid) begin half <= 0; pix <= 0; debt <= 0; end
    else if (ce) begin
        if (fb_vid) begin half <= 0; debt <= 0; end
        else if (owed && !(take && f_empty)) debt <= debt - 16'd1;
        else if (take && f_empty && !owed) debt <= debt + 16'd1;
        pv <= de_f && inwin;
        if (de_f) begin
            if (inwin && !f_empty && !stale && !owed) pix <= dec_v ? havg(f_q[15:0], f_q[31:16]) : half ? f_q[31:16] : f_q[15:0];
            else pix <= 16'd0;
            phid <= half ? f_q[35:34] : f_q[33:32];
            if (inwin && !dec_v) half <= ~half;
        end
    end
end
// ---- the pixel to the pads (pq, R8 G8 B8) and the timing it goes with
logic [23:0] pq;
logic        de_q, hs_q, vs_q, cr_q;
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
// ---- MIRLO_VI_AA: the N64 VI's anti-alias and divot filters (the console's
// VI_filter / VI_filter_pen, Angrylion's), on a pixel whose coverage is not
// complete (the RDP's three bits: bit 0 and the hidden two, fetched with it).
// The rows above and below: two line buffers of the rows fetched (a row is
// shown the line after it is fetched), a pixel, its hidden bits and its valid
// bit, read on the pixel's own clock (ce) at its slot and written
// back on the next: lb1 the row just fetched, lb2 the one before. The window:
// three rows by five columns, column k the pixel k slots back (the centre is
// k = 2). Taps: (x-1, x+1) above and below, (x-2, x+2) beside; one that is
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
// three. Delay: six pixels (the timing is delayed to match).
logic [1:0]  aa_s, dv_s, dd_s;
always_ff @(posedge clk_vid) begin aa_s <= {aa_s[0], vi_aa}; dv_s <= {dv_s[0], vi_divot}; dd_s <= {dd_s[0], vi_dedither}; end
// (a hi-res picture shown at half its width, dec_v: the filters are off -- their taps are the neighbours of the full-size
// picture, and the line buffers are a 320-wide frame's)
wire         aa_on = aa_s[1] && N64FB && !dec_v;
wire         dd_on = dd_s[1] && N64FB && !dec_v;
wire         dv_on = dv_s[1] && aa_on;
(* ramstyle = "M10K, no_rw_check" *) logic [18:0] lb1 [0:511];
(* ramstyle = "M10K, no_rw_check" *) logic [18:0] lb2 [0:511];
logic [18:0] lq1, lq2;
logic [8:0]  lwa;
wire  [8:0]  lra = hcount - 9'd1;     // the slot whose pixel pix takes on this ce
// the window: {valid, hidden 2, RGBA5551} (the coverage: {bit 0, hidden})
logic [18:0] wa [0:3], wc [0:4], wb [0:3];
integer wi;
always_ff @(posedge clk_vid) if (ce) begin
    lq1 <= lb1[lra]; lq2 <= lb2[lra]; lwa <= lra;
    // the slot of the last ce: pix, and what was above it
    lb1[lwa] <= {pv, phid, pix};
    lb2[lwa] <= lq1;
    wa[0] <= lq2; wc[0] <= lq1; wb[0] <= {pv, phid, pix};
    for (wi = 1; wi < 4; wi = wi + 1) begin wa[wi] <= wa[wi - 1]; wb[wi] <= wb[wi - 1]; end
    for (wi = 1; wi < 5; wi = wi + 1) wc[wi] <= wc[wi - 1];
end
// second smallest and second largest of six (11 compares)
function automatic logic [9:0] mm2(input logic [4:0] a, b, c, d, e, f);
    logic [4:0] l0, h0, l1, h1, l2, h2, sa, sb, la, lb;
    l0 = a < b ? a : b; h0 = a < b ? b : a;
    l1 = c < d ? c : d; h1 = c < d ? d : c;
    l2 = e < f ? e : f; h2 = e < f ? f : e;
    // the smallest low's pair: the second smallest is the least of the other lows and that pair's high
    if (l0 <= l1 && l0 <= l2)  begin sa = l1 <= l2 ? l1 : l2; sb = h0; end
    else if (l1 <= l2)         begin sa = l0 <= l2 ? l0 : l2; sb = h1; end
    else                       begin sa = l0 <= l1 ? l0 : l1; sb = h2; end
    if (h0 >= h1 && h0 >= h2)  begin la = h1 >= h2 ? h1 : h2; lb = l0; end
    else if (h1 >= h2)         begin la = h0 >= h2 ? h0 : h2; lb = l1; end
    else                       begin la = h0 >= h1 ? h0 : h1; lb = l2; end
    return {sa <= sb ? sa : sb, la >= lb ? la : lb};
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
always_ff @(posedge clk_vid) if (ce) begin
    fa0 <= !ctr[18] ? 24'd0 : aa_px ? {pen(ctr[15:11], mr, cw), pen(ctr[10:6], mg, cw), pen(ctr[5:1], mb, cw)}
         : dd_on ? dd8 : rgb8(ctr[15:0]);
    fe[0] <= aa_px; fv[0] <= ctr[18];
    fa1 <= fa0; fa2 <= fa1; fe[2:1] <= fe[1:0]; fv[2:1] <= fv[1:0];
end
// divot: the median of three, by channel (a neighbour not in the picture is the centre)
function automatic logic [7:0] med3(input logic [7:0] a, b, c);
    logic [7:0] lo, hi, m;
    lo = a < b ? a : b; hi = a < b ? b : a;
    m  = hi < c ? hi : c;
    return lo > m ? lo : m;                 // max(min(a, b), min(max(a, b), c))
endfunction
wire [23:0] dl = fv[2] ? fa2 : fa1, dr = fv[0] ? fa0 : fa1;
wire        dv_px = dv_on && fv[1] && (fe[0] || fe[1] || fe[2]);
// the timing, six pixels on
logic [5:0] de_sh, hs_sh, vs_sh, cr_sh;
always_ff @(posedge clk_vid) if (ce) begin
    pq <= dv_px ? {med3(dl[23:16], fa1[23:16], dr[23:16]), med3(dl[15:8], fa1[15:8], dr[15:8]), med3(dl[7:0], fa1[7:0], dr[7:0])} : fa1;
    de_sh <= {de_sh[4:0], de}; hs_sh <= {hs_sh[4:0], hsync}; vs_sh <= {vs_sh[4:0], vsync}; cr_sh <= {cr_sh[4:0], crop};
end
assign de_q = de_sh[5]; assign hs_q = hs_sh[5]; assign vs_q = vs_sh[5]; assign cr_q = cr_sh[5];

// ---- the OSD
function automatic logic [7:0] font(input logic [3:0] d, input logic [2:0] r);  // 5x7 hex digits
    logic [4:0] f;
    case ({d, r})
    7'h00: f = 5'h0E;
    7'h01: f = 5'h11;
    7'h02: f = 5'h13;
    7'h03: f = 5'h15;
    7'h04: f = 5'h19;
    7'h05: f = 5'h11;
    7'h06: f = 5'h0E;
    7'h08: f = 5'h04;
    7'h09: f = 5'h0C;
    7'h0A: f = 5'h04;
    7'h0B: f = 5'h04;
    7'h0C: f = 5'h04;
    7'h0D: f = 5'h04;
    7'h0E: f = 5'h0E;
    7'h10: f = 5'h0E;
    7'h11: f = 5'h11;
    7'h12: f = 5'h01;
    7'h13: f = 5'h02;
    7'h14: f = 5'h04;
    7'h15: f = 5'h08;
    7'h16: f = 5'h1F;
    7'h18: f = 5'h1F;
    7'h19: f = 5'h02;
    7'h1A: f = 5'h04;
    7'h1B: f = 5'h02;
    7'h1C: f = 5'h01;
    7'h1D: f = 5'h11;
    7'h1E: f = 5'h0E;
    7'h20: f = 5'h02;
    7'h21: f = 5'h06;
    7'h22: f = 5'h0A;
    7'h23: f = 5'h12;
    7'h24: f = 5'h1F;
    7'h25: f = 5'h02;
    7'h26: f = 5'h02;
    7'h28: f = 5'h1F;
    7'h29: f = 5'h10;
    7'h2A: f = 5'h1E;
    7'h2B: f = 5'h01;
    7'h2C: f = 5'h01;
    7'h2D: f = 5'h11;
    7'h2E: f = 5'h0E;
    7'h30: f = 5'h06;
    7'h31: f = 5'h08;
    7'h32: f = 5'h10;
    7'h33: f = 5'h1E;
    7'h34: f = 5'h11;
    7'h35: f = 5'h11;
    7'h36: f = 5'h0E;
    7'h38: f = 5'h1F;
    7'h39: f = 5'h01;
    7'h3A: f = 5'h02;
    7'h3B: f = 5'h04;
    7'h3C: f = 5'h08;
    7'h3D: f = 5'h08;
    7'h3E: f = 5'h08;
    7'h40: f = 5'h0E;
    7'h41: f = 5'h11;
    7'h42: f = 5'h11;
    7'h43: f = 5'h0E;
    7'h44: f = 5'h11;
    7'h45: f = 5'h11;
    7'h46: f = 5'h0E;
    7'h48: f = 5'h0E;
    7'h49: f = 5'h11;
    7'h4A: f = 5'h11;
    7'h4B: f = 5'h0F;
    7'h4C: f = 5'h01;
    7'h4D: f = 5'h02;
    7'h4E: f = 5'h0C;
    7'h50: f = 5'h0E;
    7'h51: f = 5'h11;
    7'h52: f = 5'h11;
    7'h53: f = 5'h1F;
    7'h54: f = 5'h11;
    7'h55: f = 5'h11;
    7'h56: f = 5'h11;
    7'h58: f = 5'h1E;
    7'h59: f = 5'h11;
    7'h5A: f = 5'h11;
    7'h5B: f = 5'h1E;
    7'h5C: f = 5'h11;
    7'h5D: f = 5'h11;
    7'h5E: f = 5'h1E;
    7'h60: f = 5'h0E;
    7'h61: f = 5'h11;
    7'h62: f = 5'h10;
    7'h63: f = 5'h10;
    7'h64: f = 5'h10;
    7'h65: f = 5'h11;
    7'h66: f = 5'h0E;
    7'h68: f = 5'h1C;
    7'h69: f = 5'h12;
    7'h6A: f = 5'h11;
    7'h6B: f = 5'h11;
    7'h6C: f = 5'h11;
    7'h6D: f = 5'h12;
    7'h6E: f = 5'h1C;
    7'h70: f = 5'h1F;
    7'h71: f = 5'h10;
    7'h72: f = 5'h10;
    7'h73: f = 5'h1E;
    7'h74: f = 5'h10;
    7'h75: f = 5'h10;
    7'h76: f = 5'h1F;
    7'h78: f = 5'h1F;
    7'h79: f = 5'h10;
    7'h7A: f = 5'h10;
    7'h7B: f = 5'h1E;
    7'h7C: f = 5'h10;
    7'h7D: f = 5'h10;
    7'h7E: f = 5'h10;
    default: f = 5'h00;
    endcase
    return {1'b0, f, 2'b00};
endfunction
localparam int OX = 4, OY = 4;             // the box's top left (active pixels)
wire  [9:0] ax = hcount - 10'd1 - 10'(OX), ay = vcount - 10'd1 - 10'(OY);
wire        in_box = osd_en && de && ax < 9'(17 * 8) && ay < 9'(8 * 8);
// the frame counter (fps_en, without the OSD): the frames the game showed --
// VI origin changes -- over the last 60 VI frames, two decimal digits in the
// top left corner
logic [7:0] fps_s1, fps_s2;
always_ff @(posedge clk_vid) begin fps_s1 <= fps_bcd; fps_s2 <= fps_s1; end
wire        in_fps = fps_en && !osd_en && de && ax < 9'd16 && ay < 9'd8;
logic       s1_in, s1_sp;
logic [2:0] s1_px, s1_py;
logic [3:0] s1_nib;
logic       osd_in, osd_px;
wire  [7:0] glyph = font(s1_nib, s1_py);
always_ff @(posedge clk_vid) if (ce) begin
    // stage 1: the character and its digit
    s1_in <= in_box || in_fps;
    s1_px <= ax[2:0]; s1_py <= ay[2:0];
    s1_sp <= in_box && ax[7:3] == 5'd8;                 // the column between the two words
    s1_nib <= in_fps ? (ax[3] ? fps_s2[3:0] : fps_s2[7:4])
                     : osd_val[{ay[5:3], ax[7:3] > 5'd8}][4 * (7 - (ax[7:3] > 5'd8 ? ax[7:3] - 5'd9 : ax[7:3])) +: 4];
    // stage 2: the pixel
    osd_in <= s1_in;
    osd_px <= !s1_sp && glyph[3'd7 - s1_px];
end

// the VI's window (slow-changing, from sys): black while H_START is 0 --
// libultra's osViBlack, the game's level changes, when the framebuffer is
// being rewritten -- and below its last line (V_START..V_END: SM64 shows
// 237 lines from its origin, one line into the framebuffer: the 240th
// would be the next framebuffer's first, or the memory past the last one)
logic [1:0] blk_s; logic [8:0] lin_s1, lin_s2;
always_ff @(posedge clk_vid) begin blk_s <= {blk_s[0], vi_black}; lin_s1 <= vi_lines; lin_s2 <= lin_s1; end
// The core's video options: the picture's first and last crop_v lines and its last crop_r columns, blanked -- relative to the
// picture (a window too), the frame and its timing unchanged. Thresholds
// in the timing's own counts (the window's are inwin's), registered: static.
logic [6:0] cv_s1, cv_s2;
logic [9:0] crop_top, crop_bot, crop_rgt;
wire  [9:0] cvl = {5'd0, cv_s2[4:0]};                 // (a field's lines, as the N64's)
always_ff @(posedge clk_vid) begin
    cv_s1 <= {crop_r, crop_v}; cv_s2 <= cv_s1;
    crop_top <= py0 + cvl;
    crop_bot <= py0 + 10'(win_s2[29:21]) - cvl;
    crop_rgt <= px0 + (dec_v ? 10'(win_s2[8:0]) : {win_s2[8:0], 1'b0}) - {8'd0, cv_s2[6:5]};
end
wire  crop = blk_s[1] || vcount > {1'b0, lin_s2}      // (active lines are vcount 1..240)
          || vcount <= crop_top || vcount > crop_bot || hcount > crop_rgt;

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
// outputs, behind the timing by the pixel's register and the OSD's two stages.
// Every stage moves on a pixel's clock only, so a two-clock pixel's pads hold
// still for both; vga_skip is low on the clock the pads have just changed on
// (core_top sends it on with the pads, one register later) and high on the other
logic de_d, hs_d, vs_d, de_d2, hs_d2, vs_d2, cr_d, cr_d2;
logic [23:0] pix_d;
wire  [7:0]  o_r = gam_s[1] ? gq_r : pix_d[23:16];
wire  [7:0]  o_g = gam_s[1] ? gq_g : pix_d[15:8];
wire  [7:0]  o_b = gam_s[1] ? gq_b : pix_d[7:0];
always_ff @(posedge clk_vid) begin
    if (ce) begin
        de_d <= de_q; hs_d <= hs_q; vs_d <= vs_q; cr_d <= cr_q;
        de_d2 <= de_d; hs_d2 <= hs_d; vs_d2 <= vs_d; cr_d2 <= cr_d; pix_d <= pq;
        gq_r <= gtab[pq[23:16]]; gq_g <= gtab[pq[15:8]]; gq_b <= gtab[pq[7:0]];
        vga_de <= de_d2; vga_hsync <= hs_d2; vga_vsync <= vs_d2;
        vga_r <= !de_d2 ? 8'd0 : osd_in ? {8{osd_px}} : cr_d2 ? 8'd0 : o_r;
        vga_g <= !de_d2 ? 8'd0 : osd_in ? {8{osd_px}} : cr_d2 ? 8'd0 : o_g;
        vga_b <= !de_d2 ? 8'd0 : osd_in ? {8{osd_px}} : cr_d2 ? 8'd0 : o_b;
    end
    vga_skip <= !ce;
end

endmodule
`default_nettype wire
