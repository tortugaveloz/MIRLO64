/* rdpgen: command streams (.rdps, README.md) for rdpo.
 *
 *   rdpgen SUITE [SEED [N]] > stream.rdps
 *
 * Suites:
 *   cal     calibration, no LOD: textured, shaded, depth-tested triangles and
 *           texture rectangles in 1- and 2-cycle mode, affine and perspective
 *   units   test_lod.c's worked steps, each an affine triangle (left- and
 *           right-major) whose S step a pixel and T step a line are the step
 *   floor   test_lod.c's perspective floor: six levels, 2-cycle, TEX_LOD,
 *           (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0
 *   lod2    random 2-cycle triangles with TEX_LOD (levels 1..7, detail and
 *           sharpen on and off, perspective on and off, magnified and minified,
 *           left- and right-major), combiners that read LOD_FRACTION
 *   frac2   the same without TEX_LOD (the combiner reads LOD_FRACTION)
 *   lod1    1-cycle triangles with TEX_LOD
 *   rect    texture rectangles (and flipped ones) with TEX_LOD, 1- and 2-cycle
 *
 * Environment: RDPGEN_LEVEL, RDPGEN_DETAIL, RDPGEN_SHARPEN, RDPGEN_TEXLOD force those fields (lod1: TEX_LOD);
 * RDPGEN_UNTEXTURED=k: one lod2 / frac2 triangle in k without texture coefficients (8; 0 none). RDPGEN_EXACT=1 keeps triangles on whole pixels with a vertical
 * major edge and steps whose low bits are 0 (no attribute rounding on either
 * side); RDPGEN_PERSP=0/1 forces perspective off/on; RDPGEN_CC=k forces combiner k.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned rng;
static unsigned rr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static unsigned rn(unsigned n) { return rr() % n; }
static double rf(double a, double b) { return a + (b - a) * (rr() / 4294967296.0); }

#define CIMG 0x100000u
#define ZIMG 0x140000u
#define TEXB 0x200000u
#define W 320
#define H 240

static void cmd(int n, ...)
{
    __builtin_va_list ap; __builtin_va_start(ap, n);
    printf("cmd");
    for (int i = 0; i < n; i++) printf(" %08x", __builtin_va_arg(ap, unsigned));
    printf("\n");
    __builtin_va_end(ap);
}
static void cmdv(const uint32_t *w, int n) { printf("cmd"); for (int i = 0; i < n; i++) printf(" %08x", w[i]); printf("\n"); }
static void mem(uint32_t a, const uint8_t *b, int n) { printf("mem %06x ", a); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n"); }

/* ---- textures ----------------------------------------------------------------------------------------------- */
/* tile k: an RGBA16 texture of its own colour with a checker, size w x h (w, h powers of two), loaded with
 * LOAD_TILE through tile 7 (at least 4 texels a row are loaded: a narrower one wraps by its mask) */
static unsigned tmem_at;                 /* 64-bit words */
static const unsigned pal[8][3] = { { 31, 0, 0 }, { 31, 16, 0 }, { 28, 28, 0 }, { 0, 28, 0 }, { 0, 24, 28 }, { 8, 8, 31 }, { 24, 0, 28 }, { 28, 28, 28 } };
static void texture(int k, unsigned w, unsigned h, unsigned shift, int checker_sh, int clamp_st)
{
    unsigned lw = w < 4 ? 4 : w;
    static uint8_t buf[64 * 64 * 2];
    for (unsigned t = 0; t < h; t++)
        for (unsigned s = 0; s < lw; s++) {
            unsigned r = pal[k][0], g = pal[k][1], b = pal[k][2];
            int dark = checker_sh >= 0 ? (int)(((s ^ t) >> checker_sh) & 1u) : 0;
            if (dark) { r = r * 5 / 8; g = g * 5 / 8; b = b * 5 / 8; }
            /* a gradient in the low bits: a texel's place shows */
            r = (r & ~3u) | (s & 3u); b = (b & ~3u) | (t & 3u);
            uint16_t v = (uint16_t)(r << 11 | g << 6 | b << 1 | 1u);
            buf[2 * (t * lw + s)] = (uint8_t)(v >> 8); buf[2 * (t * lw + s) + 1] = (uint8_t)v;
        }
    uint32_t addr = TEXB + (uint32_t)k * 0x4000u;
    mem(addr, buf, (int)(lw * h * 2));
    unsigned line = lw / 4;                           /* 64-bit words a row */
    unsigned ms = 0; while ((1u << ms) < w) ms++;
    unsigned mt = 0; while ((1u << mt) < h) mt++;
    cmd(2, 0x3D000000u | 2u << 19 | (lw - 1), addr);                                         /* SET_TEXTURE_IMAGE RGBA16 */
    cmd(2, 0x35000000u | 2u << 19 | line << 9 | tmem_at, 7u << 24);                           /* load tile 7 */
    cmd(2, 0x27000000u, 0);
    cmd(2, 0x34000000u, 7u << 24 | ((lw - 1) << 2) << 12 | ((h - 1) << 2));                   /* LOAD_TILE */
    cmd(2, 0x27000000u, 0);
    unsigned cs = clamp_st & 1, ct = (clamp_st >> 1) & 1;
    cmd(2, 0x35000000u | 2u << 19 | line << 9 | tmem_at,
        (uint32_t)k << 24 | ct << 19 | mt << 14 | shift << 10 | cs << 9 | ms << 4 | shift);
    cmd(2, 0x32000000u, (uint32_t)k << 24 | ((w - 1) << 2) << 12 | ((h - 1) << 2));
    tmem_at += line * h;
}

/* ---- setup -------------------------------------------------------------------------------------------------- */
static void setup(int clear_z)
{
    printf("rdram zero\n");
    cmd(2, 0x3F000000u | 2u << 19 | (W - 1), CIMG);
    cmd(2, 0x3E000000u, ZIMG);
    cmd(2, 0x2D000000u, (W << 2) << 12 | (H << 2));
    cmd(2, 0x2F000000u | 3u << 20, 0);                                                   /* fill mode */
    if (clear_z) {
        cmd(2, 0x3F000000u | 2u << 19 | (W - 1), ZIMG);
        cmd(2, 0x37000000u, 0xFFFCFFFCu);
        cmd(2, 0x36000000u | ((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
        cmd(2, 0x27000000u, 0);
        cmd(2, 0x3F000000u | 2u << 19 | (W - 1), CIMG);
    }
    cmd(2, 0x37000000u, 0x42114211u);                                                     /* a grey, coverage bit 1 */
    cmd(2, 0x36000000u | ((W - 1) << 2) << 12 | ((H - 1) << 2), 0);
    cmd(2, 0x27000000u, 0);
}

/* ---- combiners ---------------------------------------------------------------------------------------------- */
#define CC(a0, c0, Aa0, Ac0, a1, c1, b0, b1, Aa1, Ac1, d0, Ab0, Ad0, d1, Ab1, Ad1) \
    cmd(2, 0x3C000000u | (a0) << 20 | (c0) << 15 | (Aa0) << 12 | (Ac0) << 9 | (a1) << 5 | (c1), \
        (uint32_t)(b0) << 28 | (b1) << 24 | (Aa1) << 21 | (Ac1) << 18 | (d0) << 15 | (Ab0) << 12 | (Ad0) << 9 | (d1) << 6 | (Ab1) << 3 | (Ad1))
/* 2-cycle combiners; cycle 1 never reads TEXEL1 (the oracle's is the next pixel's texel, a known difference) */
static void combiner2(int k)
{
    switch (k) {
    case 0:   /* (TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0, both channels; then COMBINED */
        CC(2, 13, 2, 0, 15, 31, 1, 15, 7, 7, 1, 1, 1, 0, 7, 0); break;
    case 1:   /* LOD_FRACTION itself as the colour and the alpha (no texel read) */
        CC(6, 13, 6, 0, 15, 31, 8, 15, 7, 7, 7, 7, 7, 0, 7, 0); break;
    case 2:   /* TEXEL0, then (TEXEL0 - COMBINED) * LOD_FRACTION + COMBINED: the second cycle's TEXEL0 is the TEXEL1 tile's */
        CC(15, 31, 7, 7, 1, 13, 15, 0, 1, 0, 1, 7, 1, 0, 0, 0); break;
    case 5:   /* TEXEL0, then TEXEL1 in the second cycle (the oracle: the next pixel's texel; the model: the tile's own) */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 1, 7, 1, 2, 7, 2); break;
    case 3:   /* (ENV - PRIM) * LOD_FRACTION + PRIM: the signed fraction, sharpen's included (ENV 255, PRIM 128) */
        CC(5, 13, 5, 0, 15, 31, 3, 15, 7, 7, 3, 3, 3, 0, 7, 0); break;
    default:  /* TEXEL0 alone, then COMBINED: only the tile shows */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 1, 7, 1, 0, 7, 0); break;
    }
}
/* 1-cycle combiners (the second cycle's selects are the one cycle's) */
static void combiner1(int k)
{
    switch (k) {
    case 0:   /* TEXEL0 */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 7, 7, 7, 1, 7, 1); break;
    case 1:   /* LOD_FRACTION */
        CC(15, 31, 7, 7, 6, 13, 15, 8, 6, 0, 7, 7, 7, 7, 7, 7); break;
    case 2:   /* (ENV - TEXEL0) * LOD_FRACTION + TEXEL0 */
        CC(15, 31, 7, 7, 5, 13, 15, 1, 5, 0, 7, 7, 7, 1, 1, 1); break;
    case 3:   /* TEXEL1 (the oracle: the next pixel's texel; only with RDPGEN_CC=3) */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 7, 7, 7, 2, 7, 2); break;
    case 4:   /* PRIM */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 7, 7, 7, 3, 7, 3); break;
    case 5:   /* SHADE */
        CC(15, 31, 7, 7, 15, 31, 15, 15, 7, 7, 7, 7, 7, 4, 7, 4); break;
    default:  /* TEXEL0 * SHADE */
        CC(15, 31, 7, 7, 1, 4, 15, 8, 1, 4, 7, 7, 7, 7, 7, 7); break;
    }
}

/* ---- triangles ---------------------------------------------------------------------------------------------- */
typedef struct { double x, y, s, t, w, r, g, b, a, z; } vtx_t;
static uint32_t fx(double v) { double q = floor(v * 65536.0 + 0.5); if (q > 2147483647.0) q = 2147483647.0; if (q < -2147483648.0) q = -2147483648.0; return (uint32_t)(int32_t)(int64_t)q; }
static int exact_mode;
/* A triangle as a microcode sets it up: edges, then the planes (value at the major edge on YH's whole row, DaDx,
 * DaDe, DaDy) of shade, S*W, T*W (s10.5 in the integer half), W (s.15: 0x7FFF ~ 1), Z. flags: 4 shade 2 tex 1 z. */
static void tri(vtx_t a, vtx_t b, vtx_t c, unsigned flags, unsigned level, unsigned tile, int persp)
{
    vtx_t v[3] = { a, b, c }, tmp;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2 - i; j++)
            if (v[j].y > v[j + 1].y) { tmp = v[j]; v[j] = v[j + 1]; v[j + 1] = tmp; }
    double y1 = floor(v[0].y * 4) / 4, y2 = floor(v[1].y * 4) / 4, y3 = floor(v[2].y * 4) / 4;
    if (y3 <= y1) return;
    double dh = (v[2].x - v[0].x) / (v[2].y - v[0].y), dm = v[1].y > v[0].y ? (v[1].x - v[0].x) / (v[1].y - v[0].y) : 0;
    double dl = v[2].y > v[1].y ? (v[2].x - v[1].x) / (v[2].y - v[1].y) : 0;
    double yt = floor(y1);
    double xh = v[0].x + dh * (yt - v[0].y), xm = v[0].x + dm * (yt - v[0].y), xl = v[1].x + dl * (y2 - v[1].y);
    int lft = v[0].x + dh * (v[1].y - v[0].y) < v[1].x;
    uint32_t w[44]; int n = 0;
    w[n++] = (0x08u | flags) << 24 | (unsigned)lft << 23 | (level & 7) << 19 | (tile & 7) << 16 | ((uint32_t)(int)(y3 * 4) & 0x3FFF);
    w[n++] = ((uint32_t)(int)(y2 * 4) & 0x3FFF) << 16 | ((uint32_t)(int)(y1 * 4) & 0x3FFF);
    w[n++] = fx(xl); w[n++] = fx(dl); w[n++] = fx(xh); w[n++] = fx(dh); w[n++] = fx(xm); w[n++] = fx(dm);
    double det = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[2].x - v[0].x) * (v[1].y - v[0].y);
    if (fabs(det) < 1e-9) return;
    /* a plane through the three values: at (xh, yt), DaDx, DaDy, DaDe */
    #define PLANE(A0, A1, A2, VAL, DX, DE, DY) do { \
        double d1 = (A1) - (A0), d2 = (A2) - (A0); \
        double px = (d1 * (v[2].y - v[0].y) - d2 * (v[1].y - v[0].y)) / det; \
        double py = (d2 * (v[1].x - v[0].x) - d1 * (v[2].x - v[0].x)) / det; \
        DX = px; DY = py; DE = py + px * dh; VAL = (A0) + px * (xh - v[0].x) + py * (yt - v[0].y); } while (0)
    if (flags & 4) {
        double V[4], X[4], E[4], Y[4];
        PLANE(v[0].r, v[1].r, v[2].r, V[0], X[0], E[0], Y[0]);
        PLANE(v[0].g, v[1].g, v[2].g, V[1], X[1], E[1], Y[1]);
        PLANE(v[0].b, v[1].b, v[2].b, V[2], X[2], E[2], Y[2]);
        PLANE(v[0].a, v[1].a, v[2].a, V[3], X[3], E[3], Y[3]);
        uint32_t q[4][4];
        for (int k = 0; k < 4; k++) { q[0][k] = fx(V[k]); q[1][k] = fx(X[k]); q[2][k] = fx(E[k]); q[3][k] = fx(Y[k]); }
        #define HI2(p, r) (((p) & 0xFFFF0000u) | ((r) >> 16))
        #define LO2(p, r) (((p) << 16) | ((r) & 0xFFFFu))
        w[n++] = HI2(q[0][0], q[0][1]); w[n++] = HI2(q[0][2], q[0][3]); w[n++] = HI2(q[1][0], q[1][1]); w[n++] = HI2(q[1][2], q[1][3]);
        w[n++] = LO2(q[0][0], q[0][1]); w[n++] = LO2(q[0][2], q[0][3]); w[n++] = LO2(q[1][0], q[1][1]); w[n++] = LO2(q[1][2], q[1][3]);
        w[n++] = HI2(q[2][0], q[2][1]); w[n++] = HI2(q[2][2], q[2][3]); w[n++] = HI2(q[3][0], q[3][1]); w[n++] = HI2(q[3][2], q[3][3]);
        w[n++] = LO2(q[2][0], q[2][1]); w[n++] = LO2(q[2][2], q[2][3]); w[n++] = LO2(q[3][0], q[3][1]); w[n++] = LO2(q[3][2], q[3][3]);
    }
    if (flags & 2) {
        double A[3][3], V[3], X[3], E[3], Y[3];
        for (int i = 0; i < 3; i++) {
            double ww = persp ? v[i].w : 1.0;
            A[i][0] = v[i].s * 32.0 * ww; A[i][1] = v[i].t * 32.0 * ww; A[i][2] = ww * 32767.0;
        }
        for (int k = 0; k < 3; k++) PLANE(A[0][k], A[1][k], A[2][k], V[k], X[k], E[k], Y[k]);
        uint32_t q[4][3];
        for (int k = 0; k < 3; k++) {
            q[0][k] = fx(V[k]); q[1][k] = fx(X[k]); q[2][k] = fx(E[k]); q[3][k] = fx(Y[k]);
            if (exact_mode) for (int j = 0; j < 4; j++) q[j][k] &= ~0x1FFFu;
        }
        w[n++] = HI2(q[0][0], q[0][1]); w[n++] = HI2(q[0][2], 0); w[n++] = HI2(q[1][0], q[1][1]); w[n++] = HI2(q[1][2], 0);
        w[n++] = LO2(q[0][0], q[0][1]); w[n++] = LO2(q[0][2], 0); w[n++] = LO2(q[1][0], q[1][1]); w[n++] = LO2(q[1][2], 0);
        w[n++] = HI2(q[2][0], q[2][1]); w[n++] = HI2(q[2][2], 0); w[n++] = HI2(q[3][0], q[3][1]); w[n++] = HI2(q[3][2], 0);
        w[n++] = LO2(q[2][0], q[2][1]); w[n++] = LO2(q[2][2], 0); w[n++] = LO2(q[3][0], q[3][1]); w[n++] = LO2(q[3][2], 0);
    }
    if (flags & 1) {
        double V, X, E, Y;
        PLANE(v[0].z, v[1].z, v[2].z, V, X, E, Y);
        w[n++] = fx(V); w[n++] = fx(X); w[n++] = fx(E); w[n++] = fx(Y);
    }
    cmdv(w, n);
}

/* other modes: dithers off (they read noise), the 3-point filter or point, the blender passing the pixel */
static uint32_t modes_h(int cyc, int persp, int detail, int sharpen, int texlod, int bilerp)
{
    return (uint32_t)cyc << 20 | (persp ? 1u : 0u) << 19 | (detail ? 1u : 0u) << 18 | (sharpen ? 1u : 0u) << 17 | (texlod ? 1u : 0u) << 16
         | (bilerp ? 2u : 0u) << 12 | 1u << 11 | 1u << 10 | 3u << 6 | 3u << 4;
}

static vtx_t vtx(double x, double y, double s, double t, double w)
{
    vtx_t p = { x, y, s, t, w, rf(0, 255), rf(0, 255), rf(0, 255), rf(0, 255), rf(100, 32000) };
    return p;
}

/* a random triangle on screen with texture coordinates: scale is the texels a pixel (level 0) */
static void rand_tri(unsigned flags, unsigned level, unsigned tile, int persp)
{
    double cx = rf(20, W - 20), cy = rf(20, H - 20), sz = rn(4) ? rf(4, 60) : rf(60, 200);
    vtx_t p[3];
    double scale = exp(rf(log(1.0 / 8), log(rn(3) ? 64.0 : 1024.0)));
    double ang = rf(0, 6.2832), aniso = rn(3) ? 1.0 : rf(0.2, 5);
    double s0 = rf(-200, 200), t0 = rf(-200, 200);
    for (int i = 0; i < 3; i++) {
        double x = cx + rf(-sz, sz), y = cy + rf(-sz, sz);
        if (exact_mode) { x = floor(x); y = floor(y); }
        double u = (x - cx) * scale, vv = (y - cy) * scale * aniso;
        double s = s0 + u * cos(ang) - vv * sin(ang), t = t0 + u * sin(ang) + vv * cos(ang);
        double w = persp ? rf(0.02, 1.0) : 1.0;
        p[i] = vtx(x, y, s, t, w);
    }
    if (exact_mode) { p[1].x = p[0].x; }          /* a vertical edge (often the major one) */
    /* keep S*W, T*W and the steps in range for the s10.5 integer halves */
    for (int i = 0; i < 3; i++) {
        if (p[i].s > 1000) p[i].s = 1000; if (p[i].s < -1000) p[i].s = -1000;
        if (p[i].t > 1000) p[i].t = 1000; if (p[i].t < -1000) p[i].t = -1000;
    }
    tri(p[0], p[1], p[2], flags, level, tile, persp);
}

static void random_textures(void)
{
    tmem_at = 0;
    for (int k = 0; k < 8; k++) {
        unsigned sw = 8u << rn(2), sh = 8u << rn(2);
        texture(k, sw, sh, 0, (int)rn(3) - 1, rn(4) == 0 ? (int)rn(4) : 0);
    }
}

static int env_int(const char *n, int dflt) { const char *v = getenv(n); return v ? atoi(v) : dflt; }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: rdpgen cal|units|floor|lod2|frac2|lod1|rect [seed [n]]\n"); return 2; }
    const char *suite = argv[1];
    unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], 0, 0) : 1;
    int nprim = argc > 3 ? atoi(argv[3]) : 12;
    rng = seed * 2654435761u + 0x9E3779B9u; if (!rng) rng = 1;
    for (int i = 0; i < 8; i++) rr();
    exact_mode = env_int("RDPGEN_EXACT", 0);
    const int fpersp = env_int("RDPGEN_PERSP", -1), fcc = env_int("RDPGEN_CC", -1);
    const int flevel = env_int("RDPGEN_LEVEL", -1), fdetail = env_int("RDPGEN_DETAIL", -1), fsharpen = env_int("RDPGEN_SHARPEN", -1);
    const int ftexlod = env_int("RDPGEN_TEXLOD", -1), fnotex = env_int("RDPGEN_UNTEXTURED", 8);
    printf("# rdpgen %s %u %d%s\n", suite, seed, nprim, exact_mode ? " (exact)" : "");

    if (!strcmp(suite, "units")) {
        /* test_lod.c's steps: tile 2, level count 5 unless said; ds a pixel (S), dt a line (T), in s10.5 */
        static const struct { const char *what; uint32_t h; unsigned minl, maxl; int ds, dt; unsigned tf, dyf; } u[] = {
            { "1 texel", 1, 0, 5, 32, 0 }, { "2.5 texels", 1, 0, 5, 0, 80 }, { "S 1, T 3.75", 1, 0, 5, 32, 120 },
            { "half a texel", 1, 0, 5, 16, 0 }, { "half, sharpen", 3, 0, 5, 16, 0 }, { "half, sharpen, min 24", 3, 24, 5, 16, 0 },
            { "half, detail", 5, 0, 5, 16, 0 }, { "2.5, detail", 5, 0, 5, 80, 0 }, { "16 texels, 2 levels", 1, 0, 2, 512, 0 },
            { "2.5, level count 0", 1, 0, 0, 80, 0 }, { "-1 texel", 1, 0, 5, -32, 0 }, { "2.5, no TEX_LOD", 0, 0, 5, 0, 80 },
            { "-2.5 texels", 1, 0, 5, -80, 0 }, { "-1 texel line", 1, 0, 5, 0, -32 }, { "512 texels", 1, 0, 7, 512 * 32, 0 },
            { "300 texels", 5, 0, 7, 300 * 32, 0 }, { "half, detail, min 31", 5, 31, 5, 16, 0 }, { "1 texel, sharpen", 3, 0, 5, 32, 0 },
            /* DtDy with a fraction under the 17 bits LOD_DY_MASK keeps (0x7C00), and with its top bit (0x8000): T starts at .5 + 1/64 */
            { "DtDy fraction 0x7C00, sharpen", 3, 0, 5, 0, 32, 0x8400, 0x7C00 }, { "DtDy fraction 0x8000, sharpen", 3, 0, 5, 0, 32, 0x8400, 0x8000 },
        };
        setup(0);
        random_textures();
        const int nu = (int)(sizeof u / sizeof u[0]);
        int cc = fcc >= 0 ? fcc : (int)(seed % 4);
        cmd(2, 0x3B000000u, 0xFFFFFFFFu); /* ENV white */
        combiner2(cc);
        for (int i = 0; i < nu; i++)
            for (int lft = 0; lft < 2; lft++) {
                uint32_t h = modes_h(1, 0, (u[i].h & 4) != 0, (u[i].h & 2) != 0, (u[i].h & 1) != 0, 0);
                cmd(2, 0x2F000000u | h, 0);
                cmd(2, 0x3A000000u | u[i].minl << 8 | 0x40u, 0x808080FFu);   /* PRIM 128, min level */
                /* a right triangle 14 x 6 pixels: S = s0 + ds * x, T = t0 + dt * y, in s10.5; the vertical edge is the major one */
                int x0 = 6 + (i % 6) * 52 + lft * 24, y0 = 8 + (i / 6) * 12 + lft * 0;
                int32_t s0 = 4096, t0 = 4096;
                uint32_t wv[44]; int n = 0;
                unsigned yl = (unsigned)(y0 + 8) * 4, yh = (unsigned)y0 * 4;
                /* lft: the major edge (vertical) on the left, the minor on the right; else mirrored */
                int32_t xmaj = (lft ? x0 : x0 + 20) << 16, xmin = (lft ? x0 + 20 : x0) << 16;
                wv[n++] = 0x0Au << 24 | (unsigned)lft << 23 | (u[i].maxl & 7) << 19 | 2u << 16 | yl;
                wv[n++] = yl << 16 | yh;
                wv[n++] = (uint32_t)xmin; wv[n++] = 0; wv[n++] = (uint32_t)xmaj; wv[n++] = 0; wv[n++] = (uint32_t)xmin; wv[n++] = 0;
                /* S, T, W at the major edge on the top row; DsDx = ds (sign by the walk: the step to x + 1), DtDy = dt */
                int32_t sv = s0 + (lft ? 0 : u[i].ds * 20), tv = t0;
                int32_t S = sv << 16, T = (int32_t)((uint32_t)tv << 16 | u[i].tf), Wv = 0x7FFF << 16, dSx = u[i].ds << 16, dTy = (int32_t)((uint32_t)u[i].dt << 16 | u[i].dyf);
                uint32_t q[4][3] = { { (uint32_t)S, (uint32_t)T, (uint32_t)Wv }, { (uint32_t)dSx, 0, 0 }, { 0, (uint32_t)dTy, 0 }, { 0, (uint32_t)dTy, 0 } };
                wv[n++] = HI2(q[0][0], q[0][1]); wv[n++] = HI2(q[0][2], 0); wv[n++] = HI2(q[1][0], q[1][1]); wv[n++] = HI2(q[1][2], 0);
                wv[n++] = LO2(q[0][0], q[0][1]); wv[n++] = LO2(q[0][2], 0); wv[n++] = LO2(q[1][0], q[1][1]); wv[n++] = LO2(q[1][2], 0);
                wv[n++] = HI2(q[2][0], q[2][1]); wv[n++] = HI2(q[2][2], 0); wv[n++] = HI2(q[3][0], q[3][1]); wv[n++] = HI2(q[3][2], 0);
                wv[n++] = LO2(q[2][0], q[2][1]); wv[n++] = LO2(q[2][2], 0); wv[n++] = LO2(q[3][0], q[3][1]); wv[n++] = LO2(q[3][2], 0);
                printf("# %s, %s-major\n", u[i].what, lft ? "left" : "right");
                cmdv(wv, n);
            }
        cmd(2, 0x29000000u, 0);
        return 0;
    }

    if (!strcmp(suite, "floor")) {
        setup(0);
        tmem_at = 0;
        for (int k = 0; k < 6; k++) { unsigned n = 32u >> k; texture(k, n, n, (unsigned)k, k <= 2 ? 2 - k : -1, 0); }
        cmd(2, 0x3C000000u | 2u << 20 | 13u << 15 | 7u << 12 | 7u << 9 | 15u << 5 | 31u,
            1u << 28 | 15u << 24 | 7u << 21 | 7u << 18 | 1u << 15 | 7u << 12 | 6u << 9 | 0u << 6 | 7u << 3 | 0u);
        int persp = fpersp >= 0 ? fpersp : 1;
        unsigned extra = seed == 2 ? (1u << 17) : seed == 3 ? (1u << 18) : 0;      /* seed 2 sharpen, 3 detail */
        cmd(2, 0x2F000000u | modes_h(1, persp, 0, 0, 1, 1) | extra, 0);
        const double f = 160.0, zn = 0.7;
        vtx_t P[4];
        const double X[4] = { -10, 10, 10, -10 }, Z[4] = { 0.7, 0.7, 60, 60 };
        for (int i = 0; i < 4; i++) { vtx_t p = { 160.0 + f * X[i] / Z[i], 40.0 + f / Z[i], (X[i] + 10.0) * 32.0, (Z[i] - zn) * 32.0, zn / Z[i], 0, 0, 0, 0, 0 }; P[i] = p; }
        tri(P[0], P[1], P[2], 2, 5, 0, persp);
        tri(P[0], P[2], P[3], 2, 5, 0, persp);
        cmd(2, 0x29000000u, 0);
        return 0;
    }

    int is_cal = !strcmp(suite, "cal"), is_lod2 = !strcmp(suite, "lod2"), is_frac2 = !strcmp(suite, "frac2");
    int is_lod1 = !strcmp(suite, "lod1"), is_rect = !strcmp(suite, "rect");
    if (!(is_cal || is_lod2 || is_frac2 || is_lod1 || is_rect)) { fprintf(stderr, "unknown suite %s\n", suite); return 2; }
    setup(1);
    random_textures();
    cmd(2, 0x3B000000u, 0xFFFFFFFFu);       /* ENV: white */
    for (int i = 0; i < nprim; i++) {
        int persp = fpersp >= 0 ? fpersp : (int)rn(2);
        int detail = rn(3) == 0, sharpen = rn(3) == 0, bil = (int)rn(2);
        unsigned level = 1 + rn(7), tile = rn(8), minl = rn(4) ? 0 : rn(32);
        if (flevel >= 0) level = (unsigned)flevel;
        if (fdetail >= 0) detail = fdetail;
        if (fsharpen >= 0) sharpen = fsharpen;
        cmd(2, 0x3A000000u | minl << 8 | rn(256), 0x80808080u);   /* PRIM 128, min level */
        uint32_t lo = 0;
        if (is_cal) {
            const char *ck = getenv("RDPGEN_CAL");
            if (ck) {
                /* one feature at a time: flat, shade, z, aa, aff, persp, rect, t1 (2-cycle TEXEL1) */
                int t1 = !strcmp(ck, "t1"), rect = !strcmp(ck, "rect"), tex = t1 || rect || !strcmp(ck, "aff") || !strcmp(ck, "persp");
                int pp = !strcmp(ck, "persp"), zz = !strcmp(ck, "z"), aa = !strcmp(ck, "aa"), sh = !strcmp(ck, "shade");
                cmd(2, 0x2F000000u | modes_h(t1 ? 1 : 0, pp, 0, 0, 0, 0), (zz ? (1u << 4 | 1u << 5) : 0u) | (aa ? (1u << 3 | 2u << 8 | 1u << 7) : 0u));
                if (t1) combiner2(5); else combiner1(tex ? 0 : sh ? 5 : 4);
                if (rect) {
                    unsigned x0 = rn(4 * 280), y0 = rn(4 * 200), x1 = x0 + 4 + rn(4 * 40), y1 = y0 + 4 + rn(4 * 40);
                    cmd(4, (rn(4) ? 0x24000000u : 0x25000000u) | x1 << 12 | y1, (uint32_t)tile << 24 | x0 << 12 | y0, rn(1 << 14) << 16 | rn(1 << 14), (0x100u + rn(0x700)) << 16 | (0x100u + rn(0x700)));
                } else rand_tri((tex ? 2u : 0u) | (zz ? 1u : 0u) | (sh ? 4u : 0u), 0, tile, pp);
                continue;
            }
            int cyc = (int)rn(2);
            unsigned zf = rn(2);
            lo = zf ? (1u << 4 | 1u << 5) : 0;                   /* Z compare and update, opaque */
            if (rn(3) == 0) lo |= 1u << 3 | 2u << 8 | 1u << 7;  /* AA, CVG_DST zap, CLR_ON_CVG */
            cmd(2, 0x2F000000u | modes_h(cyc, persp, 0, 0, 0, bil), lo);
            if (cyc == 1) combiner2(fcc >= 0 ? fcc : (rn(2) ? 4 : 0)); else combiner1(fcc >= 0 ? fcc : (rn(2) ? 0 : 3));
            if (rn(5) == 0) {
                unsigned x0 = rn(4 * 280), y0 = rn(4 * 200), x1 = x0 + 4 + rn(4 * 40), y1 = y0 + 4 + rn(4 * 40);
                cmd(4, 0x24000000u | x1 << 12 | y1, (uint32_t)tile << 24 | x0 << 12 | y0, rn(1 << 14) << 16 | rn(1 << 14), (0x200u + rn(0x600)) << 16 | (0x200u + rn(0x600)));
            } else rand_tri(2 | (zf ? 1u : 0u) | (rn(2) ? 4u : 0u), 0, tile, persp);
            continue;
        }
        if (is_lod1) {
            cmd(2, 0x2F000000u | modes_h(0, persp, detail, sharpen, ftexlod >= 0 ? ftexlod : 1, bil), lo);
            combiner1(fcc >= 0 ? fcc : (int)rn(3));
            rand_tri(2, level, tile, persp);
            continue;
        }
        if (is_rect) {
            int cyc = (int)rn(2);
            cmd(2, 0x2F000000u | modes_h(cyc, 0, detail, sharpen, 1, bil), lo);
            if (cyc == 1) combiner2(fcc >= 0 ? fcc : (int)rn(5)); else combiner1(fcc >= 0 ? fcc : (int)rn(3));
            unsigned x0 = rn(4 * 280), y0 = rn(4 * 200), x1 = x0 + 4 + rn(4 * 40), y1 = y0 + 4 + rn(4 * 40);
            static const unsigned steps[] = { 0x0100, 0x0200, 0x0400, 0x0600, 0x0800, 0x1000, 0x2000, 0x7FFF, 0xFC00, 0xF800, 0xE000, 0x0155, 0x0C00 };
            unsigned dsdx = rn(3) ? steps[rn(13)] : rn(65536), dtdy = rn(3) ? steps[rn(13)] : rn(65536);
            cmd(4, (rn(4) ? 0x24000000u : 0x25000000u) | x1 << 12 | y1, (uint32_t)tile << 24 | x0 << 12 | y0, rn(1 << 14) << 16 | rn(1 << 14), dsdx << 16 | dtdy);
            continue;
        }
        /* lod2 / frac2 */
        cmd(2, 0x2F000000u | modes_h(1, persp, detail, sharpen, is_lod2, bil), lo);
        combiner2(fcc >= 0 ? fcc : (int)rn(4));
        rand_tri((fnotex && rn((unsigned)fnotex) == 0 ? 0u : 2u) | (rn(4) == 0 ? 4u : 0u), level, tile, persp);   /* now and then no texture coefficients */
    }
    cmd(2, 0x29000000u, 0);
    return 0;
}
