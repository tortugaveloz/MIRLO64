/* n64rt_div64.h: the runtime's unsigned 64-bit division, on the CPU's 32-bit divu.
 *
 * The C library's 64-bit division (compiler-rt's __udivmoddi4) is a loop of shifts and subtractions on this 32-bit ABI: some
 * 600 cycles a division, where the console's ddivu takes 69. A game that divides 64-bit values by the thousand at its start
 * (its __ull_div is the runtime's: n64rt_boot.c ll_patch) spent most of its fields in that loop. Here the quotient is long
 * division by words, each step a 64 / 32 division done as two 32 / 16 ones on the hardware's divu (the classic normalised
 * method, Knuth's algorithm D with 16-bit digits): three or four divu, a hundred and fifty cycles or so.
 *
 * udiv64(a, b, &r): a / b, the remainder in *r; b != 0. Pure integer code: n64rt/test/div64_host.c checks it on the host. */
#ifndef N64RT_DIV64_H
#define N64RT_DIV64_H
#include <stdint.h>

static inline uint32_t div64_nlz(uint32_t x)          /* leading zeros (x != 0) */
{
    uint32_t n = 0;
    if (!(x >> 16)) { n += 16; x <<= 16; }
    if (!(x >> 24)) { n += 8; x <<= 8; }
    if (!(x >> 28)) { n += 4; x <<= 4; }
    if (!(x >> 30)) { n += 2; x <<= 2; }
    if (!(x >> 31)) n += 1;
    return n;
}
/* (u1:u0) / v with u1 < v (the quotient fits a word): the quotient, the remainder in *r */
static inline uint32_t div64_lu(uint32_t u1, uint32_t u0, uint32_t v, uint32_t *r)
{
    const uint32_t b = 65536u;
    uint32_t s = div64_nlz(v), vn1, vn0, un32, un10, un1, un0, q1, q0, rhat;
    v <<= s;                                            /* normalised: its top bit set */
    vn1 = v >> 16; vn0 = v & 0xFFFFu;
    un32 = s ? (u1 << s) | (u0 >> (32 - s)) : u1;
    un10 = u0 << s;
    un1 = un10 >> 16; un0 = un10 & 0xFFFFu;
    q1 = un32 / vn1; rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) { q1--; rhat += vn1; if (rhat >= b) break; }
    uint32_t un21 = un32 * b + un1 - q1 * v;
    q0 = un21 / vn1; rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) { q0--; rhat += vn1; if (rhat >= b) break; }
    *r = (un21 * b + un0 - q0 * v) >> s;
    return q1 * b + q0;
}
static inline uint64_t udiv64(uint64_t a, uint64_t b, uint64_t *rem)
{
    uint32_t ah = (uint32_t)(a >> 32), al = (uint32_t)a, bh = (uint32_t)(b >> 32), bl = (uint32_t)b, r;
    if (!bh) {                                          /* a word's divisor: a word of quotient at a time */
        if (!ah) { *rem = al % bl; return al / bl; }
        uint32_t qh = ah / bl, ql = div64_lu(ah % bl, al, bl, &r);
        *rem = r;
        return (uint64_t)qh << 32 | ql;
    }
    /* a divisor past a word: the quotient is a word -- estimated from the divisor's top word, normalised, then put right */
    uint32_t n = div64_nlz(bh);
    uint32_t v1 = (uint32_t)((b << n) >> 32);
    uint64_t u1 = a >> 1;
    uint32_t q1 = div64_lu((uint32_t)(u1 >> 32), (uint32_t)u1, v1, &r);
    uint64_t q0 = ((uint64_t)q1 << n) >> 31;
    if (q0) q0--;
    uint64_t rr = a - q0 * b;
    if (rr >= b) { q0++; rr -= b; }
    *rem = rr;
    return q0;
}
#endif
