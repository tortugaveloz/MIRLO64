/* GameShark picker: the cheat database and the game's file of codes, with no hardware in it (a host test, test/gsdb_host.c, runs it on the PC).
 *
 * The database (tools/build_db.py makes it from mupen64plus's cheat file), all numbers big-endian:
 *   header 16 bytes: "GSDB", 1 (u32), the number of games n (u32), the index's size (u32, 16 n)
 *   index: n entries of 16 bytes sorted by (crc1, crc2, country): crc1 u32, crc2 u32, the game's offset u32 (from the file's start), country u8, length u24
 *   a game: its name (a length byte and the bytes), the number of cheats u16, and each cheat: name (length byte, bytes), the number of codes u8 and each code:
 *           kind u8 (0 plain, 1 a code with options), word u32 (the code's 8 hex digits), value u16 (kind 0), and for kind 1: the number of options u8 and each: value u16,
 *           label (length byte, bytes). A cheat has at most one code with options.
 * The user's choice for each cheat of a game is a byte: 0 off; 1 on (a cheat with no options); 1 + k the option k of the one with options.
 * The file the core writes for a game, the one Mirlo64 reads (n64rt_cheat.c, n64rt_gs.h has the code format):
 *   # Game Shark: <the game's name>
 *   # <a cheat's name>                  or  # <a cheat's name> = <the option's label>
 *   80123456 0063                       its codes, one in a line (an option's value in place of the code's)
 * Only the cheats that are on are there. A file that someone wrote by hand reads too: a block of lines under a comment that is the name of a cheat is that cheat on; lines
 * that are the codes of a cheat, wherever they are, are that cheat on; what is left is kept as it is, after the cheats, and written back. */
#ifndef N64RT_GSDB_H
#define N64RT_GSDB_H

#include <stdint.h>

#define GS_MAX_CHEATS 1024u     /* a game with more has the first 1024 listed (the database of mupen64plus's data has at most 223 in a game) */
#define GS_MAX_EXTRA  512u      /* lines of a file that no cheat of the game explains, kept */

typedef struct {                /* the ROM's header, as the cheat file keys a game */
    uint32_t crc1, crc2;
    uint8_t  country;
    char     title[21];
} gs_rom_t;

typedef struct {
    const uint8_t *name;        /* in the blob: a length byte, then the bytes */
    const uint8_t *codes;       /* the first code */
    uint8_t        ncodes;
    int16_t        optcode;     /* the index of the code with options, -1 if none */
} gs_cheat_t;

typedef struct {
    const uint8_t *name;
    uint16_t       ncheats;
    gs_cheat_t     cheat[GS_MAX_CHEATS];
} gs_game_t;

typedef struct { const char *p; uint16_t n; } gs_line_t;

typedef struct {
    uint8_t   sel[GS_MAX_CHEATS];
    gs_line_t extra[GS_MAX_EXTRA];
    uint16_t  nextra;
} gs_state_t;

/* the ROM header's first bytes (at least 0x40) in any of the three orders a ROM file has (z64: 80 37 12 40, n64: 40 12 37 80, v64: 37 80 40 12) -> 1, or 0 for no ROM */
int gs_rom_header(const uint8_t *h, uint32_t n, gs_rom_t *r);

/* the database's header: 1 and n, the index's bytes, if it is one */
int gs_db_head(const uint8_t *h16, uint32_t n, uint32_t *ngames, uint32_t *index_bytes);
/* the index's entry i (16 bytes; valid until the next call), for a database that is not in memory: the player's picker reads it from the card by pieces. NULL: it could not be read */
typedef const uint8_t *(*gs_entry_fn)(void *ctx, uint32_t i);
/* gs_db_find over entries given by a function (a binary search: log2(n) calls, then the entries of the same CRCs one after the other) */
int gs_db_find_fn(gs_entry_fn entry, void *ctx, uint32_t ngames, const gs_rom_t *r, uint32_t *off, uint32_t *len);
/* the index (16 n bytes) searched for a game: the exact key, else the CRCs with any country: 1 and the game's offset and length, 0 if it has none */
int gs_db_find(const uint8_t *index, uint32_t ngames, const gs_rom_t *r, uint32_t *off, uint32_t *len);

/* a game's blob parsed (every length checked against `len`): 1, or 0 for a blob that is not one */
int gs_game_parse(const uint8_t *blob, uint32_t len, gs_game_t *g);
/* the strings and codes of a cheat */
unsigned gs_cheat_name(const gs_cheat_t *c, char *out, unsigned size);          /* NUL-ended; the length */
unsigned gs_cheat_nopts(const gs_cheat_t *c);                                    /* the options of its code with options, 0 if none */
unsigned gs_cheat_opt(const gs_cheat_t *c, unsigned k, char *label, unsigned size, uint16_t *value);

/* a file's text read: which cheats are on, the rest in st->extra (the pointers are into `txt`) */
void gs_file_parse(const gs_game_t *g, const char *txt, uint32_t len, gs_state_t *st);
/* the file for the state: the bytes written (at most `size` - 1, NUL-ended; 0 and nothing if it does not fit) */
unsigned gs_file_write(const gs_game_t *g, const gs_state_t *st, char *out, unsigned size);

#endif
