// The N64's VI (Video Interface) registers in hardware (MIRLO_VI; docs/mips.md "The VI in hardware").
//
// VI, 0x0440_0000 (the game's lw / sw reach it as a device access, as the MI's do: mips_core's VIW, mirlo_mips.sv's
// decode; its registers are the first 0x38 bytes of the 1 MiB block):
//   00 STATUS  04 ORIGIN  08 WIDTH  0C V_INTR  10 CURRENT  14 BURST  18 V_SYNC  1C H_SYNC  20 LEAP
//   24 H_START 28 V_START 2C V_BURST 30 X_SCALE 34 Y_SCALE
// Every register but CURRENT reads back what was written (a block RAM of 16 words: no flip-flop of its own). What the
// hardware itself does with them:
//   CURRENT  read: a half-line counter, {nl[8:0], field}: the line (0..261) of the N64's field, which the scan-out's
//            line pulses count (264 lines of the scan-out, so the N64's line 0 is held for three of them), bit 0 the
//            interlace field (0 unless STATUS' SERRATE, bit 6, is set: the field alternates every 264 lines). The line
//            18 is the scan-out's frame boundary (the latch of its base, `sc_latch`): VI_V_START's first line, where
//            the VI takes its ORIGIN. A write clears the VI interrupt (vi_clr).
//   V_INTR   the VI interrupt (MI_INTR's VI) rises, once a field, when the line counter reaches V_INTR / 2 (the
//            half-line's bit 0 is not compared: an interlaced mode's two fields both match). libultra's value 2 is the
//            line 1: 17 lines (a millisecond) before the latch, for the game's retrace handler to write its ORIGIN.
//   ORIGIN   the scan-out's base: the RDRAM's address, taken by it at its frame boundary. A hi-res (WIDTH 512 or more)
//            picture the game gives as two fields' origins one row of the buffer apart (libultra: the second is the first
//            + WIDTH * 2 bytes) is one frame of both, from the lower: the 640 x 480 frame -- a write of the
//            base + one row leaves the base as it is. A serrated picture is sent a field at a time, as the VI sends it:
//            each from the origin the game wrote last (`base_f`), which libultra's retrace handler picks by CURRENT's
//            field bit (`fld`, below; rtl/soc/n64_video.sv).
//   STATUS   GAMMA (bit 3), taken at the latch, is the scan-out's gamma correction; AA_MODE (bits 9:8, 0 or 1:
//            anti-alias) and DIVOT_ENABLE (bit 4) its filters (MIRLO_VI_AA).
//   H_START  0: black (osViBlack) -- the scan-out reads nothing from the next frame on -- and so is an origin that has
//            the picture out of the game's RDRAM (`thr`, MONITOR.VITHR: the last origin, in 64-byte units, a picture
//            of the mode's size fits at; the runtime keeps it).
//   V_SYNC   the TV system of the scan-out's field: once the game has written one, 525 lines (0x20D: NTSC, MPAL, a PAL console's
//            60 Hz modes; 512..575) or 625 (0x271: PAL; 576..703: 50 Hz, 313 lines of the scan-out, rtl/soc/n64_video.sv); a
//            value that is neither (0, noise) changes nothing; before the first, and after a reset, it is `tv_def`, the
//            runtime's choice from the ROM's region (n64rt_boot.c). The
//            half-line counter of a PAL field is 312 lines (CURRENT 0..623), VI_V_START's first line 47 (libultra's PAL modes:
//            half-line 95) where the NTSC modes' is 18.
//   WIDTH, H_START, V_START, X_SCALE, Y_SCALE   what the scan-out's window and size come from (the runtime computes them: n64rt_io.c, and
//            is told when one of them has changed -- `chg`, a pulse -- reading the registers back).
// The accesses are sequenced by mirlo_mips.sv: the address is held while the access lasts; for a read, `rd` is the answer
// in the cycle after the address (rd_s); a write is done in the cycle `wr_s` is up.
`default_nettype none

module rcp_vi (
    input  wire         clk, rst,
    // the scan-out's pulses (sys): a line began; its frame boundary (the base's latch)
    input  wire         sc_line, sc_latch,
    input  wire         tv_pal,         // the scan-out's field is PAL (n64_video's tv_pal, taken at the frame boundary)
    input  wire         tv_def,         // the TV system before the game writes V_SYNC (the runtime's, from the ROM's region)
    output wire         vi_pal,         // the TV system asked for: n64_video's vi_pal
    input  wire  [16:0] thr,
    // the game's / the runtime's access
    input  wire  [3:0]  a,              // the register: x_addr[5:2]
    input  wire  [31:0] wd,
    input  wire         wr_s,
    output logic [31:0] rd,
    output wire         vi_clr,         // a write to CURRENT (the MI's VI bit is cleared)
    // events
    output logic        irq,            // a pulse: the line V_INTR names began
    output logic        chg,            // a pulse: WIDTH, H_START, V_START, X_SCALE or Y_SCALE changed
    // the scan-out's
    output wire  [25:0] base,           // the framebuffer's byte address in the SDRAM
    output wire  [25:0] base_f,         // the origin the game wrote last: a serrated picture's field's
    output wire         il,             // a serrated picture: SERRATE (VI_CTRL bit 6), hi-res (sent as 480i, a field at a time)
    output wire         fld,            // the interlace field of the N64's field under way (CURRENT's bit 0 while SERRATE is set)
    output logic        black,
    output logic        gamma,
    output logic        aa,             // AA_MODE 0 or 1: the scan-out's anti-alias filter (MIRLO_VI_AA)
    output logic        divot,          // DIVOT_ENABLE
    output logic        dedither        // DITHER_FILTER_ENABLE (bit 16)
);

// ---------------------------------------------------------------- the registers: a RAM; what the hardware uses
(* ramstyle = "M10K, no_rw_check" *) reg [31:0] m [0:15];
reg [31:0] q;
integer ii;
initial for (ii = 0; ii < 16; ii = ii + 1) m[ii] = 32'd0;
wire wr_ok = wr_s && a <= 4'd13 && a != 4'd4;
always @(posedge clk) begin
    if (wr_ok) m[a] <= wd;
    q <= m[a];
end

logic [22:0] org_b;             // the base: the origin the scan-out takes
logic [22:0] org_r;             // the origin as the game wrote it last
logic [11:0] wid;
logic [8:0]  vint;
logic        st_gamma, st_serr, hs_zero, gm_f;
logic        st_aa, st_dv, st_dd;
logic        vs_set, vs_pal;            // V_SYNC has been written; and it names 625 lines (576 or more)
assign vi_pal = vs_set ? vs_pal : tv_def;
// a hi-res picture's second field's origin: the base + one row of the 640-pixel picture (1280 bytes), which the base does not follow: with
// WIDTH 640 (libultra's interlaced modes: Y_SCALE 2.0) that is WIDTH * 2 bytes; with WIDTH 1280 (Donkey Kong 64's: the
// buffer's pitch is two rows, Y_SCALE 1.0, so a field is every other row) it is WIDTH bytes
wire         row_up = |wid[11:9] && (wd[22:0] == org_b + {10'd0, wid, 1'b0} || (|wid[11:10] && wd[22:0] == org_b + {11'd0, wid}));
logic [8:0]  nl;
logic [1:0]  hold;
logic        field;
assign vi_clr = wr_s && a == 4'd4;
always_ff @(posedge clk) begin
    if (rst) begin org_b <= 23'd0; org_r <= 23'd0; wid <= 12'd0; vint <= 9'd0; st_gamma <= 1'b0; st_serr <= 1'b0; hs_zero <= 1'b1; st_aa <= 1'b0; st_dv <= 1'b0; st_dd <= 1'b0; vs_set <= 1'b0; vs_pal <= 1'b0; end
    else if (wr_s) case (a)
    4'd0: begin st_gamma <= wd[3]; st_serr <= wd[6]; st_aa <= !wd[9]; st_dv <= wd[4]; st_dd <= wd[16]; end
    4'd1: begin if (!row_up) org_b <= wd[22:0]; org_r <= wd[22:0]; end
    4'd2: wid <= wd[11:0];
    4'd3: vint <= wd[9:1];
    4'd6: begin                                      // a value that names a TV system sets it; any other (0, noise) leaves it as it was
        if (wd[9:6] == 4'b1000)                  begin vs_set <= 1'b1; vs_pal <= 1'b0; end      // 512..575: 525 lines
        else if (wd[9:6] == 4'b1001 || wd[9:6] == 4'b1010) begin vs_set <= 1'b1; vs_pal <= 1'b1; end      // 576..703: 625 lines
    end
    4'd9: hs_zero <= wd == 32'd0;
    default: ;
    endcase
end

// a register the window is computed from has changed (q is the register's old word)
wire [31:0] dx = q ^ wd;
wire        chg_now = wr_s && ((a == 4'd2 && |dx[11:0]) || (a == 4'd10 && |(dx & 32'h03FF_03FF)) || (a == 4'd13 && |dx[11:0]) || (a == 4'd12 && |dx[11:0]) || (a == 4'd9 && |dx[25:0]));
always_ff @(posedge clk) chg <= !rst && chg_now;

// ---------------------------------------------------------------- the half-line counter
// The scan-out's field is 264 lines; the N64's 262. nl counts the line pulses, wraps at 261 and holds the line 0 for
// three of them (a field of 264: 235, 236, 237 are the line 0, the line 1 at 238, the line 18 at 255, the latch). The
// latch sets the counter, so what the two clocks of a size change do to the count is made good in a frame.
// PAL: a field of 313 lines of the scan-out, the N64's 312: nl wraps at 311, the line 0 is held for two (the line 1 at 282, the line
// 47, the latch, at 304; the half-line 95 of libultra's PAL modes).
localparam logic [8:0] NL_LATCH_N = 9'd18, NL_LATCH_P = 9'd47;
localparam logic [8:0] NL_LAST_N = 9'd261, NL_LAST_P = 9'd311;
wire [8:0]  NL_LATCH = tv_pal ? NL_LATCH_P : NL_LATCH_N;
wire [8:0]  NL_LAST  = tv_pal ? NL_LAST_P : NL_LAST_N;
wire        adv   = sc_line && hold == 2'd0;
wire [8:0]  nl_n  = sc_latch ? NL_LATCH : adv ? (nl == NL_LAST ? 9'd0 : nl + 9'd1) : nl;
always_ff @(posedge clk) begin
    irq <= 1'b0;
    if (rst) begin nl <= 9'd0; hold <= 2'd0; field <= 1'b0; end
    else begin
        nl <= nl_n;
        if (sc_latch) hold <= 2'd0;
        else if (sc_line) begin
            if (hold != 2'd0) hold <= hold - 2'd1;
            else if (nl == NL_LAST) begin hold <= tv_pal ? 2'd1 : 2'd2; field <= ~field; end
        end
        irq <= nl_n != nl && nl_n == vint;
    end
end

// ---------------------------------------------------------------- what the scan-out takes
assign base = {3'b111, org_b};
assign base_f = {3'b111, org_r};
assign il = st_serr && |wid[11:9];
assign fld = field;
always_ff @(posedge clk) begin
    black <= hs_zero || org_b[22:6] > thr;
    if (sc_latch) begin gm_f <= st_gamma; aa <= st_aa; divot <= st_dv; dedither <= st_dd; end
end
assign gamma = gm_f;

// ---------------------------------------------------------------- the read
always_comb begin
    rd = q;
    if (a == 4'd4) rd = {22'd0, nl, field & st_serr};
    else if (a > 4'd13) rd = 32'd0;
end

endmodule
`default_nettype wire
