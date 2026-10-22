/* Minimal freestanding runtime helpers -- GCC may emit calls to these even
 * with -ffreestanding, and we link -nostdlib. */
#include <stddef.h>

/* Word-wise when both ends are word-aligned: GCC -Os turns every struct
 * assignment over a few words into a call here -- the clipper's 40-byte
 * clip_vtx_t copies alone were 14% of a game frame's geom cycles when this
 * copied a byte at a time (sim/geom_full PCPROF). */
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if ((((unsigned)dp | (unsigned)sp) & 3u) == 0u) {
        unsigned *dw = (unsigned *)dp; const unsigned *sw = (const unsigned *)sp;
        for (; n >= 4u; n -= 4u) *dw++ = *sw++;
        dp = (unsigned char *)dw; sp = (const unsigned char *)sw;
    }
    while (n--) *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *dp = d; const unsigned char *sp = s;
    if (dp < sp) while (n--) *dp++ = *sp++;
    else { dp += n; sp += n; while (n--) *--dp = *--sp; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = a, *pb = b;
    for (; n--; pa++, pb++) if (*pa != *pb) return *pa - *pb;
    return 0;
}

/* Minimal libgcc replacements for 64-bit shift-by-variable-amount and 64-bit
 * division -- rv32im has no native 64-bit ops (only 32x32->64 MUL, which GCC
 * synthesizes inline without a library call), so these are int64_t
 * operations GCC can't emit inline on its own. A normal freestanding build
 * gets them from -lgcc, but this one is -nostdlib, so they must be provided
 * here.
 *
 * __divdi3 currently has NO caller anywhere in this firmware (fx_from_bits_n
 * only needs the two shifts below) -- geom_fixed.c's fx_recip() and
 * fx_div_ii() were both rewritten to route through native 32-bit DIV/REM
 * instead (see fx_recip()'s own comment for the derivation), specifically
 * because this routine's own size was real ROM budget this firmware needed.
 * Left in place (the linker drops it via --gc-sections when unreferenced,
 * so it costs nothing) as general infrastructure in case a genuine 64/64
 * division ever becomes necessary again -- not a general-purpose soft-int64
 * library beyond that. */
long long __ashldi3(long long a, int b) {
    if (b <= 0) return a;
    if (b >= 64) return 0;
    return (long long)((unsigned long long)a << b);
}
long long __ashrdi3(long long a, int b) {
    if (b <= 0) return a;
    if (b >= 64) b = 63;
    return a >> b;   /* signed >> is arithmetic on this target (GCC/riscv) */
}
unsigned long long __lshrdi3(unsigned long long a, int b) {
    if (b <= 0) return a;
    if (b >= 64) return 0;
    return a >> b;
}
/* Plain bit-at-a-time long division -- correctness over speed. Call volume
 * is one per fx_recip()/fx_div_ii() (a handful per triangle: barycentric
 * area normalisation, viewport inverse-scale), not a per-pixel inner loop,
 * so 64 iterations/call is not a measured hotspot. */
long long __divdi3(long long a, long long b) {
    int neg = 0;
    unsigned long long ua, ub, q = 0, r = 0;
    if (a < 0) { ua = (unsigned long long)(-(unsigned long long)a); neg = !neg; } else ua = (unsigned long long)a;
    if (b < 0) { ub = (unsigned long long)(-(unsigned long long)b); neg = !neg; } else ub = (unsigned long long)b;
    if (ub == 0) return neg ? 0x8000000000000000LL : 0x7FFFFFFFFFFFFFFFLL;   /* div-by-0 guard */
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((ua >> i) & 1u);
        if (r >= ub) { r -= ub; q |= (1ULL << i); }
    }
    return neg ? -(long long)q : (long long)q;
}

/* RDP triangle setup (lang/c/rdp/rdp_setup.h): one unsigned 64-bit divide
 * per triangle (the area reciprocal) and count-leading-zeros; rv32im has no
 * clz instruction, so GCC calls these. */
unsigned long long __udivdi3(unsigned long long a, unsigned long long b) {
    unsigned long long q = 0, r = 0;
    if (b == 0) return ~0ULL;
    int top = 63;
    while (top > 0 && !((a >> top) & 1u)) top--;
    for (int i = top; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1u);
        if (r >= b) { r -= b; q |= (1ULL << i); }
    }
    return q;
}
int __clzsi2(unsigned int a) {
    int n = 0;
    if (!(a & 0xFFFF0000u)) { n += 16; a <<= 16; }
    if (!(a & 0xFF000000u)) { n += 8; a <<= 8; }
    if (!(a & 0xF0000000u)) { n += 4; a <<= 4; }
    if (!(a & 0xC0000000u)) { n += 2; a <<= 2; }
    if (!(a & 0x80000000u)) { n += 1; }
    return n;
}
int __clzdi2(unsigned long long a) {
    unsigned int hi = (unsigned int)(a >> 32);
    return hi ? __clzsi2(hi) : 32 + __clzsi2((unsigned int)a);
}
