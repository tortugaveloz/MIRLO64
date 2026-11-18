/* The console's RDP timing (rdp_time.h). Every number below says where it comes from, with its confidence:
 *
 *   HW   measured on a console and published: Thar0's RDP-Timing-Tests (github.com/Thar0/RDP-Timing-Tests, sample_results.txt:
 *        a 320 x 240 RGBA16 fill rectangle drawn in 1-cycle and 2-cycle mode, 1000 runs each, the DPC's PIPEBUSY and BUFBUSY
 *        counters, VI off; 100 tests with every IM_RD / Z_CMP / Z_UPD / pass-or-fail / alpha-compare / bank case, 54 of them
 *        (the minimum of PIPEBUSY) in sim/rdp/hw_timing/hw_rects.py; MIT; the VI-on tests are not used)
 *   HW2  measured on a console, published as a code comment, the raw data not: the cen64 fork's dpc_probe law
 *        (gitlab.com/jgemu/cen64, src/rdp/rdp_core.c, "Hardware-measured span law"), 1-cycle mode, VI blanked, the
 *        minimum of 8 runs: 14 clocks a rectangle, 12 a span, a pixel clock 129/128
 *   DOC  the vendor's documents: the N64 Programming Manual ch. 12 (1 pixel a clock in 1-cycle mode, 1 per 2 in 2-cycle
 *        mode, 64 bits a clock in fill and copy mode: theoretical peaks, "reached on large rectangles")
 *   WIKI n64brew's RDP pages (Reality_Display_Processor/Pipeline, /Commands): the syncs 25/50/33 and the one clock of an
 *        attribute setter or a NOP; written by someone who tests on hardware, no method given
 *   EST  an estimate: no source gives the console's figure; the value is a choice, and says which
 *
 * The 54 hardware timings of large rectangles are what the memory terms below are fitted to; the model reproduces them to a
 * geometric mean of 4 % (worst 11 %). A triangle's own setup, a texture load, a fill's memory, the copy mode's memory and
 * a SYNC FULL have no console measurement anywhere (marked EST below). One more caveat, from the commit message of the
 * earlier rdp-pace branch (not reproduced here: it needs a save state of the game): a real game (Donkey Kong 64's jungle) was
 * run by the console at 2.94 fields a frame, and the RTL's own RDP time there was longer than these rates charge for the
 * same commands, so a heavy 3D scene may take the console longer than this model says: the rectangle timings do not
 * contain a textured triangle's rate.
 *
 * rtl/rdp/rdp_top.v holds the RTL to these rules a pixel at a time (its eighths of a clock, `pk`) where this model charges
 * a 16-byte group: the same for a row of whole groups, within a group's transfer for a short span. */
#include "rdp_time.h"
#include <stdlib.h>
#include <string.h>

/* ---- the constants ------------------------------------------------------------------------------------------------ */

/* A primitive's fixed cost: 13 clocks and a clock for each 64-bit word of its command. HW2 measured 14 for a rectangle
 * "including its 2-word fetch" (a fill rectangle is one 64-bit word); Thar0's 320 x 240 compute-only runs add up to
 * 240 * 324 + 12 (1-cycle) and 240 * 646 + 9 (2-cycle), 12 and 9 clocks over the rows. A triangle's 4 to 22 words at a
 * clock each is the MiSTer core's rule and the floor of a 64-bit command path; the console's figure for a triangle is
 * EST (no source measures one). */
#define PRIM_BASE      13
/* A span with no memory transfer in it (alpha compare failing every pixel, no IM_RD, no Z_CMP: the pipeline runs without
 * ever waiting for RDRAM): HW Thar0, 324 clocks for 320 pixels in 1-cycle mode and 646 in 2-cycle mode (the n64brew
 * "dead cycle at the end of every line": 4 and 6 clocks of nothing a line, not a clock). */
#define ROW_FREE_1C    4
#define ROW_FREE_2C    6
/* A span that moves memory: 12 clocks and 129/128 a pixel clock when the pipeline is the limit (HW2; Thar0's
 * colour-write-only rectangle is 80467 = 240 * 335.3 + 10: 12 + 320 * 129/128 = 334.5 + 0.3) */
#define ROW_MEM        12
/* The RDRAM's moves, in 16-byte groups of the colour image (8 pixels of a 16-bit one): a transfer (the colour read with
 * IM_RD, the depth read with Z_CMP, the colour write, the depth write) takes 7 clocks of a group, and 3 more a group when
 * the span reads, and 4 when the depth image shares the colour image's 1 MiB RDRAM bank and the group moves both (the
 * open row is closed and reopened between). HW, fitted to Thar0's 54 depth / image-read / pass-or-fail / bank cases of
 * 1-cycle and 2-cycle: geometric mean 1.04, worst 1.11 and 0.92. The colour is read with IM_RD whether the blender
 * reads it or not (Thar0's README: "when force_blend is disabled the RDP will still perform framebuffer reads"). */
#define XFER           7
#define READ_EXTRA     3
#define SAMEBANK_EXTRA 4
/* Fill mode: 64 bits a clock straight to the RDRAM (DOC: the peak, bypassing the span buffers), 12 clocks a span (HW2's
 * span cost, taken for another cycle type as its author does: "same silicon"). The console's real fill rate is not
 * measured by anyone (EST: the peak is the upper bound; the RDRAM's own row and refresh costs are not in it). */
#define FILL_ROW       12
/* The syncs (WIKI): SYNC LOAD 25, SYNC PIPE 50, SYNC TILE 33; SYNC FULL: no fixed figure (it waits for the pipeline and the
 * writes), the cen64 author's stand-in is 50 (EST). An attribute setter and a NOP: 1. */
#define SYNC_LOAD      25
#define SYNC_PIPE      50
#define SYNC_TILE      33
#define SYNC_FULL      50
/* A texture load: 64 bits of TMEM a clock plus one (EST: the MiSTer core's and Angrylion's loop; no console measurement;
 * LOAD TILE's per-row cost, "slower than LOAD BLOCK" (WIKI), is not in it) */

/* ---- the state a primitive's time is built from ----------------------------------------------------------------------- */

void rdp_time_init(rdp_time_t *t)
{
    memset(t, 0, sizeof *t);
    t->wseg = (uint8_t *)calloc((16u << 20) >> 4 >> 3, 1);     /* a bit a 16-byte group of 16 MiB */
}

void rdp_time_free(rdp_time_t *t)
{
    free(t->sp); free(t->wseg); free(t->wlist);
    memset(t, 0, sizeof *t);
}

void rdp_time_begin(rdp_time_t *t)
{
    t->n_sp = 0;
    for (unsigned i = 0; i < t->n_w; i++) t->wseg[t->wlist[i] >> 3] = 0;
    t->n_w = 0;
}

void rdp_time_span(rdp_time_t *t, int y, int x0, int x1)
{
    if (t->n_sp == t->cap_sp) { t->cap_sp = t->cap_sp ? t->cap_sp * 2 : 1024; t->sp = (rdp_time_span_t *)realloc(t->sp, t->cap_sp * sizeof *t->sp); }
    t->sp[t->n_sp].y = y; t->sp[t->n_sp].x0 = x0; t->sp[t->n_sp].x1 = x1; t->n_sp++;
}

void rdp_time_write(rdp_time_t *t, uint32_t a)
{
    uint32_t g = (a & ((16u << 20) - 1)) >> 4;
    if (t->wseg[g >> 3] & (1u << (g & 7))) return;
    if (t->n_w == t->cap_w) { t->cap_w = t->cap_w ? t->cap_w * 2 : 4096; t->wlist = (uint32_t *)realloc(t->wlist, t->cap_w * sizeof *t->wlist); }
    t->wseg[g >> 3] |= (uint8_t)(1u << (g & 7));
    t->wlist[t->n_w++] = g;
}
static int written(const rdp_time_t *t, uint32_t g) { g &= (1u << 20) - 1; return (t->wseg[g >> 3] >> (g & 7)) & 1; }

/* ---- a primitive ------------------------------------------------------------------------------------------------------- */

static uint64_t prim(const rdp_time_t *t, const rdp_t *r, unsigned qwords, int with_mem)
{
    unsigned cyc = (r->other_h >> RDP_H_CYCLE_SHIFT) & 3, csz = r->cimg_size;
    unsigned bpp = csz == 3 ? 4 : csz == 1 ? 1 : 2;
    int rd_c = (r->other_l & RDP_L_IM_RD) && cyc < 2;
    int rd_z = (r->other_l & RDP_L_Z_CMP) && cyc < 2 && csz != 1;
    uint64_t total = PRIM_BASE + qwords;
    for (unsigned s = 0; s < t->n_sp; s++) {
        const rdp_time_span_t *sp = &t->sp[s];
        uint32_t ca = r->cimg + ((uint32_t)sp->y * r->cimg_width + (uint32_t)sp->x0) * bpp;
        if (cyc == 3) {                                        /* fill: 64-bit words, whole words of the row's bytes */
            uint32_t b0 = ca, b1 = ca + (uint32_t)(sp->x1 - sp->x0 + 1) * bpp;
            total += (with_mem ? FILL_ROW : ROW_FREE_1C) + ((b1 + 7) >> 3) - (b0 >> 3);
            continue;
        }
        uint32_t za = r->zimg + ((uint32_t)sp->y * r->cimg_width + (uint32_t)sp->x0) * 2;
        int x = sp->x0, any_tr = 0;
        double acc = 0, pix = 0;
        while (x <= sp->x1) {
            uint32_t g = ca >> 4;                              /* the group: pixels to the next 16-byte boundary of the colour image */
            int n = 0; uint32_t zg0 = za >> 4, zg1 = zg0;
            while (x <= sp->x1 && (ca >> 4) == g) { zg1 = za >> 4; n++; x++; ca += bpp; za += 2; }
            double p = cyc == 0 ? n : cyc == 1 ? 2.0 * n : (double)n * bpp / 8.0;
            pix += p;
            if (with_mem) {
                unsigned tr = (rd_c ? 1 : 0) + (rd_z ? 1 : 0) + (written(t, g) ? 1 : 0);
                int wz = 0; for (uint32_t q = zg0; q <= zg1; q++) if (written(t, q) && q != g) wz = 1;
                tr += wz;
                int zt = rd_z || wz, ctr = rd_c || written(t, g);
                double m = (double)XFER * tr + ((((r->cimg ^ r->zimg) >> 20) == 0 && zt && ctr) ? SAMEBANK_EXTRA : 0.0);
                double q = p * 129.0 / 128.0;
                /* (the pipeline's clocks wait the 3 only when it compares depth: an IM_RD-only 2-cycle span is its 16 clocks a group) */
                double qq = q + (rd_z ? READ_EXTRA : 0.0), mm = m + ((rd_c || rd_z) ? READ_EXTRA : 0.0);
                acc += qq > mm ? qq : mm;
                any_tr |= tr != 0;
            }
        }
        if (with_mem && any_tr) total += ROW_MEM + (uint64_t)(acc + 0.5);
        else total += (cyc == 1 ? ROW_FREE_2C : ROW_FREE_1C) + (uint64_t)(pix + 0.5);
    }
    return total;
}

uint64_t rdp_time_cmd(rdp_time_t *t, const rdp_t *r, const uint32_t *w, unsigned n, int with_mem)
{
    unsigned op = (w[0] >> 24) & 63;
    if ((op >= 0x08 && op <= 0x0F) || op == 0x24 || op == 0x25 || op == 0x36) return prim(t, r, n / 2, with_mem);
    switch (op) {
    case 0x26: return SYNC_LOAD;
    case 0x27: return SYNC_PIPE;
    case 0x28: return SYNC_TILE;
    case 0x29: return SYNC_FULL;
    case 0x30: case 0x33: case 0x34: {                         /* LOAD TLUT, LOAD BLOCK, LOAD TILE */
        int32_t sl = (w[0] >> 12) & 0xFFF, tl = w[0] & 0xFFF, sh = (w[1] >> 12) & 0xFFF, th = w[1] & 0xFFF;
        int64_t tx = op == 0x33 ? (int64_t)(sh - sl) + 1 : ((int64_t)((sh >> 2) - (sl >> 2)) + 1) * ((int64_t)((th >> 2) - (tl >> 2)) + 1);
        if (tx < 0) tx = 0;
        uint64_t bytes = op == 0x30 ? (uint64_t)tx * 2 : ((uint64_t)tx << r->timg_size) >> 1;
        return (bytes + 7) / 8 + 1;
    }
    default: return 1;
    }
}
