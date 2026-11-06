# Analogizer support

The Analogizer is an add-on that plugs into the Pocket's cartridge slot and
turns the core's video into analog signals (RGBS, RGsB, YPbPr, Y/C, RGBHV).
The slot is also where a Game Boy cartridge goes (the Transfer Pak setting
"Transfer Pak cartridge", rtl/soc/gb_cart.sv), so the two share the 32 pins:
at any time one of them owns the port.

All the modules under `rtl/analogizer` are written for this core from the
adapter's pin map and from the video standards; none is taken from another
project, so the tree stays under the Apache License.

## The port

Every pin goes through a level translator whose direction is the bank's `_dir`
(1: the FPGA drives). With the adapter in the slot the FPGA drives banks 1, 2
and 3 and the adapter's DAC reads them:

| bank | bit 7 | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---|---|---|---|---|---|---|---|---|
| `cart_tran_bank3` | R5 | R4 | R3 | R2 | R1 | R0 | HS pin | VS pin |
| `cart_tran_bank2` | B0 | /BLANK | G5 | G4 | G3 | G2 | G1 | G0 |
| `cart_tran_bank1` | spare | spare | pixel clock | B5 | B4 | B3 | B2 | B1 |

The DAC takes six bits a channel (the top six of the core's eight). `HS pin`
and `VS pin` are the sync outputs; what each mode puts on them:

| mode | R | G | B | HS pin | VS pin | /BLANK |
|---|---|---|---|---|---|---|
| RGBS | R | G | B | csync | 1 | blank |
| RGsB (sync on green) | R | G | B | 1 | csync (the DAC's SYNC) | blank |
| YPbPr | Pr | Y | Pb | 1 | csync (the DAC's SYNC) | 1 |
| Y/C NTSC, PAL | C | Y | 0 | csync | 1 | 1 |
| RGBHV (scandoubled) | R | G | B | hsync | vsync | 1 |

`/BLANK` is active low; in the RGB modes the colour is also forced to 0 in the
blanking. Bank 0 (bits 7:4) and pins 30 and 31 belong to the SNAC controller
port and are not used by the video; bank 0 is driven high, unless the SNAC port's Joybus is on
([below](#the-snac-port-as-the-consoles-pif)).

## What the scan-out sends

`rtl/soc/n64_video.sv` sends the N64 VI's own timing: a 640-pixel line of 790
(NTSC) or 800 (PAL) pixel clocks, 264 / 313 lines a field, 59.94 / 50 Hz, a
pixel each two clocks of the 25 MHz `clk_vid` (12.5 MHz pixels, 15.8 kHz
lines), `vga_skip` marking the second clock of a pixel. That is a 240p signal
a CRT takes as it is: the 15 kHz modes (RGBS, RGsB, YPbPr, Y/C) use it
directly. A serrated hi-res picture is woven by the scan-out into 480 lines a
frame, 640 x 480 at 31.6 kHz, a pixel a clock: that is a VGA signal, which
only the RGBHV mode shows; for the 15 kHz modes the weave is switched off
(the scan-out's `no_weave` input), so a serrated picture goes out field by
field, 240 lines each, as 240p.

The RGBHV mode doubles the lines of the 240p signal (31.6 kHz, 25 MHz pixels)
and passes a woven frame as it is.

## Modules (rtl/analogizer)

- `an_csync`: composite sync from the scan-out's hsync and vsync (hs, with the vertical interval's broad pulses and
  serrations; active low, `CSYNC_ACTIVE_LOW`).
- `an_ypbpr`: the RGB to Y, Pb, Pr matrix (BT.601, studio levels): nine products by constants, which the synthesis puts in
  four DSP blocks, summed in two clocks.
- `an_yc`: the luma and chroma for S-Video and composite, NTSC and PAL (the adapter makes the composite by adding the
  two): a 22-bit carrier, two sine tables (U and V, with the weights and the gain in them, so no multiplier scales U and
  V), U and V low-passed, the burst in the back porch, the V axis switched every line in PAL (by reading the V table
  half a turn on). R carries the chroma, centred on 128; G the luma; B is 0; csync goes out on the HS pin. It also hands
  out its own matrix and sync for the YPbPr mode, so the top builds one of each.
- `an_scandbl`: the line doubler for RGBHV: 240p lines written once and read twice at the 25 MHz pixel rate (31.6 kHz,
  528 lines), the scanlines option dimming the second copy; a woven frame goes through as it is. The line buffer keeps
  six bits a channel, which is all the DAC takes.
- `an_out`: the mode multiplexer and the pin map above; everything registered on `clk_vid`. The colours of RGBS and RGsB
  go through `an_scandbl`'s pass-through (as the RGBHV woven frame does), a clock behind the scan-out's, so that one
  colour path serves the three RGB modes.

Tests: `sim/analogizer/run_tests.sh` (Verilator, against floating-point models in Python; `an_out` checks the pin map of
every mode on the scan-out's timing) and `sim/analogizer/scandbl/run_tests.sh`.

### Resources

The five modules together take about 180 ALMs, 5 DSP blocks (four in the matrix, one for the modulator) and 4 M10K blocks
(two for the line buffer, one for each sine table) on the Cyclone V (measured on `an_out` alone, with the core's synthesis
settings). The U and V weights are in the sine tables, the YPbPr matrix is in DSP blocks, and the RGB modes share the
colour path of the line doubler, which is what keeps them small. The matrix is a plain multiplication by constants: if
the DSP blocks run out the synthesis builds them in logic, at about 75 ALMs more (125 against 50).

### What a monitor has to confirm

The modules are checked in simulation against the standards, not against an adapter; these are the assumptions:

- the csync polarity (`CSYNC_ACTIVE_LOW`) and the RGBHV polarities (`HS_ACTIVE_LOW`, `VS_ACTIVE_LOW`);
- the Y and C levels at the adapter's resistor network (`CGAIN` of `an_yc`: 12/16) and the burst's level;
- the burst's place: 0.6 us (NTSC) after the sync; the scan-out's sync is 3.84 us wide (the standard's 4.7);
- the carrier's phase restarts every line (the line rate is the scan-out's 15.82 kHz: a receiver relocks on each burst);
- in RGBS and RGsB the colour is one clock (40 ns) behind the scan-out's and the sync two behind it (the YPbPr and Y/C
  modes have each their own delay, the sync and the signal aligned);
- the RGBHV frame is 528 lines at 59.94 Hz, vsync 8 lines wide (`VS_LINES`, `VS_DELAY` for the standard's 2);
- the DAC's clock (bank 1 bit 5) is `clk_vid_90deg`: its phase against the data may need the other edge.

## Who owns the port

There is no pin that tells an Analogizer from a cartridge, and no setting for
it: the runtime, at the core's start, brings the port up as a Game Boy cartridge's host and
reads its header: the Nintendo logo and the header checksum, the test the Transfer Pak
code already makes before it trusts a cartridge. If the header is a Game Boy's
the port stays the cartridge's; if not (an Analogizer, or an empty slot) the
Analogizer takes the port and shows the video mode of "Analogizer". The
probe puts addresses and /RD on the pins the adapter reads as video for a few
milliseconds, once, at the core's start; the slot is not hot-pluggable, so
that is the only time it is looked at.

## Settings

One entry in the core's menu: "Analogizer" (it was "Analogizer video"; its values are the same, so a saved setting stays): Off, RGBS, RGsB, YPbPr, Y/C NTSC,
Y/C PAL, RGBHV, and RGBHV with scanlines 25 %, 50 %, 75 % (the scanlines dim the odd lines, which only the doubled
RGBHV picture has), and N64 SNAC (no picture: the real N64 controllers on the adapter's SNAC connector only). The Pocket's own picture stays on.
Everything but Off also turns the SNAC controllers on ([below](#the-switch-the-menus-analogizer)).

The Pocket's menu has two limits, both measured on a Pocket: it shows only 14 of the core's entries (a 15th loads without
an error and is silently left out, so "Restart Game" has to stay the 14th, the last), and it refuses an `interact.json`
whose lists have more than about 75 options in all ("Load error in 'interact'": 75 loads, 77 does not, 76 untried). The
core's menu has 14 entries and 68 options: the two sticks share one "Stick dead zone" (steps of 10 %, 0 to 70 %) to make
room for the video entry, and the Expansion Pak and the Gameshark codes share one list ("Expansion Pak/Gameshark") to make
room for "Video settings" ([video_options.md](video_options.md)). The SNAC controllers have no entry of their own: they are a choice of "Analogizer".

## The SNAC port as the console's PIF

The adapter's USB 3 connector is the N64 SNAC board's: four Joybus data lines, one for each controller port of a console. With
the menu's "Analogizer" not Off the core talks Joybus to whatever is plugged into them, as a console's PIF does, and gives the game the
devices' own answers. Nothing about a device is special-cased: the game's command bytes go out on the wire and the reply bytes
(with the device's CRC) come back, so a controller, a Controller Pak, a Rumble Pak, a Transfer Pak with a real Game Boy
cartridge, a mouse, a keyboard or a VRU work as far as the game's own code does.

### Where it sits

`rtl/soc/rcp_si.sv` is the SI and the PIF RAM; the PIF's command interpreter is the runtime's (`pif_run`, `lang/n64rt/n64rt_io.c`).
Each channel of the game's 64-byte block is one of three kinds:

| Channel | Kind | When |
|---|---|---|
| 0 to 3 | a real device | the menu's "Analogizer" is not Off and the SNAC scan has seen a device answer on that controller's port |
| 0 to 3 | virtual | else: the Pocket's pad (or the dock's) and the virtual Controller Pak or Transfer Pak, as before |
| 4 | the cartridge's EEPROM | always the runtime's |

**Policy.** A physical device on a port takes that channel; otherwise it is the virtual one. So with a pad on port 1 and the
Pocket's own pad, the game reads the physical pad and the Pocket's buttons do nothing; with nothing on port 1 the Pocket is
controller 1. A port is "present" when the status command (`0x00`) has been answered twice in a row by the scan (every port is polled every 8 ms
by the hardware) and not missed three times since, so a device plugged in or out while a game runs is followed: a controller that goes away between two commands
gets the no-response bit, and the next scan hands its channel back to the virtual one.

For a real channel, `pif_run` copies the game's command (its `tx` bytes) into the SNAC buffer, starts a transaction on the
port, waits for it, and writes the bytes received into the block's reply bytes unchanged. The rx-length byte's bit 7 (no response)
is the wire's timeout: no edge within `TFIRST` (64 us) after the console's stop bit. Bit 6 (the device's reply was short) is set
when fewer bytes than the game asked for came, or the wire never went quiet. A device that sends more than the game asked for
is not noticed (the extra bytes are waited out, not reported).

**Timing.** The runtime busy-waits for each transaction (a controller read is about 0.2 ms, a pak block write 36 bytes x 32 us = 1.3 ms),
so the game's thread is held for the wire's time, no longer. The monitor level does not advance the game's Count, so the game does not see that
wait itself: `MONITOR.SIDONE` carries the console's whole time for the commands, which is for each real command the figure of a virtual
one (`pif_run`'s, fitted to the console) or, if the device is slower than that, 10,500 RCP cycles plus the wire's own time. A block of four
controller reads then ends about when the console's does (1.5 ms by the game's clock), and a slow device holds it as long as it holds the wire.
What the game does not have, in real time, is the CPU during the wait (about 0.5 ms for a frame's four controllers, 3 % of a frame); the
hardware (video, audio) goes on.

### The hardware

`an_joybus_m` is one Joybus master on one open-drain wire. A bit is a 4 us cell that starts low: a 1 is 1 us low and 3 us high, a 0
is 3 us low and 1 us high, most significant bit first; the command ends with the console's stop bit (1 us low). The reply is
read the way a console does: after the falling edge that starts a bit, the wire is sampled 2.05 us on (a 1 is high by then, a 0
still low). The master never drives a 1: its only output is `low`. After the reply it waits for 12 us of a quiet wire so that
the device's stop bit and the next command do not meet (255 us at most: a wire held low is given up, flagged `SHORT`). A microsecond is 62 or
63 clocks of the 62.5 MHz clock, alternately.

`an_snac_pif` puts four ports behind it: a 128-byte buffer (one M10K block), the registers of [control.md](control.md#snac) and
the scan. One transaction runs at a time; the selected port's wire is the master's and the other three are released. The scan
(`CTRL.SCAN`) polls each port in turn, one every 2 ms, with the status command, into the scan table in the buffer
(`64 + 4 n`: the three bytes, then their count); a transaction the runtime asks for meanwhile waits for the scan's one that is under way (170 us at most).

### The pins and the electrical rules

| Port | Pin | Direction | Notes |
|---|---|---|---|
| 1 | `cart_tran_pin30` | its own `cart_tran_pin30_dir` | also `/RES` of a cartridge: the clamp that holds it low (`cart_pin30_pwroff_reset`) is released while the Joybus is on |
| 2 | `cart_tran_pin31` | its own `cart_tran_pin31_dir` | |
| 3 | `cart_tran_bank0[4]` | `cart_tran_bank0_dir`, shared | |
| 4 | `cart_tran_bank0[7]` | `cart_tran_bank0_dir`, shared | |

Every wire has an external 1 kOhm pull-up to 3.3 V on the harness, and every pin goes through a level translator whose direction is
the bank's `_dir`. A wire is **pulled low** by setting the pin's data to 0 and turning the translator and the FPGA's driver on,
and **released** by turning both off, so that the translator is an input and the pull-up raises the wire. Ports 1 and 2 have
a direction each, and that is all there is to it. Nothing is driven high on them, ever.

Ports 3 and 4 share bank 0, which has one direction for its four bits (7:4; bits 6 and 5 are the cartridge's /WR and /RD; they are
not part of the N64 harness as far as is known, which is a fact to confirm). So whenever one of them is pulled low the bank is an output, and the other
three bits must have some value: they are driven **1**, which is the level the pull-up gives them anyway. That happens only
during the low pulse of the other port (1 to 3 us of a bit cell, of a transaction on that other port), and only matters if a
device on the driven line pulls it low at that moment, which a Joybus device does only when it is spoken to, and the master speaks to
one device at a time. In the rest of the cell, and while any device answers, the bank is an input and everything is released. The
simulation checks that, in every run, the master never drives a 1 on a bank 0 bit against a device pulling that wire low. If a
harness puts something else on bank 0's bits 5 or 6, those are driven 1 during the same pulses.

The translator's direction leads the FPGA's driver by one clock (16 ns) when a wire is pulled low and trails it when it is
released (`gb_cart.sv`'s rule), so the two never drive against each other. The price is one clock at each edge with the translator
outward and the FPGA's pin let go; whether that shows on a real harness as a runt pulse is not measured (none has been tried
with this): the first thing to look at with a scope if a device misbehaves.

The pins are the same that the Game Boy cartridge and the video DAC arbitrate for (`an_own`, decided at the core's start, see
"Who owns the port" above): the Joybus has bank 0 and pins 30 and 31 only when the adapter owns the port **and** the runtime has
turned it on (`SNAC.CTRL.ON`, set by the runtime). `core_top.sv`'s existing assignments for those pins gain one more case, `sn_en`;
there is still a single writer for each pin.

### The switch: the menu's "Analogizer"

The menu entry "Analogizer" (it was "Analogizer video"; the values it had are the same, so a saved setting stays) decides it: **Off** leaves
the Joybus off, and every other choice turns it on -- a video mode (RGBS ... RGBHV, values 1 to 9) with its picture, or **N64 SNAC** (value 10),
which is the controllers alone: the DAC's pins as with Off, no picture. That is the whole policy (`n64rt_snac_sync`, `INTERACT2` bits 3:0). The
runtime looks at it at the core's start and at every run of the PIF's commands, so changing it while a game runs turns the controllers on or off at the
game's next poll (turning them on costs one pause of 26 ms, for the scan's first sweeps, which the game's clock does not see); there is no need to
restart. A game that has asked for its controllers before the change keeps what it found (a game looks for its controllers when it starts, as with the
Controller 2 pak): Restart Game after choosing.

**A Game Boy cartridge in the slot, with the menu not Off.** The pins are the cartridge's: bank 0 is its PHI, /WR, /RD and /CS, pin 30 its /RES,
pin 31 its VIN. A low pulse there would be a write or a reset the cartridge's save would pay for, so the hardware does not let the menu decide
the pins: the Joybus gets them (`sn_en` in `core_top.sv`) only when the adapter owns the port (`an_own`: the header test of "Who owns the port", made
once at the core's start), and `an_own` is 0 with a Game Boy cartridge. So the menu turns the Joybus on in the runtime, which says so in its log, but
nothing is driven, the scan does not run (the wires are read as idle, because bank 0 carries the cartridge's PHI clock, which would look like a device) and
nobody is present: controllers 1 to 4 stay the Pocket's and the virtual ones. This keeps the existing arbitration as it was (the same signal gives
the DAC's pins and the cartridge's their owner) and there is no pin that two drivers can fight on. The simulations check it: the bench reports any
Joybus pin driven without the adapter owning the port, and a cartridge's PHI on the wires makes no device appear. To use real controllers, take
the cartridge out (the owner is decided at the core's start).

How safe the rest is:

1. **Default.** Off: nothing is driven and nothing is scanned, as before this feature. The default of the entry is Off.
2. **Nothing connected, or an adapter without a SNAC board, with the menu on.** The scan is the hardware's; the runtime only reads `STAT`
   in each run of the PIF's commands and waits for the wire only for a transaction on a port that is present, so the PIF's timing does not change. A
   wire with nothing on it either floats high and times out after 64 us, or reads low and the master gives up after 255 us: no port becomes
   present, because a device needs **two answers in a row** of three whole bytes (and goes after three misses). The virtual channels are as before.
3. **What the pins see.** Only low pulses (never a 1: ports 1 and 2 are open drain; on ports 3 and 4 the shared bank 0 drives the
   other lines 1, the level the pull-ups give, for the pulse). A scan is one status command (`0x00`): 8 cells of 3 us low in 4 us and a stop
   bit, about 36 us of the wire (about 150 us with a device's 3-byte reply), one port every 2 ms, so each port every 8 ms and the wire
   busy at most 7 % of the time.

The risk this does not remove: a SNAC harness for another kind of pad (NES, SNES, DB15, plugged into the same connector for another
core) has push-pull outputs on some of these lines, and a low pulse against one of them is a short of a few microseconds every 8 ms
through the translator's output. That harness is not usable with this core, so it should not be connected while the menu is not Off.

### What it takes and what it does not

Taken: any device that speaks the 1-wire Joybus at the console's cell timing, with commands of up to 63 bytes each way, on
four ports (one transaction at a time on the wire, as a console's PIF does). Limits: the commands of the game's block run one
after the other and the runtime waits for each, so the game is held for the wire's time (on a console the CPU goes on while the PIF
works; the SIDONE time above keeps the SI's side right); the extra bytes of an overlong reply are not reported (bit 6 reports short
ones only); the GameCube controller's timing and a Joybus device on the Pocket side are not part of this. The pins are 3.3 V CMOS;
the harness must provide the controllers' supply and the pull-ups.

### Resources

The SNAC block (`an_snac_pif` with `an_joybus_m`) is about 150 ALMs (the master 87, the sequencer, the scan and the pins 62), 170 registers and one M10K block (the 128-byte
buffer), no DSP block; the register block and the runtime add nothing of note (the runtime grows by 1.2 KiB). Measured in the fit of the whole core.
The core with it closes timing on some seeds only, as before (14 of the 15 tried did not).

### Tests

`sim/analogizer/snac/run_tests.sh` (Verilator): the master and the mux against behavioural devices (`jb_dev.sv`): cell widths and periods
of every command on every port, replies, pak reads and writes with CRC, an absent device, bit cells from -20 % to +50 %, a late
reply, a short reply, a device that talks on, a noisy line, a wire held low, a flaky contact that never becomes present, the scan with a
device coming and going, and the pins' rules (never a 1 against a device, the driver and translator order).
`sim/mips_soc/snac_check.sh` (the whole SoC with the runtime): `lang/n64rt/test/snac_game.c` drives the PIF with the SI's DMA as libultra does
(status, buttons, pak blocks with their CRC, the EEPROM) against the same device models on the four wires, in five runs: the menu Off with devices
on the wires (nothing is seen), N64 SNAC with devices on ports 1, 2 and 4, RGBS with a device on port 2 only (the Pocket's pad on controller 1 with a
virtual Controller Pak), a Game Boy cartridge in the slot with the menu on (the pins stay the cartridge's, no device is seen), and the menu changed from
Off to N64 SNAC while the game runs.
