/* rdpo: the RDP reference model (lang/c/mrdp, built with MRDP_N64; RDP_EXACT
 * for rdpo_exact) against an independent pixel-exact software RDP (the oracle,
 * oracle_glue.c), on the same command words and the same initial RDRAM.
 *
 *   rdpo [options] stream.rdps ...
 *
 * A stream (.rdps, text, see README.md) sets up RDRAM and lists the commands.
 * Both renderers take every command; after each primitive the colour image,
 * its hidden coverage bits and the depth image are compared, and (by default)
 * the model's images are then made the oracle's again, so that each primitive
 * is judged on its own: a difference is the command that made it.
 *
 * Every pixel's LOD is compared too (hooks on both sides: the
 * model's copy is patched at build time, the oracle's by oracle_hooks.patch):
 * the S, T of the three points each side took, and what each made of them. And
 * every LOD the oracle computes is put through the model's mrdp_lod with the
 * oracle's own inputs (the "unit" check: the LOD logic alone).
 *
 * Options:
 *   -q            one line a stream
 *   -v            more examples
 *   -noresync     keep the model's own images from one command to the next
 *   -min FILE     write the first differing primitive as a stream of its own
 *                 (every state command before it, and it): then run it again
 *   -at N        with -min: command N instead of the first that differs
 *   -ppm PREFIX   the final colour images, PREFIX_model.ppm, PREFIX_oracle.ppm
 *                 and PREFIX_diff.ppm
 *   -nodepth      do not count depth differences (still reported once)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mrdp.h"

/* oracle_glue.c */
void orc_init(uint8_t *rdram);
void orc_cmd(const uint32_t *w, int n);
unsigned orc_hidden(uint32_t a);
void orc_set_hidden(uint32_t a, unsigned v);

#define RSIZE (8u << 20)
#define MSIZE (16u << 20)            /* the model's: the hidden plane at 8 MiB (mrdp.h) */
static uint8_t *mmem;                /* the model's memory, big-endian bytes */
static uint8_t *omem;                /* the oracle's, 32-bit words in host order */
static mrdp_t mdl;

static uint16_t mdl_rd(void *c, uint32_t a) { (void)c; a &= MSIZE - 2; return (uint16_t)(mmem[a] << 8 | mmem[a + 1]); }
static void mdl_wr(void *c, uint32_t a, uint16_t v) { (void)c; a &= MSIZE - 2; mmem[a] = (uint8_t)(v >> 8); mmem[a + 1] = (uint8_t)v; }
static inline uint8_t ob(uint32_t a) { return omem[(a & (RSIZE - 1)) ^ 3u]; }
static inline void obw(uint32_t a, uint8_t v) { omem[(a & (RSIZE - 1)) ^ 3u] = v; }
static inline uint16_t o16(uint32_t a) { return (uint16_t)(ob(a) << 8 | ob(a + 1)); }
static inline uint16_t m16(uint32_t a) { return (uint16_t)(mmem[a & (RSIZE - 2)] << 8 | mmem[(a & (RSIZE - 2)) + 1]); }

/* the model's hidden plane (mrdp.c hid_get / hid_set, MRDP_HID_BASE) */
static unsigned m_hid(uint32_t a)
{
    unsigned k = (a >> 1) & 15u;
    uint32_t ha = MRDP_HID_BASE + (((a & 0x7FFFFFu) >> 5) << 2) + (k < 8u ? 2u : 0u);
    return (mdl_rd(0, ha) >> (2u * (k & 7u))) & 3u;
}
static void m_hid_set(uint32_t a, unsigned v)
{
    unsigned k = (a >> 1) & 15u;
    uint32_t ha = MRDP_HID_BASE + (((a & 0x7FFFFFu) >> 5) << 2) + (k < 8u ? 2u : 0u);
    uint16_t h = mdl_rd(0, ha);
    h = (uint16_t)((h & ~(3u << (2u * (k & 7u)))) | ((v & 3u) << (2u * (k & 7u))));
    mdl_wr(0, ha, h);
}

/* ---- the stream -------------------------------------------------------------------------------------------- */
typedef struct { uint32_t w[44]; int n; int line; } cmd_t;
static cmd_t *cmds; static int ncmds, capcmds;
typedef struct { int kind; uint32_t a, n, seed; uint8_t *bytes; uint8_t v; } init_t;   /* 0 random, 1 zero, 2 mem, 3 fill */
static init_t inits[4096]; static int ninits;

static unsigned xs32(unsigned *s) { unsigned x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x; }

static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int load_stream(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    ncmds = 0; ninits = 0;
    static char line[1 << 20];
    int ln = 0;
    static char fpath[1024];
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        char kw[16]; int used = 0;
        if (sscanf(p, "%15s%n", kw, &used) != 1) continue;
        p += used;
        if (!strcmp(kw, "cmd")) {
            if (ncmds == capcmds) { capcmds = capcmds ? 2 * capcmds : 1024; cmds = realloc(cmds, (size_t)capcmds * sizeof *cmds); }
            cmd_t *c = &cmds[ncmds++]; c->n = 0; c->line = ln;
            char *e;
            for (;;) { unsigned long v = strtoul(p, &e, 16); if (e == p) break; if (c->n < 44) c->w[c->n++] = (uint32_t)v; p = e; }
        } else if (!strcmp(kw, "rdram") && sscanf(p, " file %1023s", fpath) == 1) {
            unsigned long off = 0; char *q = strstr(p, fpath) + strlen(fpath); off = strtoul(q, 0, 0);
            FILE *rf = fopen(fpath, "rb");
            uint8_t *b = malloc(RSIZE);
            if (!rf || fseek(rf, (long)off, SEEK_SET) || fread(b, 1, RSIZE, rf) != RSIZE) { fprintf(stderr, "%s:%d: cannot read 8 MiB of %s at %lu\n", path, ln, fpath, off); if (rf) fclose(rf); fclose(f); return -1; }
            fclose(rf);
            inits[ninits].kind = 2; inits[ninits].a = 0; inits[ninits].n = RSIZE; inits[ninits].bytes = b; ninits++;
        } else if (!strcmp(kw, "rdram")) {
            char what[16] = ""; unsigned seed = 1;
            sscanf(p, "%15s %u", what, &seed);
            inits[ninits].kind = !strcmp(what, "random") ? 0 : 1; inits[ninits].seed = seed ? seed : 1; ninits++;
        } else if (!strcmp(kw, "fill")) {
            unsigned a, n, v; sscanf(p, "%x %x %x", &a, &n, &v);
            inits[ninits].kind = 3; inits[ninits].a = a; inits[ninits].n = n; inits[ninits].v = (uint8_t)v; ninits++;
        } else if (!strcmp(kw, "mem")) {
            unsigned a; int u = 0; sscanf(p, "%x%n", &a, &u); p += u;
            while (*p == ' ') p++;
            size_t len = strlen(p);
            uint8_t *b = malloc(len / 2 + 1); uint32_t n = 0;
            for (; hexval(p[0]) >= 0 && hexval(p[1]) >= 0; p += 2) b[n++] = (uint8_t)(hexval(p[0]) << 4 | hexval(p[1]));
            inits[ninits].kind = 2; inits[ninits].a = a; inits[ninits].n = n; inits[ninits].bytes = b; ninits++;
        } else { fprintf(stderr, "%s:%d: unknown line '%s'\n", path, ln, kw); fclose(f); return -1; }
        if (ninits >= 4096) { fprintf(stderr, "%s: too many memory lines\n", path); fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

static void init_memory(void)
{
    memset(mmem, 0, MSIZE); memset(omem, 0, RSIZE);
    for (int i = 0; i < ninits; i++) {
        init_t *it = &inits[i];
        if (it->kind == 0) { unsigned s = it->seed * 2654435761u + 1; for (uint32_t a = 0; a < RSIZE; a += 4) { uint32_t v = xs32(&s); for (int b = 0; b < 4; b++) mmem[a + b] = (uint8_t)(v >> (24 - 8 * b)); } }
        else if (it->kind == 1) memset(mmem, 0, RSIZE);
        else if (it->kind == 2) for (uint32_t k = 0; k < it->n; k++) mmem[(it->a + k) & (RSIZE - 1)] = it->bytes[k];
        else for (uint32_t k = 0; k < it->n; k++) mmem[(it->a + k) & (RSIZE - 1)] = it->v;
    }
    for (uint32_t a = 0; a < RSIZE; a++) obw(a, mmem[a]);
    /* the hidden bits as the oracle starts them: a word never written has {b0, b0} (its bit 0 twice) */
    for (uint32_t a = 0; a < RSIZE; a += 2) m_hid_set(a, (mmem[a + 1] & 1u) ? 3u : 0u);
}

/* ---- the LOD hooks ----------------------------------------------------------------------------------------- */
#define GW 1024
#define GH 512
typedef struct { int gen; int s[3], t[3]; int clamp, lod, mag, dis, lf, t1, t2; } orec_t;
static orec_t (*orec)[GW];
static int cur_gen;
static uint32_t cur_h, cur_minl; static unsigned cur_maxl, cur_tile; static int cur_texrect;
static inline int sx17(int v) { v &= 0x1FFFF; return (v & 0x10000) ? v - 0x20000 : v; }

typedef struct { uint64_t n, bad, bad_frac, bad_tile, bad_lod, bad_mag; } ustat_t;
static ustat_t ust;
static int verbose;
static int unit_shown;
static void lod_print(const char *who, const int32_t *s, const int32_t *t, int oob, unsigned lod, unsigned mag, unsigned dis, unsigned lf, int t1, int t2)
{
    printf("      %-6s S %6d %6d %6d  T %6d %6d %6d  oob %d  lod %5u mag %u far %u frac %03x tiles %d %d\n",
           who, s[0], s[1], s[2], t[0], t[1], t[2], oob, lod, mag, dis, lf, t1, t2);
}
void orc_lod_hook(int kind, int x, int y, int s0, int t0, int s1, int t1, int s2, int t2,
                  int clamp, int lod, int mag, int dis, int lf, int tile1, int tile2)
{
    (void)kind;
    int32_t s[3] = { sx17(s0), sx17(s1), sx17(s2) }, t[3] = { sx17(t0), sx17(t1), sx17(t2) };
    if (x >= 0 && x < GW && y >= 0 && y < GH) {
        orec_t *o = &orec[y][x];
        o->gen = cur_gen; memcpy(o->s, s, sizeof s); memcpy(o->t, t, sizeof t);
        o->clamp = clamp; o->lod = lod; o->mag = mag; o->dis = dis; o->lf = lf; o->t1 = tile1; o->t2 = tile2;
    }
    /* the unit check: the oracle's inputs through the model's LOD logic */
    mrdp_lod_t L = mrdp_lod(cur_h, cur_minl, cur_maxl, cur_tile, s, t, clamp != 0);
    ust.n++;
    int bf = (int)L.frac != lf, bt = (int)L.tile0 != tile1 || (tile2 >= 0 && (int)L.tile1 != tile2);
    int bl = !clamp && (int)L.lod != lod, bm = (int)L.magnify != mag || (int)L.distant != dis;
    if (bf || bt || bl || bm) {
        ust.bad++; ust.bad_frac += bf; ust.bad_tile += bt; ust.bad_lod += bl; ust.bad_mag += bm;
        if (unit_shown++ < (verbose ? 20 : 3)) {
            printf("    UNIT logic differs at (%d,%d) other_h %06x min %u levels %u tile %u\n", x, y, cur_h, cur_minl, cur_maxl, cur_tile);
            lod_print("oracle", s, t, clamp, (unsigned)lod, (unsigned)mag, (unsigned)dis, (unsigned)lf, tile1, tile2);
            lod_print("model", s, t, clamp, L.lod, L.magnify, L.distant, L.frac, L.tile0, L.tile1);
        }
    }
}

/* the model's hooks (the build's patched copy of mrdp.c: lod_pixel's inputs, then draw_pixel_cvg's result) */
static int32_t mh_s[3], mh_t[3]; static int mh_oob;
void mdl_lod_inputs(const int32_t *s, const int32_t *t, int oob) { memcpy(mh_s, s, sizeof mh_s); memcpy(mh_t, t, sizeof mh_t); mh_oob = oob; }
typedef struct { uint64_t n, same, in_diff, in_diff_out_same, logic, no_orc; uint64_t in_s0, in_pt1, in_pt2, in_oob; uint64_t only1, only2, steps_same, hist[5]; int maxd; } pstat_t;
static pstat_t pst;
static int pix_shown, logic_shown;
void mdl_lod_hook(int x, int y, const mrdp_lod_t *L)
{
    pst.n++;
    if (x < 0 || x >= GW || y < 0 || y >= GH || orec[y][x].gen != cur_gen) { pst.no_orc++; return; }
    const orec_t *o = &orec[y][x];
    int oob = o->clamp != 0;
    int d0 = mh_s[0] != o->s[0] || mh_t[0] != o->t[0], d1 = mh_s[1] != o->s[1] || mh_t[1] != o->t[1], d2 = mh_s[2] != o->s[2] || mh_t[2] != o->t[2];
    int din = (mh_oob != oob) || (!oob && !mh_oob && (d0 || d1 || d2));
    int dout = (int)L->frac != o->lf || (int)L->tile0 != o->t1 || (o->t2 >= 0 && (int)L->tile1 != o->t2) || (int)L->magnify != o->mag || (int)L->distant != o->dis;
    if (!din && !dout) { pst.same++; return; }
    if (din) {
        pst.in_diff++; if (!dout) pst.in_diff_out_same++;
        if (mh_oob != oob) pst.in_oob++; else { pst.in_s0 += d0; pst.in_pt1 += d1; pst.in_pt2 += d2; }
        if (mh_oob == oob) {
            int ds = abs(mh_s[0] - o->s[0]), dt = abs(mh_t[0] - o->t[0]), dm = ds > dt ? ds : dt;
            pst.hist[dm == 0 ? 0 : dm == 1 ? 1 : dm < 4 ? 2 : dm < 16 ? 3 : 4]++;
            if (dm > pst.maxd) pst.maxd = dm;
            if (!d0 && d1) pst.only1++;
            if (!d0 && d2) pst.only2++;
            if (d0 && mh_s[1] - mh_s[0] == o->s[1] - o->s[0] && mh_t[1] - mh_t[0] == o->t[1] - o->t[0] && mh_s[2] - mh_s[0] == o->s[2] - o->s[0] && mh_t[2] - mh_t[0] == o->t[2] - o->t[0]) pst.steps_same++;
        }
        if ((dout && pix_shown++ < (verbose ? 12 : 2)) || (verbose && mh_oob != oob)) {
            printf("    LOD inputs differ at (%d,%d), and the result:\n", x, y);
            lod_print("oracle", o->s, o->t, oob, (unsigned)o->lod, (unsigned)o->mag, (unsigned)o->dis, (unsigned)o->lf, o->t1, o->t2);
            lod_print("model", mh_s, mh_t, mh_oob, L->lod, L->magnify, L->distant, L->frac, L->tile0, L->tile1);
        }
    } else {
        pst.logic++;
        if (logic_shown++ < (verbose ? 12 : 3)) {
            printf("    LOD logic differs at (%d,%d) (same inputs):\n", x, y);
            lod_print("oracle", o->s, o->t, oob, (unsigned)o->lod, (unsigned)o->mag, (unsigned)o->dis, (unsigned)o->lf, o->t1, o->t2);
            lod_print("model", mh_s, mh_t, mh_oob, L->lod, L->magnify, L->distant, L->frac, L->tile0, L->tile1);
        }
    }
}

/* ---- comparison -------------------------------------------------------------------------------------------- */
static uint32_t cimg, cwidth = 320, csize = 2, zimg; static int have_z;
static int is_prim(uint32_t w0) { unsigned op = (w0 >> 24) & 63; return (op >= 8 && op <= 15) || op == 0x24 || op == 0x25 || op == 0x36; }
static int img_h = 240;                 /* rows compared: the scissor's (SET_SCISSOR) */

typedef struct { uint64_t c, h, z; int fx, fy; uint32_t fa; int what; } diff_t;
static diff_t compare(int count_depth)
{
    diff_t d = { 0, 0, 0, -1, -1, 0, 0 };
    if (csize == 2 || csize == 1) {
        uint32_t bpp = csize;
        for (int y = 0; y < img_h; y++)
            for (uint32_t x = 0; x < cwidth; x++) {
                uint32_t a = cimg + (y * cwidth + x) * bpp;
                int dc = bpp == 2 ? m16(a) != o16(a) : mmem[a & (RSIZE - 1)] != ob(a);
                int dh = bpp == 2 && m_hid(a) != orc_hidden(a);
                if (dc) d.c++;
                if (dh) d.h++;
                if ((dc || dh) && d.fx < 0) { d.fx = (int)x; d.fy = y; d.fa = a; d.what = dc ? 0 : 1; }
            }
    }
    if (have_z && csize == 2 && zimg != cimg)
        for (int y = 0; y < img_h; y++)
            for (uint32_t x = 0; x < cwidth; x++) {
                uint32_t a = zimg + (y * cwidth + x) * 2;
                if (m16(a) != o16(a)) { d.z++; if (count_depth && d.fx < 0) { d.fx = (int)x; d.fy = y; d.fa = a; d.what = 2; } }
            }
    return d;
}
/* the model's images made the oracle's again */
static void resync(void)
{
    uint32_t bpp = csize == 1 ? 1 : 2;
    for (int y = 0; y < img_h; y++)
        for (uint32_t x = 0; x < cwidth; x++) {
            uint32_t a = cimg + (y * cwidth + x) * bpp;
            for (uint32_t b = 0; b < bpp; b++) mmem[(a + b) & (RSIZE - 1)] = ob(a + b);
            if (bpp == 2) m_hid_set(a, orc_hidden(a));
            if (have_z && bpp == 2) { uint32_t z = zimg + (y * cwidth + x) * 2; mmem[z & (RSIZE - 1)] = ob(z); mmem[(z + 1) & (RSIZE - 1)] = ob(z + 1); }
        }
}

static void write_ppm(const char *path, int oracle, int diff)
{
    FILE *o = fopen(path, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u %d\n255\n", cwidth, img_h);
    for (int y = 0; y < img_h; y++)
        for (uint32_t x = 0; x < cwidth; x++) {
            uint32_t a = cimg + (y * cwidth + x) * csize;
            unsigned char c[3];
            if (csize == 1) { unsigned v = oracle ? ob(a) : mmem[a]; c[0] = c[1] = c[2] = (unsigned char)v; }
            else {
                unsigned v = oracle ? o16(a) : m16(a), r5 = v >> 11, g5 = (v >> 6) & 31, b5 = (v >> 1) & 31;
                c[0] = (unsigned char)(r5 << 3 | r5 >> 2); c[1] = (unsigned char)(g5 << 3 | g5 >> 2); c[2] = (unsigned char)(b5 << 3 | b5 >> 2);
            }
            if (diff) {
                int dd = csize == 1 ? mmem[a] != ob(a) : m16(a) != o16(a);
                c[0] = dd ? 255 : c[0] / 4; c[1] = dd ? 0 : c[1] / 4; c[2] = dd ? 0 : c[2] / 4;
            }
            fwrite(c, 1, 3, o);
        }
    fclose(o);
}

static void print_cmd(const cmd_t *c)
{
    printf("      cmd");
    for (int i = 0; i < c->n; i++) printf(" %08x", c->w[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    int quiet = 0, do_resync = 1, count_depth = 1; int min_at = -1; const char *minpath = 0, *ppm = 0;
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-q")) quiet = 1;
        else if (!strcmp(argv[ai], "-v")) verbose = 1;
        else if (!strcmp(argv[ai], "-noresync")) do_resync = 0;
        else if (!strcmp(argv[ai], "-nodepth")) count_depth = 0;
        else if (!strcmp(argv[ai], "-min") && ai + 1 < argc) minpath = argv[++ai];
        else if (!strcmp(argv[ai], "-at") && ai + 1 < argc) min_at = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "-ppm") && ai + 1 < argc) ppm = argv[++ai];
        else { fprintf(stderr, "unknown option %s\n", argv[ai]); return 2; }
    }
    if (ai >= argc) { fprintf(stderr, "usage: rdpo [-q] [-v] [-noresync] [-nodepth] [-min out.rdps] [-ppm prefix] stream.rdps ...\n"); return 2; }
    mmem = malloc(MSIZE); omem = malloc(RSIZE); orec = calloc(GH, sizeof *orec);
    int any_bad = 0;
    uint64_t T_col = 0, T_str = 0, T_bstr = 0, T_prim = 0, T_bprim = 0, T_un = 0, T_ubad = 0, T_pn = 0, T_psame = 0, T_pin = 0, T_pinsame = 0, T_plogic = 0;
    for (; ai < argc; ai++) {
        if (load_stream(argv[ai])) return 2;
        init_memory();
        orc_init(omem);
        mrdp_init(&mdl, 0, mdl_rd, mdl_wr);
        cimg = 0; cwidth = 320; csize = 2; zimg = 0; have_z = 0; img_h = 240;
        cur_h = 0; cur_minl = 0; cur_maxl = 0; cur_tile = 0; cur_texrect = 0;
        memset(&ust, 0, sizeof ust); unit_shown = 0;
        memset(&pst, 0, sizeof pst); pix_shown = logic_shown = 0;
        /* Every tile set to zero on both sides before the stream: the oracle keeps flags it derives from a tile's
         * fields (among them whether it clamps) that only SET_TILE computes, so a tile a stream never sets would sample
         * unclamped there, a state the console does not have (its clamp is the tile's fields: a zero tile clamps). */
        for (uint32_t k = 0; k < 8; k++) {
            const uint32_t st[2] = { 0x35000000u, k << 24 };
            orc_cmd(st, 2);
            mrdp_push(&mdl, st[0]); mrdp_push(&mdl, st[1]);
        }
        uint64_t tot_c = 0, tot_h = 0, tot_z = 0; int nprim = 0, bad_prims = 0, first_bad = -1;
        if (!quiet) printf("%s: %d commands\n", argv[ai], ncmds);
        for (int i = 0; i < ncmds; i++) {
            const cmd_t *c = &cmds[i];
            unsigned op = (c->w[0] >> 24) & 63;
            if (op == 0x3F) { cimg = c->w[1] & 0xFFFFFF; cwidth = (c->w[0] & 0x3FF) + 1; csize = (c->w[0] >> 19) & 3; }
            if (op == 0x3E) { zimg = c->w[1] & 0xFFFFFF; have_z = 1; }
            if (op == 0x2D) { img_h = (int)(((c->w[1] & 0xFFF) + 3) >> 2); if (img_h > 1024) img_h = 1024; if (img_h < 1) img_h = 1; }
            if (op == 0x2F) cur_h = c->w[0] & 0xFFFFFF;
            if (op == 0x3A) cur_minl = (c->w[0] >> 8) & 31;
            if (op >= 8 && op <= 15) { cur_maxl = (c->w[0] >> 19) & 7; cur_tile = (c->w[0] >> 16) & 7; cur_texrect = 0; }
            if (op == 0x24 || op == 0x25) { cur_maxl = 0; cur_tile = (c->w[1] >> 24) & 7; cur_texrect = 1; }
            cur_gen = i + 1;
            orc_cmd(c->w, c->n);            /* (the oracle first: the model's LOD hook looks up its pixel) */
            for (int k = 0; k < c->n; k++) mrdp_push(&mdl, c->w[k]);
            if (mdl.ncmd != 0) { fprintf(stderr, "%s line %d: the command's length is not the model's (%d words given)\n", argv[ai], c->line, c->n); return 2; }
            if (!is_prim(c->w[0])) continue;
            nprim++;
            diff_t d = compare(count_depth);
            tot_c += d.c; tot_h += d.h; tot_z += d.z;
            int bad = d.c || d.h || (count_depth && d.z);
            if (bad) {
                bad_prims++;
                if (first_bad < 0) first_bad = i;
                if (!quiet && (bad_prims <= 3 || verbose)) {
                    static const char *wn[3] = { "colour", "coverage", "depth" };
                    printf("  command #%d (line %d, op %02x): %llu colour, %llu coverage, %llu depth words differ; first %s at (%d,%d):",
                           i, c->line, op, (unsigned long long)d.c, (unsigned long long)d.h, (unsigned long long)d.z, wn[d.what], d.fx, d.fy);
                    if (d.what == 1) printf(" model %u oracle %u\n", m_hid(d.fa), orc_hidden(d.fa));
                    else if (csize == 1 && d.what == 0) printf(" model %02x oracle %02x\n", mmem[d.fa], ob(d.fa));
                    else printf(" model %04x oracle %04x\n", m16(d.fa), o16(d.fa));
                    print_cmd(c);
                }
            }
            if (do_resync) resync();
        }
        int bad_stream = bad_prims || ust.bad;
        bad_stream |= pst.logic != 0;
        any_bad |= bad_stream;
        T_col += tot_c;
        T_str++; T_bstr += bad_stream != 0; T_prim += (uint64_t)nprim; T_bprim += (uint64_t)bad_prims; T_un += ust.n; T_ubad += ust.bad;
        T_pn += pst.n; T_psame += pst.same; T_pin += pst.in_diff; T_pinsame += pst.in_diff_out_same; T_plogic += pst.logic;
        printf("%s %s: %d primitives (%llu pixels drawn by the model), %d differ (%llu colour, %llu coverage, %llu depth words)",
               bad_stream ? "DIFF" : "SAME", argv[ai], nprim, (unsigned long long)mdl.pixels_drawn, bad_prims, (unsigned long long)tot_c, (unsigned long long)tot_h, (unsigned long long)tot_z);
        if (first_bad >= 0) printf(", first: command #%d line %d", first_bad, cmds[first_bad].line);
        printf("\n");
        if (ust.n) printf("  LOD unit: %llu oracle LODs through mrdp_lod, %llu differ (frac %llu, tiles %llu, lod %llu, magnify/far %llu)\n",
                          (unsigned long long)ust.n, (unsigned long long)ust.bad, (unsigned long long)ust.bad_frac, (unsigned long long)ust.bad_tile,
                          (unsigned long long)ust.bad_lod, (unsigned long long)ust.bad_mag);
        if (pst.n) printf("  LOD pixels: %llu drawn with an LOD, %llu agree, %llu took other inputs (%llu of them the same result; point 0 %llu, next pixel %llu, next line %llu, range %llu), %llu same inputs but another result, %llu with no oracle LOD\n",
                          (unsigned long long)pst.n, (unsigned long long)pst.same, (unsigned long long)pst.in_diff, (unsigned long long)pst.in_diff_out_same,
                          (unsigned long long)pst.in_s0, (unsigned long long)pst.in_pt1, (unsigned long long)pst.in_pt2, (unsigned long long)pst.in_oob,
                          (unsigned long long)pst.logic, (unsigned long long)pst.no_orc);
        if (pst.in_diff) printf("    inputs: pixel S, T off by 0: %llu, 1: %llu, 2-3: %llu, 4-15: %llu, 16+: %llu (max %d); the same steps (only the pixel's S, T moved) %llu; pixel the same but the next pixel not %llu, the next line not %llu\n",
                                (unsigned long long)pst.hist[0], (unsigned long long)pst.hist[1], (unsigned long long)pst.hist[2], (unsigned long long)pst.hist[3], (unsigned long long)pst.hist[4], pst.maxd,
                                (unsigned long long)pst.steps_same, (unsigned long long)pst.only1, (unsigned long long)pst.only2);
        if (ppm) {
            char path[1024];
            snprintf(path, sizeof path, "%s_model.ppm", ppm); write_ppm(path, 0, 0);
            snprintf(path, sizeof path, "%s_oracle.ppm", ppm); write_ppm(path, 1, 0);
            snprintf(path, sizeof path, "%s_diff.ppm", ppm); write_ppm(path, 0, 1);
        }
        if (minpath && min_at >= 0 && min_at < ncmds) first_bad = min_at;
        if (minpath && first_bad >= 0) {
            FILE *o = fopen(minpath, "w");
            if (o) {
                fprintf(o, "# the first differing primitive of %s (line %d) with every state command before it\n", argv[ai], cmds[first_bad].line);
                FILE *src = fopen(argv[ai], "r");
                static char line[1 << 20];
                while (src && fgets(line, sizeof line, src)) { char *p = line; while (*p == ' ') p++; if (!strncmp(p, "rdram", 5) || !strncmp(p, "mem", 3) || !strncmp(p, "fill", 4)) fputs(line, o); }
                if (src) fclose(src);
                for (int i = 0; i <= first_bad; i++) {
                    if (i < first_bad && is_prim(cmds[i].w[0])) continue;
                    fprintf(o, "cmd");
                    for (int k = 0; k < cmds[i].n; k++) fprintf(o, " %08x", cmds[i].w[k]);
                    fprintf(o, "\n");
                }
                fclose(o);
            }
        }
    }
    if (T_str >= 1)
        printf("TOTAL %llu streams, %llu differ; %llu primitives, %llu differ (%llu colour words); LOD unit %llu, %llu differ; LOD pixels %llu, %llu agree, %llu other inputs (%llu same result), %llu same inputs other result\n",
               (unsigned long long)T_str, (unsigned long long)T_bstr, (unsigned long long)T_prim, (unsigned long long)T_bprim, (unsigned long long)T_col, (unsigned long long)T_un, (unsigned long long)T_ubad,
               (unsigned long long)T_pn, (unsigned long long)T_psame, (unsigned long long)T_pin, (unsigned long long)T_pinsame, (unsigned long long)T_plogic);
    return any_bad;
}
