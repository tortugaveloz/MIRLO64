/* Host test of the picker's reader of the compressed database (gsdz/gsdz.c and tinf): every game of a .gsdz (format 2) inflates to the block the same game has in the .gsdb (format 1) it was
 * made from, a game that is not there is not found, and damaged files (a byte changed, a stream cut) and too small a buffer are refused and never read or written out of bounds
 * (under ASAN and UBSAN).
 *   gcc -O1 -g -Wall -fsanitize=address,undefined -DNDEBUG -I../gsdz -I.. gsdz_host.c ../gsdz/gsdz.c ../gsdz/tinf/tinflate.c ../n64rt_gsdb.c -o /tmp/gsdz_host && /tmp/gsdz_host x.gsdb x.gsdz */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gsdz.h"
#include "n64rt_gsdb.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n + 16u); if (fread(b, 1, *n, f) != *n) exit(2); fclose(f);
    return b;
}
int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: gsdz_host x.gsdb x.gsdz\n"); return 2; }
    uint32_t n1, n2, ng1, ib1;
    uint8_t *v1 = slurp(argv[1], &n1), *v2 = slurp(argv[2], &n2);
    gsdz_t h;
    CHECK(gs_db_head(v1, n1, &ng1, &ib1), "the .gsdb header");
    CHECK(gsdz_open(&h, v2, n2) == GSDZ_OK && h.ngames == ng1, "the .gsdz header");
    printf("%u games: the .gsdb %u bytes, the .gsdz %u bytes (%.1f %%), the largest block %u bytes\n", h.ngames, n1, n2, 100.0 * n2 / n1, h.max_ulen);
    static uint8_t out[1 << 17];
    CHECK(h.max_ulen <= 65535u, "a block fits 16 bits");
    for (uint32_t i = 0; i < ng1; i++) {
        const uint8_t *e = v1 + 16u + 16u * i;
        if (i && !memcmp(e, e - 16, 8) && e[12] == e[12 - 16]) continue;            /* (a key twice in a database: the search finds the first) */
        const uint32_t c1 = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3], c2 = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
        const uint32_t off1 = (uint32_t)e[8] << 24 | e[9] << 16 | e[10] << 8 | e[11], len1 = (uint32_t)e[13] << 16 | e[14] << 8 | e[15];
        const int n = gsdz_get(&h, c1, c2, e[12], out, h.max_ulen, 1);
        CHECK(n == (int)len1 && !memcmp(out, v1 + off1, len1), "game %u: the inflated block differs (%d)", i, n);
        if (i % 11u == 0) {
            gsdz_entry_t en;
            CHECK(gsdz_find(&h, c1, c2, e[12], &en) == GSDZ_OK, "game %u not found", i);
            CHECK(gsdz_unpack(&h, &en, out, en.ulen - 1u, 1) == GSDZ_ERR_BUFFER, "game %u: a buffer one byte short is refused", i);
            uint8_t *c = malloc(en.clen);
            for (uint32_t k = 0; k < 24u; k++) {
                memcpy(c, v2 + en.offset, en.clen);
                c[(k * 2654435761u) % en.clen] ^= (uint8_t)(1u << (k & 7u));
                gsdz_t g2 = h; uint8_t *copy = malloc(n2); memcpy(copy, v2, n2); memcpy(copy + en.offset, c, en.clen); g2.db = copy;
                (void)gsdz_unpack(&g2, &en, out, h.max_ulen, 1);          /* (it must not crash; with the Adler-32 checked it refuses what differs) */
                free(copy);
            }
            free(c);
        }
    }
    gsdz_entry_t en;
    CHECK(gsdz_find(&h, 0x12345678u, 0x9ABCDEF0u, 0x45, &en) == GSDZ_ERR_NOTFOUND, "a game that is not there");
    { const uint8_t *e = v2 + 24u + 20u * 5u; const uint32_t c1 = (uint32_t)e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3], c2 = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
      CHECK(gsdz_find(&h, c1, c2, 0xEE, &en) == GSDZ_OK && en.crc1 == c1, "the CRCs with another country are found"); }
    CHECK(gsdz_open(&h, v2, 23) != GSDZ_OK && gsdz_open(&h, v2, n2 - 1u) != GSDZ_OK, "a truncated file is refused");
    uint8_t bad[32]; memset(bad, 0, sizeof bad);
    CHECK(gsdz_open(&h, bad, sizeof bad) != GSDZ_OK, "zeros are no database");
    free(v1); free(v2);
    printf(fails ? "gsdz_host: %d FAILED\n" : "gsdz_host: all passed\n", fails);
    return fails != 0;
}
