/* 32-bit colour images in the model (rdp.h, docs/cimg32.md): worked cases against the rules the
 * N64 programming manual gives for them, and one scene drawn into a 16-bit image and a 32-bit one, compared pixel by
 * pixel.
 *   cc -O2 -DRDP_N64 -I. test_cimg32.c rdp.c -o test_cimg32 && ./test_cimg32
 * The rules checked:
 *   - a pixel is a 32-bit word, R G B A from its lowest byte; the depth image stays 16-bit at the same pixel offset
 *   - fill writes the fill colour whole at each pixel and no hidden bits
 *   - the blender's memory colour is the pixel's 8-bit R G B, its memory coverage the alpha byte's top three bits
 *   - a pixel is written R G B as the blender (and the RGB dither) leaves them, the alpha byte the coverage << 5
 *   - copy mode into a 32-bit image draws nothing
 * and that the same scene's 16-bit image is the 32-bit one's channels truncated to five bits, with the same coverage,
 * wherever the memory colours the scene reads are multiples of 8 (where both images read the same memory colour).
 */
#include "rdp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM (16u << 20)                 /* (the hidden plane at 8 MiB: rdp.h) */
static uint8_t mem[MEM];
static uint16_t rd16(void *c, uint32_t a) { (void)c; a &= MEM - 2; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; a &= MEM - 2; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
static rdp_t R;
static int fails, checks;

#define W 64u                           /* the colour image's width, 32 rows used */
#define H 32u
#define IMG16  0x10000u
#define IMG32  0x20000u
#define ZIMG   0x40000u
#define TIMG   0x50000u
static void cmd(uint32_t w0, uint32_t w1) { rdp_push(&R, w0); rdp_push(&R, w1); }
static uint32_t px32(int x, int y)
{
    uint32_t a = IMG32 + ((uint32_t)y * W + (uint32_t)x) * 4u;
    return (uint32_t)rd16(0, a) << 16 | rd16(0, a + 2u);
}
static uint16_t px16(int x, int y) { return rd16(0, IMG16 + ((uint32_t)y * W + (uint32_t)x) * 2u); }
static unsigned hid16(uint32_t a)
{
    uint32_t k = (a >> 1) & 15u, ha = RDP_HID_BASE + ((a >> 5) << 2) + (k < 8u ? 2u : 0u);
    return (rd16(0, ha) >> (2u * (k & 7u))) & 3u;
}
static unsigned cvg16(int x, int y) { uint32_t a = IMG16 + ((uint32_t)y * W + (uint32_t)x) * 2u; return (px16(x, y) & 1u) << 2 | hid16(a); }
static void ok(int good, const char *fmt, const char *what, int x, int y, uint32_t got, uint32_t want)
{
    checks++;
    if (!good) { printf("FAIL %s at (%d,%d): ", what, x, y); printf(fmt, got, want); printf("\n"); fails++; }
    else { printf("ok   %s at (%d,%d) = ", what, x, y); printf("%08x", got); printf("\n"); }
}
static void check32(const char *what, int x, int y, uint32_t want)
{
    uint32_t got = px32(x, y);
    ok(got == want, "got %08x want %08x", what, x, y, got, want);
}
static void check16(const char *what, int x, int y, uint32_t want)
{
    uint32_t got = px16(x, y);
    ok(got == want, "got %04x want %04x", what, x, y, got, want);
}
static uint32_t c8888(unsigned r, unsigned g, unsigned b, unsigned cvg) { return r << 24 | g << 16 | b << 8 | cvg << 5; }
static uint16_t c5551(unsigned r, unsigned g, unsigned b, unsigned a) { return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | a); }

/* other modes' high word: cycle type, dithers off (RGB sel 3, alpha sel 3) */
#define H_1CYC   ((3u << 6) | (3u << 4))
#define H_COPY   ((2u << 20) | (3u << 6) | (3u << 4))
#define H_FILL   ((3u << 20) | (3u << 6) | (3u << 4))
/* SET_COLOR_IMAGE: RGBA, size 2 (16-bit) or 3 (32-bit) */
static void cimg(unsigned size) { cmd(0x3F000000u | size << 19 | (W - 1), size == 3 ? IMG32 : IMG16); }
/* the frame: memory cleared, the depth image at the far plane, the colour image filled with a complete background */
static void frame(unsigned size, uint32_t bg32, uint32_t bg16x2)
{
    rdp_init(&R, 0, rd16, wr16);
    cmd(0x3E000000u, ZIMG);
    cmd(0x2D000000u, (W << 2) << 12 | (H << 2));                               /* scissor 64 x 32 */
    cmd(0x3F000000u | 2u << 19 | (W - 1), ZIMG);
    cmd(0x37000000u, 0xFFFCFFFCu);
    cmd(0x2F000000u | H_FILL, 0);
    cmd(0x36000000u | ((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
    cimg(size);
    cmd(0x37000000u, size == 3 ? bg32 : bg16x2);
    cmd(0x36000000u | ((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
}
/* combiner, 1-cycle (cycle 1's fields): RGB and alpha both the input `sel` (1 TEXEL0, 3 PRIM, 4 SHADE) as D */
static void combine_d(unsigned sel)
{
    cmd(0x3C000000u | (15u << 5) | 31u, (15u << 24) | (sel << 6) | (7u << 21) | (7u << 18) | (7u << 3) | sel);
}
/* a triangle with a vertical left edge at x (s15.16) from row y0 to y0 + 8 and a diagonal to (30, y0 + 8); shade: flat
 * r g b a (bytes) with red stepping dr a pixel (opcode 0x0C), else no attributes (0x08) */
static int y0;                          /* the triangles' first row */
static void tri_vleft(int32_t x, int shade, int r, int g, int b, int a, int dr)
{
    int32_t yl = (y0 + 8) << 2, ym = (y0 + 8) << 2, yh = y0 << 2;
    uint32_t dxm = (uint32_t)(((int64_t)((30 << 16) - x)) / 8);
    cmd((shade ? 0x0C000000u : 0x08000000u) | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
    cmd(30u << 16, 0);
    cmd((uint32_t)x, 0);
    cmd((uint32_t)x, dxm);
    if (shade) {
        cmd((uint32_t)r << 16 | (uint32_t)g, (uint32_t)b << 16 | (uint32_t)a);    /* values (integer halves) */
        cmd((uint32_t)(dr & 0xFFFF) << 16, 0);                                  /* DrDx's integer half */
        for (int i = 0; i < 6; i++) cmd(0, 0);                                   /* fractions, DxDe, DxDy: 0 */
    }
}
/* the triangle (0,0) (x,0) (x,8): its vertical right edge at x, tri_vleft's left edge */
static void tri_vright(int32_t x)
{
    int32_t yl = (y0 + 8) << 2, ym = y0 << 2, yh = y0 << 2;
    cmd(0x08000000u | 1u << 23 | (uint32_t)yl, (uint32_t)ym << 16 | (uint32_t)yh);
    cmd((uint32_t)x, 0);
    cmd(0, (uint32_t)(x / 8));
    cmd(0, 0);
}
/* a 4 x 4 RGBA32 texture at TIMG, loaded to TMEM 0 as tile 0 (LOAD_TILE), its texels' low bits not 0 */
static uint32_t texel(int s, int t) { return (uint32_t)(0x13 + 0x20 * s) << 24 | (uint32_t)(0x27 + 0x30 * t) << 16 | 0x5Du << 8 | 0xFFu; }
static void texture(void)
{
    for (int t = 0; t < 4; t++)
        for (int s = 0; s < 4; s++) {
            uint32_t a = TIMG + (uint32_t)(t * 4 + s) * 4u, v = texel(s, t);
            wr16(0, a, (uint16_t)(v >> 16)); wr16(0, a + 2u, (uint16_t)v);
        }
    cmd(0x3D000000u | 3u << 19 | 3u, TIMG);                                    /* texture image: RGBA 32, 4 wide */
    cmd(0x35000000u | 3u << 19 | 1u << 9, 0);                                  /* tile 0: RGBA 32, a 64-bit word a row (each half), TMEM 0 */
    cmd(0x34000000u, (3u << 2) << 12 | (3u << 2));                             /* LOAD_TILE 0..3 x 0..3 (and the tile's size) */
}
/* TEXTURE_RECTANGLE of tile 0 at (x, y), 4 x 4 pixels, a texel a pixel */
static void texrect(int x, int y)
{
    cmd(0x24000000u | (uint32_t)((x + 4) << 2) << 12 | (uint32_t)((y + 4) << 2), (uint32_t)(x << 2) << 12 | (uint32_t)(y << 2));
    cmd(0, 0x04000400u);
}

/* the scene compared at both sizes: a background (multiples of 8), a blended shaded triangle, an opaque textured
 * rectangle, and two anti-aliased triangles sharing an edge (the coverage, and the blender's memory alpha) */
#define AA_OPA ((1u << 3) | (1u << 6) | (1u << 13) | (1u << 22) | (1u << 18))   /* AA_EN IM_RD ALPHA_CVG_SEL, P in A in M mem B mem alpha, CVG_DST_CLAMP */
static void scene(unsigned size)
{
    frame(size, 0x4080C0E0u, (uint32_t)c5551(0x40, 0x80, 0xC0, 1) * 0x10001u);
    /* 1. FORCE_BL, P in, A in alpha, M mem, B 1 - a: shade red 0x31 + 3x, green 0x22, blue 0x17, alpha 0x80 */
    cmd(0x2F000000u | H_1CYC, (1u << 14) | (1u << 22));
    combine_d(4);
    tri_vleft(2 << 16, 1, 0x31, 0x22, 0x17, 0x80, 3);
    /* 2. the textured rectangle: no blend, the texel as it is */
    cmd(0x2F000000u | H_1CYC, 0);
    texture();
    combine_d(1);
    texrect(40, 4);
    /* 3. AA: two triangles sharing the edge x = 10.5, rows 16..23: the first opaque over the complete background, its
     * edge pixels partial; the second blends into them by coverage (the first's colour is a multiple of 8: it is
     * read back as the memory colour, which the 16-bit image would otherwise read less precisely) */
    cmd(0x2F000000u | H_1CYC, AA_OPA);
    combine_d(3);
    y0 = 16;
    cmd(0x3A000000u, 0xF80800FFu);
    tri_vleft((10 << 16) + 0x8000, 0, 0, 0, 0, 0, 0);
    cmd(0x3A000000u, 0x0A11F6FFu);                                             /* (not read back: its low bits may be anything) */
    tri_vright((10 << 16) + 0x8000);
    y0 = 0;
}

int main(void)
{
    /* ---- 1. fill: the fill colour whole at each pixel, no hidden bits ---- */
    frame(3, 0x12345678u, 0);
    check32("fill: the whole word at each pixel", 0, 0, 0x12345678u);
    check32("fill: the next pixel the same word", 1, 0, 0x12345678u);
    check32("fill: the last pixel", (int)W - 1, (int)H - 1, 0x12345678u);
    {
        uint32_t nz = 0;
        for (uint32_t a = RDP_HID_BASE; a < RDP_HID_BASE + 0x100000u; a += 2) nz += rd16(0, a) != 0;
        ok(nz == 0, "%u hidden words written, want %u", "fill: no hidden bits written", 0, 0, nz, 0);
        ok(px32((int)W, (int)H - 1) == 0, "%08x past the image, want %08x", "fill: the word past the image untouched", (int)W, (int)H - 1, px32((int)W, (int)H - 1), 0);
    }
    /* a fill rectangle's corners in fill mode: whole pixels, the lower right one included (as a 16-bit image's) */
    cmd(0x37000000u, 0xA1B2C3E0u);
    cmd(0x36000000u | (6u << 2) << 12 | (3u << 2), (4u << 2) << 12 | (2u << 2));   /* (4,2)..(6,3) */
    check32("fill rectangle: its first pixel", 4, 2, 0xA1B2C3E0u);
    check32("fill rectangle: its last pixel", 6, 3, 0xA1B2C3E0u);
    check32("fill rectangle: the pixel after it", 7, 3, 0x12345678u);
    check32("fill rectangle: the row under it", 4, 4, 0x12345678u);
    /* ... where a 16-bit image takes the fill colour's two halves in turn */
    frame(2, 0, 0x12345678u);
    check16("16-bit fill, an even pixel: the high half", 0, 0, 0x1234u);
    check16("16-bit fill, an odd pixel: the low half", 1, 0, 0x5678u);

    /* ---- 2. a blended triangle over the background ----
     * FORCE_BL, P = the pixel (prim 0xF3 0x22 0x17, alpha 0x80), M = memory, B = 1 - a: A5 = 0x80 >> 3 = 16,
     * B5 = 0x7F >> 3 = 15, out = (16 P + 16 M) >> 5 = (P + M) / 2 on the 8-bit channels: (0xF3 + 0x40) / 2 = 0x99,
     * (0x22 + 0x80) / 2 = 0x51, (0x17 + 0xC0) / 2 = 0x6B. The coverage: 8 + 7 (IM_RD off: the memory's 7) blended,
     * clamped to 7, the alpha byte 0xE0. A 16-bit image keeps the top five bits: 0x99 >> 3 = 19, 0x51 >> 3 = 10,
     * 0x6B >> 3 = 13, and bit 0 set. */
    for (unsigned size = 2; size <= 3; size++) {
        frame(size, 0x4080C0E0u, (uint32_t)c5551(0x40, 0x80, 0xC0, 1) * 0x10001u);
        cmd(0x2F000000u | H_1CYC, (1u << 14) | (1u << 22));
        combine_d(3);
        cmd(0x3A000000u, 0xF3221780u);
        tri_vleft(2 << 16, 0, 0, 0, 0, 0, 0);
        if (size == 3) {
            check32("blend: (P + M) / 2 at 8 bits, coverage 7", 5, 3, c8888(0x99, 0x51, 0x6B, 7));
            check32("blend: outside the triangle, the background", 1, 3, 0x4080C0E0u);
        } else
            check16("blend, 16-bit: the same, truncated", 5, 3, c5551(0x99, 0x51, 0x6B, 1));
    }

    /* ---- 3. the memory colour at 8 bits, and the memory coverage from the alpha byte ----
     * a background with low bits (0x47 0x83 0xC5) and coverage 5 (alpha 0xA0), blended as above: (0xF3 + 0x47) / 2 =
     * 0x9D, (0x22 + 0x83) / 2 = 0x52, (0x17 + 0xC5) / 2 = 0x6E: the low bits of the memory count (a 16-bit image
     * would read 0x40 0x80 0xC0). With IM_RD the coverage is 8 + 5 = 13: an overflow, so (FORCE_BL still blends) the
     * clamp gives 7. Then the same with CVG_DST_WRAP: (8 + 5) & 7 = 5, alpha 0xA0. */
    for (int wrap = 0; wrap < 2; wrap++) {
        frame(3, 0x4783C5A0u, 0);
        cmd(0x2F000000u | H_1CYC, (1u << 14) | (1u << 22) | (1u << 6) | (wrap ? 1u << 8 : 0u));
        combine_d(3);
        cmd(0x3A000000u, 0xF3221780u);
        tri_vleft(2 << 16, 0, 0, 0, 0, 0, 0);
        check32(wrap ? "memory colour's low bits; coverage (8 + 5) & 7" : "memory colour's low bits; coverage 8 + 5 clamped",
                5, 3, c8888(0x9D, 0x52, 0x6E, wrap ? 5u : 7u));
    }
    /* the blender's memory alpha (P the pixel, A its alpha 0x80, M memory, B memory alpha) is the coverage: an edge
     * pixel of coverage 2 (the edge at 2 + 6/8) over memory coverage 5 does not overflow (2 + 5 < 8), so with AA_EN
     * it blends. With a depth compare the coverage weights are not shifted (prim depth at the far plane: within dz of
     * the cleared depth image): A5 = (0x80 >> 5) << 2 = 16, B5 = {5, 11} = 23, the sum 16 P + 24 M divided by
     * A5 / 4 + B5 / 4 + 1 = 10 through the reciprocal table (0x3334): red (16 * 0xF3 + 24 * 0x47) >> 2 = 1398,
     * 1398 * 0x3334 >> 17 = 0x8B; green 3688 >> 2 = 922 -> 0x5C; blue 5096 >> 2 = 1274 -> 0x7F. WRAP: (2 + 5) & 7 = 7. */
    {
        frame(3, 0x4783C5A0u, 0);
        cmd(0x2F000000u | H_1CYC, (1u << 3) | (1u << 6) | (1u << 22) | (1u << 18) | (1u << 8) | (1u << 4) | (1u << 2));
        cmd(0x2E000000u, 0x7FFF0000u);                                         /* prim depth 0x7FFF: 18-bit 0x3FFF8 */
        combine_d(3);
        cmd(0x3A000000u, 0xF3221780u);
        tri_vleft((2 << 16) + (6 << 13), 0, 0, 0, 0, 0, 0);
        check32("memory alpha = the coverage: (16 P + 24 M) / 10, coverage (2 + 5) & 7", 2, 3, c8888(0x8B, 0x5C, 0x7F, 7));
    }

    /* ---- 4. a textured rectangle: the texel's 8 bits as they are, its alpha not stored (the coverage is) ---- */
    frame(3, 0, 0);
    cmd(0x2F000000u | H_1CYC, 0);
    texture();
    combine_d(1);
    texrect(40, 4);
    for (int t = 0; t < 4; t += 3)
        for (int s = 0; s < 4; s += 3) {
            char what[80]; snprintf(what, sizeof what, "texrect: texel (%d,%d) whole, coverage 7", s, t);
            uint32_t v = texel(s, t);
            check32(what, 40 + s, 4 + t, (v & 0xFFFFFF00u) | 0xE0u);
        }
    check32("texrect: nothing left of it", 39, 4, 0);

    /* ---- 5. the RGB dither still applies (the manual: "enabled even in 32 bit mode") ----
     * prim red 0x93 (low bits 3), the magic square: (0,0) 0 < 3 -> 0x98; (1,0) 6 >= 3 -> 0x93; (2,0) 1 < 3 -> 0x98.
     * No 5-bit truncation: 0x93 stays 0x93 where the threshold is not under the low bits. */
    frame(3, 0, 0);
    cmd(0x2F000000u | (0u << 6) | (3u << 4), 0);                              /* RGB dither: magic square */
    combine_d(3);
    cmd(0x3A000000u, 0x930000FFu);
    cmd(0x36000000u | (4u << 2) << 12 | (4u << 2), 0);                         /* a 1-cycle rectangle (0,0)..(3,3) */
    check32("dither: threshold 0 < 3, up to 0x98", 0, 0, c8888(0x98, 0, 0, 7));
    check32("dither: threshold 6 >= 3, 0x93 kept whole", 1, 0, c8888(0x93, 0, 0, 7));
    check32("dither: threshold 1 < 3, up to 0x98", 2, 0, c8888(0x98, 0, 0, 7));

    /* ---- 6. depth with a 32-bit image: the depth image's word at the pixel's 16-bit offset ---- */
    frame(3, 0, 0);
    cmd(0x2F000000u | H_1CYC, (1u << 4) | (1u << 5) | (1u << 2));              /* Z_CMP Z_UPD, Z from SET_PRIM_DEPTH */
    cmd(0x2E000000u, 0x10000000u);                                             /* prim depth 0x1000 (15 bits): 18-bit 0x8000 */
    combine_d(3);
    cmd(0x3A000000u, 0x112233FFu);
    cmd(0x36000000u | (12u << 2) << 12 | (5u << 2), (10u << 2) << 12 | (4u << 2));   /* (10,4)..(11,4) */
    {
        uint16_t z = rd16(0, ZIMG + (4u * W + 10u) * 2u);
        /* SET_PRIM_DEPTH 0x1000 is the 18-bit 0x8000: exponent 0, mantissa 0x8000 >> 6 = 0x200, the word 0x200 << 2 */
        ok(z == 0x0800u, "depth word %04x want %04x", "depth at the pixel's 16-bit offset in the depth image", 10, 4, z, 0x0800u);
        ok(rd16(0, ZIMG + (4u * W + 12u) * 2u) == 0xFFFCu, "depth word %04x want %04x", "depth: the pixel after the rectangle untouched", 12, 4,
           rd16(0, ZIMG + (4u * W + 12u) * 2u), 0xFFFCu);
        check32("depth: the pixel drawn", 10, 4, c8888(0x11, 0x22, 0x33, 7));
    }

    /* ---- 7. copy mode into a 32-bit image: not drawn (the manual: "You cannot copy the 32-bit RGBA color image type") ---- */
    frame(3, 0, 0);
    texture();
    cmd(0x2F000000u | H_COPY, 0);
    {
        uint64_t before = R.pixels_drawn;
        cmd(0x24000000u | (43u << 2) << 12 | (7u << 2), (40u << 2) << 12 | (4u << 2));
        cmd(0, 0x10000400u);
        ok(R.pixels_drawn == before && px32(40, 4) == 0, "%u pixels, want %u", "copy mode: nothing drawn", 40, 4, (uint32_t)(R.pixels_drawn - before), 0);
    }

    /* ---- 8. the scene at both sizes, compared ---- */
    static uint32_t img32[W * H];
    static uint16_t img16[W * H];
    static uint8_t c16[W * H];
    scene(3);
    for (unsigned i = 0; i < W * H; i++) img32[i] = px32((int)(i % W), (int)(i / W));
    uint64_t n32 = R.pixels_drawn;
    scene(2);
    for (unsigned i = 0; i < W * H; i++) { img16[i] = px16((int)(i % W), (int)(i / W)); c16[i] = (uint8_t)cvg16((int)(i % W), (int)(i / W)); }
    {
        unsigned diff = 0, low = 0, partial = 0;
        for (unsigned i = 0; i < W * H; i++) {
            uint32_t v = img32[i];
            unsigned r = v >> 24, g = (v >> 16) & 0xFFu, b = (v >> 8) & 0xFFu, cv = (v >> 5) & 7u;
            uint16_t want = (uint16_t)c5551(r, g, b, cv >> 2);
            if (want != img16[i] || cv != c16[i]) { if (diff < 8) printf("     scene pixel (%u,%u): 32-bit %08x (cvg %u), 16-bit %04x (cvg %u)\n", i % W, i / W, v, cv, img16[i], c16[i]); diff++; }
            if ((r | g | b) & 7u) low++;
            if (cv != 7u) partial++;
        }
        printf("     scene: %llu pixels drawn at both sizes (%llu); %u of %u pixels have low bits a 16-bit image loses, %u a partial coverage\n",
               (unsigned long long)n32, (unsigned long long)R.pixels_drawn, low, W * H, partial);
        ok(diff == 0 && n32 == R.pixels_drawn && low > 0 && partial > 0, "%u pixels differ, want %u",
           "scene: the 16-bit image is the 32-bit one's top five bits, same coverage", 0, 0, diff, 0);
        /* the shared AA edge pixel: the first triangle's coverage 4, written 3; the second's 4 blends into it (no depth
         * compare: the memory coverage's weight shifted out, (16 P + 4 M) / 5) and completes it, 3 + 4 = 7 */
        printf("     scene: the shared AA edge pixel (10,19): 32-bit %08x, 16-bit %04x\n", img32[19 * W + 10], img16[19 * W + 10]);
        ok(((img32[19 * W + 10] >> 5) & 7u) == 7u && c16[19 * W + 10] == 7u, "coverage %u want %u", "scene: the shared edge pixel complete at both sizes", 10, 19,
           (img32[19 * W + 10] >> 5) & 7u, 7u);
    }

    printf("test_cimg32: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
