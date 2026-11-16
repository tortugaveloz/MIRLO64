/* Double-precision add, sub, mul, abs, neg and the compares as rtl/mips/fpu_d64.sv does them (the CPU's FPD64 level).
 *
 * The same rules as the single-precision path (fpu_n64.sv): an input below the normals is a zero of its sign (the
 * VR4300 with FCR31.FS set); arithmetic rounds to nearest even with an unbounded exponent, then a result below the
 * normals is a zero of its sign and one above them infinity; a NaN result is the default one, whatever FCR31's rounding
 * mode and enables say (there are no exceptions and no cause or flag bits). The default NaN is the single's 0x7FBFFFFF
 * in a double's fields, 0x7FF7FFFF_FFFFFFFF. Exact: no step goes through a float or a host double, so that the host's
 * own arithmetic can check it (sim/fpd64: millions of operands and a grid of the special values).
 */
#ifndef FPD64_H
#define FPD64_H
#include <stdint.h>

#define FPD_QNAN 0x7FF7FFFFFFFFFFFFull

typedef struct { int s; int e; uint64_t m; } fpd_t;   /* m: the 53-bit significand with its hidden bit; 0 for a zero (e 0) */

static inline fpd_t fpd_unpack(uint64_t x)
{
    fpd_t r;
    r.s = (int)(x >> 63);
    r.e = (int)((x >> 52) & 0x7FF);
    r.m = r.e == 0 ? 0 : ((x & 0xFFFFFFFFFFFFFull) | (r.e == 0x7FF ? 0 : 1ull << 52));
    if (r.e == 0x7FF) r.m = x & 0xFFFFFFFFFFFFFull;      /* a NaN's fraction (zero: infinity) */
    return r;
}
static inline int fpd_isnan(uint64_t x) { return (x >> 52 & 0x7FF) == 0x7FF && (x & 0xFFFFFFFFFFFFFull) != 0; }
static inline int fpd_isinf(uint64_t x) { return (x >> 52 & 0x7FF) == 0x7FF && (x & 0xFFFFFFFFFFFFFull) == 0; }
static inline int fpd_iszero(uint64_t x) { return (x >> 52 & 0x7FF) == 0; }             /* a zero or a denormal */
static inline uint64_t fpd_ftz(uint64_t x) { return (x >> 52 & 0x7FF) == 0 ? x & 0x8000000000000000ull : x; }

/* a value s * m * 2^(e - 1075) (m up to 128 bits, sticky: bits below m that are not zero) rounded to nearest even at 53
 * bits with an unbounded exponent, then packed: zero below the normals, infinity above */
static inline uint64_t fpd_round(int s, int64_t e, unsigned __int128 m, int sticky)
{
    int n = 0;
    if (m == 0) return (uint64_t)s << 63;
    while ((m >> (n + 1)) != 0) n++;                         /* the top bit is bit n */
    /* the significand: 53 bits at the top; the exponent of the top bit */
    unsigned __int128 q; int g, st;
    if (n >= 53) {
        int sh = n - 52;
        q = m >> sh;
        g = (int)((m >> (sh - 1)) & 1);
        st = sticky || (sh >= 2 && (m & (((unsigned __int128)1 << (sh - 1)) - 1)) != 0);
    } else {
        q = m << (52 - n); g = 0; st = sticky;               /* (sticky below bits that were shifted in: only if a sticky came with m) */
    }
    int64_t be = e + n - 52 + 1075;                           /* biased exponent of q's top bit (bit 52) */
    if (g && (st || (q & 1))) q++;
    if (q >> 53) { q >>= 1; be++; }
    if (be >= 2047) return (uint64_t)s << 63 | 0x7FF0000000000000ull;
    if (be <= 0) return (uint64_t)s << 63;
    return (uint64_t)s << 63 | (uint64_t)be << 52 | ((uint64_t)q & 0xFFFFFFFFFFFFFull);
}

/* x + y, or x - y (sub) */
static inline uint64_t fpd_add(uint64_t x, uint64_t y, int sub)
{
    if (sub) y ^= 0x8000000000000000ull;
    if (fpd_isnan(x) || fpd_isnan(y)) return FPD_QNAN;
    fpd_t a = fpd_unpack(x), b = fpd_unpack(y);
    if (a.e == 0x7FF || b.e == 0x7FF) {
        if (a.e == 0x7FF && b.e == 0x7FF && a.s != b.s) return FPD_QNAN;
        return (uint64_t)(a.e == 0x7FF ? a.s : b.s) << 63 | 0x7FF0000000000000ull;
    }
    if (a.m == 0 && b.m == 0) return (uint64_t)(a.s & b.s) << 63;
    /* the larger first (by magnitude); the smaller aligned below it with 62 guard bits and a sticky */
    if (a.e < b.e || (a.e == b.e && a.m < b.m)) { fpd_t t = a; a = b; b = t; }
    if (a.m == 0) return 0;
    unsigned __int128 A = (unsigned __int128)a.m << 62, B = 0;
    int st = 0, d = a.e - (b.e == 0 ? a.e : b.e);
    if (b.m != 0) {
        if (d >= 120) { st = 1; }
        else { B = ((unsigned __int128)b.m << 62) >> d; st = (((unsigned __int128)b.m << 62) & (((unsigned __int128)1 << d) - 1)) != 0; }
    }
    unsigned __int128 S;
    if (a.s == b.s) S = A + B;
    else { S = A - B; if (st) { S--; /* the exact B is a little more than B: A - B is a little less than S */ } }
    /* with st set on a subtract, S is the floor of the exact difference; its fraction is nonzero */
    if (a.s != b.s && S == 0 && !st) return 0;               /* x - x: +0 */
    return fpd_round(a.s, (int64_t)a.e - 1075 - 62, S, st);
}

static inline uint64_t fpd_mul(uint64_t x, uint64_t y)
{
    if (fpd_isnan(x) || fpd_isnan(y)) return FPD_QNAN;
    fpd_t a = fpd_unpack(x), b = fpd_unpack(y);
    int s = a.s ^ b.s;
    if (a.e == 0x7FF || b.e == 0x7FF) {
        if (a.m == 0 && a.e != 0x7FF) return FPD_QNAN;        /* 0 * inf */
        if (b.m == 0 && b.e != 0x7FF) return FPD_QNAN;
        return (uint64_t)s << 63 | 0x7FF0000000000000ull;
    }
    if (a.m == 0 || b.m == 0) return (uint64_t)s << 63;
    return fpd_round(s, (int64_t)a.e + b.e - 2150, (unsigned __int128)a.m * b.m, 0);
}

static inline uint64_t fpd_abs(uint64_t x) { return fpd_isnan(x) ? FPD_QNAN : fpd_ftz(x) & 0x7FFFFFFFFFFFFFFFull; }
static inline uint64_t fpd_neg(uint64_t x) { return fpd_isnan(x) ? FPD_QNAN : fpd_ftz(x) ^ 0x8000000000000000ull; }

#endif
