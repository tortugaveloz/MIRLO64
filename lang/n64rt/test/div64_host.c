/* div64_host: n64rt_div64.h's udiv64 against the host's 64-bit division, on edge cases and random operands of every width.
 *   gcc -O2 -I.. div64_host.c -o /tmp/div64_host && /tmp/div64_host     (prints PASS, or the first operands that differ) */
#include <stdio.h>
#include <stdlib.h>
#include "n64rt_div64.h"
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
static int check(uint64_t a, uint64_t b)
{
    uint64_t r, q = udiv64(a, b, &r);
    if (q == a / b && r == a % b) return 0;
    printf("FAIL: %016llx / %016llx: %016llx r %016llx, want %016llx r %016llx\n", (unsigned long long)a, (unsigned long long)b,
           (unsigned long long)q, (unsigned long long)r, (unsigned long long)(a / b), (unsigned long long)(a % b));
    return 1;
}
int main(void)
{
    static const uint64_t e[] = { 0, 1, 2, 3, 0xFFFF, 0x10000, 0xFFFFFFFFull, 0x100000000ull, 0x100000001ull, 0x7FFFFFFFFFFFFFFFull,
                                  0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFF00000000ull, 0x80000000ull, 0x8000ull };
    for (unsigned i = 0; i < sizeof e / sizeof e[0]; i++)
        for (unsigned j = 0; j < sizeof e / sizeof e[0]; j++) if (e[j] && check(e[i], e[j])) return 1;
    for (long n = 0; n < 20000000; n++) {
        uint64_t a = rnd() >> (rnd() & 63), b = rnd() >> (rnd() & 63);
        if (!b) b = 1;
        if (check(a, b)) return 1;
    }
    printf("PASS\n");
    return 0;
}
