/* The GameShark picker's scrolling of a row that does not fit (n64rt_gspick.c): the pixel offset of its text, from the time since the cursor came to the row. A pure function, host-tested by
 * test/gsscroll_host.c. The text waits 1 s at its start, then moves to the left (the window goes to the end of the text) at 35 pixels a second, waits 2 s at the end and comes back
 * smoothly at the same speed to the start, where the whole starts again. A text that fits is never moved. */
#ifndef N64RT_GSSCROLL_H
#define N64RT_GSSCROLL_H
#include <stdint.h>

#define GS_SCROLL_SPEED   35u           /* pixels a second, both ways */
#define GS_SCROLL_START   1000u         /* ms at the start before it moves */
#define GS_SCROLL_END     2000u         /* ms at the end before it comes back */

/* `t_ms`: the time since the cursor came to the row, `text_px`: the text's width, `win_px`: the window's. The result: how many pixels the text is moved to the left (0 .. text_px - win_px). */
static inline uint32_t n64rt_gsscroll(uint32_t t_ms, uint32_t text_px, uint32_t win_px)
{
    if (text_px <= win_px) return 0;
    const uint32_t d = text_px - win_px;
    const uint32_t run = (d * 1000u + GS_SCROLL_SPEED - 1u) / GS_SCROLL_SPEED;           /* the ms to go the whole way */
    uint32_t t = t_ms % (GS_SCROLL_START + run + GS_SCROLL_END + run);
    if (t < GS_SCROLL_START) return 0;
    t -= GS_SCROLL_START;
    if (t < run) { const uint32_t o = t * GS_SCROLL_SPEED / 1000u; return o < d ? o : d; }
    t -= run;
    if (t < GS_SCROLL_END) return d;
    t -= GS_SCROLL_END;
    { const uint32_t o = t * GS_SCROLL_SPEED / 1000u; return o < d ? d - o : 0u; }
}
#endif
