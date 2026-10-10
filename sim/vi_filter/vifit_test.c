/* lang/n64rt/n64rt_vifit.h: a VI window taller than the scan-out's 240-line field is fitted to it, not cut. Host test (cc vifit_test.c). */
#include <stdio.h>
#include <stdlib.h>
#include "../../lang/n64rt/n64rt_vifit.h"
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)
static unsigned row_of(unsigned j, unsigned ys) { return (j * ys) >> 10; }
int main(void)
{
    unsigned r, s;
    /* Mario Golf (Europe), the VI's registers as the sim reads them (V_SYNC 0x271, H_START 0x800300, V_START 0x002F0269, Y_SCALE 0x35C, WIDTH 320):
     * 285 lines from the half-line 47, before the PAL field's 95: line 0 of the field, rows 0..284 of the window, a 240-row buffer */
    n64rt_vi_fit(0, 285, 0x35C, &r, &s);
    CHECK(r == 240 && s == 0x3FD, "Mario Golf: rows %u ys %03x (want 240, 3FD)", r, s);
    CHECK(row_of(r - 1, s) == row_of(285 - 1, 0x35C), "Mario Golf: the last line shows the row %u, the VI's last shows %u", row_of(r - 1, s), row_of(284, 0x35C));
    CHECK(row_of(0, s) == 0, "Mario Golf: the first line's row");
    /* the modes that fitted: 237 lines (libultra's NTSC and PAL: SM64 (Europe) V_START 0x5F0239), GoldenEye's 237 lines of 330 rows, Banjo-Kazooie's 216 from 12 */
    n64rt_vi_fit(0, 237, 0x400, &r, &s); CHECK(r == 237 && s == 0x400, "237 lines changed: %u %03x", r, s);
    n64rt_vi_fit(0, 237, 0x580, &r, &s); CHECK(r == 237 && s == 0x580, "GoldenEye changed: %u %03x", r, s);
    n64rt_vi_fit(12, 216, 0x400, &r, &s); CHECK(r == 216 && s == 0x400, "Banjo-Kazooie changed: %u %03x", r, s);
    n64rt_vi_fit(3, 237, 0x400, &r, &s); CHECK(r == 237 && s == 0x400, "237 lines from 3 (240 exactly) changed: %u %03x", r, s);
    n64rt_vi_fit(0, 240, 0x400, &r, &s); CHECK(r == 240 && s == 0x400, "240 lines changed: %u %03x", r, s);
    /* a PAL mode of the whole 288 lines (V_START 0x2F..0x26F), a 288-row buffer: all of it in the 240 lines, the last row the last */
    n64rt_vi_fit(0, 288, 0x400, &r, &s);
    CHECK(r == 240 && row_of(r - 1, s) >= 286 && row_of(r - 1, s) <= 287, "288 lines: %u rows, last row %u (want 286..287)", r, row_of(r - 1, s));
    /* one more than the field has from y0: the window is whole in what is left */
    n64rt_vi_fit(4, 237, 0x400, &r, &s);
    CHECK(r == 236 && 4 + r <= 240 && row_of(r - 1, s) >= 235 && row_of(r - 1, s) <= 236, "237 lines from 4: %u rows ys %03x last row %u", r, s, row_of(r - 1, s));
    /* any window: it ends in the field, and its first and last rows are the game's (the step is rounded down: the last row at most one short) */
    srand(1);
    for (int i = 0; i < 100000; i++) {
        unsigned y0 = rand() % 239, rows = 2 + rand() % 400, ys = 0x100 + rand() % 0x600;
        n64rt_vi_fit(y0, rows, ys, &r, &s);
        CHECK(y0 + r <= 240 || y0 + rows <= 240, "y0 %u rows %u ys %x: ends at %u", y0, rows, ys, y0 + r);
        if (y0 + rows > 240 && s < 0x7FF) {
            int d = (int)row_of(r - 1, s) - (int)row_of(rows - 1, ys);
            CHECK(d >= -1 && d <= 0, "y0 %u rows %u ys %x -> %u, %x: last row differs by %d (the VI's: never a row past it)", y0, rows, ys, r, s, d);
        }
    }
    printf("vifit: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
