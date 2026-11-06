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
port and are not used by the video; bank 0 is driven high.

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
Analogizer takes the port and shows the video mode of "Analogizer video". The
probe puts addresses and /RD on the pins the adapter reads as video for a few
milliseconds, once, at the core's start; the slot is not hot-pluggable, so
that is the only time it is looked at.

## Settings

One entry in the core's menu: "Analogizer video": Off, RGBS, RGsB, YPbPr, Y/C NTSC,
Y/C PAL, RGBHV, and RGBHV with scanlines 25 %, 50 %, 75 % (the scanlines dim the odd lines, which only the doubled
RGBHV picture has). The Pocket's own picture stays on.

The Pocket's menu has two limits, both measured on a Pocket: it shows only 14 of the core's entries (a 15th loads without
an error and is silently left out, so "Restart Game" has to stay the 14th, the last), and it refuses an `interact.json`
whose lists have more than about 75 options in all ("Load error in 'interact'": 75 loads, 77 does not, 76 untried). The
core's menu has 14 entries and 67 options: the two sticks share one "Stick dead zone" (steps of 10 %, 0 to 70 %) to make
room for the video entry, and the Expansion Pak and the Gameshark codes share one list ("Expansion Pak/Gameshark") to make
room for "Video settings" ([video_options.md](video_options.md)).
