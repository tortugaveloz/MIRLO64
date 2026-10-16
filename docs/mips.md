# MIRLO 64: the system

MIRLO 64 is a Nintendo 64 compatible core for the Analogue Pocket's Cyclone V FPGA. This document describes the system
around the RSP and the RDP: the memory map, the CPU, the runtime `n64rt.sys`, the RCP's interface blocks, the saves, the
controllers, the clocks and the fit, and the simulations. The RSP and the RDP have documents of their own
([rsp.md](rsp.md), [rdp.md](rdp.md)); the video output is in [resolution.md](resolution.md) and the register map in
[control.md](control.md).

## Contents

1. [Overview](#overview)
2. [Memory map](#memory-map)
3. [The CPU](#the-cpu)
4. [The runtime](#the-runtime)
5. [The RCP's blocks in hardware](#the-rcps-blocks-in-hardware)
6. [Saves and accessories](#saves-and-accessories)
7. [Controllers and settings](#controllers-and-settings)
8. [Clocks, timing and the fit](#clocks-timing-and-the-fit)
9. [Simulation and tests](#simulation-and-tests)

## Overview

`target/pocket/core_top.sv` has the PLL, the Pocket's bridge and the pads, and instantiates the SoC,
`rtl/soc/mirlo_mips.sv`:

| Block | Files | What it is |
|---|---|---|
| CPU | `rtl/soc/mips_sys.sv`, `rtl/mips/mips_core.sv`, `rtl/mips/fpu_n64.sv` | A MIPS of the VR4300 class, big-endian, with its FPU, caches and a 4 KiB boot ROM, on a 93.75 MHz clock of its own |
| CPU port | `rtl/soc/cpu_port.sv` | The CPU's memory and device accesses, taken to the system clock; the stores' FIFO |
| RSP | `rtl/rsp/` | Runs the game's own microcode; its DMA is a port of the SDRAM; its registers are a device of the CPU's |
| RDP | `rtl/rdp/` | Draws the game's command stream; `rdp_dpc.sv` fetches the commands from the RDRAM, `rtl/soc/rcp_dpc_view.sv` shows DPC_STATUS as the console does |
| MI, PI engine | `rtl/soc/rcp_mi.sv` | The interrupt registers and the line to the CPU; the cartridge's DMA and word accesses |
| VI | `rtl/soc/rcp_vi.sv` | The VI's registers, its line counter and interrupt; what the scan-out takes from them |
| AI | `rtl/soc/rcp_ai.sv` | The AI's registers, its two-buffer queue, the reader and the resampler to the Pocket's 48 kHz |
| SI, PIF RAM | `rtl/soc/rcp_si.sv` | The SI's registers, the 64-byte DMA and the PIF RAM |
| Scan-out | `rtl/soc/n64_video.sv` | Reads the game's frame buffer and sends the VI's picture to the Pocket in a 640 x 240 frame, with the VI's filters |
| Registers | `rtl/soc/mirlo_regs.sv` | The Pocket's interfaces (audio FIFO, bridge commands, controllers, settings), a timer, the monitor's block, the cartridge slot; the map is `tools/mirlo_regs.py` |
| Monitor RAM | `rtl/soc/mon_ram.sv` | 2 KiB of block RAM for the monitor's entry |
| Cartridge slot | `rtl/soc/gb_cart.sv` | A Game Boy cartridge in the Pocket's slot, a byte at a time |
| SDRAM | `rtl/soc/sdr_arb.sv`, `sdr_ctrl.sv`, `sdr_phy.sv` | The arbiter, the controller and the PHY of the Pocket's 64 MiB SDRAM |

The SDRAM is the only memory in use: it holds the runtime, the ROM, the saves and the N64's RDRAM. The arbiter has six
ports: the scan-out, the CPU, the RDP, the RSP's DMA, the Pocket's bridge (which the PI's engine, the AI's reader and the
SI's DMA share while the bridge is idle) and the RDP's command fetch. The Pocket's PSRAMs and SRAM are not used: their
pads are tied off in `core_top.sv`.

A game runs from its own ROM, unmodified, with its own libultra: its exception handler, threads, timers and interrupts
are the game's. The CPU maps KSEG0 and KSEG1 onto the RDRAM, so the game's instructions, data and pointers are the
N64's. What the game reaches of the RCP is either a hardware block (the table above) or a trap into the **monitor
level**, a privilege level of the CPU in which the runtime does the access on the game's behalf.

## Memory map

### The CPU's address space

`mips_core` is built with a flat map (`FLAT`): an address is physical unless it is one of the N64's.

| Address | What |
|---|---|
| `0x4000_0000` - `0x43FF_FFFF` | The SDRAM, 64 MiB, cached. The runtime runs here; its exception vector is `0x4000_0180` |
| `0x8000_0000` + 8 MiB (KSEG0), `0xA000_0000` + 8 MiB (KSEG1) | The N64's RDRAM: SDRAM `0x4380_0000`, cached through KSEG0 and uncached through KSEG1 |
| `0xBFC0_0000`, 4 KiB | The boot ROM (the reset vector) |
| `0x2000_0000` | The RSP's host port: SP_MEM_ADDR `+00`, SP_DRAM_ADDR `+04`, SP_RD_LEN `+08`, SP_WR_LEN `+0C`, SP_STATUS `+10`, SP_DMA_FULL `+14`, SP_DMA_BUSY `+18`, SP_SEMAPHORE `+1C`, SP_PC `+20`; DPC_START `+40`, DPC_END `+44`, DPC_CURRENT `+48`, DPC_STATUS `+4C` |
| `0xF000_0000` | The registers ([control.md](control.md)) |
| `0xFFFF_8000`, 2 KiB | The monitor RAM: a store with `$zero` as its base reaches it |
| `0x0000_0000` - `0x1FFF_FFFF`, `0x5000_0000` - `0x7FFF_FFFF` | Mapped addresses: translated by the tables the runtime fills from the game's TLB entries |

The caches are physically tagged, so the runtime at `0x438x_xxxx` and the game through KSEG0 see one copy of the RDRAM.

An instruction fetched from KSEG0's RDRAM, or from a mapped page, is the game's. Its loads and stores to an address that
is neither RDRAM nor mapped are an address error for the monitor level, with these exceptions, which are bus accesses to
hardware once the runtime has turned the pass-through on (COP0 register 7, bit 8) and only for `lw` and `sw`:

| N64 physical address (through KSEG0 or KSEG1) | Block |
|---|---|
| `0x0430_0000` | MI (the registers repeat through the megabyte) |
| `0x0440_0000` | VI |
| `0x0450_0000` | AI |
| `0x0460_0010` | PI_STATUS |
| `0x0480_0000` | SI |
| `0x1FC0_07C0` - `0x1FC0_07FF` | PIF RAM. It lies inside the boot ROM's 4 KiB: with the pass-through on, those 64 bytes are the PIF RAM's |
| `0x0800_0000` - `0x0FFF_FFFF` | The cartridge's domain 2 (the save), except addresses with bit 16 set, where a FlashRAM has its command register |
| `0x1000_0000` - `0x12FF_FFFF` | The ROM's first 48 MiB |

Code that is not the game's (the runtime) reaches every address directly. A device access to an address nothing answers
reads 0 and is counted in the `CTRL.BUS_ERRORS` register.

### The SDRAM

The layout is the same for every game (`lang/mips/include/layout.h`):

| SDRAM (CPU address) | Size | Contents |
|---|---|---|
| `0x4000_0000` | 1 MiB | The runtime, `n64rt.sys`, and its boot stack |
| `0x4010_0000` | 48 MiB | The ROM, where the Pocket loads it. It is never moved |
| `0x4310_0000` | 64 KiB | The save-type database, `n64savedb.txt` |
| `0x4330_8000` | 32 KiB | The Controller Pak |
| `0x4332_0000` | 128 KiB | The cartridge's save: EEPROM, SRAM or FlashRAM |
| `0x4334_0000` | | A FlashRAM's page buffer (128 bytes) and, at `+0x100`, its status words |
| `0x4370_0000` | 1 MiB | The RDP's hidden coverage plane: the other two coverage bits of each 16-bit word of RDRAM ([rdp.md](rdp.md)) |
| `0x4380_0000` | 8 MiB | The N64's RDRAM |

The ranges between them are not used. The console is an N64 with the Expansion Pak, `osMemSize` 8 MiB, unless the
core's "Expansion Pak" setting is off: then it is 4 MiB and the upper half does not answer the game (see
[The Expansion Pak](#the-expansion-pak)). Every block addresses all 8 MiB. The runtime keeps nothing of its own in the
game's RDRAM.

### The Pocket's bridge and the data slots

The bridge's address 0 is the SDRAM's first byte. `mirlo_mips.sv` decodes a bridge address by its megabyte:

| Bridge address | A write goes to |
|---|---|
| below `0x0310_0000` (49 MiB) | the SDRAM, at that address |
| `0x0310_0000` - `0x040F_FFFF` | nowhere as ROM: a ROM is kept up to its first 48 MiB. (The megabytes `0x38` - `0x3F` of this range reach the SDRAM at the RDRAM's place, which the runtime clears when it starts a game.) |
| anything else | the SDRAM, at the address's low 26 bits |

A read is always the SDRAM's, at the address's low 26 bits. A slot that lies in the saves' part of the SDRAM therefore
has, as its address, its SDRAM offset with bit 26 set (`LAYOUT_SLOT_BRIDGE`), where no ROM's bytes arrive. The slots
(`pkg/pocket/Cores/tortuga.Mirlo64/data.json`):

| Slot | Name | File | Bridge address | CPU address |
|---|---|---|---|---|
| 0 | Game | `.z64` (or `.bin`), required | `0x0010_0000` | `0x4010_0000` |
| 1 | Save | `.sav`, nonvolatile, at most 128 KiB | `0x0732_0000` | `0x4332_0000` |
| 2 | Controller Pak | `.mpk`, nonvolatile, 32 KiB | `0x0730_8000` | `0x4330_8000` |
| 3 | Runtime | `n64rt.sys`, required | `0x0000_0000` | `0x4000_0000` |
| 4 | Save-type database | `n64savedb.txt`, optional, at most 64 KiB | `0x0710_0000` | `0x4310_0000` |

The Pocket loads the slots while the core waits, and reads the nonvolatile ones back into their files when the core
quits: as many bytes as the slot's file size in the core's table. The runtime sets that size (`APF_BRIDGE.FILE_SIZE`)
when the game first uses the memory, so a game that never saves leaves no file. The slots are numbered 0 to 4 with no
gap: the table of file sizes is indexed by a slot's place in `data.json`.

A core and a program each carry the word `0x4D4C0002` (`LAYOUT_MARK`) 8 bytes from their start, at `0xBFC0_0008` and
`0x4000_0008`. The boot ROM starts only a program that has it, and the runtime starts a game only on a core that has
it; otherwise a notice is shown on the screen and the SDRAM is left as the Pocket loaded it, so the saves are written
back unchanged.

## The CPU

`rtl/mips/mips_core.sv` executes the VR4300's instructions with 32-bit addresses: MIPS II, the 64-bit integers by
levels (below), `eret`, `ll` / `sc`, the trap instructions, the cache instruction and COP1. `rtl/soc/mips_sys.sv`
instantiates it with the boot ROM and these parameters:

| Parameter | Value | Meaning |
|---|---|---|
| `FLAT`, `BIG`, `N64WIN` | 1 | The map above; big-endian; the RDRAM window, and the address error for the game's other accesses |
| `EXC_BASE` | `0x4000_0000` | The runtime's vector is `EXC_BASE + 0x180` |
| `TLB_EN` / `TLB_RAM` / `FPG` | 0 / 1 / 1 | No VR4300 TLB; translation tables in block RAM; fetch page registers |
| `MON`, `MI`, `PIW`, `VIW`, `AIW`, `SIW`, `PI2W` | 1 | The monitor level and the pass-through windows |
| `W64`, `W64_ALU`, `W64_SH`, `W64_MD` / `W64_DIV` | 1 / 0 | The 64-bit levels |
| `FAST`, `XWB` | 1 | The shortened paths for the 93.75 MHz clock; the write buffer outside the core |
| `IC_LOG2`, `DC_LOG2` | 10 | Cache lines as powers of two (`MIRLO_CPU_IC_LOG2`, `MIRLO_CPU_DC_LOG2`) |

Memory is word-native: a 32-bit word is its value, and byte *k* of a word is lane 3 - *k*.

### Pipeline

Five stages: F (I-cache), D (decode, register file), E (ALU, branch decision), M (D-cache, the slow path, COP0, COP1,
exceptions) and W (write-back), with forwarding into E from M and W. With `FAST` the longest paths are cut:

- **F**: the I-cache's word enters D before its tag is compared. A word of another line is dropped in D a cycle later,
  the line is read and the instruction is fetched again. Instructions are decoded as they leave F.
- **Branches** are decided at E on forwarded operands. The fetch follows a guess (a jump's target; a backward branch
  taken, a forward one not; a register jump not), and a wrong guess costs two cycles. A delay slot costs nothing.
- **M**: a load takes the D-cache's word before its tag is compared. A load of the wrong line is not written back; the
  line is read and the instruction runs again from its fetch. A load in a delay slot waits one cycle for its tag
  instead.
- E's forwarding selects, the ALU's result kind and M's kind of access are registers set one stage ahead.

Nothing a cache tag says reaches the pipeline's advance. Multiplies and divides run in the background with HI and LO
interlocked: `mult(u)` is one product of the DSP block, in HI and LO two cycles later; `div(u)` is a bit a cycle.

### Caches and memory

Both caches are direct mapped with 32-byte lines and physical tags, and each is 32 KiB (1,024 lines) of block RAM. They
hold the SDRAM only: the runtime's memory and the RDRAM through KSEG0. The D-cache writes through, so memory is always
current for the other masters (the RSP's DMA, the RDP, the PI's engine, the scan-out); a line another master has
overwritten is dropped with a cache instruction, which invalidates its line whatever the operation. Software that
empties a cache walks the VR4300's 16 KiB and 8 KiB with index operations: an index operation drops every line that
the VR4300's index stands for, one a cycle, so such code needs no change.

Uncached, device and mapped data accesses, and `ldc1` / `sdc1`, take the slow path: one word at a time. `ld` and `sd`
to cached memory are two words in two cycles.

Stores leave the core as they are done (`XWB`) and go through a 16-entry FIFO in `cpu_port.sv`, written on the CPU's
clock and read on the system clock. The SDRAM's stores go to the arbiter in runs (a write among another port's reads
costs the bus its turn); a device's store goes to the device port in its turn, and the CPU waits for it. The order is
kept: a read, or a device access, begins only when every store ahead of it has gone. The two clocks come from one PLL
at a ratio of 3/2, so nothing in the crossing is asynchronous: every path is register to register and timed by the
analyser at the closest pair of edges (5.3 ns). A value from the CPU's side stays until it is answered; a value from
the system side lasts one system cycle and carries a bit that tells a second sample from the next word.

### 64-bit integers

The 64 bits are beside the pipeline, not in it. A register's upper half is stored as its difference from the lower
half's sign (`hx`), which is zero for every 32-bit value, and is read from the register file only: an instruction waits
in D while a register it reads is on its way from a 64-bit result. E computes 32 bits; 32-bit code pays nothing.

| Level | In the SoC | What the CPU does |
|---|---|---|
| `W64` | yes | 64-bit registers; `ld`, `sd`, `lwu`, `dmfc0`, `dmtc0`; branches on all 64 bits |
| `W64_ALU` | yes | `dadd(i)(u)`, `dsub(u)`, and `and` / `or` / `xor` / `nor` / `slt(u)` when an operand is not a 32-bit value: E does the lower word, M the upper word with E's carry |
| `W64_SH` | yes | The d-shifts, a byte or a bit a cycle |
| `W64_MD` | yes | `dmult(u)`: four products of the operands' halves on the 32-bit multiply's DSP block, summed a word at a time; HI and LO of 64 bits |
| `W64_DIV` | no | `ddiv(u)` |

What the CPU does not have is a Reserved Instruction, which the runtime does on the real 64-bit registers: `ddiv(u)`,
`ldl` / `ldr` / `sdl` / `sdr`, and the FPU's 64-bit instructions. A trap instruction on a 64-bit operand, `lld` and
`scd` are Reserved Instructions that the runtime does not do.

### The FPU

`fpu_n64.sv` is the VR4300's COP1 in single precision: 32 registers of 32 bits, a double being an even / odd pair. A
double operand is converted to a float and a double result is the float widened back. Results and inputs below the
normals are 0 (as the VR4300 with FCR31's FS bit set), a NaN result is `0x7FBFFFFF`, and the conversions to an integer
saturate. There are no FPU exceptions and no cause or flag bits in FCR31. The moves, single and double arithmetic,
`sqrt`, the conversions between S, D and W, and the compares are implemented, each over several cycles on one datapath
(M waits): a 24 x 24 multiply on a DSP block, divide and square root by 28 restoring iterations. `dmfc1`, `dmtc1` and
the `.l` formats are Reserved Instructions, done by the runtime.

### Address translation

In place of the VR4300's 32-entry TLB the core has two translation tables in block RAM, one for data and one for
fetches (an instruction and what it reaches may share an index), of 512 entries each. An entry stands for one 4 KiB
page, indexed by the page number's low 9 bits (`va[20:12]`), and holds valid, dirty, global and cacheable bits, the
ASID, `va[31:21]` and the physical page `pa[28:12]`. A mapped access or fetch reads its page's entry. An entry that is
valid, of this page and of this ASID (unless global) translates it into the RDRAM; anything else is the TLB's exception
(TLBL, TLBS, or Mod for a store to an entry that is not dirty). The runtime then writes the entry from the game's own
TLB entries and the instruction runs again, so a page costs one exception however much it is touched.

A translation fault that goes to the monitor level leaves BadVAddr and EntryHi as the game has them: its address is
COP0 register 22's. The tables are not the VR4300's TLB -- a page that an entry of the game's maps can miss them at any
time, where a wired entry of the console's TLB never misses -- so a miss must not be seen by code of the game's that
runs from a mapped page, an exception handler about to read BadVAddr among it. The monitor writes BadVAddr and EntryHi
itself for the exception it delivers to the game.

A mapped data access is uncached. A mapped fetch runs through the I-cache when its entry has the cacheable bit: two
**fetch page registers** hold the last two cacheable pages a table read found (virtual page, ASID or global, physical
page). The I-cache is indexed by the virtual address and tagged by the physical one, `{valid, pa[28:12]}`; F delivers
the cache's word and D checks it a cycle later against the register's page. A fetch from a page neither register holds
reads the table and loads the older register. Any write to a table entry empties both registers. Because the index
comes from the virtual address, a frame mapped at a virtual address whose index bits above the page offset differ from
the frame's can sit in the cache under more than one index; once such a page has been entered, a hit-type cache
instruction on the I-cache drops the line under every such index, so code rewritten through one alias is the new code
through all.

### The monitor level

With `MON`, COP0 register 7's bit 1 (`GAME`) divides exceptions in two:

- Those a console would take go to the game's own handler at `0x8000_0180`, with EPC, Cause and Status.EXL as on the
  VR4300, when the instruction is the game's.
- The rest go to the **monitor level**: a device access of the game's, an instruction the core lacks, a translation
  fault, and the monitor's own interrupt (`irq_mon`, which the game cannot mask). The entry sets ERL and ErrorEPC and
  jumps to the runtime's vector; it touches neither EXL nor EPC nor Cause, so it can interrupt the game's handler too.
  `eret` with ERL set returns to ErrorEPC.

Count runs at half the CPU's clock, 46.875 MHz, as the VR4300's. Cause.IP2 is the MI's interrupt line.

### COP0 registers particular to this core

| Register | Bits |
|---|---|
| 7 (MCTL) | `[1]` GAME; `[2]` the monitor's entry was in a delay slot (ErrorEPC is the branch); `[7:3]` the entry's cause: an exception code, or 31 for the monitor's interrupt; `[8]` the pass-through to the RCP's blocks, written only together with bit 9; `[10]` the game's RDRAM is 4 MiB (its accesses to the upper half of the window go to the monitor), written only together with bit 11; `[31:28]` read the 64-bit levels present: ALU, SH, MD, DIV |
| 22 | Read only: the address of the last translation fault the monitor level took |
| 8 (BadVAddr) | Writable by `mtc0` while ERL is set |
| 23 | Write: stores a table entry for EntryHi's page pair and ASID. `[28:12]` the physical page, `[5]` cacheable, `[4]` which page of the pair, `[3]` the table (1: fetches), `[2]` global, `[1]` dirty, `[0]` valid. Read: `0x5442_0001`, with bit 3 set when the last TLB exception was a fetch's |
| 0 (Index) | Bit 31 (P) is writable by `mtc0` while ERL is set |
| 13 (Cause) | BD and ExcCode are writable by `mtc0` while ERL is set |
| 4 (Context) | BadVPN2 is writable by `mtc0` while ERL is set |

While GAME is set, `tlbwi`, `tlbwr`, `tlbr` and `tlbp` are Reserved Instructions for the monitor level. The three
`mtc0` enables let the monitor do what the VR4300 does itself for a `tlbp` that finds nothing and for a TLB exception
it delivers to the game.

## The runtime

`lang/n64rt` builds `n64rt.sys`, the program of data slot 3. It is the loader (`loader/main.c`, `n64rt_boot.c`), the
monitor (`n64rt_exc.S`, `n64rt_os.c`, `n64rt_io.c`, `n64rt_rsp.c`) and the accessories and saves (`n64rt_pak.c`,
`n64rt_save.h`, `n64rt_savedb.c`). Nothing in it belongs to a particular game: no table, no patch list, no address.
What it needs to know comes from the ROM's header and boot code.

`make -C lang/n64rt` gives `build/n64rt.bin`, which is released as `n64rt.sys` (README.md has the whole build). It
must stay under 960 KiB: with its stack it occupies the megabyte below the ROM. Switches of `lang/n64rt/n64rt.mk`:
`DEBUG=1` (a detailed log on the UART), `PADLOG=1` (a line when a pad's words change), `EEP16K=1` (a 16 Kbit EEPROM
whatever the database says), `STATS=1` (counts what the game asks of the runtime; the fast path is off),
`GAME_MEM=<bytes>` (the RDRAM the game sees), and `RSPDUMP=1` and `TLBPROF=1` for the simulation's marks.

### Boot

The boot ROM (`lang/mips/boot`) waits, touching only registers, for the Pocket to release its reset after loading the
slots, checks the layout mark and jumps to `0x4000_0000`. If nothing was loaded it waits for a serial boot over the
JTAG UART (`tools/jtag/sfl_term.py`, `tools/jtag/jtag_run.py`; the project is built with `MIRLO_NO_JTAG_UART`, which
leaves the UART out).

The loader then:

1. Reads slot 0's size. The file must start with `0x80371240`; a file that does not is refused with a plain red screen
   (a byte-swapped ROM is named as such on the UART). A file under 1 MiB that is not a ROM is called as a program.
2. Finds the CIC from the CRC-32 of the boot code (ROM `0x40` - `0xFFF`) and the entry point from the header.
3. Refuses a ROM of more than 48 MiB with a notice on the screen ("ROM exceeds maximum file size of 48 MiB."): the
   SDRAM has no room for it.
4. Takes the TV system from the header's destination code.
5. Clears the RDRAM, sets the RDP's hidden coverage plane to all ones, and copies 1 MiB from ROM `0x1000` to the entry
   address, as IPL3 does.
6. Writes what the CIC's IPL3 leaves behind and the boot information words (below).
7. Probes the CPU: an `ld` that runs or traps, the levels in COP0 register 7, the tables' mark in register 23. The
   runtime takes what the core has from these and from `MONITOR.ID`, not from its own build.
8. Replaces the dividing 64-bit helpers in the boot segment (below).
9. Reads the save slot's file size, looks the cartridge up in the save-type database, initialises the accessory and
   the audio FIFO, and loads DMEM with the ROM's boot code (ROM `0x40` - `0xFFF`, at the same offsets), as the
   console's boot leaves it.
10. Turns the pass-through on, sets up the monitor's wake-ups, the VI's window and the AI's default rate, and enters
    the game at its entry point with Status `0x3400_0000` (CU0, CU1, FR, interrupts off), the stack at the top of the
    RDRAM and MCTL.GAME set.

### CIC and boot words

| Boot code | Entry point | What is left in memory | `osCicId` |
|---|---|---|---|
| 6101, 6102 | the header's | | 0 |
| 6103 | the header's less `0x10_0000` | ROM `0x4C0` - `0x8BF` at RDRAM 0; DMEM's first word `0xFFFF_FFFF`, IMEM's `0x17D7` | `0x17D7` |
| 6105 | the header's | the ROM's word at `0x750` at RDRAM `0x2F_E1C0`; DPC_CLOCK reads 0 | `0x17D9` |
| 6106 | the header's less `0x20_0000` | | 0 |

One PAL boot code has an entry of its own in the table and is handled as a 6102, as is a boot code the table does not
know. Software checks these leftovers, so they are part of a valid console.

The words at `0x8000_0300`: `osTvType` (below), `osRomType` 0, `osRomBase` `0xB000_0000`, `osResetType` 0 (cold),
`osCicId` as above, `osVersion` 0 and `osMemSize` (8 MiB, or 4 MiB without the Expansion Pak).

### The Expansion Pak

The core's "Expansion Pak" setting is a check box, on by default (`INTERACT1` bit 24, which is 0 with the Pak in). The
loader reads it once, as the game starts. Without the Pak the game's memory is 4 MiB: that is what `osMemSize` says,
and the runtime sets MCTL's bit 10, with which the game's own accesses to the upper 4 MiB of the RDRAM's window go to
the monitor and are answered as memory that is not there: a load reads 0 and a store goes nowhere. Software that sizes
the memory itself, a word written and read back a megabyte at a time, finds 4 MiB too. The runtime's own code still
reaches all 8 MiB. `lang/n64rt/test/mem4_test.c` sizes the memory that way and checks both halves, with the Pak in and
out.

### TV system

`osTvType` comes from the header's destination code (ROM `0x3E`): PAL (0) for D, F, H, I, L, P, S, U, W, X and Y; MPAL
(2) for B; NTSC (1) for every other code. The game's libultra derives its VI clock, its first video mode and the audio
divisor from it. The scan-out starts in that system (`VIDEO_FRAMEBUFFER.WIDTH` bit 18) and follows VI_V_SYNC once the
game has written a value that names a system (bit 19, read, is the one in use). Beyond that the runtime needs the
system only for the AI's rate: `osViClock` is 49,656,530 Hz (PAL), 48,681,812 Hz (NTSC) or 48,628,316 Hz (MPAL).

### The monitor

The monitor is entered at the runtime's vector with ERL set. All the game's registers are live, `$k0` and `$k1`
included, so the entry (`exception_entry` in `n64rt_exc.S`) first stores `$k0` in the monitor RAM with `$zero` as the
base and `$k1` in the monitor's context, `n64rt_mon_ctx`, which is 4 KiB aligned so that one `lui` reaches every field.

**The fast path** uses `$k0` and `$k1` only and saves nothing else. It takes an address error (MCTL cause 4 or 5) whose
instruction is in KSEG0, is an `lw` or `sw`, is not in a delay slot or is in the delay slot of a `jr` (libultra's leaf
functions return so), and whose address is one of these registers, each a copy of another word:

| Register | Where the access goes |
|---|---|
| SP_MEM_ADDR to SP_SEMAPHORE, SP_PC | the RSP's host port at `0x2000_0000` |
| DPC_START to DPC_STATUS, loads only | `0x2000_0040` |
| the PI's registers other than PI_RD_LEN, PI_WR_LEN and PI_STATUS; loads of PI_DRAM_ADDR and PI_CART_ADDR excepted | `n64rt_pi_regs` |
| the RI's registers | `n64rt_ri_regs` |

The base register is read, and a load's result written, through two jump tables indexed by the register number.
Anything else falls through to the full path at the point it is found to be different, with nothing of the game's
changed. A second fast path does the game's `tlbr`, `tlbp` and the `tlbwi` that changes no translation, with seven of
the game's registers saved in the context for the work.

**The full path** saves the game's registers at 64 bits, HI and LO, FCSR and the 32 FPU registers, and calls
`n64rt_mon_exception` on a stack of the runtime's:

| MCTL cause | What the monitor does |
|---|---|
| 31, the monitor's interrupt | `n64rt_rcp_events`: the hardware's wake-ups (below) |
| 4, 5 (address error) | `emulate_hw`: the game's access to an RCP register, or to the cartridge |
| 1, 2, 3 (TLB) | `tlb_fault`: the page entered in the tables, or the exception delivered to the game |
| 10 (Reserved Instruction) | `tlb_insn`: the game's TLB instruction; else `emulate_64`: a 64-bit instruction |

Anything else stops the game with a message on the UART.

Count, the game's clock, keeps the time the monitor takes: it is the console's clock, and what the monitor does for the
game takes real time. The exception is an entry while the game has its interrupts off (Status: IE clear or EXL set),
which is where it is when it sets a timer: there Count is put back to its value at the entry, so that an entry the game
did not ask for cannot carry Count past a Compare the game is about to write.

The monitor's interrupt is `MONITOR.PEND & MONITOR.MASK`. The runtime masks in four events:

| PEND bit | Event | What the runtime does |
|---|---|---|
| 1 | VI_WIDTH, VI_V_START or VI_Y_SCALE changed | computes the scan-out's width, window and size |
| 2 | a tick, every 2^18 system clocks (4.2 ms) | keeps its field count; notes the frame buffer's origin |
| 3 | AI_DACRATE was written | computes the resampler's step |
| 4 | the PIF has commands to run | runs them (SI, below) |

### TLB instructions and exceptions

The runtime keeps the game's 32 TLB entries as the registers gave them and does each instruction on the game's COP0
registers as the VR4300 would (`tlb_insn` in `n64rt_os.c`): `tlbwi` writes the entry Index names, `tlbwr` the one
Random names, `tlbr` gives the registers back (G in both EntryLo registers), and `tlbp` searches by VPN2 and ASID and
sets Index's P bit when it finds nothing. A write over an entry that mapped something empties that entry's pages in
the CPU's tables. So the game's own `osMapTLB`, `osUnmapTLB`, `osUnmapTLBAll`, `__osProbeTLB` and
`osVirtualToPhysical` run as they are, wherever the game keeps them, including code it loads or unpacks after boot.

A translation fault is answered from those entries (`tlb_service`). When an entry names the page, is valid and allows
the access, its 4 KiB page is written into the faulting table with the entry's dirty bit, global bit and cache
attribute (cacheable unless EntryLo's C field is 2), and the instruction runs again. Otherwise the exception is the
game's, delivered as the VR4300 does (`tlb_deliver`):

| Case | ExcCode | Vector |
|---|---|---|
| no entry names the page | TLBL (2) for a load or fetch, TLBS (3) for a store | `0x8000_0000` if the game's EXL is clear, else `0x8000_0180` |
| an entry names it with V clear | the same codes | `0x8000_0180` |
| a store to a page whose D bit is clear | Mod (1) | `0x8000_0180` |

The monitor sets Cause.ExcCode and BD, EPC (the branch's, in a delay slot; neither is touched when EXL was set),
Context's BadVPN2 and Status.EXL, writes BadVAddr and EntryHi's page from the fault's address (COP0 register 22), and
returns to the vector. Some software maps its code through the TLB and fills it from a refill handler: that handler
runs as it is, its `tlbwr` is the monitor's, and the access then misses the tables again and is filled from the entry
the handler wrote.

**The entries a `tlbwr` puts out are kept.** Software with a refill handler has more pages in use than the 32 entries
hold, and each `tlbwr` puts a random one out. On the console the page that entry mapped is a refill away, a dozen
instructions; here a refill is several trips through the monitor. So the entry a `tlbwr` replaces is kept by the
runtime (128 of them, by their page pair, 4 KiB pages only) and its pages stay in the CPU's tables: an access to them
goes on as before, and one that misses the tables is filled from the kept entry, neither seen by the game. The game
sees its 32 entries as the VR4300 has them (`tlbr`, `tlbp`, Index, Random).

This is not what the console does: there the page of an entry that went takes a refill from the game's page table as
it is then. The game does not choose which entry a `tlbwr` replaces and cannot count on that refill, but its page table
can change under a kept entry. A kept entry therefore goes, with its pages in the tables, as soon as the game

- looks for the page with a `tlbp` (which finds nothing, as on the console);
- writes an entry of the same page pair, or one that maps one of its frames;
- reads into one of its frames from the cartridge;
- writes three entries in a row that map nothing (a flush: then all of them go).

What is left is software that reuses a frame by copying into it with the CPU while another thread still runs from the
page it was. A runtime built with `NO_TLBKEEP=1` keeps nothing; `lang/n64rt/test/tlbkeep_test.c` is a game with a
refill handler of its own that checks each rule.

A game can map `0x0000_0000` - `0x1FFF_FFFF` and `0x5000_0000` - `0x7FFF_FFFF` onto the RDRAM. `0x2000_0000` -
`0x4FFF_FFFF` holds the SoC's own devices and the SDRAM, where the runtime is, and is not mapped.

### 64-bit instructions and the helpers

`emulate_64` does the Reserved Instructions on the context's 64-bit registers: `ddiv(u)`, the unaligned doubleword
loads and stores, and the FPU's `dmfc1`, `dmtc1`, conversions from `.l` and the round / trunc / ceil / floor / cvt to
`.l` (a 64-bit integer is an FPU register pair; the arithmetic is the FPU's single precision). An entry costs far
more than the instruction it stands for, which matters for code that divides in a loop. A compiler's conversions
between a 64-bit integer and a float are two of the FPU's in a row (`dmtc1` and `cvt.d.l`, or `trunc.l.s` and
`dmfc1`): the second, behind the first or in the delay slot of the `jr ra` behind it, is done on the same entry.

A compiler's 64-bit support routines are such code: `__ull_div`, `__ll_mod` and the others are each a pair of `ld`, one
64-bit instruction and a return. `lang/n64rt/n64rt_llsigs.h` has their instruction words as the IDO 5.3 compiler makes
them (generated by `tools/gen_llsigs.py` from `tools/llsigs/ll.c`). At boot `ll_patch` scans the boot segment and, where
all the words of a routine match, writes a jump to the runtime's 32-bit version over its start. The routines that shift
or multiply are left as they are, since the CPU runs their instruction; the ones that divide (`__ull_div`, `__ull_rem`,
`__ll_div`, `__ll_rem`, `__ll_mod`, `__ull_divremi`) are replaced, and the unsigned division and remainder are done
in 32 bits when both operands fit. A replacement runs with the game's interrupts off and restores Status in the delay
slot of its return, because the CPU sends an interrupt to the game's handler only when the interrupted instruction is
the game's. This is the only change the runtime makes to the game's code, and it is made in the RDRAM's copy.

### RCP registers the runtime emulates

`emulate_hw` (`n64rt_os.c`) decodes the trapped instruction, works the address out from the game's registers and calls
`rcp_read_` / `rcp_write_` (`n64rt_io.c`). The behaviour is the console's:

- **Sub-word and 64-bit accesses** to the RCP's side of the bus are 32-bit bus cycles. `lb`, `lbu`, `lh`, `lhu`, `lwl`
  and `lwr` read the aligned word and take their part. `sb` and `sh` write the whole word, the register shifted to the
  byte's or half's lane with zeros below it: `sb` of `0x12345678` at offsets 0 to 3 writes `0x78000000`, `0x56780000`,
  `0x34567800`, `0x12345678`. `sd` writes its upper 32 bits; `ld` is two word reads; `lwc1` and `swc1` are a word.
  This also serves the blocks in hardware, whose pass-through takes `lw` and `sw` only.
- **SP**: the registers are the RSP's own, at `0x2000_0000`, including the DMA queue (SP_DMA_FULL). DMEM and IMEM
  (`0x0400_0000`, repeated every 8 KiB up to `0x0404_0000`) are reached by a DMA through two words of the RDRAM with
  the RSP halted; the two words, SP_MEM_ADDR and SP_DRAM_ADDR are put back afterwards.
- **DPC**: reads come from the RDP's registers; writes of DPC_START, DPC_END and DPC_STATUS are forwarded to them.
  DPC_CLOCK is a 24-bit counter of system clocks, cleared by DPC_STATUS bit 9; it reads 0 with a CIC-6105 boot code,
  as on those consoles. The busy counters read 0.
- **PI**: PI_DRAM_ADDR is 24 bits and PI_CART_ADDR 32, both even; a DMA leaves them past what it moved; PI_RD_LEN
  reads `0x7F`, and PI_WR_LEN `0x7F` after a DMA into the RDRAM. The other registers read back what was written.
- **RI**: eight registers that read back what was written.
- **What the console does not have** reads as on the console, and a store there goes nowhere. The 64DD's registers
  and ROM (`0x0500_0000` - `0x07FF_FFFF`): nothing answers on the cartridge's bus, and a read gets the address's low
  half twice (`0x0500_0508` reads `0x0508_0508`), which is how software finds that no drive is there. RDRAM past the
  game's memory, below the RCP: a load is 0, as software that sizes the memory itself expects. A page that a TLB entry
  maps onto no memory (libultra maps `0xC000_0000` onto a development board's debug port): the same.
- **Elsewhere**: the unused parts of the bus read 0, and writes to them are ignored.

## The RCP's blocks in hardware

Each block answers the game's `lw` / `sw` directly; the runtime finds the blocks by `MONITOR.ID`'s bits. A store to one
of them is acknowledged two cycles late, so that its effect on the interrupt line is in the CPU's clock by the next
instruction.

| Block | In hardware | In the runtime |
|---|---|---|
| MI | MODE, VERSION, INTR, MASK; the line `(INTR & MASK) != 0` as Cause.IP2 | nothing |
| PI | PI_STATUS; the DMA engine; word accesses to the ROM and to domain 2 | the other registers, the start of a DMA, the FlashRAM's commands |
| VI | all registers, the line counter, the interrupt, the scan-out's base, gamma, black and filters, the TV system | the scan-out's width, window and size |
| AI | all of it | the resampler's step |
| SI | registers, the 64-byte DMA, PIF RAM, the interrupt | the PIF's command interpreter |

### MI

MI_MODE's bits, MI_VERSION (`0x0202_0102`), MI_INTR and MI_MASK with its set / clear pairs are as the console's. SP is
the RSP's interrupt as it is. VI is set by the VI's V_INTR, DP by the RDP's SYNC FULL, AI and SI by their blocks, PI by
the engine. The game clears VI with a write to VI_CURRENT, AI with one to AI_STATUS, SI with one to SI_STATUS, PI with
bit 1 of PI_STATUS and DP with MI_MODE's bit 11, each in the cycle of the store. The runtime can set and clear bits
through `MONITOR.MIRAISE` and `MONITOR.MICLR`; it raises PI for a copy it did itself.

### PI

PI_STATUS reads `{interrupt, 0, 0, DMA busy}`; a write of bit 0 aborts a copy and of bit 1 clears the interrupt.

The **engine** copies between any two places of the SDRAM, a word at a time through the bridge's port (about 20 system
clocks a word): `MONITOR.PIDST` (the destination word), `MONITOR.PISRC` (the source word; bit 24: the copy starts at
the word's second half, for a cartridge address that is 2 mod 4), `MONITOR.PILEN` (words, with the last word's byte
count in bits 23:22; the write starts it). It ends in the PI interrupt.

The game's write of PI_WR_LEN or PI_RD_LEN traps, and the runtime starts the copy (`pi_start`). The length is the
console's: PI_WR_LEN moves `len + 1` bytes when that is at most 126 and inside one 2 KiB row of the RDRAM, else
`len + 1` rounded up to even; PI_RD_LEN moves `(len | 1) + 1`. The engine does the copy when the RDRAM address is word
aligned and the source is even: from the ROM, and to and from the save's memory. The runtime first drops the CPU's
D-cache lines over an RDRAM destination, and when the destination overlaps a frame buffer it waits (50 ms at most) for
the RSP and the RDP to finish, so that the copied data is not drawn over. What the engine cannot do, the runtime copies
with the CPU.

Between copies the engine also does the game's own word accesses to the cartridge: a read of the ROM (the word at
1 MiB plus the offset), and a read or write of domain 2 at the save's memory plus the offset's low 17 bits. With
`MONITOR.PI2` bit 0 set (a FlashRAM), a read of domain 2 is the status word and a write goes nowhere.

### VI

The registers at `0x00` - `0x34` are a block RAM of 16 words and read back what was written, except VI_CURRENT.

- **VI_CURRENT** reads `{line, field}`: the line counts the scan-out's line pulses and is set at the scan-out's frame
  boundary, where the base is latched (line 18 for NTSC, 47 for PAL: VI_V_START's first line in libultra's modes). The
  field bit alternates and reads only when VI_STATUS has SERRATE set. A write clears the VI interrupt.
- **VI_V_INTR**: the interrupt rises once a field when the line counter reaches V_INTR / 2. libultra's value 2 is about
  a millisecond ahead of the latch, which is the time the game's retrace handler has to write VI_ORIGIN.
- **VI_ORIGIN** is the scan-out's base, taken at the frame boundary. A serrated picture (SERRATE with a hi-res WIDTH) is
  sent a field at a time, each from the origin the game wrote last: libultra's retrace handler writes the origin of the
  field VI_CURRENT's bit says, as on the console ([resolution.md](resolution.md)). For a hi-res picture that is not
  serrated, a write of the base plus one buffer row leaves the base as it is.
- **VI_STATUS**: GAMMA, AA_MODE, DIVOT and the dither filter bit go to the scan-out at the latch.
- **VI_H_START** written 0 (`osViBlack`) blanks the picture, and so does an origin beyond `MONITOR.VITHR`, the last
  origin at which a picture of the mode's size fits in the RDRAM.
- **VI_V_SYNC** names the TV system once written: 512 - 575 is 525 lines (NTSC, MPAL and a PAL console's 60 Hz modes),
  576 - 703 is 625 lines (PAL, 50 Hz); another value changes nothing. The change is taken at a frame boundary.

The runtime computes what the scan-out needs -- the rows' pitch, the picture's window in the line and in the field,
and the two scales -- from VI_WIDTH, VI_H_START, VI_V_START, VI_X_SCALE and VI_Y_SCALE when one of them changes
(`n64rt_vi_hw_event`); [resolution.md](resolution.md) has how each picture is shown.

### AI

AI_DRAM_ADDR and AI_LEN queue two buffers: a length written to an idle AI starts it, a second is queued, and a third is
dropped. AI_LEN reads the bytes left of the buffer being played; AI_STATUS reads full (bits 31 and 0), busy (bit 30)
and DMA enabled (bit 25); AI_CONTROL bit 0 enables the DMA. A read of any register but AI_STATUS is AI_LEN. The
interrupt rises when a buffer is written to an idle AI and when the buffer read to its end is replaced by the queued
one; there is none when the queue empties. A write to AI_STATUS clears it.

The reader fetches a frame (a word: left in the high half) from the RDRAM whenever the Pocket's audio FIFO holds fewer
than 256 words (5.3 ms), so AI_LEN and the interrupts lead the heard audio by that. The resampler is linear, with a
phase accumulator in Q16 source frames and one multiplier shared by the two channels:
`out = prev + ((next - prev) * (phase >> 1) >> 15)`. With nothing to play it writes silence for an eighth of a second to
hold the FIFO's lead and then stops; the FIFO is primed with silence when the next sound starts.

libultra's audio manager hands a buffer over once a retrace, when the audio task of the retrace before has ended -- after
the graphics task under way has yielded to it -- and sizes the next by what AI_LEN says is left. A hand-over that comes
some 4 ms late and is followed by one in time leaves the AI full two retraces on: `osAiSetNextBuffer` returns -1 and a
manager that does not look at that loses the buffer, and the music jumps ahead. How late a hand-over can be depends on
how soon the graphics task yields: it yields between two display list commands, not while its microcode waits for the RDP
to make room in its command buffer, so a slow RDP is a late hand-over ([rdp.md](rdp.md): a row's spans are read and
written as runs). `tools/ai_pace.py <AILOG>` gives a log's hand-overs a second a line: how late, the retraces without one,
the buffers of the game's rotation never handed over, and the silence inside the sound.

A write of AI_DACRATE wakes the runtime, which reads it through `MONITOR.AIRATE` and writes back the step,
`rate * 65536 / 48000` with `rate = osViClock / (DACRATE + 1)` rounded as `osAiSetFrequency` does.

### SI and the PIF

SI_DRAM_ADDR (`0x00`), SI_PIF_ADDR_RD64B (`0x04`: PIF RAM to RDRAM), SI_PIF_ADDR_WR64B (`0x10`: RDRAM to PIF RAM) and
SI_STATUS (`0x18`: interrupt bit 12, DMA error bit 3, IO busy bit 1, DMA busy bit 0) are in hardware. The DMA moves 16
words through the bridge's port and ends in the SI interrupt; a start while one runs sets the error bit. PIF RAM is a
block RAM of 16 words that the CPU also reads and writes a word at a time. Its last byte is the PIF's status byte: a
write leaves it 0, and its bit 0 written as 1 marks the block as holding joybus commands.

The PIF itself is the runtime. The hardware raises PEND bit 4 and sets IO busy at the end of a WR64B that carried the
run flag, and at an RD64B of a block that holds commands; that read is deferred until the PIF is done, as the console's
SI waits for the PIF. The runtime (`n64rt_si_hw_event`) reads the 16 words, runs the commands (`pif_run`), writes back
the words that changed and writes `MONITOR.SIDONE`. The commands:

| Channel | Command | Answer |
|---|---|---|
| 0 - 3, a controller that is there | `0x00`, `0xFF` status | a standard controller, with or without a pak |
| | `0x01` read | buttons and stick (next section) |
| | `0x02`, `0x03` read, write a pak block | 32 bytes and the joybus CRC, when controller 1 has a pak |
| 4, the cartridge | `0x00` status | the EEPROM's type |
| | `0x04`, `0x05` read, write | a block of 8 bytes |

A command nothing answers gets the no-response bit. Controller 1 is always there; controllers 2 to 4 are there while the
dock reports a pad, checked at every command, so a pad can be plugged in or removed while a game runs.

### SP and DPC

The RSP and the RDP are the hardware's own ([rsp.md](rsp.md), [rdp.md](rdp.md)). The CPU's side differs from the
console only in the address: the monitor passes the game's accesses to `0x2000_0000`. `rcp_dpc_view.sv` gives
DPC_STATUS the console's layout and ignores a DPC_START written while a start is pending.

## Saves and accessories

### The cartridge's save

One slot (`data.json` slot 1, 128 KiB kept) serves every kind of save. The runtime sees the game's first access to each
kind of memory and sets the slot's file size then; what the file did not have reads `0xFF`, an erased memory.

| Kind | How it is recognised | Size |
|---|---|---|
| EEPROM | the first command on the PIF's channel 4 | 512 bytes (4 Kbit) or 2,048 bytes (16 Kbit) |
| SRAM | the first DMA to or from domain 2 (`0x0800_0000`) | 32 KiB |
| FlashRAM | a write to domain 2's command register (`0x0801_0000`) | 128 KiB |

Domain 2 is an SRAM until the command register is written, and a FlashRAM from then on. The FlashRAM's command
interpreter is `lang/n64rt/n64rt_save.h`: read-array, status and ID modes (the ID is `0x00C2001E`), sector and chip
erase, and page program from a 128-byte buffer the game writes by DMA. Its page buffer and status words are in the
SDRAM beside the save, so that the PI's engine and the hardware's status read reach them.

### The save-type database

Nothing in what the PIF is asked tells a 4 Kbit EEPROM from a 16 Kbit one, and software that expects one type rejects
the other. The size is therefore told from outside, by a text file on the SD card:
`Assets/n64/common/n64savedb.txt` (in the repository, `pkg/pocket/Assets/n64/common/n64savedb.txt`). The Pocket
loads it as slot 4, and the runtime looks the cartridge up once at boot (`lang/n64rt/n64rt_savedb.c`).

```
# comment, to the end of the line
<ID>  <type>  [rev=<n>,<n>...]
```

- **ID**: the four characters at ROM `0x3B` - `0x3E`. A `?` stands for any character. The comparison ignores case, and
  the first matching line wins, so a specific line goes above a wildcard one.
- **type**: `eeprom4k`, `eeprom16k`, `sram`, `flash` or `none`. A line of any other type is skipped.
- **rev=**: the line is only for those values of the header's version byte (ROM `0x3F`).

Only the EEPROM's size is acted on: an SRAM or a FlashRAM is found from the game's accesses. A cartridge that is not in
the file, or no file, is a 4 Kbit EEPROM. A command for block 64 or above makes the slot 2,048 bytes whatever the file
says. No setting overrides the database; a wrong entry is corrected in the file. The lookup is bounded by the file's
size and cannot fail on malformed input. `tools/build_savedb.py` regenerates the file from public lists (mupen64plus,
the libretro No-Intro database, ares, micro-64.com); it contains cartridge IDs and types only.

### Controller 1's accessory

The setting "Controller 1 pak" is read at every access (`lang/n64rt/n64rt_pak.c`); the other controllers have none.
The accessory is what the joybus sees, 32-byte blocks at a 16-bit address, and the game's own `osPfs*` and `osGbpak*`
code runs on top of it. There is no Rumble Pak: the core's interface to the Pocket has no vibration output.

**Controller Pak**: 32 KiB in the SDRAM, `data.json` slot 2, saved as `<game>.mpk`. An image that is not a formatted
pak is formatted empty. The slot gets a file size when the game first writes to the pak.

**Transfer Pak**: its registers are the accessory's own:

| Address | Meaning |
|---|---|
| `0x8000` | the pak's power: `0x84` on, `0xFE` off |
| `0xA000` | which 16 KiB of the cartridge's address space is at `0xC000` |
| `0xB000` | write: the cartridge's power. Read: bit 7 a cartridge is in, bit 0 its power, bit 3 its reset released, bit 2 once after the power was written, bit 6 once after the cartridge changed |
| `0xC000` | the cartridge |

The cartridge is the Game Boy cartridge in the Pocket's own slot, reached a byte at a time through the `GB_CART`
registers and `rtl/soc/gb_cart.sv`. Its mapper, RAM, battery and clock are the real ones: nothing is emulated and no
file is involved. `core.json` has `cartridge_adapter` 0, which has the Pocket power the slot.

- The cartridge is held in reset except while the game has its power on and while it is looked for: when the core
  starts with the setting on, when the game turns the power on, and once a second while the game polls the status with
  the power off. A cartridge can thus be put in or taken out at the game's prompt.
- It counts as present when its header's checksum and logo (by CRC) are right. Until then the port only reads, with
  no chip select and no write (`GB_CART.CTRL`'s TRUST bit), so a cartridge of another kind left in the slot sees
  nothing but its address pins and /RD driven.
- One access is one PHI cycle of 64 system clocks (about 0.98 MHz). A read has /RD low from clock 4 to 60 and takes the
  data at 58. A write has /WR low from clock 16 to 48; the data bank's level translator is turned outward only inside a
  write, when /RD is high, so the translator and the cartridge never drive the data pins together.

## Controllers and settings

The Pocket writes each setting of `pkg/pocket/Cores/tortuga.Mirlo64/interact.json` to a field of the two
`APF_INTERACT` words ([control.md](control.md)); the runtime reads the words at every poll of a controller and maps
the Pocket's keys with `n64rt_pad_map()` (`lang/n64rt/n64rt_pad.h`), a pure function. The two crop settings go to the
scan-out instead.

| Setting | Address | Field | Options (value); the default in bold |
|---|---|---|---|
| Crop Vertical | `0x1000_010C` | `interact0[9:5]` | None (0), **8 lines** (8), 12 lines (12) |
| Crop Right | `0x1000_0114` | `interact0[19:15]` | None (0), **1 column** (1) |
| Expansion Pak | `0x1000_0138` | `interact1[24]` | **On** (0), off (1): a check box; read when the game starts |
| Controller 1 pak | `0x1000_0108` | `interact0[4:0]` | **None** (0), Controller Pak (1), Transfer Pak (2) |
| Stick | `0x1000_012C` | `interact1[19:15]` | **D-pad** (0), L stick (1), R stick (2) |
| D-pad | `0x1000_0134` | `interact1[23:22]` | **None** (0), D-pad (1), L stick (2), R stick (3) |
| Z button | `0x1000_0124` | `interact1[9:5]` | None (5), **L** (0), R (2), ZL (3), ZR (4) |
| L button | `0x1000_0118` | `interact0[24:20]` | **None** (5), L (1), R (2), ZL (0), ZR (4) |
| R button | `0x1000_011C` | `interact0[29:25]` | None (5), L (1), **R** (0), ZL (3), ZR (4) |
| C buttons | `0x1000_0128` | `interact1[14:10]` | **None** (0), Right stick (1) |
| Start button | `0x1000_0130` | `interact1[21:20]` | None (2), **Start** (0), Select+Start (1) |
| Modifier | `0x1000_0110` | `interact0[14:10]` | **None** (0), L (1), R (2), ZL (3), ZR (4) |

The Pocket's own Controls menu (`input.json`) lists A, B, C-Down (Select), C-Left (Y) and C-Right (X); it remaps what
each button sends, and the runtime sees only the resulting key bits.

- **Button codes** (Z, L, R, Modifier): 1 is the Pocket's L, 2 R, 3 ZL, 4 ZR, 5 or any other value none; 0 is the
  setting's own key (L for Z, ZL for the L button, R for R, none for the Modifier). The settings are independent: one
  key may serve two N64 buttons.
- **A and B** are fixed. **C buttons**: C-left is Y, C-right is X, C-down is Select. With "C buttons" set to Right stick
  the right stick adds all four, beyond half its range.
- **Modifier**: while its key is held, X, B, Y and A are C-up, C-down, C-left and C-right, and the D-pad gives half the
  stick's deflection. The key itself never reaches the game.
- **Start**: the Pocket's Start; or Select and Start together, which then count for nothing else; or none.
- **Stick**: with D-pad, the D-pad keys are the stick at full deflection (80). With L stick or R stick, the analog
  stick's two bytes (`0x80` the centre, a dead zone of 12, scaled to 80 at the end of the range). A pad the dock
  reports with analog sticks (type 3) gives the stick alone. A pad reported without (type 2) is believed when its stick
  bytes are not all `0x00` or all `0xFF`, and gives the D-pad keys while its stick is centred. The Pocket's own
  controls give the D-pad keys.
- **D-pad** (the N64's): nothing, the D-pad keys, or a stick beyond half its range as the four directions.

`lang/mips/test/padtest.c` is a program that shows on the screen, for the four pads, the key word, the stick bytes and
the N64 controller they make with the current settings. It is installed in the runtime's place
(`Assets/n64/common/n64rt.sys`) to find out what a pad sends through the dock.

## Clocks, timing and the fit

### Clocks

One PLL (`target/pocket/mf_pllbase`) makes the SoC's clocks from the Pocket's 74.25 MHz reference:

| Clock | Frequency | Use |
|---|---|---|
| `clk_sys` | 62.5 MHz | The SoC: SDRAM controller, RSP, RDP, the RCP's blocks, registers. The N64 RCP's frequency |
| `clk_cpu` | 93.75 MHz | The CPU, its FPU, caches and boot ROM. The VR4300's frequency; 3/2 of `clk_sys` |
| `clk_sys2x` | 125 MHz | The SDRAM's PHY; a second 125 MHz output, shifted by 4 ns, is the SDRAM clock's source |
| `clk_vid` | 12.5 MHz | The video pads: a fifth of `clk_sys`, two clocks a pixel; a second output shifted by 90 degrees goes to the Pocket |

The audio's clocks come from a PLL of their own on the Pocket's second reference (`target/pocket/audio.sv`).

A field of the scan-out is 1,042,710 system clocks for NTSC (59.940 Hz) and 1,250,000 for PAL (50.000 Hz). The SDRAM's
and the cartridge port's timings are counts of system clocks. `CONFIG_CLOCK_FREQUENCY` in the generated
`lang/mips/include/generated/soc.h` is 62,832,000 unless `CLOCK_HZ` is given to `lang/mips/program.mk`; software uses
it for delays and time-outs, which are therefore half a percent off.

`target/pocket/core_constraints.sdc` puts `clk_sys`, `clk_sys2x`, `clk_vid` and `clk_cpu` in one group: every path
between them is timed. The other clocks are asynchronous groups.

### Timing closure

The device is a Cyclone V `5CEBA4F23C8` and the design fills about 95 % of its logic. The RDP, the CPU with its FPU and
the RSP are the three largest blocks, followed by the scan-out and the register block. With the device this full,
whether a compile closes timing depends on the fitter's seed (`SEED` in `projects/mirlo.qsf`); README.md names seeds
that closed for the release. A bitstream is good only if no analysis is negative in any of the four corners (the slow
and fast models at 85 and 0 degrees): setup, hold, recovery, removal and pulse width.

The CPU's 93.75 MHz domain is the most demanding. The `FAST` structure of `mips_core` and `fpu_n64` exists for it:
cache words used ahead of their tags, branches decided at E, decode and forwarding selects registered a stage ahead,
the store FIFO's full flag registered, and the FPU's register bypass selected by flip-flops.

The project's settings that matter for area and timing (`projects/mirlo.qsf`): synthesis optimised for area
(`OPTIMIZATION_TECHNIQUE AREA`), wide multiplexers restructured, small memories recognised at any size, physical
synthesis at its fast effort, and a placement effort multiplier of 10. The fitter's seed is the remaining free variable.

### The fit scripts

`tools/area2/` has the flow for trying seeds side by side:

- `fit.sh <tag> <seed> [extra.qsf|-] [map]` copies `rtl/`, `platform/`, `target/` and `projects/` into `s_<tag>/`, so
  that later edits cannot reach a run in progress, sets the seed, appends a list of Verilog macros and any extra
  assignments, and starts a full compile in the background (`quartus_map` only with `map`). The log is
  `projects_a2_<tag>.compile.log`. `QUARTUS_BIN` names Quartus's `bin` directory. The macros it appends set the
  configuration the bitstream is built with (the caches' sizes among them); `EXTRA_MACROS` adds others.
- `ooc.sh <tag> <unit>` synthesises one unit alone (the CPU, the RDP, the RSP, the scan-out or the registers) with the
  project's settings and the same macros: a minute or two, and the same count for the same source, where the whole
  design's synthesis can move a unit nobody touched from one run to the next.
- `closure.py <output_files dir>...` reads the whole `mirlo.sta.summary` and prints the worst slack of all analyses
  and the setup and hold slack of the CPU's clock, `clk_sys` and `clk_sys2x`.
- `summ.sh <projects dir>...` prints the fit's status and utilisation, the worst slacks, and a check that the SDRAM's
  address pins kept their output registers.
- `fails.sh` groups the failing endpoints of a finished fit; `entity_table.py`, `cmp_entities.py` and `map_table.py`
  tabulate area by entity from the fitter's and the synthesis reports; the `vqm_*.py` scripts inspect a post-fit
  netlist; `rdp_regress.sh` and `soc_regress.sh` run the RDP's and the SoC's simulations.

## Simulation and tests

All benches use Verilator. `VERILATOR` and `VERILATOR_ROOT` name the installation.

### The whole SoC: `sim/mips_soc`

`build.sh soc` verilates the SoC with an SDRAM chip model that checks the chip's timing in nanoseconds, and a model of
a Game Boy cartridge (`gb_cart_model.sv`); it needs the images of `tools/mips_inits.py`. `build.sh sdr` builds a bench
of the SDRAM controller, PHY and arbiter alone. The bench makes `clk_sys` at 62.5 MHz and the CPU's clock at 3/2 of it,
in the PLL's phase.

```
obj_soc/tb_soc <program.bin> [-c Mcycles] [-o dir] [-e every_n_frames]
ROM=<game.z64> obj_soc/tb_soc lang/n64rt/build/n64rt.bin -c 600 -o out
```

The bench is the Pocket's side of the data slots, with `data.json`'s addresses. With `ROM=` the ROM is slot 0 at 1 MiB
and the program is slot 3 at 0, which is how a game runs under the runtime and how a test program takes the runtime's
place. The UART's bytes go to stdout, a `0x04` byte ends the run, and frames are written as PPM files. Its main
switches, all environment variables (the first lines of `tb_soc.cpp` list them):

| Variable | Effect |
|---|---|
| `SLOT<n>=<file>`, `SLOTS_FIRST=1` | a slot's file (1 the save, 2 the Controller Pak, 4 the database) |
| `SLOTS_OUT=<dir>` | at the end, the nonvolatile slots are read back through the bridge, as when the core quits |
| `BRIDGE_ALL=1`, `VERIFY=1` | everything through the bridge's port; memory compared with the files |
| `INTERACT=<address>:<value>,...` | the core's settings |
| `PAD`, `PAD2`, `PAD3`, `PAD4` `=from-to:keys[/joy],...` | a controller's keys over a span of fields |
| `VGA=1` | every frame the pads send, as PPM |
| `CPUSTAT=1`, `CPUTRACE=from,count` | where the CPU's cycles go; the instructions it retires |
| `NOLOAD=1` | nothing loaded: the boot ROM's serial boot |
| `+gbcart=<rom.gb>` | a cartridge in the slot (a plus argument) |

### Programs for the SoC: `lang/mips/test`

`make -C lang/mips/test <name>` builds `build/<name>.bin`; each prints PASS or its failures on the UART. They need the
C library (`make -C lang/mips -f lib.mk VARIANT=game`) and are run as
`ROM=<any file> obj_soc/tb_soc build/<name>.bin -c <Mcycles>`.

| Program | What it checks |
|---|---|
| `cpu_test`, `libc_test` | Loads and stores of every width, the caches, the FPU, the divider; the C library |
| `int64_test` | The 64-bit levels against a table |
| `n64win_test`, `mon_test` | The RDRAM window and its trap; the monitor level beside a game's own handler |
| `tlbram_test`, `tlbmon_test`, `tlbgame_test`, `tlbpg_test` | The translation tables; the game's TLB instructions; TLB exceptions delivered to the game; the fetch page registers |
| `mi_test`, `vi_test`, `ai_test`, `si_test`, `pi2_test` | Each RCP block: registers, interrupts, DMA, and that the game's accesses enter the monitor only where they should |
| `rcpcpu_test` | The CPU's side of the SP and DPC registers: the semaphore, the DMA queue, DPC writes |
| `ram8_test` | Every block above the RDRAM's first 4 MiB; `sim/mips_soc/ram8_check.sh` runs it and checks the audio and the picture |
| `hires_test`, `vwin_test`, `viaa_test` | The scan-out's sizes and windows, and the VI's filters (`sim/vi_filter/viaa_check.py` compares the frame) |
| `rsp_replay` | One recorded RSP task on the whole SoC, timed |
| `padtest` | The controllers on the screen (previous section) |

`tools/ai_check.py` runs the resampler's algorithm over the frames the AI fetched (the bench's `AILOG=<file>`) and
compares the output sample for sample.

### The CPU alone: `sim/mips`

`build.sh` verilates `mips_core` and `fpu_n64` in lockstep with a reference model of the N64's CPU, given as
`N64_MODEL=<dir>`; the model is not in this repository. Both run the same ROM and must agree on every retired
instruction's address, word and written register (64 bits), on every exception and on the FPU's registers.
`GEN="-GFAST=1 -GXWB=1 -GW64_ALU=1 ..."` passes the core's parameters. `gen_rand_rom.py` writes random integer programs
(every instruction on registers full of 64-bit values, through the cache and around it; its mode `nodiv` matches the
SoC's levels) and `gen_rand_fpu_rom.py` random FPU programs.

### Single blocks

- `sim/ai`: `build.sh`, then `obj_ai/tb_ai [handler latency us]`: the AI's queue and interrupt, the third buffer
  dropped, the reader's lead, and a driver that queues its next buffer from the interrupt.
- `sim/gb_cart`: `run.sh <rom.gb>` runs the cartridge port against the cartridge model, and then mutants of the port,
  each of which the model's checks must catch. `lang/n64rt/test/make_gb.py` makes a ROM for it.
- `sim/vi_filter`: `build.sh`, then `obj_vi/tb_vi <seed> <divot> <partial %>`: the scan-out's filters against a model.
- `sim/rsp`, `sim/rdp`, `sim/rdp_dither`: see [rsp.md](rsp.md) and [rdp.md](rdp.md).

### The runtime: `lang/n64rt/test`

| Test | How it runs | What it checks |
|---|---|---|
| `pad_test.py` | on the host (needs gcc) | `n64rt_pad_map()` against an independent table, over every combination of settings |
| `savedb_host.c` | on the host, with the address sanitiser (the command is in its header) | the database's lookup: its cases, the shipped file, random bytes |
| `tlb_insn_host.py` | on the host (needs gcc) | the runtime's TLB instructions against a model of the VR4300's registers, on random operations |
| `rcp_bench.mk` | a ROM the released runtime runs in `sim/mips_soc` | the monitor's RCP emulation against the console's answers |
| `tlb_bench.mk` | the same, with a runtime built with `TLBPROF=1` | the TLB instructions' results and their cost |
| `tlbkeep_test.mk` | a ROM with a refill handler of its own | the entries kept when a `tlbwr` puts them out, and each rule that drops one |
| `mem4_test.mk` | a ROM that sizes the memory itself | the "Expansion Pak" setting: 8 MiB with it, 4 MiB without (`INTERACT=0x10000138:1` in the simulator) |
| `Makefile` (`pak_test`) | a program for `sim/mips_soc`, with `+gbcart=` | libultra's Controller Pak file system and Transfer Pak functions on `n64rt_pak.c` |
| `si_test.mk` | a ROM and a runtime built for it | libultra's controller, EEPROM and pak code, and a pad that comes and goes |

`pak_test` and `si_test` compile libultra's own sources, which are not in this repository: `ULTRALIB=<dir>` names a
checkout. The commands to run each test are in the header of its makefile.
