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
| PSRAM | `rtl/soc/psram.sv` | The controller and the PHY of the Pocket's two PSRAMs, 32 MiB: saves, Controller Paks and a ROM's first 30 MiB |

The SDRAM holds the runtime, coverage, the N64's RDRAM and what a ROM has past its first 30 MiB; the Pocket's two PSRAMs
hold saves and the ROM's first 30 MiB, so reads from that part take nothing of the SDRAM's time (see
[The ROM](#the-rom)). The arbiter has six ports: the scan-out, the CPU, the RDP, the RSP's DMA, the Pocket's bridge (which
the PI's engine, the AI's reader and the SI's DMA share while the bridge is idle) and the RDP's command fetch. The CPU
and the RSP's DMA go first: each waits for the words it asks for (a cache line, a transfer the microcode waits on),
where the RDP and the scan-out ride out a wait in their buffers -- the scan-out goes first too, with them, while its FIFO
is below a quarter full (at every frame's start, and when a long run of the CPU's or the RSP's requests has left it
behind: else a frame's first lines found it empty and the picture came a few dozen lines lower). The Pocket's SRAM is
not used: its pads are tied off in `core_top.sv`.

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
| `0x0000_0000` - `0x1FFF_FFFF`, `0x5000_0000` - `0x7FFF_FFFF`, `0xC000_0000` - `0xEFFF_FFFF` | Mapped addresses: translated by the CPU's TLB, the game's own 32 entries |
| `0x2000_0000` - `0x4FFF_FFFF`, in the game's view | Mapped addresses too (below) |

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
| `0x1000_0000` - `0x13FF_FFFF` | The ROM, up to 64 MiB |

Code that is not the game's (the runtime) reaches every address directly; it reads the ROM through its window at
`0x3000_0000` - `0x33FF_FFFF`, whose byte *o* is the ROM's, as the cartridge's reads have it. A device access to an address nothing answers
reads 0 and is counted in the `CTRL.BUS_ERRORS` register.

### The SDRAM

The layout is the same for every game (`lang/mips/include/layout.h`):

| SDRAM (CPU address) | Size | Contents |
|---|---|---|
| `0x4000_0000` | 1 MiB | The runtime, `n64rt.sys`, and its boot stack |
| `0x4010_0000` | 34 MiB | A ROM's bytes from 30 MiB on (its first 30 MiB are in PSRAM: [The ROM](#the-rom)) |
| `0x4230_0000` | up to 10 MiB | A save state ([Save states](#save-states)) |
| `0x42D0_0000` | up to 132 KiB | The virtual Transfer Pak 1's cartridge RAM, read from and written to the Game Boy game's own `.sav` ([The virtual Transfer Pak](#the-virtual-transfer-pak)) |
| `0x42D5_0000` | up to 132 KiB | The same for the virtual Transfer Pak 2 (controller 2's) |
| `0x42E0_0000` | up to 3 MiB | The virtual Transfer Pak 2's Game Boy ROM |
| `0x42D3_0000` | 4 KiB | The parameter struct of the runtime's open command ([GameShark codes](#gameshark-codes)) |
| `0x42D4_0000` | up to 64 KiB | The game's GameShark codes, a text file |
| `0x4310_0000` | 64 KiB | The save-type database, `n64savedb.txt` |
| `0x4340_0000` | up to 3 MiB | The virtual Transfer Pak 1's Game Boy ROM |
| `0x4370_0000` | 1 MiB | The RDP's hidden coverage plane: the other two coverage bits of each 16-bit word of RDRAM ([rdp.md](rdp.md)) |
| `0x4380_0000` | 8 MiB | The N64's RDRAM |

The ranges between them are not used. The console is an N64 with the Expansion Pak, `osMemSize` 8 MiB, unless the
core's "Expansion Pak" setting is off: then it is 4 MiB and the upper half does not answer the game (see
[The Expansion Pak](#the-expansion-pak)). Every block addresses all 8 MiB. The runtime keeps nothing of its own in the
game's RDRAM.

### The ROM

A ROM of up to 64 MiB is loaded as it is, at the bridge's 1 MiB (data slot 0), and read where the load leaves it:

* **its first 30 MiB in the PSRAMs** (`rtl/soc/psram.sv`): the two chips work as one 32-bit memory. ROM offset *o* is stored at PSRAM byte address `0x0020_0000 + o`.
* **the rest in the SDRAM**, from `0x4010_0000`: the bridge writes 31 MiB to 65 MiB at the address less 30 MiB.
* **the bridge's port** (`port_apf`, `n64_ports.sv`) is the PSRAM's only master: an access with `x_sel` goes to the
  PSRAM instead of the arbiter, and its read data lands where the SDRAM's does -- so the PI's engine, the game CPU's reads
  and the Pocket's writes all take the same path.
* **the PI's engine** (`rcp_mi.sv`): `PISRC` bit 25 says the copy's source is the PSRAM's. The runtime sets it for a copy
  in the first 30 MiB; one from 30 MiB on is the SDRAM's, and one across the boundary is the runtime's own (`n64rt_rom_copy`). `PIDST` bit 25 selects a PSRAM destination for save writes.
* **the game CPU's reads** (`0x1000_0000` - `0x13FF_FFFF`) and **the runtime's window** (`0x3000_0000`) are decoded the
  same way, by the ROM's offset.
* `MONITOR.ID` bit 7 says the core places the ROM so, bit 8 that the chips are set up (their configuration registers
  written with their power-up values when the SDRAM's own power-up wait is over: another core may have left them
  otherwise). The runtime starts no game without them.

The PSRAM's word takes some 11 cycles of the 62.5 MHz clock, about 22 MB/s: several times the console's PI.

### The Pocket's bridge and the data slots

The bridge's address 0 is the SDRAM's first byte. `mirlo_mips.sv` decodes a bridge address by its megabyte:

| Bridge address | A write goes to |
|---|---|
| below `0x0010_0000` (1 MiB) | the SDRAM, at that address |
| `0x0010_0000` - `0x01EF_FFFF` (1 - 31 MiB) | PSRAM, at the bridge address plus 1 MiB: a ROM's first 30 MiB |
| `0x01F0_0000` - `0x040F_FFFF` (31 - 65 MiB) | SDRAM, at the bridge address less 30 MiB: the rest of a ROM |
| `0x0730_0000` - `0x073F_FFFF` | PSRAM save and accessory storage, with the cartridge save remapped to byte offset `0x0014_0000` |
| anything else | the SDRAM, at the address's low 26 bits |

A read of a save or Controller Pak slot is routed to PSRAM. The slots
(`pkg/pocket/Cores/tortuga.Mirlo64/data.json`):

| Slot | Name | File | Bridge address | CPU address |
|---|---|---|---|---|
| 0 | Game | `.z64` (or `.bin`), required | `0x0010_0000` | `0x3000_0000` ROM window |
| 1 | Save | `.sav`, nonvolatile, at most 128 KiB | `0x0732_0000` | `0x3414_0000` |
| 2 | Controller Pak | `.mpk`, nonvolatile, 32 KiB | `0x0730_8000` | `0x3410_8000` |
| 3 | Runtime | `n64rt.sys`, required | `0x0000_0000` | `0x4000_0000` |
| 4 | Save-type database | `n64savedb.txt`, optional, at most 64 KiB | `0x0710_0000` | `0x4310_0000` |
| 5 | Game Boy save file 1 | no file: the runtime opens the Game Boy game's `.sav` by path, writable (the file is at most 128 KiB + 48 bytes) | `0x06D0_0000` | `0x42D0_0000` |
| 6 | Game Boy save file 2 | the same, for controller 2's pak | `0x06D5_0000` | `0x42D5_0000` |
| 7 | Gameshark codes | no file at the start: the runtime opens one by path, read-only, at most 64 KiB | `0x06D4_0000` | `0x42D4_0000` |
| 8 | Virtual Transfer Pak 1 | `.gb`, `.gbc`, optional, user-reloadable, at most 3 MiB | `0x0740_0000` | `0x4340_0000` |
| 9 | Virtual Transfer Pak 2 | the same, for controller 2's pak | `0x06E0_0000` | `0x42E0_0000` |

The Pocket loads the slots while the core waits, and reads the nonvolatile ones back into their files when the core
quits: as many bytes as the slot's file size in the core's table. The runtime sets that size (`APF_BRIDGE.FILE_SIZE`)
when the game first uses the memory, so a game that never saves leaves no file. The slots are numbered 0 to 9 with no
gap: the table of file sizes is indexed by a slot's place in `data.json`.

A core and a program each carry the word `0x4D4C0007` (`LAYOUT_MARK`) 8 bytes from their start, at `0xBFC0_0008` and
`0x4000_0008`. The boot ROM starts only a program that has it, and the runtime starts a game only on a core that has
it; otherwise a notice is shown on the screen and memory is left as the Pocket loaded it, so the saves are written
back unchanged. The same notice, with its own text, refuses a core whose CPU lacks any of the 64-bit levels (below).

## The CPU

`rtl/mips/mips_core.sv` executes the VR4300's instructions with 32-bit addresses: MIPS II, the 64-bit integers by
levels (below), `eret`, `ll` / `sc`, the trap instructions, the cache instruction and COP1. `rtl/soc/mips_sys.sv`
instantiates it with the boot ROM and these parameters. The CPU does the instructions a game uses, the 64-bit
integer ones included (but `lld`, `scd` and the traps on a 64-bit operand, below); the runtime does none of them on its behalf.

| Parameter | Value | Meaning |
|---|---|---|
| `FLAT`, `BIG`, `N64WIN` | 1 | The map above; big-endian; the RDRAM window, and the address error for the game's other accesses |
| `EXC_BASE` | `0x4000_0000` | The runtime's vector is `EXC_BASE + 0x180` |
| `TLB_EN` / `TLB_RAM` | 1 / 0 | The VR4300's TLB, 32 entries in block RAM; no translation tables (`MIRLO_TLB_TABLES`: the tables) |
| `MON`, `MI`, `PIW`, `VIW`, `AIW`, `SIW`, `PI2W` | 1 | The monitor level and the pass-through windows |
| `W64`, `W64_ALU`, `W64_SH`, `W64_MD`, `W64_UA` / `W64_DIV` | 1 / 1 | The 64-bit levels (the SoC has all of them) |
| `FAST`, `XWB` | 1 | The shortened paths for the 93.75 MHz clock; the write buffer outside the core |
| `IC_LOG2`, `DC_LOG2` | 9, 9 | Cache lines as powers of two (`MIRLO_CPU_IC_LOG2`, `MIRLO_CPU_DC_LOG2`) |
| `SNOOP` | 0 | Experimental software-managed D-cache coherence; see `dcache-no-snoop-design.md` |

Memory is word-native: a 32-bit word is its value, and byte *k* of a word is lane 3 - *k*.

### Pipeline

Five stages: F (I-cache), D (decode, register file), E (ALU, branch decision), M (D-cache, the slow path, COP0, COP1,
exceptions) and W (write-back), with forwarding into E from M and W. With `FAST` the longest paths are cut:

- **F**: the I-cache's word enters D before its tag is compared. A word of another line is dropped in D a cycle later,
  the line is read and the instruction is fetched again. Instructions are decoded as they leave F.
- **Branches** are decided at E on forwarded operands. The fetch follows a guess (a jump's target; a backward branch
  taken, a forward one not; a register jump to its register as D reads it, when nothing ahead of it in E or M is to write
  that register -- every function's return, mostly -- and not taken otherwise), and a wrong guess costs two cycles. A delay slot costs nothing. The
  branch's two ways (its target and the instruction after its delay slot) are registered as it leaves E, with whether
  it was taken, and the fetch chooses between them behind the registers.
- **M**: a load takes the D-cache's word before its tag is compared. A load of the wrong line is not written back; the
  line is read and the instruction runs again from its fetch. A load in a delay slot waits one cycle for its tag
  instead.
- E's forwarding selects, the ALU's result kind, whether a shift's amount is a register's and M's kind of access are
  registers set one stage ahead.

Nothing a cache tag says reaches the pipeline's advance. Multiplies and divides run in the background with HI and LO
interlocked: `mult(u)` is one product of the DSP block, in HI and LO two cycles later; `div(u)` and `ddiv(u)` are a bit a cycle
(32 and 64 steps, from the cycle after M completes the instruction: `div(u)` is done as it always was, `ddiv(u)` a little over two
times that; the dividend's magnitude is what the multiplier's registers take from M). HI and LO keep the
upper words of their 64-bit values as they are (`hi_up`, `lo_up`), and `hx` is made as `mfhi` / `mflo` read them; which words of HI and LO are 32-bit values is a flag set as they are written.

### Caches and memory

Both caches are direct mapped with physical tags. The I-cache has 512 lines of 32 bytes (16 KiB); the
D-cache has 512 lines of 16 bytes (8 KiB). They
hold the SDRAM only: the runtime's memory and the RDRAM through KSEG0. The D-cache writes through the CPU store FIFO;
the FIFO orders earlier stores before the device accesses that start DMA. External writes do not snoop the D-cache in
the default build: software manages buffer ownership and invalidates stale destination lines with cache instructions.
Index invalidation covers every line represented by the VR4300's 8 KiB index. A mapped hit invalidation drops every
possible physical page colour. Hit writeback is a no-op because lines are clean; index load tag reports an invalid tag
to writeback-walking software, rather than exposing actual cache tags. Code that uses tags for other purposes is not
fully emulated.

When enabled, the D-cache also drops by itself a line that another master writes into (`SNOOP`). Each 16-byte line is independent of
its neighbours. `mirlo_mips.sv` takes from the arbiter the line of every
write it grants the RDP, the RSP's DMA and the bridge (the PI's engine, the SI's DMA), across to the CPU's clock as
`cpu_port.sv`'s read words go. The core looks each up in a copy of the tags (a valid bit and the nine tag bits above
the index: one M10K, whose read port is the snoop's alone, so a lookup never waits), queues a hit and drops the line
through the tags' write port in a cycle where the core writes no tag; a refill that such a write overtakes brings its
line in invalid. While two or more lines wait, those masters' writes are held at the arbiter. A drop costs the CPU a
refill and changes nothing else in its cycles; a write 16 MiB from a cached line (the nine bits do not tell them
apart) drops it too, which is rare and costs the same.

Uncached, device and mapped data accesses (but, with the tables, a mapped load from a cacheable page: "Address translation"), and `ldc1` / `sdc1`, take the slow path: one word at a time. `ld` and `sd`
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
| `W64_DIV` | yes | `ddiv(u)`, a bit a cycle (64 steps; `div(u)` run on the same divider, 32 steps); without it `ddiv(u)` are Reserved Instructions, and `div(u)` have a 32-bit divider of their own |
| `W64_UA` | yes | `ldl`, `ldr`, `sdl`, `sdr` (with `W64_SH`, `FAST`, `XWB`, `BIG`): an access to the doubleword the address is in, on the d-shifter's register, a byte a cycle. A store rotates `rt` by 8 - *n* (`sdl`) or 7 - *n* (`sdr`) bytes (*n* the address' low three bits) and writes its two words with the strobes of the bytes it covers; a load takes the doubleword through a byte at a time into `rt`'s place, 8 steps (each of the four takes about 20 CPU clocks on cached memory, back to back). Device, mapped and non-RDRAM addresses are the same address errors as `ld`'s and `sd`'s |

The bitstream and `n64rt.sys` are a pair, and the shipped CPU has every level. COP0 register 7 reads the levels in its
bits 31 (ALU), 30 (SH), 29 (MD), 28 (DIV) and 27 (UA); the runtime checks at start-up that all five are set and, if one
is not, stops with a notice on the screen (as for a missing layout mark) and starts no game. It keeps no copy of a
register's upper half, has no soft HI and LO and does no 64-bit instruction itself. What the CPU does not have is a
Reserved Instruction, and a Reserved Instruction stops the game with a message on the UART like any exception the
runtime does not handle: a trap instruction (`teq`, `tge` ...) on a 64-bit operand, `lld`, `scd`.

### The FPU

`fpu_n64.sv` is the VR4300's COP1 in single precision: 32 registers of 32 bits, a double being an even / odd pair. A
double operand is converted to a float and a double result is the float widened back. Results and inputs below the
normals are 0 (as the VR4300 with FCR31's FS bit set), but for a single's compare, which orders them exactly as the
VR4300 does (it neither flushes nor traps them there; software keeps pointers in float registers and tests them against
0.0, and every KSEG0 address below 8 MiB is a denormal); a NaN result is `0x7FBFFFFF`, and the conversions to an integer
saturate. There are no FPU exceptions and no cause or flag bits in FCR31. The moves, single and double arithmetic,
`sqrt`, the conversions between S, D and W, and the compares are implemented, each over several cycles on one datapath
(M waits): a 24 x 24 multiply on a DSP block, divide and square root by 28 restoring iterations. `dmfc1` and `dmtc1`
move a 64-bit register through a pair; `cvt.s.l` and `cvt.d.l` convert the pair's 64-bit integer exactly (a word's by
`cvt.s.w`'s path, another shifted a bit a cycle); the conversions to `.l` are those to `.w`, the word sign-extended into
the pair (beyond a word's range a value saturates as a word's).

### Address translation

The CPU has the VR4300's TLB (`TLB_EN`): 32 entries, each a pair of pages of 4 KiB to 16 MiB (PageMask), fully associative
on VPN2 and the ASID (a global entry matches any), with the Valid, Dirty, Global and C fields of EntryLo0 and EntryLo1, in
block RAM. `tlbwi`, `tlbwr`, `tlbr` and `tlbp` are the CPU's own, with the game's COP0 registers (EntryHi, EntryLo0/1,
PageMask, Index with its P bit, Random, Wired, Context's BadVPN2), and a TLB exception is the game's: a page no entry names is
a refill (TLBL, code 2, for a load or a fetch, TLBS, code 3, for a store) at `0x8000_0000` while Status.EXL is clear and
at `0x8000_0180` while it is set; a page an entry names with V clear is the same code at `0x8000_0180`; a store to a page
with D clear is Mod (1) at `0x8000_0180`. BadVAddr, Context's BadVPN2 and EntryHi's VPN2 take the address (the ASID is kept),
EPC (the branch's, in a delay slot) and Cause.BD are set unless EXL was, and EXL is. So the game's own `osMapTLB`,
`osUnmapTLB`, `osVirtualToPhysical` and a refill handler of its own (`tlbwr`) run as they are, and the runtime does
nothing for them. What the console's TLB does and this one does not: the 64-bit XContext and the 64-bit addresses (an address is
32 bits), and the machine check of two entries that match one address (undefined there; here the first by index wins). The registers keep what is written
(EntryLo 30 bits, PageMask 12 bits, whatever the mask), a translation uses `pa[31:12]` of EntryLo's page number, and Wired's low five bits
bound Random from below.

The entries are searched one a cycle, from entry 0, by a scanner that F and M share (`tlbp` and `tlbr` use it too), so
the first entry of the index order that matches wins. Each of F and M keeps the page it last translated in a **micro-TLB**
(one page, of whatever size the entry's pages are; the fetch side's are the page registers below): a mapped access to
that page costs the translation's cycle and no search. A TLB write, `tlbr` and a new ASID empty them, and a search
under way when that happens is dropped and made again. The costs, in cycles of the game's CPU (`lang/mips/test/tlbcost_test.c`):

| What | Cycles |
|---|---|
| a mapped load from the page the last load translated, over the same load from KSEG1 | 2 |
| a mapped load that changes page, over that: the search | entry *k* costs about 0.85 *k* (0 for entry 0, 15 for entry 16, 27 for entry 31) |
| a refill through the game's own handler of 7 instructions (exception, handler, `tlbwr`, `eret`, the load again), over a load from KSEG0 | 133, the mapped load's own 30 and its search included |
| the same, through the table's monitor (`TLB_RAM`: below), for comparison | some 3,000 |

A mapped load or store takes the slow path: one uncached word (two for a 64-bit one), and a store through a mapped page
drops the line it may shadow in the D-cache. A mapped `lw`, `sw`, `ld`, `sd`, `ldl`, `sdl`, `ll` and `sc` are the same as an unmapped
one's in what they do (`LLAddr` is the physical address). A mapped fetch is the slow path as well, a word at a time, unless the page is held by one of
the **fetch page registers**: two registers keep the last two pages
a search found that are cacheable (the entry's C field is not 2) and in the RDRAM -- the virtual page with its size, the ASID or global, the
physical page -- and a fetch from one runs through the I-cache like KSEG0's. The I-cache is indexed by the virtual address and tagged by
the physical one, `{valid, pa[28:12]}`; F delivers the cache's word and D checks it a cycle later against the register's page. A
fetch from a page neither register holds searches the TLB and loads the older register. A TLB write empties both registers.
Because the index comes from the virtual address, a frame mapped at a virtual address whose index bits above the page offset differ
from the frame's can sit in the cache under more than one index; once such a page has been entered, a hit-type cache instruction
on the I-cache drops the line under every such index, so code rewritten through one alias is the new code through all. The D-cache
does the same for a hit-type instruction through a mapped address.

**Where the physical address goes.** An access through a valid page to a physical address that is not the game's RDRAM -- an
RCP register, the cartridge, or nothing at all (with MCTL bit 10 the RDRAM's upper 4 MiB too) -- is the device access of an unmapped
address: an address error (4 or 5) for the monitor level, with the game's Cause, EPC, BadVAddr and EntryHi untouched, and COP0
register 22 holding the physical address the page gave. The runtime answers it as it answers the unmapped one (`emulate_hw`):
a register of the RCP as the console's, and an address of `0x2000_0000` or more -- the debug port that libultra maps at
`0xC000_0000` onto `0x8000_0000` -- as nothing: a load reads 0 and a store goes nowhere. The game's own exceptions for the same page
(an invalid page, a clean one) come first.

**The tables (`TLB_RAM`, until they are removed).** The core can still be built without the TLB and with two translation tables
in block RAM, one for data and one for fetches, of 512 entries each, which the runtime fills from the game's entries
(`MIRLO_TLB_TABLES` in `rtl/soc/mips_sys.sv`): an entry stands for one 4 KiB page, indexed by the page number's low 9 bits, and holds
valid, dirty, global and cacheable bits, the ASID, `va[31:21]` and the physical page `pa[28:12]`. A mapped access or fetch reads
its page's entry; anything but a valid entry of this page and this ASID is the TLB's exception, which goes to the monitor level,
leaves BadVAddr and EntryHi as the game has them (COP0 register 22 has the address) and is answered from the entries the runtime keeps
(below); the game's own `tlbwi`, `tlbwr`, `tlbr` and `tlbp` are Reserved Instructions for the monitor, which does them. A mapped load of a
word or less from a cacheable page goes through the D-cache (`PGD`), and the fetch page registers hold the last two cacheable pages of the
fetch table. `n64rt.sys` finds which CPU it has in COP0 register 23 and runs on either.

### The monitor level

With `MON`, COP0 register 7's bit 1 (`GAME`) divides exceptions in two:

- Those a console would take go to the game's own handler at `0x8000_0180` (a TLB refill at `0x8000_0000`), with EPC, Cause
  and Status.EXL as on the VR4300, when the instruction is the game's -- the TLB's exceptions among them.
- The rest go to the **monitor level**: a device access of the game's (an address that is not the RDRAM, by a pointer or through a
  page of the TLB), an instruction the core lacks, and the monitor's own interrupt (`irq_mon`, which the game cannot mask). (On the
  tables' CPU, below, a translation fault too.) The entry sets ERL and ErrorEPC and
  jumps to the runtime's vector; it touches neither EXL nor EPC nor Cause, so it can interrupt the game's handler too.
  `eret` with ERL set returns to ErrorEPC.

Count runs at half the CPU's clock, 46.875 MHz, as the VR4300's. Cause.IP2 is the MI's interrupt line.

### COP0 registers particular to this core

| Register | Bits |
|---|---|
| 7 (MCTL) | `[1]` GAME; `[2]` the monitor's entry was in a delay slot (ErrorEPC is the branch); `[7:3]` the entry's cause: an exception code, or 31 for the monitor's interrupt; `[8]` the pass-through to the RCP's blocks, written only together with bit 9; `[10]` the game's RDRAM is 4 MiB (its accesses to the upper half of the window go to the monitor), written only together with bit 11; `[12]` the game's view maps `0x2000_0000` - `0x4FFF_FFFF` (above), written only together with bit 13; `[14]` the game's word accesses to the SP's and the DPC's registers are the RSP's host port's (no trap; a store of DPC_STATUS still traps: the runtime keeps DPC_CLOCK), written only together with bit 15; `[31:27]` read the 64-bit levels present: ALU, SH, MD, DIV, UA |
| 22 | Read only: the physical address a page gave to the last device access through the TLB that the monitor level took; with the tables, the address of the last translation fault it took |
| 8 (BadVAddr) | Writable by `mtc0` while ERL is set |
| 23 | Read only, the capability word. With the TLB: `0x544C_42` (the ASCII `TLB`) in bits 31:8, bit 2 the fetch page registers (1 in the SoC), bit 1 the real TLB, bit 0 the monitor. With the tables: write stores a table entry for EntryHi's page pair and ASID (`[28:12]` the physical page, `[5]` cacheable, `[4]` which page of the pair, `[3]` the table (1: fetches), `[2]` global, `[1]` dirty, `[0]` valid); read `0x5442_0001`, with bit 3 set when the last TLB exception was a fetch's; 0 on a core with neither. `n64rt.sys` reads it at its start |
| 0 (Index) | Bit 31 (P) is writable by `mtc0` while ERL is set |
| 13 (Cause) | BD and ExcCode are writable by `mtc0` while ERL is set |
| 4 (Context) | BadVPN2 is writable by `mtc0` while ERL is set |

With the tables, while GAME is set, `tlbwi`, `tlbwr`, `tlbr` and `tlbp` are Reserved Instructions for the monitor level. The
three `mtc0` enables let the monitor do what the VR4300 does itself for a `tlbp` that finds nothing and for a TLB exception it
delivers to the game; with the TLB, Index's P bit and BadVAddr are written by the monitor level to put a save state back.

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
   (a byte-swapped ROM is named as such on the UART). A file under 1 MiB that is not a ROM is copied to the SDRAM at
   1 MiB and called there, as a program.
2. Finds the CIC from the CRC-32 of the boot code (ROM `0x40` - `0xFFF`) and the entry point from the header.
3. Takes the TV system from the header's destination code.
4. Clears the RDRAM, sets the RDP's hidden coverage plane to all ones, and copies 1 MiB from ROM `0x1000` to the entry
   address, as IPL3 does.
5. Writes what the CIC's IPL3 leaves behind and the boot information words (below).
6. Checks the CPU (before anything else, with the layout mark): the five 64-bit levels in COP0 register 7 must all be
   present, or a notice is shown and nothing is started. Reads the translation tables' mark in register 23. The runtime
   takes what else the core has from `MONITOR.ID` and from this, not from its own build.
7. Replaces the audio library's envelope rate and a matrix conversion in the boot segment (below).
8. Reads the save slot's file size, looks the cartridge up in the save-type database, initialises the accessory and
   the audio FIFO, and loads DMEM with the ROM's boot code (ROM `0x40` - `0xFFF`, at the same offsets), as the
   console's boot leaves it -- or, for a boot code that wipes the RSP's memories, DMEM and IMEM as it leaves them (below).
10. Turns the pass-through on, sets up the monitor's wake-ups, the VI's window and the AI's default rate, and enters
    the game at its entry point with Status `0x3400_0000` (CU0, CU1, FR, interrupts off), the stack at the top of the
    RDRAM and MCTL.GAME set.

### CIC and boot words

| Boot code | Entry point | What is left in memory | `osCicId` |
|---|---|---|---|
| 6101, 6102 | the header's | | 0 |
| 6103 | the header's less `0x10_0000` | ROM `0x4C0` - `0x8BF` at RDRAM 0; DMEM and IMEM all `0xFFFF_FFFF` but IMEM's first word, `0x17D7` | `0x17D7` |
| 6105 | the header's | ROM `0x554` - `0x887` at RDRAM 4 (IPL3's second stage, which it runs from there); the ROM's words at `0x750` and `0x73C` at RDRAM `0x2F_E1C0` and `0x2F_B1F4`; DPC_CLOCK reads 0; DMEM's first `0x40` bytes the ROM's header, and the RSP's r4, r5, r6, r11 and v13 as the PIF's RSP program leaves them (below) | `0x17D9` |
| 6106 | the header's less `0x20_0000` | IPL3's second stage at RDRAM 0: ROM `0x4F0` - `0x7AB`, deciphered as IPL3 does it; DMEM and IMEM all `0xFFFF_FFFF` but IMEM's first word, `0x17D8` | `0x17D8` |

One PAL boot code has an entry of its own in the table and is handled as a 6102, as is a boot code the table does not
know. Software checks these leftovers, so they are part of a valid console.

The boot microcode of a CIC-6105 game's RSP tasks checks, at its first run, what the PIF's program on the RSP left: r4 (IPL3's
first word, ROM `0x40`), r5 (an address in `0x4_C000` - `0x4_FFFF`), r6 (a word of IPL3, `addiu t1, t1, 4`), r11 (`0x3D8`), the sum
of v13's eight lanes (`-7510`) and DMEM's first halfword (not 0 or `0xFFFF`: the ROM's header is there). On a console it sets
signal 7 and writes two of its own words over its first two in the game's copy -- a jump past the check for every later task. On
an RSP left otherwise it clears signal 7 and the jump it writes leads to code that at every task's start writes DPC_END with
what it read of DPC_CLOCK (0 for the RSP here: always): a list the RDP is still taking is cut short, and a game that yields its
graphics task to an audio task never sees the resumed one end. The runtime leaves the registers as the console does
(`rsp_ipl_state` in `lang/n64rt/n64rt_rsp.c`: a program of its own, run once on the RSP before the game starts) and DMEM's
first `0x40` bytes as the ROM has them; with another boot code those bytes are 0 and the registers 0.

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
| SP_MEM_ADDR to SP_SEMAPHORE, SP_PC (on a core without MCTL bit 14) | the RSP's host port at `0x2000_0000` |
| DPC_START to DPC_STATUS, loads only (likewise) | `0x2000_0040` |
| the PI's registers other than PI_RD_LEN, PI_WR_LEN and PI_STATUS; loads of PI_DRAM_ADDR and PI_CART_ADDR excepted | `n64rt_pi_regs` |
| the RI's registers | `n64rt_ri_regs` |

The base register is read, and a load's result written, through two jump tables indexed by the register number.
Anything else falls through to the full path at the point it is found to be different, with nothing of the game's
changed. On the tables' CPU a second fast path does the game's `tlbr`, `tlbp` and the `tlbwi` that changes no translation,
with seven of the game's registers saved in the context for the work.

**The full path** saves the game's registers at 64 bits, HI and LO, FCSR and the 32 FPU registers, and calls
`n64rt_mon_exception` on a stack of the runtime's:

| MCTL cause | What the monitor does |
|---|---|
| 31, the monitor's interrupt | `n64rt_rcp_events`: the hardware's wake-ups (below) |
| 4, 5 (address error) | `emulate_hw`: the game's access to an RCP register, or to the cartridge, by a pointer or through a page of the TLB |
| 1, 2, 3 (TLB; the tables' CPU only) | `tlb_fault`: the page entered in the tables, or the exception delivered to the game |
| 10 (Reserved Instruction) | `tlb_insn`: the game's TLB instruction (the tables' CPU only) |

Anything else stops the game with a message on the UART.

Count, the game's clock, keeps the time the monitor takes: it is the console's clock, and what the monitor does for the
game takes real time. The exception is an entry while the game has its interrupts off (Status: IE clear or EXL set),
which is where it is when it sets a timer: there Count is put back to its value at the entry, so that an entry the game
did not ask for cannot carry Count past a Compare the game is about to write.

The monitor's interrupt is `MONITOR.PEND & MONITOR.MASK`. The runtime masks in five events:

| PEND bit | Event | What the runtime does |
|---|---|---|
| 1 | VI_WIDTH, VI_V_START or VI_Y_SCALE changed | computes the scan-out's width, window and size |
| 2 | a tick, every 2^18 system clocks (4.2 ms) | keeps its field count; notes the frame buffer's origin |
| 3 | AI_DACRATE was written | computes the resampler's step |
| 4 | the PIF has commands to run | runs them (SI, below) |
| 5 | the Pocket asks for a save state, or for its load | makes it or puts it back ([Save states](#save-states)) |

### TLB instructions and exceptions

With the CPU's own TLB the runtime has nothing to do for the game's TLB instructions and exceptions: they are the CPU's, and
nothing of the game's libultra is patched. It keeps no entries, and the monitor's fast path for the TLB instructions never runs.
What reaches it is a device access through a page (MCTL cause 4 or 5, `emulate_hw`: COP0 register 22 has the physical address; below `0x2000_0000`
it goes on as the unmapped address `0xA000_0000 | pa`, above it is nothing), and the instruction at a mapped `pc` that it has to
read to do that, a 64-bit instruction or an access it emulates (`insn_at`, `n64rt_tlb_phys`): found by `tlbp` and `tlbr`, with the game's
Index, PageMask, EntryHi and EntryLo registers put back. A state holds the 32 entries ([Save states](#save-states)).

**With the translation tables** (`TLB_RAM`, the core built with `MIRLO_TLB_TABLES`; the runtime knows which by COP0 register 23) the
runtime does all of the following. It keeps the game's 32 TLB entries as the registers gave them and does each instruction on the game's COP0
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

**The entries a `tlbwr` puts out are kept** (the tables' only). Software with a refill handler has more pages in use than the 32 entries
hold, and each `tlbwr` puts a random one out. On the console the page that entry mapped is a refill away, a dozen
instructions; with the tables a refill is several trips through the monitor. So the entry a `tlbwr` replaces is kept by the
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

A game can map `0x0000_0000` - `0x1FFF_FFFF`, `0x5000_0000` - `0x7FFF_FFFF` and `0xC000_0000` - `0xEFFF_FFFF` (the VR4300's
KSSEG and most of KSEG3) onto the RDRAM, and run code from them. `0x2000_0000` - `0x4FFF_FFFF` holds the SoC's own devices and
the SDRAM, where the runtime is, and `0xF000_0000` - `0xFFFF_FFFF` the registers and the monitor's RAM: those are not mapped,
but for the **game's view**. With MCTL bit 12 set (the runtime sets it at start-up), `0x2000_0000` - `0x4FFF_FFFF` are mapped
addresses as well while the game runs: MCTL.GAME set, Status.ERL clear (not the monitor level) and Status bit 4 clear. A game
can then map code at `0x4000_0000`, unpack an overlay into it and jump there, as on the console. The monitor level, and the
runtime's helpers that the game's code calls directly (the envelope rate, `n64rt_boot.c`: its entry
sets Status bit 4, a bit of KSU that the game's code never sets, and the wrapper puts the game's Status back), see the RSP and
the SDRAM there. The game starts with an `eret` from ERL, so the runtime's code before it is not in the game's view either.

### 64-bit instructions and the helpers

The CPU runs every 64-bit instruction of the games' code itself: the integer ones by the levels above (`ld`, `sd`, `lwu`,
`ldl`, `ldr`, `sdl`, `sdr`, the d-adds, d-shifts, `dmult(u)`, `ddiv(u)`, the 64-bit logic and compares) and the FPU's
`dmfc1`, `dmtc1`, `cvt.s.l`, `cvt.d.l` and the round / trunc / ceil / floor / cvt to `.l` (a 64-bit integer is an FPU
register pair; the arithmetic is the FPU's single precision). The compiler's 64-bit support routines (`__ull_div`,
`__ll_mod` and the others, each a pair of `ld`, one 64-bit instruction and a return) are the game's own code, run as they
are: a `ddivu` takes some 70 cycles. The monitor saves and restores the registers at 64 bits and HI and LO whole; it
reads the upper words only for an `sd` to an RCP register (`emulate_hw`) and for the compare of a branch whose delay slot it
does for the game (`branch_next`), and writes them for an `ld` of one.

The audio library's envelope mixer has a function that turns a volume ramp into the rate the microcode applies every
eight samples, `(target / volume)^(8 / samples)` in 16.16, which it takes as `(1 + ln(target / volume) / 2^30)` to the
`2^30 / samples`, squared three times, in double precision. With this FPU's doubles (floats, above) `1 + ln(x) / 2^30` is
1.0, so every rate was 1.0 and no volume moved: notes that start from silence stayed silent, the others never faded and
stopped dead. `ul_patch` finds the function by its words (the ones a link relocates masked, through a hash) and puts the
same jump over it. Three builds of the function are known: two take the O32 ABI's arguments, and one, from a compiler
that allocates registers across a module, takes them in registers of its own (`$a0`, `$f16`, `$f14`, `$s1`) and is
called with values live in what it leaves alone, so its wrapper keeps every register but the result. Each jumps to
`ul_getrate`, which takes the same power as `2^y` in floats -- `y` the library's own logarithm
of the ratio times `8 / 2^30` times its own `2^30 / samples` -- and gets the console's 16.16 rate, or one `2^-16` off. The
envelope rate is the one change the runtime makes to the game's arithmetic, made in the RDRAM's copy; it is entered with
the game's interrupts off and Status bit 4 on, and the wrapper puts Status back in the delay slot of its return (the CPU
sends an interrupt to the game's handler only when the interrupted instruction is the game's). The other patch of the same
kind, `mf_patch`, replaces a hand-written float-matrix conversion that reads a double's mantissa (`guMtxF2L`: `cvt.d.s`,
`add.d`, `mfc1` of the even register, which a float-computed double leaves zero) with the same conversion in integers.

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
  A DMA into the RDRAM from where nothing answers (past the ROM's end, or outside the ROM and domain 2) gets the open
  bus, each half word the low half of its own address (`0x1FFF_E000` reads `E000 E002 E004 ...`): the RDRAM is written,
  not left as it was.
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
the RSP and the RDP to finish, so that the copied data is not drawn over. The frame buffers are the origins the VI has
shown in the last two seconds (120 fields): one it has not shown since is no longer one. libultra's first ORIGIN, before the game
hands the VI a buffer, is a line (0x280) into the RDRAM's first bytes; kept for good, it made every DMA into the low memory of a game that
streams from the cartridge wait for the RDP's frame -- 11 ms at a time, with the game held in the monitor and its audio thread
with it, the AI dry for 0.65 ms at almost every field (a click each) and for a whole field now and then. What the engine cannot do, the runtime copies
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
- **VI_ORIGIN** is the scan-out's base, taken at the frame boundary. For a hi-res picture whose two fields' origins are
  one buffer row apart, a write of the base plus one row leaves the base as it is: a serrated picture is sent woven, its
  rows in order from the lower origin ([resolution.md](resolution.md)). An origin a serrated hi-res picture's game
  writes in the second field (VI_CURRENT's field bit, which libultra's retrace handler picks the field's origin by) is
  that field's, and the base is the row before it: a game that swaps its buffer every field gives the two fields'
  origins in different buffers, and the woven frame is the newest buffer's whole, at the same rows every field.
- **VI_STATUS**: GAMMA, AA_MODE, DIVOT and the dither filter bit go to the scan-out at the latch.
- **VI_H_START** written 0 (`osViBlack`) blanks the picture, and so does an origin beyond `MONITOR.VITHR`, the last
  origin at which a picture of the mode's size fits in the RDRAM: from the next frame boundary the scan-out reads
  nothing and sends black, every line, whatever its line buffers held of the picture before.
- **VI_V_SYNC** names the TV system once written: 512 - 575 is 525 lines (NTSC, MPAL and a PAL console's 60 Hz modes),
  576 - 703 is 625 lines (PAL, 50 Hz); another value changes nothing. The change is taken at a frame boundary.
- **`0x0440_0038`**, not a register of the console's: the runtime's, for a save state. It reads the base the scan-out
  takes, which a read of VI_ORIGIN does not give (the origin of a hi-res picture's second field leaves the base where it
  was), and in bit 31 the field bit; a write sets both.

The runtime computes what the scan-out needs -- the rows' pitch, the picture's window in the line and in the field,
and the two scales -- from VI_WIDTH, VI_H_START, VI_V_START, VI_X_SCALE and VI_Y_SCALE when one of them changes
(`n64rt_vi_hw_event`); [resolution.md](resolution.md) has how each picture is shown. A low-resolution window (WIDTH below
512) that asks for more pixels than the row has -- WIDTH 384, X_SCALE 0.649 and a 597-clock window ask for 387 -- ends with
the row: the console's VI reads the next row's first pixels there, at the edge a television's overscan hides, and the scan-out
shows the whole line, so they are not shown.

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
write leaves it 0 but for bit 1 (the challenge, below), and its bit 0 written as 1 marks the block as holding joybus
commands.

The PIF itself is the runtime. The hardware raises PEND bit 4 and sets IO busy at the end of a WR64B that carried the
run flag or the challenge's bit, at a CPU write of the status byte with the challenge's bit, and at an RD64B of a block
that holds commands; that read is deferred until the PIF is done, as the console's SI waits for the PIF. The runtime
(`n64rt_si_hw_event`) reads the 16 words, runs the commands (`pif_run`) or answers the challenge (`pif_challenge`),
writes back the words that changed and writes `MONITOR.SIDONE` with how long the console's PIF takes over those
commands (0 for a challenge). The read moves nothing until that time is out, counted from the runtime's write (DMA busy
meanwhile), so a read takes about what it takes on the console: 0.24 ms for a block of no commands, 0.62 ms for one
controller read, 1.55 ms for four with one controller there (`pif_run`'s figures, fitted to the console's) -- a game
whose controller thread polls in a loop, above the thread that loads the game, leaves that thread the time the console
does. A run that a WR64B started is counted from there, the console's time for libultra's write and then read; a WR64B
itself moves its words some 4,000 cycles (the console's 65 us) after the start.

The challenge is the CIC-6105's: the 30 nibbles in bytes 48-62 go to the cartridge's CIC, and its answer, a nibble
for each, takes their place; bytes 46, 47 and 63 are left 0. A game with that CIC sends one and checks the answer
itself. With another CIC the block is left as it was. The commands:

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
console only in the address: the RSP's host port at `0x2000_0000` answers at the N64's addresses too (`mirlo_mips.sv`),
and the core passes the game's word accesses there once the runtime has set MCTL bit 14 -- a register read is some
ten cycles, as an MI's; before that the monitor did each, some 200 cycles. A store of DPC_STATUS is still the
monitor's, which keeps DPC_CLOCK (its CLR_CLOCK_CTR bit). `rcp_dpc_view.sv` gives
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
PSRAM beside the save, so that the PI's engine and the hardware's status read reach them.

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

**Controller Pak**: 32 KiB in PSRAM, `data.json` slot 2, saved as `<game>.mpk`. Space for four paks is reserved; only Controller 1 is implemented. An image that is not a formatted
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

### The virtual Transfer Pak

The setting "Controller 1 pak" has a second Transfer Pak, **Virtual Transfer Pak**: the same accessory (the registers above) with a cartridge that
is made of two files the Pocket loads, in place of the Game Boy cartridge in the Pocket's slot (`lang/n64rt/n64rt_gbvirt.c`; the slot's port is not
touched). The game's own `osGbpak*` code runs on it as on the real one.

The setting "Controller 2 pak" (None, or Virtual Transfer Pak) puts a second one in controller 2, with a cartridge of its own: two instances of the same code (`n64rt_gbvirt.c`, n = 0 and 1; `n64rt_pak.c` keeps the Transfer Pak's registers of each controller apart), each with its own mapper, RAM and clock, and the slots below with "2". Controller 2 is a pad that is there when that setting asks for the pak (the dock's, with its keys, if it reports one; else with nothing pressed). A game that looks for its controllers when it starts finds the pak only if the setting was made before. The real cartridge in the Pocket's slot is controller 1's alone.

* **The ROM** is data slot 8, "Virtual Transfer Pak 1" (`.gb`, `.gbc`; at most 3 MiB), `0x4340_0000`; controller 2's is slot 9, "Virtual Transfer Pak 2", `0x42E0_0000`. With no
  file in a slot there is no cartridge in that pak, for either one. They are last among the data slots, and the Pocket lists the Load entries together, next to "Load Game". There is no default file: "Load Virtual Transfer Pak 1"
  loads one from `Assets/n64/common` while the game runs. It is there for the game when its header's checksum and logo are right and its
  cartridge type (`0x147`) is one of the mappers below. A ROM put in or taken out is seen as a cartridge put in or taken out, as with the real one: a game
  that polls the status finds it within a second. A game that looks for the cartridge only when it starts (as a console's power-on) does not see a ROM
  loaded after that.
* **The RAM** is memory only: `0x42D0_0000` (controller 2's: `0x42D5_0000`), the cartridge's RAM as the emulators keep it (its size is the header's, `0x149`) and for MBC3 with a clock 48 bytes after it: the clock's
  registers and the latched ones as ten little-endian words, and a 64-bit time that is left 0. Its file is the Game Boy game's own save, which a Game Boy core keeps as well: `<ROM's name>.sav` in `Saves/gbc/common` for a `.gbc` ROM
  and for a `.gb` in `Saves/gb/common`, else in `Saves/gbc/common` (a Game Boy core that plays both kinds keeps a `.gb`'s save in gbc), read and written **by path** (below). The Pocket's own save files (the nonvolatile slots, named after the N64 game) are not used for it: they cannot name another game's file.
* **The save file by path** (`sav_read` and `flush`, n64rt_gbvirt.c). When a cartridge is new to the pak (`ram_init`) the runtime asks the Pocket for the path of the file in the ROM's slot (`APF_BRIDGE.REQUEST_GETFILE`: the Pocket writes
  a 256-byte NUL-ended path to `RAM_DATA_ADDRESS`, here `LAYOUT_GS_PARAM`; measured: absolute, with the extension, as `/Assets/n64/common/<name>.gbc`), takes its name without the extension, and tries `/Saves/gbc/common/<name>.sav` for a `.gbc` and
  `/Saves/gb/common/<name>.sav`, then `/Saves/gbc/common/<name>.sav`, for a `.gb` (any other extension: by `/gbc/` or `/gb/` in the path, as a `.gbc` or a `.gb`; with neither, no save file). Only result 3 (not found) goes on to the next folder; the file found there is the one written back. It has the Pocket open that path into data slot 5 (controller 2's: 6), a slot with no file that is not read-only
  (`REQUEST_OPENFILE`: the parameter struct has the path, then the 32-bit flags, bit 0 creating a file that is not there, and a size; the path must be absolute, with the slash), and read it (`REQUEST_READ`) or write it (`REQUEST_WRITE`, the host's 0x0184)
  from the offset 0. The Pocket opens only files in the platform folders `core.json`'s `platform_ids` lists, so it lists `gbc` and `gb` besides `n64`: measured, with only `n64` the open of a `Saves/gbc/common` file fails with result code 4.
  Reading: the file there and as long as the RAM or longer (of a longer one only the RAM and the clock's footer are read): its bytes are the RAM, and it may be written; in no folder (result 3 in each): the RAM is `0xFF` and the first write back makes the file, in `Saves/gbc/common` for a `.gb` too; shorter than the RAM,
  or any other answer (the folder refused, no answer; no other folder is tried then): the RAM is `0xFF` and the file is never written. Each outcome is logged (`n64rt: Game Boy save: ...`). Writing is of the cartridge's RAM size (and the clock's footer), from the offset 0, never more (a longer file keeps its end),
  and only if the game has written to the RAM or the clock since the last one. When: (a) the game powers the Transfer Pak's cartridge off (the Transfer Pak's `0xB000`; measured: a game does it at its cartridge check, and the file is written at once), (b) another ROM
  is found in the slot, before the RAM is replaced, (c) 2 s after the last write to the RAM (`n64rt_gbv_field`, in every field), which also covers a state loaded (its RAM is not the file's: it is marked changed). Not with a save state's request: the Pocket answers no
  target command while it makes the state (measured: none for some 15 s), and the state carries the RAM. Not when the core is quit: nothing tells the runtime (the Pocket's host command to write a data slot goes to the bridge in hardware and reaches no register). A write that
  fails is tried again 5 s later; three failures with an answer, or twenty with none, end the write backs for that cartridge.
* **The mappers**: none, MBC1, MBC2, MBC3 (with its clock) and MBC5, as `0x147` says. A cartridge of another type (HuC, MMM01, the Camera) is not
  there for the game. The ROM's banks are its size rounded up to a power of two, and a read beyond the file is `0xFF`. Powering the cartridge on
  (`0xB000`) resets the mapper's registers: the RAM disabled, ROM bank 1, RAM bank 0.
* **The clock** counts seconds while the core runs. The core has no calendar, so the time the Pocket is off is not counted.
* **Save states** (Sleep, Memories): the virtual cartridges are in them, with their RAM and clock; a real one in the Pocket's slot is unplugged ([Save states](#save-states)).

`lang/n64rt/test` runs it under libultra's own code (`pak_test`; its header says how).

### GameShark codes

The setting "Enable Gameshark codes" (off by default) makes the runtime run the game's GameShark codes (`lang/n64rt/n64rt_cheat.c`,
`n64rt_gs.c`). They are a text file on the SD card, in `Assets/n64/common/gameshark/`, named after the cartridge: its four-character ID
(ROM `0x3B..0x3E`: `NSME`, the game's code and its region) and, for a revision whose addresses differ, its version byte in decimal:
`NSME.1.txt` is tried first, then `NSME.txt`. Every line that is a code is run, in the file's order, in every video field, as a GameShark does
from its hook on the vertical retrace: there is no choosing among them (the file holds the codes to run). `n64rt_gs.h` has the format
(`80123456 0063  a description`; lines that are not a code and `#`, `;` and `//` comments are skipped) and the types: `80`/`81` (8- and 16-bit
writes), `A0`/`A1`, `D0` to `D3` (the next line runs if the byte or half-word at the address is, or is not, the value), `50` (the repeater), and
`F0`/`F1` (as `80`/`81`). The GS button's `88` and `89` and the types that are not known are skipped. A write that would not change a value is not made;
one that does is made visible to the game's data cache, and the instruction cache is dropped in the field if any did. With 4 MiB of RDRAM (the
Expansion Pak setting off) writes above it are ignored. `lang/n64rt/test/gs_host.c` tests the parse and the codes on the host.

The file is read by the Pocket at the runtime's request, with the bridge's target commands (`APF_BRIDGE`, [control.md](control.md)); the
core's hardware already has them. The runtime writes the open command's parameter struct (the path, 256 bytes, then the flags and the size,
0) at `0x42D3_0000`, sets `SLOT_ID` to 7 and `RAM_DATA_ADDRESS` to it, and writes `REQUEST_OPENFILE`; the Pocket reads the struct from the
SDRAM through the bridge and opens the file. Then `DATA_OFFSET` 0, `TRANSFER_LENGTH` the file's size (`FILE_SIZE`), `RAM_DATA_ADDRESS` `0x42D4_0000` and
`REQUEST_READ`. Each command's end is `STATUS`; the result is `COMMAND_RESULT_CODE` (0 done; the Pocket answers 4 for a path it does not find).
**The path has to start with a slash and be the card's own** (`/Assets/n64/common/gameshark/NSME.txt`): measured on a Pocket, the same path
without the slash, relative to `common/`, to `n64/common/` or alone, all fail with 4.

The setting is looked at in every field (a box that is on and has no codes loads the file; off drops them) because the Pocket does
not send a setting to the core at a fixed time. A setting's `address` in `interact.json` has to be one no other setting has: the setting
table's n = 8, `0x1000_0120`, is the Start macro's, and a second setting there overwrote the first (the codes were never on).

### Save states

The Pocket's Sleep and its Memories are save states (`core.json`'s `sleep_supported`). To make one the Pocket sends the
core its Savestate Start command; when the core says the state is ready, it reads the core's block, as many bytes as the core
says (at most 10 MiB, `0x00A0_0000`, of which a state is at most `0x0097_0000`; a title screen's is some 0.9 MB, written as 1 MiB) at the bridge's `0x0630_0000` -- the SDRAM's `0x4230_0000`, above the last byte of a ROM of 64 MiB --
into a file. To put one back (the wake from a Sleep, a Memory chosen) it starts the core as for any launch, the game
booting afresh, copies the file in at the same address and sends Savestate Load. `target/pocket/core_top.sv` answers
both commands at once and reports them busy until the runtime is done; `APF_BRIDGE.SAVESTATE` and `SAVESTATE_DONE`
([control.md](control.md)) are the runtime's side, and `MONITOR.PEND` bit 5 its wake-up. The block is no memory the game
or the runtime uses, but the Pocket's read of it shares the SDRAM's port with the game (below), and the booted game runs
while the Pocket copies one in. `lang/n64rt/n64rt_state.c` does the work, with the game stopped in the monitor wherever it was:

1. **The hardware at rest.** The monitor waits for the RSP to halt and its DMA, the RDP, the PI's engine and the SI to
   be idle, and for the AI to have played its buffers (some 0.2 s, most of it the AI's), answering the other wake-ups as
   they come. The game's tasks end and their interrupts are pending in the state. A state that cannot be made so in
   half a second is refused (the Pocket's error), and the game goes on.
2. **A state** is the runtime's own `.data` and `.bss` (the game's registers in the monitor's context, the PI's and the RI's
   registers, the saves' kinds and sizes, and on the tables' CPU the game's TLB entries and the kept ones); the RDRAM and the RDP's hidden
   coverage plane; the cartridge's save, a FlashRAM's page buffer and status, the Controller Pak; and the hardware's
   registers: COP0 (Count, Compare, EPC, the TLB's registers, Cause's software bits), the CPU's own 32 TLB entries (`tlbr` on the way out, `tlbwi`
   on the way back, with Index, PageMask, EntryHi and EntryLo0/1 put back after them) and the FPU's condition bit, the MI's
   mode, interrupts and mask, the VI's registers, base, field and line, the AI's rate and its DMA enable, the PIF RAM, the
   SI's address and whether the PIF has run commands, DMEM, the RSP's status, PC and semaphore, the DPC's registers, and
   the sizes the Pocket's table has for the saves' and the paks' slots. Its header names the runtime (a sum of its code)
   and the ROM (its header's check words, title and ID): a state is refused by another `n64rt.sys` or another game.
   The game then goes on (a Memory) as from the moment the hardware came to rest: the VI's line is waited for and Count,
   Compare and the MI's interrupts are put back, so the copies' time is none of the game's.
   **Its size.** The Pocket's time to save a state and to put one back goes with the state's size (some 16 s of a wake, for
   9.9 MB), so the memories are written as tokens over 32-bit words: a word with bit 31 clear is the count of words that
   follow it as they are, with bit 31 set the count of times the word after it is repeated (three or more). The header
   and the registers are at fixed offsets, then DMEM, the save, the Controller Pak, the coverage plane, the RDRAM and last
   the runtime's own memory, each from an offset the header names. The core tells the Pocket the size, in MiB, that the runtime has written
   to `APF_BRIDGE.TRANSFER_LENGTH` (bits 23:20, whole MiB) before its `SAVESTATE_DONE`. The RDRAM of a game that has used little of it is mostly zeros, and the coverage plane a constant:
   a title screen's state is some 0.9 MB.
   **The read-out.** The Pocket reads the block after the answer, for seconds (some 11 s for 4 MB), through the bridge's port: the
   one the AI's reader and the game's SDRAM accesses go through. A game that went on meanwhile had its audio buffers come
   late and its sound crackled for as long as the read lasted. So the game stays stopped, in the monitor, until it is over (`readout_wait`). The
   core does not say when that is; it shows in the time a cold read of 4 KiB of the RDRAM takes: measured before the answer
   and in the 300 ms after it, the read is over when the time has been back near the first for a second. If the contention is
   not to be seen the wait is 4 s a MiB, and it is never more than 40 s. Count is put back after it (the game's time is that of the
   state's, not the wait's) and the runtime's own clocks move on by it.
3. **A load** quiets the booted game's hardware the same way (its RSP halted where it is if it does not stop), puts back
   the RSP (DMEM through the RDRAM; BROKE by a BREAK it runs), the DPC (CURRENT and END; a SYNC FULL through the RDP's
   command port clears what END's write set), the PIF RAM, the AI, the VI, the MI and COP0, then the memories, waits for
   the VI's line to be the state's (the game's next VI interrupt is as far ahead as it was), puts back Count, copies the
   runtime's memory over its own and leaves the monitor through its exit, into the game where it was.

Not in a state, because the hardware is at rest when it is made: IMEM and the RSP's registers (every task loads its
microcode and sets its registers), the AI's queue and the SI's DMA. Nor the RDP's modes and TMEM, which nothing reads
back: a game sets what it draws with in every frame's display list. A Game Boy cartridge in the Pocket's slot, in the Transfer Pak, is unplugged in the state: its RAM, mapper and
clock are its own, which a state cannot hold. The pak is made off, with no cartridge and "changed" in the copy of the runtime's memory that the state takes (the game that
goes on does not see it, `n64rt_pak_state_begin`); the game that is put back finds it gone and looks again, and the cartridge is in the slot still. The virtual Transfer Paks are in the
state: their registers are in the runtime's memory, and the RAM and the clock of each cartridge that is in have a region of their own (`RG_GB1`, `RG_GB2`) and the
sizes of their save files their place in the table; the clock goes on from where it was. Their ROMs the Pocket loads as for any launch, and
must be the same file (its header's CRC) when the state is put back: otherwise that cartridge is not in, for the game, as a ROM taken out. The core tells the Pocket it takes a state
when a game runs, and none when it does not.
`sim/mips_soc`'s `SS_SAVE`, `SS_LOAD` and `sim/mips_soc/state_compare.py` make a state, load it into another run and
compare the two runs' pictures and sound.

## Controllers and settings

The Pocket writes each setting of `pkg/pocket/Cores/tortuga.Mirlo64/interact.json` to a field of the two
`APF_INTERACT` words ([control.md](control.md)); the runtime reads the words at every poll of a controller and maps
the Pocket's keys with `n64rt_pad_map()` (`lang/n64rt/n64rt_pad.h`), a pure function. The two crop settings go to the
scan-out instead.

| Setting | Address | Field | Options (value); the default in bold |
|---|---|---|---|
| Crop Vertical | `0x1000_010C` | `interact0[9:5]` | None (0), **8 lines** (8), 12 lines (12) |
| Crop Horizontal | `0x1000_0114` | `interact0[19:15]` | [1:0] columns at the right, [3:2] at the left, up to 3 each: None (0), 1 / 2 / 3 columns (5 / 10 / 15), 1 / 2 / 3 columns left (4 / 8 / 12), **1 column right** (1), 2 / 3 columns right (2 / 3) |
| Expansion Pak | `0x1000_0138` | `interact1[24]` | **On** (0), off (1): a check box; read when the game starts |
| Enable Gameshark codes | `0x1000_013C` | `interact1[25]` | **Off** (0), on (1): a check box; seen in every field ([GameShark codes](#gameshark-codes)) |
| Controller 1 pak | `0x1000_0108` | `interact0[4:0]` | **None** (0), Controller Pak (1), Transfer Pak (2), Virtual Transfer Pak (3) |
| Controller 2 pak | `0x1000_0118` | `interact0[24:20]` | **None** (0), Virtual Transfer Pak (3) |
| Stick | `0x1000_012C` | `interact1[19:15]` | **D-pad** (0), L stick (1), R stick (2) |
| D-pad | `0x1000_0134` | `interact1[23:22]` | **None** (0), D-pad (1), L stick (2), R stick (3) |
| L stick dead zone | `0x1000_0124` | `interact1[9:5]` | Menu 0 % (1), 5 % (2), 10 % (3), ... 70 % (15); actual threshold is 30 % larger (0 % to 91 %) |
| R stick dead zone | `0x1000_011C` | `interact0[29:25]` | Same as the L stick |
| Start macro | `0x1000_0120` | `interact1[4:0]` | **None** (0), C-Down + C-Up (14), C-Left + C-Right (13), C-Down + C-Left (15), C-Down + C-Right (16), C-Up + C-Left (17), C-Up + C-Right (18): the two keys together are the Start; the Start key alone is then C-up |
| R as modifier | `0x1000_0110` | `interact0[14:10]` | **None** (0), Face C buttons (1): R held turns X, B, Y, A into C-up, C-down, C-left, C-right; Joystick half press (2): R held makes the D-pad's stick 60 % |
| C buttons | `0x1000_0128` | `interact1[14:10]` | **None** (0), D-pad (3), L stick (2), R stick (1) |

The Pocket's own Controls menu (`input.json`) lists A, B, C-Down (Select), C-Left (Y), C-Right (X), C-Up, L (ZL, `pad_trig_l2`), R (R), Z (L) and Start; C-Up is on ZR (`pad_trig_r2`: a key the documentation does not list); it remaps what
each button sends, and the runtime sees only the resulting key bits (so remapping its "Start" moves the Start key itself).

- **Z, L and R** are the Pocket's L, ZL and R keys; the Controls menu moves them.
- **Button codes** (Modifier, Start): 1 is the Pocket's L, 2 R, 3 ZL, 4 ZR, 5 none, 6 A, 7 B, 8 X, 9 Y,
  10 Select, 11 Start, any other value none; 0 is the
  setting's own key (L for Z, ZL for the L button, R for R, none for the Modifier). The settings are independent: one
  key may serve two N64 buttons.
- **A and B** are fixed. **C buttons**: C-left is Y, C-right is X, C-down is Select, C-up ZR (the Controls menu's C-Up). With
  "C buttons" set to L stick or R stick, that stick adds all four beyond half its range, one at a time: the quarter of the
  circle it points into (45 degrees either side of up, right, down, left; a diagonal of exactly 45 degrees is up or down).
  With D-pad, the D-pad keys add them too.
- **R as modifier**: when set, R does that while held and never reaches the game.
- **Start**: a key (a button code; that key is then the Start only), or Select and Start together, which then count for
  nothing else. The Start key, when the setting leaves it (another key; with Select+Start, Start alone), is C-up.
- **Stick**: with None, the stick stays centred. With D-pad, the D-pad keys are the stick at full deflection (80). With L stick or R stick, the analog
  stick's two bytes (`0x80` the centre; nothing within its stick's dead zone, then linear from its edge to 80 at the end of the range). A pad the dock
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

The device is a Cyclone V `5CEBA4F23C8` and the design fills about 97 % of its logic (`projects/fits.json` is the
record: each fit that closed, with its ALMs, 17,885 to 17,941 of 18,480 for today's sources). The RDP, the CPU with its
FPU and the RSP are the three largest blocks, followed by the scan-out and the register block. With the device this
full, whether a compile closes timing depends on the fitter's seed (`SEED` in `projects/mirlo.qsf`);
`projects/fits.json` records the seeds that closed for the sources as they are. A bitstream is good only if no analysis
is negative in any of the four corners (the slow and fast models at 85 and 0 degrees): setup, hold, recovery, removal
and pulse width.

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
  `projects_a2_<tag>.compile.log`. `QUARTUS_BIN` names Quartus's `bin` directory. The macros it appends are the ones
  the RTL still reads with a value (the RSP's lanes, the caches' sizes), at the RTL's own defaults; `EXTRA_MACROS` adds
  others.
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
in the PLL's phase. `EXTRA_DEFS="+define+MIRLO_TLB_TABLES"` builds it with the tables' CPU instead of the TLB; the tests that need what a CPU lacks say SKIPPED.

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
| `CONMODEL=n`, `CONMODEL_P=name=value,...` | every n fields, the game's code as this CPU ran it beside a timing model of the console's VR4300 run on the same instructions (`con_model.h`: its caches, interlocks, multiply / divide and FPU latencies, uncached and device accesses); `[conraw ...]` lines carry the counts, to price again with other costs |
| `RCPSTAT=n`, `RCPSTAT_IDLE=lo,hi` | every n fields: the RSP's and the RDP's time, the CPU's outside the game's idle loop, and each SDRAM port's words and cycles waiting |
| `RSPLOG=<file>`, `RSPPROF=<file>` | the RSP's tasks, a line each; its cycles by the microcode's pc |
| `RSPDUMP=<dir>` (`RSPDUMP_AUDIO=1`) | the graphics (and audio) tasks with the RDRAM, for `sim/rsp` and `rsp_replay` |
| `CKPT=<dir>,<n>` | a copy of the run left waiting every n fields, to be run on from with other options |
| `NOLOAD=1` | nothing loaded: the boot ROM's serial boot |
| `SS_SAVE=<field>`, `SS_OUT`, `SS_EXIT=1` | the Pocket asks for a save state at that field; the state is read out into the file (`<-o dir>/state.bin`) and the run ends, if asked |
| `SS_LOAD=<file>`, `SS_LOAD_AT=<field>` | the Pocket copies a state in at that field (60) and asks for its load (`SS_BRIDGE=1`: through the bridge's port, a word at a time) |
| `+gbcart=<rom.gb>` | a cartridge in the slot (a plus argument) |

### Programs for the SoC: `lang/mips/test`

`make -C lang/mips/test <name>` builds `build/<name>.bin`; each prints PASS or its failures on the UART. They need the
C library (`make -C lang/mips -f lib.mk VARIANT=game`) and are run as
`ROM=<any file> obj_soc/tb_soc build/<name>.bin -c <Mcycles>`.

| Program | What it checks |
|---|---|
| `cpu_test`, `libc_test` | Loads and stores of every width, the caches, the FPU, the divider; the C library |
| `int64_test`, `ddiv64_test` | The 64-bit levels against a table; `ddiv(u)` and `div(u)` against a table of exact quotients and remainders (`ddiv64_vec.py`) |
| `ualn_test` | `ldl`, `ldr`, `sdl`, `sdr` at every byte address (cached, uncached, mapped, on either TLB), in delay slots and behind stores to the same doubleword, block moves at every alignment, and their cost |
| `n64win_test`, `mon_test` | The RDRAM window and its trap; the monitor level beside a game's own handler |
| `tlbram_test`, `tlbmon_test`, `tlbgame_test`, `tlbpg_test` | Each is one program that runs the checks for the CPU it finds (COP0 23), the tables' or the VR4300's own TLB (`<name>_real.c`). On the translation tables' CPU: the tables; the game's TLB instructions; TLB exceptions delivered to the game; the fetch page registers (each says SKIPPED on a CPU with neither). On the real TLB: `tlbram_test`, the seven page sizes, the micro-TLBs, every register of the TLB exceptions and their vectors, `tlbp`, `tlbr`, Index, Wired, Random, the caches, `0x6000_0000`; `tlbmon_test`, the game's own TLB instructions with the monitor (no trap); `tlbgame_test`, its refill and general handlers; `tlbpg_test`, the cached fetches from mapped pages |
| `tlbint_real_test` | An interrupt that comes while a fetch from a page no entry names is in M: it goes to the general vector (never the refill vector), with the fault coming again afterwards |
| `tlbdev_test` | A page onto a device or past the RDRAM: the monitor's entry and COP0 22, the 4 MiB setting, the game's exceptions first, every range the game maps, the game's view |
| `tlbcost_test` | What a mapped load and a refill cost in cycles (printed) |
| `mi_test`, `vi_test`, `ai_test`, `si_test`, `pi2_test` | Each RCP block: registers, interrupts, DMA, and that the game's accesses enter the monitor only where they should |
| `rcpcpu_test` | The CPU's side of the SP and DPC registers: the semaphore, the DMA queue, DPC writes |
| `dma_coh_test` | The D-cache against the PI's engine, the RSP's DMA and the SI's RD64B writing a buffer beside data loaded during the transfer |
| `ram8_test` | Every block above the RDRAM's first 4 MiB; `sim/mips_soc/ram8_check.sh` runs it and checks the audio and the picture |
| `hires_test`, `vwin_test`, `viaa_test` | The scan-out's sizes and windows, and the VI's filters (`sim/vi_filter/viaa_check.py` compares the frame) |
| `rsp_replay` | One recorded RSP task on the whole SoC, timed |
| `padtest` | The controllers on the screen (previous section) |

`tools/ai_check.py` runs the resampler's algorithm over the frames the AI fetched (the bench's `AILOG=<file>`) and
compares the output sample for sample.

### The CPU alone: `sim/mips`

`build.sh` verilates `mips_core` and `fpu_n64` in lockstep with a reference model of the N64's CPU, `sim/n64` (or
another copy given as `N64_MODEL=<dir>`). Both run the same ROM and must agree on every retired
instruction's address, word and written register (64 bits), on every exception and on the FPU's registers.
`GEN="-GFAST=1 -GXWB=1 -GW64_ALU=1 ..."` passes the core's parameters. `gen_rand_rom.py` writes random integer programs
(every instruction on registers full of 64-bit values, through the cache and around it; its mode `full` matches the
SoC's levels, `div` is that with mostly divides, on the operands where a divider goes wrong, and `nodiv` has no
`ddiv(u)`; the argument `tlb` adds four windows of mapped pages onto the scratch memory, loads and stores through them with
`ll`, `sc`, `ld`, `sd`, `ldl` and `sdl`, and random `tlbwi`, `tlbwr`, `tlbp`, `tlbr` and ASID changes) and `gen_rand_fpu_rom.py` random FPU programs.
`tlb_rom.sh <out.z64>` builds `tlb_rom.c`, a directed TLB program: every page size, ASID and G, a refill and a fault in a delay slot and with EXL
set, a fault of a fetch. `TB_TLBDUMP=1` prints both TLBs at a mismatch (the RTL's with `TBC=-DTB_RTL_TLB` and `GEN=--public-flat-rw`).

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

### Boot sweep: `tools/sweep`

`tools/sweep/sweep.py <rom folder> [--seconds 8] [-j n] [--pad spec]` runs every ROM in a folder through `tb_soc`
with the runtime, several at a time, and judges each boot from its evidence: the runtime's entry and fatal lines, the
scan-out's video (`VGA=1`), `RSPLOG`, `EXCSTAT` and the final summary line. Each game gets a verdict (`alive`,
`frozen`, `black`, `crash`, `no-boot`, `timeout`), and the run a `report.md`, a `report.json` and a contact sheet of
the last frames. `--status <out>` shows a sweep under way, `--compare <baseline> <candidate>` checks an RTL change
frame by frame against an earlier sweep, and `tools/sweep/README.md` has the options and the rules.
