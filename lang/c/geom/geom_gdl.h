/* Geometry display-list format -- what the game CPU builds in SDRAM and the
 * geometry core walks. A trimmed, word-aligned translation of the F3DEX
 * commands the geometry pipeline actually needs. lang/c/game/gdl_build.h
 * writes it.
 *
 * Every command is a 32-bit header word: [31:24] opcode, [23:0] arg, followed
 * by 0+ payload words. All matrices/vectors are fp32 or S15.16 words.
 */
#ifndef GEOM_GDL_H
#define GEOM_GDL_H

#include <stdint.h>

enum {
    GDL_END       = 0x00,  /* arg unused                                   */
    GDL_MTX_LOAD  = 0x01,  /* arg: 0 = modelview TOS, 1 = projection; +16 f32 */
    GDL_MTX_MUL   = 0x02,  /* arg: 0 = modelview TOS, 1 = projection; +16 f32 (target = target * payload) */
    GDL_MTX_PUSH  = 0x03,  /* push a copy of the modelview TOS             */
    GDL_MTX_POP   = 0x04,  /* arg: count                                   */
    GDL_VIEWPORT  = 0x05,  /* +8 f32: vp_scale[4] then vp_trans[4] (already /4) */
    GDL_VTX       = 0x06,  /* arg: (n<<8)|v0 ; +n * GDL_VTX_WORDS words    */
    GDL_TRI1      = 0x07,  /* arg: (i0<<16)|(i1<<8)|i2                     */
    GDL_TRI2      = 0x08,  /* arg: i0..i2 ; +1 word: i3..i5 (3x u8 in [23:0]) */
    GDL_GEOMODE   = 0x09,  /* arg: bit0 = lighting on                      */
    GDL_TEXNORM   = 0x0A,  /* +1 f32: 1/tex_render_size (0 = texturing off) */
    GDL_DL        = 0x0B,  /* +1 word: absolute address; call (push return) */
    GDL_ENDDL     = 0x0C,  /* return, or halt if the call stack is empty   */
    GDL_FOG       = 0x0D,  /* arg: bit0 = fog on ; +1 word packed RGBA8 fog
                              colour, +2 f32 fogMul, fogOffset. Per-vertex fog
                              (the rasterizer has no fog): each
                              vertex colour is lerped toward the fog colour by
                              clamp(ndc_z*fogMul + fogOffset, 0, 1). */
    GDL_TEXBIND   = 0x0F,  /* bind the current texture. +8 words:
                              [0] fmt[3:0] | siz[7:4] | cms[11:8] | cmt[15:12] | palette[19:16]
                                  (cms/cmt: G_TX_MIRROR 1, G_TX_CLAMP 2, and 4 = no mask:
                                  clamp at the size, never wrap)
                              [1] w[15:0] | h[31:16]   (texel dimensions)
                              [2] f32 1/w   [3] f32 1/h   (s,t normalisation)
                              [4],[5] src pointer  lo,hi
                              [6],[7] TLUT pointer lo,hi (CI formats; 0 otherwise)
                              arg bit0 (GDL_TEXBIND_EXT): +3 words, the texture is a
                              rectangle out of a larger image (a LOADTILE, or a tile
                              too large for TMEM cropped to what a rectangle uses):
                              [8] pw: the image's row pitch in texels (0 = w)
                              [9] k0: the rectangle's first texel in the image
                              [10] uls[15:0] | ult[31:16]: the tile's origin, u10.2
                                   texels, subtracted from every s,t (SET_TILE_SIZE's SL/TL)
                              The geom core decodes and stages the texture for
                              RDP; a host build samples it directly. */
    GDL_TEXENV    = 0x10,  /* arg: the N64 colour combiner, reduced to a few modes.
                              [1:0] rgb: GDL_TE_RGB_*; [3:2] alpha:
                              GDL_TE_A_*. 0 = modulate both (the default). The alpha test
                              is on only while the alpha comes from the texel and the
                              surface is not translucent. Emitted by a display-list translator on
                              G_SETCOMBINE, only when the mode changes. A geom ROM from
                              before [3:1] existed read bit0 alone: 5 (the old "1") still
                              means lerp there, the new modes fall back to modulate. */
    GDL_RAW       = 0x11,  /* arg: n (1..GDL_RAW_MAX) ; +n raw RDP command words,
                              pushed to RDP verbatim, in GDL order. This is how the
                              game CPU's framebuffer commands (the per-frame clears,
                              the end-of-frame sync) reach RDP without the CPU
                              writing its command FIFO itself: RDP has ONE command
                              FIFO, and a CPU write there would have to wait until
                              the geom core has finished streaming, serialising every
                              frame behind the previous one (frame.c, pipelined
                              overlap). */
    GDL_RENDERMODE = 0x12, /* arg: GDL_RM_* flags, no payload. The N64 render mode's depth and
                              blend behaviour (an N64 translator decodes G_SETOTHERMODE_L): ZCMP depth
                              test, ZUPD depth write, DECAL (ZMODE_DEC: drawn onto a coplanar
                              surface), XLU (FORCE_BL, in*a + mem*(1-a)). Emitted only when the
                              flags change; the default until the first one is ZCMP|ZUPD. */
    GDL_CCOLOR    = 0x13,  /* arg: bit0 on, bit1 rgb keeps the shade, bit2 alpha keeps the
                              shade; +1 word RGBA8 constant K. The N64 combiner's PRIMITIVE and
                              ENVIRONMENT inputs: while on, every triangle's vertex colour is
                              (shade or 1) * K, per channel group, at the moment it is drawn
                              (the combiner acts at draw time, and F3DEX2 reuses vertices
                              across colour changes). a translator folds the constants the
                              combiner multiplies by into K; the GDL_TEXENV mode then treats
                              the result as the shade. Off: vertex colours untouched. */
    GDL_LIGHT     = 0x0E,  /* arg: slot[7:0] | is_ambient[8] | num_dir_lights[23:16].
                              +1 word 0x00RRGGBB colour, +1 word dir packed as
                              3x s8 (x=bits[7:0], y=[15:8], z=[23:16]), unit-ish,
                              ignored for the ambient slot. Directional Gouraud
                              lighting replaces the flat-grey stub: per vertex
                              shade = ambient + sum_k col_k * max(0, N.dir_k),
                              N = normalize(normal_obj * modelview3x3). */
    GDL_FBSIZE    = 0x14,  /* arg: hres[11:0] | vres[23:12]. The framebuffer's size (268 or
                              320 x 240: a game's header picks it, lang/c/game/frame.c
                              frame_set_video); the screen edges of the clip outcodes. Until
                              the first one, GEOM_FB_HRES x GEOM_FB_VRES. */
    GDL_F3D       = 0x16,  /* arg: n (= GDL_F3D_WORDS) ; +n words: an F3DEX2 display list the
                              geom core processes itself, as the N64's RSP does (geom_f3d.c,
                              linked when the firmware is built with a translator): the
                              list's root; [1..3] unused; the static range [lo, hi) whose
                              lists may be cached, and the cache's arena and its size.
                              Translated a piece at a time into the core's own RAM and
                              walked from there -- the N64's DMEM -- never through SDRAM.
                              Without a translator in the firmware the command is skipped. */
    GDL_COMBINE   = 0x15,  /* arg: bit0 on, bit1 the combine reads a texel, bit3 the game's
                              alpha compare (othermode L [1:0] != 0: it decides the alpha
                              test, not the GDL_TEXENV mode), [15:8] PRIM's LOD
                              fraction (G_SETPRIMCOLOR's w0 low byte). +4 words: the N64
                              colour combiner's two words (G_SETCOMBINE's w0 -- its low 24 bits
                              -- and w1, both cycles), PRIMITIVE and ENVIRONMENT as RGBA8. On,
                              RDP draws with them as written (and GDL_TEXENV / GDL_CCOLOR only
                              decide the alpha test); off, or a texel read with no texture
                              bound, with the GDL_TEXENV mode as before. n64rt's translator
                              sends it; a geom that predates it stops at it. */
};

#define GDL_TEXBIND_EXT 0x1u   /* GDL_TEXBIND's arg: +3 words (pitch, first texel, origin) */
#define GDL_TEXBIND_SCALE 0x2u /* ... and [2],[3] are gsSPTexture's scale (S15.16), not scale / w */

#define GDL_F3D_WORDS 8u

#define GDL_OP(w)   ((uint8_t)((w) >> 24))
#define GDL_ARG(w)  ((w) & 0x00FFFFFFu)
#define GDL_HDR(op, arg) (((uint32_t)(op) << 24) | ((arg) & 0x00FFFFFFu))

/* One vertex in a GDL_VTX payload: object-space s16 position + s16 texcoord +
 * u8 RGBA. Byte-for-byte SM64's Vtx_t (incl. the unused flag word), so it is
 * exactly 4 words. */
#define GDL_VTX_WORDS 4
#define GDL_RM_ZCMP    0x1u
#define GDL_RM_ZUPD    0x2u
#define GDL_RM_DECAL   0x4u
#define GDL_RM_XLU     0x8u
#define GDL_RM_DEFAULT (GDL_RM_ZCMP | GDL_RM_ZUPD)
/* GDL_TEXENV fields. ENV/PRIM constants in the N64 combiner are taken as 1:
 * SM64 uses them for fades, and a faded object at full opacity is the common case. */
#define GDL_TE_RGB_MODULATE 0u   /* texel * shade        G_CC_MODULATE*        */
#define GDL_TE_RGB_LERP     1u   /* lerp(shade, texel, texel a)  G_CC_BLENDRGB* */
#define GDL_TE_RGB_TEXEL    2u   /* texel                G_CC_DECALRGB*        */
#define GDL_TE_RGB_SHADE    3u   /* shade                G_CC_SHADE*           */
#define GDL_TE_A_MODULATE   (0u << 2)   /* texel a * shade a */
#define GDL_TE_A_SHADE      (1u << 2)
#define GDL_TE_A_TEXEL      (2u << 2)
#define GDL_TE_BLEND        (GDL_TE_RGB_LERP | GDL_TE_A_SHADE)   /* the old arg 1 */
#define GDL_RAW_MAX 64u
typedef struct {
    int16_t  ob[3];
    uint16_t flag;       /* unused here, keeps the layout == SM64 Vtx_t */
    int16_t  tc[2];      /* S10.5 texel coords (as in gbi.h) */
    uint8_t  cn[4];      /* RGBA or normal + alpha */
} geom_vtx_t;            /* 16 bytes = 4 words */
_Static_assert(sizeof(struct { int16_t a[3]; uint16_t b; int16_t c[2]; uint8_t d[4]; }) == 16,
               "geom_vtx_t must be 16 bytes");

#define GDL_MTX_TARGET_MODELVIEW 0
#define GDL_MTX_TARGET_PROJECTION 1

#define GDL_GEOMODE_LIGHTING 0x1
/* SM64's geometry mode without G_CULL_BACK: draw both faces. Unset (every
 * GDL that predates it) keeps back-face culling. */
#define GDL_GEOMODE_NOCULL   0x2
/* F3DEX2 G_TEXTURE_GEN: each lit vertex's s,t come from its normal against
 * the two look-at vectors (GDL_LIGHT slots GDL_LIGHT_LOOKAT_X/Y), as
 * s = (N.lookat_x + 1) * 2^15 in texel/32 units -- the same units as a
 * vertex's own s,t, so gsSPTexture's scale (folded into GDL_TEXNORM) then
 * maps it as it would any other. SM64's power star, Mario's cap wings, water
 * rings and bubbles. */
#define GDL_GEOMODE_TEXGEN   0x4
/* GDL_LIGHT slots that carry gSPLookAt's X and Y directions (colour unused) */
#define GDL_LIGHT_LOOKAT_X   8
#define GDL_LIGHT_LOOKAT_Y   9

#endif /* GEOM_GDL_H */
