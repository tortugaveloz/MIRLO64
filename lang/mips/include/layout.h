/* Where an N64 game's ROM, its saves and its accessories are in the
 * Pocket's memory (docs/mips.md, "The memory of an N64 game") -- the same
 * for every game, whatever its ROM's size and the RDRAM it uses:
 *
 *   SDRAM  0 -  1 MiB   the program (the runtime)
 *          1 - 33 MiB   a ROM's bytes from 32 MiB on (a ROM of more than 32 MiB)
 *         49 - 56 MiB   the accessories and the saves (data.json's slots 1 and 2)
 *         56 - 64 MiB   the N64's RDRAM (mips_core's window)
 *   PSRAM  0 - 32 MiB   a ROM's first 32 MiB
 *
 * The Pocket's bridge (rtl/soc/mirlo_mips.sv) writes what it is given below
 * 1 MiB to the SDRAM at its address, what it is given from 1 MiB to 33 MiB to
 * the PSRAM at the address mod 32 MiB, and from 33 MiB to 65 MiB to the SDRAM
 * at the address less 32 MiB. The runtime reads the ROM through its window
 * (N64RT_ROM_WIN, lang/n64rt/n64rt.h), as the cartridge's reads do: its byte
 * o is the file's at 1 MiB + o. The program is loaded at 0 and the ROM at 1 MiB --
 * as two files, or as a game file with both in it -- so a ROM never reaches
 * the accessories' area: a slot there has, as its address in data.json, its
 * SDRAM address with bit 26 set (LAYOUT_SLOT_BRIDGE), where no ROM is.
 *
 * A core and a program of this layout say so, each with the word
 * LAYOUT_MARK at 8 bytes from its start (the boot ROM: 0xBFC0_0008; a
 * program: 0x4000_0008, lang/mips/linker/init_asm.S). The boot ROM starts
 * only a program that has it, and the runtime runs a game only on a core that
 * has it: an older one of either keeps the saves where the other does not,
 * and the Pocket writes a save's memory back into its file when the core
 * quits. */
#ifndef __LAYOUT_H
#define __LAYOUT_H

#define LAYOUT_MARK         0x4D4C0003      /* "ML", layout 3 (2: the ROM's first 48 MiB in the SDRAM at 1 MiB) */
#define LAYOUT_MARK_OFS     8

#define LAYOUT_FILE_PSRAM   0x02100000      /* what the Pocket loads from 1 MiB to this is in the PSRAM; */
#define LAYOUT_FILE_MAX     0x04100000      /* from it up to this, in the SDRAM at the address less 32 MiB */
#define LAYOUT_SLOT_BRIDGE  0x04000000      /* a slot's address in data.json: its SDRAM offset | this */

/* the accessories, the saves and the save-type database, SDRAM 0x4310_0000 - 0x437F_FFFF. 0x4310_0000 -
 * 0x4310_FFFF is the database (data.json's slot LAYOUT_SAVEDB_SLOT, below); 0x4311_0000 - 0x4330_7FFF (about 2 MiB) is
 * not used: it was the file-backed Transfer Pak's Game Boy ROM and RAM (data.json's slots 2 and 3), which are gone; the
 * layout is the same (LAYOUT_MARK) and nothing else is mapped there */
#define LAYOUT_SAVEDB       0x43100000      /* the save-type database, a text file (n64savedb.txt: docs/mips.md), loaded by the Pocket */
#define LAYOUT_SAVEDB_SLOT  4               /* data.json's: optional (no file: its size is 0), after the program's slot 3 */
#define LAYOUT_SAVEDB_MAX   0x00010000      /* (slot 4 is not nonvolatile: the Pocket never writes it back) */
#define LAYOUT_CPAK         0x43308000      /* the Controller Pak, 32 KiB (data.json's slot LAYOUT_CPAK_SLOT) */
#define LAYOUT_CPAK_SLOT    2               /* (the slots are numbered 0, 1, 2 ... with no gap: the Pocket's
                                             * table of file sizes is indexed by the slot's place in data.json) */
#define LAYOUT_PAK_BYTES    0x8000
#define LAYOUT_SAVE         0x43320000      /* slot 1: the N64 game's save. An EEPROM's 512 bytes today;
                                             * 128 KiB are kept for it (an SRAM's 32 KiB, a FlashRAM's 128) */
#define LAYOUT_SAVE_MAX     0x00020000
#define LAYOUT_SAVE_AUX     0x43340000      /* a FlashRAM's page buffer (128 bytes) and, at + 0x100, its status word pair: the
                                             * runtime's (n64rt_io.c), what the PI's engine and the game's reads of domain 2 reach
                                             * (rtl/soc/rcp_mi.sv's PI2: its address is the RTL's, mirlo_mips.sv's io_adr) */
#define LAYOUT_FREE         0x43341000      /* nothing yet, up to the hidden plane */
#define LAYOUT_HID          0x43700000      /* the RDP's hidden-bit plane: the N64's other two coverage bits of each
                                             * 16-bit word of RDRAM, 1 MiB (lang/c/rdp/rdp.h; the scan-out reads it) */
#define LAYOUT_RDRAM        0x43800000

#endif
