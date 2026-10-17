# MIRLO 64: the registers

The system's own registers -- the Pocket's interfaces, the scan-out's settings, the monitor's block and the cartridge
slot -- are 32-bit words at `0xF000_0000`. The map is one table, `tools/mirlo_regs.py`, which generates the hardware's
decode (`rtl/soc/mirlo_regs.svh`, used by `rtl/soc/mirlo_regs.sv`) and the software's
`lang/mips/include/generated/csr.h`, with an accessor for each register (`<block>_<register>_read()` / `_write()`) and
for each field. Offsets can change with the map: take them from `csr.h`, not from this page.

These are not the N64's registers. What a game reads and writes of the RCP is described in [mips.md](mips.md); the
runtime (`n64rt.sys`) is what uses the registers here.

| Block | Base | What |
|---|---|---|
| `APF_AUDIO` | `0xF000_0000` | The Pocket's audio FIFO |
| `APF_BRIDGE` | `0xF000_0100` | The Pocket's file commands and the data slots |
| `APF_INPUT` | `0xF000_0200` | The controllers |
| `APF_INTERACT` | `0xF000_0300` | The core's settings (`interact.json`) |
| `APF_VIDEO` | `0xF000_0400` | Vblank and the frame counter |
| `CTRL` | `0xF000_0500` | A scratch word, the bus error count |
| (reserved) | `0xF000_0600` | |
| RDP command port | `0xF000_0700` | A command FIFO for the RDP and its counters |
| `TIMER0` | `0xF000_0800` | A 64-bit cycle counter |
| `UART` | `0xF000_0900` | The JTAG UART |
| `VIDEO_FRAMEBUFFER` | `0xF000_0A00` | The scan-out: its base, its enables, the picture's pitch, window and scales |
| `GB_CART` | `0xF000_0C00` | A Game Boy cartridge in the Pocket's slot |
| `MONITOR` | `0xF000_0D00` | The monitor level's events, and the MI, PI, VI, AI and SI blocks' controls |

An address that no register has reads 0. A 64-bit register is two words, the high one first.

## APF_AUDIO

The Pocket takes 48 kHz stereo samples of 16 bits. The FIFO holds 1,024 of them.

| Register | Offset | Dir | What |
|---|---|---|---|
| `OUT` | `0x00` | W | One sample: the left and right channels, 16 bits each, in a word |
| `PLAYBACK_EN` | `0x04` | RW | 1: the FIFO is played |
| `BUFFER_FLUSH` | `0x08` | W | A write of 1 empties the FIFO |
| `BUFFER_FILL` | `0x0C` | R | The FIFO's level (0 to 1,023) |

The AI block in hardware (`rtl/soc/rcp_ai.sv`) feeds the same FIFO with the game's samples, resampled to 48 kHz.

## APF_BRIDGE

The Pocket's target commands (Analogue's
[host/target command documentation](https://www.analogue.co/developer/docs/host-target-commands) has their details and
result codes). A data slot is one of the entries of the core's `data.json`.

| Register | Offset | Dir | What |
|---|---|---|---|
| `REQUEST_READ` | `0x00` | W | A write of 1 asks the Pocket to read from the slot into memory |
| `REQUEST_WRITE` | `0x04` | W | A write of 1 asks it to write memory to the slot |
| `REQUEST_GETFILE` | `0x08` | W | A write of 1 asks for the path of the slot's file |
| `REQUEST_OPENFILE` | `0x0C` | W | A write of 1 asks to change the slot's file to the one a path in memory names |
| `SLOT_ID` | `0x10` | RW | The slot (16 bits) |
| `DATA_OFFSET` | `0x14` | RW | The offset in the slot's file |
| `TRANSFER_LENGTH` | `0x18` | RW | The bytes to transfer |
| `RAM_DATA_ADDRESS` | `0x1C` | RW | The SDRAM address (`0x4xxx_xxxx`) of the data |
| `FILE_SIZE` | `0x20` | RW | Read: the size of the slot's file. Write: sets the size the Pocket has for it, which is what it writes back of a nonvolatile slot |
| `STATUS` | `0x24` | R | 1 when a command has completed; a read clears it |
| `CURRENT_ADDRESS` | `0x28` | R | The address the bridge is at |
| `COMMAND_RESULT_CODE` | `0x2C` | R | The last command's result (3 bits) |
| `BOOT_READY` | `0x30` | RW | A bit the software can set and read; the Pocket is told the core is ready by the SDRAM's controller, not by this |
| `HOST_STATE` | `0x34` | R | Bit 0 `RESET_N`: the Pocket has released the core's reset. Bit 1 `LOADED`: it has loaded the data slots |

The Pocket loads the ROM, the runtime and the saves by itself before the core starts, and reads the nonvolatile slots
back when the core is quit: the runtime only sets a save's size (`FILE_SIZE`).

## APF_INPUT

The controllers as the Pocket gives them: controller 1 is the Pocket's own buttons or the Dock's first pad. There is no
interrupt; the runtime reads them when the game polls a controller.

| Register | Offset | Dir | What |
|---|---|---|---|
| `CONT1_KEY` .. `CONT4_KEY` | `0x00`, `0x04`, `0x08`, `0x0C` | R | The buttons (below) |
| `CONT1_JOY` .. `CONT4_JOY` | `0x10`, `0x14`, `0x18`, `0x1C` | R | The sticks (below) |
| `CONT1_TRIG` .. `CONT4_TRIG` | `0x20`, `0x24`, `0x28`, `0x2C` | R | The analog triggers (below) |

| `KEY` bits | | `KEY` bits | |
|---|---|---|---|
| 0 | D-pad up | 8 | L1 |
| 1 | D-pad down | 9 | R1 |
| 2 | D-pad left | 10 | L2 |
| 3 | D-pad right | 11 | R2 |
| 4 | A | 12 | L3 |
| 5 | B | 13 | R3 |
| 6 | X | 14 | Select |
| 7 | Y | 15 | Start |
| 28:16 | unused | 31:29 | the controller's type |

| `JOY` bits | | `TRIG` bits | |
|---|---|---|---|
| 7:0 | left stick, X | 7:0 | left trigger |
| 15:8 | left stick, Y | 15:8 | right trigger |
| 23:16 | right stick, X | | |
| 31:24 | right stick, Y | | |

How the runtime maps these to the N64's controller is in [mips.md](mips.md) (Controllers and settings) and
`lang/n64rt/n64rt_pad.h`.

## APF_INTERACT

The core's settings: two words that the Pocket writes from `interact.json`. An entry of `interact.json` whose `address`
is `0x1000_0100 + 4n` (n from 0 to 15) reaches the SoC: n = 0 and 1 write a whole word, the others a field of one, so
each setting has an address of its own and needs no mask.

| Register | Offset | Dir | What |
|---|---|---|---|
| `INTERACT0` | `0x00` | RW | Word 0 |
| `INTERACT_CHANGED0` | `0x04` | R | 1 when the Pocket has written the whole word 0 since the CPU last read `INTERACT0` |
| `INTERACT1` | `0x08` | RW | Word 1 |
| `INTERACT_CHANGED1` | `0x0C` | R | The same for word 1 |

| n | `address` | Field | The setting (`pkg/pocket/Cores/tortuga.Mirlo64/interact.json`) |
|---|---|---|---|
| 0 | `0x1000_0100` | `INTERACT0`, whole | |
| 1 | `0x1000_0104` | `INTERACT1`, whole | |
| 2 | `0x1000_0108` | `INTERACT0[4:0]` | Controller 1 pak |
| 3 | `0x1000_010C` | `INTERACT0[9:5]` | Crop Vertical (the scan-out reads it) |
| 4 | `0x1000_0110` | `INTERACT0[14:10]` | Modifier |
| 5 | `0x1000_0114` | `INTERACT0[19:15]` | Crop Right (the scan-out reads its low two bits) |
| 6 | `0x1000_0118` | `INTERACT0[24:20]` | L button |
| 7 | `0x1000_011C` | `INTERACT0[29:25]` | R button |
| 8 | `0x1000_0120` | `INTERACT1[4:0]` | not used |
| 9 | `0x1000_0124` | `INTERACT1[9:5]` | Z button |
| 10 | `0x1000_0128` | `INTERACT1[14:10]` | C buttons |
| 11 | `0x1000_012C` | `INTERACT1[19:15]` | Stick |
| 12 | `0x1000_0130` | `INTERACT1[21:20]` | Start button |
| 13 | `0x1000_0134` | `INTERACT1[23:22]` | D-pad |
| 14 | `0x1000_0138` | `INTERACT1[24]` | Expansion Pak (0: in) |
| 15 | `0x1000_013C` | `INTERACT1[25]` | not used |

## APF_VIDEO

One register, `VIDEO`, at offset `0x00`:

| Field | Bits | What |
|---|---|---|
| `VBLANK_STATUS` | 0 | 1 while the scan-out is in vblank |
| `VBLANK_TRIGGERED` | 1 | Set when vblank starts, cleared by a read of the register |
| `FRAME_COUNTER` | 31:2 | The fields sent since reset |

## CTRL

| Register | Offset | Dir | What |
|---|---|---|---|
| `RESET` | `0x00` | W | No effect: there is no soft reset (quit the core and launch it again) |
| `SCRATCH` | `0x04` | RW | A word (`0x1234_5678` after reset) |
| `BUS_ERRORS` | `0x08` | R | The CPU's accesses to addresses nothing answers (16 bits) |

## RDP command port

A FIFO of command words for the RDP beside its own command fetch (`rtl/rdp/rdp_dpc.sv`, [rdp.md](rdp.md)), and the
RDP's counters. A game's commands come through the DPC registers, not through this port; the runtime reads the count of
full syncs here.

| Offset | Dir | What |
|---|---|---|
| `0x00` | W | A command word into the FIFO (lost, and counted, when the FIFO is full) |
| `0x04` | R | Bits 11:0: the FIFO's level. Bit 12: full |
| `0x08` | R | The words lost (16 bits) |
| `0x0C` | R | The SYNC FULL commands the RDP has completed: everything before them is in memory |
| `0x14` | R | Bit 0: the RDP is idle and the FIFO empty |

The accessors' names are in `csr.h`.

## TIMER0

A free-running 64-bit count of system clock cycles (62.5 MHz).

| Register | Offset | Dir | What |
|---|---|---|---|
| `UPTIME_LATCH` | `0x00` | W | A write latches the count |
| `UPTIME_CYCLES` | `0x04` | R | The latched count: the high word at `0x04`, the low word at `0x08` |

## UART

A UART over the Pocket's JTAG port (`rtl/soc/jtag_uart_bridge.v`; `tools/jtag/jtag_uart_relay.py` is the host's side):
the boot ROM's and the runtime's messages. The project file builds the bitstream without it (the macro
`MIRLO_NO_JTAG_UART` in `projects/mirlo.qsf`).

| Register | Offset | Dir | What |
|---|---|---|---|
| `RXTX` | `0x00` | RW | Write: a byte out (dropped if the FIFO is full). Read: the next byte in |
| `TXFULL` | `0x04` | R | The transmit FIFO is full |
| `RXEMPTY` | `0x08` | R | The receive FIFO is empty |
| `EV_PENDING` | `0x0C` | RW | Always 0: the UART is polled |

## VIDEO_FRAMEBUFFER

The scan-out's settings (`rtl/soc/n64_video.sv`, [resolution.md](resolution.md)). With the VI in hardware
(`MONITOR.CTL` bit 3) the base, the gamma and the blanking are the VI's own; the runtime writes the rest from the VI's
registers.

| Register | Offset | Dir | What |
|---|---|---|---|
| `DMA_BASE` | `0x00` | RW | The frame buffer's address; taken at the frame boundary, so a new base never tears the frame being sent |
| `DMA_LENGTH` | `0x04` | RW | Not used |
| `DMA_ENABLE` | `0x08` | RW | The scan-out reads the frame buffer |
| `DMA_OFFSET` | `0x0C` | R | Reads 0 |
| `VTG_ENABLE` | `0x10` | RW | The video timing runs (with `DMA_ENABLE`: the picture) |
| `VTG_MODE` | `0x14` | RW | No effect |
| `WIDTH` | `0x18` | RW | Bits 11:0: the pitch, the pixels from one row of the frame buffer to the next (512 or more: the VI's filters are off). Bit 16: the VI's gamma correction. Bit 18: the TV system to use until the game programs the VI (1: PAL). Bit 19, read only: the TV system the scan-out is using |
| `WINDOW` | `0x1C` | RW | The picture's window in the field: bits 15:8 its first line, bits 24:16 its lines (0: the whole field) |
| `HWINDOW` | `0x20` | RW | Its window in the line: bits 9:0 its first clock, bits 25:16 its width in clocks |
| `SCALE` | `0x24` | RW | The steps through the frame buffer, with 10 fraction bits: bits 10:0 the pixels a clock of the window steps (at most 1), bits 26:16 the rows a line steps (at most 2) |
| `ROWWORDS` | `0x28` | RW | The words the scan-out reads of a row (11 bits) |

## GB_CART

A Game Boy cartridge in the Pocket's slot (`rtl/soc/gb_cart.sv`), read and written a byte at a time: what the Transfer
Pak uses.

| Register | Offset | Dir | What |
|---|---|---|---|
| `ID` | `0x00` | R | `0x4742_0001` |
| `CTRL` | `0x04` | RW | Bit 0 `ON`: the cartridge out of reset, the bus driven. Bit 1 `TRUST`: chip select and writes allowed (it is a Game Boy's cartridge) |
| `ACCESS` | `0x08` | RW | Write: an access starts (not while `BUSY`): `DATA` bits 7:0, `ADDR` bits 23:8, `WE` bit 24. Read: `BUSY` bit 31 and the byte read in `DATA` |

## MONITOR

The monitor level of the CPU ([mips.md](mips.md)) and the controls of the RCP's blocks in hardware
(`rtl/soc/rcp_mi.sv`, `rcp_vi.sv`, `rcp_ai.sv`, `rcp_si.sv`). Only the runtime uses these.

| Register | Offset | Dir | What |
|---|---|---|---|
| `ID` | `0x00` | R | `0x4D4F_0000` with a bit for each block the core has: 0 the monitor, 1 the MI, 2 the PI's engine, 3 the VI, 4 the AI, 5 the SI and the PIF RAM, 6 the PI's domain 2 and the cartridge's word reads |
| `CTL` | `0x04` | RW | Bit 0 `OWN`: the game's interrupt line is the one below, not the RSP's own. Bit 1 `RCP`: that line. Bit 2 `MI`: the game's interrupt is the MI's line. Bit 3 `VI`: the VI is the hardware's. Bit 4 `AI`: the AI is the hardware's |
| `PEND` | `0x08` | RW | What has happened (a write of 1s clears): bit 0 the RSP's interrupt rose; bit 1 a field began (with the VI in hardware: the registers the picture's window comes from changed); bit 2 a tick of 4.2 ms; bit 3 the RDP completed a full sync (with the AI in hardware: the game wrote the AI's rate); bit 4 the PIF has commands to run |
| `MASK` | `0x0C` | RW | Which of them interrupt the monitor |
| `KICK` | `0x10` | RW | A write of 1s sets `PEND`'s bits |
| `LEVEL` | `0x14` | R | Bit 0: the RSP's interrupt, as it is |
| `MIRAISE` | `0x18` | W | A write of 1s sets bits of the MI's interrupt register (SI, AI, VI, PI, DP: bits 1 to 5) |
| `MICLR` | `0x1C` | W | A write of 1s clears them |
| `MIINTR` | `0x20` | R | Bits 5:0: the MI's interrupts. Bits 13:8: its mask. Bit 16: the PI's engine is running |
| `PIDST` | `0x24` | W | The PI's engine: the destination word (in the RDRAM, or anywhere in the SDRAM when bits 23:21 are not 0) |
| `PISRC` | `0x28` | W | Its source word in the SDRAM (bit 24: from the word's second half) |
| `PILEN` | `0x2C` | RW | The words to copy (bits 23:22: the bytes of the last one, 0 for all four): the write starts the copy. Read: the words left, and bit 31 while it runs |
| `VITHR` | `0x30` | W | The last origin (in units of 64 bytes) at which a picture of the mode's size fits in the RDRAM: beyond it the scan-out shows black |
| `AIRATE` | `0x34` | RW | Write: the resampler's step (source frames for each 48 kHz sample, 16 fraction bits). Read: the AI's rate register as the game wrote it |
| `SIDONE` | `0x38` | W | A write: the runtime has run the PIF's commands (the SI's busy flag clears); [17:0] the console's time for them in RCP cycles, which an RD64B waits out |
| `PI2` | `0x3C` | RW | Bit 0: the cartridge's domain 2 is a FlashRAM (the game's reads of it are the status word, its writes go nowhere) |
