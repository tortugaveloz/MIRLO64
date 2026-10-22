/* Runs a GDL file through the host build of the geom pipeline and prints the
 * RDP command words it emits, one per line (check_host_vs_rtl.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "geom_gdl.h"
#include "geom_pipeline.h"
#include "geom_triangle.h"
void geom_test_texbind(int fmt, int siz, int cms, int cmt, int pal, int w, int h, const void *src, const void *tlut)
{ (void)fmt;(void)siz;(void)cms;(void)cmt;(void)pal;(void)w;(void)h;(void)src;(void)tlut; }
/* The command words, one per line, and (HOSTREPLAY_FB=<file>)
 * the framebuffer the RDP model draws from them, raw RGB565. The GDL sits
 * at mkgdl's load address inside a host SDRAM copy, so its texture sources
 * (SoC addresses) resolve. */
#include "../../../lang/c/rdp/rdp.h"
#include "../../../lang/c/rdp/rdp_frame.h"
#define HR_FB 0x40C00000u
#define HR_ZB 0x40D01000u
uint8_t *g_geom_host_sdram;
static rdp_t s_rdp;
static uint16_t hr_rd16(void *c, uint32_t a) { (void)c; return *(uint16_t *)(g_geom_host_sdram + (a - 0x40000000u)); }
static void hr_wr16(void *c, uint32_t a, uint16_t v) { (void)c; *(uint16_t *)(g_geom_host_sdram + (a - 0x40000000u)) = v; }
static int s_print = 1;
void geom_test_rdp(const uint32_t *w, unsigned n)
{
    for (unsigned i = 0; i < n; i++) { if (s_print) printf("%08x\n", w[i]); rdp_push(&s_rdp, w[i]); }
}
void geom_test_rdp_mem(uint32_t addr, uint32_t bytes) { (void)addr; (void)bytes; }
int main(int argc, char **argv) {
    g_geom_host_sdram = calloc(64u << 20, 1);
    rdp_init(&s_rdp, NULL, hr_rd16, hr_wr16);
    uint32_t *gdl = (uint32_t *)(g_geom_host_sdram + 0x01000000u);
    FILE *f = fopen(argv[1], "rb"); fread(gdl, 4, 1 << 20, f); fclose(f);
    {   /* the frame's clears: not part of the geom's stream (tb_geom_cpu pushes the same) */
        uint32_t w[RDP_FRAME_CLEAR_WORDS];
        s_print = 0;
        geom_test_rdp(w, rdp_frame_clear(w, HR_FB, HR_ZB, GEOM_FB_HRES, GEOM_FB_VRES, 0x404040u));
        s_print = 1;
    }
    geom_reset(); geom_run_display_list(gdl);
    if (getenv("HOSTREPLAY_FB")) {
        FILE *fb = fopen(getenv("HOSTREPLAY_FB"), "wb");
        fwrite(g_geom_host_sdram + (HR_FB - 0x40000000u), 2, GEOM_FB_HRES * GEOM_FB_VRES, fb); fclose(fb);
    }
    fprintf(stderr, "rdp: %llu pixels, %u loads, %u unknown ops\n",
            (unsigned long long)s_rdp.pixels_drawn, s_rdp.load_count, s_rdp.unknown_ops);
    if (getenv("HOSTREPLAY_DIAG")) {   /* the triangle funnel: in, near/w, far, offscreen/culled, emitted */
        extern long h_diag[8];
        fprintf(stderr, "diag in=%ld near=%ld far=%ld rej=%ld emitted=%ld\n", h_diag[0], h_diag[1], h_diag[2], h_diag[3], h_diag[4]);
    }
    return 0;
}
