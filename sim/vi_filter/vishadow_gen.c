/* The RDP's commands for lang/mips/test/vishadow_test.c, and the picture the scan-out must show, from the model (lang/c/rdp).
 * A 16-bit colour image 320 x 240: a fill (the background), and four bands of shaded triangles, a ramp from 0 to 255 across 256 pixels in
 * red, green, blue and grey -- 256 levels of 8 bits where the image's 5551 has 32.
 *   vishadow_gen <mode> <out.ppm> [cmds.h]
 *   mode 0: the setting off -- the model dithers as the game asks (RGB_DITHER_SEL 0) and the picture is each channel's 5 bits << 3
 *   mode 1: "32-bit" on -- the model's shadow plane (no dither); the picture takes each pixel's low three bits from its shadow halfword where
 *           the check is the word's, else the channel's top three bits again (what rtl/soc/n64_video.sv does)
 * The picture is written as the pads send it (640 x 240: each pixel twice), the VI with its filters off (VI_STATUS 0x302). cmds.h: the command
 * words as a C array (lang/mips/test/vishadow_cmds.h).
 *   cc -O2 -DRDP_N64 -DRDP_X_RECTLOD -DRDP_X_COPY -DRDP_X_SHADE -DRDP_X_SPAN -DRDP_X_MEMDZ -I../../lang/c/rdp -o vishadow_gen vishadow_gen.c ../../lang/c/rdp/rdp.c -lm
 * (the rules of docs/rdp_exact.md that rtl/rdp follows, as sim/rdp/build_rtl.sh builds the model with) */
#include "rdp.h"
#include "rdp_setup.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 320
#define H 240
#define FB 0x100000u
#define MEM (32u << 20)
static uint16_t *g_mem;
static uint16_t rd16(void *c, uint32_t a) { (void)c; return g_mem[(a & (MEM - 2)) >> 1]; }
static void wr16(void *c, uint32_t a, uint16_t v) { (void)c; g_mem[(a & (MEM - 2)) >> 1] = v; }
static uint32_t cl[512]; static int ncl;
static rdp_t rd;
static void put(uint32_t w) { cl[ncl++] = w; rdp_push(&rd, w); }
static void cmd(uint32_t hi, uint32_t lo) { put(hi); put(lo); }
static void set_combine(unsigned a, unsigned b, unsigned c, unsigned d, unsigned Aa, unsigned Ab, unsigned Ac, unsigned Ad)
{
    uint32_t hi = (a << 20) | (c << 15) | (Aa << 12) | (Ac << 9) | (a << 5) | c;
    uint32_t lo = (b << 28) | (b << 24) | (Aa << 21) | (Ac << 18) | (d << 15) | (Ab << 12) | (Ad << 9) | (d << 6) | (Ab << 3) | Ad;
    cmd((RDP_OP_SET_COMBINE << 24) | hi, lo);
}
static rdp_vtx_t V(double x, double y, double R, double G, double B)
{
    rdp_vtx_t v; memset(&v, 0, sizeof v);
    v.x = (int32_t)lrint(x * 65536); v.y = (int32_t)lrint(y * 65536);
    v.a[0] = (int32_t)lrint(R * 65536); v.a[1] = (int32_t)lrint(G * 65536); v.a[2] = (int32_t)lrint(B * 65536); v.a[3] = 255 << 16;
    return v;
}
static void tri(rdp_vtx_t *a, rdp_vtx_t *b, rdp_vtx_t *c)
{
    uint32_t w[44];
    int n = rdp_setup_triangle(w, a, b, c, RDP_SETUP_SHADE, 0);
    for (int i = 0; i < n; i++) put(w[i]);
}
/* a band: the ramp 0..255 from x = 32 to 288, rows y0..y1, the channels in the weights r, g, b */
static void band(double y0, double y1, double r, double g, double b)
{
    rdp_vtx_t a = V(32, y0, 0, 0, 0), bb = V(288, y0, 255 * r, 255 * g, 255 * b), c = V(32, y1, 0, 0, 0), d = V(288, y1, 255 * r, 255 * g, 255 * b);
    tri(&a, &bb, &c); tri(&bb, &d, &c);
}
static unsigned crc3(unsigned w) { unsigned c = 0; for (int i = 15; i >= 0; i--) { unsigned fb = ((c >> 2) & 1u) ^ ((w >> i) & 1u); c = (c << 1) & 7u; if (fb) c ^= 3u; } return c; }
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: vishadow_gen <0|1> <out.ppm> [cmds.h]\n"); return 2; }
    const int mode = atoi(argv[1]);
    g_mem = calloc(MEM / 2, 2);
    rdp_init(&rd, NULL, rd16, wr16);
    rd.shadow = mode ? 3 : 0;
    rd.rgb_dither_off = 0;
    cmd(RDP_OP_SET_SCISSOR << 24, ((W << 2) << 12) | (H << 2));
    cmd((RDP_OP_SET_CIMG << 24) | (W - 1) | (2u << 19), FB);
    cmd((RDP_OP_SET_OTHER << 24) | (3u << 20), 0);                          /* fill mode */
    cmd(RDP_OP_SET_FILL << 24, 0x21082108u);
    cmd((RDP_OP_FILL_RECT << 24) | ((W - 1) << 14) | ((H - 1) << 2), 0);
    cmd((RDP_OP_SET_OTHER << 24) | (0u << 6), 0);                           /* 1-cycle, no blender, RGB_DITHER_SEL 0 */
    set_combine(15, 15, 31, 4, 7, 7, 7, 4);                                  /* shade */
    band(8, 40, 1, 0, 0); band(48, 80, 0, 1, 0); band(88, 120, 0, 0, 1); band(128, 160, 1, 1, 1);
    cmd(RDP_OP_SYNC_FULL << 24, 0);
    /* the picture */
    FILE *f = fopen(argv[2], "wb");
    fprintf(f, "P6\n%d %d\n255\n", 2 * W, H);
    int levels[4] = {0, 0, 0, 0}; int seen[4][256]; memset(seen, 0, sizeof seen);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const uint32_t a = FB + 2u * (uint32_t)(y * W + x);
        const unsigned p = g_mem[a >> 1], sa = RDP_SH_BASE + (a >> 1), sw = (g_mem[sa >> 1] >> ((sa & 1u) ? 0 : 8)) & 0xFFu;       /* (a byte a pixel) */
        int ch[3] = { (int)(p >> 11) & 31, (int)(p >> 6) & 31, (int)(p >> 1) & 31 }, lo[3];
        const int ok = mode && (sw & 7u) == crc3(p);
        for (int k = 0; k < 3; k++) {       /* red and green keep two of the three low bits, blue one; the bit missing repeats the one above it */
            const int two = k == 0 ? (sw >> 6) & 3 : (sw >> 4) & 3;
            lo[k] = !mode ? 0 : ok ? (k == 2 ? ((sw >> 3) & 1 ? 7 : 0) : (two << 1 | (two & 1))) : ch[k] >> 2;
        }
        uint8_t px[3];
        for (int k = 0; k < 3; k++) { px[k] = (uint8_t)(ch[k] << 3 | lo[k]); }
        fwrite(px, 1, 3, f); fwrite(px, 1, 3, f);
        if (y == 24 && x >= 40 && x < 280) { if (!seen[0][px[0]]++) levels[0]++; }
        if (y == 64 && x >= 40 && x < 280) { if (!seen[1][px[1]]++) levels[1]++; }
        if (y == 104 && x >= 40 && x < 280) { if (!seen[2][px[2]]++) levels[2]++; }
        if (y == 144 && x >= 40 && x < 280) { if (!seen[3][px[0]]++) levels[3]++; }
    }
    fclose(f);
    if (getenv("VISHADOW_SH")) {            /* the model's main halfwords and shadow bytes of the picture, 2 x 320 x 240 */
        FILE *q = fopen(getenv("VISHADOW_SH"), "wb");
        for (int i = 0; i < W * H; i++) { uint16_t v[2] = { g_mem[(FB >> 1) + i], (uint16_t)((g_mem[(RDP_SH_BASE + FB / 2 + i) >> 1] >> (((RDP_SH_BASE + FB / 2 + i) & 1u) ? 0 : 8)) & 0xFFu) }; fwrite(v, 2, 2, q); }
        fclose(q);
    }
    printf("vishadow_gen: mode %d: distinct levels along the middle row of the red, green, blue and grey ramps (240 pixels): %d %d %d %d\n", mode, levels[0], levels[1], levels[2], levels[3]);
    if (argc > 3) {
        FILE *h = fopen(argv[3], "w");
        fprintf(h, "/* generated by sim/vi_filter/vishadow_gen.c: the RDP's commands for vishadow_test.c */\nstatic const unsigned vishadow_cmds[%d] = {", ncl);
        for (int i = 0; i < ncl; i++) fprintf(h, "%s0x%08xu,", i % 6 ? " " : "\n    ", cl[i]);
        fprintf(h, "\n};\n");
        fclose(h);
    }
    return 0;
}
