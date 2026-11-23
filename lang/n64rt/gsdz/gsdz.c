/* Reader of the compressed Game Shark database (format 2): see gsdz.h and docs/format-v2.md.
 * Copyright 2026 tortuga.  SPDX-License-Identifier: Apache-2.0 */
#include "gsdz.h"
#include "tinf/tinf.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

int gsdz_open(gsdz_t *h, const uint8_t *db, uint32_t len)
{
    if (len < GSDZ_HEADER || db[0] != 'G' || db[1] != 'S' || db[2] != 'D' || db[3] != 'B' || be32(db + 4) != GSDZ_VERSION) return GSDZ_ERR_FORMAT;
    const uint32_t n = be32(db + 8), ib = be32(db + 12), maxu = be32(db + 16), total = be32(db + 20);
    if (n > 0xFFFFu || ib != GSDZ_ENTRY * n || total > len || total < GSDZ_HEADER + ib) return GSDZ_ERR_FORMAT;
    h->db = db; h->len = total; h->ngames = n; h->max_ulen = maxu;
    return GSDZ_OK;
}

int gsdz_entry(const gsdz_t *h, uint32_t i, gsdz_entry_t *e)
{
    if (i >= h->ngames) return GSDZ_ERR_NOTFOUND;
    const uint8_t *p = h->db + GSDZ_HEADER + GSDZ_ENTRY * i;
    e->crc1 = be32(p); e->crc2 = be32(p + 4); e->offset = be32(p + 8);
    e->clen = (uint16_t)be16(p + 12); e->ulen = (uint16_t)be16(p + 14); e->country = p[16];
    return GSDZ_OK;
}

int gsdz_find(const gsdz_t *h, uint32_t crc1, uint32_t crc2, uint8_t country, gsdz_entry_t *e)
{
    uint32_t lo = 0, hi = h->ngames;
    while (lo < hi) {                                           /* the first entry that is not before (crc1, crc2) */
        const uint32_t mid = lo + (hi - lo) / 2u;
        const uint8_t *p = h->db + GSDZ_HEADER + GSDZ_ENTRY * mid;
        const uint32_t a = be32(p), b = be32(p + 4);
        if (a < crc1 || (a == crc1 && b < crc2)) lo = mid + 1u; else hi = mid;
    }
    int first = -1;
    for (uint32_t i = lo; i < h->ngames; i++) {
        const uint8_t *p = h->db + GSDZ_HEADER + GSDZ_ENTRY * i;
        if (be32(p) != crc1 || be32(p + 4) != crc2) break;
        if (first < 0) first = (int)i;
        if (p[16] == country) { first = (int)i; break; }
    }
    return first < 0 ? GSDZ_ERR_NOTFOUND : gsdz_entry(h, (uint32_t)first, e);
}

uint32_t gsdz_adler32(const uint8_t *p, uint32_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        uint32_t k = n < 5552u ? n : 5552u;                     /* the most bytes before the sums can overflow 32 bits */
        n -= k;
        while (k--) { a += *p++; b += a; }
        a %= 65521u; b %= 65521u;
    }
    return b << 16 | a;
}

int gsdz_unpack(const gsdz_t *h, const gsdz_entry_t *e, uint8_t *out, uint32_t out_cap, int verify)
{
    if (e->clen < 7u || e->offset < GSDZ_HEADER || (uint64_t)e->offset + e->clen > h->len) return GSDZ_ERR_FORMAT;
    if (out_cap < e->ulen) return GSDZ_ERR_BUFFER;
    const uint8_t *z = h->db + e->offset;
    /* RFC 1950: CMF (method 8), FLG (header check, no preset dictionary), the deflate data, the Adler-32 of the output (big-endian) */
    if ((z[0] & 0x0Fu) != 8u || ((z[0] << 8 | z[1]) % 31u) != 0 || (z[1] & 0x20u)) return GSDZ_ERR_DATA;
    unsigned int dlen = e->ulen;
    if (tinf_uncompress(out, &dlen, z + 2, (unsigned int)e->clen - 6u) != TINF_OK || dlen != e->ulen) return GSDZ_ERR_DATA;
    if (verify && gsdz_adler32(out, e->ulen) != be32(z + e->clen - 4u)) return GSDZ_ERR_DATA;
    return (int)e->ulen;
}

int gsdz_get(const gsdz_t *h, uint32_t crc1, uint32_t crc2, uint8_t country, uint8_t *out, uint32_t out_cap, int verify)
{
    gsdz_entry_t e;
    const int r = gsdz_find(h, crc1, crc2, country, &e);
    return r ? r : gsdz_unpack(h, &e, out, out_cap, verify);
}
