// Bench of rtl/analogizer/an_scandbl.sv: a stream with the scan-out's timing (the generator below follows the timing code of
// rtl/soc/n64_video.sv: lines of 790 / 800 pixel steps, the short lines in the vertical blanking, hs 656..704, de 0..639,
// vs six steps before an hs at the lines VRES+1 .. VRES+9 (woven: +17), a pixel each two clocks with skip on the second, or a pixel
// a clock in a woven frame) through the doubler, and the output against what the module is meant to send.
//   tb_scandbl main            the mode schedule below (NTSC / PAL, 240p with every scanline setting, woven, switches)
//   tb_scandbl short           NTSC 240p, then woven (the polarity builds)
//   tb_scandbl noise           random inputs for a while, then clean NTSC: it must recover
#include "Van_scandbl.h"
#include "verilated.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <set>
#include <string>
#include <algorithm>

#ifndef HS_LOW
#define HS_LOW 1
#endif
#ifndef VS_LOW
#define VS_LOW 1
#endif
#ifndef VS_LINES_P
#define VS_LINES_P 8
#endif
#ifndef VS_DELAY_P
#define VS_DELAY_P 0
#endif
#ifndef CB_P
#define CB_P 6
#endif

#ifndef DLY_ADJ_P
#define DLY_ADJ_P 0
#endif
static const int D_N = 924 + DLY_ADJ_P, D_P = 944 + DLY_ADJ_P;
struct Mode { bool wv, pal; int sl; };

static uint32_t hsh(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h & 0xFFFFFF;
}

struct Gen {
    std::vector<Mode> modes;
    int frame = 0;
    bool ce_t = false, wv_v, pal_v;
    int hcount = 0, vcount = 0;
    bool hactive = false, vactive = false, hsync = false, vsync = false, short_ln = false;
    bool de_d = false, hs_d = false, vs_d = false, vga_de = false, vga_hs = false, vga_vs = false, skip = false;
    uint32_t pix_d = 0, vga_pix = 0;
    Mode cur() const { return modes[std::min<size_t>(frame, modes.size() - 1)]; }
    bool done() const { return frame >= (int)modes.size(); }
    void init() { wv_v = modes[0].wv; pal_v = modes[0].pal; }
    void step() {
        bool ce = wv_v || ce_t;
        bool de = hactive && vactive;
        uint32_t pix0 = de ? hsh(frame * 2 + wv_v, vcount - 1, hcount - 1) : 0;
        const int VRES = wv_v ? 480 : 240;
        const int HSCAN = pal_v ? 799 : 789, SHORT_PX = pal_v ? 8 : 2;
        const int VSCAN = wv_v ? (pal_v ? 625 : 527) : (pal_v ? 312 : 263);
        const int SHORT_S = wv_v ? 506 : 253;
        const int SHORT_END = wv_v ? (pal_v ? 2 * 253 + 100 - 1 : 2 * 253 + 18 - 1) : (pal_v ? 253 + 50 - 1 : 253 + 9 - 1);
        bool n_hactive = hactive, n_vactive = vactive, n_hsync = hsync, n_vsync = vsync, n_short = short_ln;
        int n_h = hcount, n_v = vcount; bool n_wv = wv_v, n_pal = pal_v; int n_frame = frame;
        if (ce) {
            n_h = hcount + 1;
            if (hcount == 0) n_hactive = true;
            if (hcount == 640) n_hactive = false;
            if (hcount == 656) n_hsync = true;
            if (hcount == 704) n_hsync = false;
            if (hcount == (short_ln ? HSCAN - SHORT_PX : HSCAN)) n_h = 0;
            if (hcount == 650) {
                if (vcount == VRES + 1) n_vsync = true;
                if (vcount == VRES + (wv_v ? 17 : 9)) n_vsync = false;
            }
            if (hcount == 656) {
                n_v = vcount + 1;
                if (vcount == 0) n_vactive = true;
                if (vcount == VRES) n_vactive = false;
                if (vcount == VSCAN) {
                    n_v = 0; n_frame = frame + 1;
                    Mode m = modes[std::min<size_t>(n_frame, modes.size() - 1)];
                    n_wv = m.wv; n_pal = m.pal;
                }
                if (vcount == SHORT_S - 1) n_short = true;
                if (vcount == SHORT_END) n_short = false;
            }
            de_d = de; hs_d = hsync; vs_d = vsync; pix_d = pix0;
        }
        bool n_skip = !ce && vga_de;
        if (ce) {
            vga_de = de_d; vga_hs = hs_d; vga_vs = vs_d;
            vga_pix = de_d ? pix_d : 0x00A5C3;        // outside DE the pads carry something else (the slot word)
        }
        skip = n_skip;
        ce_t = !ce_t;
        hactive = n_hactive; vactive = n_vactive; hsync = n_hsync; vsync = n_vsync; short_ln = n_short;
        hcount = n_h; vcount = n_v; wv_v = n_wv; pal_v = n_pal; frame = n_frame;
    }
};

struct In  { uint32_t rgb; uint8_t f; };     // f: 1 de, 2 hs, 4 vs, 8 skip, 16 woven, 32 pal
struct Out { uint32_t rgb; uint8_t f; };     // f: 1 de, 2 hs, 4 vs
struct Line { long start; std::vector<uint32_t> px; bool wv; int sl; };

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (fails < 40) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } fails++; } } while (0)

static uint32_t exch(uint32_t c8) {      // the line buffer keeps the top CB bits, the output repeats them below
    uint32_t c = c8 >> (8 - CB_P);
    return CB_P == 8 ? c : ((c << (8 - CB_P)) | (c >> (2 * CB_P - 8)));
}
static uint32_t dimch(uint32_t v8, int m) {
    uint32_t v = v8 >> (8 - CB_P), r;
    switch (m) { case 1: r = v - (v >> 2); break; case 2: r = v >> 1; break; case 3: r = v >> 2; break; default: r = v; }
    return CB_P == 8 ? r : ((r << (8 - CB_P)) | (r >> (2 * CB_P - 8)));
}
static uint32_t expect_px(uint32_t p, int m) {
    return (dimch(p >> 16 & 255, m) << 16) | (dimch(p >> 8 & 255, m) << 8) | dimch(p & 255, m);
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    std::string test = argc > 1 ? argv[1] : "main";
    std::vector<Mode> modes;
    auto add = [&](int n, bool wv, bool pal, int sl) { for (int i = 0; i < n; i++) modes.push_back({wv, pal, sl}); };
    int noise = 0;
    if (test == "main") {
        add(2, 0, 0, 0); add(2, 1, 0, 3); add(1, 0, 0, 1); add(1, 0, 0, 2); add(1, 0, 0, 3);
        add(1, 0, 1, 0); add(1, 0, 1, 2); add(1, 1, 1, 0); add(1, 0, 1, 3); add(1, 0, 1, 1);
        add(1, 0, 0, 0); add(1, 1, 0, 0); add(1, 0, 1, 2); add(1, 1, 1, 1); add(1, 0, 0, 3); add(1, 0, 0, 0);
    } else if (test == "short") {
        add(2, 0, 0, 0); add(1, 0, 0, 2); add(1, 1, 0, 0); add(1, 0, 0, 0); add(1, 0, 0, 0);
    } else if (test == "noise") {
        noise = 7000 + (argc > 2 ? atoi(argv[2]) : 0) * 1237;
        add(4, 0, argc > 3 ? atoi(argv[3]) : 0, 0);
    } else { printf("unknown test\n"); return 2; }

    Gen g; g.modes = modes; g.init();
    Van_scandbl* d = new Van_scandbl;
    std::vector<In> in; std::vector<Out> out;
    size_t cap = 600000ull * (modes.size() + 2); in.reserve(cap); out.reserve(cap);
    auto tick = [&]() { d->clk = 0; d->eval(); d->clk = 1; d->eval(); };
    d->rst = 1; d->woven = 0; d->scanlines = 0; d->rgb = 0; d->hs = 0; d->vs = 0; d->de = 0; d->skip = 0;
    tick(); tick(); tick(); d->rst = 0;
    srand(1 + (argc > 2 ? atoi(argv[2]) : 0));
    for (int i = 0; i < noise; i++) {        // garbage in: random pins, with long runs (a line's worth of one level)
        static int hold = 0, hs_v = 0, vs_v = 0, de_v = 0, wv_v = 0;
        if (hold-- <= 0) { hold = rand() % 1700; hs_v = rand() & 1; vs_v = rand() & 1; de_v = rand() & 1; wv_v = rand() & 1; }
        d->rgb = rand() & 0xFFFFFF; d->hs = hs_v ^ (rand() % 97 == 0); d->vs = vs_v; d->de = de_v; d->skip = rand() & 1;
        d->woven = wv_v; d->scanlines = rand() & 3;
        tick();
    }
    long limit = 0;
    {   // run until the last frame has wrapped, plus one more frame's worth of blanking
        limit = 650000ll * (modes.size() + 1);
    }
    std::vector<Line> lines;
    Line curl; bool in_line = false;
    long c = 0;
    int endframe = (int)modes.size();
    long tail = -1;
    for (; c < limit; c++) {
        // outputs visible in this cycle
        out.push_back({d->rgb_o, (uint8_t)(d->de_o | d->hs_o << 1 | d->vs_o << 2)});
        Mode m = g.cur();
        // the cycle's inputs: the generator's pads
        d->rgb = g.vga_pix; d->hs = g.vga_hs; d->vs = g.vga_vs; d->de = g.vga_de; d->skip = g.skip;
        d->woven = g.wv_v; d->scanlines = m.sl;
        uint8_t f = g.vga_de | g.vga_hs << 1 | g.vga_vs << 2 | g.skip << 3 | g.wv_v << 4 | g.pal_v << 5;
        in.push_back({g.vga_pix, f});
        if (g.vga_de && !g.skip) {
            if (!in_line) { in_line = true; curl = Line(); curl.start = c; curl.wv = g.wv_v; curl.sl = m.sl; }
            curl.px.push_back(g.vga_pix);
        }
        if (in_line && !g.vga_de) { lines.push_back(curl); in_line = false; }
        tick();
        g.step();
        if (g.frame >= endframe && tail < 0) tail = c + 2 * 800 * 2;    // two lines into the frame after the last
        if (tail >= 0 && c > tail) break;
    }
    long N = in.size();
    // ---- picture: the output's DE runs against what each input line makes
    std::vector<Line> olines; { bool on = false; Line l;
        for (long i = 0; i < N; i++) { bool de = out[i].f & 1;
            if (de && !on) { on = true; l = Line(); l.start = i; }
            if (de) l.px.push_back(out[i].rgb);
            if (!de && on) { on = false; olines.push_back(l); } } }
    std::vector<std::vector<uint32_t>> exp; std::vector<long> expin;
    for (auto& l : lines) {
        if (l.wv) { exp.push_back(l.px); expin.push_back(l.start); }
        else {
            std::vector<uint32_t> a, b;
            for (auto p : l.px) { a.push_back(expect_px(p, 0)); b.push_back(expect_px(p, l.sl)); }
            exp.push_back(a); expin.push_back(l.start); exp.push_back(b); expin.push_back(l.start);
        }
    }
    printf("%s: %ld cycles, %zu frames, %zu input lines, %zu output lines (expected %zu)\n", test.c_str(), N, modes.size(), lines.size(), olines.size(), exp.size());
    size_t first = 0, ofs = 0;
    if (test == "noise") {      // only the last two frames are held to the picture
        // (the output ends where the stream does: the last lines of the input may not be out yet; find where the last output line is)
        long back = -1;
        for (int j = 0; j < 6 && back < 0; j++) {
            bool all = true;
            for (int i = 0; i < 40; i++) if (olines[olines.size() - 1 - i].px != exp[exp.size() - 1 - j - i]) all = false;
            if (all) back = j;
        }
        CHECK(back >= 0, "the last output line is none of the last input lines");
        size_t lastk = exp.size() - 1 - (back < 0 ? 0 : back);
        first = lastk + 1 - 2 * 240 * 2; ofs = olines.size() - 2 * 240 * 2;
    } else {
        // the stream ends mid-frame: the lines still in the doubler are not in the output
        CHECK(olines.size() >= exp.size() - 2 && olines.size() <= exp.size(), "output line count %zu, expected %zu", olines.size(), exp.size());
    }
    size_t nmatch = 0;
    for (size_t k = first; k < exp.size(); k++) {
        size_t o = k - first + ofs;
        if (o >= olines.size()) break;
        bool ok = olines[o].px == exp[k];
        CHECK(ok, "output line %zu (input line at cycle %ld, pass %zu) differs (len %zu vs %zu)", o, expin[k], k & 1, olines[o].px.size(), exp[k].size());
        if (ok) nmatch++;
    }
    printf("  picture: %zu lines match\n", nmatch);
    if (test == "noise") { printf("%s: %s (%d failures)\n", test.c_str(), fails ? "FAIL" : "PASS", fails); return fails != 0; }

    // ---- sync
    const int HLV = HS_LOW ? 0 : 1, VLV = VS_LOW ? 0 : 1;    // active levels
    auto oh = [&](long i) { return ((out[i].f >> 1) & 1) == HLV; };
    auto ov = [&](long i) { return ((out[i].f >> 2) & 1) == VLV; };
    std::vector<long> Ein, Vin, Hout, Vout, Vt;
    for (long i = 1; i < N; i++) {
        if ((in[i].f & 2) && !(in[i - 1].f & 2)) Ein.push_back(i);
        if ((in[i].f & 4) && !(in[i - 1].f & 4)) Vin.push_back(i);
        if (oh(i) && !oh(i - 1)) Hout.push_back(i);
        if (ov(i) && !ov(i - 1)) Vout.push_back(i);
        if (!ov(i) && ov(i - 1)) Vt.push_back(i);
    }
    auto mode_at = [&](long i) { return in[std::max(0L, std::min(N - 1, i))].f & 48; };
    auto stable = [&](long i, long w) { return i - w >= 0 && i + w < N && mode_at(i - w) == mode_at(i) && mode_at(i + w) == mode_at(i); };
    std::set<long> Eset(Ein.begin(), Ein.end()), Hset(Hout.begin(), Hout.end());
    std::vector<long> chg; for (long i = 1; i < N; i++) if (mode_at(i) != mode_at(i - 1)) chg.push_back(i);
    auto nearchg = [&](long i, long w) { for (long x : chg) if (std::labs(x - i) < w) return true; return false; };
    // hs: the pair of lines per input line (240p), or the same line a clock later (woven)
    long n240 = 0, nwv = 0; std::set<long> explained;
    long inlat = -1;
    for (long E : Ein) {
        if (!stable(E, 3400) || E + 4000 >= N) continue;
        bool wv = in[E].f & 16, pal = in[E].f & 32;
        if (wv) {
            CHECK(Hset.count(E + 1), "woven: no hs_o at %ld", E + 1); explained.insert(E + 1); nwv++;
        } else {
            long t1 = E + (pal ? D_P : D_N) + 659, OL = pal ? 800 : 790;
            CHECK(Hset.count(t1) && Hset.count(t1 + OL), "240p: hs_o at %ld / %ld missing (E %ld)", t1, t1 + OL, E);
            explained.insert(t1); explained.insert(t1 + OL); n240++;
        }
    }
    long unexpl = 0;
    for (long t : Hout) if (!explained.count(t) && !nearchg(t, 9000) && t > 8000 && t < N - 6000) { unexpl++; CHECK(false, "unexplained hs_o lead at %ld", t); }
    printf("  hs: %ld 240p input lines (2 output lines each) and %ld woven lines checked, %ld extra leads\n", n240, nwv, unexpl);
    // the line: de -> hs 656 clocks, hs 48 clocks wide
    long nl = 0;
    for (auto& l : olines) {
        long s = l.start; if (s + 760 >= N) continue;
        bool ok = !oh(s + 655) && oh(s + 656) && oh(s + 703) && !oh(s + 704) && l.px.size() == 640;
        CHECK(ok, "line at %ld: hs window (de 640, hs from 656 for 48)", s); nl++;
    }
    printf("  %ld output lines: de 640 clocks, hs 656..703\n", nl);
    // the latency of a 240p line: 659 clocks to its first pixel, the second copy OL later
    {
        long nlat = 0, bad = 0; const long LAT = 659 + DLY_ADJ_P;
        for (auto& l : lines) if (!l.wv && l.start + 3000 < N && stable(l.start, 3400)) {
            bool pal = in[l.start].f & 32; long OL = pal ? 800 : 790;
            bool ok = (out[l.start + LAT].f & 1) && !(out[l.start + LAT - 1].f & 1) && (out[l.start + LAT + OL].f & 1) && !(out[l.start + LAT - 1 + OL].f & 1)
                      && (out[l.start + LAT + 639].f & 1) && !(out[l.start + LAT + 640].f & 1);
            nlat++; if (!ok) bad++;
        }
        CHECK(bad == 0, "latency: %ld of %ld lines not at +659 (+OL)", bad, nlat);
        printf("  latency 240p: %ld lines first pixel at +659 clocks and the second copy at +%s\n", nlat, "OL");
        long nw = 0, badw = 0;
        for (long i = 0; i + 1 < N; i++) if ((in[i].f & 16) && stable(i, 2)) {
            // woven: out[i+1] is the pass-through of in[i]
            bool de = in[i].f & 1;
            bool ex = (out[i + 1].f & 1) == de && (((out[i + 1].f >> 1) & 1) == (((in[i].f >> 1) & 1) ^ HS_LOW)) && (((out[i + 1].f >> 2) & 1) == (((in[i].f >> 2) & 1) ^ VS_LOW))
                      && out[i + 1].rgb == (de ? in[i].rgb : 0u);
            nw++; if (!ex) { badw++; CHECK(false, "woven pass-through at %ld", i); }
        }
        printf("  woven pass-through: %ld clocks identical (1 clock later, polarity %s/%s)\n", nw, HS_LOW ? "hs low" : "hs high", VS_LOW ? "vs low" : "vs high");
    }
    // vs: the frame
    struct Fr { long vin; long vo; bool wv, pal; };
    std::vector<Fr> frs; std::set<long> deltas_240[2]; long nframes = 0;
    for (long V : Vin) {
        bool wv = in[V].f & 16, pal = in[V].f & 32;
        if (!stable(V, 3400) || V + 40000 >= N) continue;
        long D = pal ? D_P : D_N;
        long want = wv ? V + 1 : V + 12 + D + 659;       // 240p: the pair after the line the vs rises 12 clocks before
        if (!wv) {                                       // (VS_DELAY output lines later)
            auto h = std::find(Hout.begin(), Hout.end(), want);
            CHECK(h != Hout.end() && h + VS_DELAY_P < Hout.end(), "no hs_o lead at %ld", want);
            if (h != Hout.end() && h + VS_DELAY_P < Hout.end()) want = *(h + VS_DELAY_P);
        }
        CHECK(std::find(Vout.begin(), Vout.end(), want) != Vout.end(), "vs_o lead missing at %ld (input vs at %ld, %s)", want, V, wv ? "woven" : "240p");
        if (!wv) {
            // coincides with an hs lead, VS_LINES output lines wide
            long vo = want;
            CHECK(Hset.count(vo), "vs_o lead %ld is not on an hs lead", vo);
            long vt = -1; for (long t : Vt) if (t > vo) { vt = t; break; }
            CHECK(vt > 0 && Hset.count(vt), "vs_o trail %ld is not on an hs lead", vt);
            long nh = 0; for (long t : Hout) if (t >= vo && t < vt) nh++;
            CHECK(nh == VS_LINES_P, "vs_o is %ld lines wide, expected %d", nh, VS_LINES_P);
            // front porch: output lines (DE) between the last picture line and the vs lead
            long last = -1; for (auto& l : olines) if (l.start < vo) last = l.start;
            long fp = 0; for (long t : Hout) if (t > last + 656 && t < vo) fp++;
            CHECK(fp == 2 + VS_DELAY_P, "front porch %ld lines (expected %d)", fp, 2 + VS_DELAY_P);
            deltas_240[pal].insert(vo - V);   // (with VS_DELAY 0: 12 + D + 659)
            // lines in the frame
        }
        nframes++;
    }
    printf("  vs: %ld frames: lead at input vs +12 +D +659 (240p: %s%ld %s%ld clocks), %d lines wide\n", nframes,
        deltas_240[0].empty() ? "" : "NTSC ", deltas_240[0].empty() ? 0L : *deltas_240[0].begin(),
        deltas_240[1].empty() ? "" : "PAL ", deltas_240[1].empty() ? 0L : *deltas_240[1].begin(), VS_LINES_P);
    // frame period and lines a frame, 240p frames wholly in one mode
    for (size_t k = 0; k + 1 < Vout.size(); k++) {
        long a = Vout[k], b = Vout[k + 1];
        if (!stable(a, 3400) || !stable(b, 3400)) continue;
        if (mode_at(a) != mode_at(b)) continue;
        bool wv = in[a].f & 16, pal = in[a].f & 32;
        long want = pal ? 500000 : 417084;
        // the input's own period
        CHECK(b - a == want, "vs_o period %ld, expected %ld (%s %s)", b - a, want, wv ? "woven" : "240p", pal ? "PAL" : "NTSC");
        long nh = 0; for (long t : Hout) if (t >= a && t < b) nh++;
        long wantl = pal ? 626 : 528;
        CHECK(nh == wantl, "lines a frame: %ld, expected %ld (%s %s)", nh, wantl, wv ? "woven" : "240p", pal ? "PAL" : "NTSC");
        printf("  frame %s %s: %ld clocks, %ld lines\n", wv ? "woven" : "240p ", pal ? "PAL " : "NTSC", b - a, nh);
    }
    // the polarity: idle levels while the sync is not on
    {
        long ah = 0, av = 0;
        for (long i = 0; i < N; i++) { ah += oh(i); av += ov(i); }
        printf("  hs_o active %ld clocks, vs_o active %ld clocks (of %ld)\n", ah, av, N);
        CHECK(ah > 0 && av > 0, "no sync");
    }
    printf("%s: %s (%d failures)\n", test.c_str(), fails ? "FAIL" : "PASS", fails);
    delete d;
    return fails != 0;
}
