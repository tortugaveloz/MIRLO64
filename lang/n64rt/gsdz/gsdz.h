/* Reader of the compressed Game Shark database (format 2, docs/format-v2.md): finds a game by its key and returns its block, the same bytes as the format-1 game block.
 * No dependencies beyond tinf (c/tinf, zlib licence) and <stdint.h>.  No malloc, no globals, no unaligned access; the inflater needs about 1.4 KiB of stack and NO window
 * (it writes into the caller's output buffer and copies from it).
 * Copyright 2026 tortuga.  SPDX-License-Identifier: Apache-2.0 */
#ifndef GSDZ_H
#define GSDZ_H

#include <stdint.h>

#define GSDZ_VERSION     2u
#define GSDZ_HEADER      24u          /* bytes */
#define GSDZ_ENTRY       20u          /* bytes */

enum { GSDZ_OK = 0, GSDZ_ERR_FORMAT = -1, GSDZ_ERR_NOTFOUND = -2, GSDZ_ERR_BUFFER = -3, GSDZ_ERR_DATA = -4 };

typedef struct {
    const uint8_t *db;                /* the whole file, as the Pocket loaded it */
    uint32_t       len;               /* its length in bytes (the header's file size is checked against it) */
    uint32_t       ngames;
    uint32_t       max_ulen;          /* the largest uncompressed block: the size of the buffer gsdz_unpack needs */
} gsdz_t;

typedef struct {                      /* one index entry */
    uint32_t crc1, crc2;
    uint32_t offset;                  /* from the start of the file to the zlib stream */
    uint16_t clen;                    /* its length */
    uint16_t ulen;                    /* the length of the block it holds */
    uint8_t  country;
} gsdz_entry_t;

/* Checks the header and the index's bounds.  0 or GSDZ_ERR_FORMAT.  `len` is the size of the memory (a file of the slot's size is fine: only the header's own size is used). */
int gsdz_open(gsdz_t *h, const uint8_t *db, uint32_t len);

/* Binary search of the index: the entry with this key; else the first entry with these CRC words and any country.  0 or GSDZ_ERR_NOTFOUND. */
int gsdz_find(const gsdz_t *h, uint32_t crc1, uint32_t crc2, uint8_t country, gsdz_entry_t *e);

/* The entry number i (0 <= i < ngames), in key order. */
int gsdz_entry(const gsdz_t *h, uint32_t i, gsdz_entry_t *e);

/* Inflates the block of an entry into out[0 .. e->ulen) (out_cap must be at least e->ulen; h->max_ulen always suffices).  Returns the block's length (e->ulen), or a negative
 * GSDZ_ERR_*.  With verify != 0 the zlib stream's Adler-32 is also checked. */
int gsdz_unpack(const gsdz_t *h, const gsdz_entry_t *e, uint8_t *out, uint32_t out_cap, int verify);

/* gsdz_find + gsdz_unpack. */
int gsdz_get(const gsdz_t *h, uint32_t crc1, uint32_t crc2, uint8_t country, uint8_t *out, uint32_t out_cap, int verify);

uint32_t gsdz_adler32(const uint8_t *p, uint32_t n);

#endif
