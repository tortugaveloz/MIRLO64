/* Host test of the compressed ELF's decompressors (n64rt_unpack.c): a raw stream of libdragon's n64elfcompress (level 1 LZ4, 2 aPLib,
 * 3 Shrinkler) and the program it was made from. Each case is `KIND stream plain margin`, and the test checks that
 *   - the stream decompresses to the program, out of place and IN place -- the stream where the compressor puts it, ending `margin`
 *     bytes (+ 8) past the end of the program's room, the output coming up over it --, and the size is the program's;
 *   - a buffer one byte short of the program is refused, and nothing is written past it;
 *   - the stream cut at every length (a few hundred of them, spread over it), and with a byte changed (a few hundred places), is read
 *     and written inside its buffers, never out of them (under ASAN: the buffers are exactly their sizes), whatever it returns.
 * Run by unpack_check.py (which has the streams made): see it for the whole of it.
 *   gcc -O1 -g -Wall -fsanitize=address,undefined -I.. unpack_host.c ../n64rt_unpack.c -o /tmp/unpack_host && /tmp/unpack_host 1 s.lz4 p.bin 40 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n64rt_unpack.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
static uint8_t *slurp(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *n = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n ? *n : 1u); if (*n && fread(b, 1, *n, f) != *n) exit(2); fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: unpack_host KIND stream plain margin\n"); return 2; }
    const int kind = atoi(argv[1]);
    const uint32_t margin = (uint32_t)atol(argv[4]);
    uint32_t cn, pn;
    uint8_t *cmp = slurp(argv[2], &cn), *plain = slurp(argv[3], &pn);

    /* out of place */
    uint8_t *out = malloc(pn + 1u);
    int r = n64rt_unpack(kind, cmp, cn, out, pn);
    CHECK(r == (int)pn, "out of place: returned %d, the program is %u bytes", r, pn);
    CHECK(r == (int)pn && !memcmp(out, plain, pn), "out of place: the bytes differ");
    free(out);

    /* in place, as n64elfcompress lays it out: the room is the program's size and the margin (+ 8 for the stubs that write ahead),
     * the stream at its end; ELF segments may be odd-sized: the room is what it is */
    {
        uint32_t off = pn - cn + margin + 8u;
        if (off & 7u) off = (off + 7u) & ~7u;
        const uint32_t room = off + cn + 8u > pn + 8u ? off + cn + 8u : pn + 8u;
        uint8_t *b = calloc(room, 1);
        memcpy(b + off, cmp, cn);
        r = n64rt_unpack(kind, b + off, cn, b, pn);
        CHECK(r == (int)pn, "in place: returned %d, the program is %u bytes", r, pn);
        CHECK(r == (int)pn && !memcmp(b, plain, pn), "in place: the bytes differ");
        free(b);
    }

    /* the buffer one byte short: refused (the program has a byte more than it) */
    if (pn) {
        uint8_t *small = malloc(pn - 1u ? pn - 1u : 1u);
        r = n64rt_unpack(kind, cmp, cn, small, pn - 1u);
        CHECK(r < 0 || (uint32_t)r <= pn - 1u, "a short buffer: returned %d", r);
        CHECK(r < 0, "a short buffer: the program was not refused (%d)", r);
        free(small);
    }

    /* damaged streams, in buffers of exactly their sizes (ASAN) */
    {
        const uint32_t step = cn / 300u ? cn / 300u : 1u;
        uint8_t *dst = malloc(pn ? pn : 1u);
        uint8_t *b;
        for (uint32_t n = 0; n < cn; n += step) {                   /* cut */
            b = malloc(n ? n : 1u); memcpy(b, cmp, n);
            (void)n64rt_unpack(kind, b, n, dst, pn);
            free(b);
        }
        b = malloc(cn ? cn : 1u);
        uint32_t seed = 12345u;
        for (uint32_t k = 0; k < 400u && cn; k++) {                 /* a byte changed */
            memcpy(b, cmp, cn);
            seed = seed * 1103515245u + 12345u;
            b[(seed >> 8) % cn] ^= (uint8_t)(1u << ((seed >> 4) & 7u)) | 0x40u;
            (void)n64rt_unpack(kind, b, cn, dst, pn);
        }
        free(b); free(dst);
    }
    printf("unpack_host kind %d: %s -> %u bytes (%u): %s\n", kind, argv[2], pn, cn, fails ? "FAIL" : "ok");
    free(cmp); free(plain);
    return fails != 0;
}
