/* The VI's window in the scan-out's 240-line field (rtl/soc/n64_video.sv): the picture is `rows` lines from line y0 and its line j shows the buffer's row
 * (j ys) >> 10. A window that has more lines than the field has from y0 -- PAL's wide modes: Mario Golf (Europe) writes V_START 0x2F0269, 285 lines
 * from the half-line 47 (before the 95 the field starts at) with Y_SCALE 0x35C, a 240-row buffer stretched to them -- is FITTED to what is left of the
 * field, not cut at its last line: the lines are fewer and the step Y_SCALE takes through the buffer larger by their ratio, so that the window's first and
 * last lines show the same rows of the buffer as the VI's (the last of its 285 lines is the row 238 with 0x35C, and so it is of the 240: 0x3FD). That is what
 * the scan-out showed before the window was the VI's (a 240-line frame: all the rows of the buffer, 1:1). Pure arithmetic: sim/vi_filter/vifit_test.c checks it. */
#ifndef N64RT_VIFIT_H
#define N64RT_VIFIT_H
static inline void n64rt_vi_fit(unsigned y0, unsigned rows, unsigned ys, unsigned *rows_o, unsigned *ys_o)
{
    unsigned r = rows, s = ys;
    if (rows > 1u && y0 < 239u && y0 + rows > 240u) {
        r = 240u - y0;
        s = (ys * (rows - 1u)) / (r - 1u);          /* (the window's last line, line rows - 1, is the line r - 1; rounded down: never a row past the VI's last) */
        if (s > 0x7FFu) s = 0x7FFu;
    }
    *rows_o = r; *ys_o = s;
}
#endif
