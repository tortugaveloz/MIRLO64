/* The RDP commands at the head of every frame: scissor, depth clear, color
 * clear, depth image. One definition for the game CPU (frame.c puts them in
 * the frame's GDL as GDL_RAW), the host replay and the geom-core RTL bench
 * (sim/geom_full) -- so they cannot
 * disagree about what a frame starts from. Header-only, no libc. */
#ifndef RDP_FRAME_H
#define RDP_FRAME_H

#include <stdint.h>
#include "rdp.h"

#define RDP_FRAME_CLEAR_WORDS 18u

/* w[RDP_FRAME_CLEAR_WORDS]; returns the word count. Depth clears to the far
 * plane (0xFFFF), color to rgb888 as RGB565. hres must be even (fill mode
 * writes pixel pairs). Leaves the color image on fb and the mode on 1-cycle
 * with no Z (SET_OTHER 0): every draw sets its own mode first. */
static inline unsigned rdp_frame_clear(uint32_t *w, uint32_t fb, uint32_t zb,
                                        unsigned hres, unsigned vres, uint32_t rgb888)
{
    uint32_t r = (rgb888 >> 16) & 0xFF, g = (rgb888 >> 8) & 0xFF, b = rgb888 & 0xFF;
    uint32_t c565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    uint32_t xl = (uint32_t)(hres - 1) << 2, yl = (uint32_t)(vres - 1) << 2;
    unsigned n = 0;
#define RDP_FC(hi, lo) do { w[n++] = (hi); w[n++] = (lo); } while (0)
    RDP_FC((uint32_t)RDP_OP_SET_SCISSOR << 24, ((uint32_t)hres << 2) << 12 | ((uint32_t)vres << 2));
    RDP_FC((uint32_t)RDP_OP_SET_OTHER << 24 | (3u << RDP_H_CYCLE_SHIFT), 0);
    RDP_FC((uint32_t)RDP_OP_SET_CIMG << 24 | (hres - 1), zb);
    RDP_FC((uint32_t)RDP_OP_SET_FILL << 24, 0xFFFFFFFFu);
    RDP_FC((uint32_t)RDP_OP_FILL_RECT << 24 | (xl << 12) | yl, 0);
    RDP_FC((uint32_t)RDP_OP_SET_CIMG << 24 | (hres - 1), fb);
    RDP_FC((uint32_t)RDP_OP_SET_FILL << 24, c565 << 16 | c565);
    RDP_FC((uint32_t)RDP_OP_FILL_RECT << 24 | (xl << 12) | yl, 0);
    RDP_FC((uint32_t)RDP_OP_SET_ZIMG << 24, zb);
#undef RDP_FC
    return n;
}

/* The end of a frame: SYNC_FULL. RDP counts it in sync_count once every
 * pixel before it is in SDRAM -- what the game CPU flips on. */
static inline unsigned rdp_frame_end(uint32_t *w)
{
    w[0] = (uint32_t)RDP_OP_SYNC_FULL << 24;
    w[1] = 0;
    return 2;
}

#endif
