# MIRLO 64: the video output

`rtl/soc/n64_video.sv` reads the frame buffer the game's VI registers name and sends the Pocket what the N64's VI
sends a television. This page is the frame, its timing, and how each picture is placed in it. The VI's registers and
interrupt are in [mips.md](mips.md), the filters in [rdp.md](rdp.md) (The scan-out), the registers the runtime writes in
[control.md](control.md) (`VIDEO_FRAMEBUFFER`).

## The frame

Original sends a 640 x 240 progressive frame: scaler slot 0 of
`pkg/pocket/Cores/tortuga.Mirlo64/video.json`, shown 4:3. A line is the VI's own, 640 pixel clocks wide; the pixel clock
is `clk_vid`, 12.5 MHz (the system clock divided by 5), one clock a pixel.

| TV system | Clocks a line | Lines a field | A field | Rate |
|---|---|---|---|---|
| NTSC (and MPAL) | 790 (nine lines in vblank: 788) | 264 | 1,042,710 system clocks | 59.940 Hz |
| PAL | 800 (fifty lines in vblank: 792) | 313 | 1,250,000 system clocks | 50.000 Hz |

The field's length is the N64's for each system, so the VI's line counter and its interrupt keep the console's timing.
The picture is 240 lines in both; a PAL field has more lines of blanking, not a taller picture.

The timing never stalls: the frame's words go from the SDRAM to the pads through a FIFO of 16,384 words, and a word
that is not there in time shows black. The frame buffer's address is taken at the frame boundary, the first line of
vblank, so a new one never tears the frame being sent.

## The TV system

Until the game programs the VI, the system is the one of the ROM's region: the runtime writes it to bit 18 of
`VIDEO_FRAMEBUFFER.WIDTH`. From then on it is what the game's `VI_V_SYNC` says: 525 lines (a value of 512 to 575) is
NTSC, 625 lines (576 to 703) is PAL, and any other value changes nothing. The system is taken at the frame boundary and
the timing changes where that field ends, so no field is cut short. Bit 19 of the same register reads the system in
use.

## The picture

The picture is the VI's, by the game's own registers:

| VI register | What the scan-out does with it |
|---|---|
| `VI_ORIGIN` | The frame buffer's address |
| `VI_WIDTH` | The pitch: the pixels from one row of the buffer to the next |
| `VI_H_START` | The picture's window in the line: its first clock and its width, counted from the VI's first visible clock (108; 128 for PAL) |
| `VI_V_START` | Its window in the field: its first line and its lines, counted from the VI's first visible half-line (37; 95 for PAL) |
| `VI_X_SCALE` | The step through a row, in 1/1024 of a pixel for each clock of the window: clock `i` shows the pixel `(i * X_SCALE) >> 10` |
| `VI_Y_SCALE` | The step through the rows for each line of the window: line `j` shows the row `(j * Y_SCALE) >> 10` |

Outside the window the frame is black. The pixel and the row are the nearest ones, not interpolated; the VI's filters
(anti-alias, divot, dither filter, gamma) work on the frame buffer's own pixels before the scale, as the VI's do. So
one path shows every size: a 320-wide buffer with a scale of 0.5 (each pixel two clocks), a wider one with its own
scale, and a 640-wide one with a scale of 1.

Three cases are not the plain rule:

- **A picture 512 pixels wide or more** is shown with the VI's filters off.
- **A serrated picture** (the VI's interlaced modes with a 640-wide buffer: 480 rows as two fields of 240) is not sent
  as two fields: it is the same 640 x 240 progressive frame, and each line is the average, per channel, of the rows
  `2k` and `2k + 1` of the picture, which is what a television mixes of the two fields.
- **A window taller than the field** from its first line (some PAL modes stretch a 240-row buffer over more lines than
  the frame has) is fitted to what is left of the field, not cut: it has fewer lines and a proportionally larger step
  through the rows, so its first and last lines show the same rows as the VI's (`lang/n64rt/n64rt_vifit.h`).

The runtime works the window, the scales and the words to read for a row out of the VI's registers
(`lang/n64rt/n64rt_io.c`, `vi_width_gamma`) and writes them to `VIDEO_FRAMEBUFFER`.

`VI_H_START` 0 (libultra's `osViBlack`) shows black. A frame buffer so close to the end of the RDRAM that the picture
would run past it shows black too.

## The core's settings

"Crop Vertical" (none, 8 or 12 lines) blanks that many lines at the top and at the bottom of the picture, and "Crop
Right" (none or 1 column) its last column, a column being a pixel of a 320-wide picture. They are relative to the
picture's window; Original keeps the same frame and timing.

"Screen Size" adds Fit, Stretch, Hybrid and Overscale after this scan-out. The
adapter captures the full 640 x 240 field without reducing its horizontal
samples. Native framing uses the Pocket's scaler; other sizes and Hybrid use
bilinear filtering. See [display-modes.md](display-modes.md) for presets,
aspect correction, latency and memory use.

## Tests

`sim/vi_filter/regress.sh` builds the scan-out alone against a model of it (`tb_vi.cpp`) and checks the pads pixel for
pixel: the filters, pitches and scales other than 320, windows, the crop settings, serrated pictures, PAL and a change
of TV system between fields. `sim/vi_filter/vifit_test.c` checks the fitted window's arithmetic.
