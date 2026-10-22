/* A smooth Gouraud gradient through the model (lang/c/rdp, RDP_N64 -- the
 * contract rtl/rdp is checked against, sim/rdp tb_rdp), once per RGB_DITHER_SEL:
 * 3 (off: what the Pocket drew before), 0 (magic square: SM64's), 1 (Bayer).
 * Quad 256 x 24 px, R going 96 -> 128 (a wall's lit shade), G, B constant 80 /
 * 160. Prints, per mode, the runs of one flat 5-bit level along a row and the
 * worst error of the 4 x 4 mean against the exact 8-bit ramp, and writes
 * gradient_<sel>.ppm (the framebuffer's 5 bits, replicated to 8).
 *   cc -O2 -DRDP_N64 -I../../lang/c/rdp -o gradient_test gradient_test.c ../../lang/c/rdp/rdp.c -lm && ./gradient_test */
#include "rdp.h"
#include "rdp_setup.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM (8u << 20)
static uint16_t *g_mem;
static uint16_t rd16(void *c, uint32_t a) { (void)c; return g_mem[(a & (RAM - 2)) >> 1]; }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; g_mem[(a & (RAM - 2)) >> 1] = v; }
#define W 320
#define H 240
#define FB 0x100000u
static void cmd(rdp_t *r, uint32_t hi, uint32_t lo) { rdp_push(r, hi); rdp_push(r, lo); }
static void set_combine(rdp_t *r, unsigned a, unsigned b, unsigned c, unsigned d,
                        unsigned Aa, unsigned Ab, unsigned Ac, unsigned Ad)
{
    uint32_t hi = (a << 20) | (c << 15) | (Aa << 12) | (Ac << 9) | (a << 5) | c;
    uint32_t lo = (b << 28) | (b << 24) | (Aa << 21) | (Ac << 18) | (d << 15) | (Ab << 12) | (Ad << 9) | (d << 6) | (Ab << 3) | Ad;
    cmd(r, (RDP_OP_SET_COMBINE << 24) | hi, lo);
}
static rdp_vtx_t V(double x, double y, double R, double G, double B)
{
    rdp_vtx_t v; memset(&v, 0, sizeof v);
    v.x = (int32_t)lrint(x * 65536); v.y = (int32_t)lrint(y * 65536);
    v.a[0] = (int32_t)lrint(R * 65536); v.a[1] = (int32_t)lrint(G * 65536); v.a[2] = (int32_t)lrint(B * 65536); v.a[3] = 255 << 16;
    return v;
}
static void tri(rdp_t *r, rdp_vtx_t *a, rdp_vtx_t *b, rdp_vtx_t *c)
{
    uint32_t w[44];
    int n = rdp_setup_triangle(w, a, b, c, RDP_SETUP_SHADE, 0);
    for (int i = 0; i < n; i++) rdp_push(r, w[i]);
}
int main(void)
{
    int fail = 0;
    printf("%-18s %12s %14s %s\n", "RGB_DITHER_SEL", "runs/row", "worst 4x4 err", "(8-bit levels; R ramp 96..128 over 256 px)");
    for (int pass = 0; pass < 3; pass++) {
        unsigned sel = pass == 0 ? 3 : pass == 1 ? 0 : 1;
        g_mem = calloc(RAM / 2, 2);
        rdp_t rd; rdp_init(&rd, NULL, rd16, wr16);
        rdp_t *r = &rd;
        cmd(r, (RDP_OP_SET_SCISSOR << 24), ((W << 2) << 12) | (H << 2));
        cmd(r, (RDP_OP_SET_CIMG << 24) | (W - 1) | (2u << 19), FB);
        cmd(r, (RDP_OP_SET_OTHER << 24) | (sel << 6), 0);                 /* 1-cycle, no blender, RGB dither sel */
        set_combine(r, 15, 15, 31, 4, 7, 7, 7, 4);                           /* shade */
        rdp_vtx_t a = V(32, 8, 96, 80, 160), b = V(288, 8, 128, 80, 160), c = V(32, 32, 96, 80, 160), d = V(288, 32, 128, 80, 160);
        tri(r, &a, &b, &c); tri(r, &b, &d, &c);
        cmd(r, RDP_OP_SYNC_FULL << 24, 0);
        /* the rows 12..15: runs of equal R5 along x, and the 4 x 4 mean at 8 bits */
        int runs = 0, last = -1; double worst = 0;
        for (int x = 40; x < 280; x++) {
            int v = g_mem[(FB >> 1) + 13 * W + x] >> 11;
            if (v != last) { runs++; last = v; }
        }
        for (int x = 40; x < 276; x += 4) {
            double s = 0;
            for (int y = 12; y < 16; y++) for (int k = 0; k < 4; k++) s += (g_mem[(FB >> 1) + y * W + x + k] >> 11) * 8.0;
            double ex = 96.0 + (x + 1.5 - 32.0) * 32.0 / 256.0;           /* the ramp at the block's centre */
            double e = fabs(s / 16.0 - ex);
            if (e > worst) worst = e;
        }
        printf("%-18s %12d %14.2f\n", sel == 3 ? "3 off" : sel == 0 ? "0 magic square" : "1 Bayer", runs, worst);
        if (pass == 0 && worst < 2.0) fail++;      /* undithered must band */
        if (pass > 0 && worst > 1.5) fail++;       /* dithered must track the ramp */
        char name[64]; snprintf(name, sizeof name, "gradient_%u.ppm", sel);
        FILE *f = fopen(name, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, 40);
        for (int y = 0; y < 40; y++) for (int x = 0; x < W; x++) {
            uint16_t p = g_mem[(FB >> 1) + y * W + x]; unsigned R = p >> 11, G = (p >> 6) & 31, B = (p >> 1) & 31;
            fputc(R << 3 | R >> 2, f); fputc(G << 3 | G >> 2, f); fputc(B << 3 | B >> 2, f);
        }
        fclose(f); free(g_mem);
    }
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
