/* Layout 7: SDRAM holds the runtime, last 34 MiB of ROM, database, coverage
 * and 8 MiB RDRAM. PSRAM holds the first 30 MiB of ROM and four
 * reserved Controller Pak images, the cart save and FlashRAM scratch.
 * Bridge slot addresses remain compatible with existing data.json files.
 * The marker prevents an incompatible runtime from accessing the saves. */
#ifndef __LAYOUT_H
#define __LAYOUT_H

#define LAYOUT_MARK         0x4D4C0007      /* "ML", layout 7 */
#define LAYOUT_MARK_OFS     8

#define LAYOUT_FILE_PSRAM   0x01F00000      /* bridge file offset at which the SDRAM ROM tail begins */
#define LAYOUT_FILE_MAX     0x04100000      /* end of the maximum 64 MiB ROM */
#define LAYOUT_SLOT_BRIDGE  0x04000000      /* a slot's address in data.json: its SDRAM offset | this */

/* Database remains in SDRAM. The save and Controller Pak slots below use
 * raw uncached PSRAM addresses; data.json retains its existing bridge addresses. */
#define LAYOUT_SAVEDB       0x43100000      /* the save-type database, a text file (n64savedb.txt: docs/mips.md), loaded by the Pocket */
#define LAYOUT_SAVEDB_SLOT  4               /* data.json's: optional (no file: its size is 0), after the program's slot 3 */
#define LAYOUT_SAVEDB_MAX   0x00010000      /* (slot 4 is not nonvolatile: the Pocket never writes it back) */
#define LAYOUT_CPAK         0x34108000      /* the Controller Pak, 32 KiB (data.json's slot LAYOUT_CPAK_SLOT) */
#define LAYOUT_CPAK_SLOT    2               /* (the slots are numbered 0, 1, 2 ... with no gap: the Pocket's
                                             * table of file sizes is indexed by the slot's place in data.json) */
#define LAYOUT_PAK_BYTES    0x8000
#define LAYOUT_PAK_CAPACITY 4               /* four 32 KiB Controller Paks reserved */
#define LAYOUT_SAVE         0x34140000      /* slot 1: the N64 game's save. An EEPROM's 512 bytes today;
                                             * 128 KiB are kept for it (an SRAM's 32 KiB, a FlashRAM's 128) */
#define LAYOUT_SAVE_MAX     0x00020000
#define LAYOUT_SAVE_AUX     0x34160000      /* a FlashRAM's page buffer (128 bytes) and, at + 0x100, its status word pair: the
                                             * runtime's (n64rt_io.c), what the PI's engine and the game's reads of domain 2 reach
                                             * (rtl/soc/rcp_mi.sv's PI2: its address is the RTL's, mirlo_mips.sv's io_adr) */
#define LAYOUT_FREE         0x34161000      /* unused PSRAM up to the ROM at byte offset 0x00200000 */
/* The virtual Transfer Paks' Game Boy cartridges (lang/n64rt/n64rt_gbvirt.c), one for each of controllers 1 and 2: a ROM and its battery RAM.
 * Each ROM is up to 3 MiB; each save is 128 KiB and a clock's 48 bytes, nonvolatile. The slots come last in data.json (the core's settings
 * list "Load GB Transfer Pak #1" and "#2" at the end): the ROMs are the last two. */
#define LAYOUT_GBSAVE       0x42D00000      /* slot 5: cartridge #1's RAM (and its clock's 48 bytes), at the bridge's 0x06D0_0000; nonvolatile */
#define LAYOUT_GBSAVE_SLOT  5
#define LAYOUT_GBSAVE_MAX   0x00021000
#define LAYOUT_GBSAVE2      0x42D50000      /* slot 6: cartridge #2's RAM, at the bridge's 0x06D5_0000; nonvolatile */
#define LAYOUT_GBSAVE2_SLOT 6
#define LAYOUT_GBROM        0x43400000      /* slot 8: cartridge #1's ROM, up to 3 MiB, at the bridge's 0x0740_0000. Below it the bridge's
                                             * 0x0730_0000 - 0x073F_FFFF are the PSRAM's, not the SDRAM's */
#define LAYOUT_GBROM_SLOT   8
#define LAYOUT_GBROM_MAX    0x00300000
#define LAYOUT_GBROM2       0x42E00000      /* slot 9: cartridge #2's ROM, up to 3 MiB, at the bridge's 0x06E0_0000 (to the save-type database's 0x4310_0000) */
#define LAYOUT_GBROM2_SLOT  9
/* the GameShark codes of the game (lang/n64rt/n64rt_cheat.c): data.json's slot LAYOUT_GS_SLOT, a file the runtime asks the Pocket to open by
 * its path (Assets/n64/common/gameshark/<the cartridge's ID>.txt) and to read to LAYOUT_GS; LAYOUT_GS_PARAM holds the open command's
 * parameter struct (the path, 256 bytes, then its flags and its size). The bridge's 0x06D3_0000 and 0x06D4_0000. */
#define LAYOUT_GS_PARAM     0x42D30000
#define LAYOUT_GS           0x42D40000
#define LAYOUT_GS_SLOT      7
#define LAYOUT_GS_MAX       0x00010000
#define LAYOUT_HID          0x43700000      /* the RDP's hidden-bit plane: the N64's other two coverage bits of each
                                             * 16-bit word of RDRAM, 1 MiB (lang/c/rdp/rdp.h; the scan-out reads it) */
#define LAYOUT_RDRAM        0x43800000
/* a save state (the Pocket's Sleep and Memories: lang/n64rt/n64rt_state.c), 0x4230_0000 - 0x42CF_FFFF: above the last byte of a
 * ROM of 64 MiB (0x4010_0000 + 34 MiB), which the bridge reads and writes at its address with bit 26 set (target/pocket/core_top.sv
 * gives the Pocket 0x0630_0000 and the size) */
#define LAYOUT_STATE        0x42300000
#define LAYOUT_STATE_BYTES  0x00A00000     /* (the largest size the core tells the Pocket: whole MiB; a state is at most 0x0097_0000 of it) */

#endif
