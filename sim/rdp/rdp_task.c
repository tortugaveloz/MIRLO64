/* The software pipeline of a graphics task: the RSP reference model
 * (sim/rsp) runs the game's microcode on a task dump, the RDP commands it
 * gives go to MRDP's reference model (lang/c/mrdp), which draws into the
 * dump's RDRAM; the colour image comes out as a PPM. For MRDP's work on taking
 * the N64's command stream: what a frame looks like, in seconds.
 *
 *   rdp_task [-o out_prefix] [-n] task_gfx_*.bin ...
 *     -n  no drawing: only count the commands by opcode
 *     -v  the commands, as they come
 *   RDP_WATCH=<hex address>: the commands that write that word of RDRAM
 *   RDP_RAW=1: and the colour image as it is (prefix_N.raw)
 * It counts the pixels that took the texture LOD (docs/lod.md) by level.
 * The PPM is of the last colour image at its own size: a 32-bit one's
 * (docs/cimg32.md) R, G, B bytes as they are.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsp.h"
#include "mrdp.h"

#define RDRAM_SIZE (8u << 20)
#define MEM_SIZE (16u << 20)                 /* and the hidden plane at 8 MiB (mrdp.h, MRDP_HID_BASE) */
static uint8_t ram[MEM_SIZE];
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* MRDP's memory: the N64's RDRAM, 16-bit big-endian */
static uint16_t rd16(void *c, uint32_t a) { (void)c; a &= MEM_SIZE - 2; return (uint16_t)(ram[a] << 8 | ram[a + 1]); }
static mrdp_t rdp;
static uint32_t watch = ~0u, cur_w[8], cur_n;
static unsigned long cur_i;
static void wr16(void *c, uint32_t a, uint16_t v)
{
    (void)c; a &= MEM_SIZE - 2;
    if (a == watch) {
        printf("  %06x <- %04x by command %lu:", a, v, cur_i);
        for (uint32_t i = 0; i < cur_n && i < 8; i++) printf(" %08x", cur_w[i]);
        printf("\n");
        {
            unsigned op = (cur_w[0] >> 24) & 63u;
            int ti = (op == 0x24 || op == 0x25) ? (int)((cur_w[1] >> 24) & 7) : (int)((cur_w[0] >> 16) & 7);
            const mrdp_tile_t *t = &rdp.tile[ti];
            printf("      other modes %08x %08x, combine %08x %08x; tile %d: fmt %u size %u line %u tmem %u palette %u, s %u..%u t %u..%u (10.2), "
                   "mask %u/%u shift %u/%u clamp %u/%u mirror %u/%u; texture image %06x width %u\n",
                   rdp.other_h, rdp.other_l, rdp.combine_hi, rdp.combine_lo, ti, t->fmt, t->size, t->line, t->tmem, t->palette,
                   t->uls, t->lrs, t->ult, t->lrt, t->masks, t->maskt, t->shifts, t->shiftt, t->cs, t->ct, t->ms, t->mt, rdp.timg, rdp.timg_width);
        }
    }
    ram[a] = (uint8_t)(v >> 8); ram[a + 1] = (uint8_t)v;
}

static int draw = 1, verbose;
static uint64_t ops[64];
static uint32_t last_cimg, last_width, last_size;
static FILE *trace;
static void on_rdp(void *user, const uint32_t *w, int n)
{
    (void)user;
    unsigned op = (w[0] >> 24) & 63u;
    ops[op]++;
    if (verbose) {
        printf("  %02x:", op);
        for (int i = 0; i < 2 * n && i < 8; i++) printf(" %08x", w[i]);
        printf("\n");
    }
    if (!draw) return;
    cur_i++; cur_n = (uint32_t)(2 * n);
    for (int i = 0; i < 2 * n && i < 8; i++) cur_w[i] = w[i];
    uint64_t px0 = rdp.pixels_drawn;
    /* RDP_RANGE=a-b[,c-d..]: only the draws (triangles, rectangles) whose command index (the trace's line) is in a range; the state commands all run */
    static int range_init, nr; static long ra[32], rb[32];
    if (!range_init) { range_init = 1; const char *e = getenv("RDP_RANGE"); while (e && *e && nr < 32 && sscanf(e, "%ld-%ld", &ra[nr], &rb[nr]) == 2) { nr++; e = strchr(e, ','); if (e) e++; } }
    int is_draw = (op >= 8 && op <= 15) || op == 0x24 || op == 0x25 || op == 0x36;
    int skip = 0;
    if (nr && is_draw && op != 0x36) { skip = 1; for (int q = 0; q < nr; q++) if ((long)cur_i - 1 >= ra[q] && (long)cur_i - 1 <= rb[q]) skip = 0; }
    if (getenv("RDP_PEEKLOAD") && op == 0x34 && cur_i < 140) {
        uint32_t uls = (w[0] >> 14) & 0x3FF, ult = (w[0] >> 2) & 0x3FF;
        uint32_t a = rdp.timg + ((ult * rdp.timg_width + uls) << rdp.timg_size >> 1);
        printf("  loadtile #%lu: timg %06x width %u size %u -> src %06x:", cur_i - 1, rdp.timg, rdp.timg_width, rdp.timg_size, a);
        for (int i = 0; i < 6; i++) printf(" %04x", rd16(0, a + 2 * i));
        printf("\n");
    }
    if (!skip) for (int i = 0; i < 2 * n; i++) mrdp_push(&rdp, w[i]);
    else { /* a draw skipped: the state it would have set (the primitive's tile) is not needed */ }
    if (trace) {   /* RDP_TRACE=<file>: every command, all its words, and the pixels it drew */
        fprintf(trace, "%02x %d", op, 2 * n);
        for (int i = 0; i < 2 * n; i++) fprintf(trace, " %08x", w[i]);
        fprintf(trace, " px=%llu\n", (unsigned long long)(rdp.pixels_drawn - px0));
    }
    if (op == MRDP_OP_SET_CIMG && rdp.cimg != rdp.zimg) { last_cimg = rdp.cimg; last_width = rdp.cimg_width; last_size = rdp.cimg_size; }
}

int main(int argc, char **argv)
{
    const char *prefix = "frame";
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-o") && ai + 1 < argc) prefix = argv[++ai];
        else if (!strcmp(argv[ai], "-n")) draw = 0;
        else if (!strcmp(argv[ai], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: rdp_task [-o out_prefix] [-n] [-v] task.bin ...\n"); return 2; }
    }
    static rsp_t s;
    if (getenv("RDP_WATCH")) watch = (uint32_t)strtoul(getenv("RDP_WATCH"), 0, 16) & (MEM_SIZE - 2);
    for (int k = 0; ai < argc; ai++, k++) {
        cur_i = 0;
        if (getenv("RDP_TRACE")) { if (trace) fclose(trace); char tp[1024]; snprintf(tp, sizeof tp, "%s.%d", getenv("RDP_TRACE"), k); trace = fopen(tp, "w"); }
        FILE *f = fopen(argv[ai], "rb");
        uint8_t hdr[16];
        if (!f || fread(hdr, 1, 16, f) != 16 || memcmp(hdr, "RSPT", 4) || fread(ram, 1, RDRAM_SIZE, f) != RDRAM_SIZE) {
            fprintf(stderr, "%s: not a task dump\n", argv[ai]); return 1;
        }
        fclose(f);
        uint32_t ta = be32(hdr + 4) & (RDRAM_SIZE - 1u);
        rsp_reset(&s, ram, RDRAM_SIZE);
        memset(ram + RDRAM_SIZE, 0xFF, MEM_SIZE - RDRAM_SIZE);      /* (the plane: all ones, as the runtime sets it) */
        s.rdp_cmd = on_rdp;
        mrdp_init(&rdp, 0, rd16, wr16);
        memset(ops, 0, sizeof ops);
        last_cimg = 0; last_width = 320; last_size = 2;
        uint8_t task[64];
        if (getenv("RDP_CLEARFB")) { /* RDP_CLEARFB=addr[,addr]: the colour buffers start black (not the last frame's) */
            const char *e = getenv("RDP_CLEARFB");
            while (*e) { uint32_t a = (uint32_t)strtoul(e, (char **)&e, 16); memset(ram + a, 0, 0x25800); if (*e == ',') e++; }
        }
        memcpy(task, ram + ta, 64);
        static const int ptrs[] = { 2, 4, 6, 8, 10, 11, 12, 14 };
        for (unsigned i = 0; i < sizeof ptrs / sizeof ptrs[0]; i++) wbe32(task + 4 * ptrs[i], be32(task + 4 * ptrs[i]) & 0x1FFFFFFFu);
        memcpy(s.dmem + 0xFC0, task, 64);
        uint32_t boot = be32(task + 8) & (RDRAM_SIZE - 1u), bs = be32(task + 12);
        memcpy(s.imem, ram + boot, bs > 4096 ? 4096 : bs);
        int r = rsp_run(&s, 200000000ull);
        printf("%s: %s, %llu RDP commands, %llu pixels, %u unknown;", argv[ai], r ? "DID NOT END" : "ended",
               (unsigned long long)s.rdp_cmds, (unsigned long long)rdp.pixels_drawn, rdp.unknown_ops);
        for (int i = 0; i < 64; i++) if (ops[i]) printf(" %02x:%llu", i, (unsigned long long)ops[i]);
        printf("\n");
        /* what of the frame took the texture LOD (docs/lod.md) */
        printf("  LOD: %llu pixels (2-cycle, TEX_LOD), %llu magnified, %llu at the far level; by level:", (unsigned long long)rdp.lod_px,
               (unsigned long long)rdp.lod_mag, (unsigned long long)rdp.lod_far);
        for (int i = 0; i < 8; i++) printf(" %llu", (unsigned long long)rdp.lod_lvl[i]);
        printf("\n");
        if (draw && last_cimg) {
            char path[1024];
            snprintf(path, sizeof path, "%s_%d.ppm", prefix, k);
            if ((f = fopen(path, "wb"))) {
                fprintf(f, "P6\n%u 240\n255\n", last_width);
                for (uint32_t i = 0; i < last_width * 240u; i++) {
                    if (last_size == 3) {           /* RGBA8888: R, G, B as they are (the alpha byte is the coverage) */
                        const uint8_t *p = ram + ((last_cimg + 4 * i) & (MEM_SIZE - 4));
                        fwrite(p, 1, 3, f);
                        continue;
                    }
                    uint16_t v = rd16(0, last_cimg + 2 * i);
                    unsigned r5 = v >> 11, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
                    uint8_t px[3] = { (uint8_t)(r5 << 3 | r5 >> 2), (uint8_t)(g5 << 3 | g5 >> 2), (uint8_t)(b5 << 3 | b5 >> 2) };
                    fwrite(px, 1, 3, f);
                }
                fclose(f);
            }
            /* RDP_RAW=1: the colour image as it is too (RGBA5551 big-endian, bit 0
             * the coverage; a 32-bit image's RGBA8888, the coverage in the alpha byte's
             * top bits), for a model of the VI's filters (sim/vi_filter/vi_model.py) */
            snprintf(path, sizeof path, "%s_%d.raw", prefix, k);
            if (getenv("RDP_RAW") && (f = fopen(path, "wb"))) {
                fwrite(ram + (last_cimg & (RDRAM_SIZE - 1)), 1, last_width * 240u * (last_size == 3 ? 4u : 2u), f);
                fclose(f);
            }
            /* and its pixels' hidden coverage bits, a byte each (prefix_N.hid; a 32-bit image has none) */
            snprintf(path, sizeof path, "%s_%d.hid", prefix, k);
            if (getenv("RDP_RAW") && last_size != 3 && (f = fopen(path, "wb"))) {
                for (uint32_t i = 0; i < last_width * 240u; i++) {
                    uint32_t a = (last_cimg + 2 * i) & (RDRAM_SIZE - 2), kk = (a >> 1) & 15u;
                    uint32_t ha = MRDP_HID_BASE + ((a >> 5) << 2) + (kk < 8u ? 2u : 0u);
                    uint8_t h = (uint8_t)((rd16(0, ha) >> (2u * (kk & 7u))) & 3u);
                    fwrite(&h, 1, 1, f);
                }
                fclose(f);
            }
        }
    }
    return 0;
}
