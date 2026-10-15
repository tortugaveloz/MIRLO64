/* The N64's coverage in the model (MRDP_N64, MRDP_CVG; mrdp.h): constructed
 * triangles against the console's documented behaviour (Angrylion's RDP model).
 *   cc -O2 -DMRDP_N64 -I. test_cvg.c mrdp.c -o test_cvg && ./test_cvg
 */
#include "mrdp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM (16u << 20)                 /* (the hidden plane at 8 MiB: mrdp.h) */
static uint8_t mem[MEM];
static uint16_t rd16(void *c, uint32_t a) { (void)c; a &= MEM - 2; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; a &= MEM - 2; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
static mrdp_t R;
static int fails;

#define CIMG 0x10000u
#define W 64u
static void cmd(uint32_t w0, uint32_t w1) { mrdp_push(&R, w0); mrdp_push(&R, w1); }
static uint16_t px(int x, int y) { return rd16(0, CIMG + ((uint32_t)y * W + (uint32_t)x) * 2); }
/* the stored coverage: bit 0 and the hidden plane's two (mrdp.h) */
static unsigned cvg_at(int x, int y)
{
    uint32_t a = CIMG + ((uint32_t)y * W + (uint32_t)x) * 2, k = (a >> 1) & 15u;
    uint32_t ha = MRDP_HID_BASE + ((a >> 5) << 2) + (k < 8u ? 2u : 0u);
    return (rd16(0, a) & 1u) << 2 | ((rd16(0, ha) >> (2u * (k & 7u))) & 3u);
}
static void check_cvg(const char *what, int x, int y, unsigned want)
{
    unsigned got = cvg_at(x, y);
    if (got != want) { printf("FAIL %s at (%d,%d): coverage %u want %u\n", what, x, y, got, want); fails++; }
    else printf("ok   %s at (%d,%d): coverage %u\n", what, x, y, got);
}
static void check(const char *what, int x, int y, uint16_t want)
{
    uint16_t got = px(x, y);
    if (got != want) { printf("FAIL %s at (%d,%d): got %04x want %04x\n", what, x, y, got, want); fails++; }
    else printf("ok   %s at (%d,%d) = %04x\n", what, x, y, got);
}
#define ZIMG 0x20000u
static void setup(uint32_t other_l)
{
    mrdp_init(&R, 0, rd16, wr16);
    memset(mem, 0, sizeof mem);
    cmd(0x3E000000u, ZIMG);
    cmd(0x3F000000u | 2u << 19 | (W - 1), ZIMG);                              /* the depth image cleared to the far plane */
    cmd(0x2D000000u, (W << 2) << 12 | (32 << 2));
    cmd(0x37000000u, 0xFFFFFFFFu);
    cmd(0x2F000000u | (3u << 20) | (3u << 6) | (3u << 4), 0);
    cmd(0x36000000u | ((W - 1) << 2) << 12 | (31 << 2), 0);
    cmd(0x3F000000u | 2u << 19 | (W - 1), CIMG);                              /* 16-bit colour image */
    cmd(0x2D000000u, (W << 2) << 12 | (32 << 2));                             /* scissor 64 x 32 */
    /* combiner: RGB = PRIM, A = PRIM (cycle 1's settings; 1-cycle mode) */
    cmd(0x3C000000u | (15u << 5) | 31u, (15u << 24) | (3u << 6) | (7u << 21) | (7u << 18) | (7u << 3) | 3u);
    /* dithers off (RGB sel 3, alpha sel 3); 1-cycle */
    cmd(0x2F000000u | (3u << 6) | (3u << 4), other_l);
    cmd(0x37000000u, 0x00010001u);                                            /* fill: complete black */
    cmd(0x2F000000u | (3u << 20) | (3u << 6) | (3u << 4), 0);
    cmd(0x36000000u | ((W - 1) << 2) << 12 | (31 << 2), 0);
    cmd(0x2F000000u | (3u << 6) | (3u << 4), other_l);
}
/* a triangle with a vertical left edge at x (s15.16) from y = 0 to 8 and a
 * diagonal to (30, 8): the N64 command's 8 words (no attributes) */
static int with_z;                     /* the triangles carry a flat Z: zval (s15.16; the 18-bit depth is it >> 13) */
static uint32_t zval = 0x40000000u;
static void zwords(void) { if (with_z) { cmd(zval, 0); cmd(0, 0); } }
static void tri_vleft(int32_t x)
{
    int32_t yl = 8 << 2, ym = 8 << 2, yh = 0;
    uint32_t dxm = (uint32_t)(((int64_t)((30 << 16) - x)) / 8);
    cmd(0x08000000u | (with_z ? 1u << 24 : 0) | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
    cmd(30u << 16, 0);                 /* XL (at YM), DxLDy */
    cmd((uint32_t)x, 0);               /* XH, DxHDy: vertical */
    cmd((uint32_t)x, dxm);             /* XM, DxMDy */
    zwords();
}
/* tri_vleft with a flat shade block (opcode 0x0C): r, g, b, a as the integer parts (s15.16's high half), no slopes */
static void tri_shade(int32_t x, int r, int g, int bl, int a)
{
    int32_t yl = 8 << 2, ym = 8 << 2, yh = 0;
    uint32_t dxm = (uint32_t)(((int64_t)((30 << 16) - x)) / 8);
    cmd(0x0C000000u | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
    cmd(30u << 16, 0);
    cmd((uint32_t)x, 0);
    cmd((uint32_t)x, dxm);
    cmd((uint32_t)(r & 0xFFFF) << 16 | (uint32_t)(g & 0xFFFF), (uint32_t)(bl & 0xFFFF) << 16 | (uint32_t)(a & 0xFFFF));
    for (int i = 0; i < 7; i++) cmd(0, 0);                    /* the rest of the 16 words: slopes and fractions zero */
}
/* the triangle (0,0) (x,0) (x,8): a diagonal major edge on the left, the
 * vertical edge at x on the right (shares tri_vleft's edge) */
static void tri_vright(int32_t x)
{
    int32_t yl = 8 << 2, ym = 0, yh = 0;
    uint32_t dxh = (uint32_t)(x / 8);
    cmd(0x08000000u | (with_z ? 1u << 24 : 0) | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
    cmd((uint32_t)x, 0);               /* XL at YM = 0: the vertical edge */
    cmd(0, dxh);                       /* XH = 0, the diagonal */
    cmd(0, 0);                         /* XM: degenerate */
    zwords();
}
static uint16_t c5551(unsigned r, unsigned g, unsigned b, unsigned a) { return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | a); }

#define AA_OPA  ((1u << 3) | (1u << 6) | (1u << 13) | (1u << 22) | (1u << 18))   /* AA_EN IM_RD ALPHA_CVG_SEL, CLR_IN A_IN CLR_MEM A_MEM, CVG_DST_CLAMP */

int main(void)
{
    /* 1. an edge's coverage, by its eighth: FORCE_BL with 1 - a over black
     * shows cvg / 8 of the colour (ALPHA_CVG_SEL: a = cvg << 5) */
    static const unsigned want_cvg[8] = { 8, 6, 6, 4, 4, 2, 2, 0 };
    for (int e = 0; e < 8; e++) {
        setup((1u << 3) | (1u << 13) | (1u << 14) | (0u << 22) | (0u << 18) | (2u << 22));   /* AA_EN ALPHA_CVG_SEL FORCE_BL; P=in A=in M=blend(0) B=1-a */
        cmd(0x39000000u, 0x00000000u);                                        /* blend colour black */
        cmd(0x3A000000u, 0xFF0000FFu);                                        /* prim red */
        tri_vleft((10 << 16) + (e << 13));
        unsigned c = want_cvg[e];
        /* P * A5 + M * (B5 + 1): A5 = 4c (c < 8), B5 = 31 - 4c, sum = 255 * 4c -> >> 5 */
        unsigned r8 = c == 8 ? 255 : (255 * 4 * c) >> 5;
        char what[64]; snprintf(what, sizeof what, "edge at 10 + %d/8: cvg %u", e, c);
        if (c == 0) check(what, 10, 3, 0x0001);
        else check(what, 10, 3, c5551(r8, 0, 0, 1));          /* (FORCE_BL blends: the coverage sum completes) */
    }

    /* 2. two triangles sharing the edge at 10.5: the first's edge pixel is
     * opaque over the complete background and marked partial; the second
     * blends by coverage into a complete pixel; interiors are opaque */
    setup(AA_OPA);
    cmd(0x3A000000u, 0xFF0000FFu);
    tri_vleft((10 << 16) + 0x8000);
    check("first triangle's edge pixel: opaque, partial", 10, 3, c5551(255, 0, 0, 0));
    check_cvg("its coverage: 4 samples, written less one", 10, 3, 3);
    check_cvg("the inside", 11, 3, 7);
    check("its interior", 11, 3, c5551(255, 0, 0, 1));
    check("outside", 9, 3, 0x0001);
    cmd(0x3A000000u, 0x0000FFFFu);
    tri_vright((10 << 16) + 0x8000);
    /* without a depth compare the N64 shifts the memory coverage out (blshiftb
     * 4): B5 = {0, 11} = 3, sum = 16 P + 4 M, / 5 -> (4 blue + red) / 5 */
    check("shared edge pixel, no Z: (4 P + M) / 5, complete", 10, 3, c5551((255 * 4) / 20, 0, (255 * 16) / 20, 1));
    check("second's interior", 9, 3, c5551(0, 0, 255, 1));
    /* with Z_CMP (equal depths: within dz, no shifts): 4 red + 4 blue of 8,
     * A5 = 16, B5 = {3, 11} = 15, sum = 16 P + 16 M, / 8 -> 127 each */
    with_z = 1;
    setup(AA_OPA | (1u << 4) | (1u << 5));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x8000);
    check("Z: first edge pixel opaque, partial", 10, 3, c5551(255, 0, 0, 0));
    cmd(0x3A000000u, 0x0000FFFFu); tri_vright((10 << 16) + 0x8000);
    check("Z: shared edge pixel half and half, complete", 10, 3, c5551(127, 0, 127, 1));
    check_cvg("Z: shared edge pixel's coverage 3 + 4", 10, 3, 7);
    /* (the depth's word: the 18 bits 0x20000 are the exponent 1 with a mantissa of 0) */
    if (rd16(0, ZIMG + (3 * W + 10) * 2) != 0x2000) { printf("FAIL depth at (10,3): %04x\n", rd16(0, ZIMG + (3 * W + 10) * 2)); fails++; }
    with_z = 0;

    /* 3. CLR_ON_CVG: no overflow -> the memory colour stays, the coverage completes */
    setup(AA_OPA | (1u << 7));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x8000);
    cmd(0x3A000000u, 0x0000FFFFu); tri_vright((10 << 16) + 0x8000);
    check("CLR_ON_CVG shared pixel: memory colour, complete", 10, 3, c5551(255, 0, 0, 1));

    /* 4. CVG_DST_WRAP: 4 + 3 = 7 complete; a third pass of 4 over 7 wraps to 3 */
    setup(AA_OPA | (1u << 8));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x8000);
    cmd(0x3A000000u, 0x0000FFFFu); tri_vright((10 << 16) + 0x8000);
    check("WRAP shared pixel complete", 10, 3, c5551((255 * 4) / 20, 0, (255 * 16) / 20, 1));
    cmd(0x3A000000u, 0x00FF00FFu); tri_vleft((10 << 16) + 0x8000);
    check("WRAP: 4 over a complete pixel overflows: opaque", 10, 3, c5551(0, 255, 0, 0));
    check_cvg("WRAP: (4 + 7) & 7", 10, 3, 3);

    /* 5. CVG_DST_ZAP: always complete; SAVE: the memory's */
    setup(AA_OPA | (2u << 8));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x8000);
    check("ZAP edge pixel complete", 10, 3, c5551(255, 0, 0, 1));
    setup(AA_OPA | (3u << 8));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x8000);
    check("SAVE edge pixel keeps the background's complete", 10, 3, c5551(255, 0, 0, 1));

    /* 6. CVG_X_ALPHA: alpha 128 halves the coverage of an interior pixel (8 ->
     * (128 * 8 + 4) >> 8 = 4): opaque over the background, partial; alpha 16
     * leaves none: not drawn */
    setup(AA_OPA | (1u << 12));
    cmd(0x3A000000u, 0xFF000080u); tri_vleft(10 << 16);
    check("CVG_X_ALPHA 128: interior opaque, partial", 12, 3, c5551(255, 0, 0, 0));
    check_cvg("CVG_X_ALPHA 128: (128 * 8 + 4) >> 8 = 4, written less one", 12, 3, 3);
    cmd(0x3A000000u, 0x00FF0010u); tri_vleft(10 << 16);
    check("CVG_X_ALPHA 16: not drawn", 12, 3, c5551(255, 0, 0, 0));

    /* 6b. TEX_EDGE (CVG_X_ALPHA, ALPHA_CVG_SEL): the combiner's alpha 255 is
     * 256 before the coverage multiply (the hardware's CombineAlpha stage),
     * so an inside pixel has coverage (256 * 8 + 4) >> 8 = 8, written 7 --
     * complete, with no storage trick */
    setup(AA_OPA | (1u << 12));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft(10 << 16);
    check("TEX_EDGE inside, alpha 255: bit 0 set", 12, 3, c5551(255, 0, 0, 1));
    check_cvg("TEX_EDGE inside: 8 written less one", 12, 3, 7);
    /* ... and at alpha 254 the coverage is (254 * 8 + 4) >> 8 = 7, written 6 */
    setup(AA_OPA | (1u << 12));
    cmd(0x3A000000u, 0xFF0000FEu); tri_vleft(10 << 16);
    check_cvg("TEX_EDGE inside, alpha 254: 7 written less one", 12, 3, 6);

    /* 6c. The combiner's arithmetic ((A - B) * C + D * 256 + 0x80) >> 8, 1 = 256,
     * a result's 9 bits clamped (the hardware's CombineColor stage).
     * RGB = (A - 0) * C + D; the alpha is PRIM's. */
#define CC_RGB(sa, sb, mul, add) \
    cmd(0x3C000000u | ((uint32_t)(sa) << 5) | (uint32_t)(mul), ((uint32_t)(sb) << 24) | ((uint32_t)(add) << 6) | (7u << 21) | (7u << 18) | (7u << 3) | 3u)
    /* PRIM red 200 * ENV red 255: (200 * 255 + 128) >> 8 = 199 (the 5 bits 24;
     * 200 * 255/255 would be 25) */
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC80000FFu); cmd(0x3B000000u, 0xFF0000FFu); CC_RGB(3, 15, 5, 7);
    tri_vleft(10 << 16);
    check("combiner: PRIM 200 * ENV 255 -> 199", 12, 3, c5551(192, 0, 0, 1));
    /* ONE - PRIM at C = 255: (256 - 40) * 255 + 128 >> 8 = 215 (5 bits 26) */
    setup(AA_OPA);
    cmd(0x3A000000u, 0x280000FFu); cmd(0x3B000000u, 0xFF0000FFu); CC_RGB(6, 3, 5, 7);
    tri_vleft(10 << 16);
    check("combiner: (ONE - 40) * 255 -> 215", 12, 3, c5551(208, 0, 0, 1));
    /* 255 * 255 + 255 (D = PRIM): 509, bits 8:7 set: the hardware's 0 */
    setup(AA_OPA);
    cmd(0x3A000000u, 0xFF0000FFu); cmd(0x3B000000u, 0xFF0000FFu); CC_RGB(3, 15, 5, 3);
    tri_vleft(10 << 16);
    check("combiner: 255 * 255 + 255 -> 509 = 0", 12, 3, c5551(0, 0, 0, 1));
    /* 200 * 255 + ENV 100 (C = PRIM's alpha): 199 + 100 = 299, bit 8 alone: 255 */
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC80000FFu); cmd(0x3B000000u, 0x640000FFu); CC_RGB(3, 15, 10, 5);
    tri_vleft(10 << 16);
    check("combiner: 199 + 100 = 299 -> 255", 12, 3, c5551(255, 0, 0, 1));
    /* 199 + ENV 255: 454 = 0x1C6, bits 8:7 set: 0 */
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC80000FFu); cmd(0x3B000000u, 0xFF0000FFu); CC_RGB(3, 15, 10, 5);
    tri_vleft(10 << 16);
    check("combiner: 199 + 255 = 454 -> 0", 12, 3, c5551(0, 0, 0, 1));

    /* 7. no AA_EN: the N64's rule -- a pixel is drawn if its top-left sample
     * (sample 7: the pixel's left edge, the row's top) is covered, so an edge
     * on a pixel's left or before it takes the pixel, one a hair after does not;
     * the coverage written is the count less one, 7 for a whole pixel */
    setup((1u << 6) | (1u << 22) | (1u << 18));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft(10 << 16);
    check("no AA: an edge at 10.0 takes pixel 10", 10, 3, c5551(255, 0, 0, 1));
    setup((1u << 6) | (1u << 22) | (1u << 18));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) + 0x4000);
    check("no AA: an edge at 10.25 does not", 10, 3, 0x0001);
    check("no AA: ... and takes 11", 11, 3, c5551(255, 0, 0, 1));
    setup((1u << 6) | (1u << 22) | (1u << 18));
    cmd(0x3A000000u, 0xFF0000FFu); tri_vleft((10 << 16) - 0x8000);
    check("no AA: an edge at 9.5 takes 10 (the old centre rule would take 9)", 10, 3, c5551(255, 0, 0, 1));
    check("no AA: ... and not 9", 9, 3, 0x0001);

    /* 8. a triangle without texture coefficients (its command has no S, T, W block) still takes TEXEL0 from the tile's texel
     * (0, 0): S = T = W = 0 and the texture stages run (as the hardware does; a character's collar, G_TEXTURE off with a combiner that reads
     * TEXEL0, was black). A 4 x 1 RGBA16 texture, red, at 0x30000, in tile 0 (loaded through tile 7). */
    setup((1u << 6) | (1u << 22) | (1u << 18));
    for (unsigned i = 0; i < 4; i++) { mem[0x30000 + 2 * i] = 0xF8; mem[0x30000 + 2 * i + 1] = 0x01; }
    cmd(0x3D000000u | 2u << 19, 0x30000u);                                    /* SET_TEXTURE_IMAGE: RGBA, 16 bit */
    cmd(0x35000000u | 2u << 19, 7u << 24);                                    /* SET_TILE 7: RGBA16 at TMEM 0 */
    cmd(0x33000000u, 7u << 24 | 3u << 12);                                    /* LOAD_BLOCK: 4 texels */
    cmd(0x35000000u | 2u << 19 | 1u << 9, 0);                                 /* SET_TILE 0: RGBA16, a line of one 64-bit word */
    cmd(0x32000000u, 3u << 14 | 0u << 12 | 0u);                               /* SET_TILE_SIZE 0: 4 x 1 (the model clamps nothing here) */
    cmd(0x3C000000u | (15u << 5) | 31u, (15u << 24) | (7u << 21) | (7u << 18) | (1u << 6) | (7u << 3) | 1u);   /* RGB = TEXEL0, A = TEXEL0 */
    tri_vleft(10 << 16);
    check("no texture coefficients: TEXEL0 is the tile's texel (0, 0)", 12, 3, c5551(255, 0, 0, 1));

    /* 9. RGB_DITHER_SEL 2 is noise (G_CD_NOISE: a game's dust): a flat grey with its low three bits at 4 goes up a step in
     * about half of the pixels, at no place of a 4 x 4 pattern -- a pixel and the one four to its right differ somewhere
     * (with the Bayer matrix the picture repeats every four pixels), and each channel has its own random bits */
    setup((1u << 6) | (1u << 22) | (1u << 18));
    cmd(0x2F000000u | (2u << 6) | (3u << 4), (1u << 6) | (1u << 22) | (1u << 18));   /* RGB noise, alpha dither off */
    cmd(0x3A000000u, 0x848484FFu);
    tri_vleft(0);
    {
        int up = 0, n = 0, rep = 0, chdiff = 0;
        for (int y = 4; y < 8; y++)
            for (int x = 0; x < 14; x++) {
                unsigned a = px(x, y), b = px(x + 4, y);
                unsigned r = a >> 11, g = (a >> 6) & 31;
                n++; up += r == 17; rep += a == b; chdiff += (r == 17) != (g == 17);
            }
        printf("%s noise dither: %d of %d pixels up a step, %d repeat at +4, %d differ between channels\n",
               (up > 12 && up < 48 && rep < n - 4 && chdiff > 0) ? "ok  " : "FAIL", up, n, rep, chdiff);
        if (!(up > 12 && up < 48 && rep < n - 4 && chdiff > 0)) fails++;
    }

    /* 10. The shade enters the combiner saturated to a byte: below 0 -> 0, above 255 -> 255 (with 9 bits, 0..511, a lit surface's
     * shade overshoots 255 at its edges -- a lit character's clothes -- and the raw 9 bits of the first cycle's TEXEL0 * SHADE
     * went past 255; as COMBINED, the second cycle's C, they are a negative number: a black pixel). A flat shade (R, G, B, A) =
     * (300, 600, -50, 450) is (255, 255, 0, 255) here; PRIM = (100, 100, 100, 128). */
#define SH_TRI(r, g, b, a) tri_shade(10 << 16, (r), (g), (b), (a))
    /* (SHADE - PRIM) * PRIM_A: R (255 - 100) * 128 + 0x80 >> 8 = 78 (the same as 255), G 78, B (0 - 100) * 128 -> -50: bits 8:7 set, 0 */
    setup(AA_OPA);
    cmd(0x3A000000u, 0x64646480u); CC_RGB(4, 3, 10, 7);
    SH_TRI(300, 600, -50, 450);
    check("shade: a byte, (SHADE - PRIM) * PRIM_A, 300 -> 255 -> 78", 12, 3, c5551(78, 78, 0, 1));
    /* a shade inside a byte is as it was: (200 - 100) * 128 -> 50 */
    setup(AA_OPA);
    cmd(0x3A000000u, 0x64646480u); CC_RGB(4, 3, 10, 7);
    SH_TRI(200, 100, 0, 0);
    check("shade: in range, unchanged", 12, 3, c5551(50, 0, 0, 1));
    /* A lit garment: 2 cycles, PRIM (255) * SHADE, then ENV (200) * COMBINED. With a shade of 300 the first cycle gave 299, whose
     * bit 8 made COMBINED a negative C and the pixel black; the shade as a byte (255): 254, then 200 * 254 + 0x80 >> 8 = 198 */
    setup(AA_OPA);
    cmd(0x2F000000u | (1u << 20) | (3u << 6) | (3u << 4), (1u << 3) | (1u << 6) | (1u << 13) | (3u << 26) | (2u << 18));
    cmd(0x3A000000u, 0xFFFFFFFFu);
    cmd(0x3B000000u, 0xC8C8C8FFu);
    cmd(0x3C000000u | (3u << 20) | (4u << 15) | (7u << 12) | (7u << 9) | (5u << 5) | 0u,
        (15u << 28) | (7u << 15) | (7u << 12) | (7u << 9) | (7u << 21) | (7u << 18) | (15u << 24) | (7u << 6) | (7u << 3) | 7u);
    SH_TRI(300, 300, 300, 255);
    check("shade: 2-cycle PRIM * SHADE then ENV * COMBINED, a shade past 255 is not black", 12, 3, c5551(198, 198, 198, 1));

    /* 11. A 2-cycle pixel's second combiner cycle takes the first cycle's TEXEL1 as its TEXEL0: the next tile's sample (the N64's;
     * a ground can read (COMBINED - TEXEL0) * A + TEXEL0 there, the RGBA16 grass in tile 1 under an I8 detail in tile 0).
     * Tile 0: one red texel, tile 1: one green one (RGBA16, S = T = 0), the combiner cycle 0 = the constant, cycle 1 = TEXEL0 or
     * TEXEL1: TEXEL0 is tile 1's green, TEXEL1 tile 0's red. */
    for (int k = 0; k < 2; k++) {
        setup((1u << 3) | (1u << 6) | (1u << 13));
        for (unsigned i = 0; i < 4; i++) { mem[0x30000 + 2 * i] = 0xF8; mem[0x30000 + 2 * i + 1] = 0x01; mem[0x30100 + 2 * i] = 0x07; mem[0x30100 + 2 * i + 1] = 0xC1; }
        cmd(0x3D000000u | 2u << 19, 0x30000u);
        cmd(0x35000000u | 2u << 19, 7u << 24);
        cmd(0x33000000u, 7u << 24 | 3u << 12);
        cmd(0x3D000000u | 2u << 19, 0x30100u);
        cmd(0x35000000u | 2u << 19 | 8u, 7u << 24);                                /* tile 7 at TMEM word 8 */
        cmd(0x33000000u, 7u << 24 | 3u << 12);
        cmd(0x35000000u | 2u << 19 | 1u << 9, 0);                                 /* tile 0: RGBA16 at 0 */
        cmd(0x35000000u | 2u << 19 | 1u << 9 | 8u, 1u << 24);                     /* tile 1: RGBA16 at 8 */
        cmd(0x32000000u, 3u << 14);
        cmd(0x32000000u, 1u << 24 | 3u << 14);
        cmd(0x2F000000u | (1u << 20) | (3u << 6) | (3u << 4), (1u << 3) | (1u << 6) | (1u << 13) | (3u << 26) | (2u << 18));
        /* cycle 0: RGB = 0 + ENV-less constant (PRIM); alpha 1. cycle 1: RGB = TEXEL0 (k = 0) or TEXEL1 (k = 1); alpha 1 */
        cmd(0x3A000000u, 0x64646480u);
        cmd(0x3C000000u | (15u << 20) | (31u << 15) | (7u << 12) | (7u << 9) | (15u << 5) | 31u,
            (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (3u << 15) | (7u << 12) | (6u << 9) | ((k ? 2u : 1u) << 6) | (7u << 3) | 6u);
        {
            int32_t x = 10 << 16, yl = 8 << 2, ym = 8 << 2, yh = 0;
            uint32_t dxm = (uint32_t)(((int64_t)((30 << 16) - x)) / 8);
            cmd(0x0A000000u | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
            cmd(30u << 16, 0); cmd((uint32_t)x, 0); cmd((uint32_t)x, dxm);
            for (int i = 0; i < 8; i++) cmd(0, 0);                                /* S = T = W = 0, no slopes */
        }
        check(k ? "2-cycle: TEXEL1 in the second cycle is the tile's sample (red)" : "2-cycle: TEXEL0 in the second cycle is the next tile's sample (green)",
              12, 3, k ? c5551(255, 0, 0, 1) : c5551(0, 255, 0, 1));
    }
    /* 12. With the TLUT on a 16-bit texel is an index too: the first byte of its word (a game loads its depth image as a
     * texture and takes a table's entry by the depth's top byte). A 4 x 1 texture of 0x4C37 in tile 0; the table's entry 0x4C
     * green, 0x37 (the word's other byte) red, all the others blue. */
    setup((1u << 6) | (1u << 22) | (1u << 18));
    for (unsigned i = 0; i < 4; i++) { mem[0x30000 + 2 * i] = 0x4C; mem[0x30000 + 2 * i + 1] = 0x37; }
    for (unsigned i = 0; i < 256; i++) {
        const uint16_t e = i == 0x4C ? c5551(0, 255, 0, 1) : i == 0x37 ? c5551(255, 0, 0, 1) : c5551(0, 0, 255, 1);
        mem[0x31000 + 2 * i] = (uint8_t)(e >> 8); mem[0x31000 + 2 * i + 1] = (uint8_t)e;
    }
    cmd(0x3D000000u | 2u << 19, 0x31000u);                                    /* the table: 256 entries into TMEM's upper half */
    cmd(0x35000000u | 256u, 7u << 24);                                        /* SET_TILE 7: TMEM 256 */
    cmd(0x30000000u, 7u << 24 | (255u << 2) << 12);                           /* LOAD_TLUT 0..255 */
    cmd(0x3D000000u | 2u << 19, 0x30000u);
    cmd(0x35000000u | 2u << 19, 7u << 24);
    cmd(0x33000000u, 7u << 24 | 3u << 12);
    cmd(0x35000000u | 2u << 19 | 1u << 9, 0);                                 /* SET_TILE 0: RGBA16 */
    cmd(0x32000000u, 3u << 14 | 0u << 12 | 0u);
    cmd(0x3C000000u | (15u << 5) | 31u, (15u << 24) | (7u << 21) | (7u << 18) | (1u << 6) | (7u << 3) | 1u);   /* RGB = TEXEL0, A = TEXEL0 */
    cmd(0x2F000000u | (1u << 15) | (3u << 6) | (3u << 4), (1u << 6) | (1u << 22) | (1u << 18));   /* TLUT on (RGBA16 entries) */
    tri_vleft(10 << 16);
    check("TLUT on, a 16-bit texel: the entry of its first byte", 12, 3, c5551(0, 255, 0, 1));

    /* 13. The depth image's word is the N64's: an exponent (the depth's leading ones) and the 11 bits under them, above the top
     * two bits of the pixel's dz code (a flat triangle's code is 3: 0). Opaque, no AA, Z_CMP and Z_UPD. */
#define Z_OPA ((1u << 4) | (1u << 5) | (1u << 6) | (1u << 22) | (1u << 18))
#define ZWORD(x, y) rd16(0, ZIMG + ((uint32_t)(y) * W + (uint32_t)(x)) * 2)
    with_z = 1;
    {
        static const struct { uint32_t z18; uint16_t word; } zt[] = {
            { 0x00000, 0x0000 }, { 0x0007F, 0x0004 }, { 0x1FFFF, 0x1FFC }, { 0x20000, 0x2000 }, { 0x2ABCD, 0x3578 }, { 0x30000, 0x4000 },
            { 0x38008, 0x6004 }, { 0x3C004, 0x8004 }, { 0x3E002, 0xA004 }, { 0x3F001, 0xC004 }, { 0x3F7FF, 0xDFFC }, { 0x3F800, 0xE000 },
            { 0x3FFFE, 0xFFF8 } };
        for (unsigned i = 0; i < sizeof zt / sizeof zt[0]; i++) {
            setup(Z_OPA);
            cmd(0x3A000000u, 0xFF0000FFu); zval = zt[i].z18 << 13; tri_vleft(10 << 16);
            const uint16_t got = ZWORD(12, 3);
            if (got != zt[i].word) { printf("FAIL depth %05x: the word is %04x, want %04x\n", (unsigned)zt[i].z18, got, zt[i].word); fails++; }
            else printf("ok   depth %05x is the word %04x\n", (unsigned)zt[i].z18, got);
        }
        /* outside the 18 bits: the far plane, and 0 */
        setup(Z_OPA); zval = 0x7FFFFFFFu; tri_vleft(10 << 16);
        if (ZWORD(12, 3) != 0xFFFC) { printf("FAIL depth past the far plane: %04x\n", ZWORD(12, 3)); fails++; }
        setup(Z_OPA); zval = 0x90000000u; tri_vleft(10 << 16);
        if (ZWORD(12, 3) != 0x0000) { printf("FAIL depth below 0: %04x\n", ZWORD(12, 3)); fails++; }
    }
    /* the far end keeps all 18 bits: a pixel one step nearer is in front (in 16 bits the two were the same depth) ... */
    setup(Z_OPA);
    cmd(0x3A000000u, 0xFF0000FFu); zval = 0x3F902u << 13; tri_vleft(10 << 16);
    cmd(0x3A000000u, 0x0000FFFFu); zval = 0x3F901u << 13; tri_vleft(10 << 16);
    check("depth: one step nearer at the far end is in front", 12, 3, c5551(0, 0, 255, 1));
    cmd(0x3A000000u, 0x00FF00FFu); zval = 0x3F901u << 13; tri_vleft(10 << 16);
    check("depth: the same depth is not", 12, 3, c5551(0, 0, 255, 1));
    /* ... and the near end loses six: a decal on a surface there is within what its word lost (0x0123F is kept as 0x01200,
     * and the decal's own step is 7) */
    setup(Z_OPA);
    cmd(0x3A000000u, 0xFF0000FFu); zval = 0x0123Fu << 13; tri_vleft(10 << 16);
    cmd(0x2F000000u | (3u << 6) | (3u << 4), (Z_OPA & ~(1u << 5)) | (3u << 10));        /* ZMODE_DEC, no Z_UPD */
    cmd(0x3A000000u, 0x0000FFFFu); tri_vleft(10 << 16);
    check("depth: a decal on a near surface", 12, 3, c5551(0, 0, 255, 1));
    cmd(0x3A000000u, 0x00FF00FFu); zval = 0x01300u << 13; tri_vleft(10 << 16);          /* (behind by more: not a decal's) */
    check("depth: a decal farther than that is not drawn", 12, 3, c5551(0, 0, 255, 1));
    with_z = 0; zval = 0x40000000u;

    /* 14. SET CONVERT's K4 and K5 are the combiner's B 7 and C 15, 9 bits each: K4 a negative number from 0x180 (as COMBINED),
     * K5 a signed 9-bit. PRIM is a grey of 200, ENV of 250. */
#define K45(k4, k5) cmd(0x2C000000u, (uint32_t)(k4) << 9 | (uint32_t)(k5))
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC8C8C8FFu); cmd(0x3B000000u, 0xFAFAFAFFu); K45(0, 255); CC_RGB(3, 7, 15, 7);
    tri_vleft(10 << 16);
    check("combiner: (PRIM - K4 0) * K5 255 -> 199", 12, 3, c5551(199, 199, 199, 1));
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC8C8C8FFu); cmd(0x3B000000u, 0xFAFAFAFFu); K45(100, 255); CC_RGB(3, 7, 15, 7);
    tri_vleft(10 << 16);
    check("combiner: (PRIM - K4 100) * K5 255 -> 100", 12, 3, c5551(100, 100, 100, 1));
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC8C8C8FFu); cmd(0x3B000000u, 0xFAFAFAFFu); K45(0, 0x180); CC_RGB(3, 7, 15, 5);
    tri_vleft(10 << 16);
    check("combiner: PRIM * K5 -128 + ENV -> 150", 12, 3, c5551(150, 150, 150, 1));
    setup(AA_OPA);
    cmd(0x3A000000u, 0xC8C8C8FFu); cmd(0x3B000000u, 0xFAFAFAFFu); K45(0x1C0, 0x80); CC_RGB(3, 7, 15, 7);
    tri_vleft(10 << 16);
    check("combiner: (PRIM - K4 -64) * K5 128 -> 132", 12, 3, c5551(132, 132, 132, 1));

    /* 15. A YUV texture (a JPEG picture decoded by the RSP): the image's U Y0 V Y1 loaded as U V to TMEM's lower half and Y0 Y1
     * to its upper, converted by the texture filter (1-cycle, G_TC_CONV: BI_LERP_0 off) with SET_CONVERT's K0..K3, then the
     * combiner's (TEXEL0 - K4) * K5 + TEXEL0 -- the game's own words (EC15FD5D 3B78E42A: K 175, -43, -89, 222, 114, 42). The pair
     * U 168, Y0 100, V 108, Y1 200: u 40, v -20, the K's 2K + 1. Texel 0 (Y 100): R 100 + (351 * -20 + 128 >> 8) = 73, G 100 +
     * (-85 * 40 - 177 * -20 + 128 >> 8) = 101, B 100 + (445 * 40 + 128 >> 8) = 170; combined R (73 - 114) * 42 + 73 * 256 + 128
     * >> 8 = 66, G 99, B 179. Texel 1 (Y 200, through the tile's S mask: S 0 less uls 1 wraps to 1): 173, 201, 270 -> 255;
     * combined 183, 215, and B 278 overflows to 255. */
    for (int k = 0; k < 2; k++) {
        static const uint8_t yuv[8] = { 168, 100, 108, 200, 168, 100, 108, 200 };
        setup((1u << 6) | (1u << 22) | (1u << 18));
        memcpy(&mem[0x30000], yuv, sizeof yuv);
        cmd(0x3D000000u | 1u << 21 | 2u << 19, 0x30000u);                         /* SET_TEXTURE_IMAGE: YUV, 16 bit */
        cmd(0x35000000u | 1u << 21 | 2u << 19, 7u << 24);                         /* SET_TILE 7: YUV16 at TMEM 0 */
        cmd(0x33000000u, 7u << 24 | 3u << 12);                                    /* LOAD_BLOCK: 4 texels */
        cmd(0x35000000u | 1u << 21 | 2u << 19 | 1u << 9, k ? 1u << 4 : 0u);        /* SET_TILE 0: YUV16, line 1 (k: S mask 1) */
        cmd(0x32000000u | (k ? 4u << 12 : 0u), 3u << 14);                         /* SET_TILE_SIZE 0 (k: uls 1) */
        cmd(0xEC15FD5Du, 0x3B78E42Au);                                            /* SET_CONVERT */
        /* RGB = (TEXEL0 - K4) * K5 + TEXEL0, A = 1 (cycle 1's settings; 1-cycle) */
        cmd(0x3C000000u | (1u << 5) | 15u, (15u << 28) | (7u << 24) | (7u << 21) | (7u << 18) | (1u << 6) | (7u << 3) | 6u);
        tri_vleft(10 << 16);
        check(k ? "YUV: texel 1 (Y1) converted, (T - K4) * K5 + T" : "YUV: texel 0 (Y0) converted, (T - K4) * K5 + T",
              12, 3, k ? c5551(183, 215, 255, 1) : c5551(66, 99, 179, 1));
    }

    printf(fails ? "%d FAILED\n" : "ALL PASS\n", fails);
    return fails != 0;
}
