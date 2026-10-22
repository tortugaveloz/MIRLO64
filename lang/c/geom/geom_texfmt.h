/* N64 texture-format decode -> the rasterizer's texels: RGBA16 to RGBA5551
 * (R[15:11] G[10:6] B[5:1] A[0]), the IA formats to RGBA4444 (R[15:12]
 * G[11:8] B[7:4] A[3:0]): 4 bits of alpha keep the soft edges of flames,
 * glows and smoke that a single alpha bit would turn into hard cut-outs. The
 * bind picks the format. The IA comments below speak of the 5551 widths.
 *
 * Header-only, so the same code builds for the geom core (rv32im, no FPU:
 * integer bit ops only) and for host tools.
 *
 * Scope: RGBA16 and RGBA32 (to 5551), IA16, IA8, IA4, I8 and I4 (to 4444,
 * an I texel's alpha is its intensity, as on the N64), CI8 and CI4 through
 * the palette (to 5551 from an RGBA16 palette, to 4444 from an IA16 one:
 * geom_texfmt_is_ia() says which). Anything else returns false and the
 * caller falls back to untextured (Gouraud), as if GDL_TEXBIND had never
 * arrived. Only a Mirlo-N64 build (GEOM_N64_ORDER) decodes RGBA32, I and CI:
 * SM64 has none, and its geom ROM has no room for them.
 */
#ifndef GEOM_TEXFMT_H
#define GEOM_TEXFMT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* The texel bytes, big-endian, as the N64's. A little-endian geom core
 * reading them out of memory that holds whole words (GEOM_N64_ORDER): byte k
 * of the image is at address ^ 3. The big-endian one reads them as they are.
 *
 * GEOM_TEXOUT: decoded texel i goes where RDP reads it -- the low half of
 * word i / 2 for an even i (its LOADs take a 16-bit texel from a word's
 * little-endian lane, as the host's C model does), which on a big-endian core
 * is halfword i ^ 1. */
#if defined(GEOM_N64_ORDER) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define GEOM_TEXB(src, k) (*(const uint8_t *)((uintptr_t)((src) + (k)) ^ 3u))
#else
#define GEOM_TEXB(src, k) ((src)[k])
#endif
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define GEOM_TEXOUT(i) ((i) ^ 1u)
#else
#define GEOM_TEXOUT(i) (i)
#endif

/* fmt/siz: N64 G_IM_FMT_ / G_IM_SIZ_ codes, exactly as carried in
 * GDL_TEXBIND's p0 field (geom_pipeline.c's GDL_TEXBIND case / geom_gdl.h).
 * src: raw N64-format texture bytes, big-endian (this port runs
 * NO_SEGMENTED_MEMORY with no asset-time byte-swapping -- the compiled
 * decomp data is bit-for-bit what a real N64 ROM would carry).
 * texel_count: width*height.
 * out: caller-provided buffer of at least texel_count uint16_t.
 * Returns false (leaving *out untouched) for any format/size this decoder
 * doesn't implement -- the caller must treat that as "skip texturing", not
 * as an error to propagate/crash on. */
/* a texel as the 4444 IA format (intensity in R, G and B) */
static inline uint16_t geom_texfmt_ia4444(uint32_t i4, uint32_t a4) {
    return (uint16_t)((i4 << 12) | (i4 << 8) | (i4 << 4) | a4);
}
/* palette entry k: RGBA5551 as it is, or IA16 as 4444 */
static inline uint16_t geom_texfmt_tlut(const uint8_t *tlut, uint32_t k, int tlut_ia) {
    uint32_t hi = GEOM_TEXB(tlut, 2u * k), lo = GEOM_TEXB(tlut, 2u * k + 1u);
    return tlut_ia ? geom_texfmt_ia4444(hi >> 4, lo >> 4) : (uint16_t)(hi << 8 | lo);
}
/* the texels come out as 4444 IA (else 5551 RGBA) */
static inline int geom_texfmt_is_ia(uint32_t fmt, int tlut_ia) {
    return fmt == 3u || fmt == 4u || (fmt == 2u && tlut_ia);
}

/* texel k of the image as 5551 or 4444 (0 for a format not decoded here) */
static inline uint16_t geom_texfmt_texel(uint32_t fmt, uint32_t siz, const uint8_t *src, uint32_t k,
                                         const uint8_t *tlut, int tlut_ia)
{
    uint32_t n4 = (k & 1u) ? GEOM_TEXB(src, k >> 1) & 0xFu : (uint32_t)GEOM_TEXB(src, k >> 1) >> 4;  /* 4-bit: high nibble first */
    switch (fmt << 2 | siz) {
    case 0u << 2 | 2u:                                  /* RGBA16: already 5551 */
        return (uint16_t)((uint32_t)GEOM_TEXB(src, 2u * k) << 8 | GEOM_TEXB(src, 2u * k + 1u));
#ifdef GEOM_N64_ORDER
    case 0u << 2 | 3u: {                                /* RGBA32: R G B A -> 5551, alpha its top bit */
        uint32_t r = GEOM_TEXB(src, 4u * k), g = GEOM_TEXB(src, 4u * k + 1u);
        uint32_t b = GEOM_TEXB(src, 4u * k + 2u), a = GEOM_TEXB(src, 4u * k + 3u);
        return (uint16_t)((r >> 3) << 11 | (g >> 3) << 6 | (b >> 3) << 1 | (a >> 7));
    }
#endif
    case 3u << 2 | 2u:                                  /* IA16: 8-bit intensity, 8-bit alpha */
        return geom_texfmt_ia4444((uint32_t)GEOM_TEXB(src, 2u * k) >> 4, (uint32_t)GEOM_TEXB(src, 2u * k + 1u) >> 4);
    case 3u << 2 | 1u: {                                /* IA8: 4-bit intensity, 4-bit alpha */
        uint32_t b = GEOM_TEXB(src, k);
        return geom_texfmt_ia4444(b >> 4, b & 0xFu);
    }
    case 3u << 2 | 0u: {                                /* IA4: 3-bit intensity, 1-bit alpha */
        uint32_t i3 = (n4 >> 1) & 0x7u;
        return geom_texfmt_ia4444((i3 << 1) | (i3 >> 2), (n4 & 1u) ? 0xFu : 0u);
    }
#ifdef GEOM_N64_ORDER
    case 4u << 2 | 1u: {                                /* I8: alpha = intensity */
        uint32_t i4 = (uint32_t)GEOM_TEXB(src, k) >> 4;
        return geom_texfmt_ia4444(i4, i4);
    }
    case 4u << 2 | 0u:                                  /* I4 */
        return geom_texfmt_ia4444(n4, n4);
    case 2u << 2 | 1u:                                  /* CI8, CI4: the palette's entry */
        return geom_texfmt_tlut(tlut, GEOM_TEXB(src, k), tlut_ia);
    case 2u << 2 | 0u:
        return geom_texfmt_tlut(tlut, n4, tlut_ia);
#endif
    default:
        return 0;
    }
}

/* Decode a w x h image, keeping every (1 << sx)-th column and (1 << sy)-th
 * row: (w >> sx) x (h >> sy) texels out (a texture larger than TMEM is
 * shown at a lower resolution rather than not at all). Texel (x, y) is
 * texel k0 + y * pw + x of src: a rectangle out of a wider image (pw is its
 * row pitch in texels, >= w). tlut: the palette
 * (CI), already at the tile's palette -- entry k of a CI4 texel's 16, or of
 * a CI8 texel's 256; tlut_ia: its entries are IA16. Returns false (out
 * untouched) for a format not decoded here, or CI without a palette. On the
 * geom core it runs from ROM A: ROM B holds the hot code and is full. */
#if defined(__mips__) && !defined(GEOM_HOST_TEST)
__attribute__((noinline, section(".text.rom_a.texfmt")))
#endif
static bool geom_texfmt_decode(uint32_t fmt, uint32_t siz, const uint8_t *src,
    uint32_t w, uint32_t h, uint32_t sx, uint32_t sy, uint16_t *out,
    const uint8_t *tlut, int tlut_ia, uint32_t pw, uint32_t k0)
{
    uint32_t c = fmt << 2 | siz;
#ifdef GEOM_N64_ORDER
    int known = c == (0u << 2 | 2u) || c == (0u << 2 | 3u) || fmt == 3u || (fmt == 4u && siz <= 1u)
             || (fmt == 2u && siz <= 1u && tlut);
#else
    int known = c == (0u << 2 | 2u) || fmt == 3u;
#endif
    if (!known || (fmt == 3u && siz == 3u)) return false;
    uint32_t wd = w >> sx, hd = h >> sy, n = w * h;
    if (!sx && !sy && pw == w && k0 == 0) switch (c) {                        /* whole: a loop per format (the switch hoisted) */
    case 0u << 2 | 2u:
        for (uint32_t i = 0; i < n; i++)
            out[GEOM_TEXOUT(i)] = (uint16_t)((uint32_t)GEOM_TEXB(src, 2u * i) << 8 | GEOM_TEXB(src, 2u * i + 1u));
        return true;
    case 3u << 2 | 2u:
        for (uint32_t i = 0; i < n; i++)
            out[GEOM_TEXOUT(i)] = geom_texfmt_ia4444((uint32_t)GEOM_TEXB(src, 2u * i) >> 4, (uint32_t)GEOM_TEXB(src, 2u * i + 1u) >> 4);
        return true;
    case 3u << 2 | 1u:
        for (uint32_t i = 0; i < n; i++) { uint32_t b = GEOM_TEXB(src, i); out[GEOM_TEXOUT(i)] = geom_texfmt_ia4444(b >> 4, b & 0xFu); }
        return true;
#ifdef GEOM_N64_ORDER
    case 2u << 2 | 1u:
        for (uint32_t i = 0; i < n; i++) out[GEOM_TEXOUT(i)] = geom_texfmt_tlut(tlut, GEOM_TEXB(src, i), tlut_ia);
        return true;
    case 2u << 2 | 0u:
        for (uint32_t i = 0; i < n; i += 2) {
            uint32_t b = GEOM_TEXB(src, i >> 1);
            out[GEOM_TEXOUT(i)] = geom_texfmt_tlut(tlut, b >> 4, tlut_ia);
            if (i + 1u < n) out[GEOM_TEXOUT(i + 1u)] = geom_texfmt_tlut(tlut, b & 0xFu, tlut_ia);
        }
        return true;
#endif
    default:
        break;                                          /* the rest: the texel loop below */
    }
    uint32_t o = 0;
    for (uint32_t y = 0; y < hd; y++)
        for (uint32_t x = 0; x < wd; x++, o++)
            out[GEOM_TEXOUT(o)] = geom_texfmt_texel(fmt, siz, src, k0 + (y << sy) * pw + (x << sx), tlut, tlut_ia);
    return true;
}
#endif /* GEOM_TEXFMT_H */
