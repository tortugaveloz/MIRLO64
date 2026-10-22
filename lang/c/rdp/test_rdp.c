/* Host tests of the RDP reference model: reciprocal accuracy, then a small
 * scene (clears, Gouraud + Z, a perspective-textured quad, a texrect)
 * rendered to test_rdp.ppm, with a few pixel-exact assertions.
 *   cc -O2 -o test_rdp test_rdp.c rdp.c && ./test_rdp */
#include "rdp.h"
#include "rdp_setup.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_BASE 0x40000000u
#define MEM_SIZE (32u << 20)
static uint16_t *g_mem;

static uint16_t rd16(void *c, uint32_t a) { (void)c; return g_mem[(a - MEM_BASE) >> 1]; }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; g_mem[(a - MEM_BASE) >> 1] = v; }

#define W 268
#define H 240
#define FB   0x40C00000u
#define ZB   0x40D01000u
#define TEX  0x41400000u

static int g_fail;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void push(rdp_t *r, uint32_t w) { rdp_push(r, w); }
static void cmd(rdp_t *r, uint32_t hi, uint32_t lo) { push(r, hi); push(r, lo); }
static void set_other(rdp_t *r, uint32_t h, uint32_t l) { cmd(r, (RDP_OP_SET_OTHER << 24) | h, l); }

static uint16_t px(int x, int y) { return g_mem[((FB - MEM_BASE) >> 1) + y * W + x]; }

/* N64 combiner words for one cycle, duplicated into both cycles */
static void set_combine(rdp_t *r, unsigned a, unsigned b, unsigned c, unsigned d,
                        unsigned Aa, unsigned Ab, unsigned Ac, unsigned Ad)
{
    uint32_t hi = (a << 20) | (c << 15) | (Aa << 12) | (Ac << 9) | (a << 5) | c;
    uint32_t lo = (b << 28) | (b << 24) | (Aa << 21) | (Ac << 18) | (d << 15) | (Ab << 12) | (Ad << 9) | (d << 6) | (Ab << 3) | Ad;
    cmd(r, (RDP_OP_SET_COMBINE << 24) | hi, lo);
}

static void emit_tri(rdp_t *r, rdp_vtx_t *a, rdp_vtx_t *b, rdp_vtx_t *c, unsigned flags)
{
    uint32_t w[44];
    int n = rdp_setup_triangle(w, a, b, c, flags, 0);
    for (int i = 0; i < n; i++) push(r, w[i]);
}

static rdp_vtx_t V(double x, double y, int R, int G, int B, double z)
{
    rdp_vtx_t v;
    memset(&v, 0, sizeof v);
    v.x = (int32_t)lrint(x * 65536); v.y = (int32_t)lrint(y * 65536);
    v.a[0] = R << 16; v.a[1] = G << 16; v.a[2] = B << 16; v.a[3] = 255 << 16;
    v.a[7] = (int32_t)lrint(z * 65535 * 32768);
    return v;
}

int main(void)
{
    /* 1. reciprocal */
    double worst = 0;
    for (uint32_t w = 0x100; w < 0x7FFFFFFF; w += w / 997 + 1) {
        int sh;
        uint32_t r = rdp_rcp(w, &sh);
        double exact = 65536.0 / ((double)(w << sh) / 1073741824.0);
        double e = fabs(r - exact) / exact;
        if (e > worst) worst = e;
    }
    printf("rcp worst relative error %.2e (%.1f bits)\n", worst, -log2(worst));
    CHECK(worst < 1.0 / 8192, "reciprocal less than 13 bits");
    CHECK(rdp_persp(100 << 16, 0x10000) == 100 * 32, "persp at W=1");
    CHECK(rdp_persp(50 << 16, 0x8000) == 100 * 32, "persp at W=0.5: %d", rdp_persp(50 << 16, 0x8000));

    g_mem = calloc(MEM_SIZE / 2, 2);
    rdp_t rd;
    rdp_init(&rd, NULL, rd16, wr16);
    rdp_t *r = &rd;

    /* 2. frame setup and clears (fill mode, as the N64 does) */
    cmd(r, (RDP_OP_SET_SCISSOR << 24), ((W << 2) << 12) | (H << 2));
    cmd(r, (RDP_OP_SET_ZIMG << 24), ZB);
    set_other(r, 3u << RDP_H_CYCLE_SHIFT, 0);
    cmd(r, (RDP_OP_SET_CIMG << 24) | (W - 1), ZB);
    cmd(r, RDP_OP_SET_FILL << 24, 0xFFFFFFFF);
    cmd(r, (RDP_OP_FILL_RECT << 24) | (((W - 1) << 2) << 12) | ((H - 1) << 2), 0);
    cmd(r, (RDP_OP_SET_CIMG << 24) | (W - 1), FB);
    cmd(r, RDP_OP_SET_FILL << 24, 0x21042104);       /* dark grey */
    cmd(r, (RDP_OP_FILL_RECT << 24) | (((W - 1) << 2) << 12) | ((H - 1) << 2), 0);
    CHECK(px(0, 0) == 0x2104 && px(W - 1, H - 1) == 0x2104, "fill");

    /* 3. two Gouraud triangles interpenetrating, Z compare + update */
    set_other(r, 0, RDP_L_Z_CMP | RDP_L_Z_UPD);
    set_combine(r, 15, 15, 31, 4, 7, 7, 7, 4);        /* shade */
    rdp_vtx_t a = V(20, 20, 255, 0, 0, 0.2), b = V(200, 40, 0, 255, 0, 0.8), c = V(60, 200, 0, 0, 255, 0.2);
    emit_tri(r, &a, &b, &c, RDP_SETUP_SHADE | RDP_SETUP_Z);
    rdp_vtx_t d = V(30, 180, 255, 255, 0, 0.5), e = V(240, 30, 255, 255, 0, 0.5), f = V(250, 200, 255, 255, 0, 0.5);
    emit_tri(r, &d, &e, &f, RDP_SETUP_SHADE | RDP_SETUP_Z);

    /* 4. a checkerboard texture, loaded into TMEM, on a perspective quad */
    for (int t = 0; t < 32; t++)
        for (int s = 0; s < 32; s++) {
            int on = ((s >> 2) ^ (t >> 2)) & 1;
            uint16_t v = on ? (uint16_t)(31 << 11 | 31 << 6 | 31 << 1 | 1) : (uint16_t)(8 << 11 | 1);
            g_mem[((TEX - MEM_BASE) >> 1) + t * 32 + s] = v;
        }
    cmd(r, (RDP_OP_SET_TIMG << 24) | (2u << 19) | 31, TEX);
    cmd(r, (RDP_OP_SET_TILE << 24) | (8 << 9) | 0, (7u << 24));                      /* load tile */
    cmd(r, (RDP_OP_LOAD_TILE << 24), (7u << 24) | ((31 << 2) << 12) | (31 << 2));
    cmd(r, (RDP_OP_SET_TILE << 24) | (8 << 9) | 0, (0u << 24) | (5 << 14) | (5 << 4));   /* wrap 32 */
    cmd(r, (RDP_OP_SET_TILESIZE << 24), (0u << 24) | ((31 << 2) << 12) | (31 << 2));
    set_other(r, RDP_H_PERSP | (2u << RDP_H_FILT_SHIFT), RDP_L_Z_CMP | RDP_L_Z_UPD);
    set_combine(r, 15, 15, 31, 1, 7, 7, 7, 1);       /* texel */
    /* a real projected floor: y = -1, x in [-2, 2], z in [1.5, 8]; camera at
     * the origin looking down +z, focal 120 px. W = zmin / z (max 1). */
    double X[4] = { -2, 2, 2, -2 }, Z[4] = { 8, 8, 1.5, 1.5 };
    double ss[4] = { 0, 128, 128, 0 }, ts[4] = { 0, 0, 128, 128 };
    rdp_vtx_t q[4];
    for (int i = 0; i < 4; i++) {
        double sx = 190 + 120 * X[i] / Z[i], sy = 110 + 120 * 1.0 / Z[i];
        q[i] = V(sx, sy, 0, 0, 0, 0.1);
        double iw = 1.5 / Z[i];
        q[i].a[4] = (int32_t)lrint(ss[i] * iw * 65536);
        q[i].a[5] = (int32_t)lrint(ts[i] * iw * 65536);
        q[i].a[6] = (int32_t)lrint(iw * 65536);
    }
    emit_tri(r, &q[0], &q[1], &q[2], RDP_SETUP_TEX | RDP_SETUP_Z);
    emit_tri(r, &q[0], &q[2], &q[3], RDP_SETUP_TEX | RDP_SETUP_Z);

    /* 5. a texrect (1-cycle, point sampled), no Z */
    set_other(r, 0, 0);
    cmd(r, (RDP_OP_TEXRECT << 24) | ((60 << 2) << 12) | (230 << 2), (0u << 24) | ((10 << 2) << 12) | (198 << 2));
    cmd(r, 0, (1u << 10) << 16 | (1u << 10));         /* S=T=0, DsDx=DtDy=1.0 */
    push(r, RDP_OP_SYNC_FULL << 24); push(r, 0);

    CHECK(r->sync_count == 1, "sync");
    CHECK(r->unknown_ops == 0, "unknown ops %u", r->unknown_ops);
    uint16_t p1 = px(25, 25);
    CHECK((p1 >> 11) > 25 && ((p1 >> 5) & 63) < 10, "red corner %04x", p1);
    CHECK(px(230, 100) == 0xFFE0, "yellow %04x", px(230, 100));
    /* texrect: 1:1 checkerboard, 4x4 squares, first square dark */
    CHECK(px(10, 198) == px(13, 201) && px(10, 198) != px(14, 198) && px(10, 198) != px(10, 202), "texrect");
    CHECK(px(10, 198) == ((8 << 3) >> 3) << 11, "texrect texel %04x", px(10, 198));

    FILE *fp = fopen("test_rdp.ppm", "wb");
    fprintf(fp, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t v = px(x, y);
            unsigned char c3[3] = { (unsigned char)((v >> 11) << 3), (unsigned char)(((v >> 5) & 63) << 2), (unsigned char)((v & 31) << 3) };
            fwrite(c3, 1, 3, fp);
        }
    fclose(fp);
    printf("pixels drawn %llu, %s\n", (unsigned long long)r->pixels_drawn, g_fail ? "FAILED" : "ALL PASS");
    return g_fail != 0;
}
