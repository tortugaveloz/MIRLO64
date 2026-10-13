/* The save-type database: a text file on the SD card (Assets/n64/common/n64savedb.txt, data.json's slot 4), one
 * line per cartridge -- the cartridge's ID (the four characters at ROM 0x3B..0x3E, "?" for any) and its save type --
 * which the Pocket loads beside the program. This is the lookup, and nothing else: a pure function of the file's
 * bytes, which the runtime calls once at boot (n64rt_io.c) and a host test calls with any bytes (test/savedb_host.c).
 *
 *   # comment                         everything from a "#" to the end of the line; blank lines, CR, LF, tabs, spaces
 *   NBK?  eeprom16k  rev=0,1  # ...  ID, type, optionally the versions (the header byte 0x3F) the line is for
 *
 * The first line that matches the ID (and the version, if it has one) wins. A line that is not an ID of four
 * characters and a known type is skipped, as are the fields after the type that are not rev=. Nothing in the file can
 * make the lookup read beyond `len` bytes or fail: the worst a file does is not match. */
#ifndef __N64RT_SAVEDB_H
#define __N64RT_SAVEDB_H

enum {
    N64RT_SAVEDB_NONE = 0,          /* no line matches: the game is not in the file (or there is no file) */
    N64RT_SAVEDB_EEP4K,             /* eeprom4k: 512 bytes (the default) */
    N64RT_SAVEDB_EEP16K,            /* eeprom16k: 2,048 bytes */
    N64RT_SAVEDB_SRAM,              /* sram, flash, none: known, and not the runtime's to decide (it finds an SRAM or a FlashRAM by itself) */
    N64RT_SAVEDB_FLASH,
    N64RT_SAVEDB_NOSAVE
};

/* `db`: the file's `len` bytes; `id`: the cartridge's four characters; `rev`: the header's version byte */
int n64rt_savedb_lookup(const char *db, unsigned len, const unsigned char id[4], unsigned rev);

#endif
