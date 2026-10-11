// RDP memory engine: every access to the N64's memory, over a LiteDRAM-
// native-style port (commands and read data in order; write data after its
// command is accepted). The memory is the N64's: a 32-bit word is four bytes,
// the first in [31:24]; two 16-bit pixels, the even one in [31:16]. The line
// buffers hold the words as they are.
//
// Two engines share the command channel, a command a cycle between them
// (reads first: they are what the pixels wait for), so that a row's reads,
// its pixels and the write-back of the row before overlap:
//
//   the read engine    a row's Z and / or colour span [x0, x1) into the "old"
//                      line buffers (set `rd_set`), the Z span's words and
//                      then the colour span's;
//                      z_have / c_have count the words that are in. Or a
//                      texture load (lang/c/mrdp/mrdp.c tmem_load, load_tlut):
//                      `ln` 32-bit words from byte address `cbase` (any) to
//                      TMEM's 16-bit word `ldst`, by `lmode` -- 0 one after
//                      the other, 1 each word's halves to the two halves of
//                      TMEM (32-bit RGBA), 2 `ln` 16-bit TLUT entries, each
//                      four times over, 3 as 1 with a YUV pair's U Y0 V Y1 as
//                      U V to the lower half and Y0 Y1 to the upper.
//   the write engine   a row's results back from the "new" line buffers (set
//                      `wr_set`): a word goes when its pixels' results are in
//                      (res_a / res_b: the results so far, by set), the
//                      colour span's words and then, the span over again, Z's,
//                      only pixels whose mask bit is set (bit 0 the
//                      even pixel); words with none are not written. Or a
//                      fill: the span := the fill colour.
//
// A span is a run of words in one image, not a word of each image in turn: the
// memory keeps one row open in each of its banks, and a game's colour and depth
// images can be in the same bank word for word (their addresses equal modulo
// the banks' span: a depth image 2 MiB above a framebuffer). In turn, every
// word then closed the other image's row and opened its own -- some five
// clocks a word, a frame of such a game in twice the time of one whose images
// are in different banks. As runs it is a row opened a span. (The pixels wait
// for both spans' words either way, and take a clock each: two spans' words,
// a clock a word of two pixels, are in as the row's last pixel can go.)
//
// The hidden plane (lang/c/mrdp/mrdp.h, MRDP_HID_BASE): the N64's other two
// coverage bits of every 16-bit word, 16 of them a 32-bit word, at 0x0080_0000
// + (A[22:5] << 2) for RDRAM byte A (the SoC maps the address's bit 23 to its
// SDRAM). A pixel span reads its groups' words first (`rd_h`, into the raw
// hidden buffers: hlbw_*, by group mod 128, then h_done), the pixels take
// their old coverage there, and the write engine writes each group back with
// the span's new bits merged in (the new colour buffer's lanes carry them:
// {colour 16, hidden 2}), when a pixel of it was written. A fill writes, for
// each 4-pixel quad it touches, the quad's {a, a} by byte enables (no read).
//
// A row's base address (image + y * width * 2) must be 4-byte aligned
// (images 4-aligned, even widths). Nothing here orders a read after a write
// of the same word: the sequencer starts a primitive's reads, and a load,
// only with the write engine idle (rows of one primitive never share a word).
module rdp_mem (
    input  wire        clk,
    input  wire        rst,

    // ---- the read engine
    input  wire        rd_valid,
    output wire        rd_ready,
    input  wire        rd_load,
    input  wire        rd_z, rd_c,
    input  wire        rd_h,           // the colour span's hidden words, first
    input  wire [31:0] rd_zbase, rd_cbase,
    input  wire [9:0]  rd_x0,
    input  wire [10:0] rd_x1,          // exclusive, up to 1024
    input  wire        rd_set,
    input  wire [10:0] rd_ldst,
    input  wire [10:0] rd_ln,
    input  wire [1:0]  rd_lmode,
    input  wire [11:0] rd_ldxt,           // LOAD BLOCK: what a 64-bit word adds to its row count (u1.11)
    input  wire        rd_lodd,           // LOAD TILE: an odd row
    output reg  [9:0]  z_have, c_have,
    output reg         h_done,         // the span's hidden words are in
    output reg         hlbw_we,        // the raw hidden buffers' write port ({set, group mod 128})
    output reg  [7:0]  hlbw_addr,
    // the old line buffers' write port ({set, word})
    output reg         lbw_z_we,
    output reg         lbw_c_we,
    output reg  [9:0]  lbw_addr,
    output reg  [31:0] lbw_data,

    // ---- the write engine
    input  wire        wr_valid,
    output wire        wr_ready,
    input  wire        wr_fill,
    input  wire        wr_z,           // the Z span too (Z_UPD)
    input  wire [31:0] wr_cbase, wr_zbase,
    input  wire [9:0]  wr_x0,
    input  wire [10:0] wr_x1,
    input  wire [31:0] wr_fillc,
    input  wire        wr_set,
    input  wire [10:0] res_a, res_b,   // the row's results in the new buffers so far (pixels), by set
    // the new line buffers' read port ({set, word}); masks: bit 0 even pixel, bit 1 odd
    output reg  [9:0]  lbr_addr,
    input  wire [31:0] lbr_zdata,
    input  wire [35:0] lbr_cdata,      // {colour 16, hidden 2} x 2, the even pixel first
    output wire [7:0]  hlbr_addr,      // the raw hidden buffer's second read port ({set, group mod 128}), as lbr_addr
    input  wire [31:0] hlbr_data,
    input  wire [1:0]  lbr_zmask,
    input  wire [1:0]  lbr_cmask,

    // TMEM writes (rdp_tmem, every copy)
    output reg  [3:0]  tm_we,
    output reg  [8:0]  tm_wa,
    output reg  [15:0] tm_wd0, tm_wd1, tm_wd2, tm_wd3,

    // memory
    output reg         m_cmd_valid,
    input  wire        m_cmd_ready,
    output reg         m_cmd_we,
    output reg  [31:0] m_cmd_addr,      // byte address, 4-aligned
    output wire        m_wdata_valid,
    input  wire        m_wdata_ready,
    output wire [31:0] m_wdata,
    output wire [3:0]  m_wdata_we,      // [3] the first byte, bits 31:24
    input  wire        m_rdata_valid,
    input  wire [31:0] m_rdata,

    output wire        rd_busy,         // a read request (or load) in progress
    output wire        wr_busy,         // a write request in progress
    output wire        wr_seti,         // its set
    output wire        writes_pending   // write data not yet accepted
);
    wire cmd_free = !m_cmd_valid || m_cmd_ready;

    // ======================================================================
    // the read engine
    // ======================================================================
    reg        r_act, r_load, r_z, r_c, r_set;
    reg        r_h, r_hall, q_hin;            // hidden words: wanted; all asked for; still coming back
    // (a span is 65 groups at most: the last one is known by its low seven bits)
    reg [17:0] r_hg;                          // the group to ask for
    reg [6:0]  r_hgl, q_hg;                   // the last, the one to come back
    reg [29:0] r_zb, r_cb;                // the rows' word addresses
    reg [8:0]  r_idx, r_last;             // the word to ask for next, the last (the row's words: x >> 1)
    reg        r_tog, r_all;              // both spans: the colour span's words are next (Z's are out); every command is out
    reg [8:0]  r_first;                   // the row's first word (the colour span starts over from it)
    reg [8:0]  q_idx;                     // the word to come back next
    reg        q_tog;
    wire       r_both = r_z && r_c;
    wire       r_kc   = r_both ? r_tog : r_c;          // the next command is the colour span's
    wire       q_kc   = r_both ? q_tog : r_c;
    wire [10:0] rd_xl = rd_x1 - 11'd1;
    wire [29:0] rd_cw0 = rd_cbase[31:2] + {21'd0, rd_x0[9:1]}, rd_cwl = rd_cbase[31:2] + {21'd0, rd_xl[9:1]};
    assign rd_ready = !r_act;
    assign rd_busy  = r_act;

    // ---- loads
    reg [29:0] w_next;                    // the word to ask for
    reg [11:0] l_ask, l_get;              // the words still to ask for less one, still to come back
    reg        issued_all;
    reg [1:0]  lmode, lo;                 // the mode; the source's byte offset in its word
    reg [10:0] ldst;                      // TMEM's 16-bit word of the next aligned word (mode 1: the texel)
    reg [10:0] lleft;                     // aligned words (mode 2: entries) still to write
    reg        lfirst;                    // the first memory word has not come (an unaligned source: it only primes)
    reg [31:0] lprev;
    reg        la_v;                      // an aligned word waiting to be written
    reg [31:0] la;
    reg [1:0]  lt;                        // mode 2: which of its four writes
    // an odd row's 64-bit words go in with their halves changed places (the
    // N64's: undone as they are sampled, rdp_pix): the row count of a LOAD
    // BLOCK is its dxt summed over the words, a LOAD TILE's row is given
    reg [11:0] lacc, ldxt;
    reg        lhalf, lodd;
    wire       lsw = lodd | lacc[11];
    reg        l_out;                     // mode 2: a read is out (one at a time: an entry takes two cycles to write)
    // a load: its aligned words (a TLUT entry is half of one), and one more
    // memory word when it does not start on one
    wire [10:0] ld_words = (rd_lmode == 2'd2) ? {1'b0, rd_ln[10:1]} + {10'd0, rd_ln[0]} : rd_ln;
    wire [11:0] ld_reads = {1'b0, ld_words} + {11'd0, rd_cbase[1:0] != 2'd0};
    wire [31:0] l_aligned = (lo == 2'd0) ? m_rdata
                          : (lo == 2'd1) ? {lprev[23:0], m_rdata[31:24]}
                          : (lo == 2'd2) ? {lprev[15:0], m_rdata[31:16]}
                          :                {lprev[7:0],  m_rdata[31:8]};
    wire [9:0]  l_w = ldst[10:1];         // modes 0, 2: the TMEM word (pair of 16-bit words)
    wire [15:0] l_ev = lt[1] ? la[15:0] : la[31:16];   // mode 2: this entry

    // what the read engine asks of the command channel this cycle
    wire        ld_want = r_act && r_load && !issued_all && !(lmode == 2'd2 && (l_out || la_v));
    wire        h_want  = r_act && !r_load && r_h && !r_hall;
    wire        sp_want = r_act && !r_load && !r_all && (r_z || r_c) && !h_want;
    wire        rd_want = ld_want || h_want || sp_want;
    wire [29:0] rd_word = r_load ? w_next : h_want ? {8'd0, 1'b1, 3'd0, r_hg[17:0]} : (r_kc ? r_cb : r_zb) + {21'd0, r_idx};

    // ======================================================================
    // the write engine
    // ======================================================================
    reg        w_act, w_fillm, w_z, w_set;
    reg [29:0] w_cb, w_zb;
    reg [9:0]  w_x0;
    reg [10:0] w_x1;
    reg [31:0] w_fillc;
    reg [8:0]  w_idx, w_lastw;            // the word to take from the buffers next, the last
    reg        w_all;                     // every word has been taken
    reg        w_pz;                      // the span's second pass: its Z words
    assign wr_ready = !w_act;
    assign wr_busy  = w_act;
    assign wr_seti  = w_set;
    wire [10:0] wr_xl = wr_x1 - 11'd1;

    // ---- write data FIFO (data follows its accepted command)
    localparam WF = 4;                    // 16 entries
    reg [35:0] wf [0:(1<<WF)-1];
    reg [WF:0] wf_wr, wf_rd;
    reg [35:0] wq;                        // the head
    reg        wq_v;
    wire       wf_mem_empty = wf_wr == wf_rd;
    wire       wf_full  = (wf_wr - wf_rd) >= ((1 << WF) - 3);
    wire       wf_pop   = wq_v && m_wdata_ready;
    assign m_wdata_valid = wq_v;
    assign m_wdata    = wq[31:0];
    assign m_wdata_we = wq[35:32];
    assign writes_pending = wq_v || !wf_mem_empty || (m_cmd_valid && m_cmd_we);

    // ---- a word out of the new buffers: its address goes out when its
    // pixels' results are in; the RAM's data is valid two edges later (p1 ->
    // p2) and lands in a 4-entry FIFO; its head becomes the colour's command,
    // or in the second pass Z's (dropped when no pixel of it passed)
    reg        p1, p2;
    reg [8:0]  p1_idx, p2_idx;
    reg [112:0] rf [0:3];                 // {old hidden word, word, zdata, cdata (36), zmask, cmask}
    reg [2:0]  rf_wr, rf_rd;
    wire [2:0] rf_cnt = rf_wr - rf_rd;
    wire       rf_room = ({1'b0, rf_cnt} + {3'd0, p1} + {3'd0, p2}) < 4'd4;
    wire [112:0] rf_head = rf[rf_rd[1:0]];
    reg [1:0]  h_ph;                      // the head's commands done: 0 none, 1 its word's (its group's hidden word is next)
    reg [31:0] hacc;                      // the group's hidden word so far (the head's merged in at its pop)
    reg [3:0]  hbe;                       // a fill: the quads touched so far
    reg        hany;                      // a pixel of the group was written
    reg  [9:0] w_hg;                      // (the word lbr_addr asks for: its group's low bits)
    assign hlbr_addr = {w_set, w_hg[6:0]};

    // the pixels of the row a word's results need: through its odd pixel, or the span's last
    wire [10:0] w_res  = w_set ? res_b : res_a;
    wire [10:0] w_end  = ({1'b0, w_idx, 1'b0} + 11'd2 > w_x1) ? w_x1 : {1'b0, w_idx, 1'b0} + 11'd2;
    wire        w_rdy  = w_fillm || w_pz || w_res >= (w_end - {1'b0, w_x0});
    wire [12:0] w_hgw  = w_cb[12:0] + {4'd0, w_idx};       // (the word's address: its group, for the raw hidden buffer)

    // pixel range of a word: the byte lanes of its even pixel ([3:2]) and its odd one
    function automatic [3:0] range_sel;
        input [8:0] w;
        reg [10:0] xe;   // even pixel of the word
        begin
            xe = {1'b0, w, 1'b0};
            range_sel = { {2{xe >= {1'b0, w_x0} && xe < w_x1}},
                          {2{(xe + 11'd1) >= {1'b0, w_x0} && (xe + 11'd1) < w_x1}} };
        end
    endfunction
    wire [31:0] h_hold  = rf_head[112:81];
    wire [8:0]  h_idx   = rf_head[80:72];
    wire [31:0] h_zdata = rf_head[71:40];
    wire [35:0] h_cd36  = rf_head[39:4];
    wire [31:0] h_cdata = {h_cd36[35:20], h_cd36[17:2]};
    wire [1:0]  h_zmask = rf_head[3:2];
    wire [1:0]  h_cmask = rf_head[1:0];
    wire [3:0]  h_range = range_sel(h_idx);
    wire [3:0]  h_csel  = w_pz ? 4'd0 : w_fillm ? h_range : (h_range & { {2{h_cmask[0]}}, {2{h_cmask[1]}} });
    wire [3:0]  h_zsel  = !w_pz ? 4'd0 : (h_range & { {2{h_zmask[0]}}, {2{h_zmask[1]}} });
    wire        h_v     = rf_wr != rf_rd;
    // the hidden word of the head's group: the word's two pixels' new bits
    // merged in (nibble j = its place in the group: [1:0] the even pixel); the
    // group goes out after its last word, or the span's, when a pixel of it
    // was written. A fill: the quad pattern {a, a} by the colour's bit 0 (bits
    // 16 and 0), each quad touched enabled
    wire [29:0] h_cw    = w_cb + {21'd0, h_idx};
    wire [2:0]  h_j     = h_cw[2:0];
    wire        h_first = h_j == 3'd0 || h_idx == w_x0[9:1];
    wire [31:0] h_base  = h_first ? h_hold : hacc;
    wire [3:0]  h_nib   = { h_csel[0] ? h_cd36[1:0]   : h_base[{h_j, 2'b10} +: 2],
                            h_csel[2] ? h_cd36[19:18] : h_base[{h_j, 2'b00} +: 2] };
    wire [31:0] h_merge = w_fillm ? {8{w_fillc[0], w_fillc[0], w_fillc[16], w_fillc[16]}}
                                  : (h_base & ~(32'hF << {h_j, 2'b00})) | ({28'd0, h_nib} << {h_j, 2'b00});
    wire [3:0]  h_qbe   = (h_first ? 4'd0 : hbe) | (4'd1 << h_j[2:1]);
    wire        h_any   = (h_first ? 1'b0 : hany) | (h_csel != 4'd0);
    wire        h_last  = h_j == 3'd7 || h_idx == w_lastw;
    wire        h_c     = h_v && h_ph == 2'd0 && h_csel != 4'd0;           // the head's colour word is to go
    wire        h_zz    = h_v && h_ph == 2'd0 && h_zsel != 4'd0;           // ... or (the second pass) its Z word
    wire        h_hh    = h_v && !w_pz && !h_c && h_last && h_any;        // ... or its group's hidden word
    wire        wr_want = (h_c || h_zz || h_hh) && !wf_full;
    wire        wr_go   = wr_want && cmd_free && !rd_want;
    wire        h_pop   = h_v && ((!h_c && !h_zz && !h_hh) || (wr_go && h_hh)
                                  || (wr_go && h_zz)
                                  || (wr_go && h_c && !(h_last && h_any)));

    always @(posedge clk) begin
        lbw_z_we <= 1'b0;
        lbw_c_we <= 1'b0;
        hlbw_we <= 1'b0;
        tm_we <= 4'd0;
        if (rst) begin
            r_act <= 1'b0; w_act <= 1'b0;
            m_cmd_valid <= 1'b0;
            wf_wr <= 0; wf_rd <= 0; wq_v <= 1'b0;
            p1 <= 1'b0; p2 <= 1'b0;
            rf_wr <= 0; rf_rd <= 0; h_ph <= 2'd0;
            h_done <= 1'b0;
            la_v <= 1'b0; l_out <= 1'b0;
            z_have <= 10'd0; c_have <= 10'd0;
        end else begin
            // write data out: refill the head from the RAM
            if (!wq_v || wf_pop) begin
                if (!wf_mem_empty) begin
                    wq <= wf[wf_rd[WF-1:0]];
                    wq_v <= 1'b1;
                    wf_rd <= wf_rd + 1'b1;
                end else
                    wq_v <= 1'b0;
            end

            // ---- the command channel: a read if one is wanted, else a write
            if (cmd_free) begin
                m_cmd_valid <= 1'b0;
                if (rd_want) begin
                    m_cmd_valid <= 1'b1;
                    m_cmd_we <= 1'b0;
                    m_cmd_addr <= {rd_word, 2'b00};
                end else if (wr_want) begin
                    m_cmd_valid <= 1'b1;
                    m_cmd_we <= 1'b1;
                    m_cmd_addr <= h_hh && !h_c && !h_zz ? {8'd0, 1'b1, 3'd0, h_cw[20:3], 2'b00}
                                                        : {(h_c ? w_cb : w_zb) + {21'd0, h_idx}, 2'b00};
                    wf[wf_wr[WF-1:0]] <= h_c ? {h_csel, w_fillm ? w_fillc : h_cdata}
                                       : h_zz ? {h_zsel, h_zdata}
                                       : {w_fillm ? h_qbe : 4'hF, h_merge};
                    wf_wr <= wf_wr + 1'b1;
                end
            end

            // ================================================== the read engine
            if (!r_act) begin
                if (rd_valid) begin
                    r_act <= 1'b1;
                    r_load <= rd_load; r_z <= rd_z; r_c <= rd_c; r_set <= rd_set;
                    r_zb <= rd_zbase[31:2]; r_cb <= rd_cbase[31:2];
                    r_idx <= rd_x0[9:1]; q_idx <= rd_x0[9:1]; r_last <= rd_xl[9:1]; r_first <= rd_x0[9:1];
                    r_tog <= 1'b0; q_tog <= 1'b0; r_all <= 1'b0;
                    z_have <= 10'd0; c_have <= 10'd0;
                    r_h <= rd_h && !rd_load; r_hall <= !(rd_h && !rd_load); q_hin <= rd_h && !rd_load;
                    r_hg <= rd_cw0[20:3]; q_hg <= rd_cw0[9:3]; r_hgl <= rd_cwl[9:3];
                    h_done <= !(rd_h && !rd_load);
                    if (rd_load) begin
                        w_next <= rd_cbase[31:2];
                        l_ask <= ld_reads - 12'd1;
                        l_get <= ld_reads;
                        issued_all <= 1'b0;
                        lmode <= rd_lmode; lo <= rd_cbase[1:0];
                        ldst <= rd_ldst; lleft <= rd_ln;
                        lacc <= 12'd0; ldxt <= rd_ldxt; lhalf <= 1'b0; lodd <= rd_lodd;
                        lfirst <= rd_cbase[1:0] != 2'd0;
                        la_v <= 1'b0; l_out <= 1'b0; lt <= 2'd0;
                        if (rd_ln == 11'd0) r_act <= 1'b0;
                    end
                end
            end else if (r_load) begin
                // read commands: back to back, or (TLUT) one at a time
                if (ld_want && cmd_free) begin
                    if (l_ask == 12'd0) issued_all <= 1'b1;
                    l_ask <= l_ask - 12'd1;
                    w_next <= w_next + 30'd1;
                    l_out <= 1'b1;
                end
                // a memory word: to an aligned word (the first of an unaligned source only primes)
                if (m_rdata_valid) begin
                    lprev <= m_rdata;
                    lfirst <= 1'b0;
                    if (!lfirst) begin la <= l_aligned; la_v <= 1'b1; lt <= 2'd0; end
                    l_get <= l_get - 12'd1;
                    l_out <= 1'b0;
                end
                // an aligned word into TMEM
                if (la_v) begin
                    case (lmode)
                        2'd0: begin
                            tm_we <= l_w[9] ? 4'b1100 : 4'b0011; tm_wa <= {l_w[8:1], l_w[0] ^ lsw};
                            tm_wd0 <= la[31:16]; tm_wd1 <= la[15:0]; tm_wd2 <= la[31:16]; tm_wd3 <= la[15:0];
                            ldst <= ldst + 11'd2; lleft <= lleft - 11'd1;
                            lhalf <= ~lhalf; if (lhalf) lacc <= lacc + ldxt;
                            if (!m_rdata_valid || lfirst) la_v <= 1'b0;
                        end
                        2'd1, 2'd3: begin
                            tm_we <= ldst[0] ? 4'b1010 : 4'b0101; tm_wa <= {ldst[9:2], ldst[1] ^ lsw};
                            tm_wd0 <= lmode[1] ? {la[31:24], la[15:8]} : la[31:16]; tm_wd1 <= lmode[1] ? {la[31:24], la[15:8]} : la[31:16];
                            tm_wd2 <= lmode[1] ? {la[23:16], la[7:0]} : la[15:0];   tm_wd3 <= lmode[1] ? {la[23:16], la[7:0]} : la[15:0];
                            ldst <= ldst + 11'd1; lleft <= lleft - 11'd1;
                            lhalf <= ~lhalf; if (lhalf) lacc <= lacc + ldxt;
                            if (!m_rdata_valid || lfirst) la_v <= 1'b0;
                        end
                        default: begin
                            tm_we <= l_w[9] ? 4'b1100 : 4'b0011; tm_wa <= l_w[8:0];
                            tm_wd0 <= l_ev; tm_wd1 <= l_ev; tm_wd2 <= l_ev; tm_wd3 <= l_ev;
                            ldst <= ldst + 11'd2;
                            lt <= lt + 2'd1;
                            if (lt[0]) lleft <= lleft - 11'd1;
                            if (lt == 2'd3 || (lt == 2'd1 && lleft == 11'd1)) la_v <= 1'b0;
                        end
                    endcase
                end
                if (lleft == 11'd0 && !la_v && issued_all && l_get == 12'd0) r_act <= 1'b0;
            end else begin
                // the hidden words first, a command a cycle
                if (h_want && cmd_free) begin
                    if (r_hg[6:0] == r_hgl) r_hall <= 1'b1;
                    r_hg <= r_hg + 18'd1;
                end
                if (m_rdata_valid && q_hin) begin
                    hlbw_addr <= {r_set, q_hg};
                    lbw_data <= m_rdata;
                    hlbw_we <= 1'b1;
                    q_hg <= q_hg + 7'd1;
                    if (q_hg == r_hgl) begin
                        q_hin <= 1'b0; h_done <= 1'b1;
                        if (!r_z && !r_c) r_act <= 1'b0;
                    end
                end
                // a row's spans: a command a cycle, the Z span's words and then the colour span's
                if (sp_want && cmd_free) begin
                    r_idx <= r_idx + 9'd1;
                    if (r_idx == r_last) begin
                        if (r_both && !r_tog) begin r_tog <= 1'b1; r_idx <= r_first; end
                        else r_all <= 1'b1;
                    end
                end
                // data back, in the order asked
                if (m_rdata_valid && !q_hin) begin
                    lbw_addr <= {r_set, q_idx};
                    lbw_data <= m_rdata;
                    if (q_kc) begin lbw_c_we <= 1'b1; c_have <= c_have + 10'd1; end
                    else      begin lbw_z_we <= 1'b1; z_have <= z_have + 10'd1; end
                    q_idx <= q_idx + 9'd1;
                    if (q_idx == r_last) begin
                        if (r_both && !q_tog) begin q_tog <= 1'b1; q_idx <= r_first; end
                        else r_act <= 1'b0;
                    end
                end
            end

            // ================================================= the write engine
            if (!w_act) begin
                p1 <= 1'b0; p2 <= 1'b0;
                if (wr_valid) begin
                    w_act <= 1'b1;
                    w_fillm <= wr_fill; w_z <= wr_z; w_set <= wr_set;
                    w_cb <= wr_cbase[31:2]; w_zb <= wr_zbase[31:2];
                    w_x0 <= wr_x0; w_x1 <= wr_x1; w_fillc <= wr_fillc;
                    w_idx <= wr_x0[9:1]; w_lastw <= wr_xl[9:1];
                    w_all <= 1'b0; h_ph <= 2'd0; w_pz <= 1'b0;
                end
            end else begin
                // the head: a command went (above), or there is none for it
                if (wr_go && h_c) h_ph <= 2'd1;
                if (h_pop) begin
                    rf_rd <= rf_rd + 3'd1; h_ph <= 2'd0;
                    hacc <= h_merge; hbe <= h_qbe; hany <= h_any;
                end
                // the RAM's data for the word addressed two edges ago
                p2 <= p1;
                p2_idx <= p1_idx;
                if (p2) begin
                    rf[rf_wr[1:0]] <= {hlbr_data, p2_idx, lbr_zdata, lbr_cdata, lbr_zmask, lbr_cmask};
                    rf_wr <= rf_wr + 3'd1;
                end
                // the next word, when its results are in
                if (!w_all && rf_room && w_rdy) begin
                    lbr_addr <= {w_set, w_idx};
                    w_hg <= w_hgw[12:3];
                    p1 <= 1'b1;
                    p1_idx <= w_idx;
                    if (w_idx == w_lastw) w_all <= 1'b1;
                    w_idx <= w_idx + 9'd1;
                end else begin
                    p1 <= 1'b0;
                end
                // the span's words have all gone: the same again for its Z words (Z_UPD), or done
                if (w_all && !p1 && !p2 && rf_wr == rf_rd) begin
                    if (w_z && !w_fillm && !w_pz) begin w_pz <= 1'b1; w_all <= 1'b0; w_idx <= w_x0[9:1]; end
                    else w_act <= 1'b0;
                end
            end
        end
    end
endmodule
