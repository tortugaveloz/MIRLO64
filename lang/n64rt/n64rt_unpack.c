/* The decompressors of libdragon's compressed ELF in C: n64rt_unpack.h. What they must do is what the library's own decoders do
 * (the C ones of its src/compress, which its assembly stubs are the fast form of); written for the runtime's CPU, which has no
 * 64-bit registers in C (Shrinkler's 64-bit interval is a pair of words), and to be called on the stream where the IPL3's DMA left
 * it, with the output coming up over it. Nothing is allocated; Shrinkler's contexts are 2 KiB of this file's own. */
#include "n64rt_unpack.h"

#define BAD (-1)

/* ---- LZ4 -------------------------------------------------------------------------------------------------------- */
int n64rt_unpack_lz4(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max)
{
    const uint8_t *const end = src + size;
    uint8_t *o = dst;
    uint8_t *const oend = dst + max;
    while (src < end) {
        const uint32_t token = *src++;
        uint32_t n = token >> 4;
        if (n == 15u) {
            uint32_t b;
            do {
                if (src >= end) return BAD;
                b = *src++;
                n += b;
            } while (b == 255u);
        }
        if (n > (uint32_t)(end - src) || n > (uint32_t)(oend - o)) return BAD;
        for (const uint8_t *e = src + n; src != e; ) *o++ = *src++;
        if (end - src < 2) break;                       /* the last sequence has literals only */
        const uint32_t off = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
        src += 2;
        uint32_t m = token & 15u;
        if (m == 15u) {
            uint32_t b;
            do {
                if (src >= end) return BAD;
                b = *src++;
                m += b;
            } while (b == 255u);
        }
        m += 4u;
        if (off == 0 || off > (uint32_t)(o - dst) || m > (uint32_t)(oend - o)) return BAD;
        const uint8_t *from = o - off;
        for (uint8_t *e = o + m; o != e; ) *o++ = *from++;
    }
    return (int)(o - dst);
}

/* ---- aPLib ------------------------------------------------------------------------------------------------------ */
int n64rt_unpack_aplib(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max)
{
    const uint8_t *const end = src + size;
    uint8_t *o = dst;
    uint8_t *const oend = dst + max;
    uint32_t cc = 0, t;
    int left = 0;                                       /* the bits of cc not read yet */
    uint32_t nlit = 3, off = 0;
    /* the stream's bits, most significant first; the byte is read when its first bit is wanted */
#define BYTE(v) do { if (src >= end) return BAD; (v) = *src++; } while (0)
#define BIT(v) do { if (left == 0) { BYTE(cc); left = 8; } left--; (v) = (cc >> left) & 1u; } while (0)
    /* the Elias-gamma number: its first 1 bit, then a (bit, continuation) pair for every further bit of it */
#define GAMMA(v) do { uint32_t g_ = 1, c_; do { uint32_t b_; BIT(b_); g_ = (g_ << 1) | b_; BIT(c_); } while (c_); (v) = g_; } while (0)
    if (max == 0) return BAD;
    BYTE(t);
    *o++ = (uint8_t)t;
    for (;;) {
        uint32_t b, len;
        BIT(b);
        if (!b) {                                       /* 0: a literal byte */
            if (o == oend) return BAD;
            BYTE(t);                                    /* (the next byte of the stream, not a bit of the reservoir) */
            *o++ = (uint8_t)t;
            nlit = 3;
            continue;
        }
        BIT(b);
        if (!b) {                                       /* 10: a match with a gamma-coded offset (8 + n bits) or a repeat of the last */
            uint32_t hi, lo;
            GAMMA(hi);
            if (hi >= nlit) {
                hi -= nlit;
                BYTE(lo);
                off = (hi << 8) | lo;
                GAMMA(len);
                if (off < 128u || off >= 32000u) len += 2u;
                else if (off >= 1280u) len += 1u;
            } else {
                GAMMA(len);
            }
        } else {
            BIT(b);
            if (!b) {                                   /* 110: an offset of 7 bits and a length of 2 or 3, or the end (0) */
                uint32_t cmd;
                BYTE(cmd);
                if (cmd == 0) break;
                off = cmd >> 1;
                len = 2u + (cmd & 1u);
            } else {                                    /* 111: a byte of the last 15 (or a zero) */
                uint32_t k = 0;
                for (int i = 0; i < 4; i++) { BIT(b); k = (k << 1) | b; }
                if (o == oend || (k > (uint32_t)(o - dst))) return BAD;
                *o = k ? o[-(int)k] : 0;
                o++;
                nlit = 3;
                continue;
            }
        }
        if (off == 0 || off > (uint32_t)(o - dst) || len > (uint32_t)(oend - o)) return BAD;
        const uint8_t *from = o - off;
        for (uint8_t *e = o + len; o != e; ) *o++ = *from++;
        nlit = 2;
    }
#undef BYTE
#undef BIT
#undef GAMMA
    return (int)(o - dst);
}

/* ---- Shrinkler -------------------------------------------------------------------------------------------------- */
/* 1 context for the kind of the next symbol's parity, 4 groups of 256: the literal's bits (by the position's parity), the
 * kind and the repeat flag (the first group, and the single one before it), the offsets' and the lengths' numbers. */
#define SH_SINGLE 1
#define SH_NCTX   (SH_SINGLE + 4 * 256)
#define SH_KIND   0
#define SH_REPEAT (-1)
#define SH_G_LIT  0
#define SH_G_OFFS 2
#define SH_G_LEN  3

typedef struct {
    uint32_t hi, lo;            /* the 64-bit interval value as the library has it: its top 16 bits (hi >> 16) are what is compared */
    uint32_t size;              /* the interval's size, 0x8000 .. 0xFFFF after the renormalisation */
    uint32_t left;              /* the shifts before the next word is read into lo (which is empty then) */
    const uint8_t *src, *end;
} sh_t;
static uint16_t sh_ctx[SH_NCTX];

#define SH_INLINE static inline __attribute__((always_inline))
SH_INLINE uint32_t sh_word(sh_t *s)
{
    const uint8_t *p = s->src;
    uint32_t w;
    if (p + 4 <= s->end) w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    else { w = 0; for (int i = 0; i < 4; i++) w = (w << 8) | (p + i < s->end ? p[i] : 0u); }   /* (past the end: zeros, as the cleared RDRAM has them) */
    s->src = p + 4;
    return w;
}
SH_INLINE int sh_bit(sh_t *s, unsigned ci)
{
    if (s->size < 0x8000u) {
        /* the shifts that bring size to 0x8000 or more (the CPU has no count-leading-zeros: one shift is the common case) */
        uint32_t n = 1, t = s->size;
        if (t < 0x4000u) {
            n = 0;
            if (t < 0x100u) { n = 8; t <<= 8; }
            if (t < 0x1000u) { n += 4; t <<= 4; }
            if (t < 0x4000u) { n += 2; t <<= 2; }
            if (t < 0x8000u) n += 1;
        }
        if (n > s->left) {                              /* the word is read when a shift needs a bit of it, as the library's decoder does */
            const uint32_t k = s->left;
            if (k) { s->hi = (s->hi << k) | (s->lo >> (32u - k)); s->lo <<= k; s->size <<= k; n -= k; }
            s->lo |= sh_word(s);
            s->left = 32;
        }
        s->hi = (s->hi << n) | (s->lo >> (32u - n));
        s->lo <<= n;
        s->size <<= n;
        s->left -= n;
    }
    const uint32_t prob = sh_ctx[ci];
    const uint32_t thr = (s->size * prob) >> 16;
    if ((s->hi >> 16) >= thr) {
        s->hi -= thr << 16;
        s->size -= thr;
        sh_ctx[ci] = (uint16_t)(prob - (prob >> 4));
        return 0;
    }
    s->size = thr;
    sh_ctx[ci] = (uint16_t)(prob + (0xFFFFu >> 4) - (prob >> 4));
    return 1;
}
SH_INLINE uint32_t sh_number(sh_t *s, unsigned base)
{
    int i = 0;
    while (sh_bit(s, base + (unsigned)i * 2u + 2u)) if (++i > 30) return 0;    /* (a number of 31 bits or more: a stream that is not one) */
    uint32_t v = 1;
    for (; i >= 0; i--) v = (v << 1) | (uint32_t)sh_bit(s, base + (unsigned)i * 2u + 1u);
    return v;
}
int n64rt_unpack_shrinkler(const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max)
{
    uint8_t *o = dst;
    uint8_t *const oend = dst + max;
    sh_t s;
    for (int i = 0; i < SH_NCTX; i++) sh_ctx[i] = 0x8000u;
    s.src = src; s.end = src + size;
    {   /* the first word: the interval value is it << 31 as 64 bits (a zero bit above it); one shift before the next word is read */
        const uint32_t w = sh_word(&s);
        s.hi = w >> 1;
        s.lo = w << 31;
        s.left = 1;
        s.size = 0x8000u;
    }
    int ref = 0, prev_ref = 0;
    uint32_t off = 0;
    for (;;) {
        if (s.src > s.end + 8) return BAD;              /* (the words ran out long ago: a stream with no end) */
        if (ref) {
            int rep = 0;
            if (!prev_ref) rep = sh_bit(&s, SH_SINGLE + (unsigned)SH_REPEAT);
            if (!rep) {
                off = sh_number(&s, SH_SINGLE + (SH_G_OFFS << 8)) - 2u;
                if (off == 0) break;                    /* (the end mark; a number that was no number is 0 - 2: far too large) */
            }
            const uint32_t len = sh_number(&s, SH_SINGLE + (SH_G_LEN << 8));
            prev_ref = 1;
            if (off == 0 || off > (uint32_t)(o - dst) || len > (uint32_t)(oend - o)) return BAD;
            const uint8_t *from = o - off;
            for (uint8_t *e = o + len; o != e; ) *o++ = *from++;
        } else {
            const unsigned par = (unsigned)(o - dst) & 1u;
            unsigned c = 1;
            if (o == oend) return BAD;
            for (int i = 0; i < 8; i++) c = (c << 1) | (unsigned)sh_bit(&s, SH_SINGLE + ((par << 8) | c));
            *o++ = (uint8_t)c;
            prev_ref = 0;
        }
        ref = sh_bit(&s, SH_SINGLE + SH_KIND + ((((unsigned)(o - dst)) & 1u) << 8));
    }
    return (int)(o - dst);
}

/* ---- which one a stub is ---------------------------------------------------------------------------------------- */
int n64rt_unpack_kind(const uint32_t *stub, uint32_t nwords)
{
    int a32000 = 0, a1280 = 0, s1026 = 0, s1538 = 0, l255 = 0;
    for (uint32_t i = 0; i < nwords; i++) {
        const uint32_t w = stub[i], op = w >> 26, imm = w & 0xFFFFu;
        if (op < 8 || op > 14) continue;                /* addi, addiu, slti, sltiu, andi, ori, xori */
        a32000 |= imm == 32000u; a1280 |= imm == 1280u;
        s1026 |= imm == 1026u; s1538 |= imm == 1538u;
        l255 |= imm == 255u;
    }
    if (a32000 && a1280) return N64RT_UNPACK_APLIB;
    if (s1026 && s1538) return N64RT_UNPACK_SHRINKLER;
    if (l255) return N64RT_UNPACK_LZ4;
    return 0;
}
int n64rt_unpack(int kind, const uint8_t *src, uint32_t size, uint8_t *dst, uint32_t max)
{
    switch (kind) {
    case N64RT_UNPACK_LZ4:       return n64rt_unpack_lz4(src, size, dst, max);
    case N64RT_UNPACK_APLIB:     return n64rt_unpack_aplib(src, size, dst, max);
    case N64RT_UNPACK_SHRINKLER: return n64rt_unpack_shrinkler(src, size, dst, max);
    default:                     return BAD;
    }
}
