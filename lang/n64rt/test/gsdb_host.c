/* Host test of n64rt_gsdb.c (the GameShark picker's database and file code): every game of the cheat database parses, a choice written as a file reads back as
 * it was, a file written by hand reads, garbage and truncated input do not crash it (under ASAN and UBSAN).
 *   python3 <the Game Shark core's tools/build_db.py> mupencheat.txt /tmp/gs.gsdb
 *   gcc -O1 -g -Wall -fsanitize=address,undefined -I.. gsdb_host.c ../n64rt_gsdb.c -o /tmp/gsdb_host && /tmp/gsdb_host /tmp/gs.gsdb */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n64rt_gsdb.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static uint8_t *db; static uint32_t dbn, ngames, ibytes;
static unsigned pieces, maxpieces, maxlen, last_piece = ~0u;                        /* the 16 KiB pieces of the file a reader of the index would fetch */
static const uint8_t *entry_counting(void *ctx, uint32_t i)
{
    const uint32_t piece = (16u + 16u * i) / 16384u;
    if (piece != last_piece) { last_piece = piece; pieces++; }
    return (const uint8_t *)ctx + 16u * i;
}
static unsigned rnd_s = 12345;
static unsigned rnd(void) { rnd_s = rnd_s * 1664525u + 1013904223u; return rnd_s >> 8; }

static int load_game(unsigned idx, gs_game_t *g)
{
    const uint8_t *e = db + 16u + 16u * idx;
    const uint32_t off = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], len = (uint32_t)e[13] << 16 | e[14] << 8 | e[15];
    return gs_game_parse(db + off, len, g);
}

static void random_state(const gs_game_t *g, gs_state_t *st)
{
    memset(st, 0, sizeof *st);
    for (unsigned j = 0; j < g->ncheats; j++) {
        if (rnd() % 3u) continue;
        const unsigned no = gs_cheat_nopts(&g->cheat[j]);
        st->sel[j] = (uint8_t)(g->cheat[j].optcode < 0 ? 1u : 1u + rnd() % no);
    }
}

static void test_roundtrip(unsigned idx)
{
    static gs_game_t g; static gs_state_t a, b; static char txt[1 << 16];
    CHECK(load_game(idx, &g), "game %u does not parse", idx);
    for (int it = 0; it < 4; it++) {
        random_state(&g, &a);
        const unsigned n = gs_file_write(&g, &a, txt, sizeof txt);
        CHECK(n > 0, "game %u: the file does not fit", idx);
        gs_file_parse(&g, txt, n, &b);
        for (unsigned j = 0; j < g.ncheats; j++) CHECK(a.sel[j] == b.sel[j], "game %u cheat %u: wrote %u read %u", idx, j, a.sel[j], b.sel[j]);
        CHECK(b.nextra == 0, "game %u: %u extra lines from our own file", idx, b.nextra);
        static char txt2[1 << 16];
        const unsigned n2 = gs_file_write(&g, &b, txt2, sizeof txt2);
        CHECK(n2 == n && !memcmp(txt, txt2, n), "game %u: the file is not stable", idx);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: gsdb_host <gsdb>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("no %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); dbn = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    db = malloc(dbn); if (fread(db, 1, dbn, f) != dbn) return 2; fclose(f);

    CHECK(gs_db_head(db, dbn, &ngames, &ibytes), "the database's header");
    CHECK(!gs_db_head(db, 15, &ngames, &ibytes), "a short header");
    printf("database: %u games, %u bytes\n", ngames, dbn);

    /* every game of the index is found by its key and parses; the totals match the builder's */
    unsigned cheats = 0, optc = 0, maxc = 0;
    for (unsigned i = 0; i < ngames; i++) {
        const uint8_t *e = db + 16u + 16u * i;
        gs_rom_t r = { (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3], (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7], e[12], "" };
        uint32_t off, len;
        CHECK(gs_db_find(db + 16, ngames, &r, &off, &len), "game %u not found", i);
        { uint32_t o2, l2; pieces = 0; last_piece = ~0u;                      /* the search by entries read on demand: the same game, in few pieces */
          CHECK(gs_db_find_fn(entry_counting, (void *)(db + 16), ngames, &r, &o2, &l2) && o2 == off && l2 == len, "game %u: the search by entry differs", i);
          CHECK(pieces <= 6u, "game %u: %u pieces of the index read", i, pieces); if (pieces > maxpieces) maxpieces = pieces; if (len > maxlen) maxlen = len; }
        static gs_game_t g;
        if (!load_game(i, &g)) { CHECK(0, "game %u does not parse", i); continue; }
        cheats += g.ncheats; if (g.ncheats > maxc) maxc = g.ncheats;
        for (unsigned j = 0; j < g.ncheats; j++) if (g.cheat[j].optcode >= 0) optc++;
    }
    printf("cheats: %u (%u with options), at most %u in a game\n", cheats, optc, maxc);
    printf("the index search read at most %u pieces of 16 KiB; the largest game block is %u bytes\n", maxpieces, maxlen);
    if (ngames == 557u) CHECK(cheats == 9238u, "the builder says 9238 cheats, %u read", cheats);       /* (the totals of the first database, from mupen64plus's cheat data) */
    { gs_rom_t r = { 0x12345678u, 0x9ABCDEF0u, 0x45, "" }; uint32_t o, l; CHECK(!gs_db_find(db + 16, ngames, &r, &o, &l), "a game that is not there"); }
    { const uint8_t *e = db + 16u + 16u * 5u; gs_rom_t r = { (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3], (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7], 0xEE, "" }; uint32_t o, l;
      CHECK(gs_db_find(db + 16, ngames, &r, &o, &l), "the CRCs with another country are found"); }

    for (unsigned i = 0; i < ngames; i += 7) test_roundtrip(i);
    test_roundtrip(0); test_roundtrip(ngames - 1);

    /* a file written by hand: the codes of two cheats with no comments, a block under a comment that is a cheat's name, a block that is none, a line that is no code */
    {
        static gs_game_t g; static gs_state_t st; static char name[128], txt[8192], out[8192];
        unsigned gi = 0;
        for (unsigned i = 0; i < ngames; i++) { load_game(i, &g); if (g.ncheats >= 5 && g.cheat[1].optcode < 0 && g.cheat[2].optcode < 0 && g.cheat[3].optcode < 0) { gi = i; break; } }
        load_game(gi, &g);
        unsigned o = 0;
        for (unsigned c = 1; c <= 2; c++) for (unsigned k = 0; k < g.cheat[c].ncodes; k++) {         /* cheats 1 and 2: their lines, bare */
            const uint8_t *cp = g.cheat[c].codes; for (unsigned q = 0; q < k; q++) { const uint8_t kind = cp[0]; cp += 7; if (kind == 1) { unsigned no = *cp++; while (no--) cp += 3u + cp[2]; } }
            o += (unsigned)sprintf(txt + o, "%02X%02X%02X%02X %04X  hand-made\n", cp[1], cp[2], cp[3], cp[4], cp[5] << 8 | cp[6]);
        }
        gs_cheat_name(&g.cheat[3], name, sizeof name);
        o += (unsigned)sprintf(txt + o, "# %s\n", name);
        o += (unsigned)sprintf(txt + o, "# my own\n7F123456 0001\n\nthis is no code\n8F123456 0002  another\n");
        gs_file_parse(&g, txt, o, &st);
        CHECK(!st.sel[0] && st.sel[1] == 1 && st.sel[2] == 1 && st.sel[3] == 1 && !st.sel[4], "hand-made: sel %u %u %u %u %u", st.sel[0], st.sel[1], st.sel[2], st.sel[3], st.sel[4]);
        CHECK(st.nextra == 3, "hand-made: %u extra lines (want a comment and two codes)", st.nextra);
        const unsigned n = gs_file_write(&g, &st, out, sizeof out);
        CHECK(n && strstr(out, "7F123456 0001") && strstr(out, "8F123456 0002"), "the extra lines are written back");
        static gs_state_t st2;
        gs_file_parse(&g, out, n, &st2);
        CHECK(!memcmp(st.sel, st2.sel, GS_MAX_CHEATS) && st2.nextra == st.nextra, "hand-made: not stable (%u extra)", st2.nextra);
    }

    /* a file that does not fit, and one that is garbage */
    { static gs_game_t g; static gs_state_t st; char tiny[16]; load_game(1, &g); for (unsigned j = 0; j < g.ncheats; j++) st.sel[j] = g.cheat[j].optcode < 0 ? 1 : 1; CHECK(gs_file_write(&g, &st, tiny, sizeof tiny) == 0, "a file that does not fit says 0"); }
    for (int it = 0; it < 3000; it++) {
        static gs_game_t g; static gs_state_t st; static char junk[3000];
        load_game(rnd() % ngames, &g);
        const unsigned n = rnd() % sizeof junk;
        for (unsigned i = 0; i < n; i++) junk[i] = (char)("0123456789ABCDEF #=\n\r;/ab"[rnd() % 26u]);
        gs_file_parse(&g, junk, n, &st);
        static char out[1 << 15]; (void)gs_file_write(&g, &st, out, sizeof out);
    }
    /* a blob cut anywhere, or with its bytes changed, never reads past its end */
    for (unsigned t = 0; t < 40; t++) {
        const unsigned i = rnd() % ngames;
        const uint8_t *e = db + 16u + 16u * i;
        const uint32_t off = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], len = (uint32_t)e[13] << 16 | e[14] << 8 | e[15];
        uint8_t *cp = malloc(len);
        for (uint32_t l = 0; l <= len; l += (len > 400 ? 3u : 1u)) { memcpy(cp, db + off, len); static gs_game_t g; uint8_t *tight = malloc(l ? l : 1); memcpy(tight, cp, l); (void)gs_game_parse(tight, l, &g); free(tight); }
        for (int m = 0; m < 50; m++) { memcpy(cp, db + off, len); cp[rnd() % len] = (uint8_t)rnd(); uint8_t *tight = malloc(len); memcpy(tight, cp, len); static gs_game_t g; (void)gs_game_parse(tight, len, &g); free(tight); }
        free(cp);
    }

    /* the ROM's header in the three byte orders */
    {
        uint8_t z[0x40] = { 0x80, 0x37, 0x12, 0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xAB, 0xCD, 0xEF, 0x01, 0x23, 0x45, 0x67, 0x89 };
        memcpy(z + 0x20, "SOME GAME TITLE     ", 20); z[0x3E] = 0x45;
        uint8_t n[0x40], v[0x40]; gs_rom_t r;
        for (int i = 0; i < 0x40; i++) { n[i] = z[(i & ~3) + 3 - (i & 3)]; v[i] = z[i ^ 1]; }
        for (int k = 0; k < 3; k++) {
            CHECK(gs_rom_header(k == 0 ? z : k == 1 ? n : v, 0x40, &r), "header order %d", k);
            CHECK(r.crc1 == 0xABCDEF01u && r.crc2 == 0x23456789u && r.country == 0x45 && !strcmp(r.title, "SOME GAME TITLE"), "header order %d: %08X %08X %02X '%s'", k, r.crc1, r.crc2, r.country, r.title);
        }
        z[0] = 0; CHECK(!gs_rom_header(z, 0x40, &r), "no ROM"); CHECK(!gs_rom_header(z, 0x3F, &r), "too short");
    }
    printf(fails ? "gsdb_host: %d FAILED\n" : "gsdb_host: all passed\n", fails);
    return fails != 0;
}
