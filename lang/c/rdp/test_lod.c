/* The texture LOD in the model (rdp.c, the LOD section; docs/lod.md): the
 * console's perspective divide (rdp_n64_div) and rdp_lod against values
 * worked out by hand, then a perspective floor drawn in 2-cycle mode with a mip
 * chain of six levels, each a checker of its own colour, through the
 * combiner's (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0, to test_lod.ppm: the
 * levels follow the distance.
 *   cc -O2 -DRDP_N64 -I. test_lod.c rdp.c -o test_lod && ./test_lod
 */
#include "rdp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM (16u << 20)                 /* (the hidden plane at 8 MiB: rdp.h) */
static uint8_t mem[MEM];
static uint16_t rd16(void *c, uint32_t a) { (void)c; a &= MEM - 2; return (uint16_t)(mem[a] << 8 | mem[a + 1]); }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; a &= MEM - 2; mem[a] = (uint8_t)(v >> 8); mem[a + 1] = (uint8_t)v; }
static rdp_t R;
static int fails;
static void cmd(uint32_t w0, uint32_t w1) { rdp_push(&R, w0); rdp_push(&R, w1); }

/* ---- rdp_lod alone ---------------------------------------------------------- */
static void lod_case(const char *what, uint32_t h, unsigned minl, unsigned maxl, int32_t ds, int32_t dt,
                     unsigned want_t0, unsigned want_t1, unsigned want_frac, unsigned want_level)
{
    const int32_t s[3] = { 1000, 1000 + ds, 1000 }, t[3] = { 2000, 2000, 2000 + dt };
    rdp_lod_t L = rdp_lod(h, minl, maxl, 2, s, t, 0);
    int ok = L.tile0 == want_t0 && L.tile1 == want_t1 && L.frac == want_frac && L.level == want_level;
    printf("%s %-44s lod %5u level %u tiles %u %u frac %03x\n", ok ? "ok  " : "FAIL", what, L.lod, L.level, L.tile0, L.tile1, L.frac);
    if (!ok) { printf("     want level %u tiles %u %u frac %03x\n", want_level, want_t0, want_t1, want_frac); fails++; }
}
static void lod_units(void)
{
    const uint32_t L = RDP_H_TEX_LOD;
    /* one texel a pixel: level 0, fraction 0, blending toward level 1 */
    lod_case("1 texel: level 0", L, 0, 5, 32, 0, 2, 3, 0x00, 0);
    /* 2.5 texels: level 1 (2..4), a quarter of the way to level 2 */
    lod_case("2.5 texels: level 1, frac 1/4", L, 0, 5, 0, 80, 3, 4, 0x40, 1);
    /* 3.75 texels along T, 1 along S: the larger counts */
    lod_case("max of S and T", L, 0, 5, 32, 120, 3, 4, 0xE0, 1);
    /* half a texel: magnified, level 0 and no blend */
    lod_case("magnified", L, 0, 5, 16, 0, 2, 2, 0x00, 0);
    /* magnified with sharpen: the next level is extrapolated against, the fraction negative (0x100 up) */
    lod_case("magnified, sharpen", L | RDP_H_SHARPEN, 0, 5, 16, 0, 2, 3, 0x180, 0);
    /* sharpen's fraction is no less than the minimum level */
    lod_case("magnified, sharpen, min level 24", L | RDP_H_SHARPEN, 24, 5, 16, 0, 2, 3, 0x1C0, 0);
    /* detail, magnified: TEXEL0 the detail tile (the primitive's), TEXEL1 level 0 */
    lod_case("magnified, detail", L | RDP_H_DETAIL, 0, 5, 16, 0, 2, 3, 0x80, 0);
    /* detail, minified: the levels are a tile on */
    lod_case("2.5 texels, detail", L | RDP_H_DETAIL, 0, 5, 80, 0, 4, 5, 0x40, 1);
    /* 16 texels with 2 levels after the first: clamped to the last, at the far level (no blend, fraction all ones) */
    lod_case("past the last level", L, 0, 2, 512, 0, 4, 4, 0xFF, 2);
    /* a primitive with one level: always that one */
    lod_case("level count 0", L, 0, 0, 80, 0, 2, 2, 0xFF, 0);
    /* a negative step of one texel counts one less (ones' complement): magnified */
    lod_case("-1 texel: 31, magnified", L, 0, 5, -32, 0, 2, 2, 0x00, 0);
    /* without TEX_LOD the tiles stay the primitive's and the next; the fraction is still computed */
    lod_case("no TEX_LOD", 0, 0, 5, 0, 80, 2, 3, 0x40, 1);
    /* a coordinate out of range: the far level */
    {
        const int32_t s[3] = { 0, 0, 0 }, t[3] = { 0, 0, 0 };
        rdp_lod_t o = rdp_lod(L, 0, 3, 1, s, t, 1);
        int ok = o.tile0 == 4 && o.tile1 == 4 && o.frac == 0xFF && o.distant;
        printf("%s %-44s level %u tiles %u %u frac %03x\n", ok ? "ok  " : "FAIL", "out of range: the far level", o.level, o.tile0, o.tile1, o.frac);
        fails += !ok;
    }
}

/* ---- the console's perspective divide (rdp_n64_div) ----------------------------- */
static void div_case(const char *what, int32_t s16, int32_t w16, int32_t want, int want_range)
{
    int range;
    const int32_t v = rdp_n64_div((int32_t)((uint32_t)s16 << 16), (int32_t)((uint32_t)w16 << 16), &range);
    const int ok = range == want_range && (want_range || v == want);
    printf("%s %-44s S %6d / W %04x: %6d range %d\n", ok ? "ok  " : "FAIL", what, (int)s16, (unsigned)(w16 & 0xFFFF), (int)v, range);
    fails += !ok;
}
static void div_units(void)
{
    /* W 0x7FFF is nearly 1: the reciprocal's last point and slope, 0x2000 at its normalisation, S itself */
    div_case("S / 1", 100, 0x7FFF, 100, 0);
    /* W 0x4000 is a half: the table's first point exactly, twice S */
    div_case("S / 0.5", 100, 0x4000, 200, 0);
    div_case("-S / 0.5", -100, 0x4000, -200, 0);
    /* W 0x0100 (1/128) normalised by 6: 30000 * 128 is past the 17 bits, over (and under when negative) */
    div_case("past the 17 bits: over", 30000, 0x0100, 0, 2);
    div_case("past the 17 bits: under", -30000, 0x0100, 0, 1);
    /* a W whose integer half is 0 or negative is over, whatever S is */
    div_case("W 0: over", 0, 0x0000, 0, 2);
    div_case("W negative: over", 5, -0x100, 0, 2);
}

/* ---- a perspective floor --------------------------------------------------------- */
#define CIMG 0x100000u
#define W 320
#define H 240
#define LEVELS 6                         /* 32 x 32 down to 1 x 1 */
static const unsigned lvl_rgb[LEVELS][3] = { { 31, 0, 0 }, { 31, 16, 0 }, { 28, 28, 0 }, { 0, 28, 0 }, { 0, 24, 28 }, { 8, 8, 31 } };
static unsigned lvl_tmem[LEVELS];        /* 64-bit words */

/* the mip chain straight into TMEM (RGBA5551, the N64's layout: an odd row's 64-bit words with their halves changed
 * places, rdp.c tmem_fetch), and a tile for each level: tile k is level k, its S, T shifted down by k */
static void textures(void)
{
    unsigned at = 0;
    for (int k = 0; k < LEVELS; k++) {
        unsigned n = 32u >> k, line = n < 4 ? 1 : n / 4, mask = (unsigned)(5 - k);
        lvl_tmem[k] = at;
        for (unsigned tt = 0; tt < n; tt++)
            for (unsigned s = 0; s < n; s++) {
                /* a checker of 4-texel squares at level 0, each level its downsample: 2-texel squares, 1-texel, then
                 * (too small to hold it) the two shades' mean. (Single-texel squares at level 0 would come out of the
                 * 3-point filter as diagonal stripes.) */
                unsigned r = lvl_rgb[k][0], g = lvl_rgb[k][1], b = lvl_rgb[k][2];
                if (k <= 2) {
                    if (((s ^ tt) >> (2 - k)) & 1u) { r = r * 5 / 8; g = g * 5 / 8; b = b * 5 / 8; }
                } else { r = r * 13 / 16; g = g * 13 / 16; b = b * 13 / 16; }
                uint32_t i = ((at * 4u + tt * line * 4u + s) ^ ((tt & 1u) ? 2u : 0u)) & 0x7FFu;
                R.tmem16[i] = (uint16_t)(r << 11 | g << 6 | b << 1 | 1u);
            }
        cmd(0x35000000u | 2u << 19 | line << 9 | at, (uint32_t)k << 24 | mask << 14 | (unsigned)k << 10 | mask << 4 | (unsigned)k);
        cmd(0x32000000u, (uint32_t)k << 24 | ((n - 1) << 2) << 12 | ((n - 1) << 2));
        at += line * n;
    }
}

typedef struct { double x, y, s, t, w; } vtx_t;   /* screen x, y; s, t in level-0 texels; w the 1/z (normalised) */
static uint32_t fx(double v) { return (uint32_t)(int32_t)llround(v * 65536.0); }
/* An N64 textured triangle (opcode 0x0A) from three vertices, as a microcode sets it up: the edges, and the planes of
 * S * w, T * w and w (s10.5 and s.15 in their integer halves): the value at the major edge on YH's scanline, DaDx,
 * DaDe (along the major edge) and DaDy. */
static void tri(vtx_t a, vtx_t b, vtx_t c, unsigned level)
{
    vtx_t v[3] = { a, b, c }, tmp;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j].y > v[j + 1].y) { tmp = v[j]; v[j] = v[j + 1]; v[j + 1] = tmp; }
    double y1 = floor(v[0].y * 4) / 4, y2 = floor(v[1].y * 4) / 4, y3 = floor(v[2].y * 4) / 4;
    double dh = (v[2].x - v[0].x) / (v[2].y - v[0].y), dm = (v[1].x - v[0].x) / (v[1].y - v[0].y + 1e-9);
    double dl = (v[2].x - v[1].x) / (v[2].y - v[1].y + 1e-9);
    double yt = floor(y1);
    double xh = v[0].x + dh * (yt - v[0].y), xm = v[0].x + dm * (yt - v[0].y), xl = v[1].x + dl * (y2 - v[1].y);
    int lft = v[0].x + dh * (v[1].y - v[0].y) < v[1].x;
    cmd(0x0A000000u | (unsigned)lft << 23 | level << 19 | ((uint32_t)(y3 * 4) & 0x3FFF),
        ((uint32_t)(y2 * 4) & 0x3FFF) << 16 | ((uint32_t)(y1 * 4) & 0x3FFF));
    cmd(fx(xl), fx(dl)); cmd(fx(xh), fx(dh)); cmd(fx(xm), fx(dm));
    /* the planes A = p + q x + r y of S*w, T*w (texels * 32), w (2^31 = 1) */
    double A[3][3], val[3], dx[3], dy[3], de[3];
    for (int i = 0; i < 3; i++) {
        A[i][0] = v[i].s * v[i].w * 32.0; A[i][1] = v[i].t * v[i].w * 32.0; A[i][2] = v[i].w * 32767.0;
    }
    double det = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    for (int k = 0; k < 3; k++) {
        double d1 = A[1][k] - A[0][k], d2 = A[2][k] - A[0][k];
        dx[k] = (d1 * (v[2].y - v[0].y) - d2 * (v[1].y - v[0].y)) / det;
        dy[k] = (d2 * (v[1].x - v[0].x) - d1 * (v[2].x - v[0].x)) / det;
        de[k] = dy[k] + dx[k] * dh;
        val[k] = A[0][k] + dx[k] * (xh - v[0].x) + dy[k] * (yt - v[0].y);
    }
    uint32_t V[3], X[3], E[3], Y[3];
    for (int k = 0; k < 3; k++) { V[k] = fx(val[k]); X[k] = fx(dx[k]); E[k] = fx(de[k]); Y[k] = fx(dy[k]); }
#define HI2(a, b) (((a) & 0xFFFF0000u) | ((b) >> 16))
#define LO2(a, b) (((a) << 16) | ((b) & 0xFFFFu))
    cmd(HI2(V[0], V[1]), HI2(V[2], 0)); cmd(HI2(X[0], X[1]), HI2(X[2], 0));
    cmd(LO2(V[0], V[1]), LO2(V[2], 0)); cmd(LO2(X[0], X[1]), LO2(X[2], 0));
    cmd(HI2(E[0], E[1]), HI2(E[2], 0)); cmd(HI2(Y[0], Y[1]), HI2(Y[2], 0));
    cmd(LO2(E[0], E[1]), LO2(E[2], 0)); cmd(LO2(Y[0], Y[1]), LO2(Y[2], 0));
}

/* a floor 1 below the eye, from z = 0.7 to 60 and x = -10 to 10, 32 texels a unit; the horizon at y = 40 */
static vtx_t floor_pt(double x, double z)
{
    const double f = 160.0, zn = 0.7;
    vtx_t p = { 160.0 + f * x / z, 40.0 + f / z, (x + 10.0) * 32.0, (z - zn) * 32.0, zn / z };
    return p;
}

/* the level whose colour a colour is nearest (the checker's shades all count) */
static int level_of(int r, int g, int b)
{
    int best = -1, bd = 1 << 30;
    for (int k = 0; k < LEVELS; k++)
        for (int shade = 0; shade < 3; shade++) {
            static const int num[3] = { 16, 10, 13 };
            int lr = (int)lvl_rgb[k][0] * num[shade] / 16, lg = (int)lvl_rgb[k][1] * num[shade] / 16, lb = (int)lvl_rgb[k][2] * num[shade] / 16;
            int d = (r - lr) * (r - lr) + (g - lg) * (g - lg) + (b - lb) * (b - lb);
            if (d < bd) { bd = d; best = k; }
        }
    return best;
}

int main(void)
{
    lod_units();
    div_units();
    rdp_init(&R, 0, rd16, wr16);
    cmd(0x3F000000u | 2u << 19 | (W - 1), CIMG);                              /* 16-bit colour image */
    cmd(0x2D000000u, (W << 2) << 12 | (H << 2));
    cmd(0x37000000u, 0x00010001u);                                            /* clear: black */
    cmd(0x2F000000u | 3u << 20, 0);
    cmd(0x36000000u | ((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
    textures();
    /* combiner, cycle 0: (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0, alpha 1; cycle 1: COMBINED */
    cmd(0x3C000000u | 2u << 20 | 13u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u,
        1u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 6u << 9 | 0u << 6 | 7u << 3 | 0u);
    /* 2-cycle, perspective, TEX_LOD, the 3-point filter (BI_LERP both), no dithers; the blender passes the pixel */
    cmd(0x2F000000u | 1u << 20 | RDP_H_PERSP | RDP_H_TEX_LOD | 2u << 12 | RDP_H_BILERP0 | RDP_H_BILERP1 | 3u << 6 | 3u << 4, 0);
    vtx_t a = floor_pt(-10, 0.7), b = floor_pt(10, 0.7), c = floor_pt(10, 60), d = floor_pt(-10, 60);
    tri(a, b, c, LEVELS - 1);
    tri(a, c, d, LEVELS - 1);

    FILE *f = fopen("test_lod.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) {
            uint16_t v = rd16(0, CIMG + 2u * (uint32_t)i);
            unsigned r5 = v >> 11, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
            uint8_t px[3] = { (uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g5 << 3 | g5 >> 2), (uint8_t)(b5 << 3 | b5 >> 2) };
            fwrite(px, 1, 3, f);
        }
        fclose(f);
    }
    /* the level down the middle, row by row (the mean of 24 pixels: the checker's shades average out), where it changes */
    int prev = -2, monotonic = 1, top = -1, bottom = -1;
    for (int y = H - 1; y > 40; y--) {
        int sr = 0, sg = 0, sb = 0, n = 0;
        for (int x = W / 2 - 12; x < W / 2 + 12; x++) {
            uint16_t v = rd16(0, CIMG + 2u * (uint32_t)(y * W + x));
            if (!(v & 0xFFFE)) continue;
            sr += v >> 11; sg += (v >> 6) & 31; sb += (v >> 1) & 31; n++;
        }
        if (n < 24) continue;
        int k = level_of(sr / n, sg / n, sb / n);
        if (bottom < 0) bottom = k;
        if (k != prev) { printf("     row %3d: level %d\n", y, k); if (prev >= 0 && k < prev) monotonic = 0; prev = k; }
        top = k;
    }
    int ok = monotonic && bottom == 0 && top == LEVELS - 1;
    printf("%s the levels rise with the distance, 0 near to %d at the horizon\n", ok ? "ok  " : "FAIL", LEVELS - 1);
    fails += !ok;
    printf("pixels drawn %llu, %s\n", (unsigned long long)R.pixels_drawn, fails ? "FAILED" : "all ok");
    printf("LOD pixels %llu (magnified %llu, at the far level %llu); by level:", (unsigned long long)R.lod_px,
           (unsigned long long)R.lod_mag, (unsigned long long)R.lod_far);
    for (int k = 0; k < 8; k++) printf(" %llu", (unsigned long long)R.lod_lvl[k]);
    printf("\n");
    return fails != 0;
}
