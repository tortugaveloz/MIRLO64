/* The geom core's MRDP back end: render state, texture loads
 * and triangles as N64 RDP commands. Shared by the geom core and the host
 * simulator (GEOM_HOST_TEST), which feeds the words to the bit-exact model
 * (lang/c/mrdp/mrdp.c) -- the same words the hardware gets.
 *
 * What the GDL carries is mapped onto the N64 modes:
 *   GDL_TEXENV rgb/alpha modes     -> SET_COMBINE (both cycles alike)
 *   GDL_RENDERMODE ZCMP/ZUPD/DECAL/XLU, the ortho class, the texel-alpha
 *   test                           -> SET_OTHER_MODES (+ blend color alpha)
 *   GDL_TEXBIND                    -> decode into SDRAM, SET_TIMG, LOAD_TILE
 *                                     via tile 7, render tile 0
 */
#ifndef GEOM_MRDP_H
#define GEOM_MRDP_H

#include <stdint.h>
#include "../mrdp/mrdp.h"
#include "geom_fixed.h"
/* the setup's reciprocals on the CFU (fx_recip_norm: RECIPN, 2^-19.8, ~7
 * cycles; its C on the host is bit-exact): for d in [2^31, 2^32) it gives
 * ~2^54 / d. Newton in C was ~120 cycles each, four a triangle. */
#define MRDP_RECIP_HOOK(d) (fx_recip_norm(d).r << 8)
#ifndef GEOM_HOST_TEST
/* one out-of-line copy of the edges (ROM B, geom.ld): inlined into
 * mrdp_emit_fx and the generic path it overflowed ROM B by 356 bytes */
#define MRDP_EDGES_ATTR __attribute__((noinline))
#endif
#include "../mrdp/mrdp_setup.h"

/* ---- transport ------------------------------------------------------------ */
#ifdef GEOM_HOST_TEST
void geom_test_mrdp(const uint32_t *w, unsigned n);   /* the simulator's model */
void geom_test_mrdp_mem(uint32_t addr, uint32_t bytes); /* the geom wrote SDRAM (capture) */
extern uint8_t *g_geom_host_sdram;                    /* 64 MiB at 0x40000000 */
static inline void mrdp_out(const uint32_t *w, unsigned n) { geom_test_mrdp(w, n); }
static inline void *mrdp_sdram_ptr(uint32_t a) { return g_geom_host_sdram + (a - 0x40000000u); }
/* a GDL's texture source: a host pointer, or (GDLs built for the target,
 * e.g. sim/geom_full's mkgdl) a SoC address inside the host's SDRAM copy */
static inline const void *mrdp_src_ptr(uintptr_t a)
{
    return (a >= 0x40000000u && a < 0x44000000u) ? (const void *)mrdp_sdram_ptr((uint32_t)a) : (const void *)a;
}
/* word pairs into a command, then sent (the target's APPEND2 + PUSH) */
static uint32_t s_mrdp_wb[48];
static unsigned s_mrdp_wn;
static inline void mrdp_w2(uint32_t a, uint32_t b) { s_mrdp_wb[s_mrdp_wn++] = a; s_mrdp_wb[s_mrdp_wn++] = b; }
static inline void mrdp_go(void) { geom_test_mrdp(s_mrdp_wb, s_mrdp_wn); s_mrdp_wn = 0; }
#else
void geom_mrdp_out(const uint32_t *w, unsigned n);    /* geom_mrdp_hw.c */
static inline void mrdp_out(const uint32_t *w, unsigned n) { geom_mrdp_out(w, n); }
#include "geom_vpar.h"
/* word pairs straight into the CFU's output buffer (APPEND2), then PUSHed:
 * one CFU op for two words, no array in between */
static unsigned s_mrdp_wn;
static inline void mrdp_w2(uint32_t a, uint32_t b) { geom_setup_append2(a, b); s_mrdp_wn += 2; }
static inline void mrdp_go(void) { geom_setup_push(s_mrdp_wn); s_mrdp_wn = 0; }
static inline void *mrdp_sdram_ptr(uint32_t a) { return (void *)(uintptr_t)a; }
static inline const void *mrdp_src_ptr(uintptr_t a) { return (const void *)a; }
#endif

static inline void mrdp_cmd(uint32_t hi, uint32_t lo)
{
    mrdp_w2(hi, lo);
    mrdp_go();
}

/* ---- render state ----------------------------------------------------------- */

/* N64 combiner inputs used here */
#define MRDP_CC_TEX0    1u
#define MRDP_CC_SHADE   4u
#define MRDP_CC_ZERO_A  15u   /* sub A / sub B: 8..15 = 0 */
#define MRDP_CC_ZERO_C  31u   /* mul: 16..31 = 0 */
#define MRDP_CC_TEX0_A  8u    /* mul: texel 0 alpha */
#define MRDP_CC_ZERO_D  7u    /* add: 7 = 0 */
#define MRDP_CCA_TEX0   1u
#define MRDP_CCA_SHADE  4u
#define MRDP_CCA_ZERO   7u

static inline void mrdp_combine_words(unsigned a, unsigned b, unsigned c, unsigned d,
                                      unsigned Aa, unsigned Ab, unsigned Ac, unsigned Ad,
                                      uint32_t *hi, uint32_t *lo)
{
    *hi = (MRDP_OP_SET_COMBINE << 24) | (a << 20) | (c << 15) | (Aa << 12) | (Ac << 9) | (a << 5) | c;
    *lo = (b << 28) | (b << 24) | (Aa << 21) | (Ac << 18) | (d << 15) | (Ab << 12) | (Ad << 9)
        | (d << 6) | (Ab << 3) | Ad;
}

/* rgb: GDL_TE_RGB_*, alpha: GDL_TE_A_* >> 2; tex = a texture is bound */
static inline void mrdp_combine_of(unsigned te, int tex, uint32_t *hi, uint32_t *lo)
{
    unsigned rgb = tex ? (te & 3u) : 3u, al = tex ? ((te >> 2) & 3u) : 1u;
    unsigned a = MRDP_CC_ZERO_A, b = MRDP_CC_ZERO_A, c = MRDP_CC_ZERO_C, d = MRDP_CC_SHADE;
    switch (rgb) {
    case 0: a = MRDP_CC_TEX0; c = MRDP_CC_SHADE; d = MRDP_CC_ZERO_D; break;                    /* modulate */
    case 1: a = MRDP_CC_TEX0; b = MRDP_CC_SHADE; c = MRDP_CC_TEX0_A; d = MRDP_CC_SHADE; break; /* lerp */
    case 2: d = MRDP_CC_TEX0; break;                                                          /* texel */
    default: break;                                                                           /* shade */
    }
    unsigned Aa = MRDP_CCA_ZERO, Ab = MRDP_CCA_ZERO, Ac = MRDP_CCA_ZERO, Ad = MRDP_CCA_SHADE;
    switch (al) {
    case 0: Aa = MRDP_CCA_TEX0; Ac = MRDP_CCA_SHADE; Ad = MRDP_CCA_ZERO; break;   /* modulate */
    case 2: Ad = MRDP_CCA_TEX0; break;                                            /* texel */
    default: break;                                                               /* shade */
    }
    mrdp_combine_words(a, b, c, d, Aa, Ab, Ac, Ad, hi, lo);
}

typedef struct {
    uint32_t other_h, other_l, cc_hi, cc_lo, blend, prim, prim_lf, env;
    int valid;
} mrdp_state_t;

static mrdp_state_t s_mrdp_st;

/* the texture TMEM holds (a bind of the same one right after is free) */
static uintptr_t s_mrdp_tex_src, s_mrdp_tex_tlut;
static uint32_t s_mrdp_tex_wh, s_mrdp_tex_p0, s_mrdp_tex_ext;

/* forget what was emitted (a new display list): nothing carries over from
 * one frame to the next, so a frame's command stream stands on its own */
#ifndef GEOM_HOST_TEST
void geom_mrdp_hw_list_start(void);
#endif
static inline void mrdp_state_invalidate(void)
{
    s_mrdp_st.valid = 0;
    s_mrdp_tex_src = 0; s_mrdp_tex_ext = 0;
#ifndef GEOM_HOST_TEST
    geom_mrdp_hw_list_start();
#endif
}

#define MRDP_ALPHA_TEST_REF 0x4Du   /* alpha test: GEQUAL 0x4D */

/* grow: the crack grow on (geom_pipeline.c s_crack_level != 0; MRDP's
 * amount is fixed, mrdp_setup.h mrdp_grow_amount) */
/* raw: GDL_COMBINE's words (combine w0, w1, prim, env) and PRIM's LOD fraction, or 0 */
static inline void mrdp_set_state(int depth, int zupd, int decal, int blend, int alpha_test,
                                  int tex, int point, unsigned texenv, unsigned grow, const uint32_t *raw)
{
    uint32_t h = MRDP_H_PERSP | (point ? 0u : (2u << MRDP_H_FILT_SHIFT));
    /* AA_EN (the crack grow) on opaque, depth-writing surfaces only: not
     * decal, not blended (and so not 2D: no depth there) */
    uint32_t l = (depth ? MRDP_L_Z_CMP : 0u) | (zupd ? MRDP_L_Z_UPD : 0u)
               | ((zupd && !decal && !blend && grow) ? MRDP_L_AA_EN : 0u)
               | (decal ? (3u << MRDP_L_ZMODE_SHIFT) : 0u)
               | (alpha_test ? MRDP_L_ALPHA_CMP : 0u);
    if (blend)   /* P = pixel, A = combined alpha, M = memory, B = 1 - A, both cycles */
        l |= MRDP_L_FORCE_BL | MRDP_L_IM_RD | (1u << 22) | (1u << 20);
    uint32_t cc_hi, cc_lo;
    if (raw) {
        cc_hi = (MRDP_OP_SET_COMBINE << 24) | (raw[0] & 0xFFFFFFu);
        cc_lo = raw[1];
        if (!s_mrdp_st.valid || raw[2] != s_mrdp_st.prim || raw[4] != s_mrdp_st.prim_lf)
            mrdp_cmd((MRDP_OP_SET_PRIM << 24) | (raw[4] & 0xFFu), raw[2]);
        if (!s_mrdp_st.valid || raw[3] != s_mrdp_st.env) mrdp_cmd(MRDP_OP_SET_ENV << 24, raw[3]);
        s_mrdp_st.prim = raw[2]; s_mrdp_st.prim_lf = raw[4]; s_mrdp_st.env = raw[3];
    } else {
        mrdp_combine_of(texenv, tex, &cc_hi, &cc_lo);
    }
    uint32_t bl = alpha_test ? MRDP_ALPHA_TEST_REF : 0u;

    if (!s_mrdp_st.valid || h != s_mrdp_st.other_h || l != s_mrdp_st.other_l)
        mrdp_cmd((MRDP_OP_SET_OTHER << 24) | h, l);
    if (!s_mrdp_st.valid || cc_hi != s_mrdp_st.cc_hi || cc_lo != s_mrdp_st.cc_lo)
        mrdp_cmd(cc_hi, cc_lo);
    if (!s_mrdp_st.valid || bl != s_mrdp_st.blend)
        mrdp_cmd(MRDP_OP_SET_BLEND << 24, bl);
    s_mrdp_st.other_h = h; s_mrdp_st.other_l = l;
    s_mrdp_st.cc_hi = cc_hi; s_mrdp_st.cc_lo = cc_lo;
    s_mrdp_st.blend = bl;
    s_mrdp_st.valid = 1;
}

/* ---- textures --------------------------------------------------------------- */

static inline unsigned mrdp_log2_exact(uint32_t n)
{
    unsigned lg = 0;
    while ((1u << lg) < n) lg++;
    return (1u << lg) == n ? lg : 0u;   /* 0: not a power of two (clamp only) */
}

/* Decoded textures are staged in SDRAM and copied into TMEM by LOAD_TILE,
 * in command order. A slot may only be rewritten once the LOAD_TILE that
 * reads it has run: the host model runs each command as it arrives; the
 * hardware counts completed loads (mrdp.load_count) and the target's
 * mrdp_tex_slot() waits on it (geom_mrdp_hw.c). */
#define MRDP_TEX_STAGING     0x41200000u
#define MRDP_TEX_SLOTS       16u
#define MRDP_TEX_SLOT_TEXELS 4096u              /* 8 KiB: 64x64 */
#ifdef GEOM_HOST_TEST
static inline uint32_t mrdp_tex_slot(void)
{
    static uint32_t n;
    return MRDP_TEX_STAGING + (n++ % MRDP_TEX_SLOTS) * (MRDP_TEX_SLOT_TEXELS * 2u);
}
static inline void mrdp_load_issued(void) { }
static inline void mrdp_store_barrier(void) { }
static inline void mrdp_loads_wait(void) { }    /* the model runs each LOAD as it arrives */
#else
uint32_t mrdp_tex_slot(void);                   /* geom_mrdp_hw.c */
void mrdp_load_issued(void);                    /* every LOAD TILE, counted */
void mrdp_loads_wait(void);                     /* until every LOAD issued has run */
void mrdp_store_barrier(void);                  /* SDRAM stores done before what follows */
#endif

/* Decoded-texture cache: decoding every bind again was 27 % of the geom core's time on a game frame. A bind of
 * a resident texture is only its LOAD TILE. Entries of 8 KiB (64x64 16-bit,
 * the largest a bind takes) in SDRAM 0x41400000..0x41600000, an
 * open-addressed table after them keyed by everything the decode depends on.
 * Full: wait until every LOAD has run (none reads a page any more), start
 * over -- once per level change in practice. */
#define MRDP_TEXC_BASE     0x41400000u
#define MRDP_TEXC_TAB      0x41600000u
#define MRDP_TEXC_ENTRIES  256u
#define MRDP_TEXC_TAB_N    2048u
#define MRDP_TEXC_PROBES   8u
static uint32_t s_mrdp_texc_used;

static inline uint32_t *mrdp_texc_tab(void) { return (uint32_t *)mrdp_sdram_ptr(MRDP_TEXC_TAB); }

static inline void mrdp_texc_reset(void)
{
    uint32_t *t = mrdp_texc_tab();
    for (uint32_t i = 0; i < MRDP_TEXC_TAB_N * 4u; i++) t[i] = 0;
    s_mrdp_texc_used = 0;
}

/* *hit: resident at *addr. Else a free entry (*addr reserved) to fill once
 * decoded, or NULL (no free entry within the probes: decode to the ring). */
static inline uint32_t *mrdp_texc_find(uint32_t src, uint32_t wh, uint32_t key, uint32_t tlut,
                                       int *hit, uint32_t *addr)
{
    uint32_t h = ((src >> 4) ^ (src >> 17) ^ wh ^ tlut) * 2654435761u;
    uint32_t *tab = mrdp_texc_tab();
    *hit = 0;
    for (uint32_t i = 0; i < MRDP_TEXC_PROBES; i++) {
        uint32_t *e = tab + (((h >> 21) + i) & (MRDP_TEXC_TAB_N - 1u)) * 4u;
        if (e[3] == 0u) {
            if (s_mrdp_texc_used >= MRDP_TEXC_ENTRIES) {
                mrdp_loads_wait();
                mrdp_texc_reset();
                e = tab + ((h >> 21) & (MRDP_TEXC_TAB_N - 1u)) * 4u;
            }
            *addr = MRDP_TEXC_BASE + s_mrdp_texc_used * (MRDP_TEX_SLOT_TEXELS * 2u);
            return e;
        }
        if (e[0] == src && e[1] == wh && e[2] == tlut && (e[3] & 0xFFFFFu) == key) {
            *hit = 1;
            *addr = MRDP_TEXC_BASE + ((e[3] >> 20) & 0x7FFu) * (MRDP_TEX_SLOT_TEXELS * 2u);
            return e;
        }
    }
    return 0;
}

static inline void mrdp_texc_fill(uint32_t *e, uint32_t src, uint32_t wh, uint32_t key, uint32_t tlut)
{
    e[0] = src; e[1] = wh; e[2] = tlut;
    e[3] = 0x80000000u | (s_mrdp_texc_used << 20) | key;
    s_mrdp_texc_used++;
}

/* addr: w x h 16-bit texels (fmt 0 RGBA5551, 2 RGBA4444) in SDRAM, rows of w.
 * cms/cmt: the N64 G_TX_* bits (1 mirror, 2 clamp), and 4: the game's tile has
 * no mask (G_TX_NOMASK), so it clamps at its size instead of wrapping -- a
 * picture drawn in 64x64 pieces filtered each piece's last texel with its
 * first. Loads tile 7, draws tile 0. */
/* uls, ult: the tile's origin (u10.2), which MRDP subtracts from every
 * texture coordinate, as the RDP does with SET_TILE_SIZE's SL/TL */
static inline void mrdp_texture_bind(uint32_t addr, uint32_t w, uint32_t h, unsigned fmt,
                                     unsigned cms, unsigned cmt, uint32_t uls, uint32_t ult)
{
    uint32_t line = (w + 3u) >> 2;
    unsigned ms = (cms & 4u) ? 0u : mrdp_log2_exact(w), mt = (cmt & 4u) ? 0u : mrdp_log2_exact(h);
    mrdp_cmd((MRDP_OP_SET_TIMG << 24) | (w - 1u), addr);
    mrdp_cmd((MRDP_OP_SET_TILE << 24) | (fmt << 21) | (line << 9), 7u << 24);
    mrdp_cmd(MRDP_OP_LOAD_TILE << 24, (7u << 24) | (((w - 1u) << 2) << 12) | ((h - 1u) << 2));
    mrdp_load_issued();
    uint32_t t1 = (0u << 24)
                | (((cmt >> 1) & 1u) << 19) | ((cmt & 1u) << 18) | (mt << 14)
                | (((cms >> 1) & 1u) << 9) | ((cms & 1u) << 8) | (ms << 4);
    mrdp_cmd((MRDP_OP_SET_TILE << 24) | (fmt << 21) | (line << 9), t1);
    mrdp_cmd((MRDP_OP_SET_TILESIZE << 24) | (uls << 12) | ult,
             (0u << 24) | ((uls + ((w - 1u) << 2)) << 12) | (ult + ((h - 1u) << 2)));
}

/* ---- triangles ----------------------------------------------------------------- */

/* v: filled by the caller in MRDP's formats (mrdp_setup.h), used as scratch.
 * Returns 1 if a command was emitted. */
static inline int mrdp_emit_triangle(mrdp_vtx_t v[3], int tex, int zbuf, int decal)
{
    unsigned flags = MRDP_SETUP_SHADE | (tex ? MRDP_SETUP_TEX : 0u) | (zbuf ? MRDP_SETUP_Z : 0u);
    /* TRI_V: MRDP computes the gradients (mrdp_setup.h); a sliver whose
     * plane factors do not fit goes as the N64 form. Either way decals are
     * pulled toward the camera by two of their own slopes + 2 LSB. */
    uint32_t w[44];
    int n = mrdp_setup_triangle_r(w, &v[0], &v[1], &v[2], flags, 0, zbuf && decal);
    if (n == 0) return 0;
    mrdp_out(w, (unsigned)n);
    return 1;
}

#endif
