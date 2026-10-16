// rtl/soc/n64_video.sv with MIRLO_VI_AA against a model of the N64 VI's picture: the window in the 640 x 240 field, the VI's
// X_SCALE / Y_SCALE steps through the framebuffer (nearest pixel and row), and its anti-alias, dither and divot filters
// (the VI_filter / VI_filter_pen behaviour, with the coverage of a partial pixel taken as 3: the framebuffer keeps one bit).
//   tb_vi [seed] [divot 0/1] [partial %] [width] [dedither 0/1]
// width: the framebuffer's pitch in pixels -- 320 (X_SCALE 0x200: doubled), 440 (0x2C0 and Y_SCALE 0x580), 384
// (0x20E in a 597-clock window), 640 (hi-res, filters off: X_SCALE 0x400), or any other; `640p` a 640 x 240
// buffer, plain 640 a 480-row one whose even rows are shown (Y_SCALE 0x800: a hi-res picture without SERRATE). A serrated
// picture (VI_CTRL SERRATE: vi_il) is sent as 480i, a field a frame: `640s` (a 640-pitch 640 x 480 buffer, Y_SCALE 2.0:
// libultra's interlaced modes) or `1280s` (WIDTH 1280, Y_SCALE 1.0: two rows of 640 a pitch). The bench is the game and its
// VI: the field (vi_fld) turns before the frame's line 240, as rcp_vi's does, and the field's origin (vi_origin2) is the
// buffer's for the even field and one 640-pixel row on for the odd one, as libultra writes them; a field's line j shows the
// row (j Y_SCALE) >> 10 from its origin. `640ps`, a 240-row buffer with SERRATE, and `704s` (another pitch) are fields too,
// both from the buffer's start.
//   env: TB_XS, TB_YS (the steps, hex), TB_PX0, TB_PW (the window in the line, clocks), TB_Y0, TB_ROWS (its lines),
//   TB_LOAD=n: n other masters on the SDRAM, each granted 16 words in turn (the arbiter's round robin, MAXB 16): the scan-out has a
//   word each cycle for 16 cycles, then none for 16 n (TB_RDY=p: instead each cycle's request is taken with a probability of p %),
//   TB_CROPV, TB_CROPR (the core's crop options), TB_PAL=1: vi_pal on from the start (PAL timing: 313 lines, 1,250,000 sys
//   clocks a field), TB_SEQ=0,1,1,0,...: vi_pal set to the n-th value at the n-th vsync (the TV system changes at the
//   end of the field: the fields are checked line by line), TB_DUMP=prefix, TB_V.
// A random RGBA5551 framebuffer (bit 0: the pixel is complete) is scanned out through the DMA port; a frame is taken off
// the pads and every pixel compared (a serrated picture's field has a black line of DE before its 240). Outside DE the pads are 0
// but for a serrated picture's words for the Pocket: the slot of
// the next frame (1, 640 x 480) after each line's DE, and during VS the next frame's features (interlaced and the last,
// every field; odd for the odd field).
#include "Vn64_video.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cmath>
#include <cstring>

static Vn64_video *t;
static unsigned WB = 320;                         // the buffer's pitch (VI_WIDTH)
static unsigned XS = 0x200, YS = 0x400;           // the VI's steps
static unsigned PX0 = 0, PW = 640, Y0 = 0, R = 240;   // the picture's window: its first clock in the line, its clocks, its first line, its lines
static unsigned HB;                               // the buffer's rows
static bool nofilt;                               // a hi-res picture (512 pixels or more a row): the filters are off
static bool ser, wov;                             // SERRATE with a hi-res pitch (vi_il); and the frame is woven
static unsigned LOAD = 0, RDY = 75;               // the SDRAM's load: TB_LOAD, TB_RDY
static std::vector<uint16_t> fbw;                 // the buffer
static std::vector<uint8_t> hidw;                 // each pixel's hidden two coverage bits (the RDP's plane)
static unsigned rng = 1;
static int pal0;                                  // TB_PAL
static std::vector<int> seq;                      // TB_SEQ
static int sys_of(int j) { return j <= 0 ? 0 : seq.empty() ? 0 : seq[(size_t)j < seq.size() ? j : seq.size() - 1]; }   // the system of the field F_j
static unsigned rnd() { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static unsigned CV = 0, CR = 0;                   // TB_CROPV, TB_CROPR: the core's crop options (lines at top and bottom; right pixels, two clocks each)

// the model: R8 G8 B8
struct P6 { int r, g, b; };
static int ch(uint16_t p, int c) { return c == 0 ? (p >> 11) & 31 : c == 1 ? (p >> 6) & 31 : (p >> 1) & 31; }
static int rowerr[480];
// the pixel (x, r) of the buffer: x from the row's start, which runs on into the next row (the reads are contiguous);
// the picture's rows are the fetched rows f(j) = (j Y_SCALE) >> 10, one per line of the window, from the origin (POFF: the
// pixels it is on from the buffer's start -- a serrated picture's odd field's)
// (a woven frame: its line 2 j + p is the field p's line j -- the row (j Y_SCALE) >> 10 of the field's pitch, WIDTH, from the
// first field's origin, and one 640-pixel row on for the second: in rows of 640 pixels)
static unsigned frow(int j) { return wov ? (((unsigned)(j >> 1) * YS) >> 10) * (WB / 640) + (unsigned)(j & 1) : ((unsigned)j * YS) >> 10; }
static uint16_t pxv(int x, unsigned r) { size_t i = (size_t)r * (wov ? 640 : WB) + (size_t)x; return i < fbw.size() ? fbw[i] : 0; }
static unsigned hidv(int x, unsigned r) { size_t i = (size_t)r * (wov ? 640 : WB) + (size_t)x; return i < hidw.size() ? hidw[i] : 0; }
// a tap of the filters: the pixel (x, line j) of the window's source, in the picture if x >= 0 and the line j is a line of the window
static bool intap(int x, int j) { return x >= 0 && j >= 0 && j < (int)R; }
static unsigned cvg(int x, int j) { return (pxv(x, frow(j)) & 1u) << 2 | hidv(x, frow(j)); }
static bool complete(int x, int j) { return cvg(x, j) == 7u; }
static int dedither;                                // the VI's dither filter (argument 5)
static P6 aa_px(int x, int j)
{
    uint16_t c = pxv(x, frow(j));
    P6 o;
    int *po[3] = { &o.r, &o.g, &o.b };
    if (complete(x, j)) {                            // complete: as it is (8 bits: c << 3, the N64's), or dedithered
        for (int k = 0; k < 3; k++) {
            int cc = ch(c, k), v = cc << 3;
            if (dedither) {
                int add = 0;
                for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                    if ((!dx && !dy) || !intap(x + dx, j + dy)) continue;
                    int n = ch(pxv(x + dx, frow(j + dy)), k);
                    add += n > cc ? 1 : n < cc ? -1 : 0;
                }
                v = (cc << 3) + add; if (v > 255) v = 255;
            }
            *po[k] = v;
        }
        return o;
    }
    static const int dx[6] = { -1, 1, -2, 2, -1, 1 }, dy[6] = { -1, -1, 0, 0, 1, 1 };
    for (int k = 0; k < 3; k++) {
        int v[6], cc = ch(c, k);
        for (int i = 0; i < 6; i++) {
            int xx = x + dx[i], yy = j + dy[i];
            v[i] = (intap(xx, yy) && complete(xx, yy)) ? ch(pxv(xx, frow(yy)), k) : cc;
        }
        // second smallest, second largest
        int s[6]; for (int i = 0; i < 6; i++) s[i] = v[i];
        for (int i = 0; i < 6; i++) for (int jj = i + 1; jj < 6; jj++) if (s[jj] < s[i]) { int q = s[i]; s[i] = s[jj]; s[jj] = q; }
        int mn = s[1] < cc ? s[1] : cc, mx = s[4] > cc ? s[4] : cc;
        // the N64's, on 8-bit channels (c << 3): c8 + ((mn8 + mx8 - 2 c8) * (7 - cvg) + 4) >> 3; its top six bits
        int c8 = cc << 3, w = 7 - (int)cvg(x, j);
        int o8 = c8 + (int)floor(((double)(((mn << 3) + (mx << 3) - 2 * c8) * w) + 4.0) / 8.0);
        *po[k] = o8;
    }
    return o;
}
static int med3(int a, int b, int c) { int s[3] = { a, b, c }; for (int i = 0; i < 3; i++) for (int j = i + 1; j < 3; j++) if (s[j] < s[i]) { int q = s[i]; s[i] = s[j]; s[j] = q; } return s[1]; }
// the pixel the pads show: window pixel i of line j (the source pixel (i X_SCALE) >> 10)
static P6 model(int sx, int j, bool divot)
{
    if (nofilt) { uint16_t c = pxv(sx, frow(j)); return P6{ ch(c, 0) << 3, ch(c, 1) << 3, ch(c, 2) << 3 }; }
    P6 e = aa_px(sx, j);
    if (divot) {
        bool part = !complete(sx, j) || (sx > 0 && !complete(sx - 1, j)) || !complete(sx + 1, j);
        if (part) {
            P6 l = sx > 0 ? aa_px(sx - 1, j) : e, r = aa_px(sx + 1, j);
            e.r = med3(l.r, e.r, r.r); e.g = med3(l.g, e.g, r.g); e.b = med3(l.b, e.b, r.b);
        }
    }
    return e;
}

static uint64_t sys_cyc;
static uint64_t pad_hash = 1469598103934665603ull;      // FNV over every pad word and sync of the run (NTSC: the same before and after the PAL change)
static int vph;
static void tick()
{
    // sys every step, clk_vid two in five
    const bool ve = vph == 0 || vph == 2;           // (clk_vid: sys / 2.5 -- an edge two, then three sys cycles on)
    t->clk = 0; if (ve) t->clk_vid = 0; t->eval();
    // the DMA: a read's word 4 sys cycles on
    static std::vector<std::pair<uint64_t, uint32_t>> pend;
    static unsigned ph = 0;                          // (TB_LOAD: the cycle in the round of 16 (n + 1))
    t->m_ready = LOAD ? (ph++ % (16 * (LOAD + 1))) < 16 : (rnd() % 100) < RDY;
    if (t->m_valid && t->m_ready) {
        uint32_t a = t->m_addr;                     // word address
        uint32_t i = a * 2, w = 0;
        if (a >= 0xDC0000u && a < 0xE00000u) {      // the hidden plane (SDRAM 0x0370_0000): pixels 16 (a - base) on
            uint32_t p0 = (a - 0xDC0000u) * 16u;
            for (uint32_t k = 0; k < 16; k++) if (p0 + k < hidw.size()) w |= (uint32_t)hidw[p0 + k] << (2 * k);
        } else if (i + 1 < fbw.size()) w = (uint32_t)fbw[i] << 16 | fbw[i + 1];
        pend.push_back({ sys_cyc + 4 + rnd() % 8, w });
    }
    t->m_rvalid = 0;
    if (!pend.empty() && pend.front().first <= sys_cyc) { t->m_rvalid = 1; t->m_rdata = pend.front().second; pend.erase(pend.begin()); }
    t->clk = 1; if (ve) t->clk_vid = 1; t->eval();
    sys_cyc++; vph = (vph + 1) % 5;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    unsigned seed = argc > 1 ? atoi(argv[1]) : 1;
    int divot = argc > 2 ? atoi(argv[2]) : 0;
    int pct = argc > 3 ? atoi(argv[3]) : 30;
    bool p640 = false;                               // "640p": a 640 x 240 buffer (else a 480-row one, its even rows shown)
    if (argc > 4) {
        WB = (unsigned)atoi(argv[4]); p640 = strstr(argv[4], "640p") != 0; ser = strchr(argv[4], 's') != 0;
    }
    if (WB == 440) { XS = 0x2C0; YS = 0x580; }
    if (WB == 384) { XS = 0x20E; YS = 0x400; PX0 = 20; PW = 597; Y0 = 9; R = 222; }
    if (WB >= 512) { XS = 0x400; YS = p640 ? 0x400 : 0x800; }
    if (ser && WB == 1280) YS = 0x400;
    ser = ser && WB >= 512;                          // (rcp_vi's il: SERRATE with a hi-res pitch)
    // a woven frame: a serrated picture of a pair of fields' pitches (WIDTH 640 with Y_SCALE 2.0, or WIDTH 1280 with 1.0): its
    // 640-pixel rows in order, the window twice the field's lines; any other serrated picture is shown as any picture is
    wov = ser && ((WB == 640 && !p640) || WB == 1280);
    nofilt = WB >= 512;
    auto env = [](const char *n, unsigned &v, int base) { if (const char *e = getenv(n)) v = (unsigned)strtoul(e, 0, base); };
    env("TB_XS", XS, 16); env("TB_YS", YS, 16); env("TB_PX0", PX0, 10); env("TB_PW", PW, 10); env("TB_Y0", Y0, 10); env("TB_ROWS", R, 10);
    env("TB_CROPV", CV, 10); env("TB_CROPR", CR, 10);
    dedither = argc > 5 ? atoi(argv[5]) : 0;
    if (const char *e = getenv("TB_LOAD")) LOAD = (unsigned)atoi(e);
    if (const char *e = getenv("TB_RDY")) RDY = (unsigned)atoi(e);
    rng = seed * 2654435761u + 7;
    pal0 = getenv("TB_PAL") ? atoi(getenv("TB_PAL")) : 0;
    if (const char *q = getenv("TB_SEQ")) for (; *q; q++) if (*q == '0' || *q == '1') seq.push_back(*q - '0');
    HB = (wov ? 2 * R + 8 : frow((int)R) + 4);    // the buffer's rows
    const unsigned smax = (((PW - 1) * XS) >> 10);   // the window's last source pixel
    const unsigned NW = (smax + 8) / 2 + 1;          // the words a row reads: its priming's seven pixels and the window's, and one more
    fbw.resize((size_t)WB * HB + 4096); hidw.resize((size_t)WB * HB + 4096);
    // patches of colour with partial pixels, and noise
    for (size_t i = 0; i < fbw.size(); i++) {
        uint16_t v = (uint16_t)(rnd() & 0xFFFE);
        if ((rnd() % 4) && i > 0) v = fbw[i - 1] & 0xFFFE;
        unsigned c = (int)(rnd() % 100) >= pct ? 7u : rnd() % 7u;      // complete, or a partial coverage 0..6
        fbw[i] = v | (uint16_t)(c >> 2);
        hidw[i] = (uint8_t)(c & 3u);
    }
    t = new Vn64_video;
    t->rst = 1; t->rst_vid = 1; t->enable = 1; t->vi_origin = 0; t->vi_il = ser; t->vi_width = WB; t->vi_black = 0;
    t->gamma = 0; t->vi_aa = 1; t->vi_divot = divot; t->vi_dedither = dedither; t->crop_v = CV; t->crop_r = CR; t->vi_lines = 240;
    t->vi_px0 = PX0; t->vi_pw = PW; t->vi_y0 = Y0; t->vi_rows = R == 240 ? 0 : R; t->vi_xs = XS; t->vi_ys = YS; t->vi_nw = NW;
    t->osd_en = 0; t->fps_en = 0; t->vi_pal = seq.empty() ? pal0 : 0;
    for (int i = 0; i < 200; i++) tick();
    t->rst = 0; t->rst_vid = 0;
    // frames off the pads
    std::vector<std::vector<int>> rows; std::vector<int> row;
    int pde = 0, pvs = 0, frames = 0, errs = 0, outside = 0;
    long changed = 0;                               // (pixels the filters change: the test is not empty)
    // the field's timing (n64_video.sv: 59.940 Hz): vsync to vsync = 1,042,710 sys clocks, 264 lines of 790 clk_vid clocks
    // (3950 sys), nine of 788 (3940)
    long vs_at = -1, hs_at = -1, tim_errs = 0; int hs_n = 0, hs_short = 0; unsigned fields_ok = 0;
    int phs = 0;
    // (a woven picture: the first frame is a progressive one, the second the change of timing -- one more)
    const int nfr = (seq.empty() ? (pal0 ? 5 : 4) : (int)seq.size() + 3) + (ser && ((WB == 640 && !strstr(argc > 4 ? argv[4] : "", "640p")) || WB == 1280) ? 1 : 0);
    // the lines of the two systems (sys clocks): NTSC 790 clk_vid x 5 = 3950, nine 788 x 5 = 3940; PAL 800 x 5 = 4000, fifty 792 x 5 = 3960
    // (a woven frame: lines half as long, twice as many, twice as many short: 1975 / 1970 and 2000 / 1980, 528 and 626, 18 and 100)
    const int WV = wov ? 2 : 1;
    const long LN[2] = { 3950 / WV, 4000 / WV }, LS[2] = { 3950 / WV - 10 / WV, 4000 / WV - 40 / WV }, FLD[2] = { 1042710, 1250000 };
    const int NLN[2] = { 264 * WV, 313 * WV }, NSH[2] = { 9 * WV, 50 * WV };
    const int KVS = WV == 2 ? 482 : 242;            // (the lines from a frame's start to its VS)
    std::vector<long> hs_ds;
    while (frames < nfr) {
        tick();
        if (vph != 1 && vph != 3) continue;         // (just after clk_vid's edge)
        pad_hash = (pad_hash ^ ((uint64_t)t->vga_r | (uint64_t)t->vga_g << 8 | (uint64_t)t->vga_b << 16 | (uint64_t)t->vga_hsync << 24 | (uint64_t)t->vga_vsync << 25 | (uint64_t)t->vga_de << 26 | (uint64_t)t->vga_skip << 27)) * 1099511628211ull;
        if (!phs && t->vga_hsync) {
            if (hs_at >= 0) {
                long d = sys_cyc - hs_at;
                hs_ds.push_back(d);
                if (seq.empty() && (!pal0 || frames >= 2)) {
                    if (d == LS[pal0]) hs_short++; else if (d != LN[pal0] && !(vs_at >= 0 && hs_n == 0) && (!wov || frames >= 2)) { if (tim_errs++ < 5) printf("line period %ld sys clocks\n", d); }
                }
            }
            hs_at = sys_cyc; hs_n++;
        }
        phs = t->vga_hsync;
        // a progressive frame's pixel is two clocks, the second skipped; a woven frame's one, none skipped
        if (t->vga_skip && (wov ? frames >= 2 : !t->vga_de) && outside++ < 5) printf("vga_skip high\n");
        // outside DE: the slot word (green's bits 6:5) of a woven frame, else 0
        if (!t->vga_de && !t->vga_vsync && frames >= 3 && (t->vga_r || t->vga_b || t->vga_g != (wov ? 0x20 : 0)) && outside++ < 5)
            printf("pads outside DE: %d %d %d\n", t->vga_r, t->vga_g, t->vga_b);
        if (t->vga_de && !t->vga_skip) { row.push_back(t->vga_r); row.push_back(t->vga_g); row.push_back(t->vga_b); }
        if (pde && !t->vga_de) { rows.push_back(row); row.clear(); }
        pde = t->vga_de;
        if (!pvs && t->vga_vsync) {
            frames++;
            if (seq.empty()) {
              if (vs_at >= 0 && (!pal0 || frames >= 3) && (!wov || frames >= 3)) {      // (PAL: the system changes at the end of the first field, NTSC from reset; a woven picture's timing at the second)
                long d = sys_cyc - vs_at;
                if (d != FLD[pal0] || hs_n != NLN[pal0] || hs_short != NSH[pal0]) { if (tim_errs++ < 5) printf("field: %ld sys clocks, %d lines, %d short (want %ld, %d, %d)\n", d, hs_n, hs_short, FLD[pal0], NLN[pal0], NSH[pal0]); }
                else fields_ok++;
              }
            } else if (vs_at >= 0 && (!wov || frames >= 3)) {        // (a woven picture: from its second frame, the first after the change of timing)
                // the interval from the last vsync (V_j, line 241 of a field F_(j-1)) to this one (V_(j+1), line 241 of F_j): the rest of
                // F_(j-1) (all of its short lines are in it) and the first 242 lines of F_j. The system of F_j is the request made at
                // V_j (taken at the frame boundary of F_(j-1), line 255 or 304, after the vsync, and applied where F_(j-1) ends); F_0 is NTSC
                const int j = frames - 1;
                const int a = sys_of(j - 1), b = sys_of(j);
                long d = sys_cyc - vs_at;
                const long want = (long)(NLN[a] - KVS) * LN[a] + KVS * LN[b] - (long)NSH[a] * (LN[a] - LS[a]);
                if (d != want || hs_n != NLN[a]) { if (tim_errs++ < 8) printf("interval %d (field %s then %s): %ld sys clocks, %d lines (want %ld, %d)\n", frames, a ? "PAL" : "NTSC", b ? "PAL" : "NTSC", d, hs_n, want, NLN[a]); }
                else { fields_ok++; printf("interval %d: %s field, then %s: %ld sys clocks, %d lines\n", frames, a ? "PAL" : "NTSC", b ? "PAL" : "NTSC", d, hs_n); }
            }
            if (!seq.empty()) t->vi_pal = sys_of(frames);       // (V_j asks for F_j's system)
            vs_at = sys_cyc; hs_n = 0; hs_short = 0; hs_ds.clear();
            if (frames >= 3) {
                if (rows.size() != 240u * WV) { printf("frame %d: %zu rows\n", frames, rows.size()); errs++; }
                for (unsigned ys = 0; ys < rows.size() && ys < 240u * WV; ys++) {
                    if (rows[ys].size() != 640 * 3) { printf("row %u: %zu px\n", ys, rows[ys].size() / 3); errs++; continue; }
                    // the picture's line j is the screen row Y0 + j; black outside the window, and where the crop options blank it
                    const int j = (int)ys - (int)Y0 * WV;
                    for (unsigned x = 0; x < 640; x++) {
                        const int i = (int)x - (int)PX0;
                        const bool inw = j >= 0 && j < (int)R * WV && i >= 0 && i < (int)PW;
                        const bool crop = !inw || (unsigned)j < CV * WV || (unsigned)j >= (R - CV) * WV || (unsigned)i >= PW - 2 * CR;
                        int gr = rows[ys][3 * x], gg = rows[ys][3 * x + 1], gb = rows[ys][3 * x + 2];
                        P6 e = crop ? P6{ 0, 0, 0 } : model((int)((unsigned)i * XS >> 10), j, divot && !nofilt);
                        if (!crop && frames == 3 && ((e.r & 7) != 0 || (e.g & 7) != 0 || (e.b & 7) != 0 || true)) changed += (e.r != ch(pxv((int)((unsigned)i * XS >> 10), frow(j)), 0) << 3);
                        if (gr != e.r || gg != e.g || gb != e.b) {
                            if (errs < 10) printf("[frame %d] (%u,%u) window (%d,%d): pads %d %d %d, model %d %d %d\n", frames, x, ys, i, j, gr, gg, gb, e.r, e.g, e.b);
                            errs++; if (getenv("TB_V")) rowerr[ys]++;
                        }
                    }
                }
                if (getenv("TB_V")) { for (unsigned y = 0; y < 240; y++) if (rowerr[y]) printf("  frame %d row %u: %d bad\n", frames, y, rowerr[y]); memset(rowerr, 0, sizeof rowerr); }
            }
            // TB_DUMP=prefix: the frame off the pads (PPM)
            if (frames == 3 && getenv("TB_DUMP")) {
                std::string pfx = getenv("TB_DUMP");
                if (FILE *f = fopen((pfx + "_pads.ppm").c_str(), "wb")) {
                    fprintf(f, "P6\n640 %zu\n255\n", rows.size());
                    for (auto &r : rows) for (unsigned x = 0; x < 640; x++) { uint8_t px[3] = { (uint8_t)r[3 * x], (uint8_t)r[3 * x + 1], (uint8_t)r[3 * x + 2] }; fwrite(px, 1, 3, f); }
                    fclose(f);
                }
            }
            rows.clear(); row.clear();
        }
        pvs = t->vga_vsync;
    }
    errs += outside + (int)tim_errs;
    if (seq.empty()) printf("field timing: %u fields of %ld sys clocks (%s at 62.5 MHz), %d lines each, %d of them shorter\n", fields_ok, FLD[pal0], pal0 ? "50.000 Hz, PAL" : "59.940 Hz", NLN[pal0], NSH[pal0]);
    else printf("field timing: %u intervals as predicted\n", fields_ok);
    if (fields_ok < (unsigned)(seq.empty() ? (pal0 ? 3 : nfr - 1 - (wov ? 1 : 0)) : seq.size())) { printf("field timing: not seen\n"); errs++; }
    printf("pad hash %016llx\n", (unsigned long long)pad_hash);
    printf("seed %u divot %d dedither %d partial %d%% width %u%s X_SCALE %03x Y_SCALE %03x window %u+%u x %u+%u: %s (%d mismatches; %ld pixels changed by the filters)\n",
           seed, divot, dedither, pct, WB, wov ? " serrated: woven, 640 x 480" : ser ? " serrated, not a pair of fields" : "", XS, YS, PX0, PW, Y0, R, errs ? "FAIL" : "PASS", errs, changed);
    return errs != 0;
}
