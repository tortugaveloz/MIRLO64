/* See f3d_emit.h. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "geom_pipeline.h"   /* GEOM_FB_HRES */
#include "f3d_emit.h"

/* --- bring-up instrumentation (remove once the walker is proven on HW) --- */
static int      s_trace_task_;     /* trace only the first N gfx tasks */
int f3d_statelog;                   /* log this task's state commands (n64rt: Select + L) */
/* a compile-time 0 without F3D_TRACE: walk() tests it for every command */
#define s_trace_task   (F3D_TRACE && s_trace_task_)
static long     s_walk_iters;      /* total switch-body iterations this display list */
#define WALK_ITER_CAP  400000L
/* Mirlo-N64: every pointer is an N64 address in the game's RDRAM. The lists
 * keep the RSP's segment table (gsSPSegment); KSEG0 addresses resolve through
 * segment 0, which holds 0. Data is big-endian inside native words (n64.h):
 * the i-th halfword of a word-aligned block is at index i ^ 1, the i-th byte
 * at i ^ 3. */
uint8_t *f3d_n64_ram;
uint32_t f3d_n64_ram_size = 8u << 20;
uint32_t f3d_n64_sdram;
/* the segment table is part of the emitter's state (f3d_sem_t: the display-
 * list cache keys a translation on it, as it resolves the list's addresses) */
static uint32_t n64_phys(uint32_t a);
#define F3D_PTR(a)     ((const void *)(f3d_n64_ram + n64_phys((uint32_t)(a))))
#define F3D_SDRAM(a)   (f3d_n64_sdram + n64_phys((uint32_t)(a)))
#define N64_H(p, i)    (((const int16_t *)(p))[i])
#define N64_B(p, i)    (((const uint8_t *)(p))[i])
#define F3D_UCODE_X    0u      /* f3d_ucode_kind's byte-address xor */
#define IN_SDRAM(p)    ((const uint8_t *)(p) >= f3d_n64_ram && (const uint8_t *)(p) < f3d_n64_ram + f3d_n64_ram_size)

/* opcode values: F3DEX2 (gbi.h under F3DEX_GBI_2) */
#define OP_VTX 0x01
#define OP_TRI1 0x05
#define OP_TRI2 0x06
#define OP_POPMTX 0xd8
#define OP_GEOMETRYMODE 0xd9
#define OP_MTX 0xda
#define OP_MOVEWORD 0xdb
#define OP_MOVEMEM 0xdc
#define OP_TEXTURE 0xd7
#define OP_DL 0xde
#define OP_ENDDL 0xdf
#define OP_SETTILESIZE 0xf2
#define OP_SETTIMG 0xfd
#define OP_SETTILE 0xf5
#define OP_LOADBLOCK 0xf3
#define OP_LOADTILE 0xf4
#define OP_LOADTLUT 0xf0
#define OP_SETCOMBINE 0xfc
#define OP_SETPRIMCOLOR 0xfa
#define OP_SETENVCOLOR 0xfb
#define OP_SETOTHERMODE_H 0xe3   /* cycle type [21:20]: copy mode steps 4 texels per dsdx unit */
#define OP_TEXRECT 0xe4          /* + G_RDPHALF_1 (s, t) + G_RDPHALF_2 (dsdx, dtdy) */
#define OP_SETOTHERMODE_L 0xe2   /* F3DEX2 (this port builds with F3DEX_GBI_2): render mode, alpha compare, z source */
#define OP_FILLRECT 0xf6         /* w0 lrx, lry  w1 ulx, uly (10.2); inclusive in fill mode */
#define OP_SETFILLCOLOR 0xf7     /* w1: two RGBA5551 pixels (16-bit colour image) */
#define OP_SETZIMG 0xfe
#define OP_SETCIMG 0xff
#define OP_SPNOOP 0xe0            /* F3DEX2 G_SPNOOP */

#define MTX_PUSH 0x01
#define MTX_LOAD 0x02
#define MTX_PROJECTION 0x04
#define MV_VIEWPORT 0x08
#define MV_LIGHT    0x0a           /* G_MV_LIGHT (F3DEX2) */
#define MW_NUMLIGHT 0x02           /* G_MW_NUMLIGHT */
#define MW_LIGHTCOL 0x0a           /* G_MW_LIGHTCOL */
#define GEO_LIGHTING 0x00020000u
#define GEO_CULL_BACK 0x00000400u   /* F3DEX2 G_CULL_BACK */
#define GEO_TEXTURE_GEN 0x00040000u /* F3DEX2 G_TEXTURE_GEN: s,t from the normals (env map) */

#define DL_DEPTH 16

/* Fill rectangles. SM64 paints a level's background colour (GEO_BACKGROUND_
 * COLOR -- white on the act selector, whose text is black) and clears the
 * screen with full-screen fills in fill mode; the one that comes before any
 * drawing is the frame's clear colour, which the caller applies to the clear
 * it does anyway (f3d_clear_*), so it costs no fill rate. Any other fill is
 * drawn. A fill whose colour image is the depth buffer (init_z_buffer) is
 * the frame's own depth clear and is dropped. */
int      f3d_clear_valid;     /* set by this display list's background fill */
uint32_t f3d_clear_rgb;       /* 0xRRGGBB */
/* F3D_COLD: code that runs once per texture bind or combiner change, not per
 * command. The Mirlo-N64 geom core keeps it in ROM A: ROM B, where the rest of
 * the code lives, is full (lang/c/geom/geom_n64_mips.ld). */
#if defined(__mips__) && defined(F3D_ON_GEOM)
#define F3D_COLD __attribute__((noinline, optimize("Os"), section(".text.rom_a.f3d")))
#else
#define F3D_COLD
#endif
/* The emitter's semantic state: everything a display list's translation
 * depends on (texture, tiles, combiner, render mode, geometry mode, lights).
 * One struct, so the display-list cache (call_dl) can hash and compare it in
 * place; the old names stay as macros. */
typedef struct {
    uint32_t  geomode, tex_render_size, texenv, cc_w0, cc_w1, env_rgba, prim_rgba;
    uint32_t  othermode_h, othermode_l, rm_flags, fill_color;
    uint32_t  lightcol[8];
    int32_t   tex_on, numlights, timg_fmt, timg_siz, tile_cms, tile_cmt, tile_masks, tile_maskt;
    int32_t   tile_pal, ts_w, ts_h, tile_fmt, tile_siz, tile_valid, texbind_dirty;
    uint32_t  tex_sscale, tex_tscale;     /* gsSPTexture's scale, Q16 (0x10000 = 1.0): integers,
                                             for the FPU-less geom core that runs this */
    uintptr_t timg_ptr, tlut_ptr;
    int32_t   tile_tmem[8], tlut_start;   /* each tile's TMEM address; the last TLUT's first entry */
    uint32_t  tile_w0[8], tile_w1[8], tsz_w0[8], tsz_w1[8];   /* each tile's SETTILE and SETTILESIZE words */
    int32_t   rtile, tile_set, tsz_set;   /* the render tile (G_TEXTURE); tiles SETTILE / SETTILESIZE has named */
    uint32_t  cc_raw0, cc_raw1;           /* the last SETCOMBINE as the game wrote it (cc_w0/w1: cc_resolve()) */
    int32_t   cc_tex1;                    /* TEXEL1's tile is the one bound (cc_resolve()) */
    int32_t   cc_pick, cc_force;          /* a blend of the two by shade alpha: which one high alpha
                                             shows (1 TEXEL0, 2 TEXEL1); the one the triangle forces */
    uint32_t  cc_key[2], cc_raw1_key;     /* cc_resolve()'s inputs at its last run */
    int32_t   timg_w, load_next;                      /* SETTIMG's width in texels; the next load[] to reuse */
    /* what TMEM holds, by where it went: the source of each recent LOADBLOCK
     * or LOADTILE -- its first texel, its row pitch in bytes (0: a LOADBLOCK,
     * whose rows are the render tile's line) and its rows or bytes */
    struct { uintptr_t src; int32_t tmem, pitch, n, words; } load[4];
    uint32_t  seg[16];
} f3d_sem_t;
static f3d_sem_t s_sem = {
    .env_rgba = 0xffffffffu, .prim_rgba = 0xffffffffu,
    .tex_sscale = 0x10000u, .tex_tscale = 0x10000u,
};
#define s_seg             (s_sem.seg)
static void sem_sync(void);
/* (sem_sync: OP_DL and OP_MTX resolve their pointers without the walk's sync) */
static uint32_t n64_phys(uint32_t a) { sem_sync(); return (s_seg[(a >> 24) & 15u] + (a & 0xFFFFFFu)) & (f3d_n64_ram_size - 1u); }
#define s_geomode         (s_sem.geomode)
#define s_tex_render_size (s_sem.tex_render_size)
#define s_texenv          (s_sem.texenv)
#define s_cc_w0           (s_sem.cc_w0)
#define s_cc_w1           (s_sem.cc_w1)
#define s_env_rgba        (s_sem.env_rgba)
#define s_prim_rgba       (s_sem.prim_rgba)
#define s_othermode_h     (s_sem.othermode_h)
#define s_othermode_l     (s_sem.othermode_l)
#define s_rm_flags        (s_sem.rm_flags)
#define s_fill_color      (s_sem.fill_color)
#define s_lightcol        (s_sem.lightcol)
#define s_tex_on          (s_sem.tex_on)
#define s_numlights       (s_sem.numlights)
#define s_timg_fmt        (s_sem.timg_fmt)
#define s_timg_siz        (s_sem.timg_siz)
#define s_tile_cms        (s_sem.tile_cms)
#define s_tile_cmt        (s_sem.tile_cmt)
#define s_tile_masks      (s_sem.tile_masks)
#define s_tile_maskt      (s_sem.tile_maskt)
#define s_tile_pal        (s_sem.tile_pal)
#define s_ts_w            (s_sem.ts_w)
#define s_ts_h            (s_sem.ts_h)
#define s_tile_fmt        (s_sem.tile_fmt)
#define s_tile_siz        (s_sem.tile_siz)
#define s_tile_valid      (s_sem.tile_valid)
#define s_texbind_dirty   (s_sem.texbind_dirty)
#define s_tex_sscale      (s_sem.tex_sscale)
#define s_tex_tscale      (s_sem.tex_tscale)
#define s_timg_ptr        (s_sem.timg_ptr)
#define s_tlut_ptr        (s_sem.tlut_ptr)
/* s_fill_color: s_sem */
static uintptr_t s_cimg, s_zimg;
static int       s_drawn;     /* a triangle or rectangle has gone out */

/* Matrix census, read by hal/sptask_dualcore.c's diagnostic block. */
unsigned long f3d_mtx_seen, f3d_mtx_badptr, f3d_mtx_degenerate;
unsigned long f3d_mtx_first_badptr;
unsigned long f3d_mtx_raw_addr[4];
/* All 16 words, not 8: mtx_to_fixed pairs hi[i] (bytes 0..31) with lo[i]
 * (bytes 32..63), so half a dump cannot be inverted back into the matrix. */
unsigned long f3d_mtx_raw[4][16];
long f3d_mtx_max_stack;
unsigned      f3d_mtx_raw_n;

/* tracked state */
/* s_geomode: s_sem */
static int      s_lighting_emitted;
/* s_tex_render_size: s_sem */
/* s_tex_on: s_sem */
/* s_numlights (directional light count, G_MW_NUMLIGHT): s_sem */
/* s_lightcol[8] (0x00RRGGBB, overrides via G_MW_LIGHTCOL): s_sem */

/* texture tile / image state (F3DEX2 SETTIMG/SETTILE/SETTILESIZE/LOADTLUT) */
/* s_timg_ptr: s_sem */
/* s_timg_fmt, s_timg_siz: s_sem */
/* s_tile_cms, _cmt, _masks, _maskt, _pal: s_sem */
/* s_ts_w, s_ts_h: s_sem */
/* s_tlut_ptr: s_sem */
/* s_tile_fmt, s_tile_siz, s_tile_valid (the RENDER tile's texel format): s_sem */
/* s_texbind_dirty: s_sem */
static int       s_texbind_emitted;
/* s_tex_sscale, s_tex_tscale (gsSPTexture scale, 1.0 = 0xFFFF): s_sem */
/* combiner, reduced to a GDL_TEXENV mode (combine_to_texenv()) */
static uint32_t  s_texenv_emitted;   /* s_texenv: s_sem */
/* the combiner's constant inputs (combine_to_ccolor()) */
/* s_cc_w0, s_cc_w1 (the last SETCOMBINE): s_sem */
/* s_env_rgba, s_prim_rgba: s_sem */
static uint64_t  s_cc_emitted;                /* flags << 32 | rgba last sent; ~0 = none */
/* what G_TEXRECT has to put back: the projection (LOADs and MULs replayed
 * here), the viewport, and othermode H's cycle type */
static int32_t   s_proj[16];
static int       s_proj_valid;
static int       s_vp_crop = 1;      /* viewports crop 1:1 (in play) or squeeze (menus): emit_viewport */
static int       s_proj_ortho;       /* the projection is orthographic: 2D */
static int       s_force_keep_2d;    /* F3D_TAG_KEEP_2D seen this frame */
/* 2D translations folded into the vertices: mv_load() */
static int       s_dlc_translating;  /* (the DL cache's, below) */
static int32_t   s_emv[16], s_mv_true[16];
static int       s_emv_valid, s_fold;
static int32_t   s_fold_d[3];        /* whole units, added to every vertex */
/* The modelview stack, mirrored: GDL_MTX_MUL multiplies target x payload,
 * the RSP's G_MTX_MUL payload x target (SM64's menus chain translate then
 * scale this way -- the dialog box sat at the scaled offset, above its
 * text). So a G_MTX_MUL goes to the geom core as the LOAD of the product. */
#define F3D_MV_DEPTH 32
static int32_t   s_mv[F3D_MV_DEPTH][16];
static int       s_mv_sp;
static int       s_mv_base;          /* the stack level of the last modelview LOAD */
/* The x (S15.16) each level had right after the matrix op that pushed it: a
 * 2D element's anchor. SM64's print_generic_string() pushes the line's start
 * and then multiplies each glyph's advance into the same level, so the level's
 * CURRENT x is the glyph's -- anchored there, every glyph moved by its own x
 * and the first letters of a dialog line (x < 60, where the crop is taken up
 * gradually) piled onto each other. */
static int32_t   s_mv_anchor[F3D_MV_DEPTH];
static int32_t   s_rect_run_x0, s_rect_run_xend, s_rect_run_y = -1000, s_rect_run_shift, s_rect_run_xplan;   /* emit_texrect */
static float     s_vp_scale[4], s_vp_trans[4];
static int       s_vp_valid;
/* s_othermode_h: s_sem */
/* othermode L (render mode) and the GDL_RM_* flags derived from it */
/* s_othermode_l: s_sem */
static uint32_t  s_rm_emitted;   /* s_rm_flags: s_sem */

/* a = b x a for 4x4 S15.16 (row vectors: the N64's G_MTX_MUL order) */
static void mtx_premul_fx(int32_t a[16], const int32_t b[16]) {
    int32_t r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t acc = 0;
            for (int k = 0; k < 4; k++) acc += (int64_t)b[i * 4 + k] * a[k * 4 + j];
            r[i * 4 + j] = (int32_t)(acc >> 16);
        }
    for (int i = 0; i < 16; i++) a[i] = r[i];
}

static void sem_dirty(void);
static int s_tri_state_ok;
#ifdef F3D_RAW_CC
static uint64_t s_rawcc_emitted = ~0ull;  /* emit_rawcc(): what GDL_COMBINE last said */
static uint32_t s_rawcc_k[4];
static uint32_t s_prim_lodfrac;           /* G_SETPRIMCOLOR's w0 low byte: the combiner's PRIM_LOD_FRAC
                                           * (Pokemon Stadium's models: alpha = COMBINED * PRIM_LOD_FRAC) */
#endif
void f3d_emit_reset(void) {
    s_tri_state_ok = 0;
    sem_dirty();
    memset(s_seg, 0, sizeof s_seg);          /* a new task: the RSP's segment table starts empty */
    /* per-frame, so the census describes THIS frame */
    f3d_mtx_seen = f3d_mtx_badptr = f3d_mtx_degenerate = 0;
    f3d_mtx_raw_n = 0;
    f3d_mtx_max_stack = -1;
    s_geomode = 0;
    s_lighting_emitted = -1;
    s_tex_render_size = 0;
    s_tex_on = 0;
    s_numlights = 1;
    for (int i = 0; i < 8; i++) s_lightcol[i] = 0;
    s_timg_ptr = 0; s_timg_fmt = 0; s_timg_siz = 2;
    s_tile_cms = s_tile_cmt = 0; s_tile_masks = s_tile_maskt = 0; s_tile_pal = 0;
    s_ts_w = s_ts_h = 0; s_tlut_ptr = 0; s_tile_valid = 0;
    memset(s_sem.tile_tmem, 0, sizeof s_sem.tile_tmem); s_sem.tlut_start = 0;
    s_sem.rtile = s_sem.tile_set = s_sem.tsz_set = 0;
    memset(s_sem.load, 0, sizeof s_sem.load); s_sem.load_next = 0; s_sem.timg_w = 0;
    s_texbind_dirty = 0; s_texbind_emitted = 0;
    s_tex_sscale = s_tex_tscale = 0x10000u;
    s_proj_valid = 0; s_vp_valid = 0; s_vp_crop = 1; s_proj_ortho = 0; s_force_keep_2d = 0;
    s_emv_valid = 0; s_fold = 0;
    s_othermode_h = 2u << 12;   /* G_TF_BILERP until the frame's init_rdp says otherwise */
    s_mv_sp = 0; s_mv_base = 0; s_rect_run_y = -1000; s_rect_run_xplan = -1000;
    for (int i = 0; i < 16; i++) s_mv[0][i] = (i % 5) ? 0 : 65536;
    /* -1: the geom core keeps this across frames, so state it explicitly. */
    s_texenv = 0; s_texenv_emitted = 0xFFFFFFFFu;
    s_cc_w0 = s_cc_w1 = 0; s_sem.cc_raw0 = s_sem.cc_raw1 = 0; s_sem.cc_tex1 = 0; s_sem.cc_key[0] = s_sem.cc_key[1] = s_sem.cc_raw1_key = 0;
    s_sem.cc_pick = s_sem.cc_force = 0; s_env_rgba = s_prim_rgba = 0xffffffffu; s_cc_emitted = ~0ull;
#ifdef F3D_RAW_CC
    s_rawcc_emitted = ~0ull; s_prim_lodfrac = 0;
#endif
    s_othermode_l = 0;
    s_rm_flags = GDL_RM_DEFAULT; s_rm_emitted = 0xFFFFFFFFu;
}

/* The N64 render mode (othermode L bits 3..31) -> GDL_RM_* flags:
 *   Z_CMP 0x10 / Z_UPD 0x20       depth test / depth write
 *   ZMODE 0xC00 == ZMODE_DEC      decal: drawn onto a coplanar surface
 *   FORCE_BL 0x4000 with the blender's second term CLR_MEM * (1 - A_IN)
 *                                 translucent: in*a + mem*(1-a)
 * The blender fields are checked for either cycle (GBL_c1 m2a/m2b at
 * [23:22]/[19:18], GBL_c2 at [21:20]/[17:16]); SM64's translucent modes set
 * both (G_RM_AA_ZB_XLU_SURF, _DECAL, _INTER). Opaque modes also read memory
 * (CLR_MEM * A_MEM, for antialiasing) but without FORCE_BL and with A_MEM,
 * so they stay opaque. */
static uint32_t rm_flags_of(uint32_t om) {
    uint32_t f = 0;
    if (om & 0x10u) f |= GDL_RM_ZCMP;
    if (om & 0x20u) f |= GDL_RM_ZUPD;
    if ((om & 0xC00u) == 0xC00u) f |= GDL_RM_DECAL;
    int c1 = ((om >> 22) & 3u) == 1u /* CLR_MEM */ && ((om >> 18) & 3u) == 0u /* 1MA */;
    int c2 = ((om >> 20) & 3u) == 1u && ((om >> 16) & 3u) == 0u;
    if ((om & 0x4000u) && (c1 || c2)) f |= GDL_RM_XLU;
    return f;
}

static void emit_rendermode_if_changed(gdl_cur_t *out) {
    if (s_rm_flags != s_rm_emitted) { gdl_rendermode(out, s_rm_flags); s_rm_emitted = s_rm_flags; }
}

/* Emit GDL_TEXBIND for the current tile if texturing is on and something
 * changed since the last emit. Called just before a triangle (mirrors
 * f3d_interp.c's maybe_upload_texture). */
/* LOADBLOCK / LOADTILE: where the texels come from, by the TMEM they fill */
F3D_COLD static void record_load(uint32_t op, uint32_t w0, uint32_t w1) {
    /* w0: uls[23:12] ult[11:0]; w1: tile[26:24] lrs[23:12] lrt|dxt[11:0].
     * A LOADBLOCK's coordinates are whole texels, a LOADTILE's u10.2. */
    uint32_t tl = (w1 >> 24) & 7u, bits = 4u << s_timg_siz, iw = (uint32_t)s_sem.timg_w;
    uint32_t uls = (w0 >> 12) & 0xfffu, ult = w0 & 0xfffu, lrs = (w1 >> 12) & 0xfffu, lrt = (uint32_t)w1 & 0xfffu;
    if (op == OP_LOADTILE) { uls >>= 2; ult >>= 2; lrs >>= 2; lrt >>= 2; }
    /* TMEM it fills, in 64-bit words: what it overwrites is gone
     * (Pilotwings' sea keeps its colour strip at 0 and its detail at
     * 0x100; a round-robin slot once dropped the strip's record) */
    const int32_t t0 = s_sem.tile_tmem[tl];
    const int32_t nw = op == OP_LOADBLOCK
        ? (int32_t)((((lrs >= uls ? lrs - uls : 0) + 1u) * bits + 63u) >> 6)
        : (int32_t)((lrt >= ult ? lrt - ult + 1u : 1u) * ((s_sem.tile_w0[tl] >> 9) & 0x1ffu));
    int k = -1;
    for (int j = 0; j < 4; j++) {
        if (!s_sem.load[j].src) { if (k < 0) k = j; continue; }
        if (s_sem.load[j].tmem < t0 + nw && t0 < s_sem.load[j].tmem + s_sem.load[j].words) {
            s_sem.load[j].src = 0;
            if (k < 0) k = j;
        }
    }
    if (k < 0) { k = s_sem.load_next; s_sem.load_next = (k + 1) & 3; }
    s_sem.load[k].tmem = t0;
    s_sem.load[k].words = nw > 0 ? nw : 1;
    if (op == OP_LOADBLOCK) {
        s_sem.load[k].src = s_timg_ptr + (uintptr_t)(((ult * iw + uls) * bits) >> 3);
        s_sem.load[k].pitch = 0;
        s_sem.load[k].n = (int32_t)((((lrs >= uls ? lrs - uls : 0) + 1u) * bits + 7u) >> 3);   /* bytes */
    } else {
        s_sem.load[k].src = s_timg_ptr + (uintptr_t)(((ult * iw + uls) * bits) >> 3);
        s_sem.load[k].pitch = (int32_t)((iw * bits) >> 3);
        s_sem.load[k].n = (int32_t)(lrt >= ult ? lrt - ult + 1u : 1u);                         /* rows */
    }
    if (!s_timg_ptr || (op == OP_LOADTILE && !iw)) s_sem.load[k].src = 0;
}
static uint32_t s_rdphalf1;   /* the last G_RDPHALF_1 (G_LOAD_UCODE's data address) */
/* each loaded vertex's alpha (OP_VTX), for tri_pick() */
static uint8_t s_valpha[64];
static void cc_resolve(void);
/* A blend of the two textures by shade alpha (cc_pick): the triangle's
 * vertices' mean alpha decides which one it is drawn with. */
F3D_COLD static void tri_pick(uint32_t sum, uint32_t nv) {
    int want = (sum >= 128u * nv) ? s_sem.cc_pick : 3 - s_sem.cc_pick;
    if (want != s_sem.cc_force) { s_sem.cc_force = want; cc_resolve(); s_tri_state_ok = 0; }
}
/* the texels a rectangle samples (emit_texrect: tile-relative, set around
 * its bind), so a tile larger than RDP's TMEM can be cut down to them */
static int32_t s_rect_u0 = -1, s_rect_v0, s_rect_u1, s_rect_v1;
/* n / d, small n (no divider on the Mirlo-N64 geom core) */
F3D_COLD static uint32_t udiv_small(uint32_t n, uint32_t d) {
    uint32_t q = 0;
    while (n >= d) { n -= d; q++; }
    return q;
}
F3D_COLD static void emit_texbind_if_needed(gdl_cur_t *out) {
    if (!s_tex_on || !s_timg_ptr) return;
    if (!s_texbind_dirty && s_texbind_emitted) return;
    /* the render tile's state (G_TEXTURE names it: SM64's is 0, Pilotwings'
     * 1), as SETTILE and SETTILESIZE left it for that tile */
    const int rt = (s_sem.rtile + (s_sem.cc_tex1 && ((s_othermode_h >> 20) & 3u) != 2u)) & 7;   /* TEXEL1's: cc_resolve() */
    const uint32_t tw0 = s_sem.tile_w0[rt], tw1 = s_sem.tile_w1[rt];
    const int tset = (s_sem.tile_set >> rt) & 1, zset = (s_sem.tsz_set >> rt) & 1;
    const int cms = (int)((tw1 >> 8) & 3u), cmt = (int)((tw1 >> 18) & 3u);
    const int masks = (int)((tw1 >> 4) & 15u), maskt = (int)((tw1 >> 14) & 15u), pal = (int)((tw1 >> 20) & 15u);
    const uint32_t sl = (s_sem.tsz_w0[rt] >> 12) & 0xfffu, tl = s_sem.tsz_w0[rt] & 0xfffu;
    const int ts_w = zset ? (int)((((s_sem.tsz_w1[rt] >> 12) & 0xfffu) - sl) / 4u) + 1 : 0;
    const int ts_h = zset ? (int)(((s_sem.tsz_w1[rt] & 0xfffu) - tl) / 4u) + 1 : 0;
    int w = ts_w > 0 ? ts_w : (masks > 0 ? (1 << masks) : 32);
    int h = ts_h > 0 ? ts_h : (maskt > 0 ? (1 << maskt) : 32);
    /* fmt/siz come from the render tile, NOT from SETTIMG. The image
     * descriptor carries the LOAD size, and G_IM_SIZ_4b_LOAD_BLOCK and
     * G_IM_SIZ_8b_LOAD_BLOCK are both G_IM_SIZ_16b: an 8-bit texture is
     * loaded as if it were 16-bit and only the tile that renders it states
     * the real texel size. Taking it from SETTIMG decoded Mario's IA8 shadow
     * as IA16 -- with twice the stride -- and drew four stray dots. */
    /* othermode H: copy mode [21:20] == 2 or texture filter [13:12] == G_TF_POINT */
    int point = ((s_othermode_h >> 20) & 3u) == 2u || ((s_othermode_h >> 12) & 3u) == 0u;
    int fmt = tset ? (int)((tw0 >> 21) & 7u) : s_timg_fmt, siz = tset ? (int)((tw0 >> 19) & 3u) : s_timg_siz;
    /* CI: the palette's entries for this tile -- a CI4 tile's 16 start at
     * palette * 16, a CI8's 256 at 0 -- in the TLUT the last LOADTLUT put at
     * TMEM entry tlut_start; its type from othermode H's TT field [15:14] */
    uintptr_t tlut = s_tlut_ptr;
    int flags = point ? GDL_TEXBIND_POINT : 0;
    if (fmt == 2 && tlut) {
        int32_t first = (siz == 0 ? pal * 16 : 0) - s_sem.tlut_start;
        if (first > 0) tlut += (uintptr_t)first * 2u;
        if (((s_othermode_h >> 14) & 3u) == 3u) flags |= GDL_TEXBIND_TLUT_IA;
    }
    /* Where the render tile's texels come from: the load that filled its
     * TMEM. A LOADBLOCK's rows are the tile's line (it copies RDRAM as it
     * is), a LOADTILE's are the image's own rows, from its origin on. The
     * tile's texel (0, 0) is then texel k0 of src, a row pw texels on, and
     * the tile's origin (SL/TL) comes off every coordinate. */
    uintptr_t src = s_timg_ptr;
    gdl_texsub_t sub = { 0, 0, zset ? sl : 0u, zset ? tl : 0u };
    const int32_t ttmem = (int32_t)(tw0 & 0x1ffu);
    uint32_t pw = (uint32_t)w;
    int li = -1;
    for (int i = 0; i < 4; i++) {   /* the load the tile starts in (nearest below it) */
        const int32_t d = ttmem - s_sem.load[i].tmem;
        if (s_sem.load[i].src && d >= 0 && d < s_sem.load[i].words && (li < 0 || s_sem.load[i].tmem > s_sem.load[li].tmem)) li = i;
    }
    for (int i = li; i >= 0; i = -1) {
        const int32_t d = ttmem - s_sem.load[i].tmem;   /* the tile may start inside the load */
        uint32_t pitch = s_sem.load[i].pitch ? (uint32_t)s_sem.load[i].pitch
                       : ((tw0 >> 9) & 0x1ffu) * (siz == 3 ? 16u : 8u);   /* (32-bit: half a texel in each TMEM half) */
        if (!pitch) break;
        int32_t rows = s_sem.load[i].pitch ? s_sem.load[i].n : (int32_t)udiv_small((uint32_t)s_sem.load[i].n + pitch - 1u, pitch);
        src = s_sem.load[i].src;
        if (!s_sem.load[i].pitch) src += (uintptr_t)d * 8u;
        else rows -= (int32_t)udiv_small((uint32_t)d * 8u, pitch);     /* (a LOADTILE: whole rows) */
        pw = (pitch * 2u) >> siz;                              /* bytes -> texels */
        if (w > (int)pw) w = (int)pw;                          /* no more than was loaded */
        if (rows > 0 && h > rows) h = rows;
        break;
    }
    /* Too large for RDP's TMEM and drawn by a rectangle (a font, a
     * backdrop): only the texels it samples, rather than the whole tile at a
     * fraction of its resolution */
    if (s_rect_u0 >= 0 && w * h > 4096 && !masks && !maskt) {
        int32_t u0 = s_rect_u0, v0 = s_rect_v0, u1 = s_rect_u1, v1 = s_rect_v1;
        if (u0 < 0) u0 = 0;
        if (v0 < 0) v0 = 0;
        if (u1 > w - 1) u1 = w - 1;
        if (v1 > h - 1) v1 = h - 1;
        if (u0 <= u1 && v0 <= v1) {
            sub.k0 += (uint32_t)v0 * pw + (uint32_t)u0;
            sub.uls += (uint32_t)u0 * 4u; sub.ult += (uint32_t)v0 * 4u;
            w = u1 - u0 + 1; h = v1 - v0 + 1;
        }
    }
    int ext = pw != (uint32_t)w || sub.k0 || sub.uls || sub.ult;
    sub.pw = pw;
    /* the tile's shift scales its coordinates: 1..10 down by 2^n, 11..15 up by
     * 2^(16-n) (a detail texture repeats faster than the surface's own) */
    uint32_t ssc = s_tex_sscale, tsc = s_tex_tscale;
    {
        uint32_t shs = tw1 & 15u, sht = (tw1 >> 10) & 15u;
        if (shs) ssc = shs <= 10u ? ssc >> shs : ssc << (16u - shs);
        if (sht) tsc = sht <= 10u ? tsc >> sht : tsc << (16u - sht);
    }
    /* the scale itself (GDL_TEXBIND_SCALE): the geom core takes 1/w as a
     * shift -- a float divide per bind was 2 % of a Majora frame */
    gdl_texbind_q(out, fmt, siz, cms | (masks ? 0 : 4), cmt | (maskt ? 0 : 4),   /* 4: G_TX_NOMASK, clamps */
                pal | flags, w, h, (const void *)src, (const void *)tlut, ssc, tsc, ext ? &sub : 0);
    s_texbind_dirty = 0;
    s_texbind_emitted = 1;
}

/* SM64 Mtx: 16 s16 integer parts then 16 u16 fraction parts;
 * element = (s16<<16 | u16) / 65536.
 *
 * mtxf_to_mtx() -> guMtxF2L() (AVOID_UB path) packs each *adjacent element
 * pair* byte-swapped: it writes one s32 per pair as (hi16(mf[r][2c])<<16) |
 * hi16(mf[r][2c+1]), so on a little-endian target the s16 at array index i
 * actually holds mf element i^1. guMtxL2F() is the matching un-packer. Reading
 * hi[i]/lo[i] linearly transposes columns 0<->1 and 2<->3 of every matrix
 * (found via the host simulator: made clip.w collapse to a constant). */
static void mtx_to_fixed(const void *mtx, int32_t out[16]) {
    const int16_t  *hi = (const int16_t  *)mtx;
    const uint16_t *lo = (const uint16_t *)mtx + 16;
    for (int i = 0; i < 16; i++) {
        int j = i;                        /* (big-endian: the N64's own order) */
        out[i] = (int32_t)(((uint32_t)(uint16_t)hi[j] << 16) | lo[j]);   /* S15.16, the GDL's format */
    }
}

static int texgen_as_written(void);
static int texgen_shade_only(void);
static void emit_texenv_if_changed(gdl_cur_t *out) {
    /* G_TEXTURE_GEN: the geom core generates s,t from the normals
     * (GDL_GEOMODE_TEXGEN), so the combiner applies as written -- the power
     * star is G_CC_DECALFADE, all of its yellow is the env-mapped texture. */
    uint32_t te = texgen_shade_only() ? (GDL_TE_RGB_SHADE | GDL_TE_A_SHADE) : s_texenv;
    if (te != s_texenv_emitted) { gdl_texenv(out, te); s_texenv_emitted = te; }
}

/* The N64 colour combiner, (A - B) * C + D per channel group, reduced to
 * the GDL_TEXENV modes the geometry core handles (geom_gdl.h). Only cycle 0
 * is read: SM64 draws in 1-cycle mode, where both cycles are set equal.
 * Field layout is gbi.h's GCCc0w0/GCCc0w1. PRIMITIVE and ENVIRONMENT (and
 * their alphas) count as the shade side of a product: combine_to_ccolor()
 * folds their values into the vertex colour through GDL_CCOLOR, so "shade"
 * below means "the vertex colour after that". (They used to be taken as 1,
 * which drew SM64's flames -- texel * env -- as bare grey IA texels.)
 *   rgb   (TEXEL0 - SHADE) * TEXEL0_ALPHA + SHADE  -> LERP  (G_CC_BLENDRGB*)
 *         a product with no texel term             -> SHADE (G_CC_SHADE*)
 *         a product with no shade term             -> TEXEL (G_CC_DECALRGB*)
 *         anything else                            -> MODULATE
 *   alpha the same, without the lerp.
 * The alpha mode matters twice: SM64's snowflakes (G_CC_DECALRGBA) have
 * grey 0x7F vertices and are white on an N64 -- modulated they came out
 * half as bright -- and a texel-derived alpha is what turns the alpha test
 * on for cut-out edges, where G_CC_MODULATERGB's shade alpha must not. */
#define CCMUX_TEXEL0        1
#define CCMUX_SHADE         4
#define CCMUX_TEXEL0_ALPHA  8
#define CCMUX_0_A          15   /* G_CCMUX_0 in the 4-bit A/B fields */
#define CCMUX_0_C          31
#define CCMUX_0_D           7
#define ACMUX_0             7
#define CCMUX_PRIM          3
#define CCMUX_ENV           5
#define CCMUX_PRIM_ALPHA   10
#define CCMUX_ENV_ALPHA    12
/* The per-vertex colour side of a product: SHADE, or a PRIMITIVE/ENVIRONMENT
 * constant, which GDL_CCOLOR folds into the vertex colour. */
static int cc_is_k(uint32_t x, uint32_t shade)
{
    return x == shade || x == CCMUX_PRIM || x == CCMUX_ENV
        || (shade == CCMUX_SHADE && (x == CCMUX_PRIM_ALPHA || x == CCMUX_ENV_ALPHA));
}
/* What a product x * y (or a lone term x, y = 1) draws on: bit0 texel, bit1 shade. */
static uint32_t cc_terms(uint32_t x, uint32_t y, uint32_t texel, uint32_t shade)
{
    return (x == texel || y == texel ? 1u : 0u) | (cc_is_k(x, shade) || cc_is_k(y, shade) ? 2u : 0u);
}
static uint32_t cc_mode(uint32_t terms, uint32_t modulate, uint32_t texel, uint32_t shade)
{
    return terms == 3u ? modulate : terms == 1u ? texel : shade;   /* constants alone: shade */
}
/* G_TEXTURE_GEN applies as written only when the colour combiner has a form
 * TexEnv reproduces -- D alone, A * C, or the texel-alpha lerp (the power
 * star's G_CC_DECALFADE). The general (A - B) * C + D -- Goddard's face shine,
 * G_CC_HILITERGBA = (PRIM - SHADE) * TEXEL0 + SHADE -- would come out as
 * texel * PRIM, black on the title screen; such a surface keeps the lit
 * shade without its highlight, as before texgen existed. */
static int texgen_as_written(void)
{
    uint32_t a = (s_cc_w0 >> 20) & 0xf, c = (s_cc_w0 >> 15) & 0x1f;
    uint32_t b = ((uint32_t)s_cc_w1 >> 28) & 0xf, d = ((uint32_t)s_cc_w1 >> 15) & 0x7;
    if (!(s_geomode & GEO_TEXTURE_GEN)) return 0;
    return (a == CCMUX_TEXEL0 && b == CCMUX_SHADE && c == CCMUX_TEXEL0_ALPHA && d == CCMUX_SHADE)
        || c == CCMUX_0_C || a == b || (b == CCMUX_0_A && d == CCMUX_0_D);
}
static int texgen_shade_only(void) { return (s_geomode & GEO_TEXTURE_GEN) && !texgen_as_written(); }
static uint32_t combine_to_texenv(uint32_t w0, uint32_t w1) {
    uint32_t a = (w0 >> 20) & 0xf, c = (w0 >> 15) & 0x1f;
    uint32_t b = (w1 >> 28) & 0xf, d = (w1 >> 15) & 0x7;
    uint32_t aa = (w0 >> 12) & 0x7, ac = (w0 >> 9) & 0x7;
    uint32_t ab = (w1 >> 12) & 0x7, ad = (w1 >> 9) & 0x7;
    uint32_t rgb, alpha;

    if (a == CCMUX_TEXEL0 && b == CCMUX_SHADE && c == CCMUX_TEXEL0_ALPHA && d == CCMUX_SHADE)
        rgb = GDL_TE_RGB_LERP;
    else if (c == CCMUX_0_C || a == b)              /* the product vanishes: D alone */
        rgb = cc_mode(cc_terms(d, d, CCMUX_TEXEL0, CCMUX_SHADE),
                      GDL_TE_RGB_SHADE, GDL_TE_RGB_TEXEL, GDL_TE_RGB_SHADE);
    else if (b == CCMUX_0_A && d == CCMUX_0_D)      /* A * C */
        rgb = cc_mode(cc_terms(a, c, CCMUX_TEXEL0, CCMUX_SHADE),
                      GDL_TE_RGB_MODULATE, GDL_TE_RGB_TEXEL, GDL_TE_RGB_SHADE);
    else
        rgb = GDL_TE_RGB_MODULATE;

    if (ac == ACMUX_0 || aa == ab)
        alpha = cc_mode(cc_terms(ad, ad, CCMUX_TEXEL0, CCMUX_SHADE),
                        GDL_TE_A_SHADE, GDL_TE_A_TEXEL, GDL_TE_A_SHADE);
    else if (ab == ACMUX_0 && ad == ACMUX_0)
        alpha = cc_mode(cc_terms(aa, ac, CCMUX_TEXEL0, CCMUX_SHADE),
                        GDL_TE_A_MODULATE, GDL_TE_A_TEXEL, GDL_TE_A_SHADE);
    else
        alpha = GDL_TE_A_MODULATE;
    return rgb | alpha;
}

/* The constants the same combiner multiplies by, as GDL_CCOLOR (geom_gdl.h):
 * each channel group's product terms (A * C, or D alone -- the same reading
 * combine_to_texenv() makes) that are PRIMITIVE / ENVIRONMENT (or their
 * alphas) multiply into K, and whether SHADE is one of them decides if the
 * vertex colour is kept or replaced by K. This is what makes the flames
 * (G_CC_FADEA: texel * env) orange instead of the bare grey IA texel, and
 * env-alpha fades (Boos, transitions) fade. */
static uint32_t cc_ch(uint32_t rgba, int sh) { return (rgba >> sh) & 0xffu; }
/* One term of a product: SHADE sets *shade, a constant multiplies into k and
 * returns 1. (The alpha mux codes for PRIMITIVE/ENVIRONMENT/SHADE are the
 * same numbers as the colour ones.) */
static int cc_fold(uint32_t x, uint32_t k[4], int *shade, int alpha_group)
{
    uint32_t src = 0; int use_a = 0;
    if (x == CCMUX_PRIM) src = s_prim_rgba;
    else if (x == CCMUX_ENV) src = s_env_rgba;
    else if (!alpha_group && x == CCMUX_PRIM_ALPHA) { src = s_prim_rgba; use_a = 1; }
    else if (!alpha_group && x == CCMUX_ENV_ALPHA)  { src = s_env_rgba;  use_a = 1; }
    else { if (x == CCMUX_SHADE) *shade = 1; return 0; }
    if (alpha_group) { k[3] = k[3] * cc_ch(src, 0) / 255u; return 1; }
    for (int c = 0; c < 3; c++)
        k[c] = k[c] * (use_a ? cc_ch(src, 0) : cc_ch(src, 24 - 8 * c)) / 255u;
    return 1;
}
F3D_COLD static uint64_t combine_to_ccolor(uint32_t w0, uint32_t w1)
{
    uint32_t a = (w0 >> 20) & 0xf, c = (w0 >> 15) & 0x1f;
    uint32_t b = (w1 >> 28) & 0xf, d = (w1 >> 15) & 0x7;
    uint32_t aa = (w0 >> 12) & 0x7, ac = (w0 >> 9) & 0x7;
    uint32_t ab = (w1 >> 12) & 0x7, ad = (w1 >> 9) & 0x7;
    uint32_t k[4] = { 255u, 255u, 255u, 255u };
    int shade_rgb = 0, shade_a = 0, konst = 0;

    if (a == CCMUX_TEXEL0 && b == CCMUX_SHADE && c == CCMUX_TEXEL0_ALPHA && d == CCMUX_SHADE)
        shade_rgb = 1;                                  /* the lerp: shade as is */
    else if (c == CCMUX_0_C || a == b)
        konst |= cc_fold(d, k, &shade_rgb, 0);
    else { konst |= cc_fold(a, k, &shade_rgb, 0); konst |= cc_fold(c, k, &shade_rgb, 0); }
    if (ac == ACMUX_0 || aa == ab)
        konst |= cc_fold(ad, k, &shade_a, 1);
    else { konst |= cc_fold(aa, k, &shade_a, 1); konst |= cc_fold(ac, k, &shade_a, 1); }
    if (!konst) return 0;     /* shade and texel only: vertex colours untouched */
    /* a group with neither shade nor a constant (the texel alone, or 1)
     * leaves the vertex colour as it is -- the texenv mode ignores it there */
    if (!shade_rgb && k[0] == 255u && k[1] == 255u && k[2] == 255u
        && (combine_to_texenv(w0, w1) & 3u) == GDL_TE_RGB_TEXEL) shade_rgb = 1;
    return ((uint64_t)(1u | (shade_rgb ? 2u : 0u) | (shade_a ? 4u : 0u)) << 32)
         | (k[0] << 24) | (k[1] << 16) | (k[2] << 8) | k[3];
}
/* ---- the combine as one cycle of TEXEL0 ------------------------------------
 * The reduction above reads one cycle, and TEXEL0 as the only texture. The RDP
 * uses the SECOND cycle's inputs in 1-cycle mode, and in 2-cycle mode the
 * second combines the first's result (COMBINED) -- Pilotwings' terrain and
 * sea mix a colour texture on TEXEL1 with an intensity detail texture on
 * TEXEL0, shade in the second cycle. cc_resolve() folds that into one cycle
 * (cc_w0/w1, both cycles the same) and picks the texture to bind: TEXEL1's
 * (tile rtile + 1) when only it is used, or when TEXEL0's is intensity only
 * (I, IA) and TEXEL1's has colour -- the other one then counts as the bound
 * one in a blend of the two, and as 1 in a product. */
typedef struct { uint32_t a, b, c, d; } cc_grp_t;
F3D_COLD static void cc_cycle(uint32_t w0, uint32_t w1, int cyc, cc_grp_t *rgb, cc_grp_t *al) {
    if (!cyc) {
        rgb->a = (w0 >> 20) & 0xf; rgb->c = (w0 >> 15) & 0x1f; rgb->b = (w1 >> 28) & 0xf; rgb->d = (w1 >> 15) & 0x7;
        al->a = (w0 >> 12) & 0x7; al->c = (w0 >> 9) & 0x7; al->b = (w1 >> 12) & 0x7; al->d = (w1 >> 9) & 0x7;
    } else {
        rgb->a = (w0 >> 5) & 0xf; rgb->c = w0 & 0x1f; rgb->b = (w1 >> 24) & 0xf; rgb->d = (w1 >> 6) & 0x7;
        al->a = (w1 >> 21) & 0x7; al->c = (w1 >> 18) & 0x7; al->b = (w1 >> 3) & 0x7; al->d = w1 & 0x7;
    }
}
/* the one input a cycle's result mostly is: D when the product vanishes,
 * else the texel wherever it appears, else the shade, else D */
static uint32_t cc_dominant(cc_grp_t f, int alpha) {
    int zc = alpha ? f.c == 7u : f.c >= 16u;
    if (zc || f.a == f.b) return f.d;
    if (f.d == 1u || f.a == 1u || f.c == 1u || (!alpha && f.c == 8u)) return 1u;
    if (f.d == 4u || f.a == 4u || f.c == 4u || (!alpha && f.c == 11u)) return 4u;
    return f.d;
}
/* one group of the second cycle over the first's: rgb or alpha (zero codes
 * differ: rgb A/B 8+, C 16+, D 7; alpha 7 everywhere, and alpha C 0 is LOD) */
F3D_COLD static cc_grp_t cc_fold2(cc_grp_t f, cc_grp_t s, int alpha) {
    const uint32_t zab = alpha ? 7u : 15u, zc = alpha ? 7u : 31u, zd = 7u;
    int zb = alpha ? s.b == 7u : s.b >= 8u, zcx = alpha ? s.c == 7u : s.c >= 16u;
    int uses = s.a == 0u || s.b == 0u || s.d == 0u || (!alpha && (s.c == 0u || s.c == 7u));
    if (!uses) return s;                                   /* the second cycle alone */
    if ((zcx || s.a == s.b) && s.d == 0u) return f;        /* COMBINED passed through */
    uint32_t x = 0xffu;                                    /* COMBINED * x */
    if (s.a == 0u && zb && s.d == zd && !(!alpha && (s.c == 0u || s.c == 7u))) x = s.c;
    else if (!alpha && s.c == 0u && zb && s.d == zd && s.a != 0u && s.a < 6u) x = s.a;   /* x * COMBINED */
    if (x == 0xffu) {
        /* anything else over COMBINED (a colourising lerp, OoT/MM's
         * (PRIM - ENV) * COMBINED + ENV): the second cycle, COMBINED taken
         * as the first cycle's dominant input */
        uint32_t y = cc_dominant(f, alpha);
        if (y >= 6u) return f;
        cc_grp_t r = s;
        if (r.a == 0u) r.a = y;
        if (r.b == 0u) r.b = y;
        if (r.d == 0u) r.d = y;
        if (!alpha && r.c == 0u) r.c = y;
        if (!alpha && r.c == 7u) r.c = y + 7u;             /* COMBINED_ALPHA -> y's alpha */
        return r;
    }
    /* the first cycle as one term y (D alone, or y * 1) */
    int fzc = alpha ? f.c == 7u : f.c >= 16u, fzb = alpha ? f.b == 7u : f.b >= 8u;
    uint32_t y = 0xffu;
    if (fzc || f.a == f.b) y = f.d;
    if (y != 0xffu && y < 6u) { cc_grp_t r = { y, zab, x, zd }; return r; }   /* y * x */
    if (y == 6u && x < 6u) { cc_grp_t r = { zab, zab, zc, x }; return r; }     /* 1 * x */
    /* a product already: keep it, but not without the shade the second
     * cycle multiplies by (G_CC_MODULATE... in cycle 2 over TEXEL0 * TEXEL1) */
    if (fzb && f.d == zd && x == 4u && f.a != 4u && f.c != 4u) {
        cc_grp_t r = f;
        if ((f.c == 1u || f.c == 2u) && f.a != 1u && f.a != 2u) r.a = f.c;   /* keep the texel, shade for the rest */
        r.c = 4u;
        return r;
    }
    /* anything else times x: its dominant input times x (MM's title logo:
     * a mix of two texels, times PRIM alpha -- which fades it in) */
    y = cc_dominant(f, alpha);
    if (y < 6u) { cc_grp_t r = { y, zab, x, zd }; return r; }
    return f;
}
static int cc_is_intensity(int tile) {
    uint32_t fmt = (s_sem.tile_w0[tile & 7] >> 21) & 7u;
    return ((s_sem.tile_set >> (tile & 7)) & 1) && (fmt == 3u || fmt == 4u);
}
/* bound texture b, the other u: in one group */
F3D_COLD static void cc_subst(cc_grp_t *g, uint32_t b, uint32_t u, int alpha) {
    const uint32_t one = 6u, zb = alpha ? 7u : 15u;
    uint32_t ua = alpha ? u : u + 7u, ba = alpha ? b : b + 7u;   /* rgb C: TEXELn_ALPHA = 7 + n */
    int blend = (g->a == u && (g->b == b || g->d == b)) || (g->b == u && (g->a == b || g->d == b));
    if (g->a == u) g->a = blend ? b : one;
    if (g->b == u) g->b = blend ? b : zb;
    if (g->d == u) g->d = blend ? b : one;
    if (g->c == u || (!alpha && g->c == ua)) {
        if (g->b == zb && g->d == 7u && g->a < 7u) { g->d = g->a; g->c = alpha ? 7u : 31u; }   /* a * 1 */
        else g->c = (g->c == u) ? b : ba;
    }
    /* TEXEL1 -> TEXEL0: the bound one is TEXEL0 to the rest */
    if (b == 2u) {
        if (g->a == 2u) g->a = 1u;
        if (g->b == 2u) g->b = 1u;
        if (g->d == 2u) g->d = 1u;
        if (g->c == 2u) g->c = 1u;
        if (!alpha && g->c == 9u) g->c = 8u;
    }
}
static int cc_uses(const cc_grp_t *g, uint32_t t, int alpha) {
    return g->a == t || g->b == t || g->d == t || g->c == t || (!alpha && g->c == t + 7u);
}
F3D_COLD static void cc_resolve(void) {
    uint32_t w0 = s_sem.cc_raw0, w1 = s_sem.cc_raw1;
    int cyc = (int)((s_othermode_h >> 20) & 3u);
    /* the same inputs as last time: the same result (Majora sets tiles by the
     * hundred; this ran for each, 7 % of the geom core's frame) */
    {
        const int rt_ = s_sem.rtile, r1_ = (rt_ + 1) & 7;
        uint32_t k0 = (uint32_t)cyc | (uint32_t)rt_ << 2 | (uint32_t)s_sem.cc_force << 5
                    | (uint32_t)((s_sem.tile_set >> rt_) & 1) << 7 | (uint32_t)((s_sem.tile_set >> r1_) & 1) << 8
                    | ((s_sem.tile_w0[rt_] >> 19) & 0x1fu) << 9 | ((s_sem.tile_w0[r1_] >> 19) & 0x1fu) << 14 | 1u << 31;
        if (k0 == s_sem.cc_key[0] && w0 == s_sem.cc_key[1] && w1 == s_sem.cc_raw1_key) return;
        s_sem.cc_key[0] = k0; s_sem.cc_key[1] = w0; s_sem.cc_raw1_key = w1;
    }
    cc_grp_t r0, a0, r1, a1, r, a;
    cc_cycle(w0, w1, 0, &r0, &a0);
    cc_cycle(w0, w1, 1, &r1, &a1);
    /* the texture first (what either cycle in use reads), then the fold */
    int u0 = cc_uses(&r1, 1u, 0) || cc_uses(&a1, 1u, 1), u1 = cc_uses(&r1, 2u, 0) || cc_uses(&a1, 2u, 1);
    int u1rgb = cc_uses(&r1, 2u, 0);
    if (cyc == 1) {
        u0 |= cc_uses(&r0, 1u, 0) || cc_uses(&a0, 1u, 1);
        u1 |= cc_uses(&r0, 2u, 0) || cc_uses(&a0, 2u, 1);
        u1rgb |= cc_uses(&r0, 2u, 0);
    }
    int rt = s_sem.rtile;
    /* TEXEL1's colour over an intensity TEXEL0 -- only when the colour is
     * TEXEL1's: Majora's name entry morphs two glyphs' alphas under a PRIM
     * colour, and its TEXEL1 tile was the skybox's, left over */
    int tex1 = u1 && (!u0 || (u1rgb && cc_is_intensity(rt) && ((s_sem.tile_set >> ((rt + 1) & 7)) & 1) && !cc_is_intensity(rt + 1)));
    /* (A - B) * SHADE_ALPHA + B of the two textures (Pilotwings' sea: the
     * colour strip in open water, the sand detail on the shore): the
     * triangle's own alpha picks the one it mostly shows (OP_TRI*) */
    {
        const cc_grp_t *g = cyc == 1 ? &r0 : &r1;
        s_sem.cc_pick = g->c != 11u ? 0 : (g->a == 2u && g->b == 1u && g->d == 1u) ? 2
                      : (g->a == 1u && g->b == 2u && g->d == 2u) ? 1 : 0;
        if (s_sem.cc_pick && s_sem.cc_force) tex1 = s_sem.cc_force == 2;
    }
    uint32_t b = tex1 ? 2u : 1u, u = tex1 ? 1u : 2u;
    cc_subst(&r0, b, u, 0); cc_subst(&a0, b, u, 1); cc_subst(&r1, b, u, 0); cc_subst(&a1, b, u, 1);
    if (cyc == 1) { r = cc_fold2(r0, r1, 0); a = cc_fold2(a0, a1, 1); }
    else { r = r1; a = a1; }                                /* 1-cycle: the second cycle's inputs */
    uint32_t n0 = r.a << 20 | r.c << 15 | a.a << 12 | a.c << 9 | r.a << 5 | r.c;
    uint32_t n1 = r.b << 28 | r.b << 24 | a.a << 21 | a.c << 18 | r.d << 15 | a.b << 12 | a.d << 9 | r.d << 6 | a.b << 3 | a.d;
    if (tex1 != s_sem.cc_tex1) { s_sem.cc_tex1 = tex1; s_texbind_dirty = 1; }
    s_cc_w0 = n0; s_cc_w1 = n1;
    s_texenv = combine_to_texenv(n0, n1);
}
#ifdef F3D_RAW_CC
/* F3D_RAW_CC (n64rt): RDP has the N64's combiner, so the combine goes to it
 * as written (GDL_COMBINE), PRIMITIVE and ENVIRONMENT with it, instead of
 * being reduced to a GDL_TEXENV mode with the constants folded into the
 * vertex colours -- which cannot draw (ENV - PRIM) * TEXEL0 + PRIM, a lerp
 * between two constants (Pokemon Stadium's menu backgrounds came out as
 * texel * ENV). s_rawcc_emitted: flags << 32, ~0 = unknown (a new task). */
/* does the combine read the texel (TEXEL0 after cc_resolve's fold)? */
static int cc_reads_texel(void)
{
    uint32_t w0 = s_cc_w0, w1 = s_cc_w1;
    uint32_t f[16] = { (w0 >> 20) & 15, (w0 >> 15) & 31, (w0 >> 12) & 7, (w0 >> 9) & 7, (w0 >> 5) & 15, w0 & 31,
                       (w1 >> 28) & 15, (w1 >> 24) & 15, (w1 >> 21) & 7, (w1 >> 18) & 7, (w1 >> 15) & 7, (w1 >> 12) & 7,
                       (w1 >> 9) & 7, (w1 >> 6) & 7, (w1 >> 3) & 7, w1 & 7 };
    int texel = 0;
    for (int i = 0; i < 16; i++) texel |= f[i] == 1u || f[i] == 2u;
    return texel | (f[1] == 8u || f[1] == 9u || f[5] == 8u || f[5] == 9u);   /* rgb C: TEXELn_ALPHA */
}
/* flags bit3: the game's alpha test -- othermode L's alpha compare [1:0], or
 * CVG_X_ALPHA (0x1000: coverage times alpha, how N64 cut-outs drop their
 * clear texels); Pokemon Stadium's menu background has neither, and the
 * inferred test cut holes in it */
static void emit_rawcc(gdl_cur_t *out, int on)
{
    uint32_t flags = on ? 1u | (cc_reads_texel() ? 2u : 0u) | ((s_othermode_l & 0x1003u) ? 8u : 0u) | s_prim_lodfrac << 8 : 0u;
    uint32_t k[4] = { on ? s_cc_w0 : 0u, on ? s_cc_w1 : 0u, on ? s_prim_rgba : 0u, on ? s_env_rgba : 0u };
    if (s_rawcc_emitted == (uint64_t)flags << 32 && k[0] == s_rawcc_k[0] && k[1] == s_rawcc_k[1]
        && k[2] == s_rawcc_k[2] && k[3] == s_rawcc_k[3]) return;
    gdl_combine(out, flags, k[0], k[1], k[2], k[3]);
    s_rawcc_emitted = (uint64_t)flags << 32;
    for (int i = 0; i < 4; i++) s_rawcc_k[i] = k[i];
}
#endif
/* the constants off for a draw of the translator's own (a fill, a copy-mode
 * rectangle): the vertex colour as it is, and no GDL_COMBINE */
static void emit_consts_off(gdl_cur_t *out)
{
    if (s_cc_emitted >> 32) { gdl_ccolor(out, 0, 0); s_cc_emitted = 0; }
#ifdef F3D_RAW_CC
    emit_rawcc(out, 0);
#endif
}
static void emit_ccolor_if_changed(gdl_cur_t *out) {
#ifdef F3D_RAW_CC
    /* as written -- unless it reads a texel with texturing off (the N64
     * logo's fade: (TEXEL0 - 0) * PRIM on untextured triangles), where the
     * reduction below draws what it always did */
    if (!texgen_shade_only() && (!cc_reads_texel() || (s_tex_on && s_tex_render_size > 0))) {
        if (s_cc_emitted >> 32) { gdl_ccolor(out, 0, 0); s_cc_emitted = 0; }   /* RDP applies them */
        emit_rawcc(out, 1);
        return;
    }
    emit_rawcc(out, 0);
#endif
    /* combine_to_ccolor() divides by 255 per channel: worth ~5 % of the game
     * CPU when it ran for every triangle. Its inputs are the combiner words,
     * the two colours and texgen, so it is recomputed only when one changes. */
    static uint32_t k_w0, k_w1, k_env, k_prim; static int k_tg = -1;
    static uint64_t k_cc;
    int tg = texgen_shade_only();
    if (tg != k_tg || s_cc_w0 != k_w0 || s_cc_w1 != k_w1 || s_env_rgba != k_env || s_prim_rgba != k_prim) {
        k_tg = tg; k_w0 = s_cc_w0; k_w1 = s_cc_w1; k_env = s_env_rgba; k_prim = s_prim_rgba;
        k_cc = tg ? 0 : combine_to_ccolor(s_cc_w0, s_cc_w1);
    }
    uint64_t cc = k_cc;
    if (cc != s_cc_emitted) {
        gdl_ccolor(out, (uint32_t)(cc >> 32), (uint32_t)cc);
        s_cc_emitted = cc;
    }
}

static void emit_geomode_if_changed(gdl_cur_t *out) {
    /* lighting, and whether back faces are culled: SM64 clears G_CULL_BACK
     * for two-sided surfaces (HMC's climbable ceiling mesh, seen from below) */
    int gm = ((s_geomode & GEO_LIGHTING) ? GDL_GEOMODE_LIGHTING : 0)
           | ((s_geomode & GEO_CULL_BACK) ? 0 : GDL_GEOMODE_NOCULL)
           | (texgen_as_written() ? GDL_GEOMODE_TEXGEN : 0);
    if (gm != s_lighting_emitted) { gdl_geomode(out, gm); s_lighting_emitted = gm; }
}
static void emit_texnorm(gdl_cur_t *out) {
    /* only whether texturing is on matters to the geom core's RDP path
     * (GDL_TEXBIND carries the scale): no float divide */
    gdl_w(out, GDL_HDR(GDL_TEXNORM, 0)); gdl_w(out, (s_tex_on && s_tex_render_size > 0) ? 0x10000u : 0u);
}

/* G_TEXRECT: a screen-space textured rectangle (the HUD, menu text, ...).
 * The geom core only draws triangles, so this is two of them, in N64 screen
 * space: vertices in quarter pixels (the rectangle's own 10.2 units) under a
 * temporary identity modelview and an orthographic projection with w = 640,
 * which keeps every matrix entry an integer or 4/3 -- 1/640 would not survive
 * S15.16. The full-screen viewport is set for it and the game's projection
 * and viewport are put back after; the texture is bound without
 * gsSPTexture's scale, which does not apply to rectangles. */
/* The N64 draws 320 x 240; the framebuffer is GEOM_FB_HRES x 240 and fills
 * the Pocket's screen (video.json, square pixels).
 *
 * In play (a Mario object exists: f3d_crop_3d()) every viewport is 1:1 and
 * loses (320 - GEOM_FB_HRES) / 2 pixels per side: 3D keeps its proportions.
 * 2D (an orthographic projection) is kept whole by moving each element
 * instead of shrinking it: SM64 places a dialog, a string, a menu with a
 * matrix PUSH and draws inside it (glyph after glyph by NOPUSH
 * translations), so the translation of the outermost push above the last
 * modelview LOAD -- the element's anchor A -- is moved to GEOM_FB_HRES/320 of
 * its place and everything under it keeps its N64 size: text stays sharp
 * (squeezed, point-sampled glyphs lost columns). emit_mv().
 *
 * The title, file select and act select (no Mario) squeeze their 3D
 * (buttons, Goddard's head) to GEOM_FB_HRES/320; their 2D keeps its size the
 * same way, its anchor squeezed with the layout, so a label stays by its
 * button and its glyphs stay whole (squeezed, point-sampled 8-px menu glyphs
 * lost a column in six: "MAPIO A", "SOPY").
 * G_TEXRECT (the HUD's digits and icons) keeps its size and moves toward the
 * nearer screen edge in play; in the menus the small ones (the 8-px menu
 * font) keep their size with a run's start squeezed, the big ones (SELECT
 * FILE, NEW) are squeezed as a whole and stay centred (emit_texrect()). */
/* the port says whether this screen's 3D is cropped (hal/exec_dl_wrap.c);
 * without one, always */
/* the framebuffer's width: GEOM_FB_HRES (268), or 320 when the game file
 * asks for the N64's own resolution (frame.c frame_set_video) -- every crop,
 * squeeze and HUD move below then comes out 0 */
int f3d_fb_hres = GEOM_FB_HRES;
int f3d_crop_3d(void) __attribute__((weak));
int f3d_crop_3d(void) { return 1; }
/* ... and whether a squeezed screen's 2D keeps its size; without one, yes */
int f3d_keep_2d(void) __attribute__((weak));
int f3d_keep_2d(void) { return 1; }
static int s_keep_2d = 1;
static int keep_2d_now(void) { return s_force_keep_2d || f3d_keep_2d(); }
static void emit_viewport_as(gdl_cur_t *out, const float scale[4], const float trans[4], int crop) {
    float s[4], t[4];
    for (int i = 0; i < 4; i++) { s[i] = scale[i]; t[i] = trans[i]; }
    if (crop) {
        t[0] -= (float)(320 - f3d_fb_hres) / 2.0f;
    } else {
        s[0] *= (float)f3d_fb_hres / 320.0f;
        t[0] *= (float)f3d_fb_hres / 320.0f;
    }
    gdl_viewport(out, s, t);
}
/* 3D: 1:1 where the port crops it; 2D: 1:1 (keeping its size) unless the
 * screen squeezes it whole (the title's backdrop) */
static int vp_crop_eff(void) { return s_proj_ortho ? s_keep_2d : s_vp_crop; }
static void emit_viewport(gdl_cur_t *out, const float scale[4], const float trans[4]) {
    emit_viewport_as(out, scale, trans, vp_crop_eff());
}
/* the modelview top, with a 2D element's anchor moved (see above) */
/* In play, the shift (N64 space, quarter pixels, whole pixels) for a 2D
 * element anchored at N64 x = a (quarter pixels) -- text, boxes, icons alike.
 * The middle of the screen (60..260) is cropped centred: elements keep their
 * N64 distances to each other, so a box centred on the screen stays centred
 * (the castle pause box is anchored at its left corner, x 82, and moved 13 px
 * right under the old proportional rule) and neighbours cannot collide (the
 * course-complete star at 55 against its act name at 76). Near an edge the
 * crop is taken up gradually instead, so nothing is cut off. */
#define PLAY_CROP_Q (((320 - f3d_fb_hres) / 2) * 4)
#define PLAY_EDGE_Q (60 * 4)
static int32_t play_2d_shift_q(int32_t a) {
    int32_t vis;   /* where it lands on the Pocket's screen */
    if (a < PLAY_EDGE_Q)                vis = a * (PLAY_EDGE_Q - PLAY_CROP_Q) / PLAY_EDGE_Q;
    else if (a > 320 * 4 - PLAY_EDGE_Q) vis = f3d_fb_hres * 4 - (320 * 4 - a) * (PLAY_EDGE_Q - PLAY_CROP_Q) / PLAY_EDGE_Q;
    else                                vis = a - PLAY_CROP_Q;
    int32_t sh = vis - a + PLAY_CROP_Q;
    return (sh + (sh >= 0 ? 2 : -2)) / 4 * 4;
}

/* 2D translations folded into the vertices. SM64 draws a string glyph after
 * glyph, each under its own modelview (a translation by the glyph's advance),
 * and as a full matrix LOAD each glyph cost the geom core a 16-word compare,
 * an MVP product and its quantisation: a dialog page added ~20 % to a CCM
 * frame (sim/geom_full, 2026-09-28). Under an orthographic projection, a
 * modelview that differs from the one the geom core holds (s_emv) only by a
 * whole-unit translation, over an identity 3x3, is not sent: the translation
 * goes into the vertices instead (OP_VTX), which lands them exactly where the
 * matrix would have. Undone (the real matrix sent) before a PUSH, since the
 * geom core copies what it holds; forgotten at a POP (the level below is
 * whatever was pushed, not tracked here). */
int f3d_fold_off;                   /* 1: every modelview as a LOAD (A/B) */
static void mv_load(gdl_cur_t *out, const int32_t m[16]) {
    for (int k = 0; k < 16; k++) s_mv_true[k] = m[k];
    if (s_proj_ortho && s_emv_valid && !s_dlc_translating && !f3d_fold_off) {
        int ok = m[0] == 65536 && m[5] == 65536 && m[10] == 65536 && m[15] == 65536
              && !(m[1] | m[2] | m[3] | m[4] | m[6] | m[7] | m[8] | m[9] | m[11]);
        for (int k = 0; k < 12 && ok; k++) ok = m[k] == s_emv[k];
        ok = ok && m[15] == s_emv[15];
        int32_t d[3];
        for (int k = 0; k < 3 && ok; k++) {
            d[k] = m[12 + k] - s_emv[12 + k];
            ok = (d[k] & 0xffff) == 0 && d[k] >= -(4096 << 16) && d[k] <= (4096 << 16);
        }
        if (ok) {
            for (int k = 0; k < 3; k++) s_fold_d[k] = d[k] >> 16;
            s_fold = (d[0] | d[1] | d[2]) != 0;
            return;
        }
    }
    gdl_mtx_load_fx(out, GDL_MTX_TARGET_MODELVIEW, m);
    for (int k = 0; k < 16; k++) s_emv[k] = m[k];
    s_emv_valid = 1; s_fold = 0;
}
static void mv_unfold(gdl_cur_t *out) {
    if (!s_fold) return;
    gdl_mtx_load_fx(out, GDL_MTX_TARGET_MODELVIEW, s_mv_true);
    for (int k = 0; k < 16; k++) s_emv[k] = s_mv_true[k];
    s_fold = 0;
}

static void emit_mv(gdl_cur_t *out) {
    const int32_t *top = s_mv[s_mv_sp];
    if (!(s_proj_ortho && s_keep_2d)) { mv_load(out, top); return; }
    /* No anchor at all -- an identity modelview, nothing pushed on it: the
     * vertices are absolute N64 screen coordinates for the whole 320-px
     * frame (dl_proj_mtx_fullscreen: the star / Mario / Bowser transitions,
     * whose hole is centred at x = 160, and the end screen). That frame is
     * cropped centred, as the 3D is, so the shape stays in the middle of the
     * Pocket's screen; as an element anchored at x = 0 it sat 27 px right. */
    if (s_mv_sp == s_mv_base && top[12] == 0 && top[13] == 0) {
        mv_load(out, top);
        return;
    }
    int32_t m[16];
    for (int k = 0; k < 16; k++) m[k] = top[k];
    int64_t a = (s_mv_sp > s_mv_base) ? s_mv[s_mv_base + 1][12] : top[12];   /* A.x, S15.16 */
    if (s_vp_crop) {   /* in play: see play_2d_shift_q(); anchored where the element was pushed */
        int64_t pa = (s_mv_sp > s_mv_base) ? s_mv_anchor[s_mv_base + 1] : top[12];
        m[12] += play_2d_shift_q((int32_t)(pa >> 14)) * 16384;
        mv_load(out, m);
        return;
    }
    /* menus (squeezed layout): moved by whole pixels, so 1:1 glyphs stay on
     * the pixel grid */
    int64_t d = a * (f3d_fb_hres - 320) / 320;
    d = (d + (d >= 0 ? 32768 : -32768)) / 65536 * 65536;
    m[12] += (int32_t)d + ((320 - f3d_fb_hres) / 2) * 65536;
    mv_load(out, m);
}

/* Two triangles covering x0..x1, y0..y1 (quarter pixels, N64 screen space)
 * under a temporary identity modelview, an orthographic projection with
 * w = 640 and the full-screen viewport; the game's are put back after.
 * Vertex colour rgba (0xRRGGBBAA), texture coordinates s, t (S10.5). */
/* the colour image's size (Mirlo-N64: from G_SETCIMG, GDL_FBSIZE): 320 x 240,
 * or Pokemon Stadium's 640 x 480 menus */
static int32_t s_fb_w = 320, s_fb_h = 240;
/* n / d by shift-and-subtract, 32 steps: no divider on the geom core, and no
 * __divdi3 in the blob */
F3D_COLD static uint32_t udiv32(uint32_t n, uint32_t d) {
    uint32_t q = 0, r = 0;
    for (int i = 31; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1u);
        if (r >= d) { r -= d; q |= 1u << i; }
    }
    return q;
}
static void emit_rect_quad(gdl_cur_t *out, int32_t xl, int32_t yl, int32_t xh, int32_t yh,
                           int32_t s0, int32_t t0, int32_t s1, int32_t t1, uint32_t rgba, int crop) {
    gdl_geomode(out, GDL_GEOMODE_NOCULL);        /* unlit, two-sided */
    s_lighting_emitted = -1;

    static const int32_t ident[16] = { 65536, 0, 0, 0,  0, 65536, 0, 0,  0, 0, 65536, 0,  0, 0, 0, 65536 };
    int32_t ortho[16] = { 65536, 0, 0, 0,  0, -87381, 0, 0,  0, 0, 0, 0,
                          -640 * 65536, 640 * 65536, 0, 640 * 65536 };
    float vps[4] = { 160.0f, 120.0f, 127.75f, 0.0f };   /* the full screen, as SM64's own viewport */
    if (s_fb_w != 320 || s_fb_h != 240) {        /* a larger screen: quarter pixels stay 1:1 */
        ortho[0] = (int32_t)udiv32(20971520u, (uint32_t)s_fb_w);     /* 65536 * 320 / w */
        ortho[5] = -(int32_t)udiv32(20971440u, (uint32_t)s_fb_h);    /* -87381 * 240 / h */
        vps[0] = (float)s_fb_w * 0.5f; vps[1] = (float)s_fb_h * 0.5f;
    }
    gdl_mtx_push(out);
    gdl_mtx_load_fx(out, GDL_MTX_TARGET_MODELVIEW, ident);
    gdl_mtx_load_fx(out, GDL_MTX_TARGET_PROJECTION, ortho);
    emit_viewport_as(out, vps, vps, crop);
    geom_vtx_t v[4];
    memset(v, 0, sizeof v);
    const int32_t x[4] = { xl, xh, xh, xl }, y[4] = { yl, yl, yh, yh };
    const int32_t sc[4] = { s0, s1, s1, s0 }, tc[4] = { t0, t0, t1, t1 };
    for (int i = 0; i < 4; i++) {
        v[i].ob[0] = (int16_t)x[i]; v[i].ob[1] = (int16_t)y[i];
        v[i].tc[0] = (int16_t)sc[i]; v[i].tc[1] = (int16_t)tc[i];
        v[i].cn[0] = (uint8_t)(rgba >> 24); v[i].cn[1] = (uint8_t)(rgba >> 16);
        v[i].cn[2] = (uint8_t)(rgba >> 8);  v[i].cn[3] = (uint8_t)rgba;
    }
    gdl_vtx(out, 28, v, 4);
    gdl_tri2(out, 28, 29, 30, 28, 30, 31);
    gdl_mtx_pop(out, 1);
    if (s_proj_valid) gdl_mtx_load_fx(out, GDL_MTX_TARGET_PROJECTION, s_proj);
    if (s_vp_valid) emit_viewport(out, s_vp_scale, s_vp_trans);
}

/* The right edge (10.2) of the run of rectangles on row y that starts with
 * the G_TEXRECT at dl: a string's glyphs follow each other in the display
 * list, each with its texture load (print_menu_generic_string()), so the
 * scan walks forward over those commands only. */
static int32_t texrect_run_end(const f3d_word_t *dl, int32_t y, int32_t xend, int32_t gap,
                               const f3d_word_t **next) {   /* next: the rectangle past the gap, if any */
    if (next) *next = 0;
    for (int n = 0; n < 1024; n++) {
        uint32_t w0 = dl->w0, op = (w0 >> 24) & 0xffu;
        if (op == OP_TEXRECT) {
            int32_t yl = (int32_t)(dl->w1 & 0xfff), xh = (int32_t)((w0 >> 12) & 0xfff);
            if (yl >= y - 8 && yl <= y + 8) {
                if ((int32_t)((dl->w1 >> 12) & 0xfff) > xend + gap) {       /* a gap: another string */
                    if (next) *next = dl;
                    break;
                }
                if (xh > xend) xend = xh;
            }
            dl += 3;
            continue;
        }
        if (op != OP_SETTIMG && op != 0xe6 /* LOADSYNC */ && op != OP_LOADBLOCK && op != 0xf4 /* LOADTILE */
            && op != OP_SETTILE && op != OP_SETTILESIZE && op != 0xe7 /* PIPESYNC */ && op != 0xe8 /* TILESYNC */
            && op != OP_SETPRIMCOLOR && op != OP_SETENVCOLOR
            /* a call (not a branch): the HUD font loads each glyph through
             * one (add_glyph_texture()'s dl_hud_img_load_tex_block) */
            && !(op == 0xde && ((w0 >> 16) & 0xffu) == 0u))
            break;
        dl++;
    }
    return xend;
}

/* In play, the HUD's bands (the top and bottom 40 px: lives, coins, stars,
 * the camera): a run left of the centre moves right by cut, right of it left.
 * Anywhere else a run moves as 2D text does (emit_mv): its LEFT edge -- the
 * text's anchor -- to GEOM_FB_HRES/320 of its place, less the crop, in whole
 * pixels. Two things this fixes on the course-complete screen: the star glyph
 * (a rectangle, x 55) sits 5 px before the act name (text, x 76), and moved
 * +20 against the text's +14 it touched it; and the coin count, growing to
 * the right as it counts up, moved its run's centre across the third's edge
 * at 10 coins and the whole row jumped 20 px. By the left edge it stays put,
 * as on the N64. */
static int32_t texrect_run_shift(int32_t xl, int32_t xe, int32_t cut, int32_t y) {
    int32_t xc = (xl + xe) / 2;
    /* The HUD's top row (hud.c, HUD_TOP_Y: y 15 on screen) by group, from its
     * left edge: lives (x 22) +12, stars (x 242) -8, and coins (x 168) +7 --
     * clear of the power meter, a 2D element centred at x 140 (108..172 on
     * the N64, +3 here) that the old -20 put the coin under (pause menu) --
     * or further left only as far as a 3-digit count needs to stay 2 px
     * before the stars. */
    if (y < 30 * 4) {
        if (xl < 100 * 4) return 12 * 4;
        if (xl >= 220 * 4) return -8 * 4;
        int32_t room = (242 - 8 - 2) * 4 - xe;
        return room < 7 * 4 ? (room < -cut ? -cut : room & ~3) : 7 * 4;
    }
    if (y < 40 * 4 || y > (240 - 40) * 4)
        return xc < (160 - 16) * 4 ? cut : xc > (160 + 16) * 4 ? -cut : 0;
    return play_2d_shift_q(xl);
}

static void emit_texrect(gdl_cur_t *out, const f3d_word_t *dl, uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2) {
    int32_t xh = (int32_t)((w0 >> 12) & 0xfff), yh = (int32_t)(w0 & 0xfff);
    int32_t xl = (int32_t)((w1 >> 12) & 0xfff), yl = (int32_t)(w1 & 0xfff);
    int32_t s0 = (int16_t)(half1 >> 16), t0 = (int16_t)half1;          /* S10.5 */
    int32_t dsdx = (int16_t)(half2 >> 16), dtdy = (int16_t)half2;      /* S5.10 */
    if (((s_othermode_h >> 20) & 3u) == 2u) {   /* copy mode: 4 texels per step, edges inclusive */
        dsdx /= 4; xh += 4; yh += 4;
    }
    if (xh <= xl || yh <= yl) return;
    /* Point-sampled: shift the texture half a pixel, so each pixel samples
     * the middle of its texel. The rasterizer evaluates a pixel at its corner, which
     * for a 1:1 rectangle is exactly a texel boundary, and its fixed-point
     * interpolation lands a hair below it -- floor() then took each row's
     * texel from the row above, and an 8-px menu glyph lost its bottom row
     * (the host's float sampler never did). */
    /* (only for a rectangle drawn 1:1 on the pixel grid -- see `keep` below;
     * a squeezed one samples between texels anyway) */
    int keep = s_keep_2d && (s_vp_crop || (yh - yl) <= 16 * 4);   /* 16: the HUD font (file select's NEW) */
    /* F3D_PIXEL_CENTRES (RDP): the rasterizer samples at pixel centres, as
     * the N64 does, so a 1:1 rectangle's pixels already land mid-texel; this
     * half-texel shift then moved every pixel to the NEXT texel and the last
     * column onto the texel past the glyph ("PRESS START" grew a ':' and a
     * '!', 2026-09-27) */
    if (keep && (((s_othermode_h >> 20) & 3u) == 2u || ((s_othermode_h >> 12) & 3u) == 0u)) {
        s0 += dsdx >> 6; t0 += dtdy >> 6;             /* half a pixel of S5.10 steps, in S10.5 */
    }
    int32_t s1 = s0 + ((dsdx * (xh - xl)) >> 7), t1 = t0 + ((dtdy * (yh - yl)) >> 7);
    /* In play the rectangle keeps its size (a 1:1 viewport, as the 3D) and
     * moves: the HUD's icons and digits hug the screen edges, so a run of
     * rectangles on the left third moves right by the pixels the crop takes
     * off each side, on the right third left by as much, in the middle not at
     * all -- by runs (same row, touching), since moving each digit on its own
     * would pile them up. */
    int32_t shift = 0;
    /* in the menus a glyph-sized rectangle keeps its size too (see above) */
    if (!s_vp_crop && keep) {
        if (!(yl >= s_rect_run_y - 8 && yl <= s_rect_run_y + 8
              && xl >= s_rect_run_x0 && xl <= s_rect_run_xend + 64)) {
            /* the run's centre moves to GEOM_FB_HRES/320 of its place (a
             * centred string stays centred); the 1:1 viewport then takes
             * (320 - GEOM_FB_HRES) / 2 off */
            /* anchored at its left edge, as SM64 writes it -- a label beside a
             * squeezed box (file select's MARIO A) kept its N64 gap; by its
             * centre its unsqueezed width pushed it onto the box. A string
             * centred on the screen (SELECT FILE) stays centred. */
            int32_t xe = texrect_run_end(dl + 3, yl, xh, 64, 0), xc = (xl + xe) / 2;
            int32_t xa = (xc > (160 - 24) * 4 && xc < (160 + 24) * 4) ? xc : xl;
            s_rect_run_shift = xa * (f3d_fb_hres - 320) / 320 + (320 - f3d_fb_hres) / 2 * 4;
            /* whole pixels: a glyph drawn 1:1 must start on a pixel, or its
             * columns straddle pixel corners (see the half-texel shift) */
            s_rect_run_shift = (s_rect_run_shift + (s_rect_run_shift >= 0 ? 2 : -2)) / 4 * 4;
            s_rect_run_x0 = xl; s_rect_run_y = yl;
        }
        s_rect_run_xend = xh;
        shift = s_rect_run_shift;
    } else if (keep && f3d_fb_hres < 320) {   /* (at 320 the HUD sits where the N64 has it) */
        /* 20 of the 27 cropped pixels: on the narrower screen the HUD sits a
         * little nearer the edges than on the N64's (owner's call).
         * A run here is one HUD group -- icon, x, digits, which touch or
         * overlap -- and moves by where its whole extent sits: left of the
         * centre right, right of it left, centred (PAUSE) not at all. Split
         * only at a real gap: 123 coins end 4 px before the star icon, and as
         * one run the stars did not move and ran off the screen; judged by
         * the first glyph alone, TIME stayed while its digits moved onto it. */
        const int32_t cut = 20 * 4, gap = 2 * 4;                     /* quarter pixels */
        if (!(yl >= s_rect_run_y - 8 && yl <= s_rect_run_y + 8
              && xl >= s_rect_run_x0 && (xl <= s_rect_run_xend + gap || xl <= s_rect_run_xplan))) {
            /* Two runs on the row that would move INTO each other are one:
             * the course-complete "HI SCORE" is centred, but its space leaves
             * the same 4-px gap as between two HUD groups, and as two runs
             * "HI" (left of centre) moved 20 px right onto "SCORE" (centred,
             * unmoved) -- the S covered the I. Runs that keep their distance
             * or move apart (coins, stars) stay separate. */
            const f3d_word_t *nx;
            int32_t xe = texrect_run_end(dl + 3, yl, xh, gap, &nx);
            int32_t sh = texrect_run_shift(xl, xe, cut, yl);
            while (nx) {
                int32_t nxl = (int32_t)((nx->w1 >> 12) & 0xfff), nxh = (int32_t)((nx->w0 >> 12) & 0xfff);
                const f3d_word_t *nx2;
                int32_t ne = texrect_run_end(nx + 3, yl, nxh, gap, &nx2);
                if ((nxl + texrect_run_shift(nxl, ne, cut, yl)) - (xe + sh) > 0) break;   /* still apart */
                xe = ne; sh = texrect_run_shift(xl, xe, cut, yl); nx = nx2;
            }
            s_rect_run_shift = sh;
            s_rect_run_x0 = xl; s_rect_run_y = yl; s_rect_run_xplan = xe;
        }
        s_rect_run_xend = xh;
        shift = s_rect_run_shift;
    }
    xl += shift; xh += shift;

    int tex_on = s_tex_on; uint32_t ss = s_tex_sscale, ts = s_tex_tscale;
    const int32_t rtile = s_sem.rtile;
    s_sem.rtile = (int32_t)((w1 >> 24) & 7u);       /* a rectangle names its own tile */
    s_tex_on = 1; s_tex_sscale = s_tex_tscale = 0x10000u; s_texbind_dirty = 1;
    /* Copy mode (the HUD) writes texels as they are: no combiner, no Z, and
     * the alpha compare drops the transparent ones -- as a texel-only blend. */
    int copy = ((s_othermode_h >> 20) & 3u) == 2u;
    /* a rectangle is the RDP's alone: the RSP's geometry mode (texture gen,
     * lighting) does not apply to it */
    uint32_t gm_saved = s_geomode;
    s_geomode &= ~GEO_TEXTURE_GEN;
    uint32_t te = s_texenv, rm = s_rm_flags;
    if (copy) {
        s_texenv = GDL_TE_RGB_TEXEL | GDL_TE_A_TEXEL;
        s_rm_flags = GDL_RM_XLU;
        emit_consts_off(out);
    } else {
        emit_ccolor_if_changed(out);
    }
    emit_texenv_if_changed(out);
    emit_rendermode_if_changed(out);
    {   /* the texels it samples, tile-relative (emit_texbind_if_needed's crop):
         * S10.5 less the tile's u10.2 origin, one more for the filter */
        int32_t sa = s0 < s1 ? s0 : s1, sb = s0 < s1 ? s1 : s0, ta = t0 < t1 ? t0 : t1, tb = t0 < t1 ? t1 : t0;
        const int rt = s_sem.rtile;
        const int32_t sl = ((s_sem.tsz_set >> rt) & 1) ? (int32_t)((s_sem.tsz_w0[rt] >> 12) & 0xfffu) : 0;
        const int32_t tl = ((s_sem.tsz_set >> rt) & 1) ? (int32_t)(s_sem.tsz_w0[rt] & 0xfffu) : 0;
        s_rect_u0 = ((sa >> 3) - sl) >> 2; s_rect_u1 = (((sb >> 3) - sl) >> 2) + 1;
        s_rect_v0 = ((ta >> 3) - tl) >> 2; s_rect_v1 = (((tb >> 3) - tl) >> 2) + 1;
        if (s_rect_u0 < 0) s_rect_u0 = 0;
    }
    emit_texbind_if_needed(out);
    s_rect_u0 = -1;
    s_texenv = te; s_rm_flags = rm;
    s_geomode = gm_saved;
    s_tex_on = tex_on; s_tex_sscale = ss; s_tex_tscale = ts; s_texbind_dirty = 1;
    s_sem.rtile = rtile;
    emit_rect_quad(out, xl, yl, xh, yh, s0, t0, s1, t1, 0xffffffffu, keep);
}

/* S2DEX's G_BG_COPY / G_BG_1CYC (uObjBg / uObjScaleBg at w1): a picture
 * larger than TMEM (OoT's pre-rendered rooms, 320x240 16-bit) drawn as strips
 * of whole texel rows, each a rectangle of the image (GDL_TEXBIND_EXT's
 * pitch and first texel, its origin the strip's). BG_COPY draws texels as
 * they are, BG_1CYC through the combiner, scaled by scaleW/scaleH (u5.10).
 * The picture's own wrap (a frame running past its edge) is not done. */
F3D_COLD static void emit_bg(gdl_cur_t *out, int copy, uint32_t w1) {
    const uint8_t *b = (const uint8_t *)F3D_PTR(w1);
    uint32_t ix = (uint16_t)N64_H(b, 0), iw = (uint16_t)N64_H(b, 1), iy = (uint16_t)N64_H(b, 4), ih = (uint16_t)N64_H(b, 5);
    int32_t fx = N64_H(b, 2), fy = N64_H(b, 6);
    uint32_t fw = (uint16_t)N64_H(b, 3), fh = (uint16_t)N64_H(b, 7);
    uint32_t img = (uint32_t)N64_B(b, 16) << 24 | (uint32_t)N64_B(b, 17) << 16 | (uint32_t)N64_B(b, 18) << 8 | N64_B(b, 19);
    uint32_t fmt = N64_B(b, 22) & 7u, siz = N64_B(b, 23) & 3u, pal = (uint16_t)N64_H(b, 12);
    uint32_t scw = copy ? 1024u : (uint16_t)N64_H(b, 14), sch = copy ? 1024u : (uint16_t)N64_H(b, 15);
    if (!scw) scw = 1024u;
    if (!sch) sch = 1024u;
    uint32_t W = iw >> 2, H = ih >> 2;                        /* texels */
    uint32_t sx = ix >> 5, sy = iy >> 5;                      /* the first texel shown */
    uint32_t tw = (fw * scw) >> 12, th = (fh * sch) >> 12;    /* texels the frame spans (u10.2 * u5.10) */
    if (!W || !H || !tw || !th || sx >= W || sy >= H) return;
    if (sx + tw > W) tw = W - sx;
    if (sy + th > H) th = H - sy;
    if (tw > 1024u) return;
    /* rows a strip can hold: 4096 texels, and RDP's TMEM line limit */
    uint32_t rows = udiv_small(4096u, tw);
    while (rows > 1u && ((rows + 1u) >> 1) * ((tw + 3u) >> 2) * 2u > 1024u) rows--;
    if (!rows) return;
    uintptr_t src = (uintptr_t)F3D_PTR(img), tlut = 0;
    int flags = copy ? GDL_TEXBIND_POINT : ((((s_othermode_h >> 12) & 3u) == 0u) ? GDL_TEXBIND_POINT : 0);
    if (fmt == 2u) {                                         /* CI: the last LOADTLUT's palette */
        tlut = s_tlut_ptr;
        int32_t first = (siz == 0 ? (int32_t)pal * 16 : 0) - s_sem.tlut_start;
        if (tlut && first > 0) tlut += (uintptr_t)first * 2u;
        if (((s_othermode_h >> 14) & 3u) == 3u) flags |= GDL_TEXBIND_TLUT_IA;
    }
    uint32_t te = s_texenv, rm = s_rm_flags;
    if (copy) {
        s_texenv = GDL_TE_RGB_TEXEL | GDL_TE_A_TEXEL;
        s_rm_flags = GDL_RM_XLU;
        emit_consts_off(out);
    } else {
        emit_ccolor_if_changed(out);
    }
    emit_texenv_if_changed(out);
    emit_rendermode_if_changed(out);
    for (uint32_t t0 = sy; t0 < sy + th; t0 += rows) {
        uint32_t n = sy + th - t0 < rows ? sy + th - t0 : rows;
        gdl_texsub_t sub = { W, t0 * W + sx, sx * 4u, t0 * 4u };
        gdl_texbind_q(out, (int)fmt, (int)siz, 2, 2, (int)((pal & 15u) | (uint32_t)flags), (int)tw, (int)n,
                      (const void *)src, (const void *)tlut, 0x10000u, 0x10000u, &sub);
        /* the strip on the screen: rows t0..t0+n of the picture, in 10.2 */
        int32_t y0 = fy + (int32_t)udiv_small((t0 - sy) << 12, sch), y1 = fy + (int32_t)udiv_small((t0 + n - sy) << 12, sch);
        int32_t x1 = fx + (int32_t)udiv_small(tw << 12, scw);
        emit_rect_quad(out, fx, y0, x1, y1, (int32_t)(sx << 5), (int32_t)(t0 << 5),
                       (int32_t)((sx + tw) << 5), (int32_t)((t0 + n) << 5), 0xffffffffu, 1);
    }
    s_texenv = te; s_rm_flags = rm;
    s_texbind_dirty = 1; s_tri_state_ok = 0;
}

/* ---- Translation cache for static display lists ----------------------------
 * SM64's models and level geometry are display lists in read-only memory that
 * never change, yet every frame walked them command by command out of SDRAM
 * -- on the target that is most of f3d_emit's ~35 ms (the game CPU's cache
 * misses; the x86 host does the same walk in ~25 us). The N64 did the same
 * walk every frame too, on the RSP, with DMA.
 *
 * A gSPDisplayList to a static list is translated once into a chunk of GDL in
 * the arena, ended by GDL_ENDDL, and from then on the frame's GDL only calls
 * it (GDL_DL, which the geom core already runs). The translation depends on
 * the state it starts in (texture, tiles, combiner, render mode, geometry
 * mode, lights: f3d_sem_t), so the entry state is the key along with the
 * list's address, and the state it leaves behind is stored and restored on a
 * hit. A chunk does not rely on what the geom core had been sent before: the
 * "last emitted" trackers start unknown inside it, so it states what it uses.
 *
 * Not cached: a list that loads matrices, sets a viewport, draws rectangles
 * or fills (they depend on the matrix stack and the screen layout), or whose
 * vertices, textures, lights or sub-lists are not static themselves -- such a
 * list is remembered as uncacheable and walked as before.
 *
 * The arena is two halves used in turn: when one fills up the other is
 * cleared and taken, and the chunks in the full one stay valid while the
 * frames already built on them are still being drawn (at most two frames:
 * pipelined mode), so a half is never reused within three frames. */
typedef struct {
    uint64_t cc_emitted;
    uint32_t texenv_emitted, rm_emitted;
    int32_t  lighting_emitted, texbind_emitted, drawn;
} f3d_emitted_t;
/* word-wise: picolibc's memcmp/memcpy go byte by byte, and on the target a
 * lookup's compares showed up in the profile */
#define SEM_WORDS ((uint32_t)(sizeof(f3d_sem_t) / 4u))
static int sem_eq(const f3d_sem_t *a, const f3d_sem_t *b) {
    const uint32_t *x = (const uint32_t *)a, *y = (const uint32_t *)b;
    uint32_t d = 0;                    /* (no early exit: an unrolled or-chain is cheaper on the N64's CPU) */
#pragma GCC unroll 16
    for (uint32_t i = 0; i < SEM_WORDS; i++) d |= x[i] ^ y[i];
    return d == 0;
}
static void sem_copy(f3d_sem_t *d, const f3d_sem_t *s_) {
    uint32_t *x = (uint32_t *)d; const uint32_t *y = (const uint32_t *)s_;
#pragma GCC unroll 16
    for (uint32_t i = 0; i < SEM_WORDS; i++) x[i] = y[i];
}
static uint32_t sem_hash(const f3d_sem_t *a) {
    const uint32_t *x = (const uint32_t *)a;
    /* a pre-filter only (sem_eq decides): rotate-xor, no multiply chain */
    uint32_t h = 0x9e3779b9u;
#pragma GCC unroll 16
    for (uint32_t i = 0; i < SEM_WORDS; i++) h = ((h << 5) | (h >> 27)) ^ x[i];
    return h;
}
/* Interned states. Every state a cache entry starts or ends in is stored once
 * in the arena (sem_node_t, found by hash), so an entry holds two pointers
 * and a lookup compares pointers. s_sem_ref is the interned copy of the
 * current state, or 0 when a command may have changed it since. A hit does
 * not copy its end state into s_sem either: s_sem_stale says s_sem still has
 * to be loaded from s_sem_ref, which walk() does before the first command
 * that reads or writes the state -- a parent list mostly calls its children
 * back to back, with only matrices in between. */
typedef struct sem_node {
    uint32_t  next;              /* word offset of the next node for this hash, 0 = none */
    uint32_t  hash;
    f3d_sem_t sem;
} sem_node_t;
#define SEM_NODE_WORDS ((uint32_t)((sizeof(sem_node_t) + 7u) / 8u * 2u))
#ifdef F3D_NO_DLC                /* no display-list cache (the geom core: no room) */
#define SEM_POOL_HASH 1u
#else
#define SEM_POOL_HASH 1024u
#endif
static uint32_t          s_sem_pool[SEM_POOL_HASH];
static const f3d_sem_t  *s_sem_ref;
static int               s_sem_stale;
static void sem_sync(void) {
    if (s_sem_stale) { sem_copy(&s_sem, s_sem_ref); s_sem_stale = 0; }
}
static void sem_dirty(void) { sem_sync(); s_sem_ref = 0; }
static void sem_set(const f3d_sem_t *ref) { s_sem_ref = ref; s_sem_stale = 1; s_tri_state_ok = 0; }
static void emitted_save(f3d_emitted_t *t) {
    memset(t, 0, sizeof *t);
    t->cc_emitted = s_cc_emitted; t->texenv_emitted = s_texenv_emitted; t->rm_emitted = s_rm_emitted;
    t->lighting_emitted = s_lighting_emitted; t->texbind_emitted = s_texbind_emitted; t->drawn = s_drawn;
}
/* The state a triangle emits (texenv, combiner colour, render mode, texture
 * bind) is checked once per run of triangles: s_tri_state_ok says nothing
 * those checks read has changed since the last triangle -- any command but a
 * triangle or a vertex load clears it, and so does anything that sets the
 * emitted state wholesale. (Checking per triangle was ~20 % of translating
 * Goddard's face on the game's CPU.) */
static void emitted_load(const f3d_emitted_t *t) {
    s_tri_state_ok = 0;
    s_cc_emitted = t->cc_emitted; s_texenv_emitted = t->texenv_emitted; s_rm_emitted = t->rm_emitted;
    s_lighting_emitted = t->lighting_emitted; s_texbind_emitted = t->texbind_emitted; s_drawn = t->drawn;
}
static void emitted_unknown(void) {
    s_tri_state_ok = 0;
    s_cc_emitted = ~0ull; s_texenv_emitted = 0xFFFFFFFFu; s_rm_emitted = 0xFFFFFFFFu;
    s_lighting_emitted = -1; s_texbind_emitted = 0;
}

/* The platform says what is static and lends the arena; without it (the
 * host tests, other firmwares) nothing is cached. */
int f3d_dlc_static(const void *p) __attribute__((weak));
int f3d_dlc_static(const void *p) { (void)p; return 0; }
void *f3d_dlc_arena(uint32_t *bytes) __attribute__((weak));
void *f3d_dlc_arena(uint32_t *bytes) { *bytes = 0; return 0; }

typedef struct dlc_entry {
    const void *dl;
    uint32_t    next;            /* word offset of the next entry for this hash, 0 = none */
    uint32_t    nwords;          /* chunk words; 0 with DLC_NEVER */
    uint32_t    flags;
    const f3d_sem_t *in, *out;   /* interned (sem_intern) */
    f3d_emitted_t out_em;
    uint32_t    words[];         /* the chunk, GDL_ENDDL last */
} dlc_entry_t;
#define DLC_NEVER   1u           /* uncacheable: walk it */
#ifdef F3D_NO_DLC
#define DLC_HASH    1u
#else
#define DLC_HASH    4096u
#endif
#define DLC_VARIANTS 32          /* entry states kept per list before giving up on it (16 left CCM with ~4 lists a frame walked in full) */
int f3d_dlc_off;                 /* 1: walk everything (A/B measurement) */
static uint32_t  s_dlc_hash[DLC_HASH];   /* word offset of the newest entry, 0 = none */
static uint32_t *s_dlc_base;             /* this half */
static uint32_t  s_dlc_words, s_dlc_used; /* half size, used (words 0..1 are never an entry) */
static int       s_dlc_half = -1;
static unsigned  s_dlc_frame, s_dlc_switch_frame;
static int       s_dlc_translating;      /* walking into a chunk */
static int       s_dlc_abort;            /* ... which cannot be cached */
unsigned long    f3d_dlc_hits, f3d_dlc_misses, f3d_dlc_never, f3d_dlc_words_saved, f3d_dlc_switches;
unsigned long    f3d_dlc_toomany, f3d_dlc_cmps;   /* past DLC_VARIANTS; entry-state compares */

static uint32_t dlc_h(const void *dl) { uintptr_t a = (uintptr_t)dl; return (uint32_t)((a >> 3) ^ (a >> 13)) & (DLC_HASH - 1u); }
static void dlc_take_half(int half) {
    uint32_t bytes; uint32_t *base = (uint32_t *)f3d_dlc_arena(&bytes);
    s_dlc_words = (bytes / 8u) & ~1u;
    s_dlc_base = base + (uint32_t)half * s_dlc_words;
    sem_dirty();                          /* s_sem_ref points into the old half */
    s_dlc_half = half; s_dlc_used = 2u;   /* entries at even words: they hold 64-bit fields */
    memset(s_dlc_hash, 0, DLC_HASH * 4u);
    memset(s_sem_pool, 0, SEM_POOL_HASH * 4u);
    s_dlc_switch_frame = s_dlc_frame;
}
static int dlc_enabled(void) {
    if (s_dlc_half < 0) {
        uint32_t bytes; if (!f3d_dlc_arena(&bytes) || bytes < 65536u) return 0;
        dlc_take_half(0);
    }
    return 1;
}
#define DLC_HDR_WORDS ((uint32_t)((sizeof(dlc_entry_t) + 3u) / 4u))

/* the interned copy of s_sem (s_sem_ref), added to this half if new; 0 if
 * there is no room */
static const f3d_sem_t *sem_intern(void) {
    if (s_sem_ref) return s_sem_ref;
    uint32_t hash = sem_hash(&s_sem), b = hash & (SEM_POOL_HASH - 1u);
    for (uint32_t o = s_sem_pool[b]; o; ) {
        sem_node_t *n = (sem_node_t *)(s_dlc_base + o);
        if (n->hash == hash && sem_eq(&n->sem, &s_sem)) return s_sem_ref = &n->sem;
        o = n->next;
    }
    if (s_dlc_words - s_dlc_used < SEM_NODE_WORDS) return 0;
    uint32_t off = s_dlc_used;
    sem_node_t *n = (sem_node_t *)(s_dlc_base + off);
    s_dlc_used += SEM_NODE_WORDS;
    n->next = s_sem_pool[b]; n->hash = hash;
    sem_copy(&n->sem, &s_sem);
    s_sem_pool[b] = off;
    return s_sem_ref = &n->sem;
}

/* bytes a cached chunk may depend on: static (its first and its last byte,
 * the platform's pages being larger than any one dependency), or the chunk
 * is abandoned */
static void dlc_need_static(uintptr_t p, uint32_t bytes) {
    if (s_dlc_translating && (!f3d_dlc_static((const void *)p) || !f3d_dlc_static((const void *)(p + bytes - 1u))))
        s_dlc_abort = 1;
}

/* Everything cached forgotten (the platform saw a dependency change). */
void f3d_dlc_flush(void) {
    if (s_dlc_half < 0) return;
    dlc_take_half(0);
    s_dlc_switch_frame = s_dlc_frame - 3u;       /* (the other half is free at once) */
}

static void walk(const f3d_word_t *dl, gdl_cur_t *out, int depth);

/* A gSPDisplayList to tgt: through the cache when it is static. */
static void call_dl(const f3d_word_t *tgt, gdl_cur_t *out, int depth) {
#ifdef F3D_NO_DLC
    walk(tgt, out, depth + 1);
    return;
#endif
    /* (a folded translation goes into the vertices: not into a cached chunk) */
    if (s_dlc_translating || s_fold || f3d_dlc_off || f3d_gbi1 == F3D_UC_S2DEX || !f3d_dlc_static(tgt) || !dlc_enabled()) { walk(tgt, out, depth + 1); return; }
    uint32_t h = dlc_h(tgt), variants = 0;
    const f3d_sem_t *in = 0;
    int have_in = 0;
    for (uint32_t o = s_dlc_hash[h]; o; ) {
        dlc_entry_t *e = (dlc_entry_t *)(s_dlc_base + o);
        if (e->dl == tgt) {
            if (e->flags & DLC_NEVER) { walk(tgt, out, depth + 1); return; }
            if (!have_in) { in = sem_intern(); have_in = 1; }
            f3d_dlc_cmps++;
            if (e->in == in && in) {                              /* hit */
                gdl_w(out, GDL_HDR(GDL_DL, 0));
                gdl_w(out, GDL_PTR(e->words));
                sem_set(e->out);
                int drawn = s_drawn;
                emitted_load(&e->out_em);
                s_drawn |= drawn;
                f3d_dlc_hits++; f3d_dlc_words_saved += e->nwords;
                return;
            }
            variants++;
        }
        o = e->next;
    }
    /* miss: translate into a new entry at the end of this half */
    if (!have_in) in = sem_intern();
    uint32_t room = s_dlc_words - s_dlc_used;
    if (!in || variants >= DLC_VARIANTS || room < DLC_HDR_WORDS + SEM_NODE_WORDS + 4096u) {
        if (variants < DLC_VARIANTS && s_dlc_frame - s_dlc_switch_frame >= 3u) {
            dlc_take_half(s_dlc_half ^ 1);                    /* the other half, cleared */
            f3d_dlc_switches++;
            call_dl(tgt, out, depth);
            return;
        }
        if (variants >= DLC_VARIANTS) f3d_dlc_toomany++;
        walk(tgt, out, depth + 1);                            /* no room this frame: as before */
        return;
    }
    uint32_t off = s_dlc_used;
    dlc_entry_t *e = (dlc_entry_t *)(s_dlc_base + off);
    f3d_emitted_t em0;
    emitted_save(&em0);
    int drawn0 = s_drawn;
    emitted_unknown();
    s_drawn = 0;
    /* room is kept for the end state's node */
    gdl_cur_t sub = { e->words, s_dlc_base + s_dlc_words - SEM_NODE_WORDS };
    s_dlc_translating = 1; s_dlc_abort = 0;
    walk(tgt, &sub, depth + 1);
    s_dlc_translating = 0;
    gdl_w(&sub, GDL_HDR(GDL_ENDDL, 0));
    if (!s_dlc_abort && sub.p > sub.end) {
        /* did not fit in what is left of this half: put the state back, and
         * take the other half if it is free, else walk it this time */
        sem_set(in);
        emitted_load(&em0);
        s_drawn = drawn0;
        if (s_dlc_frame - s_dlc_switch_frame >= 3u && s_dlc_used > 2u) {
            dlc_take_half(s_dlc_half ^ 1);
            f3d_dlc_switches++;
            call_dl(tgt, out, depth);
        } else {
            walk(tgt, out, depth + 1);
        }
        return;
    }
    e->dl = tgt;
    e->next = s_dlc_hash[h];
    if (s_dlc_abort) {
        /* uncacheable: remember that, put the state back and walk it for real */
        e->flags = DLC_NEVER; e->nwords = 0;
        e->in = e->out = 0;
        s_dlc_used += DLC_HDR_WORDS;
        s_dlc_hash[h] = off;
        sem_set(in);
        emitted_load(&em0);
        s_drawn = drawn0;
        f3d_dlc_never++;
        walk(tgt, out, depth + 1);
        return;
    }
    e->flags = 0;
    e->nwords = (uint32_t)(sub.p - e->words);
    e->in = in;
    emitted_save(&e->out_em);
    s_drawn |= drawn0;
    s_dlc_used += (DLC_HDR_WORDS + e->nwords + 1u) & ~1u;
    s_dlc_hash[h] = off;
    e->out = sem_intern();                                    /* fits: room kept above */
    f3d_dlc_misses++;
    gdl_w(out, GDL_HDR(GDL_DL, 0));
    gdl_w(out, GDL_PTR(e->words));
}

/* The game's RDP commands that set where and how it draws go to RDP as they
 * are -- RDP speaks the RDP's command format -- in order with the geometry
 * (GDL_RAW): colour/depth image, fill colour, fill rectangle, scissor.
 * Addresses are the RDRAM's on RDP's bus; RDP's colour is RGB565, so a
 * fill of the colour image takes its 5551 pixels to 565 (a depth fill keeps
 * its value). */
static void rdp_raw(gdl_cur_t *out, uint32_t hi, uint32_t lo)
{
    gdl_w(out, GDL_HDR(GDL_RAW, 2));
    gdl_w(out, hi);
    gdl_w(out, lo);
}
static uint32_t c5551_to_565(uint32_t p)
{
    uint32_t r = (p >> 11) & 31u, g = (p >> 6) & 31u, b = (p >> 1) & 31u;
    return r << 11 | (g << 1 | g >> 4) << 5 | b;
}
#define RDP_OP_SET_SCISSOR 0x2Du
#define RDP_OP_SET_OTHER   0x2Fu
#define RDP_OP_FILL_RECT   0x36u
#define RDP_OP_SET_FILL    0x37u
#define RDP_OP_SET_ZIMG    0x3Eu
#define RDP_OP_SET_CIMG    0x3Fu
static int n64_fillrect(gdl_cur_t *out, uint32_t w0, uint32_t w1)
{
    if (((s_othermode_h >> 20) & 3u) != 3u) return 0;         /* not G_CYC_FILL: the quad path */
    uint32_t fill = s_fill_color;
    if (!(s_zimg && s_cimg == s_zimg))
        fill = c5551_to_565(fill >> 16) << 16 | c5551_to_565(fill & 0xFFFFu);
    rdp_raw(out, RDP_OP_SET_OTHER << 24 | 3u << 20, 0);       /* fill; every draw sets its own mode */
    rdp_raw(out, RDP_OP_SET_FILL << 24, fill);
    rdp_raw(out, RDP_OP_FILL_RECT << 24 | (w0 & 0xFFFFFFu), w1);
    s_drawn = 1;
    return 1;
}

/* G_FILLRECT in fill mode (see f3d_clear_valid). Other cycle types take the
 * colour from the combiner; SM64 only uses them in its debug profiler bars. */
static void emit_fillrect(gdl_cur_t *out, uint32_t w0, uint32_t w1) {
    if (((s_othermode_h >> 20) & 3u) != 3u) return;          /* not G_CYC_FILL */
    if (s_zimg && s_cimg == s_zimg) return;                   /* the depth buffer's clear */
    int32_t xh = (int32_t)((w0 >> 12) & 0xfff) + 4, yh = (int32_t)(w0 & 0xfff) + 4;   /* inclusive */
    int32_t xl = (int32_t)((w1 >> 12) & 0xfff),     yl = (int32_t)(w1 & 0xfff);
    if (xh <= xl || yh <= yl) return;
    uint32_t p = s_fill_color >> 16;                          /* RGBA5551 */
    uint32_t r = (p >> 11) & 31u, g = (p >> 6) & 31u, b = (p >> 1) & 31u;
    r = r << 3 | r >> 2; g = g << 3 | g >> 2; b = b << 3 | b >> 2;
    if (!s_drawn && xl <= 0 && yl <= 0 && xh >= 320 * 4 && yh >= 240 * 4) {
        f3d_clear_valid = 1;
        f3d_clear_rgb = r << 16 | g << 8 | b;
        return;
    }
    /* drawn: shade only, opaque, no depth -- and the game's state put back */
    uint32_t te = s_texenv, rm = s_rm_flags;
    s_texenv = GDL_TE_RGB_SHADE | GDL_TE_A_SHADE;
    s_rm_flags = 0;
    emit_consts_off(out);
    emit_texenv_if_changed(out);
    emit_rendermode_if_changed(out);
    gdl_texnorm(out, 0.0f);                                   /* texturing off */
    s_texenv = te; s_rm_flags = rm;
    emit_rect_quad(out, xl, yl, xh, yh, 0, 0, 0, 0, r << 24 | g << 16 | b << 8 | 0xffu, s_vp_crop);
    emit_texnorm(out);
    s_texbind_dirty = 1;
    s_drawn = 1;
}

/* Fast3D (GBI 1: SM64 US's own microcode) is walked as F3DEX2: each command is
 * rewritten to its F3DEX2 (GBI 2) form before the decode below. The two
 * share the RDP commands (0xC0..0xFF); the rest moves
 * or packs differently (include/PR/gbi.h, !F3DEX_GBI_2). An unknown GBI 1
 * command becomes a no-op rather than whatever GBI 2 command has its number. */
int f3d_gbi1;
int f3d_texlog;                                     /* (host: log each texture bind) */

int f3d_ucode_kind(const uint8_t *p, uint32_t n, uint32_t x) {
    /* "RSP Gfx ucode F3DEX  fifo 2.08", "RSP Gfx ucode F3DLX 0.95" -- and
     * OoT's "RSP Gfx[Safe] S2DEX       fifo 2.05", with no "ucode " */
    static const char key[] = "RSP Gfx";
    if (n > 0x1000u) n = 0x1000u;
    for (uint32_t i = 0; i + sizeof key + 40u <= n; i++) {
        uint32_t j = 0;
        while (j < sizeof key - 1u && p[(i + j) ^ x] == (uint8_t)key[j]) j++;
        if (j < sizeof key - 1u) continue;
        uint32_t s = i + j;
        for (uint32_t k = s; k < s + 24u; k++)      /* S2DEX anywhere in the name */
            if (p[k ^ x] == 'S' && p[(k + 1u) ^ x] == '2' && p[(k + 2u) ^ x] == 'D') return F3D_UC_S2DEX;
        if (p[s ^ x] == ' ' && p[(s + 1u) ^ x] == 'u') s += 7u;   /* " ucode ": the name, then its version */
        for (uint32_t k = s; k + 1u < n && k < s + 64u; k++) {
            uint8_t c = p[k ^ x];
            if (c >= '0' && c <= '9' && p[(k + 1u) ^ x] == '.') return c >= '2' ? F3D_UC_GBI2 : F3D_UC_F3DEX1;
            if (c == 0) break;
        }
        return F3D_UC_GBI2;
    }
    return F3D_UC_F3D;                              /* Fast3D names itself "RSP SW Version: 2.0x" */
}

static uint32_t gbi1_geomode(uint32_t m) {
    uint32_t r = m & ~(0x200u | 0x1000u | 0x2000u);
    if (m & 0x200u)  r |= 0x200000u;               /* G_SHADING_SMOOTH */
    if (m & 0x1000u) r |= 0x200u;                  /* G_CULL_FRONT */
    if (m & 0x2000u) r |= 0x400u;                  /* G_CULL_BACK */
    return r;
}
static void gbi1_to_gbi2(uint32_t *pw0, uint32_t *pw1) {
    uint32_t w0 = *pw0, w1 = *pw1, op = w0 >> 24;
    if (op >= 0xC0u && op < 0xE0u) return;         /* none in GBI 1 below the RDP's */
    if (op >= 0xE4u) return;                        /* RDP commands: the same */
    switch (op) {
    case 0x01: {                                    /* G_MTX: p = PROJECTION 1 | LOAD 2 | PUSH 4 */
        uint32_t p = (w0 >> 16) & 0xFFu;
        w0 = 0xDAu << 24 | ((p & 1u) ? MTX_PROJECTION : 0u) | (p & MTX_LOAD) | ((p & 4u) ? 0u : MTX_PUSH);
        break;
    }
    case 0x03: {                                    /* G_MOVEMEM: viewport 0x80, lookat 0x82/0x84, lights 0x86.. */
        uint32_t idx = (w0 >> 16) & 0xFFu;
        if (idx == 0x80u) w0 = 0xDCu << 24 | MV_VIEWPORT;
        else if (idx == 0x82u) w0 = 0xDCu << 24 | (24u / 8u) << 8 | MV_LIGHT;
        else if (idx == 0x84u) w0 = 0xDCu << 24 | MV_LIGHT;
        else if (idx >= 0x86u && idx <= 0x94u) w0 = 0xDCu << 24 | (((idx - 0x86u) / 2u * 24u + 48u) / 8u) << 8 | MV_LIGHT;
        else w0 = 0xE0u << 24;
        break;
    }
    case 0x04: {                                    /* G_VTX */
        uint32_t n, v0;
        if (f3d_gbi1 == F3D_UC_F3DEX1) { n = (w0 >> 10) & 0x3Fu; v0 = ((w0 >> 16) & 0xFFu) >> 1; }   /* v0*2 << 16 | n << 10 | len-1 */
        else { n = ((w0 >> 20) & 0xFu) + 1u; v0 = (w0 >> 16) & 0xFu; }                              /* ((n-1) << 4 | v0) << 16 */
        w0 = 0x01u << 24 | n << 12 | ((v0 + n) & 0x7Fu) << 1;
        break;
    }
    case 0x06: w0 = 0xDEu << 24 | (w0 & 0x00FF0000u); break;                /* G_DL (push / branch) */
    case 0xB3: w0 = 0xF1u << 24 | (w0 & 0xFFFFFFu); break;                  /* G_RDPHALF_2 */
    case 0xB4: w0 = 0xE1u << 24 | (w0 & 0xFFFFFFu); break;                  /* G_RDPHALF_1 */
    case 0xB6: w0 = 0xD9u << 24 | (~gbi1_geomode(w1) & 0xFFFFFFu); w1 = 0; break;   /* G_CLEARGEOMETRYMODE */
    case 0xB7: w0 = 0xD9u << 24 | 0xFFFFFFu; w1 = gbi1_geomode(w1); break;  /* G_SETGEOMETRYMODE */
    case 0xB8: w0 = 0xDFu << 24; break;                                     /* G_ENDDL */
    case 0xB9: case 0xBA: {                         /* G_SETOTHERMODE_L / _H: sft << 8 | len */
        uint32_t sft = (w0 >> 8) & 0xFFu, len = w0 & 0xFFu;
        w0 = (op == 0xBAu ? 0xE3u : 0xE2u) << 24 | ((32u - sft - len) & 0xFFu) << 8 | ((len - 1u) & 0xFFu);
        break;
    }
    case 0xBB: w0 = 0xD7u << 24 | (w0 & 0x3F00u) | ((w0 & 0xFFu) ? 2u : 0u); break;   /* G_TEXTURE: on in bit 0 */
    case 0xBC: {                                    /* G_MOVEWORD: offset [23:8], index [7:0] (GBI 2: index [23:16]) */
        uint32_t idx = w0 & 0xFFu, ofs = (w0 >> 8) & 0xFFFFu;
        if (idx == MW_NUMLIGHT) w1 = (((w1 & 0x7FFFFFFFu) / 32u) - 1u) * 24u;   /* NUML: (n+1)*32 + 0x80000000 */
        else if (idx == MW_LIGHTCOL) ofs = ofs / 32u * 24u + 48u;                /* light n at (n-1)*32 */
        w0 = 0xDBu << 24 | idx << 16 | ofs;
        break;
    }
    case 0xBD: w0 = 0xD8u << 24; w1 = 64; break;                            /* G_POPMTX: one modelview */
    case 0xB0:                                      /* F3DEX 1: G_BRANCH_Z -> F3DEX2's (the same fields) */
        if (f3d_gbi1 != F3D_UC_F3DEX1) { w0 = 0xE0u << 24; break; }
        w0 = 0x04u << 24 | (w0 & 0xFFFFFFu);
        break;
    case 0xB1:                                      /* F3DEX 1: G_TRI2, both triangles' indices * 2 */
        if (f3d_gbi1 != F3D_UC_F3DEX1) { w0 = 0xE0u << 24; break; }
        w0 = 0x06u << 24 | (w0 & 0xFFFFFFu); w1 &= 0xFFFFFFu;
        break;
    case 0xB5:                                      /* F3DEX 1: G_QUAD v0 v1 v2 v3 (* 2) -> (v0 v1 v2) (v0 v2 v3) */
        if (f3d_gbi1 != F3D_UC_F3DEX1) { w0 = 0xE0u << 24; break; }
        w0 = 0x06u << 24 | (w1 & 0xFFFFFF00u) >> 8;
        w1 = (w1 >> 24) << 16 | (w1 & 0xFFFFu);
        break;
    case 0xBF: {                                    /* G_TRI1: vertex index * 10 in w1 (F3DEX 1: * 2) */
        if (f3d_gbi1 == F3D_UC_F3DEX1) { w0 = 0x05u << 24 | (w1 & 0xFFFFFFu); w1 = 0; break; }
        /* x / 10 as (x * 205) >> 11, exact below 1029: a divide is ~35 cycles on the N64's CPU */
        uint32_t a = (((w1 >> 16) & 0xFFu) * 205u) >> 11, b = (((w1 >> 8) & 0xFFu) * 205u) >> 11, c = ((w1 & 0xFFu) * 205u) >> 11;
        w0 = 0x05u << 24 | (a * 2u) << 16 | (b * 2u) << 8 | (c * 2u);
        w1 = 0;
        break;
    }
    default: w0 = 0xE0u << 24; break;               /* G_SPNOOP, G_CULLDL, the rest: nothing to draw */
    }
    *pw0 = w0; *pw1 = w1;
}

/* F3D_ON_GEOM: called at each command, where everything before out->p is
 * complete (MIRLO's geom_f3d.c walks the translation a piece at a time) */
#ifdef F3D_ON_GEOM
void f3d_cmd_boundary(gdl_cur_t *out);
extern uint32_t *f3d_chunk_mark;           /* f3d_cmd_boundary() is due once out->p reaches it */
#endif
static void walk(const f3d_word_t *dl, gdl_cur_t *out, int depth) {
    if (depth > DL_DEPTH) {
        if (s_trace_task) printf("  f3d: DL_DEPTH exceeded at %p\n", (void *)dl);
        return;
    }
    if (!IN_SDRAM(dl)) {
        if (s_trace_task) printf("  f3d: bad DL ptr %p (depth %d) -- bail\n", (void *)dl, depth);
        return;
    }
    for (;;) {
#ifdef F3D_ON_GEOM
        if (out->p >= f3d_chunk_mark) f3d_cmd_boundary(out);
#endif
        if (++s_walk_iters > WALK_ITER_CAP) {
            printf("  f3d: WALK_ITER_CAP hit, dl=%p depth=%d w0=%08lx -- bail\n",
                   (void *)dl, depth, (unsigned long)dl->w0);
            return;
        }
        if (!IN_SDRAM(dl)) {
            printf("  f3d: walked off SDRAM to %p (depth %d) -- bail\n", (void *)dl, depth);
            return;
        }
        uint32_t  w0 = dl->w0;
        uintptr_t w1 = dl->w1;
        if (f3d_gbi1 == F3D_UC_GBI2 || f3d_gbi1 == F3D_UC_S2DEX) {
            uint32_t o_ = w0 >> 24;
            if (o_ == 0xE1u) s_rdphalf1 = (uint32_t)w1;   /* G_RDPHALF_1: G_LOAD_UCODE's data */
            else if (o_ == 0xDDu) {                     /* G_LOAD_UCODE: the rest of the task speaks another */
                f3d_gbi1 = f3d_ucode_kind((const uint8_t *)F3D_PTR(s_rdphalf1), 0x800u, F3D_UCODE_X);
                if (s_dlc_translating) s_dlc_abort = 1; /* (a cached translation would not replay the switch) */
                w0 = 0xE0u << 24;
            } else if (f3d_gbi1 == F3D_UC_S2DEX && (o_ == 0x09u || o_ == 0x0Au)) {   /* G_BG_1CYC, G_BG_COPY */
                emit_bg(out, o_ == 0x0Au, (uint32_t)w1);
                w0 = 0xE0u << 24;
            }
        }
        if (f3d_gbi1 == F3D_UC_S2DEX) {             /* S2DEX: its objects/backgrounds not yet; keep lists, segments, the RDP's */
            uint32_t o = w0 >> 24;
            if (o < 0xDBu || o == 0xDCu || o == 0xDDu) w0 = 0xE0u << 24;
        } else if (f3d_gbi1) { uint32_t w1_ = (uint32_t)w1; gbi1_to_gbi2(&w0, &w1_); w1 = w1_; }
        else if ((w0 >> 24) == 0x07u) w0 = (w0 & 0xFFFFFFu) | (uint32_t)OP_TRI2 << 24;   /* F3DEX2 G_QUAD: G_TRI2's two triangles (OoT) */
        uint32_t op = (w0 >> 24) & 0xff;
        if (f3d_statelog) {
            /* (f3d_statelog) the state a task sets, and how many triangles
             * each state draws: a whole frame in a few hundred lines */
            static uint32_t ntri;
            if (op == OP_TRI1 || op == OP_TRI2) ntri += op == OP_TRI2 ? 2u : 1u;
            else if (op == OP_SETCOMBINE || op == OP_SETPRIMCOLOR || op == OP_SETENVCOLOR || op == OP_SETOTHERMODE_L
                     || op == OP_SETOTHERMODE_H || op == OP_SETFILLCOLOR || op == OP_SETTIMG || op == OP_SETTILE
                     || op == OP_LOADBLOCK || op == OP_LOADTILE || op == OP_LOADTLUT || op == OP_SETTILESIZE
                     || op == OP_TEXRECT || op == OP_FILLRECT || op == OP_SETCIMG || op == OP_SETZIMG
                     || op == OP_GEOMETRYMODE || op == OP_TEXTURE || op == 0xE1u || op == 0xF1u) {
                if (ntri) { printf("   %lu tris\n", (unsigned long)ntri); ntri = 0; }
                printf(" %02lx %08lx %08lx\n", (unsigned long)op, (unsigned long)w0, (unsigned long)w1);
            } else if (op == OP_ENDDL && depth == 0 && ntri) { printf("   %lu tris\n", (unsigned long)ntri); ntri = 0; }
        }
        if (s_trace_task && s_walk_iters <= 160)
            printf("  f3d[%ld] d%d %p op=%02lx w0=%08lx w1=%08lx\n",
                   s_walk_iters, depth, (void *)dl, (unsigned long)op,
                   (unsigned long)w0, (unsigned long)w1);

        if (op != OP_TRI1 && op != OP_TRI2 && op != OP_VTX) s_tri_state_ok = 0;
        if (op != OP_DL && op != OP_ENDDL && op != OP_MTX && op != OP_POPMTX) {
            sem_sync();                              /* reads the state: see s_sem_ref */
            if (op != OP_VTX) s_sem_ref = 0;         /* ... and may change it */
        }
        if (s_dlc_translating) {
            /* what a cached chunk cannot hold: it depends on the matrix
             * stack, the viewport or the screen layout */
            if (op == OP_MTX || op == OP_POPMTX || op == OP_TEXRECT || op == OP_FILLRECT
                || op == OP_SETCIMG || op == OP_SETZIMG
                || (op == OP_MOVEMEM && (w0 & 0xff) == MV_VIEWPORT)) {
                s_dlc_abort = 1;
                return;
            }
            /* what it reads: the list itself, and through its pointers the
             * vertices, a texture image (up to TMEM's 4 KiB), a light */
            dlc_need_static((uintptr_t)dl, 8u);
            if (op == OP_VTX) dlc_need_static((uintptr_t)F3D_PTR(w1), (((w0 >> 12) & 0xffu) + 1u) * 16u);
            else if (op == OP_SETTIMG) dlc_need_static((uintptr_t)F3D_PTR(w1), 4096u);
            else if (op == OP_MOVEMEM) dlc_need_static((uintptr_t)F3D_PTR(w1), ((w0 >> 19) & 0x1fu) * 8u + 8u);
            if (s_dlc_abort) return;
        }
        switch (op) {
        case OP_SPNOOP:
            if (w1 == F3D_TAG_KEEP_2D) {
                /* and SM64's full-screen viewport, 1:1: the title never sets
                 * one, so its backdrop's squeezed rectangles left theirs */
                static const float vp_full[4] = { 160.0f, 120.0f, 127.75f, 0.0f };
                s_force_keep_2d = 1;
                for (int k = 0; k < 4; k++) { s_vp_scale[k] = vp_full[k]; s_vp_trans[k] = vp_full[k]; }
                s_vp_valid = 1;
                emit_viewport_as(out, vp_full, vp_full, 1);
            }
            break;
        case OP_MTX: {
            uint32_t flags = (w0 & 0x7) ^ MTX_PUSH;
            int32_t m[16];                  /* S15.16 */
            /* Hardware's GDLDRAW at t=200 came back half zeros, with
             * 42c162f8 and 40bffd00 where matrix elements should be. Those
             * two are NOT pointers, whatever an earlier note here said --
             * they are the float encodings of 96.693 and 5.9996, the
             * matrix's own values; reading them as addresses is a
             * coincidence of bit patterns and sent the search after an
             * address-map problem that does not exist. So either this
             * pointer is wrong or the 64 bytes behind it are not a matrix.
             * The host is clean at the same frame and its gfx pool never runs
             * out (0 NULL allocations, 76 KB spare), so record what the
             * pointer actually is and what is behind it. */
            f3d_mtx_seen++;
            /* The SDRAM range check only means anything on the target: on the
             * host these are 64-bit heap pointers and every one of them would
             * count as bad, which also suppressed the raw dump and left the
             * comparison one-sided. */
            int ptr_bad = 0;
            if (ptr_bad) {
                if (f3d_mtx_badptr++ == 0) f3d_mtx_first_badptr = w1;
            }
            /* NOTE: gMatStackIndex must NOT be sampled here. This walk runs
             * after the graph traversal has finished and the index is back at
             * 0 -- measured on the host, it reads 0 every time and says
             * nothing. It is sampled in __wrap_alloc_display_list instead,
             * which runs during the traversal, at the moment the matrices are
             * allocated. */
            mtx_to_fixed(F3D_PTR(w1), m);
            /* Capture the BAD ones, not the first four. The first bad matrix
             * on hardware is index 3 and the first two are perfect
             * identities, so a "first four modelviews" dump spends three of
             * its four slots on matrices that are already known good. What is
             * needed is the pointer that produced a degenerate matrix and the
             * 64 bytes it aims at: inverting mtx_to_fixed on the geom core's
             * copy says those bytes have the shape of a Vtx (ob=(96,1,5),
             * flag=0, tc=(1,0)), i.e. the pointer is aimed at vertex data.
             * This dump is what confirms or kills that. */
            if (m[15] != 65536 && !(flags & MTX_PROJECTION)) {
                f3d_mtx_degenerate++;
                if (!ptr_bad && f3d_mtx_raw_n < 4) {
                    const uint32_t *raw = (const uint32_t *)F3D_PTR(w1);
                    f3d_mtx_raw_addr[f3d_mtx_raw_n] = w1;
                    for (int k = 0; k < 16; k++) f3d_mtx_raw[f3d_mtx_raw_n][k] = raw[k];
                    f3d_mtx_raw_n++;
                }
            }
            int target = (flags & MTX_PROJECTION) ? GDL_MTX_TARGET_PROJECTION : GDL_MTX_TARGET_MODELVIEW;
            if (flags & MTX_PROJECTION) {
                if (flags & MTX_LOAD) { for (int k = 0; k < 16; k++) s_proj[k] = m[k]; s_proj_valid = 1; }
                else if (s_proj_valid) mtx_premul_fx(s_proj, m);
                else { for (int k = 0; k < 16; k++) s_proj[k] = m[k]; s_proj_valid = 1; }
                gdl_mtx_load_fx(out, target, s_proj);
                /* w = z * m[11] + m[15]: an orthographic matrix has m[11] == 0 */
                int ortho = s_proj[11] == 0, crop = f3d_crop_3d(), eff = vp_crop_eff();
                int ortho_changed = ortho != s_proj_ortho;
                int keep_changed = keep_2d_now() != s_keep_2d;
                s_vp_crop = crop; s_proj_ortho = ortho; s_keep_2d = keep_2d_now();
                if (vp_crop_eff() != eff && s_vp_valid) emit_viewport(out, s_vp_scale, s_vp_trans);
                if (ortho_changed || keep_changed) emit_mv(out);
            } else {
                if (flags & MTX_PUSH) {
                    mv_unfold(out);                 /* the geom core pushes what it holds */
                    gdl_mtx_push(out);
                    if (s_mv_sp + 1 < F3D_MV_DEPTH) {
                        for (int k = 0; k < 16; k++) s_mv[s_mv_sp + 1][k] = s_mv[s_mv_sp][k];
                        s_mv_sp++;
                    }
                }
                int32_t *top = s_mv[s_mv_sp];
                if (flags & MTX_LOAD) { for (int k = 0; k < 16; k++) top[k] = m[k]; s_mv_base = s_mv_sp; }
                else mtx_premul_fx(top, m);
                if (flags & (MTX_PUSH | MTX_LOAD)) s_mv_anchor[s_mv_sp] = top[12];
                emit_mv(out);
            }
            break;
        }
        case OP_POPMTX: {
            int n = (int)(w1 >> 6);
            gdl_mtx_pop(out, n);
            s_emv_valid = 0; s_fold = 0;            /* the level below: not tracked */
            for (int k = n ? n : 1; k > 0 && s_mv_sp > 0; k--) s_mv_sp--;
            if (s_mv_base > s_mv_sp) s_mv_base = s_mv_sp;
            break;
        }

        case OP_GEOMETRYMODE:
            s_geomode = (s_geomode & (w0 & 0xffffff)) | w1;
            emit_geomode_if_changed(out);
            break;

        case OP_MOVEMEM:
            if ((w0 & 0xff) == MV_VIEWPORT) {
                const int16_t *vp = (const int16_t *)F3D_PTR(w1);
                float scale[4], trans[4];
                for (int i = 0; i < 4; i++) { scale[i] = (float)N64_H(vp, i) / 4.0f; trans[i] = (float)N64_H(vp, 4 + i) / 4.0f; }
                emit_viewport(out, scale, trans);
                for (int i = 0; i < 4; i++) { s_vp_scale[i] = scale[i]; s_vp_trans[i] = trans[i]; }
                s_vp_valid = 1;
            } else if ((w0 & 0xff) == MV_LIGHT) {
                /* F3DEX2 light DMEM: G_MVO_LOOKATX=0, LOOKATY=24, L0=48, L1=72...
                 * so directional/ambient lights are ofs >= 48; ofs 0/24 are the
                 * look-at vectors for env-mapping (not handled here). */
                uint32_t ofs = ((w0 >> 8) & 0xff) * 8u;
                if (ofs == 0u || ofs == 24u) {
                    /* gSPLookAt: the X (ofs 0) and Y (24) directions G_TEXTURE_GEN maps
                     * the normals against -- a Light_t, direction at bytes 8..10 */
                    const uint8_t *L = (const uint8_t *)F3D_PTR(w1);
                    gdl_light(out, ofs ? GDL_LIGHT_LOOKAT_Y : GDL_LIGHT_LOOKAT_X, 0, s_numlights,
                              0, (int8_t)N64_B(L, 8), (int8_t)N64_B(L, 9), (int8_t)N64_B(L, 10));
                } else if (ofs >= 48u) {
                    int slot = (int)((ofs - 48u) / 24u);       /* 0-based light index */
                    const uint8_t *L = (const uint8_t *)F3D_PTR(w1);    /* Light_t: col[3] pad colc[3] pad dir[3] pad */
                    uint32_t rgb = s_lightcol[slot & 7]
                                 ? s_lightcol[slot & 7]
                                 : ((uint32_t)N64_B(L, 0) << 16) | ((uint32_t)N64_B(L, 1) << 8) | N64_B(L, 2);
                    int is_amb = (slot >= s_numlights);        /* the light after the last directional */
                    gdl_light(out, slot & 7, is_amb, s_numlights,
                              rgb, (int8_t)N64_B(L, 8), (int8_t)N64_B(L, 9), (int8_t)N64_B(L, 10));
                }
            }
            break;

        case OP_MOVEWORD: {
            /* F3DEX2 gsMoveWd: index in bits[23:16], offset in bits[15:0]. */
            uint32_t mw_idx = (w0 >> 16) & 0xff;
            uint32_t mw_ofs = w0 & 0xffff;
            if (mw_idx == 0x06u) {                  /* G_MW_SEGMENT: gsSPSegment(ofs / 4, base) */
                s_seg[(mw_ofs >> 2) & 15u] = (uint32_t)w1 & 0xFFFFFFu;
                break;
            }
            if (mw_idx == MW_NUMLIGHT) {
                int n = (int)(w1 / 24u);            /* NUML(n) = n*24 */
                if (n < 1) n = 1;
                if (n > 7) n = 7;
                s_numlights = n;
            } else if (mw_idx == MW_LIGHTCOL) {
                /* offset G_MWO_a<n> = (n*24)+G_MVO_L0 for a-copy; light slot: */
                int slot = (int)((mw_ofs >= 48u ? (mw_ofs - 48u) : 0u) / 24u);
                if (slot >= 0 && slot < 8)
                    s_lightcol[slot] = ((w1 >> 8) & 0x00FFFFFFu);  /* w1 = 0xRRGGBBAA */
            }
            break;
        }

        case OP_SETCOMBINE:
            s_sem.cc_raw0 = w0; s_sem.cc_raw1 = (uint32_t)w1;
            s_sem.cc_force = 0;
            cc_resolve();
            break;
        case OP_SETENVCOLOR:
            s_env_rgba = (uint32_t)w1;
            break;
        case OP_SETPRIMCOLOR:
            s_prim_rgba = (uint32_t)w1;
#ifdef F3D_RAW_CC
            s_prim_lodfrac = w0 & 0xFFu;
#endif
            break;

        case OP_SETOTHERMODE_L: {
            /* F3DEX2 gSPSetOtherMode(G_SETOTHERMODE_L, sft, len, data): w0 [15:8] =
             * 32 - sft - len, [7:0] = len - 1; w1 the bits already in place. (F3D
             * used 0xB9 with sft and len stored directly -- written that way first,
             * it never matched, and every render mode was silently dropped.) SM64
             * sets the render mode per layer this way (rendering_graph_node.c). */
            uint32_t len = (w0 & 0xFFu) + 1u;
            uint32_t sft = 32u - ((w0 >> 8) & 0xFFu) - len;
            uint32_t mask = (len >= 32u ? 0xFFFFFFFFu : ((1u << len) - 1u)) << sft;
            s_othermode_l = (s_othermode_l & ~mask) | ((uint32_t)w1 & mask);
            s_rm_flags = rm_flags_of(s_othermode_l);
            break;
        }
        case 0xef: {   /* G_RDPSETOTHERMODE: both words whole, H in w0[23:0] (OoT's setup lists) */
            uint32_t was = s_othermode_h;
            s_othermode_h = w0 & 0xFFFFFFu;
            s_othermode_l = (uint32_t)w1;
            s_rm_flags = rm_flags_of(s_othermode_l);
            if ((was ^ s_othermode_h) & (3u << 20)) cc_resolve();
            break;
        }

        case OP_TEXTURE:
            /* F3DEX2 gsSPTexture: on/off is bit1 of w0. */
            s_tex_on = ((w0 >> 1) & 0x1) != 0 || (w0 & 0xff) != 0;
            if (s_sem.rtile != (int32_t)((w0 >> 8) & 7u)) { s_sem.rtile = (int32_t)((w0 >> 8) & 7u); s_texbind_dirty = 1; cc_resolve(); }
            /* w1 = s scale [31:16], t scale [15:0]; 0xFFFF is 1.0. The coins ask for 0x8000:
             * their vertices span 62 texels of a 32-texel texture, which only comes out as one
             * coin at half scale. Ignoring it tiled the coin 2x2. */
            if (s_tex_on) {
                uint32_t ss = ((uint32_t)w1 >> 16) & 0xffff, ts = (uint32_t)w1 & 0xffff;
                s_tex_sscale = ss >= 0xffffu ? 0x10000u : ss;
                s_tex_tscale = ts >= 0xffffu ? 0x10000u : ts;
            }
            s_texbind_dirty = 1;
            emit_texnorm(out);
            break;
        case OP_SETTILESIZE: {
            uint32_t uls = (w0 >> 12) & 0xfff, ult = w0 & 0xfff;
            uint32_t lrs = (w1 >> 12) & 0xfff, lrt = w1 & 0xfff;
            s_ts_w = (int)((lrs - uls) / 4u) + 1;
            s_ts_h = (int)((lrt - ult) / 4u) + 1;
            s_sem.tsz_w0[(w1 >> 24) & 7u] = w0; s_sem.tsz_w1[(w1 >> 24) & 7u] = (uint32_t)w1;
            s_sem.tsz_set |= 1 << ((w1 >> 24) & 7u);
            s_tex_render_size = (lrs > lrt ? lrs : lrt) / 4 + 1;
            s_texbind_dirty = 1;
            emit_texnorm(out);
            break;
        }
        case OP_SETTILE:
            /* w0: fmt[23:21] siz[20:19] line[17:9] tmem[8:0]
             * w1: tile[26:24] palette[23:20] cmt[19:18] maskt[17:14]
             *     shiftt[13:10] cms[9:8] masks[7:4] shifts[3:0] */
            if (((w1 >> 24) & 0x7) == 0) {   /* G_TX_RENDERTILE: the real texel format */
                s_tile_fmt   = (int)((w0 >> 21) & 0x7);
                s_tile_siz   = (int)((w0 >> 19) & 0x3);
                s_tile_valid = 1;
            }
            s_tile_cmt   = (int)((w1 >> 18) & 0x3);
            s_tile_maskt = (int)((w1 >> 14) & 0xf);
            s_tile_cms   = (int)((w1 >> 8)  & 0x3);
            s_tile_masks = (int)((w1 >> 4)  & 0xf);
            s_tile_pal   = (int)((w1 >> 20) & 0xf);
            s_sem.tile_tmem[(w1 >> 24) & 7u] = (int32_t)(w0 & 0x1ffu);
            s_sem.tile_w0[(w1 >> 24) & 7u] = w0; s_sem.tile_w1[(w1 >> 24) & 7u] = (uint32_t)w1;
            s_sem.tile_set |= 1 << ((w1 >> 24) & 7u);
            s_texbind_dirty = 1;
            if ((((w1 >> 24) - (uint32_t)s_sem.rtile) & 7u) <= 1u) cc_resolve();   /* TEXEL0's or TEXEL1's format */
            break;
        case OP_LOADBLOCK:
        case OP_LOADTILE:
            record_load(op, w0, (uint32_t)w1);
            s_texbind_dirty = 1;
            break;
        case OP_SETTIMG:
            /* w0: fmt[23:21] siz[20:19] width-1[11:0] ; w1: image pointer */
            s_timg_ptr = (uintptr_t)F3D_PTR(w1);
            s_timg_fmt = (int)((w0 >> 21) & 0x7);
            s_timg_siz = (int)((w0 >> 19) & 0x3);
            s_sem.timg_w = (int32_t)(w0 & 0xfffu) + 1;
            s_texbind_dirty = 1;
            break;
        case OP_LOADTLUT:
            /* the SETTIMG that preceded a LOADTLUT was the palette, not the
             * texture -- stash it and let the next SETTIMG be the real image. */
            s_tlut_ptr = s_timg_ptr;
            s_sem.tlut_start = s_sem.tile_tmem[(w1 >> 24) & 7u] - 256;   /* the TLUT is TMEM's upper half, an entry a 64-bit word */
            if (s_sem.tlut_start < 0) s_sem.tlut_start = 0;
            s_texbind_dirty = 1;
            break;

        case OP_VTX: {
            uint32_t n = (w0 >> 12) & 0xff;
            uint32_t v0 = ((w0 >> 1) & 0x7f) - n;
            const uint8_t *src = (const uint8_t *)F3D_PTR(w1);
            for (uint32_t i = 0; i < n; i++) s_valpha[(v0 + i) & 63u] = N64_B(src + i * 16, 15);   /* (tri_pick) */
            if (s_fold) {   /* a folded translation that would overflow a vertex: send the matrix */
                for (uint32_t i = 0; i < n && s_fold; i++) {
                    const int16_t *ob = (const int16_t *)(src + i * 16);
                    for (int k = 0; k < 3; k++) {
                        int32_t c = (int32_t)N64_H(ob, k) + s_fold_d[k];
                        if (c < -32768 || c > 32767) { mv_unfold(out); break; }
                    }
                }
            }
            emit_geomode_if_changed(out);
            gdl_w(out, GDL_HDR(GDL_VTX, (n << 8) | (v0 & 0xff)));
            uint32_t *vfirst = out->p;
            if (((uintptr_t)src & 3u) == 0 && out->p + n * GDL_VTX_WORDS <= out->end) {
                /* word copies: picolibc's memcpy goes byte by byte */
                const uint32_t *sw = (const uint32_t *)src;
                uint32_t *dw = out->p;
                for (uint32_t i = 0; i < n * GDL_VTX_WORDS; i++) dw[i] = sw[i];
                out->p += n * GDL_VTX_WORDS;
            } else
            for (uint32_t i = 0; i < n; i++) {
                if (out->p + GDL_VTX_WORDS <= out->end) memcpy(out->p, src + i * 16, 16);
                out->p += GDL_VTX_WORDS;
            }
            if (s_fold && out->p <= out->end) {
                for (uint32_t i = 0; i < n; i++) {
                    int16_t *ob = (int16_t *)(vfirst + i * GDL_VTX_WORDS);
                    for (int k = 0; k < 3; k++) ob[k] = (int16_t)(ob[k] + s_fold_d[k]);   /* (the producer's order: the geom core's) */
                }
            }
            break;
        }
        case OP_TRI1:
            if (s_sem.cc_pick)
                tri_pick((uint32_t)s_valpha[(w0 >> 17) & 63u] + s_valpha[(w0 >> 9) & 63u] + s_valpha[(w0 >> 1) & 63u], 3);
            if (!s_tri_state_ok) {
                emit_texenv_if_changed(out);
                emit_ccolor_if_changed(out);
                emit_rendermode_if_changed(out);
                emit_texbind_if_needed(out);
                s_tri_state_ok = 1;
            }
            gdl_tri1(out, (int)((w0 >> 17) & 0x7f), (int)((w0 >> 9) & 0x7f), (int)((w0 >> 1) & 0x7f));
            s_drawn = 1;
            break;
        case OP_TRI2:
            if (s_sem.cc_pick)
                tri_pick((uint32_t)s_valpha[(w0 >> 17) & 63u] + s_valpha[(w0 >> 9) & 63u] + s_valpha[(w0 >> 1) & 63u]
                         + s_valpha[(w1 >> 17) & 63u] + s_valpha[(w1 >> 9) & 63u] + s_valpha[(w1 >> 1) & 63u], 6);
            if (!s_tri_state_ok) {
                emit_texenv_if_changed(out);
                emit_ccolor_if_changed(out);
                emit_rendermode_if_changed(out);
                emit_texbind_if_needed(out);
                s_tri_state_ok = 1;
            }
            gdl_tri2(out,
                     (int)((w0 >> 17) & 0x7f), (int)((w0 >> 9) & 0x7f), (int)((w0 >> 1) & 0x7f),
                     (int)((w1 >> 17) & 0x7f), (int)((w1 >> 9) & 0x7f), (int)((w1 >> 1) & 0x7f));
            s_drawn = 1;
            break;

        case OP_SETOTHERMODE_H: {
            uint32_t len = (w0 & 0xffu) + 1u, sft = 32u - ((w0 >> 8) & 0xffu) - len;
            uint32_t mask = (len >= 32u ? 0xffffffffu : ((1u << len) - 1u)) << sft;
            uint32_t was = s_othermode_h;
            s_othermode_h = (s_othermode_h & ~mask) | ((uint32_t)w1 & mask);
            if ((was ^ s_othermode_h) & (3u << 20)) cc_resolve();   /* the cycle type */
            break;
        }
        case OP_SETFILLCOLOR:
            s_fill_color = (uint32_t)w1;
            break;
        case OP_SETCIMG:
            s_cimg = (uintptr_t)w1;
            rdp_raw(out, RDP_OP_SET_CIMG << 24 | (w0 & 0x3FFu), F3D_SDRAM(w1));
            {   /* the geom core's screen edges follow the image: a 640-wide one
                 * (Pokemon Stadium's menus, 640 x 480) was clipped to 320 x 240 */
                int32_t w = (int32_t)(w0 & 0xFFFu) + 1;
                if (w >= 64 && w != s_fb_w) {
                    s_fb_w = w; s_fb_h = w * 3 / 4;
                    gdl_fbsize(out, (unsigned)s_fb_w, (unsigned)s_fb_h);
                    if (s_dlc_translating) s_dlc_abort = 1;
                }
            }
            break;
        case OP_SETZIMG:
            s_zimg = (uintptr_t)w1;
            rdp_raw(out, RDP_OP_SET_ZIMG << 24, F3D_SDRAM(w1));
            break;
        case 0xED:                                  /* G_SETSCISSOR */
            rdp_raw(out, RDP_OP_SET_SCISSOR << 24 | (w0 & 0xFFFFFFu), w1);
            break;
        case OP_FILLRECT:
            if (n64_fillrect(out, w0, (uint32_t)w1)) break;
            emit_fillrect(out, w0, (uint32_t)w1);
            break;
        case OP_TEXRECT:
            s_drawn = 1;
            emit_texrect(out, dl, w0, (uint32_t)w1, (uint32_t)dl[1].w1, (uint32_t)dl[2].w1);
            dl += 2;   /* its G_RDPHALF_1 and G_RDPHALF_2 */
            break;

        case OP_DL: {
            int is_branch = ((w0 >> 16) & 0xff) != 0;
            const f3d_word_t *tgt = (const f3d_word_t *)F3D_PTR(w1);
            dlc_need_static((uintptr_t)tgt, 8u);
            if (is_branch) { dl = tgt; continue; }
            call_dl(tgt, out, depth);
            break;
        }
        case OP_ENDDL:
            return;

        default:
            break;  /* every unhandled opcode here is 1 Gfx word */
        }
        dl++;
    }
}

void f3d_emit_display_list(const f3d_word_t *dl, gdl_cur_t *out) {
    static int s_calls;
    s_dlc_frame++;
    s_trace_task_ = (F3D_TRACE && s_calls < 2);
    s_walk_iters = 0;
    s_drawn = 0; f3d_clear_valid = 0;
    if (s_trace_task) printf("f3d_emit: walk start dl=%p\n", (void *)dl);
    walk(dl, out, 0);
    if (s_trace_task)
        printf("f3d_emit: walk done, %ld iters\n", s_walk_iters);
    s_calls++;
}
