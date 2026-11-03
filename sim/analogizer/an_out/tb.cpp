// an_out against the scan-out's timing and a flat colour: the pin map of each mode (docs/analogizer.md), the blanking, and the sync
// pins. One line of numbers a mode; exit status 1 on the first mismatch.
#include "Van_out.h"
#include "scanout.h"
#include <cstdio>
#include <cstdlib>

static Van_out *t;
static Scanout *so;
static void clk() {
    so->outputs();
    t->hs = so->hs; t->vs = so->vs; t->de = so->de; t->skip = so->ce == 1;
    t->clk = 0; t->eval(); t->clk = 1; t->eval();
    so->step();
}
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { if (fails++ < 12) { printf("FAIL mode %d: ", mode); printf(__VA_ARGS__); printf("\n"); } } } while (0)

int main()
{
    const unsigned R = 0xAC, G = 0x58, B = 0xF4;
    for (int mode = 1; mode <= 6; mode++) {
        bool pal = mode == 5;
        Scanout s(pal); so = &s;
        t = new Van_out;
        t->rst = 1; t->mode = mode; t->scanlines = 0; t->woven = 0; t->rgb = 0; clk(); clk(); t->rst = 0;
        long hs_fall = 0, vs_fall = 0, act = 0, blk_ok = 0, blk_n = 0;
        int ph = 0, pv = 0;
        for (long i = 0; i < 2 * 210000 * 2 + 4000; i++) {
            t->rgb = (R << 16) | (G << 8) | B;
            clk();
            if (i < 2 * 210000) continue;                                    // two fields of warm-up; one field is then looked at
            unsigned b3 = t->bank3, b2 = t->bank2, b1 = t->bank1_b;
            unsigned r6 = b3 >> 2, hsp = (b3 >> 1) & 1, vsp = b3 & 1, g6 = b2 & 63, blank_n = (b2 >> 6) & 1, b6 = ((b1 & 31) << 1) | (b2 >> 7);
            if (!hsp && ph) hs_fall++; if (!vsp && pv) vs_fall++; ph = hsp; pv = vsp;
            if (blank_n && so->hc > 40 && so->hc < 600 && so->line > 20 && so->line < 220) {        // inside the picture
                act++;
                if (mode == 1 || mode == 2) { CHECK(r6 == R >> 2 && g6 == G >> 2 && b6 == B >> 2, "rgb %02x %02x %02x", r6, g6, b6); }
                if (mode == 3) {            // Pr, Y, Pb of (R, G, B), BT.601 studio swing
                    double y = 16 + (65.738 * R + 129.057 * G + 25.064 * B) / 256, pb = 128 + (-37.945 * R - 74.494 * G + 112.439 * B) / 256, pr = 128 + (112.439 * R - 94.154 * G - 18.285 * B) / 256;
                    CHECK(abs((int)r6 - (int)(pr / 4)) <= 1 && abs((int)g6 - (int)(y / 4)) <= 1 && abs((int)b6 - (int)(pb / 4)) <= 1, "pr %d y %d pb %d vs %.0f %.0f %.0f", r6, g6, b6, pr / 4, y / 4, pb / 4);
                }
                if (mode == 4 || mode == 5) CHECK(b6 == 0 && blank_n, "B must be 0 (got %d)", b6);
            }
            if (so->hc > 700 && so->hc < 780 && so->line > 20 && so->line < 220 && (mode == 1 || mode == 2 || mode == 3)) {   // blanking
                blk_n++; blk_ok += (mode == 3 ? 1 : (r6 == 0 && g6 == 0 && b6 == 0 && !blank_n));
            }
        }
        // sync pins: RGBS has csync on HS and 1 on VS; RGsB and YPbPr csync on VS and 1 on HS; Y/C csync on HS; RGBHV both
        printf("mode %d: picture clocks %ld, blanking %ld/%ld, HS falls %ld, VS falls %ld\n", mode, act, blk_ok, blk_n, hs_fall, vs_fall);
        CHECK(act > 1000, "no picture seen");
        if (mode != 3) CHECK(blk_ok == blk_n, "blanking colours/blank not zero");
        if (mode == 1 || mode == 4 || mode == 5) CHECK(hs_fall > 200 && vs_fall == 0, "sync on HS only");
        if (mode == 2 || mode == 3) CHECK(vs_fall > 200 && hs_fall == 0, "sync on VS only");
        if (mode == 6) CHECK(hs_fall > 400 && vs_fall >= 1 && vs_fall <= 4, "RGBHV: hsync %ld vsync %ld", hs_fall, vs_fall);
        delete t;
    }
    printf(fails ? "FAILED (%d)\n" : "ALL PASS\n", fails);
    return fails ? 1 : 0;
}
