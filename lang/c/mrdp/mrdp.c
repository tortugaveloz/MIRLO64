/* MRDP bit-exact reference model -- see mrdp.h (the contract).
 * Integer arithmetic only; every shift, round and
 * clamp here is what the RTL does. */
#include "mrdp.h"
#include "mrdp_setup.h"

#include <string.h>
#if defined(MRDP_DEBUG_PIXEL) || defined(MRDP_DEBUG_AA)
#include <stdio.h>
#endif

/* ---- helpers ------------------------------------------------------------ */

static inline int32_t sext(uint32_t v, int bits)
{
    return (int32_t)(v << (32 - bits)) >> (32 - bits);
}
static inline int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

int mrdp_cmd_len(uint32_t w0)
{
    unsigned op = (w0 >> 24) & 0x3F;
    if (op >= 0x08 && op <= 0x0F)
        return 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
#ifdef MRDP_N64
    /* (the N64's commands only: MRDP's own triangles are unknown ones) */
    return (op == MRDP_OP_TEXRECT || op == MRDP_OP_TEXRECT_FLIP) ? 2 : 1;
#endif
    if (op == MRDP_OP_TRI_R)
        return (int)(mrdp_tri_r_words((w0 >> 19) & 7u) / 2u);
    if (op >= MRDP_OP_TRI_V && op <= (MRDP_OP_TRI_V | 7u))
        return (int)(mrdp_tri_v_words(op & 7u) / 2u);
    if (op >= MRDP_OP_TRI_G && op <= (MRDP_OP_TRI_G | 7u))
        return (int)(mrdp_tri_g_words(op & 7u) / 2u);
    switch (op) {
    case MRDP_OP_TEXRECT: case MRDP_OP_TEXRECT_FLIP:
        return 2;
    case MRDP_OP_NOP: case MRDP_OP_SYNC_LOAD: case MRDP_OP_SYNC_PIPE:
    case MRDP_OP_SYNC_TILE: case MRDP_OP_SYNC_FULL: case MRDP_OP_SET_SCISSOR:
    case MRDP_OP_SET_PRIM_Z: case MRDP_OP_SET_OTHER: case MRDP_OP_SET_TILESIZE:
    case MRDP_OP_LOAD_TILE: case MRDP_OP_SET_TILE: case MRDP_OP_FILL_RECT:
    case MRDP_OP_SET_FILL: case MRDP_OP_SET_FOG: case MRDP_OP_SET_BLEND:
    case MRDP_OP_SET_PRIM: case MRDP_OP_SET_ENV: case MRDP_OP_SET_COMBINE:
    case MRDP_OP_SET_TIMG: case MRDP_OP_SET_ZIMG: case MRDP_OP_SET_CIMG:
        return 1;
    default:
        return 1;   /* unknown: one word, counted and skipped */
    }
}

/* ---- perspective: 1/W from a 64-entry table + linear step ---------------
 * W > 0 is normalised so bit 30 is its top bit (wn = W << sh). With
 * idx = wn[29:24] and frac = wn[23:15]:
 *   r = P[idx] - ((D[idx] * frac) >> 9),  P[i] = round(2^22 / (64 + i))
 * so r ~ 2^16 / m for the mantissa m = wn / 2^30 in [1, 2), r in [2^15, 2^16].
 * Then S/W in s10.5 is (S * r) >> (27 - sh), W being 2.30 (1.0 = 2^30). */
static uint32_t s_rcp_p[65], s_rcp_d[64];
static int s_rcp_ready;

static void rcp_init(void)
{
    for (int i = 0; i <= 64; i++)
        s_rcp_p[i] = ((1u << 23) / (64 + i) + 1) >> 1;   /* round(2^22/(64+i)) */
    for (int i = 0; i < 64; i++)
        s_rcp_d[i] = s_rcp_p[i] - s_rcp_p[i + 1];
    s_rcp_ready = 1;
}

uint32_t mrdp_rcp(uint32_t w, int *shift)
{
    if (!s_rcp_ready) rcp_init();
    if ((int32_t)w <= 0) w = 1;
    int sh = __builtin_clz(w) - 1;
    uint32_t wn = w << sh;
    uint32_t idx = (wn >> 24) & 63, frac = (wn >> 15) & 511;
    *shift = sh;
    return s_rcp_p[idx] - ((s_rcp_d[idx] * frac) >> 9);
}

int32_t mrdp_persp(int32_t s, uint32_t w)
{
    int sh;
    uint32_t r = mrdp_rcp(w, &sh);
    if (s > (1 << 26) - 1) s = (1 << 26) - 1;       /* the 27-bit multiplier input */
    if (s < -(1 << 26)) s = -(1 << 26);
    int64_t p = (int64_t)s * (int64_t)r;
    int k = 27 - sh;                  /* W is 2.30 (mrdp_slope30): 41 - 14 */
    int64_t v = p >> (k > 0 ? k : 0); /* saturated as the RTL's 45-bit product */
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int32_t)v;
}

static inline int32_t s105_noperp(int32_t s)
{
    int32_t v = s >> 11;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

#if defined(RDP_EXACT) && !defined(MRDP_CVG)
#error "RDP_EXACT is the N64's pixel pipeline's: build it with MRDP_N64 (and without MRDP_NO_BLEND)"
#endif
#ifdef MRDP_CVG
/* ---- the console's perspective divide (MRDP_CVG; docs/rdp.md, Coordinates) ---------
 * Only the 16-bit integer halves of S (or T, s10.5) and W (s.15) enter it. W's leading zeros among its low 15 bits (up to
 * 14) normalise it; the next 6 bits pick a point of the reciprocal and the 8 after them step along the slope there: a
 * 15-bit reciprocal. S times it, shifted back by the normalisation, is S / W in s10.5, 17 bits kept. The product's bits
 * above those, unless all copies of its sign, put the result out of range (range 2 over, 1 under), and so does a W whose
 * integer half is zero or negative (over), whatever S is. The tables are the console's: with them the texel coordinates
 * match a captured frame's to the subtexel, where the model's own reciprocal (mrdp_rcp, more bits of S and W) was off by
 * up to 69 subtexels at a floor's far end. */
static const uint16_t s_div_point[64] = {
    0x4000, 0x3f04, 0x3e10, 0x3d22, 0x3c3c, 0x3b5d, 0x3a83, 0x39b1,
    0x38e4, 0x381c, 0x375a, 0x369d, 0x35e5, 0x3532, 0x3483, 0x33d9,
    0x3333, 0x3291, 0x31f4, 0x3159, 0x30c3, 0x3030, 0x2fa1, 0x2f15,
    0x2e8c, 0x2e06, 0x2d83, 0x2d03, 0x2c86, 0x2c0b, 0x2b93, 0x2b1e,
    0x2aab, 0x2a3a, 0x29cc, 0x2960, 0x28f6, 0x288e, 0x2828, 0x27c4,
    0x2762, 0x2702, 0x26a4, 0x2648, 0x25ed, 0x2594, 0x253d, 0x24e7,
    0x2492, 0x243f, 0x23ee, 0x239e, 0x234f, 0x2302, 0x22b6, 0x226c,
    0x2222, 0x21da, 0x2193, 0x214d, 0x2108, 0x20c5, 0x2082, 0x2041,
};
static const uint16_t s_div_slope[64] = {
    0x0f03, 0x0f0b, 0x0f11, 0x0f19, 0x0f20, 0x0f25, 0x0f2d, 0x0f32,
    0x0f37, 0x0f3d, 0x0f42, 0x0f47, 0x0f4c, 0x0f50, 0x0f55, 0x0f59,
    0x0f5d, 0x0f62, 0x0f64, 0x0f69, 0x0f6c, 0x0f70, 0x0f73, 0x0f76,
    0x0f79, 0x0f7c, 0x0f7f, 0x0f82, 0x0f84, 0x0f87, 0x0f8a, 0x0f8c,
    0x0f8e, 0x0f91, 0x0f93, 0x0f95, 0x0f97, 0x0f99, 0x0f9b, 0x0f9d,
    0x0f9f, 0x0fa1, 0x0fa3, 0x0fa4, 0x0fa6, 0x0fa8, 0x0fa9, 0x0faa,
    0x0fac, 0x0fae, 0x0faf, 0x0fb0, 0x0fb2, 0x0fb3, 0x0fb5, 0x0fb5,
    0x0fb7, 0x0fb8, 0x0fb9, 0x0fba, 0x0fbc, 0x0fbc, 0x0fbe, 0x0fbe,
};
int32_t mrdp_n64_div(int32_t s, int32_t w, int *range)
{
    const int32_t s16 = (int16_t)((uint32_t)s >> 16), w16 = (int16_t)((uint32_t)w >> 16);
    const unsigned wi = (unsigned)w16 & 0x7FFFu;
    int shift = 14;
    for (int b = 14; b >= 0; b--) if ((wi >> b) & 1u) { shift = 14 - b; break; }
    const unsigned norm = (wi << shift) & 0x3FFFu, idx = norm >> 8, frac = (norm & 0xFFu) << 2;
    /* (the slope is stored as the 10 low bits of a negative number: | ~0x3FF, + 1 makes it the step's negation) */
    const int rcp = ((((int)(s_div_slope[idx] | ~0x3FFu) + 1) * (int)frac >> 10) + (int)s_div_point[idx]) & 0x7FFF;
    const int32_t prod = s16 * rcp;
    const int32_t mask = ((1 << 30) - 1) & -((1 << 29) >> shift), hi = prod & mask;
    *range = (hi != mask && hi != 0) ? ((prod & (1 << 29)) ? 1 : 2) : 0;
    if (w16 <= 0) *range = 2;
    const int32_t v = (shift != 14 ? prod >> (13 - shift) : prod * 2) & 0x1FFFF;
    return (v & 0x10000) ? v - 0x20000 : v;
}
/* S / W for the pixel's texel: the 17 bits saturated to 16, an out-of-range one to the end it went past (over 0x7FFF,
 * under -0x8000), as the texture unit's coordinate is 16 bits */
static int32_t n64_persp(int32_t s, int32_t w)
{
    int range;
    const int32_t v = mrdp_n64_div(s, w, &range);
    if (range) return range == 2 ? 32767 : -32768;
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}
#endif
#ifdef RDP_EXACT
/* S, T, W (o) of a point m steps d along from b: the walk's points, wrapping as the console's adders do */
static inline void x_pt(int32_t *o, const int32_t *b, int m, const int32_t *d)
{
    for (int k = 0; k < 3; k++) o[k] = (int32_t)((uint32_t)b[k] + (uint32_t)m * (uint32_t)d[k]);
}
#endif

/* ---- init / state ------------------------------------------------------- */

void mrdp_init(mrdp_t *r, void *ctx, mrdp_rd16_t rd16, mrdp_wr16_t wr16)
{
    memset(r, 0, sizeof *r);
    r->ctx = ctx;
    r->rd16 = rd16;
    r->wr16 = wr16;
    r->cimg_width = 1;
    r->timg_width = 1;
    r->sc_xl = 1023 << 2;
    r->sc_yl = 1023 << 2;
    r->dbg_x = r->dbg_y = -1;
    if (!s_rcp_ready) rcp_init();
}

static inline mrdp_rgba_t rgba_of(uint32_t w)
{
    mrdp_rgba_t c = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w };
    return c;
}

/* ---- TMEM --------------------------------------------------------------- */

static inline void tmem_locate(const mrdp_tile_t *t, int s, int tt, int *bank, int *addr)
{
    *bank = ((tt & 1) << 1) | (s & 1);
    *addr = (t->tmem + (tt >> 1) * t->line * 2 + (s >> 1)) & 1023;
}

static mrdp_rgba_t texel_expand(unsigned fmt, uint16_t v)
{
    mrdp_rgba_t c;
    switch (fmt) {
    case 1:     /* IA88 */
        c.r = c.g = c.b = (uint8_t)(v >> 8);
        c.a = (uint8_t)v;
        break;
    case 2: {   /* RGBA4444 */
        c.r = (uint8_t)(((v >> 12) & 15) * 17);
        c.g = (uint8_t)(((v >> 8) & 15) * 17);
        c.b = (uint8_t)(((v >> 4) & 15) * 17);
        c.a = (uint8_t)((v & 15) * 17);
        break;
    }
    default: {  /* RGBA5551 */
        unsigned r5 = (v >> 11) & 31, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
        c.r = (uint8_t)(r5 << 3 | r5 >> 2);
        c.g = (uint8_t)(g5 << 3 | g5 >> 2);
        c.b = (uint8_t)(b5 << 3 | b5 >> 2);
        c.a = (v & 1) ? 255 : 0;
        break;
    }
    }
    return c;
}

#ifdef MRDP_N64
/* TMEM as the N64's: 4 KiB, here 2048 16-bit words (tmem16[i] is bytes 2i,
 * 2i + 1). A tile's rows are `line` 64-bit words apart from `tmem`; a texel is
 * found by the tile's size:
 *   4-bit    word tmem*4 + t*line*4 + s/4, its nibble s%4 (the first the highest)
 *   8-bit    word ... + s/2, its byte s%2
 *   16-bit   word ... + s
 *   32-bit   R G in word (...+ s) of the lower half, B A in the same word of
 *            the upper half (as the N64 keeps them: LOAD BLOCK / LOAD TILE
 *            split a 32-bit RGBA texture so)
 *   YUV      16-bit: a pixel pair's U V in word (... + s/2) of the lower half,
 *            its Y0 Y1 in the same word of the upper half (the loads split
 *            the image's U Y0 V Y1 so); the texel is (U, V, Y, Y), U and V
 *            as their bytes (offset by 128: yuv_convert takes it off)
 * and decoded by the tile's format. With the TLUT on, a texel is an index
 * whatever the tile's format: a 4-bit one with the tile's palette, an 8-bit
 * one as it is, and of a 16- or 32-bit one the first byte of its word (of the
 * lower half) -- a game that loads its depth image as a texture and takes a
 * table's entry by the depth's top byte. Its colour is the 16-bit word
 * 4*index of the upper half (LOAD TLUT writes every entry four times, as the
 * N64).
 * On an odd row the two halves of each 64-bit word change places (the word
 * found is word ^ 2), as they did when the row was loaded -- the N64's, which
 * shows when a load's rows are not the tile's: a LOAD BLOCK with dxt 0 loads
 * every row as an even one, and a game that does that keeps its texture's
 * odd rows swapped in the ROM (a flight game's terrain). */
static mrdp_rgba_t tmem_fetch(const mrdp_t *r, const mrdp_tile_t *t, int s, int tt)
{
    const uint16_t *m = r->tmem16;
    s &= 0x7FF; tt &= 0x7FF;                         /* (the texture unit's 11 bits) */
    uint32_t row = (uint32_t)t->tmem * 4u + (uint32_t)tt * (uint32_t)t->line * 4u;
    const uint32_t odd = (tt & 1) ? 2u : 0u;
    mrdp_rgba_t c = { 0, 0, 0, 0 };
    const unsigned tlut = (r->other_h & MRDP_H_TLUT_EN) != 0, tlut_fmt = (r->other_h & MRDP_H_TLUT_IA) ? 1 : 0;
    if (t->size == 3) {
        uint32_t i = ((row + (uint32_t)s) ^ odd) & 0x3FF;
        if (tlut) return texel_expand(tlut_fmt, m[0x400 | ((unsigned)(m[i] >> 8) << 2)]);
        c.r = (uint8_t)(m[i] >> 8); c.g = (uint8_t)m[i];
        c.b = (uint8_t)(m[0x400 | i] >> 8); c.a = (uint8_t)m[0x400 | i];
        return c;
    }
    if (t->fmt == 1 && t->size == 2) {                                /* YUV */
        uint32_t i = ((row + ((uint32_t)s >> 1)) ^ odd) & 0x3FF;
        if (tlut) return texel_expand(tlut_fmt, m[0x400 | ((unsigned)(m[i] >> 8) << 2)]);   /* (its pair's U V word: U is the index) */
        c.r = (uint8_t)(m[i] >> 8); c.g = (uint8_t)m[i];
        c.b = c.a = (uint8_t)((s & 1) ? m[0x400 | i] : m[0x400 | i] >> 8);
        return c;
    }
    unsigned v = m[((row + ((uint32_t)s >> (2 - t->size))) ^ odd) & 0x7FF];
    unsigned n = (v >> (12 - 4 * (s & 3))) & 15u;                 /* the 4-bit texel */
    unsigned b = (s & 1) ? (v & 255u) : (v >> 8);                 /* the 8-bit one */
    if (tlut) {
        unsigned idx = t->size == 0 ? ((unsigned)t->palette << 4 | n) : t->size == 1 ? b : v >> 8;
        return texel_expand(tlut_fmt, m[0x400 | (idx << 2)]);
    }
    if (t->size == 2) {
        if (t->fmt == 3) return texel_expand(1, (uint16_t)v);     /* IA 8 8 */
        if (t->fmt <= 2) return texel_expand(0, (uint16_t)v);     /* RGBA 5551 (CI 16 is read as RGBA, as the console does) */
        c.r = c.g = c.b = c.a = (uint8_t)(v >> 8);                /* I: the first byte */
    } else if (t->size == 1) {
        if (t->fmt == 3) { c.r = c.g = c.b = (uint8_t)((b >> 4) * 17u); c.a = (uint8_t)((b & 15u) * 17u); }   /* IA 4 4 */
        else c.r = c.g = c.b = c.a = (uint8_t)b;                  /* I (and what has no 8-bit form) */
    } else {
        if (t->fmt == 3) { unsigned i3 = n >> 1; c.r = c.g = c.b = (uint8_t)(i3 << 5 | i3 << 2 | i3 >> 1); c.a = (n & 1u) ? 255 : 0; }   /* IA 3 1 */
        else c.r = c.g = c.b = c.a = (uint8_t)(n * 17u);          /* I */
    }
    return c;
}
#else
static mrdp_rgba_t tmem_fetch(const mrdp_t *r, const mrdp_tile_t *t, int s, int tt)
{
    int bank, addr;
    tmem_locate(t, s, tt, &bank, &addr);
    return texel_expand(t->fmt, r->tmem[bank][addr]);
}
#endif

/* ---- texture coordinate: shift, tile origin, clamp, mirror, mask ------ */

static int32_t tc_shift(int32_t v, unsigned shift)
{
    if (shift == 0) return v;
    if (shift <= 10) return v >> shift;
    return v << (16 - shift);
}

/* one axis: v in s10.5 after shift; returns the two integer texel
 * coordinates (v0, v0+1 after wrap) and the 5-bit fraction */
static void tc_axis(int32_t v, unsigned lo, unsigned hi, unsigned clampbit,
                    unsigned mirror, unsigned mask, int *c0, int *c1, int *frac)
{
    v -= (int32_t)lo << 3;                        /* u10.2 -> s10.5 */
    int i = v >> 5, f = v & 31;
    int j = i + 1;
    if (clampbit || mask == 0) {
        int mx = ((int)hi - (int)lo) >> 2;
        if (i < 0) { i = 0; j = 0; f = 0; }
        else if (i >= mx) { i = mx; j = mx; f = 0; }
    }
    if (mask) {
        int m = (1 << mask) - 1;
        if (mirror && ((i >> mask) & 1)) i = ~i;
        if (mirror && ((j >> mask) & 1)) j = ~j;
        i &= m;
        j &= m;
    }
    *c0 = i;
    *c1 = j;
    *frac = f;
}

static mrdp_rgba_t texture_sample(const mrdp_t *r, int tilei, int32_t s105, int32_t t105, int point)
{
    const mrdp_tile_t *t = &r->tile[tilei & 7];
    int32_t s = tc_shift(s105, t->shifts), tt = tc_shift(t105, t->shiftt);
    int s0, s1, t0, t1, fs, ft;
    tc_axis(s, t->uls, t->lrs, t->cs, t->ms, t->masks, &s0, &s1, &fs);
    tc_axis(tt, t->ult, t->lrt, t->ct, t->mt, t->maskt, &t0, &t1, &ft);

    mrdp_rgba_t a = tmem_fetch(r, t, s0, t0);
    if (point || ((r->other_h >> MRDP_H_FILT_SHIFT) & 3) == 0)   /* point */
        return a;

    /* N64 3-point filter: the triangle of the 2x2 the sample falls in */
    mrdp_rgba_t b = tmem_fetch(r, t, s1, t0);
    mrdp_rgba_t c = tmem_fetch(r, t, s0, t1);
    mrdp_rgba_t d = tmem_fetch(r, t, s1, t1);
    mrdp_rgba_t o;
    const uint8_t *pa = &a.r, *pb = &b.r, *pc = &c.r, *pd = &d.r;
    uint8_t *po = &o.r;
    int upper = fs + ft < 32;
    for (int k = 0; k < 4; k++) {
        int v;
        if (upper)
            v = pa[k] + (((pb[k] - pa[k]) * fs + (pc[k] - pa[k]) * ft + 16) >> 5);
        else
            v = pd[k] + (((pc[k] - pd[k]) * (32 - fs) + (pb[k] - pd[k]) * (32 - ft) + 16) >> 5);
        po[k] = (uint8_t)clamp8(v);
    }
    return o;
}

#ifdef RDP_EXACT
/* Copy mode's texel (docs/rdp_exact.md, rule 6): the console's copy takes the group's coordinate through the tile's
 * shift and origin to whole texels and adds the pixel's place in the group (m), then mirrors and masks each by the
 * tile's mask (at most 10 bits; none without a mask), and never clamps: a copy that reads past its tile's rows reads the
 * TMEM after them, as the console's does. (s, t: s10.5 in 16 bits, the perspective divide's.) */
static int x_copy_axis(int32_t v, unsigned shift, unsigned lo, unsigned mask, unsigned mirror, int m)
{
    v = (int16_t)v;
    v = shift < 11 ? v >> shift : (int16_t)(uint16_t)((uint32_t)v << (16 - shift));
    v = ((v - (int32_t)(lo << 3)) >> 5) + m;
    if (mask) {
        const unsigned mc = mask > 10 ? 10 : mask;
        if (mirror && ((v >> mc) & 1)) v = ~v;
        v &= (int32_t)((0xFFFFu >> (16 - mask)) & 0x3FFu);
    }
    return v;
}
static mrdp_rgba_t x_copy_texel(const mrdp_t *r, int tilei, int32_t s, int32_t t, int m)
{
    const mrdp_tile_t *tl = &r->tile[tilei & 7];
    return tmem_fetch(r, tl, x_copy_axis(s, tl->shifts, tl->uls, tl->masks, tl->ms, m), x_copy_axis(t, tl->shiftt, tl->ult, tl->maskt, tl->mt, 0));
}
#endif

static const mrdp_rgba_t s_no_texel = { 0, 0, 0, 0 };   /* (texture_pass's prev for the first pass) */
#ifdef MRDP_N64
/* The texture filter's YUV -> RGB (SET_CONVERT's K0..K3, the N64's 2K + 1 in 1/256ths), from a texel (U, V, Y, Y) as
 * tmem_fetch gives it: R = Y + K0 V, G = Y + K1 U + K2 V, B = Y + K3 U, A = Y, U and V less 128. Each channel clamped
 * to a byte (the hardware keeps 9 bits; the combiner's (T - K4) * K5 + T, YUV's, clamps the same pixels the same way). */
static mrdp_rgba_t yuv_convert(const mrdp_t *r, mrdp_rgba_t c)
{
    int u = c.r - 128, v = c.g - 128, y = c.b;
    int k0 = 2 * r->kc[0] + 1, k1 = 2 * r->kc[1] + 1, k2 = 2 * r->kc[2] + 1, k3 = 2 * r->kc[3] + 1;
    mrdp_rgba_t o;
    o.r = (uint8_t)clamp8(y + ((k0 * v + 128) >> 8));
    o.g = (uint8_t)clamp8(y + ((k1 * u + k2 * v + 128) >> 8));
    o.b = (uint8_t)clamp8(y + ((k3 * u + 128) >> 8));
    o.a = (uint8_t)y;
    return o;
}
static inline int tile_yuv(const mrdp_tile_t *t) { return t->fmt == 1 && t->size == 2; }
/* One pass of the texture unit: pass 0 the tile's sample (the first cycle's), pass 1 the next tile's (a 2-cycle pixel's
 * second, rtl/rdp rdp_pix.v's two_pass). A pass whose BI_LERP bit is off converts instead of filtering: the tile's
 * texel at (s0, t0), or with CONVERT_ONE (G_TC_FILTCONV) pass 1 converts pass 0's texel, prev. Only a YUV tile's texel
 * is converted (the bits are 0 -- G_TC_CONV -- in a mode a game never set; another format converted is not seen). */
static mrdp_rgba_t texture_pass(const mrdp_t *r, int tilei, int32_t s, int32_t t, int pass, mrdp_rgba_t prev)
{
    uint32_t h = r->other_h;
    int one = pass && (h & MRDP_H_CONVERT_ONE);
    int conv = ((h >> MRDP_H_CYCLE_SHIFT) & 3) < 2 && !(h & (pass ? MRDP_H_BILERP1 : MRDP_H_BILERP0))
               && tile_yuv(&r->tile[(one ? tilei - 1 : tilei) & 7]);
    if (!conv) return texture_sample(r, tilei, s, t, 0);
    return yuv_convert(r, one ? prev : texture_sample(r, tilei, s, t, 1));
}
#else
static mrdp_rgba_t texture_pass(const mrdp_t *r, int tilei, int32_t s, int32_t t, int pass, mrdp_rgba_t prev)
{
    (void)pass; (void)prev;
    return texture_sample(r, tilei, s, t, 0);
}
#endif

/* ---- color combiner: (A - B) * C + D, N64 selections -------------------- */

typedef struct {
    mrdp_rgba_t tex, shade, combined;
    mrdp_rgba_t tex1;              /* TEXEL1: (MRDP_N64: the next tile's sample in the first cycle of a 2-cycle pixel, else TEXEL0 again) */
#ifdef MRDP_CVG
    int cm9[4];                    /* COMBINED (the first cycle's result): r, g, b, a as the 9 raw bits (below) */
    int sh9[4];                    /* SHADE: r, g, b, a saturated to a byte, 0..255 (below; 9 bits wide as the RTL's) */
    int lodf;                      /* LOD_FRACTION: the pixel's, 9 bits (mrdp_lod; read as a signed 9-bit, as K5 is) */
#endif
} cc_in_t;
#define CC_TEX1(in) ((in)->tex1)
#ifdef MRDP_CVG
/* The N64's combiner arithmetic (the hardware's CombineColor / CombineAlpha stages,
 * Angrylion's): ((A - B) * C + (D << 8) + 0x80) >> 8, kept to 9 bits; "1" is
 * 256 (A, D), C is a plain 0..255 (an 8-bit 255 is 255/256 -- the hardware
 * cannot weigh by exactly 1). COMBINED (the first cycle's result, 9 raw bits)
 * is read as A or B as 0..383 if it is below 0x180 and as -128..-1 above, as C
 * as a signed 9-bit (combine_raw). The SHADE enters saturated to a byte -- the
 * interpolator takes it below 0 to 0 and above 255 to 255 -- and is a plain
 * positive number in A, B, C and D; the blender's shade alpha is clamped at 255
 * (blend_n64). (With 9 bits, 0..511, a lit surface's shade overshoots 255 at its
 * edges, TEXEL0 * SHADE reached 256 and over, and as COMBINED -- the second
 * cycle's C, ENV * COMBINED -- that is a negative number: black pixels on a
 * character's tunic lit by a moving light.)
 * The result's 9 bits: a colour clamps them (bits 8:7
 * both set -> 0, bit 8 -> 255), an alpha likewise but to 256 for bit 8 and
 * for exactly 255 (cc_alpha_res). */
#define CC_ONE 256
#define CC_CM(in, ch) ((in)->cm9[ch])
#define CC_SH(in, ch) ((in)->sh9[ch])
#else
#define CC_ONE 255
#define CC_CM(in, ch) ((int)(&(in)->combined.r)[ch])
#define CC_SH(in, ch) ((int)(&(in)->shade.r)[ch])
#endif

static int cc_rgb_a(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    const uint8_t *tex = &in->tex.r;
    const uint8_t *pr = &r->prim.r, *en = &r->env.r, *tx1 = &CC_TEX1(in).r;
    switch (sel) {
    case 0: return CC_CM(in, ch);
    case 1: return tex[ch];
    case 2: return tx1[ch];
    case 3: return pr[ch];
    case 4: return CC_SH(in, ch);
    case 5: return en[ch];
    case 6: return CC_ONE;
    default: return 0;           /* 7 noise, 8+ zero */
    }
}
static int cc_rgb_b(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
#ifdef MRDP_N64
    if (sel == 7) return r->k4;   /* (its 9 bits read as COMBINED's are: combine_raw) */
#endif
    if (sel >= 6) return 0;       /* 6 key center, 8+ zero (MRDP: 7, K4, too) */
    return cc_rgb_a(r, in, sel, ch);
}
static int cc_rgb_c(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    switch (sel) {
    case 0: case 1: case 2: case 3: case 4: case 5:
        return cc_rgb_a(r, in, sel, ch);
    case 7: return CC_CM(in, 3);
    case 8: return in->tex.a;
    case 9: return CC_TEX1(in).a;
    case 10: return r->prim.a;
    case 11: return CC_SH(in, 3);
    case 12: return r->env.a;
    case 14: return r->prim_lod_frac;
#ifdef MRDP_N64
    case 15: return r->k5;       /* (a signed 9-bit: combine_raw) */
#endif
#ifdef MRDP_CVG
    case 13: return in->lodf;    /* (a signed 9-bit: combine_raw) */
#endif
    default: return 0;           /* 6 key scale, 13 LOD fraction, 16+ (MRDP: 15, K5, too) */
    }
}
static int cc_rgb_d(const mrdp_t *r, const cc_in_t *in, unsigned sel, int ch)
{
    return sel == 7 ? 0 : cc_rgb_a(r, in, sel, ch);   /* 6 = 1 */
}
static int cc_alpha_abd(const mrdp_t *r, const cc_in_t *in, unsigned sel)
{
    switch (sel) {
    case 0: return CC_CM(in, 3);
    case 1: return in->tex.a;
    case 2: return CC_TEX1(in).a;
    case 3: return r->prim.a;
    case 4: return CC_SH(in, 3);
    case 5: return r->env.a;
    case 6: return CC_ONE;
    default: return 0;
    }
}
static int cc_alpha_c(const mrdp_t *r, const cc_in_t *in, unsigned sel)
{
    switch (sel) {
    case 1: return in->tex.a;
    case 2: return CC_TEX1(in).a;
    case 3: return r->prim.a;
    case 4: return CC_SH(in, 3);
    case 5: return r->env.a;
    case 6: return r->prim_lod_frac;
#ifdef MRDP_CVG
    case 0: return in->lodf;     /* (a signed 9-bit: combine_raw) */
#endif
    default: return 0;           /* 0 LOD fraction, 7 zero */
    }
}

#ifdef MRDP_CVG
static inline int cc_ext9(int v) { return (v & 0x180) == 0x180 ? v - 512 : v; }
static inline int cc_sgn9(int v) { return (v & 0x100) ? v - 512 : v; }
/* the raw 9 bits of ((a - b) * c + (d << 8) + 0x80) >> 8, the operands as they enter (COMBINED already extended
 * by combine_raw: cc_ext9 as A or B, cc_sgn9 as C; D's low 9 bits are all that count) */
static inline int cc_eval(int a, int b, int c, int d)
{
    return ((((a - b) * c) + d * 256 + 0x80) >> 8) & 0x1FF;
}
static inline int cc_color_res(int v9) { return (v9 & 0x180) == 0x180 ? 0 : (v9 & 0x100) ? 255 : v9 & 0xFF; }
/* an alpha: 0..256 (256 for an overflow and for exactly 255) */
static inline int cc_alpha_res(int v9) { return (v9 & 0x180) == 0x180 ? 0 : ((v9 & 0x100) || (v9 & 0xFF) == 0xFF) ? 256 : v9 & 0xFF; }
#else
/* (a - b) * c + d with c = 255 meaning 1.0 */
static inline int cc_eval(int a, int b, int c, int d)
{
    int cw = c + (c >> 7);
    return clamp8((((a - b) * cw) >> 8) + d);
}
#endif

/* does the first combiner cycle of a 2-cycle pixel read TEXEL1? (rtl/rdp rdp_pix.v two_pass is the same test on
 * the same fields: a pixel that does takes two clocks, the tile's sample and the next tile's) */
static int cc0_uses_texel1(const mrdp_t *r)
{
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    unsigned sa_r = (hi >> 20) & 15, mul_r = (hi >> 15) & 31, sa_a = (hi >> 12) & 7, mul_a = (hi >> 9) & 7;
    unsigned sb_r = (lo >> 28) & 15, add_r = (lo >> 15) & 7, sb_a = (lo >> 12) & 7, add_a = (lo >> 9) & 7;
    return sa_r == 2 || sb_r == 2 || mul_r == 2 || mul_r == 9 || add_r == 2 ||
           sa_a == 2 || sb_a == 2 || mul_a == 2 || add_a == 2;
}


/* does the second combiner cycle of a 2-cycle pixel read TEXEL0 (as a colour, or its alpha)? The N64's second cycle takes the
 * first cycle's TEXEL1 there -- the next tile's sample -- so such a pixel needs that sample too (rtl/rdp rdp_pix.v two_pass).
 * A golf course's ground: cycle 0 TEXEL0 (an I8 detail tile) * TEXEL1 (the RGBA16 grass), cycle 1 (COMBINED - TEXEL0) * A + TEXEL0 --
 * the grass itself, which a TEXEL0 of the first tile makes a pale grey-green. */
static int cc1_uses_texel0(const mrdp_t *r)
{
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    unsigned sa_r = (hi >> 5) & 15, mul_r = hi & 31, sa_a = (lo >> 21) & 7, mul_a = (lo >> 18) & 7;
    unsigned sb_r = (lo >> 24) & 15, add_r = (lo >> 6) & 7, sb_a = (lo >> 3) & 7, add_a = lo & 7;
    return sa_r == 1 || sb_r == 1 || mul_r == 1 || mul_r == 8 || add_r == 1 ||
           sa_a == 1 || sb_a == 1 || mul_a == 1 || add_a == 1;
}

#ifdef RDP_EXACT
/* does the second combiner cycle (a 1-cycle pixel's one) read TEXEL1? The console's TEXEL1 there is a texel of the next
 * pixel (docs/rdp_exact.md, rules 3 and 7), which such a pixel then samples too */
static int cc1_uses_texel1(const mrdp_t *r)
{
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    unsigned sa_r = (hi >> 5) & 15, mul_r = hi & 31, sa_a = (lo >> 21) & 7, mul_a = (lo >> 18) & 7;
    unsigned sb_r = (lo >> 24) & 15, add_r = (lo >> 6) & 7, sb_a = (lo >> 3) & 7, add_a = lo & 7;
    return sa_r == 2 || sb_r == 2 || mul_r == 2 || mul_r == 9 || add_r == 2 ||
           sa_a == 2 || sb_a == 2 || mul_a == 2 || add_a == 2;
}
#endif

/* one combiner cycle's four results: MRDP_CVG the raw 9 bits (cc_eval), else the clamped bytes */
typedef struct { int v[4]; } cc_out_t;
static cc_out_t combine_raw(const mrdp_t *r, const cc_in_t *in, int cyc)
{
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    unsigned sa_r, mul_r, sa_a, mul_a, sb_r, sb_a, add_r, add_a;
    if (cyc == 0) {
        sa_r = (hi >> 20) & 15; mul_r = (hi >> 15) & 31;
        sa_a = (hi >> 12) & 7;  mul_a = (hi >> 9) & 7;
        sb_r = (lo >> 28) & 15; add_r = (lo >> 15) & 7;
        sb_a = (lo >> 12) & 7;  add_a = (lo >> 9) & 7;
    } else {
        sa_r = (hi >> 5) & 15;  mul_r = hi & 31;
        sa_a = (lo >> 21) & 7;  mul_a = (lo >> 18) & 7;
        sb_r = (lo >> 24) & 15; add_r = (lo >> 6) & 7;
        sb_a = (lo >> 3) & 7;   add_a = lo & 7;
    }
    cc_out_t o;
#ifdef MRDP_CVG
    /* COMBINED (selection 0) and SET CONVERT's two constants are the operands that can be above 383 and mean a negative
     * number: A and B read COMBINED, and B K4, as -128..-1 from 0x180, C (selection 0, or 7: COMBINED's alpha; 15: K5)
     * as a signed 9-bit; the SHADE and the rest are the plain positive numbers they are. (K5 at 255 is the nearest the
     * multiplier has to 1: a game's (TEXEL0 - TEXEL1) * K5 + TEXEL1 is TEXEL0, and TEXEL1 with K5 at 0.) */
    for (int ch = 0; ch < 3; ch++) {
        int a = cc_rgb_a(r, in, sa_r, ch), b = cc_rgb_b(r, in, sb_r, ch), c = cc_rgb_c(r, in, mul_r, ch);
        if (sa_r == 0) a = cc_ext9(a);
        if (sb_r == 0 || sb_r == 7) b = cc_ext9(b);                       /* (7: K4) */
        if (mul_r == 0 || mul_r == 7 || mul_r == 15) c = cc_sgn9(c);      /* (15: K5) */
        if (mul_r == 13) c = cc_sgn9(c);                                  /* the LOD fraction: sharpen's is negative */
        o.v[ch] = cc_eval(a, b, c, cc_rgb_d(r, in, add_r, ch));
    }
    {
        int a = cc_alpha_abd(r, in, sa_a), b = cc_alpha_abd(r, in, sb_a);
        if (sa_a == 0) a = cc_ext9(a);
        if (sb_a == 0) b = cc_ext9(b);
        int c = cc_alpha_c(r, in, mul_a);
        if (mul_a == 0) c = cc_sgn9(c);                                   /* the LOD fraction, as in the colour */
        o.v[3] = cc_eval(a, b, c, cc_alpha_abd(r, in, add_a));
    }
#else
    for (int ch = 0; ch < 3; ch++)
        o.v[ch] = cc_eval(cc_rgb_a(r, in, sa_r, ch), cc_rgb_b(r, in, sb_r, ch),
                          cc_rgb_c(r, in, mul_r, ch), cc_rgb_d(r, in, add_r, ch));
    o.v[3] = cc_eval(cc_alpha_abd(r, in, sa_a), cc_alpha_abd(r, in, sb_a),
                     cc_alpha_c(r, in, mul_a), cc_alpha_abd(r, in, add_a));
#endif
    return o;
}
static mrdp_rgba_t combine(const mrdp_t *r, const cc_in_t *in, int cyc)
{
    cc_out_t o = combine_raw(r, in, cyc);
#ifdef MRDP_CVG
    mrdp_rgba_t c = { (uint8_t)cc_color_res(o.v[0]), (uint8_t)cc_color_res(o.v[1]), (uint8_t)cc_color_res(o.v[2]),
                      (uint8_t)clamp8(cc_alpha_res(o.v[3])) };
#else
    mrdp_rgba_t c = { (uint8_t)o.v[0], (uint8_t)o.v[1], (uint8_t)o.v[2], (uint8_t)o.v[3] };
#endif
    return c;
}

/* ---- blender: (P * a + M * b) >> 8 ------------------------------------- */

static mrdp_rgba_t bl_pm(const mrdp_t *r, unsigned sel, mrdp_rgba_t pix, mrdp_rgba_t mem)
{
    switch (sel) {
    case 0: return pix;
    case 1: return mem;
    case 2: return r->blend;
    default: return r->fog;
    }
}

static mrdp_rgba_t blend(const mrdp_t *r, int cyc, mrdp_rgba_t pix, mrdp_rgba_t mem,
                         int comb_a, int shade_a)
{
    uint32_t l = r->other_l;
    unsigned p = (l >> (cyc ? 28 : 30)) & 3, a = (l >> (cyc ? 24 : 26)) & 3;
    unsigned m = (l >> (cyc ? 20 : 22)) & 3, b = (l >> (cyc ? 16 : 18)) & 3;
    int av;
    switch (a) {
    case 0: av = comb_a; break;
    case 1: av = r->fog.a; break;
    case 2: av = shade_a; break;
    default: av = 0; break;
    }
    int aw = av + (av >> 7), bw;
    switch (b) {
    case 0: bw = 256 - aw; break;
    case 1: case 2: bw = 256; break;     /* memory alpha is 1: RGB565 has none */
    default: bw = 0; break;
    }
    mrdp_rgba_t P = bl_pm(r, p, pix, mem), M = bl_pm(r, m, pix, mem), o;
    o.r = (uint8_t)clamp8((P.r * aw + M.r * bw) >> 8);
    o.g = (uint8_t)clamp8((P.g * aw + M.g * bw) >> 8);
    o.b = (uint8_t)clamp8((P.b * aw + M.b * bw) >> 8);
    o.a = pix.a;
    return o;
}

static inline int bl_reads_memory(uint32_t l, int cyc)
{
    return ((l >> (cyc ? 28 : 30)) & 3) == 1 || ((l >> (cyc ? 20 : 22)) & 3) == 1;
}

/* ---- pixels -------------------------------------------------------------- */

#ifdef MRDP_N64
/* the colour image is the game's: RGBA5551, its low bit (the N64's coverage) set */
static inline uint16_t rgb565(mrdp_rgba_t c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | 1u);
}
/* The N64's RGB dither (RGB_DITHER_SEL, other_h[7:6]), as rtl/rdp does it: the
 * 8 bits go to 5 by truncation after a channel whose low three bits are above
 * the threshold is taken up to the next step (not past 255). The threshold is
 * the matrix's at the pixel's place on the screen: 0 magic square, 1 Bayer;
 * 2 noise: three random bits for each channel (random9[2:0], [5:3], [8:6] of the LFSR -- the sample the pixel's alpha dither
 * takes too); 3 off. */
/* the matrix's value at the pixel's place: the magic square (RGB_DITHER_SEL 0,
 * or 3: off, for the alpha dither's pattern) or the Bayer matrix (1; and 2
 * where there is no noise: the legacy path without the N64's pixel) */
static inline unsigned dither_d(const mrdp_t *r, int x, int y, int noise)
{
    static const uint8_t magic[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };
    static const uint8_t bayer[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };
    unsigned sel = (r->other_h >> 6) & 3;
    unsigned i = ((unsigned)(y & 3) << 2) | (unsigned)(x & 3);
    return (sel == 1 || (sel == 2 && !noise)) ? bayer[i] : magic[i];
}
/* rnd: the LFSR's value this pixel's alpha dither took (before its step) */
static inline mrdp_rgba_t dither_n64(const mrdp_t *r, mrdp_rgba_t c, int x, int y, uint32_t rnd, int noise)
{
    unsigned sel = (r->other_h >> 6) & 3;
    if (sel == 3) return c;
    unsigned d = dither_d(r, x, y, noise);
    uint8_t *ch[3] = { &c.r, &c.g, &c.b };
    for (int k = 0; k < 3; k++) {
        unsigned v = *ch[k], lo = v & 7;
        unsigned dk = (sel == 2 && noise) ? (rnd >> (3 * k)) & 7u : d;
        if (dk < lo && v < 248) *ch[k] = (uint8_t)((v | 7) + 1);
        else if (dk < lo) *ch[k] = 255;
    }
    return c;
}
/* the alpha dither (ALPHA_DITHER_SEL, other_h[5:4]): the pattern, its
 * inverse, noise (three random bits: the LFSR's [10:8]), none -- the pattern
 * the alpha's matrix: Bayer for RGB_DITHER_SEL 1 and 3, else the magic square
 * (Angrylion's get_dither_noise) */
static inline unsigned dither_a(const mrdp_t *r, int x, int y, uint32_t rnd)
{
    static const uint8_t magic[16] = { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 };
    static const uint8_t bayer[16] = { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 };
    unsigned sel = (r->other_h >> 4) & 3, i = ((unsigned)(y & 3) << 2) | (unsigned)(x & 3);
    unsigned d = ((r->other_h >> 6) & 1u) ? bayer[i] : magic[i];
    return sel == 0 ? d : sel == 1 ? (~d & 7u) : sel == 2 ? (rnd >> 8) & 7u : 0;
}
/* the colour image's pixel as the blender reads it: the five bits with three
 * zeros below (Angrylion's fbread GET_HI/MED/LOW) --
 * not the top bits again: with them a translucent surface's blend of nearly
 * the memory's own colour came out with low bits the RGB dither rounds up,
 * one step more at each pass, at the matrix's low thresholds: a regular grid
 * of bright dots inside every translucent rectangle drawn over and over */
static inline mrdp_rgba_t unrgb565(uint16_t v)
{
    unsigned r5 = v >> 11, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
    mrdp_rgba_t c = { (uint8_t)(r5 << 3), (uint8_t)(g5 << 3), (uint8_t)(b5 << 3), 255 };
    return c;
}
#else
static inline uint16_t rgb565(mrdp_rgba_t c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}
static inline mrdp_rgba_t unrgb565(uint16_t v)
{
    unsigned r5 = v >> 11, g6 = (v >> 5) & 63, b5 = v & 31;
    mrdp_rgba_t c = { (uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g6 << 2 | g6 >> 4),
                      (uint8_t)(b5 << 3 | b5 >> 2), 255 };
    return c;
}
#endif

typedef struct {
    int shade, tex, zbuf;          /* which attributes the primitive has */
    int tile;
    int texrect;                   /* s,t already in s10.5 */
    unsigned dz;                   /* MRDP_N64: the primitive's depth step (a decal's tolerance; the coverage's dz) */
    int32_t dzdy;                  /* MRDP_CVG: the triangle's DzDy (the Z offset to the first covered sample) */
    int fillq;                     /* a fill rectangle (fill mode): the hidden bits by 4-pixel quads */
#ifdef MRDP_CVG
    unsigned level;                /* the triangle command's level field: the primitive's last level (a texture rectangle: 0) */
    int32_t ddx[3], ddy[3];        /* S, T, W: to the next pixel of the N64's walk (its DaDx, negated for a right-major
                                      triangle, which the N64 walks right to left) and to the next line (DaDy, as kept) */
    int32_t rnx[2], rny[2];        /* a texture rectangle's S, T (s10.5) at the next pixel and on the next line: set per pixel */
#endif
#ifdef RDP_EXACT
    /* the console's walk (docs/rdp_exact.md), set per pixel: where the pixel is in its span and what follows the span,
     * which the 1-cycle LOD and the texels taken a pixel ahead depend on */
    int xj, xlen, xlodlen;         /* the pixel's place from the walk's start (the major edge's end of the span), the
                                      span's pixels after the first, and that plus the pixels the scissor took off the
                                      span at the major edge (the console counts those for its span-end cases) */
    int xnvalid;                   /* the next row has a span */
    int32_t xinc[3];               /* S, T, W a pixel along the walk */
    int32_t xnrow[3];              /* S, T, W where the next row's span starts: at its major edge, before the scissor */
    int32_t xcd[5], xdy[5];        /* shade R, G, B, A and Z: a pixel's and a line's step as the console moves them to the
                                      first covered sample (s13 in 1/16ths of the shade's unit; Z s22 in 1/1024ths) */
    unsigned xdzpix;               /* the console's depth step (|DzDx| + |DzDy| of the integer halves, as a power of 2) */
    int xcopy;                     /* copy mode: the pixel's place in its group (its texel is the group's first plus it) */
#endif
} prim_t;

/* attribute order in the iterators */
enum { AR, AG, AB, AA, AS, AT, AW, AZ, NATTR };

/* A 32-bit colour image's RGB is dithered as a 16-bit one's (dither_n64: a channel either kept or taken up to the next
 * multiple of 8) and written whole: the N64 programming manual's blender section says the RGB dither "is enabled even
 * in 32 bit mode", which is why it tells games to turn it off for opaque surfaces there. 0: not dithered (the model
 * only: rtl/rdp dithers). */
#define CIMG32_DITHER 1

#ifdef MRDP_CVG
/* ---- texture level of detail (MRDP_CVG; docs/lod.md) ---------------------
 * The N64's mipmapping, in a 2-cycle primitive. Per pixel, S and T are taken
 * through the perspective divide at three points: the pixel, the next pixel of
 * the N64's walk (DaDx; a right-major triangle is walked right to left, so its
 * next pixel is x - 1) and the next line (DaDy). The LOD is the largest of the
 * four coordinate steps, |dS| and |dT| in each direction, in texels as u10.5
 * (32 = one texel a pixel). Its top bit gives the level (level k: a step of
 * 2^k to 2^(k+1) texels), clamped to the primitive's level count; the 8 bits
 * under the top bit are the LOD fraction, the combiner's blend between the
 * level and the next. A step under one texel is a magnification: level 0, and a
 * fraction of 0 (or, with sharpen or detail, the step itself, no less than the
 * minimum level). TEXEL0 samples the primitive's tile plus the level, TEXEL1
 * the tile after it; detail mode moves both a tile on (the primitive's tile is
 * the detail texture, magnified pixels blend it in).
 *
 * The choices the public descriptions leave open, each made in one place here
 * (docs/lod.md, "Choices"), so they can be changed when the RTL is compared
 * with captured frames: */
/* DaDy of S, T and W as the LOD steps them: the top 17 bits. The N64 keeps no
 * more of them (it uses them for nothing else), and the RTL needs no more. */
#define LOD_DY_MASK   (~0x7FFF)
/* A negative coordinate step's magnitude is its ones' complement, |d| - 1:
 * the N64's inverter has no increment after it. 0 takes |d|. */
#define LOD_ONES_ABS  1
/* The fraction is computed whenever the combiner reads LOD_FRACTION, with or
 * without TEX_LOD, as the N64 does (TEX_LOD only moves the tiles). 0: only
 * with TEX_LOD. */
#define LOD_FRAC_USED 1

/* the larger of a point's two coordinate steps from the pixel, and prev: the
 * LOD so far (15 bits; bit 14 is sticky for any step of 512 texels or more) */
static unsigned lod_dist(int32_t s0, int32_t s1, int32_t t0, int32_t t1, unsigned prev)
{
    int32_t ds = s1 - s0, dt = t1 - t0;              /* 18 bits signed */
    unsigned ms = ds < 0 ? (LOD_ONES_ABS ? (unsigned)~ds : (unsigned)-ds) : (unsigned)ds;
    unsigned mt = dt < 0 ? (LOD_ONES_ABS ? (unsigned)~dt : (unsigned)-dt) : (unsigned)dt;
    unsigned m = ms > mt ? ms : mt;
    if (prev > m) m = prev;
    return (m & 0x7FFFu) | ((m & 0x1C000u) ? 0x4000u : 0u);
}

/* the place of the highest set bit of 8 bits (0 for 0 and 1): the level */
static inline unsigned lod_log2(unsigned v)
{
    unsigned e = 0;
    while (v > 1) { v >>= 1; e++; }
    return e;
}

mrdp_lod_t mrdp_lod(uint32_t h, unsigned min_level, unsigned max_level, unsigned prim_tile,
                    const int32_t s[3], const int32_t t[3], int oob)
{
    const int detail = (h & RDP_H_DETAIL) != 0, sharpen = (h & RDP_H_SHARPEN) != 0;
    unsigned lod = 0;
    if (!oob) lod = lod_dist(s[0], s[2], t[0], t[2], lod_dist(s[0], s[1], t[0], t[1], 0));
    unsigned lf, ltil;
    int mag, dis;
    if ((lod & 0x4000u) || oob) {
        /* 512 texels a pixel or more, or a coordinate out of range: the far level, the fraction all ones */
        mag = 0; ltil = 7; dis = 1; lf = 0xFF;
    } else if (lod < 32) {
        /* magnified: level 0. Plain, the fraction is 0 (all ones when the primitive has no level after it); sharpen
         * and detail take the step itself, no less than the minimum level, and sharpen makes it negative (0x100 up,
         * the combiner's signed 9 bits): the next level is weighed against, a sharper texel */
        mag = 1; ltil = 0; dis = max_level == 0;
        if (!sharpen && !detail) lf = dis ? 0xFF : 0;
        else {
            lf = (lod < min_level ? min_level : lod) << 3;
            if (sharpen) lf |= 0x100;
        }
    } else {
        /* the level is the top bit of the step in texels (lod >> 5; its 9th bit is lost here, as on the N64, but a step
         * that has it is at the far level anyway), the fraction the 8 bits under it */
        mag = 0;
        ltil = lod_log2((lod >> 5) & 0xFF);
        dis = max_level ? ((lod & 0x6000u) || ltil >= max_level) : 1;
        lf = (!sharpen && !detail && dis) ? 0xFF : ((lod << 3) >> ltil) & 0xFF;
    }
    mrdp_lod_t o;
    o.level = (uint8_t)(dis ? max_level : ltil);
    o.magnify = (uint8_t)mag; o.distant = (uint8_t)dis;
    o.frac = (uint16_t)lf; o.lod = (uint16_t)lod;
    o.tile0 = (uint8_t)(prim_tile & 7); o.tile1 = (uint8_t)((prim_tile + 1) & 7);
    if (h & RDP_H_TEX_LOD) {
        const unsigned lv = o.level;
        if (!detail) {
            /* the level's tile and the next; at the far level, or magnified without sharpen, both the level's */
            o.tile0 = (uint8_t)((prim_tile + lv) & 7);
            o.tile1 = (dis || (!sharpen && mag)) ? o.tile0 : (uint8_t)((o.tile0 + 1) & 7);
        } else {
            /* the primitive's tile is the detail texture: magnified, TEXEL0 is it and TEXEL1 level 0 (the tile after);
             * otherwise the levels are a tile on */
            o.tile0 = (uint8_t)((prim_tile + lv + (mag ? 0u : 1u)) & 7);
            o.tile1 = (uint8_t)((prim_tile + lv + ((!dis && !mag) ? 2u : 1u)) & 7);
        }
    }
    return o;
}

/* is the LOD computed for this 2-cycle primitive's pixels: TEX_LOD, or the combiner reads LOD_FRACTION (C 13 in a
 * colour, C 0 in an alpha, either cycle) */
static int lod_wanted(const mrdp_t *r)
{
    if (r->other_h & RDP_H_TEX_LOD) return 1;
    if (!LOD_FRAC_USED) return 0;
    uint32_t hi = r->combine_hi, lo = r->combine_lo;
    return ((hi >> 15) & 31) == 13 || (hi & 31) == 13 || ((hi >> 9) & 7) == 0 || ((lo >> 18) & 7) == 0;
}

/* The pixel's LOD: S, T at the pixel (at), at the next pixel of the walk and on the next line, each taken as the pixel's
 * own S, T are (the perspective divide, affine, or a rectangle's s10.5 as they are), then mrdp_lod. */
static mrdp_lod_t lod_pixel(const mrdp_t *r, const prim_t *pr, const int32_t *at)
{
    int32_t s[3], t[3];
    int oob = 0;
    if (pr->texrect) {
        /* (s10.5 in 16 bits, as the pixel's: no perspective divide, never out of range) */
        s[0] = (int16_t)at[AS]; t[0] = (int16_t)at[AT];
        s[1] = (int16_t)pr->rnx[0]; t[1] = (int16_t)pr->rnx[1];
        s[2] = (int16_t)pr->rny[0]; t[2] = (int16_t)pr->rny[1];
    } else {
        /* (a triangle without texture coefficients has S = T = W = 0 at all three: with perspective, out of range) */
        const int persp = (r->other_h & MRDP_H_PERSP) != 0;
        for (int k = 0; k < 3; k++) {
            const int32_t *d = k == 1 ? pr->ddx : pr->ddy;
            uint32_t S = pr->tex ? (uint32_t)at[AS] : 0u, T = pr->tex ? (uint32_t)at[AT] : 0u, Wv = pr->tex ? (uint32_t)at[AW] : 0u;
            if (k && pr->tex) { S += (uint32_t)d[0]; T += (uint32_t)d[1]; Wv += (uint32_t)d[2]; }
            /* (the console's perspective divide, as the pixel's own S, T take it: its 17 bits and its range flag) */
            if (persp) { int rs, rt; s[k] = mrdp_n64_div((int32_t)S, (int32_t)Wv, &rs); t[k] = mrdp_n64_div((int32_t)T, (int32_t)Wv, &rt); oob |= rs || rt; }
            else { s[k] = (int32_t)S >> 16; t[k] = (int32_t)T >> 16; }   /* (s105_noperp's: s10.5 in 16 bits) */
        }
    }
    return mrdp_lod(r->other_h, r->min_level, pr->level, (unsigned)pr->tile, s, t, oob);
}

#ifdef RDP_EXACT
/* ---- the LOD in 1-cycle mode (RDP_EXACT; docs/rdp_exact.md, rule 3) ----
 * The console computes an LOD in 1-cycle mode too, but from two points ahead of the pixel: the next pixel of the walk
 * (N) and the one after it (F), no next line. Near the end of a span it looks at the next row's span instead, by how
 * long the span is (lodlen: its pixels after the first, the scissored ones at its start included):
 *   next row with a span, pixel q of 0..len:  N = P + d, F = P + 2d; F = P - d at the last but one pixel of a span
 *       longer than 7 or the last of one of 7; at the last pixel of one longer than 7, N = the next row's start, F = N + d
 *   q = len + 1 (the pixel after the span, whose texel is the last one's TEXEL1): longer than 7: N = next row's start
 *       + d, F = + 2d; 7: N = the start, F = + d; 6: N = P + d, F = P - d; else N = P + d, F = P + 2d
 *   no next row: N = P + d, F = P + 2d
 * The LOD is the larger step of S and T from N to F (ones' complement, sticky bit as in 2-cycle mode); level, far,
 * magnified, fraction and TEXEL0's tile are mrdp_lod's. */
static int lod1_wanted(const mrdp_t *r)
{
    if (r->other_h & RDP_H_TEX_LOD) return 1;
    if (!LOD_FRAC_USED) return 0;
    return (r->combine_hi & 31) == 13 || ((r->combine_lo >> 18) & 7) == 0;   /* (the one cycle's C: the second's selects) */
}
static void lod1_pts(const prim_t *pr, const int32_t *P, int q, int32_t *N, int32_t *F)
{
    const int len = pr->xlen, ll = pr->xlodlen;
    const int32_t *d = pr->xinc, *n = pr->xnrow;
    if (!pr->xnvalid) { x_pt(N, P, 1, d); x_pt(F, P, 2, d); }
    else if (q <= len) {
        if (q != len || ll <= 7) { x_pt(N, P, 1, d); x_pt(F, P, ((q == len - 1 && ll > 7) || (q == len && ll == 7)) ? -1 : 2, d); }
        else { x_pt(N, n, 0, d); x_pt(F, n, 1, d); }
    }
    else if (ll > 7) { x_pt(N, n, 1, d); x_pt(F, n, 2, d); }
    else if (ll == 7) { x_pt(N, n, 0, d); x_pt(F, n, 1, d); }
    else { x_pt(N, P, 1, d); x_pt(F, P, ll == 6 ? -1 : 2, d); }
}
/* a point's S, T through the perspective divide the pixel's take, as the LOD keeps them (17 bits, the range flag) */
static void x_div(const mrdp_t *r, const int32_t *p, int32_t *s, int32_t *t, int *oob)
{
    if (r->other_h & MRDP_H_PERSP) { int rs, rt; *s = mrdp_n64_div(p[0], p[2], &rs); *t = mrdp_n64_div(p[1], p[2], &rt); *oob |= rs || rt; }
    else { *s = p[0] >> 16; *t = p[1] >> 16; }
}
/* the 1-cycle LOD of the span's pixel q, from the pixel's own S, T, W (at: pixel pr->xj's) */
static mrdp_lod_t lod1_pixel(const mrdp_t *r, const prim_t *pr, const int32_t *at, int q)
{
    int32_t P[3], N[3], F[3], s[3], t[3];
    int oob = 0;
    x_pt(P, &at[AS], q - pr->xj, pr->xinc);
    lod1_pts(pr, P, q, N, F);
    x_div(r, N, &s[0], &t[0], &oob);
    x_div(r, F, &s[1], &t[1], &oob);
    s[2] = s[0]; t[2] = t[0];                         /* (no next line: a step of 0 there) */
    const mrdp_lod_t L = mrdp_lod(r->other_h, r->min_level, pr->level, (unsigned)pr->tile, s, t, oob);
    return L;
}
#endif
#endif /* MRDP_CVG (the LOD) */

/* An 8-bit colour image (SET_COLOR_IMAGE's size 1): a pixel is a byte, at image + y * width + x -- what software that
 * draws into a colour-indexed texture uses. Its value is the blender's red, as it is (not dithered); a fill writes the
 * fill colour's byte for the address (bits 31:24 at a multiple of 4). Only written: the pipeline does not read it (the
 * memory colour is 0, its coverage complete), and there is no depth and no hidden coverage with it
 * (rtl/rdp/rdp_top.v cimg8). Its rows must start on a 4-byte boundary. */
static inline void wr8(mrdp_t *r, uint32_t a, unsigned v)
{
    uint16_t w = r->rd16(r->ctx, a & ~1u);
    r->wr16(r->ctx, a & ~1u, (a & 1u) ? (uint16_t)((w & 0xFF00u) | (v & 0xFFu)) : (uint16_t)((w & 0x00FFu) | (v & 0xFFu) << 8));
}

#ifdef MRDP_CVG
/* ---- the N64's pixel (MRDP_CVG, mrdp.h) ----------------------------------- */

/* the blender's reciprocals, ~2^17 / k (the blender's multiplier table; the N64's own
 * divider is lossier), k = A5 / 4 + B5 / 4 + 1 */
static const uint32_t s_bl_recip[16] = { 0x3ffff, 0x20001, 0x10001, 0x0aaab, 0x08001, 0x06667, 0x05556, 0x04925,
                                         0x04001, 0x038e4, 0x03334, 0x02e8c, 0x02aab, 0x02763, 0x02493, 0x02223 };

/* the highest set bit's place (0 for 0 and 1): the N64's dz encoding */
static inline unsigned hibit(unsigned v) { unsigned e = 0; while (v > 1) { v >>= 1; e++; } return e; }
#ifdef RDP_EXACT
/* the console's dz encoding: the OR of the places of the set bits (the place of the one set bit of a power of 2) */
static inline unsigned x_dzcomp(unsigned v)
{
    return ((v & 0xFF00u) ? 8u : 0u) | ((v & 0xF0F0u) ? 4u : 0u) | ((v & 0xCCCCu) ? 2u : 0u) | ((v & 0xAAAAu) ? 1u : 0u);
}
/* the console's depth step: |DzDx| + |DzDy| of the integer halves (a negative one's complement, 15 bits), as the power
 * of 2 above its top bit (1 for 0, 3 for 1, 0x8000 from 0x4000) */
static unsigned x_dzpix(int32_t dzdx, int32_t dzdy)
{
    const unsigned a = ((uint32_t)dzdx >> 16) & 0xFFFFu, b = ((uint32_t)dzdy >> 16) & 0xFFFFu;
    const unsigned sum = (((a & 0x8000u) ? ~a & 0x7FFFu : a) + ((b & 0x8000u) ? ~b & 0x7FFFu : b)) & 0xFFFFu;
    if (sum & 0xC000u) return 0x8000u;
    if (!sum) return 1u;
    if (sum == 1u) return 3u;
    for (unsigned c = 0x2000u; c; c >>= 1) if (sum & c) return c << 1;
    return 0u;
}
#endif

/* one blender cycle. P * A5 + M * (B5 + 1) a channel: A5 is the A alpha's
 * top five bits (B = memory alpha, the coverage: {a[7:5] >> sh_a, 00} and
 * {cvg >> sh_b, 11}), B5 the B's. blend_en off (or P's own alpha with 1 - a
 * at 255): P. CLR_ON_CVG without overflow: M. Then >> 5 (the hardware wraps
 * at 8 bits), or -- a blend without FORCE_BL on the last cycle -- divided by
 * A5 / 4 + B5 / 4 + 1 through the reciprocal table on bits [13:2] of the sum,
 * the product's [24:17]. */
static mrdp_rgba_t blend_n64(const mrdp_t *r, int cyc, mrdp_rgba_t pix, mrdp_rgba_t mem,
                             unsigned pix_a, unsigned shade_a, unsigned memcvg, unsigned sh_a, unsigned sh_b,
                             int blend_en, int overflow, int divide, int first)
{
    uint32_t l = r->other_l;
    unsigned p = (l >> (cyc ? 28 : 30)) & 3, a = (l >> (cyc ? 24 : 26)) & 3;
    unsigned m = (l >> (cyc ? 20 : 22)) & 3, b = (l >> (cyc ? 16 : 18)) & 3;
    unsigned A = a == 0 ? pix_a : a == 1 ? r->fog.a : a == 2 ? shade_a : 0;
    unsigned B = b == 0 ? 255u - A : b == 1 ? (memcvg << 5) : b == 2 ? 255u : 0;
    unsigned A5 = (b == 1) ? (((A >> 5) >> sh_a) << 2) : (A >> 3);
    unsigned B5 = (b == 1) ? ((((B >> 5) >> sh_b) << 2) | 3u) : (B >> 3);
    unsigned dv = (A5 >> 2) + (B5 >> 2) + 1;
    int blend = blend_en && (first || !(a == 0 && b == 0 && A == 255));   /* (2-cycle's first cycle: always) */
    mrdp_rgba_t P = bl_pm(r, p, pix, mem), M = bl_pm(r, m, pix, mem), o;
    const uint8_t *pp = &P.r, *pm = &M.r;
    uint8_t *po = &o.r;
    for (int ch = 0; ch < 3; ch++) {
        unsigned sum = (unsigned)pp[ch] * A5 + (unsigned)pm[ch] * (B5 + 1);          /* < 2^14 */
        if ((l & MRDP_L_CLR_ON_CVG) && !overflow) po[ch] = pm[ch];                   /* (Angrylion: M itself) */
        else if (!blend) po[ch] = pp[ch];
        else if (divide) po[ch] = (uint8_t)((((sum >> 2) & 0xFFFu) * s_bl_recip[dv]) >> 17);
        else po[ch] = (uint8_t)(sum >> 5);
    }
    o.a = pix.a;
    return o;
}

/* The hidden plane (mrdp.h, MRDP_HID_BASE): the N64's other two coverage bits
 * of each 16-bit word of RDRAM, 16 of them a 32-bit word (bits [2k+1:2k] for
 * the word at byte 2k of a 32-byte group), through the 16-bit accessors */
static inline uint32_t hid_addr(uint32_t a) { return MRDP_HID_BASE + (((a & 0x7FFFFFu) >> 5) << 2); }
static inline unsigned hid_get(mrdp_t *r, uint32_t a)
{
    unsigned k = (a >> 1) & 15u;
    uint16_t h = r->rd16(r->ctx, hid_addr(a) + (k < 8u ? 2u : 0u));
    return (h >> (2u * (k & 7u))) & 3u;
}
static inline void hid_set(mrdp_t *r, uint32_t a, unsigned v)
{
    unsigned k = (a >> 1) & 15u;
    uint32_t ha = hid_addr(a) + (k < 8u ? 2u : 0u);
    uint16_t h = r->rd16(r->ctx, ha);
    h = (uint16_t)((h & ~(3u << (2u * (k & 7u)))) | ((v & 3u) << (2u * (k & 7u))));
    r->wr16(r->ctx, ha, h);
}

/* The colour image's word for the pixel: RGBA5551, bit 0 the coverage's top
 * bit */
static inline uint16_t rgb5551(mrdp_rgba_t c, int complete)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 3) << 6) | ((c.b >> 3) << 1) | (complete ? 1u : 0u));
}

/* ---- the depth image's word: the N64's --------------------------------------
 * A depth is 18 bits (Z's s15.16 >> 13, kept inside 0..0x3FFFF). The word holds 14 of them as a floating-point number --
 * an exponent of 3 bits, the count of the depth's leading ones, and the 11 bits under them (under the first 0; the
 * exponents 6 and 7 the last 11) -- so the far end, where a perspective's depths crowd, keeps every bit and the near end
 * loses six:
 *   exponent   0        1        2        3        4        5        6        7
 *   from       0x00000  0x20000  0x30000  0x38000  0x3C000  0x3E000  0x3F000  0x3F800
 *   lost bits  6        5        4        3        2        1        0        0
 * and, in its two low bits, the top two of the pixel's dz code (its other two are the N64's hidden bits: not kept, and
 * the code is not read back). 0xFFFC, what a game clears its depth image to, is the far plane. A game reads the image as
 * the console's: with the CPU, or as a texture. */
static const uint32_t s_z_base[8] = { 0x00000, 0x20000, 0x30000, 0x38000, 0x3C000, 0x3E000, 0x3F000, 0x3F800 };
static const unsigned s_z_shift[8] = { 6, 5, 4, 3, 2, 1, 0, 0 };
static inline uint16_t z_enc(uint32_t z)
{
    unsigned e = 7;
    while (e > 0 && z < s_z_base[e]) e--;
    return (uint16_t)((e << 11 | ((z >> s_z_shift[e]) & 0x7FFu)) << 2);
}
static inline uint32_t z_dec(uint16_t w)
{
    const unsigned e = w >> 13;
    return s_z_base[e] | (((uint32_t)(w >> 2) & 0x7FFu) << s_z_shift[e]);
}

/* does the pixel pipeline read the colour image (rtl/rdp/rdp_top.v need_mem):
 * for the memory coverage (IM_RD, with AA_EN or CVG_X_ALPHA), for CLR_ON_CVG
 * with CVG_X_ALPHA and without AA_EN (a pixel with no scaled coverage is
 * written, as the blender's M), or for a blender cycle that selects it */
static int cvg_need_mem(uint32_t l, unsigned cyc)
{
    if (cyc == 2) return 0;
    if ((l & MRDP_L_IM_RD) && (l & (MRDP_L_AA_EN | MRDP_L_CVG_X_ALPHA))) return 1;
    if ((l & MRDP_L_CLR_ON_CVG) && (l & MRDP_L_CVG_X_ALPHA) && !(l & MRDP_L_AA_EN)) return 1;
    int need = cyc == 1 ? (bl_reads_memory(l, 0) || bl_reads_memory(l, 1)) : bl_reads_memory(l, 0);
    if (!(l & MRDP_L_FORCE_BL)) {
        unsigned p = (l >> (cyc == 1 ? 28 : 30)) & 3;
        need = p == 1 || (cyc == 1 && p == 0 && bl_reads_memory(l, 0));
    }
    return need;
}

/* the N64's pixel: mask is its 8 samples (bit 7 the top-left; 0xFF for a
 * rectangle or a triangle without AA_EN) */
static void draw_pixel_cvg(mrdp_t *r, const prim_t *pr, int x, int y, const int32_t *at, unsigned mask)
{
    uint32_t h = r->other_h, l = r->other_l;
    unsigned cyc = (h >> MRDP_H_CYCLE_SHIFT) & 3;
    const int c8 = r->cimg_size == 1;
    uint32_t pix_off = ((uint32_t)y * r->cimg_width + (uint32_t)x) * 2;
    uint32_t caddr = c8 ? r->cimg + (uint32_t)y * r->cimg_width + (uint32_t)x : r->cimg + pix_off, zaddr = r->zimg + pix_off;
    /* a 32-bit image: 4 bytes a pixel (R, G, B, then the coverage in the alpha byte's top bits); the depth image is
     * 16-bit whatever the colour image is, so zaddr keeps the 2-byte offset */
    const int c32 = r->cimg_size == 3;
    if (c32) caddr = r->cimg + 2u * pix_off;
    const int aa = (l & MRDP_L_AA_EN) != 0;
    unsigned cvg = (unsigned)__builtin_popcount(mask & 0xFFu);                  /* 0..8 */
    /* the noise (the RTL's LFSR: a step for each pixel with coverage) */
    const uint32_t rnd = r->lfsr;
    if (cvg) r->lfsr = ((rnd << 1) | (~((rnd >> 22) ^ (rnd >> 18)) & 1u)) & 0x7FFFFFu;

    /* depth: 18 bits (z_enc) */
    uint32_t depth;
#ifdef RDP_EXACT
    /* the console's (docs/rdp_exact.md, rule 4): a pixel without full coverage moves its Z and shade to its first covered
     * sample (offx quarters right, offy subscanlines down: the topmost covered subscanline, its leftmost sample), with
     * the steps' top bits; Z is 22 bits of the attribute >> 10, the depth its 19-bit result kept to 18 (negative: 0,
     * past the top: all ones). A primitive without Z coefficients has Z = 0 there, not the primitive depth. */
    int offx = 0, offy = 0;
    if (cvg != 8) {
        while (offy < 4 && !((mask >> (offy < 2 ? 4 : 0)) & ((offy & 1) ? 0x5u : 0xAu))) offy++;
        if (offy < 4) {
            const unsigned row = (mask >> (offy < 2 ? 4 : 0)) & ((offy & 1) ? 0x5u : 0xAu);
            while (!((row >> (3 - offx)) & 1u)) offx++;
        } else offy = 0;
    }
    if (l & MRDP_L_Z_SRC_PRIM) {
        depth = (uint32_t)r->prim_z << 2;
    } else {
        const int32_t sz = (at[AZ] >> 10) & 0x3FFFFF;
        const int32_t zc = cvg == 8 ? sz >> 3 : (int32_t)((uint32_t)sz * 4u + (uint32_t)(offx * pr->xcd[4]) + (uint32_t)(offy * pr->xdy[4])) >> 5;
        const unsigned zt = ((unsigned)zc >> 17) & 3u;
        depth = zt == 2 ? 0x3FFFFu : zt == 3 ? 0u : (uint32_t)zc & 0x3FFFFu;
    }
#else
    if ((l & MRDP_L_Z_SRC_PRIM) || !pr->zbuf) {
        depth = (uint32_t)r->prim_z << 2;
    } else {
        int32_t z = at[AZ] >> 13;
        depth = (uint32_t)(z < 0 ? 0 : z > 0x3FFFF ? 0x3FFFF : z);
    }
#endif
    /* the pixel's dz code (the N64's dzpixenc: the top bit of the primitive's depth step, in 18-bit units and
     * doubled, or of SET_PRIM_DEPTH's delta), 4 bits */
    unsigned dzenc = (l & MRDP_L_Z_SRC_PRIM) ? hibit(r->prim_dz) : (pr->zbuf ? hibit(pr->dz) + 3u : 0u);
    if (dzenc > 15u) dzenc = 15u;
#ifdef RDP_EXACT
    /* the dz code the depth word keeps (its top two bits): the console's, the step in its units of 8 depth units (the
     * model's own code above is in depth units, 3 more, and keeps serving its depth compare and blender shifts) */
    const unsigned dzst = x_dzcomp((l & MRDP_L_Z_SRC_PRIM) ? r->prim_dz : (pr->xdzpix ? pr->xdzpix : 1u));
#else
    const unsigned dzst = dzenc;
#endif

    /* inputs */
    cc_in_t in;
    memset(&in, 0, sizeof in);
    mrdp_rgba_t tex_next = { 0, 0, 0, 0 };   /* the next tile's sample (TEXEL1 of the first cycle) */
    int have_next = 0;
#ifdef RDP_EXACT
    mrdp_rgba_t tex_ahead = { 0, 0, 0, 0 };  /* the next pixel's first texel (the second cycle's TEXEL1) */
    int have_ahead = 0;
#endif
#ifdef RDP_EXACT
    /* (the console's: 9 bits of the shade, at the first covered sample, then 256..383 saturated to 255 and 384..511, a
     * negative shade, to 0) */
    for (int c = 0; c < 4; c++) {
        const int32_t v = cvg == 8 ? at[AR + c] >> 16 : (int32_t)((uint32_t)(at[AR + c] >> 14) * 4u + (uint32_t)(offx * pr->xcd[c]) + (uint32_t)(offy * pr->xdy[c])) >> 4;
        const unsigned v9 = (unsigned)v & 0x1FFu;
        in.sh9[c] = v9 < 256u ? (int)v9 : v9 < 384u ? 255 : 0;
    }
#else
    if (pr->shade) {
        /* saturated to a byte: a shade past 255 enters the combiner as 255 (see the comment above cc_in_t) */
        in.sh9[0] = clamp8(at[AR] >> 16);
        in.sh9[1] = clamp8(at[AG] >> 16);
        in.sh9[2] = clamp8(at[AB] >> 16);
        in.sh9[3] = clamp8(at[AA] >> 16);
    }
#endif
    {
        /* A primitive without texture coefficients (a triangle whose command has no S, T, W block; a fill rectangle)
         * has S = T = W = 0, and the texture stages run all the same: TEXEL0 is the tile's texel at (0, 0) -- what
         * the last texture bound there left (the hardware's command unit starts every primitive with S, T, W at 0
         * and its pixel pipeline never skips the fetch). A combiner that reads TEXEL0 on such a triangle is
         * not a mistake of the game's: a character's collar (G_TEXTURE off, combiner TEXEL0 * SHADE, then
         * ENV * COMBINED) is drawn that way, and with TEXEL0 = 0 it was black. */
        int32_t s = 0, t = 0;
#ifdef RDP_EXACT
        /* (every primitive's S, T go through the perspective divide, a rectangle's too, as the console walks it as a
         * primitive with S, T in the integer halves; a primitive without texture coefficients has S = T = W = 0 there,
         * so with PERSP W's integer half is 0, out of range, and S and T saturate to 0x7FFF: TEXEL0 is the tile's texel
         * there (its last, clamped), not its first (docs/rdp_exact.md, rule 5)) */
        if (h & MRDP_H_PERSP) { s = n64_persp(at[AS], at[AW]); t = n64_persp(at[AT], at[AW]); }
        else { s = s105_noperp(at[AS] >> 5); t = s105_noperp(at[AT] >> 5); }
#else
        if (!pr->tex) { }
        else if (pr->texrect) { s = (int16_t)at[AS]; t = (int16_t)at[AT]; }
        else if (h & MRDP_H_PERSP) { s = n64_persp(at[AS], at[AW]); t = n64_persp(at[AT], at[AW]); }
        else { s = s105_noperp(at[AS] >> 5); t = s105_noperp(at[AT] >> 5); }
#endif
        /* the tiles of TEXEL0 and TEXEL1: the primitive's and the next, or with the LOD the level's */
        int tile0 = pr->tile;
        int tile1 = (pr->tile + 1) & 7;
        if (cyc == 1 && lod_wanted(r)) {
            const mrdp_lod_t L = lod_pixel(r, pr, at);
            in.lodf = L.frac;
            tile0 = L.tile0; tile1 = L.tile1;
            if ((h & RDP_H_TEX_LOD) && pr->tex && cvg) {
                r->lod_px++; r->lod_lvl[L.level & 7]++;
                r->lod_mag += L.magnify; r->lod_far += L.distant && !L.magnify;
            }
        }
#ifdef RDP_EXACT
        /* 1-cycle mode: the console's LOD from the next pixel and the one after (lod1_pixel): TEX_LOD moves TEXEL0's
         * tile, and the one cycle reads LOD_FRACTION (with DETAIL the tile moves on even at a level count of 0) */
        if (cyc == 0 && lod1_wanted(r)) {
            const mrdp_lod_t L1 = lod1_pixel(r, pr, at, pr->xj);
            in.lodf = L1.frac;
            tile0 = L1.tile0;
        }
#endif
#ifdef RDP_EXACT
        if (cyc == 2) {
            if (h & RDP_H_TEX_LOD) {
                /* copy mode with TEX_LOD (rule 3): an LOD from the group's next step and the one after; the tile plus the
                 * level, plus 1 with DETAIL when not magnified */
                int32_t P[3], N[3], F[3], ls[3], lt[3];
                int oob = 0;
                x_pt(P, &at[AS], 0, pr->xinc);
                x_pt(N, P, 1, pr->xinc); x_pt(F, P, 2, pr->xinc);
                x_div(r, N, &ls[0], &lt[0], &oob); x_div(r, F, &ls[1], &lt[1], &oob);
                ls[2] = ls[0]; lt[2] = lt[0];
                tile0 = mrdp_lod(h, r->min_level, pr->level, (unsigned)pr->tile, ls, lt, oob).tile0;
            }
            in.tex = x_copy_texel(r, tile0, s, t, pr->xcopy);
        } else
#endif
        in.tex = texture_pass(r, tile0, s, t, 0, s_no_texel);
        /* TEXEL1 in the first cycle of a 2-cycle pixel: the next tile (tile + 1) at the same S, T -- the N64's
         * second texture (a skybox's day / night blend, a terrain's second layer). Its second cycle still
         * takes TEXEL0 for both: one fetch more per pixel is all rtl/rdp has. (Without texture coefficients:
         * TEXEL0 again, one pass.) */
        in.tex1 = in.tex;
#ifdef RDP_EXACT
        if (cyc == 1 && (cc0_uses_texel1(r) || cc1_uses_texel0(r))) {      /* (the next tile's texel, coefficients or not) */
#else
        if (pr->tex && cyc == 1 && (cc0_uses_texel1(r) || cc1_uses_texel0(r))) {
#endif
            tex_next = texture_pass(r, tile1, s, t, 1, in.tex);
            if (cc0_uses_texel1(r)) in.tex1 = tex_next;
            have_next = 1;
        }
#ifdef RDP_EXACT
        if (cyc == 1 && cc1_uses_texel1(r)) {
            /* 2-cycle mode, the second cycle's TEXEL1 (rule 7): the next pixel's first texel, at the next point of the
             * walk in the tile of that pixel's own LOD; at the span's last pixel, when the next row has a span and this
             * one is 3 or more long, the tile is the next row's start's (its LOD from that start and the line after
             * it alone), the coordinates still the next point's */
            int32_t an[NATTR], s1, t1;
            memcpy(an, at, sizeof an);
            x_pt(&an[AS], &at[AS], 1, pr->xinc);
            if (h & MRDP_H_PERSP) { s1 = n64_persp(an[AS], an[AW]); t1 = n64_persp(an[AT], an[AW]); }
            else { s1 = s105_noperp(an[AS] >> 5); t1 = s105_noperp(an[AT] >> 5); }
            int tl = pr->tile;
            if (h & RDP_H_TEX_LOD) {
                if (pr->xj == pr->xlen && pr->xnvalid && pr->xlodlen >= 3) {
                    int32_t N0[3], N2[3], ls[3], lt[3];
                    int oob = 0;
                    x_pt(N0, pr->xnrow, 0, pr->ddy); x_pt(N2, pr->xnrow, 1, pr->ddy);
                    x_div(r, N0, &ls[0], &lt[0], &oob); x_div(r, N2, &ls[2], &lt[2], &oob);
                    ls[1] = ls[0]; lt[1] = lt[0];
                    tl = mrdp_lod(h, r->min_level, pr->level, (unsigned)pr->tile, ls, lt, oob).tile0;
                } else {
                    const mrdp_lod_t La = lod_pixel(r, pr, an);
                    tl = La.tile0;
                }
            }
            tex_ahead = texture_pass(r, tl, s1, t1, 0, s_no_texel);
            have_ahead = 1;
        }
        if (cyc == 0 && cc1_uses_texel1(r)) {
            /* 1-cycle TEXEL1: the next pixel's texel, at the next point of the walk (at the last pixel of a span longer
             * than 7, with a next row, that row's start) in the next pixel's LOD tile */
            int32_t N[3], s1, t1;
            int tl = pr->tile;
            const int nr = pr->xj == pr->xlen && pr->xlodlen > 7 && pr->xnvalid;
            if (nr) x_pt(N, pr->xnrow, 0, pr->xinc);
            else x_pt(N, &at[AS], 1, pr->xinc);
            if (h & MRDP_H_PERSP) { s1 = n64_persp(N[0], N[2]); t1 = n64_persp(N[1], N[2]); }
            else { s1 = s105_noperp(N[0] >> 5); t1 = s105_noperp(N[1] >> 5); }
            if ((h & RDP_H_TEX_LOD)) tl = lod1_pixel(r, pr, at, pr->xj + 1).tile0;
            in.tex1 = texture_pass(r, tl, s1, t1, 0, s_no_texel);
        }
#endif
    }

    /* combiner (the alphas as cc_alpha_res: 0..256, ar for the coverage; the first cycle's, ar0, for the alpha compare) */
    mrdp_rgba_t comb;
    unsigned ar = 0, ar0 = 0;
    if (cyc == 2) {
        comb = in.tex;
        ar = comb.a;
    } else {
        if (cyc == 1) {
            cc_out_t c0 = combine_raw(r, &in, 0);
            for (int i = 0; i < 4; i++) in.cm9[i] = c0.v[i];
            ar0 = (unsigned)cc_alpha_res(c0.v[3]);
            /* the second cycle: TEXEL0 is the first cycle's TEXEL1 (the next tile's sample), TEXEL1 the tile's (the N64's is
             * the next pixel's tile sample: not kept). A pixel without texture coefficients keeps TEXEL0 as it is. */
            in.tex1 = in.tex;
            if (have_next) in.tex = tex_next;
#ifdef RDP_EXACT
            if (have_ahead) in.tex1 = tex_ahead;
#endif
        }
        cc_out_t c1 = combine_raw(r, &in, 1);
        comb.r = (uint8_t)cc_color_res(c1.v[0]); comb.g = (uint8_t)cc_color_res(c1.v[1]); comb.b = (uint8_t)cc_color_res(c1.v[2]);
        ar = (unsigned)cc_alpha_res(c1.v[3]);
        comb.a = (uint8_t)(ar > 255u ? 255u : ar);
    }

    /* alpha and coverage: CVG_X_ALPHA scales the coverage by the alpha,
     * ALPHA_CVG_SEL makes the alpha the coverage; else the alpha dither.
     * (2-cycle: the alpha compare takes the first cycle's alpha, through the
     * same steps.) */
    const unsigned adith = cyc == 2 ? 0u : dither_a(r, x, y, rnd);                  /* (copy: the texel's alpha as it is) */
    unsigned cvgmul = ar * cvg + 4u;                                           /* 12 bits (an alpha of 255 counts 256) */
    unsigned cvg_sel = (l & MRDP_L_CVG_X_ALPHA) ? (cvgmul >> 8) : cvg;
    unsigned pa, ca;
    if (!(l & MRDP_L_ALPHA_CVG_SEL)) pa = ar + adith;
    else pa = (l & MRDP_L_CVG_X_ALPHA) ? (cvgmul >> 3) : (cvg << 5);
    if (pa > 255) pa = 255;
    ca = pa;
    if (cyc == 1) {
        unsigned m0 = ar0 * cvg + 4u;
        if (!(l & MRDP_L_ALPHA_CVG_SEL)) ca = ar0 + adith;
        else ca = (l & MRDP_L_CVG_X_ALPHA) ? (m0 >> 3) : (cvg << 5);
        if (ca > 255) ca = 255;
    }
    unsigned shade_a = (unsigned)in.sh9[3] + adith;           /* (the blender's: 255 from 256 up) */
    if (shade_a > 255) shade_a = 255;

    /* alpha compare (DITHER_ALPHA_EN: against a random 8-bit number, the LFSR's [7:0]) */
    /* (copy mode: the texel's alpha bit, not the blend colour; no depth either) */
    const int copy = cyc == 2;
    if (l & MRDP_L_ALPHA_CMP) {
        unsigned thr = copy ? 128u : (l & MRDP_L_ALPHA_DITHER) ? (rnd & 0xFFu) : r->blend.a;
        if (ca < thr) return;
    }

    /* memory: the colour and its coverage */
    mrdp_rgba_t mem = { 0, 0, 0, 255 };
    unsigned memcvg = 7;
    const int rd_mem = !c8 && cvg_need_mem(l, cyc);
    if (rd_mem) {
        if (c32) {
            /* the memory colour at 8 bits a channel, and the coverage from the alpha byte's top three bits (the
             * hidden bits are not used with a 32-bit image: the manual's "the hidden bits are ignored") */
            const uint16_t hi = r->rd16(r->ctx, caddr), lo = r->rd16(r->ctx, caddr + 2u);
            mem.r = (uint8_t)(hi >> 8); mem.g = (uint8_t)hi; mem.b = (uint8_t)(lo >> 8);
            if (l & MRDP_L_IM_RD) memcvg = (lo >> 5) & 7u;
        } else
        {
            uint16_t mw = r->rd16(r->ctx, caddr);
            mem = unrgb565(mw);
            if (l & MRDP_L_IM_RD) memcvg = (mw & 1u) << 2 | hid_get(r, caddr);                   /* bit 0 and the hidden two */
        }
    }
    const int overflow = (memcvg + cvg_sel) >= 8u;

    /* depth compare; the blend enable */
    int zpass = 1, blend_en;
    unsigned sh_a = 0, sh_b = 0;
    if ((l & MRDP_L_Z_CMP) && !copy && !c8) {
        /* the memory's depth, as much of it as its word keeps; the tolerance: the primitive's step (16-bit units: its
         * two low bits set here), and no less than what a word of the exponents 0, 1 and 2 has lost (the N64's 16 >>
         * exponent, in 8s) -- a surface drawn over itself is within it at any depth */
        const uint16_t oldw = r->rd16(r->ctx, zaddr);
        const uint32_t old = z_dec(oldw);
        unsigned zmode = (l >> MRDP_L_ZMODE_SHIFT) & 3;
        unsigned dz = pr->zbuf ? pr->dz : 1u;
        const uint32_t tol = (dz << 2 | 3u) | ((oldw >> 13) < 3u ? (0x7Fu >> (oldw >> 13)) : 0u);
        int max = old == 0x3FFFF, infront = depth < old;
        int nearer = (int)depth - (int)tol <= (int)old;
        int farther = (int)depth + (int)tol >= (int)old;
        blend_en = (l & MRDP_L_FORCE_BL) || (!overflow && aa && farther);
        if (zmode == 1 && infront && farther && overflow) {
            /* interpenetrating: the coverage scaled by how far it is in front, in dz */
            unsigned sft = hibit(dz) + 3;
            unsigned coeff = ((old >> sft) - (depth >> sft)) & 0xFu;
            cvg_sel = ((cvg_sel * coeff) >> 3) & 0xFu;
        } else if (zmode == 2) {
            zpass = infront || max;
        } else if (zmode == 3) {
            zpass = farther && nearer && !max;
        } else {
            zpass = overflow ? (infront || max) : (nearer || max);
        }
    } else {
        blend_en = (l & MRDP_L_FORCE_BL) || (!overflow && aa);
        /* without a depth compare the memory coverage's weight is shifted by
         * the primitive's dz code */
        sh_b = dzenc >= 12u ? 15u - dzenc : 4u;
    }
    if (!zpass) return;
    /* with AA_EN drawn when a sample is left after CVG_X_ALPHA; without, the
     * pixel's own coverage (checked before) decides, so a CVG_X_ALPHA pixel
     * whose scaled coverage is 0 is still written */
    if (cvg_sel == 0 && !copy && aa) return;

    /* blender */
    mrdp_rgba_t out;
    const int divide = !(l & MRDP_L_FORCE_BL);
    if (cyc == 2) {
        out = comb;
    } else if (cyc == 1) {
        mrdp_rgba_t b0 = blend_n64(r, 0, comb, mem, pa, shade_a, memcvg, sh_a, sh_b, 1, 1, 0, 1);
        out = blend_n64(r, 1, b0, mem, pa, shade_a, memcvg, sh_a, sh_b, blend_en, overflow, divide, 0);
    } else {
        out = blend_n64(r, 0, comb, mem, pa, shade_a, memcvg, sh_a, sh_b, blend_en, overflow, divide, 0);
    }

    /* the coverage written (CVG_DST) */
    unsigned sum = cvg_sel + memcvg, cvg_out;
    switch ((l >> MRDP_L_CVG_DST_SHIFT) & 3) {
    case 0:  cvg_out = blend_en ? (sum >= 8u ? 7u : sum) : ((cvg_sel - 1u) & 7u); break;
    case 1:  cvg_out = sum & 7u; break;
    case 2:  cvg_out = 7u; break;
    default: cvg_out = memcvg; break;
    }
    if (cyc == 2) cvg_out = comb.a >= 128 ? 7u : 0u;                  /* copy: the texel's alpha bit */
    int complete = (int)(cvg_out >> 2);

#ifdef MRDP_DEBUG_PIXEL
    if (x == r->dbg_x && y == r->dbg_y) {
        fprintf(stderr, "MRDP pixel (%d,%d) <- %04x depth %05x cvg %u mem %u blend %d op %02x words:", x, y, rgb5551(out, complete), depth, cvg_sel, memcvg, blend_en, (r->cmd[0] >> 24) & 0x3F);
        for (int i = 0; i < r->ncmd; i++) fprintf(stderr, " %08x", r->cmd[i]);
        fprintf(stderr, "\n  at: r %08x g %08x b %08x a %08x s %08x t %08x w %08x z %08x\n",
                at[AR], at[AG], at[AB], at[AA], at[AS], at[AT], at[AW], at[AZ]);
    }
#endif
    if (c8) {
        wr8(r, caddr, out.r);
        r->pixels_drawn++;
        return;
    }
    if (c32) {
        /* RGBA8888: the channels whole (no 5-bit truncation), the alpha byte the coverage << 5 and its low five bits 0
         * (the pixel's own alpha is not stored, as in a 16-bit image); no hidden bits (copy mode never gets here) */
        if (CIMG32_DITHER) out = dither_n64(r, out, x, y, rnd, 1);
        r->wr16(r->ctx, caddr, (uint16_t)(out.r << 8 | out.g));
        r->wr16(r->ctx, caddr + 2u, (uint16_t)(out.b << 8 | cvg_out << 5));
        if (l & MRDP_L_Z_UPD) r->wr16(r->ctx, zaddr, (uint16_t)(z_enc(depth) | dzst >> 2));
        r->pixels_drawn++;
        return;
    }
    if (cyc != 2) out = dither_n64(r, out, x, y, rnd, 1);
    r->wr16(r->ctx, caddr, rgb5551(out, complete));
    hid_set(r, caddr, cvg_out);
    if ((l & MRDP_L_Z_UPD) && !copy) r->wr16(r->ctx, zaddr, (uint16_t)(z_enc(depth) | dzst >> 2));
    r->pixels_drawn++;
}
#endif /* MRDP_CVG */

static void draw_pixel(mrdp_t *r, const prim_t *pr, int x, int y, const int32_t *at, unsigned mask)
{
    uint32_t h = r->other_h, l = r->other_l;
    unsigned cyc = (h >> MRDP_H_CYCLE_SHIFT) & 3;
    uint32_t pix_off = ((uint32_t)y * r->cimg_width + (uint32_t)x) * 2;
    uint32_t caddr = r->cimg + pix_off, zaddr = r->zimg + pix_off;

    if (cyc == 3 && r->cimg_size == 1) {           /* fill mode, an 8-bit image (wr8) */
        caddr = r->cimg + (uint32_t)y * r->cimg_width + (uint32_t)x;
        wr8(r, caddr, r->fill >> (8u * (3u - (caddr & 3u))));
        r->pixels_drawn++;
        return;
    }
#ifdef MRDP_CVG
    if (cyc == 3 && r->cimg_size == 3) {           /* fill mode, a 32-bit image: the fill colour whole, a pixel a word (no hidden bits) */
        caddr = r->cimg + 2u * pix_off;
        r->wr16(r->ctx, caddr, (uint16_t)(r->fill >> 16));
        r->wr16(r->ctx, caddr + 2u, (uint16_t)r->fill);
        r->pixels_drawn++;
        return;
    }
#endif
    if (cyc == 3) {                                /* fill mode */
        r->wr16(r->ctx, caddr, (x & 1) ? (uint16_t)r->fill : (uint16_t)(r->fill >> 16));
#ifdef MRDP_CVG
        /* the hidden bits: each pixel's {a, a}, a its colour's bit 0 -- a fill
         * rectangle's for every 4-pixel quad it touches (as the N64 writes its
         * 64-bit words), a triangle's for its pixels */
        if (pr->fillq) {
            uint32_t q = caddr & ~7u;
            for (unsigned i = 0; i < 4; i++)
                hid_set(r, q + 2u * i, ((r->fill >> ((i & 1u) ? 0 : 16)) & 1u) ? 3u : 0u);
        } else
            hid_set(r, caddr, ((x & 1) ? r->fill : r->fill >> 16) & 1u ? 3u : 0u);
#endif
        r->pixels_drawn++;
        return;
    }
#ifdef MRDP_CVG
    draw_pixel_cvg(r, pr, x, y, at, mask);
    return;
#else
    (void)mask;
#endif

    /* depth */
    uint16_t depth;
    if ((l & MRDP_L_Z_SRC_PRIM) || !pr->zbuf) {
        depth = r->prim_z;
    } else {
        int32_t z = at[AZ] >> 15;
        depth = (uint16_t)(z < 0 ? 0 : z > 0xFFFF ? 0xFFFF : z);
    }
    if (l & MRDP_L_Z_CMP) {
        uint16_t old = r->rd16(r->ctx, zaddr);
        unsigned zmode = (l >> MRDP_L_ZMODE_SHIFT) & 3;
        int pass = zmode == 3 ? depth <= old : depth < old;
#ifdef MRDP_N64
        /* A decal lies on the surface it marks: it is drawn where the depth
         * there is within the primitive's own step of it (the N64's rule, on
         * its dz). As "not behind" alone, a shadow came and went with the
         * rounding between its plane's depth and the floor's -- and drew
         * over whatever was beyond the floor's edge. */
        if (zmode == 3) {
            int tol = pr->zbuf ? (int)pr->dz : 1;
            int lo = (int)depth - tol, hi = (int)depth + tol;
            if (lo < 0) lo = 0;
            if (hi > 0xFFFF) hi = 0xFFFF;
            pass = (int)old >= lo && (int)old <= hi;
        }
#endif
        if (!pass) return;
    }

    /* inputs */
    cc_in_t in;
    memset(&in, 0, sizeof in);
    if (pr->shade) {
        in.shade.r = (uint8_t)clamp8(at[AR] >> 16);
        in.shade.g = (uint8_t)clamp8(at[AG] >> 16);
        in.shade.b = (uint8_t)clamp8(at[AB] >> 16);
        in.shade.a = (uint8_t)clamp8(at[AA] >> 16);
    }
#ifndef MRDP_N64
    if (pr->tex)                                   /* (MRDP's own primitives: no texture, no texel) */
#endif
    {
        int32_t s = 0, t = 0;                      /* (the N64's, without texture coefficients: S = T = 0, the tile's texel (0, 0); see draw_pixel_cvg) */
        if (!pr->tex) { }
        else if (pr->texrect) { s = (int16_t)at[AS]; t = (int16_t)at[AT]; }   /* s10.5: wraps at 16 bits */
#ifdef MRDP_N64
        /* the N64's units: S and T are s10.5 in their integer half, W is s.15
         * (S / W is S * 2^15 / W): through the same reciprocal, 4 bits down */
        else if (h & MRDP_H_PERSP) { s = mrdp_persp(at[AS] >> 4, (uint32_t)at[AW]); t = mrdp_persp(at[AT] >> 4, (uint32_t)at[AW]); }
        else { s = s105_noperp(at[AS] >> 5); t = s105_noperp(at[AT] >> 5); }
#else
        else if (h & MRDP_H_PERSP) { s = mrdp_persp(at[AS], (uint32_t)at[AW]); t = mrdp_persp(at[AT], (uint32_t)at[AW]); }
        else { s = s105_noperp(at[AS]); t = s105_noperp(at[AT]); }
#endif
        in.tex = texture_pass(r, pr->tile, s, t, 0, s_no_texel);
        in.tex1 = in.tex;
    }

    /* combiner */
    mrdp_rgba_t comb;
    if (cyc == 2) {                                /* copy: the texel itself */
        comb = in.tex;
    } else if (cyc == 1) {
        in.combined = combine(r, &in, 0);
        comb = combine(r, &in, 1);
    } else {
        comb = combine(r, &in, 1);
    }

    /* alpha compare / coverage from alpha */
    if ((l & MRDP_L_ALPHA_CMP) && comb.a < r->blend.a) return;
    if ((l & MRDP_L_CVG_X_ALPHA) && comb.a < MRDP_CVG_ALPHA_MIN) return;

    /* blender */
    mrdp_rgba_t mem = { 0, 0, 0, 255 };
    int need_mem = cyc == 1 ? (bl_reads_memory(l, 0) || bl_reads_memory(l, 1)) : bl_reads_memory(l, 0);
#ifdef MRDP_N64
    /* (without FORCE_BL the pixel is the last cycle's P alone: memory is read
     * only if that is memory, or the first cycle's blend of it) */
    if (!(l & MRDP_L_FORCE_BL)) {
        unsigned p = (l >> (cyc == 1 ? 28 : 30)) & 3;
        need_mem = p == 1 || (cyc == 1 && p == 0 && bl_reads_memory(l, 0));
    }
#endif
#ifdef MRDP_NO_BLEND
    need_mem = 0;
#endif
    if (need_mem && cyc != 2) mem = unrgb565(r->rd16(r->ctx, caddr));
    mrdp_rgba_t out;
#ifdef MRDP_NO_BLEND
    /* No blender. A surface blended with what is behind it (FORCE_BL, and
     * the last cycle's blender reading the colour image) is drawn through a
     * screen door instead: of every 4 x 4 pixels, as many as its alpha is of
     * 16 (a fade's black quad, a logo's soft edge, a shadow -- drawn opaque
     * they would cover what they only tint). */
    if ((l & MRDP_L_FORCE_BL) && cyc < 2 && bl_reads_memory(l, cyc == 1)) {
        static const uint8_t door[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
        unsigned sel = (l >> (cyc == 1 ? 24 : 26)) & 3;
        unsigned a = sel == 0 ? comb.a : sel == 1 ? r->fog.a : sel == 2 ? in.shade.a : 0;
        if (a < door[y & 3][x & 3] * 16u + 8u) return;
    }
    if (1) {
        out = comb;
#else
    if (cyc == 2) {
        out = comb;
#endif
    } else if (cyc == 1) {
        mrdp_rgba_t b0 = blend(r, 0, comb, mem, comb.a, in.shade.a);
        out = (l & MRDP_L_FORCE_BL) ? blend(r, 1, b0, mem, comb.a, in.shade.a)
                                    : bl_pm(r, (l >> 28) & 3, b0, mem);
    } else {
        out = (l & MRDP_L_FORCE_BL) ? blend(r, 0, comb, mem, comb.a, in.shade.a)
                                    : bl_pm(r, (l >> 30) & 3, comb, mem);
    }

#ifdef MRDP_DEBUG_PIXEL
    if (x == r->dbg_x && y == r->dbg_y) {
        fprintf(stderr, "MRDP pixel (%d,%d) <- %04x depth %04x op %02x words:", x, y, rgb565(out), depth, (r->cmd[0] >> 24) & 0x3F);
        for (int i = 0; i < r->ncmd; i++) fprintf(stderr, " %08x", r->cmd[i]);
        fprintf(stderr, "\n  at: r %08x g %08x b %08x a %08x s %08x t %08x w %08x z %08x\n",
                at[AR], at[AG], at[AB], at[AA], at[AS], at[AT], at[AW], at[AZ]);
    }
#endif
#ifdef MRDP_N64
    if (cyc != 2) out = dither_n64(r, out, x, y, 0, 0);
#endif
    r->wr16(r->ctx, caddr, rgb565(out));
    if (l & MRDP_L_Z_UPD) r->wr16(r->ctx, zaddr, depth);
    r->pixels_drawn++;
}

static inline int sc_x0(const mrdp_t *r) { return r->sc_xh >> 2; }
static inline int sc_x1(const mrdp_t *r) { return r->sc_xl >> 2; }
static inline int sc_y0(const mrdp_t *r) { return r->sc_yh >> 2; }
static inline int sc_y1(const mrdp_t *r) { return r->sc_yl >> 2; }

/* ---- triangle ------------------------------------------------------------ */

/* attribute: value, per-pixel and per-scanline (along the H edge) slopes */
typedef struct { int32_t v, dx, de; } attr_t;

static void tri_attrs(const uint32_t *w, attr_t *a, int base, int n)
{
    /* N64 layout: w[0] ints (A,B,C,D), w[2] ints of DxDx, w[4] fracs,
     * w[6] fracs of DxDx, w[8] ints of DxDe, w[10] ints of DxDy,
     * w[12] fracs of DxDe, w[14] fracs of DxDy -- 64-bit words as pairs */
    for (int k = 0; k < n; k++) {
        int wi = k >> 1, sh = (k & 1) ? 0 : 16;
        uint32_t vi = (w[0 + wi] >> sh) & 0xFFFF, vf = (w[4 + wi] >> sh) & 0xFFFF;
        uint32_t xi = (w[2 + wi] >> sh) & 0xFFFF, xf = (w[6 + wi] >> sh) & 0xFFFF;
        uint32_t ei = (w[8 + wi] >> sh) & 0xFFFF, ef = (w[12 + wi] >> sh) & 0xFFFF;
        a[base + k].v = (int32_t)(vi << 16 | vf);
        a[base + k].dx = (int32_t)(xi << 16 | xf);
        a[base + k].de = (int32_t)(ei << 16 | ef);
    }
}

#ifdef MRDP_CVG
/* AA_EN: the N64's walker (mrdp.h). The edges start at XH & ~1, XM & ~1 at the
 * top of the first row (XL at YM, taken back to its row's top) and step
 * (DxDy >> 2) & ~1 a subscanline -- four a row -- in eighths with a sticky
 * bit, clamped to the scissor; a subscanline is valid between YH and YL and
 * the scissor's quarter rows, with its right end not before its left. A
 * pixel's samples: two a subscanline in a checkerboard (0xA on even ones, 0x5
 * on odd, bit 3 the leftmost quarter); the N64's byte has subscanlines 0 and
 * 1 in its high nibble. Attributes are the row's at the pixel centre (MRDP's
 * exh, as the other path); Z is moved to the first covered sample -- the
 * lowest subscanline, its leftmost sample -- in eighths from the centre. */
static void draw_triangle_aa(mrdp_t *r, const prim_t *pr, int aa_en, int lft, int32_t yh, int32_t ym, int32_t yl,
                             int32_t xh, int32_t dxh, int32_t xm, int32_t dxm, int32_t xl, int32_t dxl,
                             const attr_t *at)
{
    int y0 = yh >> 2, ymr = ym >> 2;
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r);
    int yb = (yl + 3) >> 2;
    if (yb > sc_y1(r)) yb = sc_y1(r);
    const uint32_t qh = (uint32_t)(dxh >> 2) & ~1u, qm = (uint32_t)(dxm >> 2) & ~1u, ql = (uint32_t)(dxl >> 2) & ~1u;
    uint32_t axh = ((uint32_t)xh & ~1u) + (uint32_t)(ya - y0) * (qh << 2);
    uint32_t axm = ((uint32_t)xm & ~1u) + (uint32_t)(ya - y0) * (qm << 2);
    uint32_t axl = ((uint32_t)xl & ~1u) - (uint32_t)(ym & 3) * ql + (uint32_t)(ya - ymr) * (ql << 2);
    const int32_t cxh = (int32_t)r->sc_xh << 1, cxl = (int32_t)r->sc_xl << 1;   /* the scissor in eighths */
    const int32_t yhl = yh > (int32_t)r->sc_yh ? yh : (int32_t)r->sc_yh;        /* and in quarter rows */
    const int32_t yll = yl < (int32_t)r->sc_yl ? yl : (int32_t)r->sc_yl;
    for (int y = ya; y < yb; y++) {
        uint32_t k = (uint32_t)(y - y0);
        int32_t exh = (int32_t)((uint32_t)xh + k * (uint32_t)dxh);   /* the row's top: attributes (the N64's, not the pixel centre) */
        int lp[4], rp[4], lf[4], rf[4], valid[4], any = 0;
        int xmin = 0, xmax = 0;
        for (int j = 0; j < 4; j++) {
            int yq = 4 * y + j;
            uint32_t xa = axh + (uint32_t)j * qh;
            uint32_t xb = (yq < ym) ? axm + (uint32_t)j * qm : axl + (uint32_t)j * ql;
            int32_t lx = (int32_t)(lft ? xa : xb), rx = (int32_t)(lft ? xb : xa);
            int crossed = (rx >> 14) < (lx >> 14);
            int32_t l8 = (int32_t)(((uint32_t)(lx >> 14) << 1) | ((lx & 0x3FFE) != 0));   /* eighths, sticky */
            int32_t r8 = (int32_t)(((uint32_t)(rx >> 14) << 1) | ((rx & 0x3FFE) != 0));
            if (l8 < cxh) l8 = cxh;
            if (l8 > cxl) l8 = cxl;
            if (r8 < cxh) r8 = cxh;
            if (r8 > cxl) r8 = cxl;
            lp[j] = l8 >> 3; lf[j] = l8 & 7; rp[j] = r8 >> 3; rf[j] = r8 & 7;
            valid[j] = yq >= yhl && yq < yll && !crossed;
            if (valid[j]) {
                if (!any || lp[j] < xmin) xmin = lp[j];
                if (!any || rp[j] > xmax) xmax = rp[j];
                any = 1;
            }
        }
        axh += qh << 2; axm += qm << 2; axl += ql << 2;
#ifdef MRDP_DEBUG_AA
        if (any) {
            int v = 0; for (int j = 0; j < 4; j++) v |= valid[j] << j;
            fprintf(stderr, "AAROW y=%d v=%x", y, v);
            for (int j = 3; j >= 0; j--) fprintf(stderr, " %d:%d:%d:%d", lp[j] & 0xFFFF, lf[j], rp[j] & 0xFFFF, rf[j]);
            fprintf(stderr, " min=%d max=%d\n", xmin, xmax);
        }
#endif
        if (!any) continue;
        if (xmax > sc_x1(r) - 1) xmax = sc_x1(r) - 1;      /* (the pixel at the scissor's end never has a sample) */
        if (xmin > xmax) continue;
        int32_t dx0 = (int32_t)((uint32_t)xmin << 16) - exh;
        int32_t cur[NATTR];
        for (int i = 0; i < NATTR; i++) {
            uint32_t e = (uint32_t)at[i].v + k * (uint32_t)at[i].de;
            cur[i] = (int32_t)(e + (uint32_t)(int32_t)(((int64_t)at[i].dx * dx0) >> 16));
        }
        for (int x = xmin; x <= xmax; x++) {
            if (x > xmin)
                for (int i = 0; i < NATTR; i++) cur[i] = (int32_t)((uint32_t)cur[i] + (uint32_t)at[i].dx);
            unsigned cj[4];
            for (int j = 0; j < 4; j++) {
                unsigned fm = (j & 1) ? 0x5u : 0xAu;
                unsigned lm = (0xFu >> ((lf[j] + 1) >> 1)) & fm;
                unsigned rm = ((0xF0u >> ((rf[j] + 1) >> 1)) & 0xFu) & fm;
                cj[j] = 0;
                if (!valid[j]) continue;
                if (x > lp[j] && x < rp[j]) cj[j] = fm;
                else if (x == lp[j] && x == rp[j]) cj[j] = lm & rm;
                else if (rp[j] > lp[j] && x == lp[j]) cj[j] = lm;
                else if (rp[j] > lp[j] && x == rp[j]) cj[j] = rm;
            }
            unsigned mask = ((cj[0] | cj[1]) << 4) | (cj[2] | cj[3]);
            if (!mask) continue;
            if (!aa_en && !(mask & 0x80u)) continue;      /* no AA_EN: drawn only if the top-left sample (sample 7) is covered */
            int32_t pc[NATTR];
            for (int i = 0; i < NATTR; i++) pc[i] = cur[i];
            int oy = 0; while (!cj[oy]) oy++;
            int ox = 0; while (!((cj[oy] >> (3 - ox)) & 1u)) ox++;
            int32_t sx8 = 2 * ox, sy8 = 2 * oy;      /* the sample, from the pixel's top-left corner, 1/8 px (0 for sample 7) */
            uint32_t corr = ((uint32_t)((at[AZ].dx >> 5) * sx8) + (uint32_t)((pr->dzdy >> 5) * sy8)) << 2;
            pc[AZ] = (int32_t)((uint32_t)pc[AZ] + corr);
#ifdef MRDP_DEBUG_AA
            fprintf(stderr, "AAPIX x=%d y=%d mask=%02x sx=%d sy=%d z=%08x\n", x, y, mask, sx8, sy8, (uint32_t)pc[AZ]);
#endif
            draw_pixel(r, pr, x, y, pc, mask);
        }
    }
}
#endif /* MRDP_CVG */

#ifdef RDP_EXACT
/* ---- the console's walk (RDP_EXACT; docs/rdp_exact.md, rules 4 and 6) ----------------------
 * A primitive (a triangle; a texture or fill rectangle is one too, as the console makes it) is walked as the console
 * walks it. Each row's four subscanlines put each edge in eighths of a pixel (a sticky bit for the rest), in its 12 bits
 * of whole pixels: an edge at or past 2048 pixels counts as left of the scissor, and the crossing test compares the
 * edges' 12.2 bits, so a row whose edge passed 2047 is not drawn. Each edge is clamped to the scissor; a row is drawn
 * when a subscanline is in and not every edge is past the scissor on one side. The span runs from the major edge's
 * end (rx: the left end with lft, else the right) to the minor edge's (lx), both ends included; in fill and copy mode
 * every pixel of it is written (the scissor's last column too), in the sampled modes those with a covered sample. Each
 * row is computed before the one above it is drawn, as the console walks every edge before it renders: the 1-cycle LOD
 * and the texels taken a pixel ahead look at the next row's span. */
typedef struct {
    int valid;
    int rx, lx, len, lodlen;       /* the walk's first pixel and last, the pixels after the first (negative: none), and
                                      len plus the pixels the scissor took off at the major edge (the LOD's span ends) */
    int unscrx, scd;               /* the major edge's own pixel, where the attributes are taken (not clipped), and the
                                      walk's steps from it to rx, 12 bits */
    int32_t a[NATTR];              /* the attributes at unscrx */
    int lp[4], rp[4], lf[4], rf[4], sv[4];   /* each subscanline's left and right edges (pixel, eighth), and whether in */
} xrow_t;
typedef struct {
    int lft;
    int32_t yh, ym, yl, xh, dxh, xm, dxm, xl, dxl;
    const attr_t *at;
    const int32_t *dy;             /* DaDy of each attribute */
} xtri_t;
/* a pixel's step of attribute i along x: DaDx without its low 5 bits (the console's span adders are 27 bits), Z whole */
static inline int32_t x_dx(const xtri_t *T, int i) { return i == AZ ? T->at[i].dx : (int32_t)((uint32_t)T->at[i].dx & ~0x1Fu); }
/* the rows the console renders: from YH's and to YL's, both clipped by the scissor (a negative YH: the scissor's; a YL of
 * 4096 quarter rows or more: the scissor's) */
static int32_t x_yhlimit(const mrdp_t *r, int32_t yh) { return (yh & 0x2000) ? (int32_t)r->sc_yh : (yh & 0x1000) ? yh : yh >= (int32_t)r->sc_yh ? yh : (int32_t)r->sc_yh; }
static int32_t x_yllimit(const mrdp_t *r, int32_t yl) { return (yl & 0x2000) ? yl : (yl & 0x1000) ? (int32_t)r->sc_yl : (yl & 0xFFF) < (int32_t)r->sc_yl ? yl : (int32_t)r->sc_yl; }
/* an edge on a subscanline, in eighths clamped to the scissor (cxh, cxl: its edges in eighths) */
static int32_t x_edge(int32_t x, int32_t cxh, int32_t cxl, int *over, int *under)
{
    const int32_t sticky = ((x >> 1) & 0x1FFF) != 0;
    int32_t e = ((x >> 13) & 0x1FFE) | sticky;
    *under = (x & 0x8000000) || (e < cxh && !(x & 0x4000000));
    e = *under ? cxh : (((x >> 13) & 0x3FFE) | sticky);
    *over = (e & 0x2000) || (e & 0x1FFF) >= cxl;
    return (*over ? cxl : e) & 0x1FFF;
}

static void x_row(const mrdp_t *r, const xtri_t *T, int y, xrow_t *o)
{
    const int flip = T->lft;
    const int32_t ycur = T->yh & ~3, yhl = x_yhlimit(r, T->yh), yll = x_yllimit(r, T->yl);
    const int32_t cxh = (int32_t)r->sc_xh << 1, cxl = (int32_t)r->sc_xl << 1;
    const uint32_t qh = (uint32_t)(T->dxh >> 2) & ~1u, qm = (uint32_t)(T->dxm >> 2) & ~1u, ql = (uint32_t)(T->dxl >> 2) & ~1u;
    /* the attributes are taken on subscanline 0, or 3 when the major edge leans away from the walk */
    const int ld = ((T->dxh < 0) ^ flip) ? 0 : 3;
    int allover = 1, allunder = 1, allinval = 1, mxmin = 0xFFF, mxmax = 0, hxmin = 0xFFF, hxmax = 0, xfrac = 0;
    o->unscrx = 0;
    for (int j = 0; j < 4; j++) {
        const int32_t k = 4 * y + j;
        const int32_t xr = (int32_t)(((uint32_t)T->xh & ~1u) + (uint32_t)(k - ycur) * qh);
        /* (the minor edge turns at YM itself, if the walk reaches it) */
        const int32_t xm = (int32_t)((k >= T->ym && T->ym >= ycur) ? ((uint32_t)T->xl & ~1u) + (uint32_t)(k - T->ym) * ql
                                                                    : ((uint32_t)T->xm & ~1u) + (uint32_t)(k - ycur) * qm);
        int ov, un;
        const int32_t hs = x_edge(xr, cxh, cxl, &ov, &un);
        allover &= ov; allunder &= un;
        const int32_t ms = x_edge(xm, cxh, cxl, &ov, &un);
        allover &= ov; allunder &= un;
        const int32_t a = (int32_t)(((uint32_t)xm ^ (1u << 27)) & (0x3FFFu << 14)), b = (int32_t)(((uint32_t)xr ^ (1u << 27)) & (0x3FFFu << 14));
        const int inval = k < yhl || k >= yll || (flip ? a < b : b < a);
        o->sv[j] = !inval;
        allinval &= inval;
        if (!inval) {
            const int mp = (ms >> 3) & 0xFFF, hp = (hs >> 3) & 0xFFF;
            if (mp < mxmin) mxmin = mp;
            if (mp > mxmax) mxmax = mp;
            if (hp < hxmin) hxmin = hp;
            if (hp > hxmax) hxmax = hp;
        }
        const int32_t le = flip ? hs : ms, re = flip ? ms : hs;
        o->lp[j] = le >> 3; o->lf[j] = le & 7; o->rp[j] = re >> 3; o->rf[j] = re & 7;
        if (j == ld) { o->unscrx = (int32_t)((uint32_t)xr << 4) >> 20; xfrac = (int)(((uint32_t)xr >> 8) & 0xFFu); }
    }
    o->rx = flip ? hxmin : hxmax;
    o->lx = flip ? mxmax : mxmin;
    o->len = flip ? o->lx - o->rx : o->rx - o->lx;
    o->valid = !allinval && !allover && !allunder && (!r->sc_field || (y & 1) == r->sc_odd);
    const int scdiff = flip ? o->rx - o->unscrx : o->unscrx - o->rx;
    o->scd = scdiff & 0xFFF;
    o->lodlen = o->len + o->scd;
    /* the row's attributes as the console starts its span: the value on the major edge (A + k DaDe, its low 9 bits
     * cleared), when the edge leans away from the walk moved to subscanline 3 and back up to the row's top (3/4 of DaDe
     * - DaDy, each with its low 9 bits cleared), then back to the edge's pixel by the edge's 8-bit x fraction times
     * DaDx >> 8 (its low bit cleared; nothing in copy mode), the low 10 bits of the sum cleared */
    const uint32_t k = (uint32_t)(y - (T->yh >> 2));
    const int copy = ((r->other_h >> MRDP_H_CYCLE_SHIFT) & 3) == 2;
    for (int i = 0; i < NATTR; i++) {
        const uint32_t e = (uint32_t)T->at[i].v + k * (uint32_t)T->at[i].de;
        int32_t diff = 0;
        if (ld == 3) {
            const int32_t de = (int32_t)((uint32_t)T->at[i].de & ~0x1FFu), dy = (int32_t)((uint32_t)T->dy[i] & ~0x1FFu);
            diff = (int32_t)((uint32_t)de - (uint32_t)(de >> 2) - (uint32_t)dy + (uint32_t)(dy >> 2));
        }
        const uint32_t dxh8 = copy ? 0u : (uint32_t)(T->at[i].dx >> 8) & ~1u;
        o->a[i] = (int32_t)(((e & ~0x1FFu) + (uint32_t)diff - (uint32_t)xfrac * dxh8) & ~0x3FFu);
    }
}

static void draw_triangle_x(mrdp_t *r, prim_t *pr, int aa_en, const xtri_t *T)
{
    const attr_t *at = T->at;
    const unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    const int ya = x_yhlimit(r, T->yh) >> 2, yb = x_yllimit(r, T->yl) >> 2;   /* (rows ya to yb, both included) */
    if (ya > yb) return;
    for (int k = 0; k < 3; k++) pr->xinc[k] = T->lft ? x_dx(T, AS + k) : (int32_t)(0u - (uint32_t)x_dx(T, AS + k));
    /* the coverage correction's steps: DaDx (as stepped) and DaDy of the shade >> 14, of Z >> 10 */
    for (int i = 0; i < 4; i++) {
        pr->xcd[i] = sext((uint32_t)(x_dx(T, AR + i) >> 14), 13);
        pr->xdy[i] = sext((uint32_t)(T->dy[AR + i] >> 14), 13);
    }
    pr->xcd[4] = sext((uint32_t)(at[AZ].dx >> 10), 22);
    pr->xdy[4] = sext((uint32_t)(T->dy[AZ] >> 10), 22);
    /* (the 2-cycle LOD's next pixel is a step of the walk too: DaDx as stepped, without its low 5 bits) */
    for (int k = 0; k < 3; k++) { pr->ddx[k] = pr->xinc[k]; pr->ddy[k] = T->dy[AS + k] & LOD_DY_MASK; }
    xrow_t row[2];
    x_row(r, T, ya, &row[ya & 1]);
    for (int y = ya; y <= yb; y++) {
        const xrow_t *o = &row[y & 1], *nx = &row[(y + 1) & 1];
        x_row(r, T, y + 1, &row[(y + 1) & 1]);
        pr->xnvalid = y + 1 <= yb && nx->valid;
        for (int k = 0; k < 3; k++) pr->xnrow[k] = nx->a[AS + k];
        pr->xlen = o->len; pr->xlodlen = o->lodlen;
        if (!o->valid || o->len < 0) continue;
        const int xinc = T->lft ? 1 : -1;
        for (int j = 0; j <= o->len; j++) {
            const int x = o->rx + j * xinc;
            unsigned mask = 0xFFu;
            if (cyc < 2) {
                unsigned cj[4];
                for (int q = 0; q < 4; q++) {
                    unsigned fm = (q & 1) ? 0x5u : 0xAu;
                    unsigned lm = (0xFu >> ((o->lf[q] + 1) >> 1)) & fm;
                    unsigned rm = ((0xF0u >> ((o->rf[q] + 1) >> 1)) & 0xFu) & fm;
                    cj[q] = 0;
                    if (!o->sv[q]) continue;
                    if (x > o->lp[q] && x < o->rp[q]) cj[q] = fm;
                    else if (x == o->lp[q] && x == o->rp[q]) cj[q] = lm & rm;
                    else if (o->rp[q] > o->lp[q] && x == o->lp[q]) cj[q] = lm;
                    else if (o->rp[q] > o->lp[q] && x == o->rp[q]) cj[q] = rm;
                }
                mask = ((cj[0] | cj[1]) << 4) | (cj[2] | cj[3]);
                if (!mask) continue;
                if (!aa_en && !(mask & 0x80u)) continue;
            }
            /* the pixel's attributes: the span's start stepped along the walk, first past the pixels the scissor took
             * off, then to the pixel (the shade and Z are moved to the first covered sample in draw_pixel_cvg). Copy
             * mode steps once a group of 4 pixels (8 in an 8-bit image), from the span's start as it is (the scissor
             * does not move it: a copy cut by the scissor's left edge is shifted, not cut) */
            int32_t pc[NATTR];
            pr->xj = j;
            uint32_t n = (uint32_t)o->scd + (uint32_t)j;
            if (cyc == 2) {
                const int g = r->cimg_size == 1 ? 8 : 4;
                n = (uint32_t)(j / g);
                pr->xcopy = j % g;
            }
            for (int i = 0; i < NATTR; i++) {
                const uint32_t d = T->lft ? (uint32_t)x_dx(T, i) : 0u - (uint32_t)x_dx(T, i);
                pc[i] = (int32_t)((uint32_t)o->a[i] + n * d);
            }
            draw_pixel(r, pr, x, y, pc, mask);
        }
    }
}
#endif

#ifdef MRDP_CVG
/* the LOD's steps of S, T, W (prim_t ddx, ddy) from a triangle's texture block p: DaDx toward the next pixel of the
 * N64's walk, which starts at the major edge (left to right with lft; right to left without), and DaDy (the integer
 * halves at word 10, the fractions at 14: tri_attrs's layout), kept to LOD_DY_MASK */
static void lod_tri(prim_t *pr, const uint32_t *p, const attr_t *at, int lft)
{
    for (int k = 0; k < 3; k++) {
        int wi = k >> 1, sh = (k & 1) ? 0 : 16;
        uint32_t yi = (p[10 + wi] >> sh) & 0xFFFF, yf = (p[14 + wi] >> sh) & 0xFFFF;
        pr->ddy[k] = (int32_t)(yi << 16 | yf) & LOD_DY_MASK;
        pr->ddx[k] = lft ? at[AS + k].dx : (int32_t)(0u - (uint32_t)at[AS + k].dx);
    }
}
#endif

static void draw_triangle(mrdp_t *r, const uint32_t *w)
{
    unsigned op = (w[0] >> 24) & 0x3F;
    prim_t pr = { .shade = !!(op & 4), .tex = !!(op & 2), .zbuf = !!(op & 1), .tile = (int)((w[0] >> 16) & 7) };
    int lft = (w[0] >> 23) & 1;
    int32_t yl = sext(w[0], 14), ym = sext(w[1] >> 16, 14), yh = sext(w[1], 14);
    int32_t xl = (int32_t)w[2], dxl = (int32_t)w[3];
    int32_t xh = (int32_t)w[4], dxh = (int32_t)w[5];
    int32_t xm = (int32_t)w[6], dxm = (int32_t)w[7];

    attr_t at[NATTR];
    memset(at, 0, sizeof at);
    const uint32_t *p = w + 8;
    if (pr.shade) { tri_attrs(p, at, AR, 4); p += 16; }
    if (pr.tex) {                                       /* S, T, W (4th unused) */
        tri_attrs(p, at, AS, 3);
#ifdef MRDP_CVG
        lod_tri(&pr, p, at, lft);
#endif
        p += 16;
    }
#ifdef MRDP_CVG
    pr.level = (w[0] >> 19) & 7;
#endif
    if (pr.zbuf) {
        at[AZ].v = (int32_t)p[0]; at[AZ].dx = (int32_t)p[1];
        at[AZ].de = (int32_t)p[2];
#ifdef MRDP_N64
        /* the depth a pixel's step moves it, |DzDx| + |DzDy| in the stored
         * depth's units, up to all ones under its top bit: a decal's tolerance */
        int32_t zx = (int32_t)p[1], zy = (int32_t)p[3];
        uint32_t s = (((uint32_t)(zx < 0 ? ~zx : zx) >> 15) & 0x1FFFFu) + (((uint32_t)(zy < 0 ? ~zy : zy) >> 15) & 0x1FFFFu);
        s |= s >> 1; s |= s >> 2; s |= s >> 4; s |= s >> 8; s |= s >> 16;
        pr.dz = (s & 0xFFFFu) | 1u;
        pr.dzdy = zy;
#ifdef RDP_EXACT
        pr.xdzpix = x_dzpix(zx, zy);
#endif
#endif
    }

    int y0 = yh >> 2;                 /* the integer scanline of XH / XM / A */
    int ymr = ym >> 2;                /* the integer scanline of XL */
#ifdef MRDP_N64
    /* the N64's XL is the low edge at YM itself (the middle vertex), not at
     * YM's integer scanline as XH and XM are at YH's: taken back the quarter
     * rows between them. (As if at the scanline, a shallow low edge was off
     * by up to 3/4 of its slope: cracks all along it.) */
#ifdef RDP_EXACT
    {
        /* DaDy of each attribute (the shade's and the texture's at words 10 and 14 of their blocks, Z's the 4th) */
        int32_t dy[NATTR] = { 0 };
        const uint32_t *q = w + 8;
        for (int b = 0; b < 2; b++) {
            if (!(b ? pr.tex : pr.shade)) continue;
            for (int i = 0; i < (b ? 3 : 4); i++) {
                const int wi = i >> 1, sh = (i & 1) ? 0 : 16;
                dy[(b ? AS : AR) + i] = (int32_t)((((q[10 + wi] >> sh) & 0xFFFFu) << 16) | ((q[14 + wi] >> sh) & 0xFFFFu));
            }
            q += 16;
        }
        if (pr.zbuf) dy[AZ] = (int32_t)q[3];
        /* (the console's edges: X 28 bits, the slopes 30, sign-extended) */
        const xtri_t T = { lft, yh, ym, yl, sext(w[4], 28), sext(w[5], 30), sext(w[6], 28), sext(w[7], 30), sext(w[2], 28), sext(w[3], 30), at, dy };
        draw_triangle_x(r, &pr, (r->other_l & MRDP_L_AA_EN) != 0, &T);
        return;
    }
#endif
#ifdef MRDP_CVG
    draw_triangle_aa(r, &pr, (r->other_l & MRDP_L_AA_EN) != 0, lft, yh, ym, yl, xh, dxh, xm, dxm, xl, dxl, at);
    return;
#endif
    xl = (int32_t)((uint32_t)xl - (uint32_t)(ym & 3) * (uint32_t)(dxl >> 2));
#endif
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r);
    int yb = ((yl - 2 + 3) >> 2);     /* rows with 4y+2 < yl: y <= (yl-3)/4 */
    if (yb > sc_y1(r)) yb = sc_y1(r);

    /* AA_EN without the N64's coverage: the crack grow (mrdp_grow_mask, TRI_R only) */
    int32_t gh = 0, gm = 0, gl = 0;
    if (r->other_l & MRDP_L_AA_EN) {
        if (r->grow & 1u) gh = mrdp_grow_amount(dxh);
        if (r->grow & 2u) gm = mrdp_grow_amount(dxm);
        if (r->grow & 4u) gl = mrdp_grow_amount(dxl);
    }
    for (int y = ya; y < yb; y++) {
        if (4 * y + 2 < yh) continue;
        /* all edge and attribute arithmetic wraps modulo 2^32, as the RTL's
         * incremental adders do */
        uint32_t k = (uint32_t)(y - y0);
        int32_t exh = (int32_t)((uint32_t)xh + (uint32_t)(dxh >> 1) + k * (uint32_t)dxh);
        int32_t exm = (4 * y + 2 < ym)
            ? (int32_t)((uint32_t)xm + (uint32_t)(dxm >> 1) + k * (uint32_t)dxm)
            : (int32_t)((uint32_t)xl + (uint32_t)(dxl >> 1) + (uint32_t)(y - ymr) * (uint32_t)dxl);
        int32_t xleft = lft ? exh : exm, xright = lft ? exm : exh;
        int32_t gmin = (4 * y + 2 < ym) ? gm : gl;
        int32_t gleft = lft ? gh : gmin, gright = lft ? gmin : gh;
        /* 33 bits, as the RTL (xs_c / xe_c): in 32 an edge wrapped near
         * +-32768 px overflowed and the model and the RTL disagreed */
        int64_t xs64 = ((int64_t)xleft + (0x7FFF - gleft)) >> 16, xe64 = ((int64_t)xright + (0x7FFF + gright)) >> 16;
        if (xs64 < sc_x0(r)) xs64 = sc_x0(r);
        if (xe64 > sc_x1(r)) xe64 = sc_x1(r);
        int xs = (int)xs64, xe = (int)xe64;
        if (xs >= xe) continue;

        /* span start: A + DaDe/2 + k*DaDe on the H edge, then DaDx * (xs + 0.5 - xh) */
        int32_t dx = (int32_t)(((uint32_t)xs << 16) + 0x8000) - exh;
        int32_t cur[NATTR];
        for (int i = 0; i < NATTR; i++) {
            uint32_t e = (uint32_t)at[i].v + (uint32_t)(at[i].de >> 1) + k * (uint32_t)at[i].de;
            cur[i] = (int32_t)(e + (uint32_t)(int32_t)(((int64_t)at[i].dx * dx) >> 16));
        }
        for (int x = xs; x < xe; x++) {
            draw_pixel(r, &pr, x, y, cur, 0xFFu);
            for (int i = 0; i < NATTR; i++) cur[i] = (int32_t)((uint32_t)cur[i] + (uint32_t)at[i].dx);
        }
    }
}

/* ---- rectangles ----------------------------------------------------------- */

#ifdef MRDP_N64
#define RECT_QUARTERS 1               /* the corners' quarter pixels count (outside copy and fill mode) */
#else
#define RECT_QUARTERS 0
#endif

#ifdef MRDP_CVG
/* a texture rectangle's geometry, and its S, T (s10.5) at column x of row y: draw_texrect's, for the LOD's other two
 * points (the pixel's own are worked out in place there, the same way) */
typedef struct { int flip, q, dshift, xh, x0, y0; int32_t s, t, dsdx, dtdy; } texrect_t;
static inline void texrect_st(const texrect_t *g, int x, int y, int32_t *ps, int32_t *pt)
{
    int32_t cq = g->q ? 4 * x - g->xh : x - g->x0;     /* columns from XH: quarters, or whole ones */
    int cs = g->q ? 2 : 0;
    *ps = g->flip ? g->s + ((g->dsdx * (y - g->y0)) >> g->dshift) : g->s + ((g->dsdx * cq) >> (g->dshift + cs));
    *pt = g->flip ? g->t + ((g->dtdy * cq) >> (5 + cs)) : g->t + ((g->dtdy * (y - g->y0)) >> 5);
}
#endif

#ifdef RDP_EXACT
/* A rectangle as the console draws it (docs/rdp_exact.md, rule 6): a primitive of its own walked as a left-major
 * triangle with vertical edges at XH and XL (their quarter pixels kept), YL taken to its row's last subscanline in fill
 * and copy mode, level count 0. A texture rectangle's S and T are s10.5 in the integer halves; the column-stepped one
 * steps by DsDx (DtDy flipped) along x, the row-stepped one by DtDy (DsDx) a row, DaDe and DaDy alike (so the LOD's next
 * line is a triangle's: the step kept to LOD_DY_MASK). Its pixels are then the walk's: partial coverage at a
 * fractional edge (the top-left sample decides without AA_EN, as for a triangle), the scissor's last column in fill and
 * copy mode, and the console's texel coordinates. */
static void draw_rect_x(mrdp_t *r, const uint32_t *w, int tex, int flip)
{
    const unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    int32_t xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF, xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    if (cyc >= 2) yl |= 3;
    prim_t pr = { .tex = tex, .tile = tex ? (int)((w[1] >> 24) & 7) : 0, .fillq = !tex };
    attr_t at[NATTR];
    int32_t dy[NATTR] = { 0 };
    memset(at, 0, sizeof at);
    if (tex) {
        const int32_t s = sext(w[2] >> 16, 16), t = sext(w[2], 16), dsdx = sext(w[3] >> 16, 16), dtdy = sext(w[3], 16);
        at[AS].v = (int32_t)((uint32_t)s << 16); at[AT].v = (int32_t)((uint32_t)t << 16);
        const int c = flip ? AT : AS, ro = flip ? AS : AT;            /* the coordinate stepped along x, and down */
        at[c].dx = (int32_t)((uint32_t)(flip ? dtdy : dsdx) << 11);
        at[ro].de = dy[ro] = (int32_t)((uint32_t)(flip ? dsdx : dtdy) << 11);
    }
    const int32_t exl = (int32_t)(((uint32_t)(xl >> 2) << 16) | ((uint32_t)(xl & 3) << 14));
    const int32_t exh = (int32_t)(((uint32_t)(xh >> 2) << 16) | ((uint32_t)(xh & 3) << 14));
    const xtri_t T = { 1, yh, yl, yl, exh, 0, exl, 0, exl, 0, at, dy };
    draw_triangle_x(r, &pr, (r->other_l & MRDP_L_AA_EN) != 0, &T);
}
#endif
static void draw_texrect(mrdp_t *r, const uint32_t *w, int flip)
{
#ifdef RDP_EXACT
    draw_rect_x(r, w, 1, flip);
    return;
#endif
    unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    int xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF;
    int xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    prim_t pr = { .tex = 1, .tile = (int)((w[1] >> 24) & 7), .texrect = 1 };
    int32_t s = sext(w[2] >> 16, 16), t = sext(w[2], 16);
    int32_t dsdx = sext(w[3] >> 16, 16), dtdy = sext(w[3], 16);
    /* Copy mode: the corners' whole pixels, the lower-right one included. Otherwise (MRDP_N64) a pixel is drawn where
     * its top-left sample is inside: 4x in [XH, XL) and 4y in [YH, YL) in quarter pixels -- a fractional corner leaves
     * out the pixel it falls in at the upper left and takes it in at the lower right. The row-stepped attribute starts
     * at YH's whole row; the column-stepped one is the plane at XH itself: at a column x it is the value plus its
     * delta times (4x - XH) / 4 (in copy mode, with no fraction, at XH's whole column). (MRDP's own: the corners'
     * whole pixels, the lower-right one excluded) */
    int q = (cyc != 2) && RECT_QUARTERS, up = q ? 3 : 0;
    int x0 = xh >> 2, x1 = (cyc == 2) ? (xl >> 2) + 1 : (xl + up) >> 2;
    int y0 = yh >> 2, y1 = (cyc == 2) ? (yl >> 2) + 1 : (yl + up) >> 2;
    int dshift = (cyc == 2) ? 7 : 5;              /* copy mode: DsDx is 4x */
    int xa = (xh + up) >> 2, ya = (yh + up) >> 2;
    if (xa < sc_x0(r)) xa = sc_x0(r);
    if (ya < sc_y0(r)) ya = sc_y0(r);
    int xb = x1 < sc_x1(r) ? x1 : sc_x1(r), yb = y1 < sc_y1(r) ? y1 : sc_y1(r);
#ifdef MRDP_CVG
    const texrect_t g = { flip, q, dshift, xh, x0, y0, s, t, dsdx, dtdy };
#endif
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            int32_t at[NATTR] = { 0 };
            int32_t cq = q ? 4 * x - xh : x - x0;     /* columns from XH: quarters, or whole ones */
            int cs = q ? 2 : 0;
            at[AS] = flip ? s + ((dsdx * (y - y0)) >> dshift) : s + ((dsdx * cq) >> (dshift + cs));
            at[AT] = flip ? t + ((dtdy * cq) >> (5 + cs)) : t + ((dtdy * (y - y0)) >> 5);
#ifdef MRDP_CVG
            /* the LOD's other two points: the N64 walks a rectangle left to right (and its level count is 0) */
            texrect_st(&g, x + 1, y, &pr.rnx[0], &pr.rnx[1]);
            texrect_st(&g, x, y + 1, &pr.rny[0], &pr.rny[1]);
#endif
            draw_pixel(r, &pr, x, y, at, 0xFFu);
        }
}

static void fill_rect(mrdp_t *r, const uint32_t *w)
{
#ifdef RDP_EXACT
    draw_rect_x(r, w, 0, 0);
    return;
#endif
    unsigned cyc = (r->other_h >> MRDP_H_CYCLE_SHIFT) & 3;
    int xl = (w[0] >> 12) & 0xFFF, yl = w[0] & 0xFFF;
    int xh = (w[1] >> 12) & 0xFFF, yh = w[1] & 0xFFF;
    /* (fill mode: whole pixels, the lower-right one included; otherwise as a texture rectangle's, above) */
    int up = (cyc != 3) && RECT_QUARTERS ? 3 : 0;
    int x0 = (xh + up) >> 2, y0 = (yh + up) >> 2;
    int x1 = (cyc == 3) ? (xl >> 2) + 1 : (xl + up) >> 2;
    int y1 = (cyc == 3) ? (yl >> 2) + 1 : (yl + up) >> 2;
    prim_t pr = { .fillq = 1 };
    int xa = x0 > sc_x0(r) ? x0 : sc_x0(r), xb = x1 < sc_x1(r) ? x1 : sc_x1(r);
    int ya = y0 > sc_y0(r) ? y0 : sc_y0(r), yb = y1 < sc_y1(r) ? y1 : sc_y1(r);
    int32_t at[NATTR] = { 0 };
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++)
            draw_pixel(r, &pr, x, y, at, 0xFFu);
}

/* ---- texture loads ---------------------------------------------------------- */

static void set_tile_size(mrdp_t *r, const uint32_t *w)
{
    mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    t->uls = (w[0] >> 12) & 0xFFF;
    t->ult = w[0] & 0xFFF;
    t->lrs = (w[1] >> 12) & 0xFFF;
    t->lrt = w[1] & 0xFFF;
}

#ifndef MRDP_N64
static void load_tile(mrdp_t *r, const uint32_t *w)
{
    set_tile_size(r, w);
    const mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    int s0 = t->uls >> 2, t0 = t->ult >> 2, s1 = t->lrs >> 2, t1 = t->lrt >> 2;
    for (int tt = t0; tt <= t1; tt++)
        for (int s = s0; s <= s1; s++) {
            uint16_t v = r->rd16(r->ctx, r->timg + ((uint32_t)tt * r->timg_width + (uint32_t)s) * 2);
            int bank, addr;
            tmem_locate(t, s - s0, tt - t0, &bank, &addr);
            r->tmem[bank][addr] = v;
        }
}
#endif

#ifdef MRDP_N64
/* The N64's loads: the texture image, as it is in memory, into TMEM, in whole
 * 64-bit words (a row's last word takes what follows the row in memory, as
 * the N64's does). The image is read by bytes: a row may start at any.
 * The halves of an odd row's 64-bit words change places as they are loaded
 * (tmem_fetch: and again as they are sampled). A row is odd by the load's own
 * count: LOAD TILE's rows, LOAD BLOCK's dxt summed over its words. */
static inline uint16_t timg_hw(mrdp_t *r, uint32_t a)
{
    uint16_t v0 = r->rd16(r->ctx, a & ~1u);
    if (!(a & 1u)) return v0;
    return (uint16_t)(v0 << 8 | r->rd16(r->ctx, (a & ~1u) + 2u) >> 8);
}
/* n 32-bit words from src to TMEM's 16-bit word dst (a multiple of 4): one
 * after the other, or (a 32-bit RGBA tile) each word's halves to the two
 * halves of TMEM, or (a 16-bit YUV tile) a pixel pair's U Y0 V Y1 as U V to the
 * lower half and Y0 Y1 to the upper. dxt: LOAD BLOCK's, what a 64-bit word adds to the row count
 * (u1.11); odd: the row is odd throughout (LOAD TILE's). */
static void tmem_load(mrdp_t *r, const mrdp_tile_t *t, uint32_t src, uint32_t dst, uint32_t n, uint32_t dxt, int odd)
{
    const int yuv = t->fmt == 1 && t->size == 2;
    const int split = (t->fmt == 0 && t->size == 3) || yuv;
    for (uint32_t k = 0; k < n; k++) {
        uint16_t h0 = timg_hw(r, src + 4u * k), h1 = timg_hw(r, src + 4u * k + 2u);
        uint32_t j = k >> 1;                         /* (the 64-bit word of the image this is of) */
        uint32_t sw = (odd || (((j * dxt) >> 11) & 1u)) ? 2u : 0u;
        if (split) {
            uint32_t i = ((dst + k) ^ sw) & 0x3FF;
            if (yuv) {
                r->tmem16[i] = (uint16_t)((h0 & 0xFF00u) | h1 >> 8);
                r->tmem16[0x400 | i] = (uint16_t)(h0 << 8 | (h1 & 0xFFu));
            } else {
                r->tmem16[i] = h0; r->tmem16[0x400 | i] = h1;
            }
        } else {
            uint32_t i = ((dst + 2u * k) ^ sw) & 0x7FE;
            r->tmem16[i] = h0; r->tmem16[i | 1u] = h1;
        }
    }
}
/* the 64-bit words n texels of the image take, as 32-bit ones */
static inline uint32_t load_words(const mrdp_t *r, uint32_t n)
{
    uint32_t bytes = ((n << r->timg_size) + 1u) >> 1;
    uint32_t w = ((bytes + 7u) >> 3) * 2u;
    return w > 1024u ? 1024u : w;                    /* TMEM once over */
}
static inline uint32_t timg_addr(const mrdp_t *r, uint32_t s, uint32_t t)
{
    return r->timg + (((t * r->timg_width + s) << r->timg_size) >> 1);
}
/* LOAD BLOCK: texels uls .. lrs of the texture image (a run, not a rectangle), to the tile's TMEM address */
static void load_block(mrdp_t *r, const uint32_t *w)
{
    const mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    uint32_t uls = (w[0] >> 12) & 0xFFF, ult = w[0] & 0xFFF, lrs = (w[1] >> 12) & 0xFFF;
    if (lrs < uls) return;
    tmem_load(r, t, timg_addr(r, uls, ult), (uint32_t)t->tmem * 4u, load_words(r, lrs - uls + 1u), w[1] & 0xFFFu, 0);
}
/* LOAD TLUT: 16-bit entries uls .. lrs of the texture image, each four times over (a 64-bit word of TMEM) */
static void load_tlut(mrdp_t *r, const uint32_t *w)
{
    const mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    uint32_t uls = ((w[0] >> 12) & 0xFFF) >> 2, ult = (w[0] & 0xFFF) >> 2, lrs = ((w[1] >> 12) & 0xFFF) >> 2;
    if (lrs < uls) return;
    uint32_t n = lrs - uls + 1u, src = r->timg + ((ult * r->timg_width + uls) << 1);
    if (n > 256u) n = 256u;
    for (uint32_t e = 0; e < n; e++) {
        uint16_t v = timg_hw(r, src + 2u * e);
        for (uint32_t k = 0; k < 4; k++) r->tmem16[((uint32_t)t->tmem * 4u + 4u * e + k) & 0x7FF] = v;
    }
}
/* LOAD TILE: a rectangle of the texture image, row by row at the tile's row length */
static void load_tile_n64(mrdp_t *r, const uint32_t *w)
{
    const mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    uint32_t s0 = t->uls >> 2, t0 = t->ult >> 2, s1 = t->lrs >> 2, t1 = t->lrt >> 2;
    if (s1 < s0 || t1 < t0) return;
    for (uint32_t tt = t0; tt <= t1; tt++)
        tmem_load(r, t, timg_addr(r, s0, tt), (uint32_t)t->tmem * 4u + (tt - t0) * (uint32_t)t->line * 4u,
                  load_words(r, s1 - s0 + 1u), 0, (int)((tt - t0) & 1u));
}
#endif

static void set_tile(mrdp_t *r, const uint32_t *w)
{
    mrdp_tile_t *t = &r->tile[(w[1] >> 24) & 7];
    t->fmt = (w[0] >> 21) & 7;
    t->line = (w[0] >> 9) & 0x1FF;
#ifdef MRDP_N64
    t->size = (w[0] >> 19) & 3;
    t->palette = (w[1] >> 20) & 15;
    t->tmem = (uint16_t)(w[0] & 0x1FF);
    t->ct = (w[1] >> 19) & 1; t->mt = (w[1] >> 18) & 1;
    t->maskt = (w[1] >> 14) & 15; t->shiftt = (w[1] >> 10) & 15;
    t->cs = (w[1] >> 9) & 1; t->ms = (w[1] >> 8) & 1;
    t->masks = (w[1] >> 4) & 15; t->shifts = w[1] & 15;
    return;
#endif
    t->tmem = (uint16_t)((w[0] & 0x1FF) | (((w[0] >> 18) & 1) << 9));
    t->ct = (w[1] >> 19) & 1; t->mt = (w[1] >> 18) & 1;
    t->maskt = (w[1] >> 14) & 15; t->shiftt = (w[1] >> 10) & 15;
    t->cs = (w[1] >> 9) & 1; t->ms = (w[1] >> 8) & 1;
    t->masks = (w[1] >> 4) & 15; t->shifts = w[1] & 15;
}

/* ---- command dispatch --------------------------------------------------------- */

/* TRI_V / TRI_G (mrdp_setup.h): the attributes' value, DaDx and DaDe, from
 * the vertex values as the RTL computes them (mrdp_vattr) or as sent, then
 * the N64 triangle they make. (DaDy is not used by the walker.) */
#ifndef MRDP_N64
static void draw_triangle_vg(mrdp_t *r, const uint32_t *w, int g)
{
    unsigned flags = (w[0] >> 24) & 7u;
    int32_t F[4] = { (int32_t)w[8], (int32_t)w[9], (int32_t)w[10], (int32_t)w[11] };
    int32_t kx = (int32_t)w[12], ky = (int32_t)w[13], dxh = (int32_t)w[5];
    int32_t val[8], ddx[8], dde[8], ddy[8];
    const uint32_t *a = w + (g ? 8 : 14);
    for (int i = 0; i < 8; i++) {
        val[i] = ddx[i] = dde[i] = ddy[i] = 0;
        if (!mrdp_attr_on(flags, i)) continue;
        if (g) { val[i] = (int32_t)a[0]; ddx[i] = (int32_t)a[1]; dde[i] = (int32_t)a[2]; }
        else mrdp_vattr(F, kx, ky, dxh, (int32_t)a[0], (int32_t)a[1], (int32_t)a[2], &val[i], &ddx[i], &dde[i], &ddy[i]);
        a += 3;
    }
    uint32_t t[44];
    t[0] = ((0x08u | flags) << 24) | (w[0] & 0x00FFFFFFu);
    for (int k = 1; k < 8; k++) t[k] = w[k];
    mrdp_put_attrs(t, 8, flags, val, ddx, dde, ddy);
    draw_triangle(r, t);
}
#endif

/* TRI_R (mrdp_setup.h): MRDP's whole setup, mrdp_tri_r() + mrdp_tri_r_attr() */
#ifndef MRDP_N64
static void draw_triangle_r(mrdp_t *r, const uint32_t *w)
{
    unsigned flags = (w[0] >> 19) & 7u, tile = (w[0] >> 16) & 7u;
    int32_t xy[6], kx, ky;
    for (int k = 0; k < 6; k++) xy[k] = (int32_t)w[1 + k];
    mrdp_edges_t E;
    mrdp_fac_t fc;
    if (!mrdp_tri_r(xy, &E, &fc, &kx, &ky)) return;
    r->grow = mrdp_grow_mask(&E);
    int32_t av[8][3] = { { 0 } };
    const uint32_t *a = w + 7;
    for (int i = 0; i < 8; i++) {
        if (!mrdp_attr_on(flags, i)) continue;
        av[i][0] = (int32_t)a[0]; av[i][1] = (int32_t)a[1]; av[i][2] = (int32_t)a[2];
        a += 3;
    }
    mrdp_tri_r_fields(av, flags);
    int32_t val[8], ddx[8], dde[8], ddy[8];
    for (int i = 0; i < 8; i++) {
        val[i] = ddx[i] = dde[i] = ddy[i] = 0;
        if (!mrdp_attr_on(flags, i)) continue;
        mrdp_tri_r_attr(&fc, kx, ky, E.dxh, av[i][0], av[i][1], av[i][2], &val[i], &ddx[i], &dde[i], &ddy[i]);
    }
    uint32_t t[44];
    mrdp_put_edges(t, &E, (0x08u | flags) << 24, tile);
    mrdp_put_attrs(t, 8, flags, val, ddx, dde, ddy);
    draw_triangle(r, t);
}
#endif

static void execute(mrdp_t *r, const uint32_t *w)
{
    unsigned op = (w[0] >> 24) & 0x3F;
    r->grow = 0;                                  /* TRI_R sets it (draw_triangle_r) */
#ifdef MRDP_N64
#ifdef MRDP_CVG
    /* drawn to a 16-bit colour image, an 8-bit one (wr8) and a 32-bit one -- but not a 32-bit one in copy mode, which the
     * console does not do (the manual: "You cannot copy the 32-bit RGBA color image type"); a 4-bit one: nothing */
    const int cimg_ok = r->cimg_size == 2 || r->cimg_size == 1
                        || (r->cimg_size == 3 && ((r->other_h >> MRDP_H_CYCLE_SHIFT) & 3) != 2);
    if (!cimg_ok && ((op >= 0x08 && op <= 0x0F) || op == MRDP_OP_TEXRECT || op == MRDP_OP_TEXRECT_FLIP || op == MRDP_OP_FILL_RECT)) return;
#else
    /* drawn to a 16-bit colour image and an 8-bit one (wr8); a 32-bit one: nothing */
    if (r->cimg_size != 2 && r->cimg_size != 1 && ((op >= 0x08 && op <= 0x0F) || op == MRDP_OP_TEXRECT || op == MRDP_OP_TEXRECT_FLIP
                              || op == MRDP_OP_FILL_RECT)) return;
#endif
#endif
    if (op >= 0x08 && op <= 0x0F) { draw_triangle(r, w); return; }
#ifndef MRDP_N64
    if (op >= MRDP_OP_TRI_V && op <= (MRDP_OP_TRI_V | 7u)) { draw_triangle_vg(r, w, 0); return; }
    if (op >= MRDP_OP_TRI_G && op <= (MRDP_OP_TRI_G | 7u)) { draw_triangle_vg(r, w, 1); return; }
    if (op == MRDP_OP_TRI_R) { draw_triangle_r(r, w); return; }
#endif
    switch (op) {
    case MRDP_OP_NOP: case MRDP_OP_SYNC_LOAD: case MRDP_OP_SYNC_PIPE: case MRDP_OP_SYNC_TILE:
        break;
    case MRDP_OP_SYNC_FULL: r->sync_count++; break;
#ifdef MRDP_N64
    case MRDP_OP_SET_CONVERT:                     /* K0..K3 signed (a YUV texel's conversion); K4, K5 the combiner's */
        r->kc[0] = (int16_t)sext((w[0] >> 13) & 0x1FF, 9);
        r->kc[1] = (int16_t)sext((w[0] >> 4) & 0x1FF, 9);
        r->kc[2] = (int16_t)sext(((w[0] & 15u) << 5) | (w[1] >> 27), 9);
        r->kc[3] = (int16_t)sext((w[1] >> 18) & 0x1FF, 9);
        r->k4 = (uint16_t)((w[1] >> 9) & 0x1FF); r->k5 = (uint16_t)(w[1] & 0x1FF);
        break;
#endif
    case MRDP_OP_TEXRECT: draw_texrect(r, w, 0); break;
    case MRDP_OP_TEXRECT_FLIP: draw_texrect(r, w, 1); break;
    case MRDP_OP_SET_SCISSOR:
        r->sc_xh = (w[0] >> 12) & 0xFFF; r->sc_yh = w[0] & 0xFFF;
        r->sc_xl = (w[1] >> 12) & 0xFFF; r->sc_yl = w[1] & 0xFFF;
#ifdef RDP_EXACT
        r->sc_field = (uint8_t)((w[1] >> 25) & 1u); r->sc_odd = (uint8_t)((w[1] >> 24) & 1u);
#endif
        break;
#ifdef MRDP_N64
    case MRDP_OP_SET_PRIM_Z: r->prim_z = (uint16_t)((w[1] >> 16) << 1); r->prim_dz = (uint16_t)w[1]; break;   /* the N64's: 15 bits, and its delta */
#else
    case MRDP_OP_SET_PRIM_Z: r->prim_z = (uint16_t)(w[1] >> 16); break;
#endif
    case MRDP_OP_SET_OTHER: r->other_h = w[0] & 0xFFFFFF; r->other_l = w[1]; break;
    case MRDP_OP_SET_TILESIZE: set_tile_size(r, w); break;
#ifdef MRDP_N64
    case MRDP_OP_LOAD_TILE: set_tile_size(r, w); load_tile_n64(r, w); r->load_count++; break;
    case MRDP_OP_LOAD_BLOCK: load_block(r, w); r->load_count++; break;
    case MRDP_OP_LOAD_TLUT: load_tlut(r, w); r->load_count++; break;
#else
    case MRDP_OP_LOAD_TILE: load_tile(r, w); r->load_count++; break;
#endif
    case MRDP_OP_SET_TILE: set_tile(r, w); break;
    case MRDP_OP_FILL_RECT: fill_rect(r, w); break;
    case MRDP_OP_SET_FILL: r->fill = w[1]; break;
    case MRDP_OP_SET_FOG: r->fog = rgba_of(w[1]); break;
    case MRDP_OP_SET_BLEND: r->blend = rgba_of(w[1]); break;
    case MRDP_OP_SET_PRIM: r->prim = rgba_of(w[1]); r->prim_lod_frac = (uint8_t)w[0];
#ifdef MRDP_CVG
        r->min_level = (uint8_t)((w[0] >> 8) & 31);
#endif
        break;
    case MRDP_OP_SET_ENV: r->env = rgba_of(w[1]); break;
    case MRDP_OP_SET_COMBINE: r->combine_hi = w[0] & 0xFFFFFF; r->combine_lo = w[1]; break;
#ifdef MRDP_N64
#endif
    case MRDP_OP_SET_TIMG: r->timg = w[1]; r->timg_width = (w[0] & 0x3FF) + 1; r->timg_size = (w[0] >> 19) & 3; break;
    case MRDP_OP_SET_ZIMG: r->zimg = w[1]; break;
    case MRDP_OP_SET_CIMG: r->cimg = w[1]; r->cimg_width = (w[0] & 0x3FF) + 1; r->cimg_size = (w[0] >> 19) & 3; break;
    default: r->unknown_ops++; break;
    }
}

void mrdp_push(mrdp_t *r, uint32_t word)
{
    if (r->ncmd == 0)
        r->need = 2 * mrdp_cmd_len(word);
    r->cmd[r->ncmd++] = word;
    if (r->ncmd == r->need) {
        execute(r, r->cmd);
        r->ncmd = 0;
    }
}
