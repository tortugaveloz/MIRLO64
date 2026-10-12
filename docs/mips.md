# Mirlo's MIPS SoC

Mirlo's three cores are MIPS, and the SoC around them is hand-written
SystemVerilog, built on the same fabric as MIRLO64 (the SDRAM controller,
arbiter and scan-out). The geom core ran faster on MIPS in MIRLO64, and sharing
the fabric keeps MIRLO and MIRLO64 close together.

| Core | Implementation |
|------|----------------|
| game CPU | `rtl/mips/mips_core.sv`: a 32-bit VR4300, big-endian (the N64's order), single-precision FPU, 16 KiB I / 8 KiB D cache |
| geom core | `rtl/mips/mips_geom.sv` (mips_lite + D-cache), MIPS32 integer subset |
| audio core | `rtl/mips/mips_lite.sv` |

The CFUs (`Vpu4DFixed` + the setup unit, `AudioDspCfu`), MRDP and the video
path sit beside the cores.

## The SoC (`rtl/soc/mirlo_mips.sv`)

`target/pocket/core_top.sv` instantiates it. The memory map is:

| Address | What |
|---------|------|
| `0x2000_0000` | the geom core's ROM (its own); `0x2000_8000` its RAM, which the game CPU can also read and write |
| `0x4000_0000` | SDRAM, 64 MiB. The game CPU caches it; the program starts at its base |
| `0x40C0_0000` | the framebuffers (by convention) |
| `0x6000_0000` | the audio core: IMEM, DMEM, CTRL, IMEM2 (the game loads it; the core itself sees them at `0x8000_0000`) |
| `0x8000_0000` / `0xA000_0000` | an N64 game's RDRAM, 8 MiB: SDRAM `0x4380_0000` through KSEG0 (cached) or KSEG1 (see below) |
| `0xF000_0000` | the registers: `rtl/soc/mirlo_regs.sv`, map in `tools/mirlo_regs.py` |
| `0xBFC0_0000` | the game CPU's boot ROM (`lang/mips/boot`) |

The game CPU has no TLB and no KSEG0: `mips_core` is built with `FLAT`, so
every address is physical except the boot ROM's KSEG1, SDRAM is the cached
span, and the exception vector is `0x4000_0180`. It is big-endian (`BIG`
1), as the N64: memory is word-native (a 32-bit word is its value in any
order), byte k of a word sits in lane 3 - k. The Pocket's bridge loads a
game file's big-endian words as they are, so a game file is the plain output
of `objcopy`, and its blocks and footer are big-endian
(`tools/make_mirlo_game.py`). The geom and audio cores stay little-endian:
they read the game's bytes and halfwords in the N64's order (the geom core's
texture decoder, `GEOM_N64_ORDER`), and
the game hands the geom core its vertices already in its layout
(`gdl_vtx_pack()`).

## Running N64 code

An N64 game's code can run on the game CPU as it is, the way a static
recompilation would use it, only without recompiling: the instructions,
the byte order and the addresses are the N64's. `mips_core`'s `N64WIN`:

- `0x8000_0000` and `0xA000_0000` + 8 MiB are the N64's RDRAM, at SDRAM
  `0x4380_0000` (KSEG0 cached, KSEG1 not). The caches are physically tagged,
  so Mirlo's own code reaching the same memory at `0x438x_xxxx` and the game
  through KSEG0 see one copy.
- An instruction fetched from KSEG0's window is the game's. Its loads and
  stores anywhere else -- the RCP's registers, the cartridge, PIF RAM --
  take an address error (BadVAddr: the address), so Mirlo's runtime sees any
  access to N64 hardware the game still makes.

The hardware itself is not there: as in a recompilation, the libultra
functions that touch it are replaced by Mirlo's. `lang/mips/test/n64win_test.c`
checks the window and the trap in `sim/mips_soc`.

### The memory of an N64 game

It is the same for every game, whatever the size of its ROM and the RDRAM it
uses (`lang/mips/include/layout.h`):

| memory | what is there |
|---|---|
| SDRAM, 0 - 1 MiB | the program: the runtime |
| SDRAM, 1 - 49 MiB | the ROM's first 48 MiB, where the Pocket loads it |
| SDRAM, 49 - 56 MiB | the accessories and the saves, below |
| SDRAM, 56 - 64 MiB | the N64's RDRAM, 8 MiB (`0x4380_0000`: the CPU's window) |
| PSRAM, the first dies | the RDP's depth (`MIRLO_Z_PSRAM`, removed from the RTL: see the PSRAMs section) |
| PSRAM, the second dies | the last 16 MiB of a 64 MiB ROM (`MIRLO_ROM64`, removed from the RTL), and nothing else |

The accessories and the saves, each a slot of `data.json`:

| SDRAM | `data.json` | what |
|---|---|---|
| `0x4310_0000` | slot 4 at `0x0710_0000` | the save-type database, `n64savedb.txt`, up to 64 KiB, optional (below); not nonvolatile, the Pocket never writes it back |
| `0x4311_0000` - `0x4330_7FFF` | | nothing: about 2 MiB, once (with the first 64 KiB) a Game Boy cartridge's ROM and RAM for the Transfer Pak from files (slots 2 and 3, removed). The layout is the same and its mark unchanged |
| `0x4330_8000` | slot 2 at `0x0730_8000` | the Controller Pak, 32 KiB |
| `0x4332_0000` | slot 1 at `0x0732_0000` | the N64 game's save: an EEPROM's 512 bytes, an SRAM's 32 KiB or a FlashRAM's 128 KiB (128 KiB are kept for it), by what the game does |
| `0x4334_0000` | | a FlashRAM's page buffer (128 bytes) and, at `+0x100`, its status words (the runtime's: "The PI's domain 2 in hardware") |
| `0x4334_1000` - `0x437F_FFFF` | | nothing yet |

**The console is an N64 with the Expansion Pak, for every game**: `osMemSize` (0x8000_0318) is 8 MiB
and the loader has no row for it. A game reads it and uses what is there -- Donkey Kong 64, Majora's Mask
and Banjo-Tooie need all of it, the others (Super Mario 64, Star Fox 64 -- both revisions --, Ocarina of Time,
Banjo-Kazooie, Mario Kart 64, Pilotwings 64, Wave Race 64, 1080, Pokemon Stadium, GoldenEye) reached the same
screens as with 4 MiB. Every block addresses the whole of it (`lang/mips/test/ram8_test.c`, run by
`sim/mips_soc/ram8_check.sh`: the CPU, the RSP's DMA, the RDP's colour, depth and texture images, the PI's engine, the
SI, the AI and the VI above 4 MiB, with the word 4 MiB below each as a sentinel). The runtime keeps nothing of its
own in the game's RDRAM -- its one scratch word is the game's, saved and put back -- so the CIC-6103 IPL3 copy at
0 (Banjo-Kazooie checks three of its words) and the 8 MiB coexist; a CIC-6105 IPL3's word at 0x2FE1C0 (ROM 0x750,
0xAD170014: Donkey Kong 64 spins on it for ever without) is the CIC's, like that copy.

The boot information words at `0x8000_0300` are what the real IPL3 leaves: `osTvType` **from the ROM header's destination code**
(ROM 0x3E, the fourth character of the cartridge ID -- n64brew's list of the codes; the PIF hands IPL3 the system of the console
the cartridge was made for): 0 PAL for D, F, H, I, L, P, S, U, W, X, Y; 2 MPAL for B; 1 NTSC for every other (A, C, E, G, J, K, N, 7,
or a header with none; this was the only value, 1, for every ROM before 2026-10-10). The runtime derives nothing else from the
region (no game or region table in `n64rt.sys`): the game's own libultra takes osViClock (49,656,530 / 48,681,812 / 48,628,316
Hz), its first VI mode and the AI's divisor from `osTvType`. The PAL CICs (7101..7106) are the 6101..6106 ones with another PIF
seed: the IPL3 code is the same (SM64 PAL's boot code has CRC 0x90BB6CB5 as the US one: a 6102; 7102, the PAL 6101, is
0x009E9EA3 in the loader's list), so the boot words and the copies at 0 / 0x2FE1C0 are as for the NTSC ones. `osRomType` 0, `osRomBase` 0xB000_0000, `osResetType` 0 (cold), `osVersion` 0, `osMemSize` as above, and
`osCicId` (0x8000_0310) **0x17D7 for a CIC-6103 and 0x17D9 for a CIC-6105, as their IPL3s store it (`addiu t1, 0x17d7 / 0x17d9;
sw t1, 16(t0)`, found by disassembling the ones in the ROMs at hand), and 0 for the others** (6101, 6102, the PAL 7102: their
IPL3s do not write it; the 6106's was not available to read). It had been the decimal CIC number (6103, 6105), which no IPL3
leaves there; libultra does not read the word (a game's own code may).

The ROM is never moved. The Pocket loads the program at the bridge's
address 0 and the ROM at 1 MiB -- as one game file with both in it
(`tools/make_mirlo_game.py`, the ports' `ROM0@0x100000`) or as two files, the
ROM a `.z64` as it is -- and the bridge puts each byte where it stays; a
slot's address in `data.json` is one that no ROM has. The bridge's addresses
(`rtl/soc/mirlo_mips.sv`; address 0 is the SDRAM's first byte, `0x4000_0000`
to the CPU):

| bridge address | a write goes to |
|---|---|
| below `0x0310_0000` (49 MiB) | the SDRAM, at that address |
| `0x0310_0000` - `0x040F_FFFF` (49 - 65 MiB) | dropped (`MIRLO_ROM64`, which wrote it to the PSRAM's second die at the address less `0x0310_0000`, was removed from the RTL) |
| anything else below `0x0800_0000` | the SDRAM, at the address's low 26 bits: a slot at SDRAM address *a* is at `0x0400_0000` + *a* |

A read is always the SDRAM's, at the address's low 26 bits: the Pocket reads
the nonvolatile slots back when the core quits, and nothing of a ROM.
The decode looks at the address's megabyte only (bits 26 - 20; the PSRAM's
is `0x31` to `0x40`), and the SDRAM's address is the bridge's as it is. A
game file with the program and a 64 MiB ROM in it would be 48 bytes more
than 65 MiB, its block table and footer: a ROM that big is loaded as a file
of its own.

One more thing is done, for a core put on the card without its `data.json`.
The save slots used to be at the RDRAM's top (`0x03FE_0000` -
`0x03FF_01FF`), which is inside a ROM's range now. So that a `data.json`
older than this map does not lose a save with a core of this map, 56 - 64
MiB of the bridge is the SDRAM's as well: a write goes to the SDRAM first
and then to the PSRAM (a read is the SDRAM's anyway). A ROM's bytes do no
harm in the RDRAM before the game starts, and the Pocket reads a save back
from where it wrote it -- unchanged, with a game that leaves the RDRAM's
upper half alone; the game does not see that save (it looks at slot 1's new
place).

The runtime (`n64rt_boot.c`) reads the ROM where it is: `n64rt_rom_word()`
and `n64rt_rom_copy()` take a word from the SDRAM or, from ROM offset 48 MiB
on, from the PSRAM at `0x5000_0000`. A word from the PSRAM costs about twenty
clocks, so a megabyte read from a big ROM's end takes some 80 ms. A ROM of
more than 48 MiB needed the core with `MIRLO_ROM64` (removed from the RTL); a
smaller one runs on any.

**A core and a program of another layout.** The Pocket writes a save's
memory back into its file when the core quits, whatever ran. Each side
carries a mark of this layout, the word `0x4D4C0002` at 8 bytes from its
start (the boot ROM's `0xBFC0_0008`, a program's `0x4000_0008`: every
program linked with `lang/mips/linker/init_asm.S`):

- The boot ROM starts no program without the mark. An older program --
  whose runtime would move the ROM onto the saves -- is not run: the screen
  says so (and the JTAG UART), and the SDRAM stays as the Pocket loaded it,
  so the saves are written back as they were.
- The runtime runs no game on a core whose boot ROM is without the mark (an
  older core, which has the save slots in what is the RDRAM here): it says
  so on the screen and stops before it has written anything there.

The program has to be built again for a core of this layout; save files
stay as they are (the Pocket finds them by name). A program of the core's own
that is not an N64 game has the same slots: its save is at slot 1's place,
and with save files on the card it keeps out of `0x4310_0000` -
`0x437F_FFFF`.

### The TLB

The game CPU has none of the VR4300's kind in the SoC (`mips_core`'s
`TLB_EN`; the macro `MIRLO_TLB` that put its 32 entries in, ~550 ALMs, was removed). An N64
game's mapped addresses are those below `0x2000_0000`; the runtime keeps the
game's `osMapTLB` entries, and what happens at a mapped address depends on
the CPU:

- `MIRLO_TLB_RAM` (`mips_core`'s `TLB_RAM`): two translation tables in block
  RAM, one for data and one for fetches, 512 entries each -- one for a 4 KiB
  page, by its number's low 9 bits. A mapped access or fetch reads its page's
  entry (four cycles); if that is not this page's, valid and of this ASID, it
  takes the TLB's exception, the runtime writes the entry from the game's
  (`tlb_fill`: a page of any size, 4 KiB at a time, through COP0 register 23)
  and the instruction runs again. A page costs one exception however much it
  is used. The tables are emptied when the game maps or unmaps. Code runs from a
  mapped page: with a data access (and an entry without the C bit) it is the slow
  path, one uncached word at a time; a fetch from a page whose entry has the C bit
  (COP0 23's bit 5, from the game's EntryLo C field) goes through the I-cache --
  see "The fetch page registers" below.
  It costs the CPU ~120 ALUTs and 50 registers (~200 and 55 on the CPU
  without 64-bit integers) and 4 M10K: 73 ALMs in the fit.
- neither: a load or store to a mapped page traps (an address error) and the
  runtime does it on the game's behalf, thousands of times an access's cost;
  code cannot run there.

With `MIRLO_MON` (and the game running, `MCTL.GAME`) the game's own TLB
instructions -- `tlbwi`, `tlbwr`, `tlbr`, `tlbp` -- are a Reserved Instruction
for the monitor level (COP0 register 23's bit 0 says the core does that): the
runtime (`n64rt_os.c`, `tlb_insn`) keeps the 32 entries as the registers gave
them and does each as the VR4300 would on the game's COP0 registers (`tlbp`
searches them by VPN2 and ASID and sets Index's P bit on a miss -- the one bit
of a game's COP0 that only the monitor level can write by `mtc0`; `tlbr` gives
the registers back), and fills the translation tables from them as before. The
game's own `osMapTLB`, `osUnmapTLB`, `osUnmapTLBAll`, `__osProbeTLB` and
`osVirtualToPhysical` run as they are, wherever the game keeps them (a segment
it unpacks or loads later too): nothing of libultra has to be found or
patched, and there is no table of games at all (the loader has none). The cost is
a few LUTs (the decode gate and Index's bit 31); `lang/mips/test/tlbmon_test.c`
checks it (`sim/mips_soc`, a SoC built with `MIRLO_TLB_RAM` and `MIRLO_MON`).

**The game's TLB instructions on the monitor's fast path, and the C bit.** `tlbr`, `tlbp` and the `tlbwi` that changes no
translation (osUnmapTLBAll's thirty-two: the entry it replaces maps nothing and the one it writes has both pages invalid) are done
in `exception_entry` (`n64rt_exc.S`, `n64rt_mon_tlbq`), not by C behind the full path's save of the game's thirty registers, the
FPRs, HI and LO: the instruction is recognised (cause 10, not in a delay slot, code in KSEG0, `0x4200_0001 / 2 / 8`) with `$k0`
and `$k1`, seven of the game's registers (`$8`-`$14`) go to the context for the work and come back, and the entries are read and
written where C keeps them (`n64rt_tlbr`, 16 bytes each: mask, hi ^ 0x8000_0000, lo0, lo1; `n64rt_tlbs`, osMapTLB's table, whose
`span` says whether an entry maps anything). A `tlbwi` is checked with `$k0` / `$k1` alone first, so that the ones that map or
unmap a page, a `tlbwr` and everything else (a delay slot, code outside KSEG0) take the full path as before, with nothing changed
(the walk of `tlb_insn`, the tables to empty). Measured with `lang/n64rt/test/tlb_bench.c` (a game that checks the instructions'
results -- the same checksum before and after -- and times them from mark to mark in the simulation; a loader built with
`TLBPROF=1`), CPU clocks an instruction with warm caches, the game's own `mtc0`s included:

| | full path (before) | fast path |
|---|---|---|
| `tlbr` (GoldenEye's refill handler walks the 32 entries with one each) | 576 | 203 |
| `tlbp`, 32 entries looked at, none or the last one | 963 | 592-596 |
| `tlbwi` that changes nothing | 649 | 218 |
| `tlbwi` that maps or unmaps a page | 690 | 803 |
| `tlbwr` | 715 | 804 |

The two slower rows pay for the check that they are not for the fast path (~110 and ~90 clocks); every other Reserved Instruction
(a 64-bit instruction the runtime does for the game) pays the first part of it (~40). `tlbp`'s time is the thirty-two entries
(~17 clocks each). The entries the TLB's walk fills into the CPU's tables carry the page's **C bit** now: COP0 register 23's write
has a bit 5, `TR_C`, set when the entry's cache attribute (EntryLo bits 5:3) is not 2 (uncached), and always for osMapTLB's
entries. `mips_core`'s tables have no field for it (`tre_t` is `{v, d, g, asid, tag, pfn}`; bit 5 of the write is ignored) and
every mapped access is uncached, a word at a time, so nothing changes today; a core that caches mapped pages only has to take the bit (the
fetch table does now: see the fetch page registers below).

**The fetch page registers (`FPG`).** Fetching a mapped page one uncached word at a
time is 20-25 cycles an instruction (GoldenEye's boot, which runs from a 4 MiB window at
`0x7000_0000`, took 115 M cycles). `mips_core`'s two page registers hold the last two
cacheable pages of the fetch table that a walk found (virtual page, ASID / global, physical
page); F delivers the I-cache's word as it does for KSEG0 and D checks it a cycle later
(`d_fmiss`): a register must hold the page, and the line's tag must be the register's
physical page. The cache is indexed by the virtual address, as the VR4300's, and tagged by the
physical one -- `{valid, pa[28:12]}`, two bits wider than KSEG0 alone needs, because a frame
at another colour (index bits above the page offset) than its virtual address's would else hit
a line of its neighbour. A page no register holds walks the table (four cycles, the entry loads
the older register) and F goes back to the cache; one whose line is not in the cache refills
it. Every write to COP0 23 empties both registers (so the runtime has nothing extra to do: `tr_flush`, `tr_inval`
and every fill are such writes), and an ASID other than the entry's is no hit. The C bit is
`n64rt`'s from the game's EntryLo: a page without it is the slow path as before (and costs a
few cycles more per word: F takes the cache's word, D drops it, the table is walked).
*Aliases*: a frame mapped at a virtual address whose bits [14:12] differ from the frame's
(the VR4300 has two such colour bits; this cache has three) can sit in the cache under two
indexes; once a walk enters such a page (`ic_alias`), Hit_Invalidate_I and its siblings drop
the line under all 8 colours (8 cycles in M instead of one), so code rewritten and invalidated
through any alias is the new code through all. A game that maps nothing, or only pages of the
frame's own colour, never pays it. `lang/mips/test/tlbpg_test.c` checks it (speed, replacement,
unmap / remap / ASID, four aliases, page boundaries); `tlbgame_test` and `tlbmon_test` run
code through the game's own tlbwi. Cost (quartus_map of the whole SoC, `mips_core:cpu` self, FPG on against off):
+101 ALUT and +126 registers (the whole-design ALM estimate +54, inside its +-70 noise), +3,072 RAM bits and no
M10K (the table entry grows by one bit, the I-cache tag from 16 to 18 bits, each within the same two blocks).
Not fitted yet.

**A page no entry maps is the game's own handler's.** The tables are filled from
the game's entries a 4 KiB page at a time, as an access misses them; what its
entries do not map is a TLB exception *of the game*, delivered as the VR4300 does
(`n64rt_os.c`, `tlb_service` / `tlb_deliver`): no entry names the page -- TLBL
(a load or a fetch, Cause.ExcCode 2) or TLBS (a store, 3), at the refill vector
`0x8000_0000` if the game's EXL is clear and at `0x8000_0180` if it is set; an
entry names it with V clear -- the same codes at `0x8000_0180`; a store to a page
whose D bit is clear -- Mod (1) at `0x8000_0180` (the tables take a page's D bit
from its entry: a clean page is entered read-only). The monitor sets what the CPU
does for an exception it takes itself -- BadVAddr and EntryHi's VPN2 and ASID (the
CPU's, when the access missed), EPC (the branch's, with Cause.BD, in a delay slot;
neither is touched when EXL was set), Cause.ExcCode, Context's BadVPN2 field,
Status.EXL -- and returns to the vector. The game's handler (GoldenEye maps
0x7F000000 from one: a table read through Context, `tlbwr`, `eret`) runs as it is,
its `tlbwr` the monitor's (`tlb_insn`), and the access then runs again, finds the
tables empty for the page and fills it from the entry the handler wrote. The two
mtc0 writes that need are the monitor level's alone, like Index's P bit: Cause's
BD and ExcCode bits and Context's BadVPN2 (ERL set; two enables in `mips_core.sv`,
a few LUTs). A core without them gives the handler the vector and EPC all the same;
the codes are what the CPU last had. `lang/mips/test/tlbgame_test.c` checks it
(refill of a load, a store and a fetch with every register the handler sees, the
odd page, a delay slot, ASID, an invalid page, Mod, EXL set, a replaced pair);
a page replaced by `tlbwr` empties only its own pages of the tables (not all of
them).

**Where a game can map.** The N64's whole KUSEG, 0 - 0x7FFF_FFFF, is the TLB's; Mirlo's
own map has the RSP at `0x2000_0000`, the SDRAM (the runtime, running at the game's
level too, for the helpers it lends) at `0x4000_0000` and the PSRAM at `0x5000_0000`. A
core with `TLB_RAM` and `MON` maps `0x0000_0000 - 0x1FFF_FFFF` as before and now
`0x6000_0000 - 0x7FFF_FFFF` too (`MAP_HI`: `kseg01` and `n64code`, the same one-LUT
decode on the address): nothing of Mirlo's is there without the audio core (a build
with it, the legacy ones, does not set `MON`). GoldenEye's first code maps 4 MiB at
`0x7000_0000` onto the RDRAM's first 4 MiB (the odd page invalid) and jumps to its
own code there: without this it ran off its end through zeros, for ever. Pages from
`0x2000_0000` to `0x5FFF_FFFF` are still not a game's to map.

The runtime finds the tables as it starts (COP0 register 23 reads their
mark, or 0), so one game file runs on both. `lang/mips/test/tlbram_test.c`
checks the tables in `sim/mips_soc` (a SoC built with `MIRLO_TLB_RAM`): data,
a store to a clean page, another page of the same index, the ASID, a fetch,
code and its data on pages of one index, a delay slot on the page after its
branch's. Addresses from `0x2000_0000` up (the rest of KUSEG, KSEG2) are
Mirlo's own in this memory map and are not mapped. Super Mario 64 builds its
title screen's head with 51 TLB exceptions instead of 200,232 accesses done
by the runtime (two seconds during which it drew nothing).

### The VR4300's 64-bit integers

A game's own code uses them: its long long helpers (`ld`, `dsll32`,
`ddivu`, `mflo`), a `-mips3` compiler's register saves, block moves eight
bytes at a time. `mips_core` has them by levels; what a level leaves out is
a Reserved Instruction that the runtime does (`n64rt_os.c`, `emulate_64`)
on the registers it finds, which are the real ones:

| level | what the CPU does | ALMs |
|---|---|---|
| `W64` (in the SoC; the macro `MIRLO_CPU_NO64` that left it out was removed) | registers of 64 bits; `ld`, `sd` (through the cache and the write buffer, two words in two cycles), `lwu`, `dmfc0` / `dmtc0`; the branches on all 64 bits | ~230 |
| `W64_ALU` (`MIRLO_CPU_64ALU`) | `dadd(i)(u)`, `dsub(u)`, and `and` / `or` / `xor` / `nor` / `slt` when an operand is not a 32-bit value: the lower word is E's (the 32-bit instruction's), M does the upper word (a 32-bit adder and logic unit, with the carry of E's adder) and gives its hx; only `slt` / `sltu` come out of M late | ~300 more (synthesis estimate) |
| `W64_SH` (`MIRLO_CPU_64SH`) | the d-shifts, a byte or a bit a cycle (at most 14 cycles) | ~120 more |
| `W64_MD` (`MIRLO_CPU_64MD`) | `dmult(u)` (the 32-bit multiply's DSP block, four products of the operands' halves summed a word a time: 8 cycles in the background, the VR4300's own); HI and LO of 64 bits; `mfhi` / `mflo` / `mthi` / `mtlo` of 64-bit values | ~180 more |
| `W64_DIV` (with `W64_MD`; not in the SoC: the macro `MIRLO_CPU_64DIV` was removed) | `ddiv(u)`, a bit a cycle on a divider twice as wide | ~90 more |

Never in the CPU: `ldl` / `ldr` / `sdl` / `sdr`, a trap instruction on a
64-bit operand, and anything of the FPU's, which stays single precision
(no `dmfc1`, `dmtc1` or `.l` formats).

The device is full, so the SoC has the first level only: it is what a game
runs in a loop (register saves, block moves), and it makes the rest exact
-- the runtime works on real 64-bit registers, saved and restored whole
with a thread. What is left to the runtime is rare: of the games tried,
Super Mario 64, Pokemon Stadium and the two Zeldas run none of it, 1080
Snowboarding a handful of `dmultu` / `ddivu` in its time arithmetic.
Without `W64_MD` the CPU's HI and LO are 32 bits; the runtime keeps a
64-bit product or quotient itself and sets COP0 register 7's bit 0, which
makes `mfhi` / `mflo` Reserved Instructions (its own, again) until the
game's next multiply or divide.

The 64 bits are beside the pipeline: E computes 32 bits as before. A
register's upper half is stored as its difference from the lower half's
sign, zero for every 32-bit value, and is read from the register file
only: an instruction waits in D while a register it reads is on its way
from a 64-bit load (or the unit). 32-bit code pays nothing.

The runtime finds out which CPU it is on as it starts (an `ld` that runs or
traps) and saves a thread's registers whole only on the one that has them;
which levels the CPU has it reads from COP0 register 7's top bits (31 ALU,
30 SH, 29 MD, 28 DIV: 0 on a core before them), so one game file runs on
every bitstream. A CPU with `W64_MD` keeps the 64-bit HI and LO itself (the
runtime's `soft` mode, which keeps them in memory and traps `mfhi` / `mflo`, is
for the CPU without it), and the helpers of libultra's own 64-bit arithmetic
that the runtime replaces (`n64rt_boot.c`, `ll_patch`) are left as the game has
them where the CPU runs their instruction (the shifts with `W64_SH`,
`dmultu` with `W64_MD`); the ones that divide are replaced on every CPU (a
32-bit division when both operands fit, which is nearly always, instead of a
Reserved Instruction's 1,500 cycles for the `ddivu`).

Area and timing of the three levels together (no `W64_DIV`), in the closing
configuration (`MIRLO_CPU_CLK=32`, the 93.75 MHz clock of its own): the fit
goes from 15,092-15,424 ALMs (the same design with the levels off, MI and PI
on) to 15,961-16,020 (seeds 3, 5, 41, 20; the synthesis estimate of the
levels is +600, and a fit's own spread is about 200 ALMs), 4 DSP blocks as
before. The levels cost timing only through the area: what the CPU's clock
domain has to close is the same paths with or without them (the stall chain
to the D-cache's RAM, the FPU, the branch's target). What made them close
(fits, CPU clock domain: seeds 3, 41 and 20 positive, seed 5 -0.06): the
upper word of a 64-bit `dadd` and the logic operations at M in 32 bits and
the lower word at E, a bit in the register file for "its hx is not zero" (not
a 32-bit compare behind the forwarding masks in D), E's branch on registered
hx flags, M's wait for the shifter, `sc` and the store's data kind in
registers, the D-cache's write address independent of the conditions of the
write, `st_full` registered (the CPU waits one store early at worst), `mthi` /
`mtlo` written a cycle after M (from the multiplier's operand registers) and a
division by zero left to the divider, so that nothing but a flop is behind
M's commit enable, the dmult's sum starting from a register of its own
(`md_p2`: 8 cycles became 9), and in `fpu_n64` its decode and its operands'
kinds and orders in registers, and the normalisation in two cycles (a count,
then a shift): one cycle more each for a floating-point operation (a stream
of nothing else runs 8 % slower).

How it is checked:

- `sim/mips` runs the core in lockstep with a reference model (a build
  with every level: original ROMs as they are, libultra's own 64-bit thread
  switch and helpers included, 400 M instructions each of seven games), and
  `gen_rand_rom.py`'s random programs at each level: every integer
  instruction on registers full of 64-bit values, loads and stores through
  the cache and around it.
- The same random programs as a game under the runtime on the whole SoC
  (`gen_rand_rom.py ... full e2e`, built like any game): the CPU's level
  and the runtime's share together leave the registers the model does.
  With `W64_ALU`, `W64_SH` and `W64_MD` and no `W64_DIV` (the SoC's build) the
  three ways -- no levels (the runtime does it all), the levels with the
  runtime's `ddiv(u)`, and every level in hardware -- leave the same registers,
  HI and LO. `gen_rand_rom.py SEED out.z64 N nodiv` is the lockstep program of
  that build (no `ddiv`, `dmult` three times as often, `mfhi` / `mflo` behind
  them); `lang/mips/test/int64_test.c` checks the levels on the SoC against a
  table (and measures `dmultu` + `dmult` with their moves: about 54 CPU clocks).

Measured (the real-clock simulator, the runtime's debug log, Reserved
Instructions the monitor did for the game by field 60, `OWN=1`): Pokemon
Stadium 13,266 without any of the runtime's `__ll_*` helpers and without the
levels, 1 with them (the default build), 9,321 with the levels but without the
helpers (all of them `ddivu`: what divides stays replaced); Banjo-Kazooie
13,230 before, 102 with the levels (what is left: the FPU's `.l` formats and
`dmfc1`, and a `ddivu` of a helper `ll_patch` does not know); Star Fox 64 and
Super Mario 64 0 in both.

### The MI and the PI's DMA in hardware

A game that runs its own libultra reads and writes MI_MASK a thousand times in 60 fields and reads MI_INTR in every
interrupt; each of those accesses was an address error the runtime emulated (the monitor level, about 8 % of the game
CPU). With `MIRLO_MI` they are real bus accesses (`rtl/soc/rcp_mi.sv`), with `MIRLO_PI` PI_STATUS is and the cartridge
DMA is done by an engine:

* the game's `lw` / `sw` (word accesses only: the others still trap) to the MI's block, physical `0x0430_0000` (KSEG0 or
  KSEG1; its registers repeat through the 1 MiB), and to PI_STATUS (`0x0460_0010`) are no address error when COP0
  register 7 (MCTL) has its bit 8 set (written together with bit 9, which says the write is meant: a sticky bit, the
  monitor's entry and exit leave it alone). `mips_core`'s `MI`, `PIW`; the address reaches the SoC's device port as
  any other and `mirlo_mips.sv` answers it from `rcp_mi` (a store takes two cycles more to be acknowledged: the line is
  in the CPU's clock by the instruction after it).
* MI_MODE, MI_VERSION, MI_INTR (SP is the RSP's own line as it is; SI, AI, VI, PI, DP are bits), MI_MASK with the
  set / clear pairs, as the VR4300's. The line `(INTR & MASK) != 0` is Cause.IP2 (`MONITOR.CTL` bit 2). VI and DP are
  set by the scan-out's field and the RDP's SYNC FULL themselves; the runtime clears VI (a write to VI_CURRENT), AI
  (AI_STATUS) and SI (SI_STATUS) with `MONITOR.MICLR`, and sets SI, AI and PI (a copy it did itself) with
  `MONITOR.MIRAISE`. `MONITOR.MIINTR` reads INTR, MASK and the engine's busy flag.
* the PI engine copies words of the SDRAM (the ROM) to the RDRAM, a word at a time, as a second master of the Pocket
  bridge's port of the arbiter (the bridge goes first; ~20 sys cycles a word, 12 MB/s): `PIDST` (the RDRAM's word),
  `PISRC` (the SDRAM's word), `PILEN` (words; the write starts it). It ends in the PI interrupt; PI_STATUS reads
  {interrupt, 0, 0, busy}, a write of bit 0 aborts and of bit 1 clears the interrupt. PI_DRAM_ADDR, PI_CART_ADDR,
  PI_RD_LEN, PI_WR_LEN stay the runtime's (a write of PI_WR_LEN is a trap): it starts the engine for a copy that is
  word-aligned at both ends and in the SDRAM, and does the rest (a halfword-aligned cartridge address, a FlashRAM, the
  PSRAM's part of a ROM) itself, as before -- with `MIRLO_PI2` (below) the engine does all of it but the PSRAM's part.
* the runtime finds them by `MONITOR.ID` bits 1 (MI) and 2 (PI) and the readback of MCTL bit 8
  (`n64rt_mi_init`), and keeps its emulation for a core without them. With them the SP and DP wake-ups of the monitor
  are masked: they served only to keep the line.
* `lang/mips/test/mi_test.c` checks it on the simulator (a game in the RDRAM, its handler, the engine);
  `sim/mips`' lockstep bench has `-GMI=1 -GPIW=1 -GVIW=1 -GAIW=1` variants (the last with a random program of the SoC's build, `nodiv`: the same 3 M instructions, cycle for cycle, with and without it) (random programs do not reach the MI: they check that the
  core is unchanged).

### The VI in hardware

A game that runs its own libultra reads VI_CURRENT about 28 times a field (the monitor's `adel`) and writes some 17 VI
registers at every retrace (`ades`); each was an address error the runtime emulated, and the runtime computed the
scan-out's base, window and size from what it kept, at each field. With `MIRLO_VI` (which needs `MIRLO_MI`; `MONITOR.ID`
bit 3) the registers are the hardware's (`rtl/soc/rcp_vi.sv`):

* the game's `lw` / `sw` to the VI's block, physical `0x0440_0000` (KSEG0 or KSEG1; registers `0x00` STATUS, `0x04` ORIGIN,
  `0x08` WIDTH, `0x0C` V_INTR, `0x10` CURRENT, `0x14` BURST, `0x18` V_SYNC, `0x1C` H_SYNC, `0x20` LEAP, `0x24` H_START,
  `0x28` V_START, `0x2C` V_BURST, `0x30` X_SCALE, `0x34` Y_SCALE; beyond them 0), are real bus accesses, as the MI's: the
  same pass-through (MCTL bit 8; `mips_core`'s `VIW`, `e_hwx`'s second window, the same flop) and `mirlo_mips.sv`'s device
  port (`X_VIR`: a read waits a cycle for the block RAM, `X_VIW`: a write is done in one and ends as the MI's). Every register
  but CURRENT is a word of a block RAM of 16 and reads back what was written: one M10K, no flip-flop of its own.
* **MONITOR.CTL bit 3** (the runtime sets it when ID has bit 3) hands the scan-out's base, gamma and black to the VI, and
  MI_INTR's VI to its V_INTR. Left 0 (the bare tests, `hires_test`, `vwin_test`, a program that writes the
  scan-out's CSRs) everything is as it was: the CSRs, and the scan-out's field as the VI interrupt.
* **The TV system** (2026-10-10): the scan-out's field is NTSC (264 lines, 1,042,710 clocks, 59.94 Hz; MPAL too) or PAL (313
  lines, 1,250,000 clocks, 50.000 Hz: `docs/resolution.md`). V_SYNC decides once the game has written one that names a system
  (512..575: 525 lines, NTSC; 576..703: 625, PAL; any other value -- 0, noise, `vi_test`'s register pattern -- changes nothing);
  until then `VIDEO_FRAMEBUFFER_WIDTH` bit 18 does, which the runtime writes from the ROM's region (`n64rt_tv`, 0 = PAL). SM64
  (Europe) programs V_SYNC 0x271, H_SYNC 0x00150C69 (as the trace shows; libultra's `osViModePalLan1`: V_START 95..569, 237 lines like NTSC's, Y_SCALE 1.0,
  V_INTR 2), so its region and its mode agree; a PAL console's 60 Hz modes (`osViModeFpal*`, V_SYNC 525) get 59.94 Hz, as theirs.
  A PAL field is counted as 312 lines (CURRENT 0..623: the line 0 held for two scan-out lines, the line 1 at video line 282), the
  latch is the line 47 (V_START half-line 95), at video line 304; V_INTR 2 is then 46 lines (2.9 ms) before the latch. The
  change of system is taken at the frame boundary (`tv_pal`, WIDTH's bit 19 read) and applied where the field ends: the
  field in progress is whole, and the counter is already the new system's for the rest of it. The runtime's fields-per-tick
  (859 for PAL, 1030) and `vi_current`'s software counter follow `n64rt_vi_pal()`.
* CURRENT reads `{line[8:0], field}`. The line counts the scan-out's line pulses (`vi_line`): the N64's NTSC field has 262 lines,
  the scan-out's 264, so the line 0 is held for three of them; the scan-out's frame boundary (`vi_latch`, the base's latch,
  video line 255) is the line 18, VI_V_START's first, and sets the counter every frame (a size change's one odd line is made
  good in a frame). The field bit alternates every 264 lines and reads as such only when STATUS' SERRATE (bit 6) is set,
  else 0. A write to CURRENT clears MI_INTR's VI bit (`rcp_mi`'s `vi_clr`), in the cycle of the store.
* The interrupt is raised when the line counter reaches V_INTR / 2 (once a field: bit 0 of the half-line is not compared, so
  an interlaced mode's two fields both match). libultra's value 2 is the line 1, 17 lines (1.1 ms) before the latch: what
  the old field pulse (video line 236, 19 lines before it) gave the game's retrace handler to write its ORIGIN. The VI
  takes ORIGIN where the scan-out latches its base (the line 18), so what the handler writes in time is on the screen in the
  next frame, as on the console; a hi-res picture whose two fields' origins the game gives one row of the buffer apart
  (the second is the base + one 640-pixel row, 1280 bytes: WIDTH * 2 for libultra's own interlaced modes -- WIDTH 640, Y_SCALE
  2.0 -- and WIDTH for Donkey Kong 64's -- WIDTH 1280, Y_SCALE 1.0, a pitch of two rows; a write of that leaves the base as it
  is) is one frame of both, from the lower (the runtime's `vi_apply_base`, now the hardware's). The runtime's `tall` counts the
  rows of the pitch too (`vi_width_gamma`: lines x Y_SCALE x pitch rows > 300): DK64's logos were cut to their upper half
  before (480 rows shown as 240 of the interleaved buffer).
* STATUS' GAMMA bit, taken at the latch, is the scan-out's gamma; H_START 0 (osViBlack) and an ORIGIN that puts the
  picture out of the game's RDRAM (`MONITOR.VITHR`, which the runtime keeps: the last origin, in 64-byte units, a picture of
  the mode's size fits at) black the picture from the next frame on, as the runtime's `dma_enable` did.
* what stays the runtime's: the width, the window (x0, y0, rows) and the size (`video_framebuffer_width` / `window`): it
  computes them from WIDTH, V_START and Y_SCALE when they change. The hardware raises MONITOR.PEND's bit 1 -- no longer a
  field, so no wake-up per field -- when a write changes one of those three (the old word is read from the RAM before the
  new is written: the game rewrites them all at every retrace, and nothing happens), and the runtime
  (`n64rt_vi_hw_event`) reads them back. The ticks (4.2 ms) note the origin for `fb_overlaps` and count the fields for the
  debug log.
* area (`quartus_map` of the closing configuration with `MIRLO_MI`, `MIRLO_PI` and the 64-bit levels, with and without
  `MIRLO_VI`): `rcp_vi` is 112 ALUTs, 63 registers and one M10K (the 16 words); the whole design +200 ALUTs (the rest: the
  device port's read and the base's multiplexers, `MONITOR.VITHR`'s 17 flip-flops and its compare, the core's second window)
  and +90 registers -- about 130 ALMs, 0.7 % of the device; no fit has been made.
* `lang/mips/test/vi_test.c` checks it on the simulator (registers, the counter, the interrupt's line and period, the
  clear); `mi_test` expects no monitor entry for its VI_CURRENT stores when the SoC has the VI.

### The AI in hardware

A game that runs its own libultra reached the AI through the monitor (every `AI_LEN`, `AI_STATUS`, `AI_DRAM_ADDR`,
`AI_CONTROL`, `AI_DACRATE` access a trap: about 11 entries per audio frame), and the runtime read the game's buffers with
the CPU, resampled them from the DAC's rate to the Pocket's 48 kHz and wrote them to the Pocket's audio FIFO: `n64rt_audio_poll`
and `fetch_frame` were 5.0 to 5.5 % of all the game CPU's cycles in every game. With `MIRLO_AI` (which needs `MIRLO_MI`;
`MONITOR.ID` bit 4) all of it is the hardware's (`rtl/soc/rcp_ai.sv`):

* the game's `lw` / `sw` to the AI's block, physical `0x0450_0000` (the registers repeat through the 1 MiB), are real bus
  accesses, as the MI's and the VI's (`mips_core`'s `AIW`, the same pass-through, MCTL bit 8; `mirlo_mips.sv` answers the
  read in the cycle of the request and ends a write as the MI's does: two cycles for the effect on the interrupt line).
  `0x00 DRAM_ADDR` (write: 8-byte aligned, the RDRAM's 8 MiB), `0x04 LEN` (write: bytes, a multiple of 8, 0 is ignored: it
  starts the DMA when the AI is idle, else it is queued as the second buffer, and a third is dropped; read: the bytes left of
  the buffer being played), `0x08 CONTROL` (bit 0: the DMA enable; clear, the reader stops where it is), `0x0C STATUS` (read
  `{full [31], busy [30], DMA on [25], 1 [24], 1 [20], full [0]}`: full is a second buffer queued, busy a buffer
  being played; any write clears the AI interrupt), `0x10 DACRATE` (14 bits), `0x14 BITRATE` (ignored). A read of any register but STATUS is AI_LEN, as
  the console's (the others are write-only there).
* the interrupt (MI_INTR's AI bit, `rcp_mi`'s `ai_pulse`) is the console's: it rises when a
  buffer is written to an idle AI (the LEN write, which starts it) and when the buffer read to its end is replaced by the
  queued one; there is none when the queue empties. (Until 2026-10-09 it rose at the end of every buffer, the last included,
  and not at a start: a driver that queues its next buffer from the interrupt, as Nintendo's HVQM2 timekeeper does, had a
  silent gap after each buffer -- 245 words at a 1 ms handler, 1,925 at 8 ms in `sim/ai`'s bench, 0 now.) The write to
  AI_STATUS clears it in the cycle of the store (`ai_clr`). A buffer is current from its LEN write to the fetch of its last word
  (the console's `fillcount`).
* the reader is a third master of the Pocket bridge's port (with the PI's engine: the AI goes first when both ask, it is
  rare and must not wait for a copy): a word, a frame (L high, R low), at a time, ~20 cycles. A frame is read when the Pocket's
  FIFO is below 256 words (5.3 ms; 512 words, 10.7 ms, the software AI's `FIFO_TARGET`, until 2026-10-09): the reader leads the
  heard audio by that, and so do AI_LEN and the interrupts (the console's lead is a few words). The FIFO is kept at its lead by time, not by reading ahead
  (Pokemon Stadium hands a buffer over before the task that fills it has run).
* the resampler is the software AI's algorithm step for step -- a phase accumulator in Q16 frames, linear:
  `out = prev + ((next - prev) * (phase >> 1) >> 15)`, the phase `+= step` per output word, a frame fetched while
  `phase >= 0x10000` -- with one 17 x 16 multiplier (one DSP block) used for the two channels in turn. Without a buffer (or
  with the DMA disabled) it writes silence to hold the FIFO's lead for an eighth of a second (6000 words), then stops, and
  the FIFO is first primed with silence up to the target when the next sound starts, as the software did. The FIFO has two
  writers now, the CSR's and this (`mirlo_mips.sv`).
* what stays the runtime's: the step. `step = rate * 65536 / 48000` with `rate = osViClock / (DACRATE + 1)` (osViClock is the
  console's, from the ROM's region: PAL 49,656,530, NTSC 48,681,812, MPAL 48,628,316 Hz -- `n64rt_vi_clock()`; the RTL has no
  TV system in it; a PAL game's 32 kHz is DACRATE 1551, 31,995 Hz, step 43,683; with the NTSC clock it would play 2 % slow) rounded as
  `osAiSetFrequency` does (a divider in the RTL would cost more than the whole of the rest of the logic that is not state):
  the hardware raises `MONITOR.PEND` bit 3 (with `MONITOR.CTL` bit 4 that bit is a write of DACRATE, no longer the RDP's SYNC
  FULL), the runtime (`n64rt_ai_hw_event`) reads DACRATE back through `MONITOR.AIRATE` and writes the step to it. A game does
  that once. The hardware's reset step is 32 kHz (43690).
* `MONITOR.CTL` bit 4 is set by the runtime when `ID` has bit 4; with it the tick no longer polls the audio feed
  (`n64rt_audio_poll` returns at once) and the AI interrupt is not raised by `ai_buffers_done`. A core without `MIRLO_AI`
  (or an OWN=0 build, which keeps the replaced libultra and its own queue) feeds the FIFO from the software as before.
* area (`quartus_map` of the closing configuration with `MIRLO_MI`, `MIRLO_PI`, `MIRLO_VI` and the 64-bit levels, with and
  without `MIRLO_AI`): `rcp_ai` is 230 ALUTs, 328 registers and one DSP block (the 17 x 16 multiplier); the whole design
  15,543 -> 15,711 ALMs (+168, 0.9 % of the device), +286 ALUTs, +330 registers, +1 DSP; no fit has been made. The state
  is most of it: two buffers' address and length, DRAM_ADDR, the four samples of the frame pair, the phase, the step, DACRATE,
  the FIFO's word. (Not added: BITRATE and CONTROL's read-back, a divider for the step.)
* measured on the simulator (the real-clock configuration, SM64 / SF64 / Pokemon Stadium, the runtime of this commit with
  the core with and without `MIRLO_AI`): the share of the game CPU's samples (PCPROF) in `n64rt_audio_poll` + `fetch_frame`
  5.11 % -> 0 (SM64), 5.15 % -> 0 (SF64); the monitor's entries a field (EXCSTAT, 30-field windows from the 31st field) 53.3 ->
  49.1 (SM64), 98.8 -> 96.8 (SF64), 85.6 -> 81.6 (Stadium); the share 5.21 % -> 0 in Stadium: the AI's accesses were 2 to 4 entries a field, not the 11 a buffer (SM64 and SF64 hand over
  a buffer of 5 ms to 10 ms: 1 to 2 buffers a field). The words written to the FIFO are, in every run (SM64 160,502 real
  samples, SF64 165,665, Stadium 202,088), exactly what `tools/ai_check.py` makes of the frames the reader fetched (RMS 0, max
  error 0); the software AI's own output is exactly what the same algorithm makes of the buffers the game handed it (SM64:
  160,429 samples, RMS 0, max 0; SF64: 144,076, RMS 0.007, max 1 LSB, one sample of a buffer the game changed after the
  hand-over). The two paths' streams are not equal word for word: the game's own audio is made at the pace of the CPU, which
  the hardware AI changes (SM64's first non-zero words are 0x00020002 on one and 0x00010001 on the other), and a game that
  hands a buffer over before it is filled (Stadium: what the reader fetches differs from what was in the RDRAM at the
  hand-over from the 11th buffer on: it is the filled one, as the software AI's read by time gave) is read by time in both.
* `sim/ai` (`build.sh`, `obj_ai/tb_ai [handler latency us]`) is a stand-alone bench of `rcp_ai.sv`: the status bits, the interrupt at
  a start / at the promotion / not at the end, the third buffer dropped, the reader's lead, and the HVQM2-style driver (no
  silent word inside the music at 1 to 11 ms of handler latency).
* `lang/mips/test/ai_test.c` checks it on the simulator (registers, the queue and its second buffer, status flags, the
  countdown, the interrupt reaching the game's handler and its clear, the FIFO's priming, CONTROL, four steps); the bench's
  `AILOG=<file>` (a SoC built with `MIRLO_AI`) logs the frames the reader fetched, the steps and the words written, and
  `tools/ai_check.py AILOG [--lcg]` runs the software algorithm over the same frames and compares sample for sample.

### The SI and the PIF RAM in hardware

A game that runs its own libultra reaches the SI through the monitor too: the four registers (`SI_DRAM_ADDR`,
`SI_PIF_ADDR_RD64B` / `WR64B`, `SI_STATUS`) and PIF RAM's words are each an address error the runtime emulated (two to
three traps a controller read, many more for an EEPROM block or a game that polls `SI_STATUS`; Star Fox 64 makes twenty
a field). With `MIRLO_SI` (which needs `MIRLO_MI`; `MONITOR.ID` bit 5) the registers, the 64-byte DMA, the PIF RAM and the
interrupt are the hardware's (`rtl/soc/rcp_si.sv`); the PIF's *command interpreter* (the joybus: controller status and
buttons, the accessories' blocks, the EEPROM) stays the runtime's, as the Pocket's controllers and slots are what it
reads and writes:

* the game's `lw` / `sw` to the SI's block, physical `0x0480_0000` (the registers repeat every 0x20 bytes), and to PIF RAM,
  `0x1FC0_07C0`..`07FF` (KSEG0 or KSEG1: what libultra's `__osSiRawReadIo` / `WriteIo` reach), are real bus accesses, as the
  MI's, VI's and AI's (`mips_core`'s `SIW`: two more compares in `e_hwx`, the second of physical bits [28:20] = `0x1FC`
  and [10:6] = `0x1F` only, the window repeating every 2 KiB; MCTL bit 8 gates it as the others). `mirlo_mips.sv` answers the
  registers as the MI's (a store takes two cycles more) and PIF RAM, a block RAM, a cycle later for a read.
* PIF RAM is inside the boot ROM's 4 KiB (`0x1FC0_0000`..): `mips_sys.sv`'s `is_boot` gives way for those 64 bytes
  while MCTL bit 8 is on (`hwx_on`, the core's own flop: no new path from the SoC's domain), so the boot ROM is there for the
  boot and for the bare programs, and the PIF RAM for a game that runs with the pass-through.
* `0x00 DRAM_ADDR` (write: 8-byte aligned, 24 bits, read back; ignored while a DMA is under way, and counted up by it as it
  goes: it reads 64 bytes on after a DMA -- the port's address is that register, no adder in the way), `0x04 RD64B` (write: the
  DMA PIF RAM -> RDRAM, 64 bytes), `0x10 WR64B` (RDRAM -> PIF RAM), `0x18 STATUS` (read {interrupt [12], error [3],
  IO busy [1], DMA busy [0]}; any write clears the interrupt and the error). A start while a DMA is under way is ignored
  and sets the error bit. The other registers read 0.
* the DMA is a fourth master of the Pocket bridge's port (the PI's engine, the AI's reader, the bridge; the SI goes after
  the AI and before the PI's engine, and, as the AI, never between the PI engine's read and write of a word): 16 words,
  ~20 cycles each, `req` held until `ack`. A write to the PIF RAM takes the word the bridge read in the cycle of the
  ack; a read from it writes the RAM's output (no data held in the port's register). The interrupt (MI_INTR's SI, `rcp_mi`'s
  `si_pulse`) rises at the end of each DMA and the write to `SI_STATUS` clears it in the cycle of the store (`si_clr`).
  The CPU's access to PIF RAM stalls while the DMA owns it (`c_busy`: a few hundred cycles at most).
* PIF RAM's byte 63 (the low byte of word 15) is the PIF's status byte: a write leaves it 0, the software PIF's
  `s_pif[63] = 0`; its bit 0, the run flag, is what the PIF acts on: a block written with it (by the DMA or by the CPU) sets
  `joy` (as the software's `s_pif_joy`: it stays set, the PIF repeats the commands at every read of the block).
* the PIF is the runtime, woken by **MONITOR.PEND bit 4** (mask bit 4 of `MONITOR.MASK`; the PEND / KICK / MASK registers are 5
  bits now), and **IO busy** (`STATUS` bit 1) is it working: set at the end of a WR64B that carried the run flag and by an
  RD64B when `joy` is set -- the commands run again before the block is read: that read is deferred (DMA busy, nothing moved)
  until the PIF is done, as the console's SI waits for the PIF; an RD64B that finds the PIF busy waits for that run, and the
  first RD64B after the run a WR64B started does not run the commands again (its replies are there: without this SM64 ran
  every EEPROM command twice) -- and cleared by a write to **`MONITOR.SIDONE`**. A start of either DMA waits for the PIF.
  The runtime (`n64rt_si_hw_event`, PEND bit 4 first in `n64rt_rcp_events`) reads the 16 words through the window, runs
  `pif_run` (the software PIF's interpreter, unchanged) on the copy, writes the words it changed back and writes SIDONE.
* with `MONITOR.ID` bit 5 the runtime (`n64rt_mi_init`) leaves the SI's accesses alone -- what still traps (a load of a byte
  or a half of one of those registers: the pass-through takes words) is read from the hardware -- and unmasks PEND bit 4.
  A core without the bit keeps the software SI and PIF as they were: one loader for both.
* area (`quartus_map` of the closing configuration with `MIRLO_MI`, `MIRLO_PI`, `MIRLO_VI`, `MIRLO_AI` and the 64-bit levels, with
  and without `MIRLO_SI`): `rcp_si` is 96 ALUTs, 40 registers and one M10K (the 16 words); the whole design 15,712 -> 15,835
  ALMs (+123, 0.7 % of the device), +256 ALUTs (the port's multiplexers, the core's compares, `is_boot`), +43 registers, +512
  memory bits. (The estimate moves with how the same logic is written: an earlier version, the address from an adder, was
  15,781.) No fit has been made.
* measured on the simulator (the real-clock configuration, the same `OWN=1` runtime on the core with and without `MIRLO_SI`;
  the monitor's entries a field, EXCSTAT, 30-field windows): Super Mario 64 51.0 -> 32.3 at its title (fields 91..120: `adel`
  340 -> 180, `ades` 780 -> 300; `int` 409 -> 490, the PIF's wake-ups), Star Fox 64 73.0 -> 45.0 (fields 121..150), Pokemon
  Stadium 87.2 -> 83.3 (fields 31..60; the same after it: its SI traffic is in its start). The frames are the same (SM64's title,
  file select, the intro; SF64's logo). The save: with an empty slot 1 SM64 formats the EEPROM (64 blocks written) and the
  512 bytes the Pocket reads back are byte for byte those of the software SI; started again with that file, it reads the 64
  blocks and writes none, and the file comes back unchanged.
* `lang/mips/test/si_test.c` checks it on the simulator (the boot ROM and the PIF RAM sharing their addresses, the registers,
  both DMAs with real RDRAM data at an address 8 bytes aligned, the status flags, the interrupt reaching the game's handler
  and its clear, the error bit, the run flag, IO busy and the deferral of the read, the CPU's read during a DMA, the game's
  own accesses without an entry into the monitor, the PI's engine and the AI running meanwhile); `mon_test`'s `ID` mask now
  allows bit 5. The lockstep bench has a `-GSIW=1` variant (the same 3 M instructions, cycle for cycle, with and without it).

### The monitor's fast path

With the MI, PI, VI, AI and SI in hardware a game that runs its own libultra still enters the monitor some 40 to 70 times a field
(the real-clock configuration): the SP's registers (`SP_STATUS` polls, the DMA registers, `SP_PC`: the runtime passes them
through to the RSP's own at `0x2000_0000`), the DPC's (`0x2000_0040`), PI_DRAM_ADDR / PI_CART_ADDR (the PI engine's copy
starts at PI_WR_LEN), the ticks, and the rare RI register or 64-bit instruction. The full entry saved and restored the 29
registers at 64 bits, the 32 FPRs, FCSR, HI and LO for each, and C decoded the instruction: 810 to 910 CPU cycles an entry on
average, about 400 of them the save and restore.

`exception_entry` (`lang/n64rt/n64rt_exc.S`) now looks first, with `$k0` and `$k1` only and nothing of the game's saved:
cause 4 or 5 (MCTL bits 7:3), code in KSEG0, the instruction a `lw` or `sw` -- not in a delay slot, or in a `jr`'s (libultra's
leaf functions return a register so: `jr $ra` / `lw $v0, SP_STATUS`; the game then goes on at the `jr`'s register as it was
before the slot) -- and the address, the base register read through a table on its number (`$k0` from the monitor RAM, `$k1`
from the context), a register that is a word's copy: `SP_MEM_ADDR`..`SP_SEMAPHORE` and `SP_PC` (the RSP's), a read of
`DPC_START`..`DPC_STATUS`, the PI's address and domain registers (`n64rt_pi_regs`, not `RD_LEN`, `WR_LEN`, `STATUS`), the
RI's (`n64rt_ri_regs`). The access is done there, a load's value written to its register through a second table, ErrorEPC set
past it and Count put back as the full path does (frozen across the monitor); anything else falls through to the full path at
the point it is found to be other, unchanged. It is on only with the MI in hardware (`n64rt_mon_fast_enable` from
`n64rt_mi_init`: without it the MI's line is worked out after every write), and off in a `STATS=1` build or with
`-DN64RT_NO_FASTMON`; in an `RSPDUMP=1` build the SP's DMA and STATUS writes stay on the full path (they mark the dumped
task). `n64rt_mon_ctx` is 4 KiB aligned so that one `lui` reaches all of it.

Measured on the simulator (the real-clock configuration with `MIRLO_SI`; release builds of the same runtime with and without
it; PCPROF from field 120, the runtime's samples being the monitor's time; EXCSTAT for the entries; a DEBUG build for the fast
path's count):

| | entries / field | on the fast path | monitor, cycles an entry | monitor, ms a field |
|---|---|---|---|---|
| Super Mario 64 (title -> Peach's letter) | 40.7 | 68 % | 913 -> 603 | 0.396 -> 0.262 |
| Star Fox 64 (to the title) | 58.3 | 57 % | 870 -> 600 | 0.541 -> 0.373 |
| Pokemon Stadium (intro) | 72.5 | 62 % | 812 -> 550 | 0.628 -> 0.427 |

A fast entry is 150 to 350 CPU cycles (the bounds: the samples of its own code, and those plus the decode the full path's
entries go through first); what is left on the full path is the expensive part -- the ticks, PI DMA starts (with their D-cache
flush), the PIF's commands, the pak's CRC, Stadium's copies from the PSRAM's part of the ROM -- at 900 to 1,300 cycles an entry,
about 200 of them the save and restore. SM64's CPU time a field (outside its idle loop) drops 0.15 to 0.24 ms (8.32 -> 8.08 at
the Mario head, 4.16 -> 4.01 at file select, 5.06 -> 4.88 at the letter). A second tier for the rest (C with only the
caller-saved registers, no FPRs: the runtime's C has no FPU instruction) would save at most a third of that save and restore,
under 0.03 ms a field: not done. The frames are the same scenes; the game's own timing shifts by a field here and there (SM64
shows each of its pictures a field earlier; its head's pose when Start is pressed differs), and Pokemon Stadium's first
retrace with its mode set (field 30) showed RDRAM 0 for one field in the fast build -- the same origin latched in both, the
game's own black / origin order racing the latch -- where the build without it, and the DEBUG build with it, show black.

One check was added to the device fast path: a load of `PI_DRAM_ADDR` or `PI_CART_ADDR` goes to the full path (which masks it;
their stores stay on the fast path). The game's TLB instructions have a fast path of their own (above).

### The RCP's registers as the console has them (the monitor's emulation)

Everything of the RCP that is not a small hardware block is emulated by the monitor level (`n64rt_os.c` `emulate_hw_at`,
`n64rt_io.c` `rcp_read_` / `rcp_write_`); n64-systemtest's `spmem`, `pifram` and `cart_memory` groups (the survey of
`~/n64-tests`) say what the console does, and the monitor now does it where it can without the RTL:

* **Sub-word and 64-bit accesses to the RCP's side of the bus** (registers, DMEM / IMEM, PIF RAM, the cartridge's address space)
  are 32-bit bus cycles whatever the instruction: `lb lbu lh lhu lwl lwr` read the aligned word and take their part of it;
  `sb` and `sh` write the WHOLE word, the register shifted to the byte's or half's lane and zeros under it -- not a masked write:
  `sb` of 0x12345678 at offsets 0, 1, 2, 3 writes 0x78000000, 0x56780000, 0x34567800, 0x12345678, `sh` at 0 and 2 writes
  0x56780000 and 0x12345678; `sd` writes only its upper 32 bits; `ld` is two word reads (the console does not survive it);
  `swl` / `swr` the same way as `sb` / `sh` (not measured); `lwc1` / `swc1` are a `lw` / `sw`. Before, all of these were an
  address error to the game. (Misaligned `lh`, `sh`, `sd` stay the game's address error.)
* **DMEM and IMEM repeat every 8 KiB up to 0x0404_0000** (a word at 0x0403_E000 is DMEM's first); the SP's registers do not
  repeat (a read there is 0).
* **A CPU access to DMEM / IMEM (a DMA through two words of RDRAM, the RSP halted) puts SP_MEM_ADDR and SP_DRAM_ADDR back** as the
  game's own DMA left them, and first waits for a DMA of the game's still under way: a game that DMAs and then reads DMEM from
  the CPU finds its registers. `SP_RD_LEN` / `SP_WR_LEN` read the same after any DMA (the count run out: 0xFF8) but for the skip
  field, which this DMA's is 0 -- the one thing that cannot be put back (a write to a length starts a DMA).
* **The SP's DMA registers are written as they are**, not held back while a DMA runs: the console queues a second DMA behind the
  running one (SP_DMA_FULL), and the RSP's host port does that in hardware (`rsp_top.sv`); a game that fills the queue sees
  SP_DMA_FULL as it would on the console. (An RSP without the queue corrupted the running DMA by such a write; an earlier version
  of the monitor waited for SP_DMA_BUSY first. The monitor's own DMEM access still waits for the game's DMA to end, above.)
* **DPC_CLOCK** (0x0410_0010) ticks: the RDP's 24-bit clock counter, one tick per RCP clock (62.5 MHz), cleared by a DPC_STATUS
  write with bit 9. Some games just need the counter ticking up and others depend on it being either zero or very accurate: it ticks on every
  console but the CIC-6105 ones, and so does this: a 6105 game reads 0.
  It is the CPU's view only, the RSP's own MFC0 of it still reads 0; BUFBUSY, PIPEBUSY and TMEM read 0.
* **The CPU's writes to DPC_START, DPC_END and DPC_STATUS** are forwarded to the RDP's registers (0x2000_0040, 0x2000_0044,
  0x2000_004C: where a read comes from; CURRENT and the clock are read-only). A core with the CPU's DPC write path takes them
  (`mirlo_mips.sv`, `rcp_dpc_view.sv`: START ignored while a start is pending, the status in the console's layout -- bits 3 and 5
  set by END or an unfreeze and cleared by SYNC FULL, 4 and 6 never, 7 always, 8 DMA busy, so a finished list reads 0xA8); one
  without it drops them at the host port, as before. All of that is the RTL's: the monitor adds nothing to what DPC_STATUS reads.
* **The PI's registers**: PI_DRAM_ADDR is 24 bits and PI_CART_ADDR 32, both even (a read gives them masked), a DMA leaves them past
  what it moved (the RDRAM side rounded up to 8, the cartridge side to an even count), PI_RD_LEN reads 0x7F and PI_WR_LEN 0x7F
  after a DMA into the RDRAM (0 before). The fast path takes the stores of the two address registers raw into the table as
  before and sends their loads to the full path, which masks. PI_STATUS' IO-busy and DMA-error bits are the PI hardware's own
  register, which a word read does not trap, and the cartridge's write latch (a store to the ROM that the next read returns) is a
  hardware read too: neither can be done here.
* **The boot information words** are what the IPL3 leaves (osCicId: 0x17D7 / 0x17D9 / nothing), above.

`lang/n64rt/test/rcp_bench.c` (`rcp_bench.mk`) is a game that does each of these and checks the console's answers (40 checks; the
runtime before this stops at its first sub-word store to DMEM; the CPU's DPC writes are counted apart, as only a core with their
path passes them); `tlb_bench.c` does the same for the TLB instructions and times them.

Checked with n64-systemtest (the slices of the survey, one run of each with the runtime before and after, on the same SoC build):
24 entries went from failing to passing and none the other way -- `spmem` SB, SH, SD, SW (out of bounds) and the ten DMA ones
(the SP address after a DMA, a DMA to the end of IMEM / DMEM and past it), `pifram` SB (4 offsets) and SH (2), `cart_memory`
(cart address masking, DMA CART -> RDRAM, its small sizes) and "SP Register Read Access From RSP". On a core with the CPU's DPC write path
and the RSP's DMA queue (the RTL of the own-fid-sys branch, run from a copy) the six `rdp` entries pass as well -- the forward of the
CPU's DPC writes needs it -- and so does the semaphore read. What still fails of those groups: the MI's repeat mode (RTL), the
cartridge's write latch and PI_STATUS bits (the hardware's own word reads), the DMAs with a misaligned PI_DRAM_ADDR; the TLB group
is as before, entry for entry.

### The PI's domain 2 and the cartridge's reads in hardware

With `MIRLO_PI` the engine copied only whole words, word-aligned at both ends, from the ROM. Everything else was the
runtime's copy, about 1,500 game-CPU cycles a DMA: Pokemon Stadium makes half of its PI DMAs from a cartridge address
2 mod 4 (its audio streams 1 KiB at a time from there), every page of its FlashRAM is read by DMA, and Ocarina of Time
reads its SRAM by DMA and writes it with `PI_RD_LEN`, which the runtime did not do at all. A game's own word read of
the FlashRAM's status, or of the ROM (`osCartRomInit`, a game's own check), was a trap. With `MIRLO_PI2` (which needs
`MIRLO_PI`; `MONITOR.ID` bit 6):

* **the engine** (`rtl/soc/rcp_mi.sv`) copies any SDRAM to any SDRAM: `PIDST` is an SDRAM word when its bits [23:21]
  are not 0 (0: the RDRAM's 21-bit word, as before, so a runtime of the old core runs unchanged), so a `PI_RD_LEN` -- the
  RDRAM into the save's memory -- is a copy like any other. Its source may start at a word's second half (`PISRC` bit 24:
  a cartridge address 2 mod 4): a 16-bit carry register keeps the source word's second half for the next word's first,
  the copy reads one word more, at the same ~20 sys cycles a word. The length is any number of bytes (`PILEN` [23:22]:
  the last word's bytes, 0 all four; the port's byte enables). The runtime still takes the four PI registers (the
  asm fast path) and starts the copy at `PI_WR_LEN` / `PI_RD_LEN`'s trap.
* **the length a DMA moves** is the console's: a `PI_WR_LEN` of `len` moves `len + 1` bytes into an RDRAM address that is
  8 bytes aligned when that is at most 126 bytes and all of it in one 2 KiB RDRAM row (the PI's first block, written
  byte by byte), else `len + 1` rounded up to even (every other block is written in 16-bit words); a `PI_RD_LEN` moves
  `(len | 1) + 1` (ares' `pi/dma.cpp`, after the n64-systemtest measurements; `n64rt_io.c`'s `pi_wr_bytes`). The runtime
  had rounded every odd length up.
* **domain 2** (`0x0800_0000`) is the cartridge's save: an SRAM (32 KiB) until the game writes the FlashRAM's command
  register (`0x0801_0000`), then a FlashRAM (128 KiB) for good -- the runtime's detection, no table. Either is the Pocket's
  save slot, `data.json` slot 1 (`LAYOUT_SAVE`, its `size_maximum` now 128 KiB): when the kind is known the runtime sets
  the slot's file size (32 KiB or 128 KiB: what the Pocket writes back; a file that is bigger is never cut down, an EEPROM
  game's stays 512 bytes) and makes what the file did not have 0xFF. A FlashRAM's page buffer and status words are the
  runtime's at `LAYOUT_SAVE_AUX` (`0x4334_0000`, `+0x100`). The engine copies to and from all of it: an SRAM's DMA both
  ways, a FlashRAM's array (read at offset * 2: the ID the runtime gives is an old MX chip's, for which libultra's
  `devAddr` is page * 64), its ID and status words by DMA, its page buffer (`osFlashWriteBuffer`). What the engine leaves
  -- a copy out of the memory, a FlashRAM in a mode that ignores the DMA -- is the runtime's copy, as before.
* **the save, for any game** (no table, nothing keyed by the cartridge's ID or CRC): the runtime sees the first access of the
  game to each kind of memory and gives it its part of the one save slot (128 KiB kept, `data.json` slot 1), whose file
  size -- what the Pocket writes back -- is set then, so a game that never saves leaves no file. A game's own libultra
  decides which: its first command on the PIF's channel 4 (status, read or write of a block) says the cartridge has an
  EEPROM, which answers as a **4 Kbit** one (512 bytes, blocks 0-63); its first read or write of domain 2 says an SRAM
  (32 KiB), until it writes the FlashRAM's command register (128 KiB, for good). What a new file has not is 0xFF (an
  erased memory). Whether the EEPROM is a 4 Kbit chip or a 16 Kbit one is **not** something a game's requests say (the status
  command, the first blocks, are the same), and the games of the older libultra versions compare the type exactly with the 4 Kbit
  one -- Pilotwings 64 prints "EEPROM CHECK FAILED", Super Mario 64, Star Fox 64 and Mario Kart 64 stay black with a 16 Kbit
  answer -- so it is told from outside, by the **save-type database** (next section): a data file on the SD card with the
  cartridge IDs of the 16 Kbit games (Banjo-Tooie, Donkey Kong 64, Perfect Dark, Conker's Bad Fur Day, Mario Party 3 ...), which
  the runtime looks the game's ID up in once at boot; a game not in it, or no file, is a 4 Kbit chip as before. There is no setting
  to override it (the core's "EEPROM size" one was removed): the database decides, and a wrong entry is fixed in the file. A
  build with `EEP16K=1` forces 16 Kbit (for trying a game). The Controller Pak is the
  other slot (2), as before.
* **the game CPU's own words**: `mips_core`'s `PI2W` passes the game's `lw` / `sw` of domain 2 (all but its
  `0x0801_xxxx`: the FlashRAM's command register stays a trap) and of the ROM's first 48 MiB (`0x1000_0000`..`0x12FF_FFFF`:
  what of a ROM is in the SDRAM) to the device port, two more terms in `e_hwx`'s compare (MCTL bit 8 gates them as the
  others). `mirlo_mips.sv`'s `X_CART` has the engine do the access between copies (`io_*`: a read waits for the copy
  under way, as `osPiReadIo` waits for the PI): the ROM's word at 1 MiB + the offset, domain 2's at the save's memory +
  the offset's 17 bits (the window repeats every 128 KiB), or, with **`MONITOR.PI2` bit 0** (the runtime's: a FlashRAM),
  a read is the status word and a write goes nowhere. A ROM placed elsewhere than at 1 MiB (a game file with its ROM0
  block further on) would read wrong: the runtime says so. A byte or half access still traps (the runtime reads the
  same memory). The FlashRAM's command interpreter is the runtime's (`lang/n64rt/n64rt_save.h`: in hardware it would be
  bigger than the rest of this); a command is one monitor entry, its status read none.
* `lang/mips/test/pi2_test.c` checks it on the simulator: copies of 1 to 4,097 bytes from either half of a word against the
  source, byte for byte, and not a byte more; the old `PIDST` / `PILEN`; the RDRAM into the save's memory; the CPU's reads
  of the ROM (KSEG0 and KSEG1, the last word of 48 MiB) and of domain 2 as an SRAM, a read during a copy; the runtime's
  own FlashRAM (`n64rt_save.h`) driven as libultra's `osFlash*` drive it -- ID by DMA, status, chip erase, a page
  written through the page buffer and read back by DMA, a sector erase -- with the engine and the hardware's status
  reads; the game's own accesses with no monitor entry but for the command and a half-word load. `mon_test`'s `ID` mask
  allows bit 6. The lockstep bench's `-GPI2W=1` variant runs the same random program cycle for cycle.
* area (`quartus_map` of the closing configuration -- `MIRLO_MI`, `PI`, `VI`, `AI`, `SI`, `VI_AA` and the 64-bit levels --
  with and without `MIRLO_PI2`): 16,915 -> 17,022 ALMs (+107, 0.6 % of the device), +76 ALUTs, +27 registers, no memory,
  no DSP; `rcp_mi` 115 -> 133 ALUTs, 92 -> 117 registers (the carry, the tail, three bits of `PIDST`). No fit has been made.
* measured on the simulator (the real-clock configuration, the `OWN=1` runtime of this change on the core with `MIRLO_PI2`,
  against the runtime and core before it; 30-field EXCSTAT windows): Pokemon Stadium's PI DMAs by field 420, 923 by the
  engine and 888 copied -> 1,820 by the engine (383 of them from a half word), none copied; its 256 reads of the
  FlashRAM's status (`osFlashReadArray`'s, one a page) are no longer monitor entries (fields 31..60: 83.3 -> 74.9 entries a
  field, `adel` 446 -> 190); the windows after are the same within a few percent (the game's pace is its own). It reaches
  its title and, with Start, the Game Pak Check, every DMA the engine's. Ocarina of Time: its SRAM found (32 KiB, from no
  file), its header written by `PI_RD_LEN` through the engine ("ZELDA" at offset 4: the 32 KiB the Pocket reads back),
  the same title frames as before; started again with that file it reads it and writes nothing, and the file comes
  back unchanged. The new runtime on the core without `MIRLO_PI2` does the same in software (Stadium: the same windows as
  before). Super Mario 64 and Star Fox 64 (EEPROM games, 287 fields each): the same
  frames, byte for byte, and the same monitor entries a window as before.

### The cartridge's EEPROM size and the save-type database

The EEPROM answers as a 4 Kbit chip (512 bytes, blocks 0-63) or as a 16 Kbit one (2,048 bytes, blocks 0-255). Nothing in what
the PIF is asked tells which chip a game was made for (the status command, the first block reads and writes are the same), and
the games of libultra's older versions compare the type with the 4 Kbit one exactly -- a 16 Kbit answer stops Pilotwings 64
("EEPROM CHECK FAILED"), Super Mario 64, Star Fox 64 and Mario Kart 64 -- while the newer ones (Banjo-Tooie, Donkey Kong 64 ...)
take what they are told and save in 512 bytes where they need 2,048. The cartridge's header does carry a game ID, and which IDs
have a 16 Kbit chip is a fact about cartridges, not code: it is a **data file on the SD card**, `n64savedb.txt`, and `n64rt.sys`
has nothing ROM-specific in it, only the generic lookup (`lang/n64rt/n64rt_savedb.c`).

**The file** is `Assets/mirlo/common/n64savedb.txt` (in the repository: `pkg/pocket/Assets/mirlo/common/n64savedb.txt`, 469
lines, about 21 KiB), plain ASCII, one line per cartridge:

```
# comment (to the end of the line)
NB7?   eeprom16k                 # Banjo-Tooie
NK4J   sram      rev=0,1         # Hoshi no Kirby 64 (J), its first two versions
```

* **ID**: the four characters at ROM 0x3B..0x3E (`NBKE`: category `N`, game `BK`, region `E`). A `?` stands for any character,
  usually the region (the 4th): `NB7?` is every region of Banjo-Tooie. The lookup is case-insensitive and **the first matching
  line wins**, so a specific line goes before a wildcard one.
* **Revisions** of a game share the ID (and nearly always the chip), so they share the line. For the rare ID whose revisions
  differ -- Hoshi no Kirby 64 (J) is an SRAM in its versions 0 and 1 and a 16 Kbit EEPROM after -- a line may carry
  `rev=0,1`, the header's version byte (ROM 0x3F) it is for.
* **type**: `eeprom4k` (512 bytes, the default), `eeprom16k` (2,048 bytes), `sram` (32 KiB), `sram96k` (96 KiB: Dezaemon 3D),
  `flash` (128 KiB), `none` (no save in the cartridge). Only the EEPROM size is acted on today (the runtime finds an SRAM or a
  FlashRAM from the game's first access of domain 2): the lookup accepts `sram`, `flash` and `none` and skips a line of any
  other type, `sram96k` included. Fields after the type that are not `rev=` are ignored, so the format can grow.
* Blank lines, comments, tabs, CR LF or LF or CR line ends, no newline at the end, a NUL or garbage: all fine. The lookup is
  bounded by the file's size (at most 64 KiB are read) and cannot fail: the worst a file does is not match, and then the chip is 4 Kbit.
  `lang/n64rt/test/savedb_host.c` checks the cases and 20,000 random files under the address sanitiser.

**Where it comes from**: `tools/build_savedb.py --fetch --ares --micro64 --romdb -o ...` makes it from five public sources --
`mupen64plus.ini` (`SaveType=` per ROM by MD5), libretro's No-Intro dat (the same MD5 to the cartridge ID, and the titles),
ares' list by 3-character ID, and the owner's list, micro-64.com's "Game Save Method List"
(<http://micro-64.com/database/gamesave.shtml>, updated 2020-04-13, fetched 2026-10-09), and a public N64 ROM database of cartridge IDs (the ID database; fetched 2026-10-09:
its retail games are `ID:` lines, first match wins, `_` a wildcard, the six characters being the header's 0x3B..0x3E and the version
byte as two hex digits -- its MD5 lines are homebrew, betas and the ROMs it patches). micro-64's list has titles by save method
and no IDs, so its titles are mapped to IDs through the No-Intro names (exact, or an alias checked against the dat; a title
that maps to nothing is listed commented at the end of the file, not guessed: none is left today). Where the sources
agree -- nearly always -- there is one row; where micro-64 or the ID database disagrees on a game the others have, the row is what
mupen64plus and ares say and carries `[micro-64.com says X]` / `[<database> says X]` (a row judged by hand, `OVERRIDES` in the script,
says which source it differs from). mupen64plus' explicit `SaveType=None` is a source too (it had been dropped, so that a
three-character row was taken for every region of a game when only the one with a chip was known: Castlevania (USA, Europe), Top
Gear Rally (USA) and Wetrix (USA, Europe) were rows of their Japanese chips). Nothing of a ROM is in the file. It has 448 rows:
108 EEPROM 4 Kbit, 29 EEPROM 16 Kbit, 39 SRAM 32 KiB, 1 SRAM 96 KiB, 21 FlashRAM, 250 no save (`none` and an absent game both
answer a 4 Kbit chip, so the extra rows change nothing at run time except Castlevania's). It covers the 420 games the
ID database lists. The disagreements, for the owner: against micro-64, GT 64 Championship Edition (16 Kbit by mupen64plus, ares and
the ID database, 4 Kbit by micro-64), Kirby 64 (16 Kbit; the Japanese versions 0 and 1 are an SRAM -- mupen64plus, ares and the ID database; micro-64
says 4 Kbit), Waialae Country Club (SRAM by ares and the ID database, 4 Kbit by micro-64), Premier Manager 64 (SRAM by ares and the ID database, 4 Kbit
by micro-64); against the ID database, Aidyn Chronicles (no chip by mupen64plus and ares, SRAM by the ID database), Hybrid Heaven (J) (SRAM by
mupen64plus and ares, none by the ID database), Monaco Grand Prix (USA, Europe) and Rally Challenge 2000 (4 Kbit by mupen64plus and ares,
none by the ID database), Donald Duck: Goin' Quackers (none by mupen64plus and the ID database, 4 Kbit by ares: the row is none), Space Dynamites
(J) (4 Kbit by ares and the ID database, none by mupen64plus: the row is 4 Kbit).
Banjo-Kazooie is **4 Kbit** by all four (and by its decompilation: three saves of 15 blocks and the global block at 60, all
in `EEPROM_MAXBLOCKS` = 64 blocks); the 16 Kbit games among the ones tested are Banjo-Tooie and Donkey Kong 64. Of the 14
ROMs tested (1080, Banjo-Kazooie, Banjo-Tooie, Donkey Kong 64, GoldenEye, Mario Kart 64, Majora's Mask, Ocarina of Time,
Pokemon Stadium, Pilotwings 64, Star Fox 64 both revisions, Super Mario 64, Wave Race 64) the file, micro-64 and the ID database agree on every one.
Edit the file by hand to add a game.

**How it gets to the runtime**: `data.json` slot 4, "Save-type database" (`filename` `n64savedb.txt`, `required` false,
`parameters` 0x08 like the runtime's slot 3, `size_maximum` 64 KiB, `address` `0x0710_0000` = SDRAM `0x4310_0000`,
`LAYOUT_SAVEDB`), so the Pocket loads it with the other slots before the program starts when the file is there; without it the
slot's size in the core's table is 0 and the runtime does what it did before (4 Kbit). At boot (`n64rt_io_init`) the runtime reads
the slot's size, the cartridge ID and the version out of the ROM's header, looks the game up, and prints one line
(`save-type database of 9458 bytes: NB7E version 0 is eeprom16k`). The slot numbers that mattered when adding it: they are a
core's `data.json` order with no gap (the Pocket's table of file sizes is indexed by the slot's place), and the code that
names them is `LAYOUT_CPAK_SLOT` (2), the save's slot 1 (`n64rt_io.c`: `save_size`, `n64rt_eeprom_init`), the runtime's 3 and now
`LAYOUT_SAVEDB_SLOT` (4) in `lang/mips/include/layout.h`, and `sim/mips_soc/tb_soc.cpp`'s `PROG_SLOT` (3) and its 8 slots;
the RTL names none of them (the bridge sends a slot's file to the address `data.json` gives). The memory layout and its mark are
unchanged: the database takes the first 64 KiB of the 2 MiB nobody used.

**No setting overrides it.** The core had an "EEPROM size" setting (Auto / 4 Kbit / 16 Kbit, `interact1[4:0]`, `0x1000_0120`); it is
gone, and the database alone decides: **a game that is not in the file, or no file, is a 4 Kbit chip**. A block of 64 or more, which
only a 16 Kbit game is asked for, makes the slot 2,048 bytes whatever the database says (a smaller save file is extended with 0xFF).
For tests and for trying a game there are build-time hooks only: `make EEP16K=1` (`N64RT_EEP16K`) forces 16 Kbit, and
`N64RT_EEP_FORCE=<1|2>` (an expression: 1 the 4 Kbit chip, 2 the 16 Kbit one) stands for a database in a simulation;
`lang/n64rt/test/si_test.mk EEP16K=1` runs the 16 Kbit chip. The bench loads the database with `SLOT4=<file>` (a file of one line
for the game under test does what a card's does). The word's old field, `interact1[4:0]`, is free.

### Controller 1's accessory

Controller 1 has a Transfer Pak in it, a Controller Pak, or nothing: the
core's setting "Controller 1 pak" (`interact.json`, `interact0[4:0]`), read
at every access. The other controllers have none.

| value | setting | what the game finds |
|---|---|---|
| 0 (the default) | None | nothing; the cartridge slot is left alone |
| 1 | Controller Pak | a Controller Pak, 32 KiB |
| 2 | Transfer Pak | a Transfer Pak with the Game Boy cartridge that is in the Pocket's own slot |

Any other value is None, and so is the setting before the Pocket has sent it
(the register resets to 0). The Transfer Pak's slot is only looked at, and
the port only powered, when the setting is 2.

`lang/n64rt/n64rt_pak.c` is the accessory as the controller's joybus sees
it, 32-byte blocks at a 16-bit address; the game's own libultra runs on top
of it (`osPfs*`, `osGbpak*` are the game's code -- the runtime's part is
`__osContRamRead`, `__osContRamWrite`, `__osPfsGetStatus`, `osPfsIsPlug` and
the PIF's commands 2 and 3 for a game whose own controller code runs).

**The cartridge in the slot** (setting 2) needs a bitstream built with the
Verilog macro `MIRLO_GB_CART` (~50 ALMs: `rtl/soc/gb_cart.sv`, the registers
`GB_CART`) and `core.json`'s `hardware.cartridge_adapter` 0, which has the
Pocket power the slot. What the game reads and writes on the Transfer Pak's
cartridge goes to the cartridge's own bus a byte at a time, so its mapper,
its RAM, its battery and its clock are the real ones: nothing is emulated,
no file is involved, and what the N64 game saves to the cartridge is saved
on the cartridge.

- The cartridge is held in reset except while the game has its power on
  (the Transfer Pak's own switch) and while it is looked for: when the core
  starts, when the game turns the power on, and once a second while the
  game asks for the status with the power off. So a cartridge may be put in
  or taken out while the game shows its "insert a Game Pak" screen.
- It is there when its header's checksum and logo are right (the logo by
  its CRC). Until then the port only reads, with /CS high: a cartridge of
  another kind left in the slot -- a GBA one answers /CS on the address
  pins and has its save RAM where a Game Boy cartridge has its data pins --
  never sees a write or a chip select.
- On a bitstream without the port (`GB_CART.ID` reads 0) the game finds a
  Transfer Pak with no cartridge in it.
- A bitstream without the macro leaves the port as Analogue's template
  does: the address and data pins inputs, the strobes driven high, the
  cartridge held in reset. Such a core is better installed with
  `cartridge_adapter` -1 (the slot unpowered).

The port's pins, timing and the states it is safe in are at the top of
`gb_cart.sv`. One access is one cycle of a 0.98 MHz PHI (64 clocks): a read
has /RD low for 0.9 us and takes the data 0.8 us after it; a write has /WR
low for 0.5 us in the cycle's middle, the data there 110 ns before and 127
ns after. The data pins' level translator is turned outward only inside a
write, when /RD is high and has been for 190 ns.

**The Controller Pak** (setting 1):

| `data.json` slot | what | file |
|---|---|---|
| 2 "Controller Pak" | 32 KiB; a new one is formatted, empty | `Saves/mirlo/common/<game>.mpk` -- the N64 game's name, as the Pocket names a save |

The file becomes a file only once the game writes to the pak.

- All of it is in the SDRAM between the ROM and the RDRAM, with the EEPROM
  ("The memory of an N64 game", above): every game has it, whatever RDRAM
  it uses and whatever the size of its ROM.
- The runtime reads a slot's file size from the core's table by the slot's
  number, which is right when the Pocket lists every slot of `data.json` in
  order (unused ones with size 0), as its documentation describes: so the
  slots are numbered 0, 1, 2, 3 with no gap (the Controller Pak is 2 and the
  runtime 3; the removed Game Boy slots were 2 and 3, the Controller Pak 4
  and the runtime 5).

**No Rumble Pak.** It is not supported because the Analogue Pocket's APF and dock give a core no vibration output: the openFPGA core's
interface (`target/pocket/core_top.sv`: video, audio, the bridge, and four controllers' `cont*_key/_joy/_trig` as inputs only) has no
rumble or haptic output, and the documented controller input (https://www.analogue.co/developer/docs/bus-communication) has none either.
`osMotorInit` (the runtime's replacement, or a game's own over the pak) finds no pak that answers as one, so a game never starts a motor.
Only controller 1 has a pak (one setting, one Controller Pak file).

`lang/n64rt/test` runs libultra's own Controller Pak file system and Transfer
Pak functions (a decompilation's `ultralib`, not in this repository) against
all of it on the SoC in simulation: a file made, written, found again after
a restart, read and deleted; a cartridge's header, ROM banks and RAM in the
slot; an empty slot; and what each kind
of accessory answers to the other's functions. `layout_test` there is a game
of its own under the whole runtime, with everything at once: a ROM of 64 MiB
(a file of its own) read where the SDRAM's part ends and at its end, 8 MiB of
RDRAM, the EEPROM and the Controller Pak -- loaded through the bridge with
the slots before the ROM and after it, every byte of the files then looked
for where the map says it is, and the two saves read back through the bridge
and given to a second run. The slot's cartridge in simulation is
`sim/mips_soc/gb_cart_model.sv` (an MBC3 as slow as a mask ROM), which also
watches the port: no two drivers on the data pins, the strobes' widths, the
address and data still around them, nothing but reads at an empty slot.

### The core's settings for the controller

`interact.json` (`pkg/pocket/Cores/tortuga.Mirlo64/`) lists them in this order (the order of the entries is the menu's). The
Pocket writes each one to a field of the two `APF_INTERACT` words (`docs/control.md`, Interact API), and the runtime reads the
words at every poll of the controller (`lang/n64rt/n64rt_pad.h`, `n64rt_pad_map()`, a pure function that `lang/n64rt/test/pad_test.py`
checks on the host against the table below over every combination of settings). The Pocket's own Controls menu still remaps what
each button sends: the runtime only sees the Pocket's key bits.

| menu | id | address (field) | values (the first is the default) |
|---|---|---|---|
| Crop Vertical | 235 | `0x1000_010C` (3: `interact0[9:5]`) | None, 8 lines (**default**), 12 lines (the scan-out: `docs/rdp.md`) |
| Crop Right | 236 | `0x1000_0114` (5: `interact0[19:15]`) | a list: 0 None, 1 1 column (**default**; the field is read as a 2-bit pixel count) |
| Controller 1 pak | 214 | `0x1000_0108` (2: `interact0[4:0]`) | 0 None, 1 Controller Pak, 2 Transfer Pak |
| Stick | 225 | `0x1000_012C` (11: `interact1[19:15]`) | 0 D-pad, 1 L stick, 2 R stick |
| D-pad | 226 | `0x1000_0134` (13: `interact1[23:22]`) | 0 None, 1 D-pad, 2 L stick, 3 R stick |
| Z button | 230 | `0x1000_0124` (9: `interact1[9:5]`) | menu None, L, R, ZL, ZR: values 5, **0**, 2, 3, 4 |
| L button | 234 | `0x1000_0118` (6: `interact0[24:20]`) | menu **None**, L, R, ZL, ZR: values 5, 1, 2, 0, 4 |
| R button | 232 | `0x1000_011C` (7: `interact0[29:25]`) | menu None, L, R, ZL, ZR: values 5, 1, **0**, 3, 4 |
| C buttons | 223 | `0x1000_0128` (10: `interact1[14:10]`) | 0 None, 1 Right stick |
| Start button | 233 | `0x1000_0130` (12: `interact1[21:20]`) | menu None, Start, Select+Start: values 2, **0**, 1 |
| Modifier | 228 | `0x1000_0110` (4: `interact0[14:10]`) | menu None, L, R, ZL, ZR: values **0**, 1, 2, 3, 4 |

(The bold value is the default, except 'L button', whose default is its None (value 5); the other lists' defaults are their value 0, so the fields' reset value, 0, which they hold until the Pocket
has written them, is the default layout for them. The Analogue docs' own sample `interact.json` has `defaultval` 2 on a list's third option, and
none of the docs says the default must be the first; the core's earlier "R" list shipped with `defaultval` 0 on its second option.)

Fields 8 (`0x1000_0120`, `interact1[4:0]`, the old "EEPROM size": the save-type database decides now), 14 (`interact1[24]`, the old
"Show FPS": nothing ever read it) and 15 (`interact1[25]`, the old "Start = Select+Start" check) are free. None of this is RTL: the fields
were already there (`rtl/soc/mirlo_regs.sv` decodes every address 2-15 into its own bits), only `interact.json` and the runtime changed,
so a bitstream built before is unchanged (no fit). **The Pocket's Controls menu** (`input.json`) lists A, B, C-Down, C-Left, C-Right
and C-Up only: Z and R are not there, so L and R reach the core as the Pocket's own L and R (the runtime reads the key word, whatever
the menu says; Start, which is not listed either, has always arrived as it is).

**The button codes** (Z, L, R, Modifier): the code 0 is the setting's own default (so the register's reset value is the default layout),
1 is the Pocket's L, 2 R, 3 ZL (L2), 4 ZR (R2), 5 (or any other) none; the menus show None, L, R, ZL, ZR in that order whatever the
codes. The defaults (the `defaultval` of a list is the option's INDEX in the Analogue docs): **Z on the Pocket's L, the N64's L on nothing (None; the code 5), R on the Pocket's R, no Modifier.** (Until the Pocket writes the fields, the register's reset value 0 reads L as ZL: it is written at start.)
Nothing couples them: a key serves two N64 buttons if you ask for it (L button on L with Z button left on L: both follow the one key).

**Start button**: *Start* (the default) is the Pocket's Start; *Select+Start* is the two together (then neither counts for anything else: Select
is not C-down while Start is held with it); *None*: the Pocket's Start does nothing for the N64's Start.

**Modifier**: the chosen Pocket key (None, L, R, ZL or ZR) turns, while held, X, B, Y, A into C-up, C-down, C-left, C-right where they sit
and the D-pad into half a stick; the key never reaches the game. If it is also the key of the Z, L or R button, that button loses it
(the mapping never sees it).

**C buttons.** The face buttons are always the C buttons: C-left on Y, C-right on X, C-down on Select, C-up through the Modifier
(its key held turns X, B, Y, A into C-up, C-down, C-left, C-right where they sit). "C buttons" = *Right stick* adds the right analog stick
as another source (ORed with them): past 64 of 127 from the centre it is C-left / C-right / C-up / C-down. *None*: the face buttons only.

**Stick** (the N64's stick). *D-pad* (the default): the D-pad keys are the stick at full deflection (half of it with the Modifier held).
*L stick*, *R stick*: that analog stick of the pad (bytes 0-1 / 2-3 of `cont*_joy`, 0x80 the centre, a dead zone of 12, 80 at the end
of the range). **D-pad** (the N64's D-pad): *None* (the default), *D-pad* (the keys), *L stick* or *R stick* (past 64 of 127, the four
directions). The stick bytes are believed for a pad the dock calls type 3 (with analog) and also for **type 2 ("without analog")
unless they are all 0x00 or all 0xFF**, in case the dock reports a pad with a stick so. With L or R stick a type 3 pad gives the stick
(centred: zero), and any other pad gives the stick when it is deflected and, when it is not, the D-pad keys at full deflection -- so the
Pocket's own controls, or a pad whose stick bytes the dock never fills, still move Mario. A stick can drive the stick, the D-pad and the C
buttons at once if asked to. `lang/mips/test/padtest.c` shows on the screen what the dock sends of each pad (below).

**The Switch Online N64 controller through the dock** is not documented: the Analogue docs (the bus: `cont*_key` bits 0-15 the d-pad,
A, B, X, Y, L1, R1, L2, R2, L3, R3, Select, Start; `cont*_joy` four bytes; KEY_TYPE 2 or 3 for a docked pad) say nothing about it, and
Analogue's firmware 2.5 only says it fixed "button mapping issues for Switch Joy-Cons, SNES controllers, and N64 controllers"
(https://www.gadgetreview.com/analogue-pocket-dock-update-nintendo-switch-online-controllers). The core's choice for it was removed (the dock does
not report its C-up, and A and C-right share a key); run `padtest` (below) to see what it sends.

**padtest.** `lang/mips/test/padtest.c` (`make padtest` there: `build/padtest.bin`) is a program of its own that draws, for the four
pads, the key word (its top digit is KEY_TYPE), the four stick bytes and what the N64 game would get of them with the settings as
they are now (and echoes them on the JTAG UART when they change). On a Pocket it goes where the runtime goes: copy it as
`Assets/mirlo/common/n64rt.sys` (keep the real file aside; the core still wants a game file in slot 0, any one: it is ignored),
start the core, press each button and move each stick and write down the key word and the bytes. In the simulation:
`ROM=<any file> PAD=3-60:30000010/808080F0 tb_soc lang/mips/test/build/padtest.bin -c 14 -o <dir> -e 12` (a PPM of the screen).
With `make PADLOG=1` the runtime itself (the game running) prints a line on the JTAG UART when a pad's words change:
`n64rt: pad 0 key 30000010 joy 808080F0 -> buttons 8000 stick 70 0`.

**Controllers 2 to 4 come and go.** Whether the dock has a pad (KEY_TYPE != 0) is looked at at every command the PIF runs --
each poll of a game (`osContStartQuery`, `osContStartReadData`, a raw read of the block, ...) --, so a pad plugged in or switched on
while a game runs answers from the first poll after (its status, its keys), and one unplugged answers `CONT_NO_RESPONSE_ERROR` from the
first poll after, with `osContGetQuery` and `osContGetReadData` agreeing. A game that only calls `osContInit` once, as on the console,
sees what was there then. Controller 1 is always there. The "Controller 1 pak" setting is controller 1's alone. Not emulated: the
status bit the console sets when a pak was pulled. `si_test` (below) checks this with the bench's `PAD2` (a dock pad that is there for a
span of fields).

### The SI: a game's own controller code

Controllers, their accessories and the cartridge's EEPROM are all behind the
SI: four registers, the PIF's 64 bytes and one interrupt. The runtime
replaces libultra's functions for them one by one, as it does the rest --
`osContInit`, `osContStartReadData`, `osEeprom*`, `__osContRamRead` ...,
eighteen of them (`n64rt.h`: `N64RT_SI_FUNCS`, `N64RT_SI_DMA`) -- which takes
finding each in the ROM and doing what each does to libultra's own variables
besides. A port's `make` can leave them to the game instead:

| `make` | what runs |
|---|---|
| (the default) | the runtime's replacements |
| `SI_GAME=1` | the game's own. What it reads and writes of the SI's registers and of PIF RAM traps, and the runtime does it (`n64rt_io.c`): the transfers of the PIF's block, the joybus commands in it -- a controller's status and buttons, the accessory's blocks, the EEPROM's -- and the interrupt, as the message of the game's `osSetEventMesg(OS_EVENT_SI)`. Nothing of it is looked for in the ROM |
| `SI_GAME=2` | the same, but for `__osSiRawStartDma`, replaced where the game's list has it: then nothing traps. A game in which it was not found runs as with 1 |

The threads, the messages, the timers and the interrupts themselves stay the
runtime's, and no gateware is involved: the trap is the one any access to
N64 hardware takes. The emulation is in every build -- a game whose
controller code was not recognised drives the SI itself whatever the
option.

What the game's own code does, it does as on the console: `osContInit` waits
until half a second after the start, older libultras' `osEepromLongRead` and
every `osEepromLongWrite` twelve milliseconds a block. Super Mario 64 shows
its first picture 1.4 s later for it, Mario Kart 64 with an EEPROM never
written 4.8 s later (it writes all of it).

`lang/n64rt/test/si_test.mk` builds libultra's SI code (a decompilation's
`ultralib`, as for `pak_test`) into an N64 program of its own and runs it
under the runtime each way: controllers, a game that polls `SI_STATUS`, the
EEPROM, and `pak_test`'s battery on libultra's own `__osContRamRead` and
`__osContRamWrite`. It ends with what each costs, in the SoC's clocks (a
frame is 1,047,000), warm and with nothing of the path in the caches:

| | replaced | `SI_GAME=2` | `SI_GAME=1` |
|---|---|---|---|
| one register read or written (a trap) | | | 520 / 2,300 |
| a frame's controller read | 820 / 2,000 | 1,750 / 4,000 | 4,000 / 7,800 |
| `osEepromRead`, a block | 120 / 310 | 3,000 / 5,500 | 11,900 / 15,700 |
| `__osContRamRead`, a block | 340 / 860 | 9,900 / 12,900 | 14,200 / 19,100 |
| `osPfsIsPlug` | 120 / 590 | 6,000 / 8,000 | 20,200 / 22,900 |

In a game the path is cold: a trap took 2,000 to 2,800 clocks in the games run
(a build with `STATS=1` times them). Super Mario 64 and Mario Kart 64 make
half a transfer a field -- three traps every other field, 0.3 % of a
field's clocks. Ocarina of Time's own controller thread makes six a field
(the buttons, the controllers' status, the Rumble Pak looked for): there
`SI_GAME=1` takes 5.3 % of the CPU's time more than the replacements and
`SI_GAME=2` 1.7 % (by its idle thread's share, during the N64 logo).

`tools/mirlo_regs.py` generates the register decode (`rtl/soc/mirlo_regs.svh`)
and the firmware's `lang/mips/include/generated/{csr,soc,mem}.h`, with the
accessor names the firmware uses (`mrdp_status_read()`, `timer0_uptime_cycles_read()`, ...).

## A faster game CPU

Two things make the game CPU faster without touching the rest of the SoC,
each behind a Verilog macro; neither is in the default build.

### Larger caches

`MIRLO_CPU_IC_LOG2` and `MIRLO_CPU_DC_LOG2` give the caches' lines as powers
of two (`mips_core`'s `IC_LOG2`, `DC_LOG2`: 9 and 8, the VR4300's 16 KiB and
8 KiB, are the default). They stay direct mapped; a larger one is block RAM
and a few address bits, no logic: 10 and 10 (32 KiB each) take 42 more M10K,
11 and 10 (64 KiB of I-cache: two blocks a bit, a multiplexer behind them)
76 more, of the 100 the device has left.

Software that empties "the" cache walks the VR4300's size with index
operations (the runtime's `flush_cpu_icache`, a game's own `osInvalICache`):
on a larger cache an index operation drops every line that index stands
for, one a cycle, so such code needs no change; a hit operation
(`op[20:18]` 4) drops the one line its address can be in.

With the runtime the game CPU spends much of its time on lines for the
I-cache (the runtime's code and the game's share its 16 KiB). The first four
seconds of two games in `sim/mips_soc`, outside the idle loop, cycles an
instruction at 62.8 MHz:

| | 16 K / 8 K | I 32 K | I 64 K | I 32 K, D 16 K | I 64 K, D 32 K |
|---|---|---|---|---|---|
| Super Mario 64's title screen | 2.14 | 2.00 | 1.87 | 1.91 | 1.71 |
| Star Fox 64's first logos | 1.84 | | 1.61 | | 1.53 |

### A clock of its own

`MIRLO_CPU_CLK=32` puts the game CPU -- `mips_sys`: the core, its caches, its
FPU, the boot ROM -- on a clock of 3/2 the SoC's, 94.248 MHz (the N64's CPU
runs at 93.75), a sixth output of the same PLL. Everything else stays on
`clk_sys`: the SDRAM and its PHY, the PSRAMs, the RSP, the RDP, the
scan-out, the registers -- and `CONFIG_CLOCK_FREQUENCY`, which is `clk_sys`'s
(the timer's `uptime` counts it). The CPU's own Count runs at half its
clock, as the VR4300's; the runtime measures the two against each other as
it starts (`n64rt_os_init`) and arms Compare by that, so one game file runs
on both.

Three things are behind the macro:

* **`mips_core`'s `FAST`** (always on in the SoC, which has the CPU clock; the
  macro `MIRLO_CPU_FAST` that had it alone on the SoC's clock was removed): the same five stages with their longest paths cut. A word of
  either cache is used before its tag is compared -- an instruction of the
  wrong line is dropped in D and fetched again once the line is in, a load
  of the wrong line is not written back and run again from its fetch while
  the line comes -- so nothing a tag says reaches the pipeline's advance. A
  branch is decided at E on forwarded operands instead of at D: the fetch
  follows a guess (a jump's target; a backward branch taken, a forward one
  not; a register jump not) and a wrong one costs two cycles, but a branch
  no longer waits in D for the instruction before it. The instruction is
  decoded as it leaves F; E's forwarding, the ALU's result and M's kind of
  access are chosen by registers set the stage before; `mult` takes two
  cycles more, and the FPU a cycle or two an operation (its operands are
  unpacked one a cycle from registers, an add aligns in a cycle of its
  own). In instructions a cycle it is within 4 % of the core without it
  (lockstep, 40 M instructions of three games: -4 % to +1 %).
* **`mips_core`'s `XWB`**: no write buffer in the core. Every store leaves it
  as it is done, into a FIFO in `cpu_port`.
* **`rtl/soc/cpu_port.sv`**, in `port_n64sys`'s place: the CPU's memory port
  taken to `clk_sys`. The two clocks' edges meet every two cycles of
  `clk_sys` and are never closer than 5.3 ns, and nothing in the crossing is
  asynchronous: each path is from a register straight to a register, taken at
  every edge and checked by the timing analyser at the closest pair of edges
  (both clocks are in one group in `core_constraints.sdc`). What a side
  sends stays until it is answered, or lasts one cycle of `clk_sys` and comes
  with a bit that tells a second sample of it from the next word. The
  stores' FIFO is written on the CPU's clock and read on `clk_sys`, the
  SDRAM's going to the arbiter in runs as the write buffer's did, a device's
  to the device port in its turn; a read is begun only when every store
  before it has gone.

In the simulation above, with a line read taking what it takes at 62.8 MHz,
the title screen's instruction costs 27.6 ns instead of 33.6 and a field's
CPU time goes from 3.65 ms to 3.03; Star Fox 64's from 3.57 to 3.00.

With the caches at 32 KiB each as well, the title screen's field takes
2.64 ms of the CPU.

### The N64's own frequencies

`MIRLO_CLK_N64` sets the PLL to the N64's figures: `clk_sys` at 62.5 MHz (the
RCP's clock) instead of 62.832, its double at 125, the video's clock still
a fifth of it (12.5 MHz, a field of 1,042,710 clocks: 59.940 Hz; a PAL field 1,250,000: 50.000 Hz -- the N64's
NTSC modes are 59.940, V_SYNC 0x20D x H_SYNC 0xC15 on its 48.68 MHz; before
2026-10-09 it was 59.783 Hz; with the other PLL, 62.832 MHz, a field is 60.26 Hz), and with `MIRLO_CPU_CLK=32` the CPU at
93.75 MHz, its Count ticking at the VR4300's 46.875. Every period is
half a percent longer, which the timing can use.

Nothing in the SoC counts on the number but the RTC's second
(`core_top.sv`'s `SYS_HZ`, which follows the macro): the SDRAM's and the
PSRAMs' timings are counts of cycles sized for 15.9 ns and hold at 16.0
(the refresh comes every 7.68 us instead of 7.64, of the 7.8 allowed; the
PSRAMs' clock is still three quarters into the cycle, 12.0 ns instead of
11.9). Software has the frequency as `CONFIG_CLOCK_FREQUENCY`, 62,832,000
in the generated `soc.h` unless given: `CLOCK_HZ=62500000` to
`program.mk`. A game file built for one clock runs on the other with its
`osGetTime` and its timers half a percent off; the fields are the
scan-out's own, and the audio follows the AI's fill. The bench runs
the SDRAM model's clock at 62.5 MHz (`tb_soc.cpp`'s `TB_SYS_HZ`).

### What fits

The device is full: 18,480 ALMs in 1,848 LABs, and the fitter needs a LAB
for every few ALMs it cannot pack. Fits of the clocks and the caches (Quartus
Prime Lite 25.1, the seeds of `projects/mirlo.qsf`; setup in ns, the worst of
the four corners, for the CPU's clock / `clk_sys` / `clk_sys2x`):

| build | seed | ALMs / LABs | M10K / DSP | setup: CPU / `clk_sys` / `clk_sys2x` | hold: CPU / `clk_sys` / `clk_sys2x` | worst of all (4 corners) |
|---|---|---|---|---|---|
| 12 | 17,445 / 1,846 | 275 / 52 | +0.371 / +0.387 / +0.346 | +0.128 / +0.058 / +0.178 | +0.058, closes |
| 5 | 17,505 / 1,834 | 275 / 52 | +0.133 / +0.150 / +0.131 | +0.119 / +0.106 / +0.132 | +0.106, closes |
| 20 | 17,471 / 1,836 | 275 / 52 | +0.123 / +0.413 / +0.457 | +0.150 / +0.090 / +0.189 | +0.090, closes |
| 3 | 17,548 / 1,839 | 275 / 52 | +0.096 / +0.037 / +0.477 | +0.102 / +0.037 / +0.172 | +0.037, closes |
| 41 | 17,450 / 1,847 | 275 / 52 | +0.270 / +0.104 / +0.721 | +0.064 / +0.035 / +0.169 | +0.035, closes |
| 27 | 17,467 / 1,844 | 275 / 52 | +0.142 / +0.127 / +0.919 | +0.151 / +0.056 / +0.153 | +0.056, closes |
| 34 | 17,499 / 1,847 | 275 / 52 | +0.335 / +0.197 / +0.328 | **-0.032** / +0.084 / +0.156 | -0.032 (CPU, fast 0C hold) |
| 9 | 17,488 / 1,842 | 275 / 52 | **-0.078** / +0.268 / +0.346 | +0.107 / +0.031 / +0.160 | -0.078 (CPU setup) |

(The slack is `mirlo.sta.summary` read whole -- 128 entries, the slow and fast models at 85 and 0 C, setup, hold,
recovery, removal and pulse width; `tools/area2/closure.py`. The summary is written in two steps: read before the end
it shows the slow models only.) The pad timing of the SDRAM follows the seed: 34, 5, 41, 27 have the figures of the integ
build that has run on the Pocket (`report_datasheet`: DQ setup 0.35-0.36 ns, `dram_a` clock-to-output 7.90 ns; seed 34 of the
previous build is the reference), 12, 20 and 3 are the other family (DQ setup 0.89-0.92, `dram_a` 7.26 ns, the one of the 62.832 MHz
build). `dram_a` / `dram_ba`: no 176279 warning and Output Register yes, in all eight.
What did it:

* **The synthesis in AREA mode** (`OPTIMIZATION_TECHNIQUE AREA`): the same design maps with ~1,000 fewer LUTs
  (`mirlo_regs` -8 %, the FPU -8 %, the CPU -6 %, `rsp_vu` -3 %, the RDP -2 %). It is global: an instance assignment of
  `OPTIMIZATION_TECHNIQUE` (to an instance or to an entity) and an `altera_attribute` on a module are both ignored by this
  Quartus (checked on the synthesis report), so the CPU cannot be left in SPEED. In AREA the CPU's clock failed by 0.6-1.0 ns on the
  first path of every seed; it was one path family, the FPU register file's bypass (next item).
* **`fpu_n64` (FAST), the bypass by flip-flops**: the select between the two writes still on their way and the RAM's word
  compared two 4-bit indexes in the cycle (`lw_idx == insn[15:12]`, 8 compares ahead of the 32-bit store data, `w_res`, the
  data cache's RAM). They are found a cycle ahead against the instruction M takes at the edge (`insn_e` if it advances, `insn`
  if not: the one the read address takes) and registered (`hx_*`, `hy_*`, `gx_*`, `gy_*`: eight flip-flops). Cycle-exact: the lockstep bench
  (`sim/mips`, FAST, `XWB`, the 64-bit levels) passes nine random FPU ROMs of 40-60,000 instructions with the baseline's cycle counts, and
  fails at once with a bit of it stuck.
* **Physical synthesis at `FAST` effort**: Normal duplicates ~1,500 registers (17,726 ALMs in AREA, seed 34); FAST 17.4-17.5k;
  off (`PHYSICAL_SYNTHESIS_COMBO_LOGIC OFF`) 16,997, but then the CPU's clock fails by 0.6-1.1 ns in AREA, and in SPEED (-190 ALMs)
  the RSP's paths (`MFC2` through the vector registers, `VRSQ` into them) fail by 0.05-0.3 ns on three seeds of four.
* **`PLACEMENT_EFFORT_MULTIPLIER 10`** (was 4): the fits take as long (10.5 minutes with four at a time); not isolated from
  the rest.
* **The tiles' two RAMs in M10K** (`rdp_top`: `tl_attr`, `tl_size`, 8 x 47 and 8 x 48; they were MLAB with a registered
  read, 85 ALMs of memory and 99 read registers): -95 registers, +4 M10K.
* **The bridge's static words** (`core_top`): `to_bridge_s` carried 83 bits and `from_bridge_s` 33 through three
  flip-flops each; only the request, its type and DONE need that. The slot, offset and length are CSRs written before the
  request (the RAM address beside them never had a synchroniser), the file size has one register. -304 registers, -66 ALMs (the
  rest is packing). **No simulation covers `core_top`: it wants a run on the Pocket** (a game from the card, a save written back).

By entity (fitter, seed 34, ALMs): the game CPU with the FPU 4,768 -> 4,561, the RDP 5,119 -> 4,970, the RSP 4,277 -> 4,242,
`n64_video` 1,024 -> 981, `mirlo_regs` 690 -> 657, the APF's pad controller 202 -> 143, the bridge's synchronisers
(`to_bridge_s`) 67 -> 2, the whole design 18,288 -> 17,498.

Tried and left: Quartus's read-during-write bypass of the six small inferred FIFOs (`rdp_mem`'s `rf` and `wf`, `rdp_dpc`,
`port_mrdp`, `n64_video`'s `tq`, `cpu_port`'s `wf`: 285 registers and as many multiplexers) cannot be removed by
`M10K, no_rw_check` (the attribute is not honoured for a read through a registered pointer); as MLAB it goes, but the 140 ALMs of
memory cost more than it saves; with the head valid a cycle later the RDP takes 1.3-1.9 % more cycles on real tasks
(6 % with an instant memory). `ALM_REGISTER_PACKING_EFFORT HIGH`, `PHYSICAL_SYNTHESIS_REGISTER_DUPLICATION` /
`ASYNCHRONOUS_SIGNAL_PIPELINING OFF`, `PHYSICAL_SYNTHESIS_MAP_LOGIC_TO_MEMORY_FOR_AREA`, `AUTO_SHIFT_REGISTER_RECOGNITION ALWAYS`,
`BALANCED` (does not pack) and `PRE_MAPPING_RESYNTHESIS ON` (+740 ALUTs) change nothing useful; `AUTO_RESOURCE_SHARING ON` is -140
ALUTs on its own.
A store FIFO written every cycle (its write enable off the CPU's latest signal) is not safe: the FIFO can hold 16.

What each feature costs in this build (Analysis & Synthesis, the macro left out of the closing set, one at a time; ALUTs and
registers, ~0.7 ALM per ALUT; some bring others with them):

| left out | ALUT | registers | DSP | ~ALMs |
|---|---|---|---|---|
| `VI_AA` (anti-alias, divot, dedither) | -957 | -471 | -3 | ~-650 |
| `VI_NO_DIVOT` (added; the macro was removed from the RTL) | -130 | -100 | | ~-95 |
| `CPU_64ALU` | -1,045 | -457 | | ~-700 |
| `CPU_64MD` | -443 | -300 | | ~-300 |
| `CPU_64SH` | -197 | -74 | | ~-140 |
| `RSP_MULW` | -379 | -138 | -4 | ~-260 |
| `AI` | -254 | -330 | -1 | ~-190 |
| `GB_CART` | -95 | -77 | | ~-65 |
| `SI` | -48 | -43 | | ~-35 |
| `PI2` | ~0 | -24 | | ~-20 |
| `TLB_RAM` | ~0 | -57 | | ~-40 (4 M10K) |
| `MON` (and with it the MI, PI, VI, AI, SI) | -1,296 | -632 | -3 | ~-900 |

`tools/area2/` has the scripts: `fit.sh <tag> <seed> [extra.qsf] [map]` (a snapshot of the tree plus the closing macros,
`DROP=` and `CPUCLK=` to change them), `summ.sh` (ALMs, LABs, slack, the pad check), `fails.sh` (failing endpoints, grouped),
`cmp_entities.py`, `rdp_regress.sh`, `soc_regress.sh`, and `vqm_*.py` (what the post-fit netlist holds, by name).

## The PSRAMs

The Pocket has two PSRAMs beside the SDRAM (cram0, cram1: each two 64 Mbit
CellularRAM dies, address and data on the same sixteen lines). They are
tied off: the two macros that brought them in (below) were removed from the RTL
(2026-10, branch own-cleanrtl; the code is in the history before that commit),
and `rtl/soc/psram.sv` is not instantiated. With them, `psram.sv` drove the two chips alike, as two memories of 4 M words of 32 bits (a
word's upper half on cram0):

* **`MIRLO_Z_PSRAM`**: their first dies are the RDP's depth memory
  ([rdp.md](rdp.md)), in synchronous bursts (fixed latency, 62.8 MHz: BCR
  `0x641F`, written at reset with the refresh register's power-up value).
  The chips' clock is gated and rises three quarters into the system
  clock's cycle, out of a DDR register on the doubled clock; what they put
  out the pads' registers take at the system clock's rising edge a period
  and a quarter after their edge. No burst crosses a chip's row of
  128 words, so WAIT is not needed (it is looked at: a read's data that
  comes with WAIT asserted sets a flag).
* **`MIRLO_ROM64`**: their second dies are 16 MiB a word at a time,
  asynchronous (the chips' power-up mode), for the last 16 MiB of a 64 MiB
  ROM. The Pocket's bridge writes them there as it loads the ROM ("The
  memory of an N64 game", above), and the game CPU reads them at
  `0x5000_0000`; nothing else is kept there.

`0x5100_0000` is the block's state: `0x5052` in the upper half, bit 4 the
chips are set up, bit 5 a read's data came with WAIT asserted, bit 6 the
depth memory is there and can be reached a word at a time at `0x5200_0000`
(`MIRLO_PSRAM_NO_ZTEST` took it out), bit 7 the bridge writes a ROM's end there,
bit 8 the RDP's depth is there, bit 9 a game drew onto its depth image and
the core was without `MIRLO_Z_CDRAW` ([rdp.md](rdp.md)); bits 3-0, written, move a burst's write and read latency a clock either way
(1 each is the datasheet's).

The chips have not been driven by this core on a Pocket yet: the bursts'
timing is the datasheet's, checked against a model of the chips
(`sim/mips_soc`, to half a clock) and by the timing analyser (the pads'
constraints are in `projects/mirlo.sdc`). `lang/mips/test/psram_test.c` is
the check on the hardware: as a game file it writes and reads both dies and
shows the result on the screen -- the upper band the second dies, the lower
one the depth memory, green or red; between them the bursts with each
latency setting. The runtime tries the settings too as it starts and keeps
the first that works (`psram_depth_check`).

## Booting

The boot ROM waits, touching
only registers, for the Pocket to release its reset after loading data slot 0
to `0x4000_0000`, then jumps there -- if the program has the memory layout's
mark ("The memory of an N64 game"); if not, it says so on the screen and
starts nothing. If the reset was already released (a
bitstream loaded over JTAG) or nothing was loaded, it falls back to an
SFL serial boot over the JTAG UART, which `tools/jtag/sfl_term.py` and
`tools/jtag/jtag_run.py` drive.

## Software

- `lang/mips/mips.mk`: the toolchain (any MIPS GCC plus binutils; the flags
  set the byte order and ABI). The game CPU uses `-march=vr4300 -msingle-float`
  under o32; the helper cores use `-march=mips32 -msoft-float`.
- `lang/mips/lib.mk`: picolibc (one configuration, no multilib, integer `printf` by default), compiler-rt's builtins and
  `lib/mirlo_rt.c` (the UART, stdio, delays), built per variant:
  `make -f lib.mk VARIANT=game` and `VARIANT=lite`.
- `lang/mips/program.mk`, `lang/mips/linker/`: a game-CPU program (crt0 with
  an exception vector that saves registers, so timer interrupts work).
- `lang/c/geom`: `make` gives `build/geom.bin`; `lang/c/audio`: `make` gives
  `build/main/audio_fw.h`.
- `lang/c/game`: `make [PROG=...]` gives `build/build.bin`.
- `tools/mips_inits.py`: the boot ROM and geom ROM images for the bitstream.

## Simulation

`sim/mips_soc/build.sh soc` verilates the whole SoC against an SDRAM chip
model. `obj_soc/tb_soc prog.bin -c <Mcycles> -o <dir>` puts the program where
the Pocket would load it, prints the UART, and dumps frames. The bench is
the Pocket's side of the data slots too, with `data.json`'s addresses:
`ROM=<file.z64>` is a game's ROM as a file of its own beside the program,
what is not the SDRAM's as it is goes through the bridge's port (everything,
with `BRIDGE_ALL=1`), `SLOT<n>=<file>` loads a slot's file through it, and
with `SLOTS_OUT=<dir>` the nonvolatile slots are read back through it when
the run ends, as when the core quits (`tb_soc.cpp`'s first lines), and
`INTERACT=<address>:<value>` writes the core's settings (`0x10000108:1`: Controller 1 pak = Controller Pak). `PAD`, `PAD2`, `PAD3`, `PAD4`=`from-to:keys[/joy],...` hold the keys of controller 1-4 over those fields (`30000010`: a dock pad with analog, A held; `/joy`: the four stick bytes), which is how the dock's pads come and go.
`lang/mips/test` holds a CPU self-test and a libc test; the cube demo is
`lang/c/game`. `NOLOAD=1` starts it as a JTAG-loaded bitstream would (the
boot ROM's serial boot).

The bench makes the game CPU's own clock, 3/2 of the SoC's
(`tb_soc.cpp`'s `TB_CPU_N` / `TB_CPU_M`), in the PLL's
phase. Its SDRAM model checks the chip's timing in nanoseconds at the
clock it is given, not in cycles. `CPUSTAT=1 [CPUSTAT_IDLE=lo,hi]` prints
where the game CPU's cycles go, a line a second -- the instructions retired,
the cycles lost to each cache, to devices, to hazards, and a field's CPU time
outside the idle loop whose pc range is given; `CPUTRACE=from,count` the
instructions it retires.

`sim/mips` runs the core alone in lockstep with the reference model
(`GEN="-GFAST=1 -GXWB=1"` for the faster core, `-GIC_LOG2=`, `-GDC_LOG2=` for
the caches). `gen_rand_rom.py` writes random integer programs (a last
argument `k1`: run from KSEG1, every instruction fetched uncached),
`gen_rand_fpu_rom.py` random FPU programs.

## First numbers (RTL simulation)

* Super Mirlo 64's title screen (Goddard's Mario head): 42.4 ms a frame
  (23.5 fps), game-CPU-bound.
* Dhrystone (`lang/c/examples/dhrystone`, `-O3 -fno-inline`): about 0.50
  DMIPS/MHz on the game CPU. Its D-cache writes through, so stores cost
  SDRAM bandwidth; a write-back cache is the first thing to try.
* Area: 17,075 ALMs (92 %).
