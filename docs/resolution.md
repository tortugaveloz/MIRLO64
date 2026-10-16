# MIRLO 64: the video output

`rtl/soc/n64_video.sv` reads the frame buffer the game's VI registers name and sends the Pocket what the N64's VI
sends a television. This page is the frame, its timing, and how each picture is placed in it. The VI's registers and
interrupt are in [mips.md](mips.md), the filters in [rdp.md](rdp.md) (The scan-out), the registers the runtime writes in
[control.md](control.md) (`VIDEO_FRAMEBUFFER`).

## The frame

The Pocket is sent the VI's fields: 640 x 240, progressive, or the fields of a 640 x 480 interlaced picture (below). The
scaler modes of `pkg/pocket/Cores/tortuga.Mirlo64/video.json` are 640 x 240 (0) and 640 x 480 (1), both shown 4:3. A line
is the VI's own, 640 pixel clocks wide; the pixel clock is `clk_vid`, 12.5 MHz (the system clock divided by 5), one clock
a pixel.

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
- **A serrated picture** is sent as 480i (below).
- **A window taller than the field** from its first line (some PAL modes stretch a 240-row buffer over more lines than
  the frame has) is fitted to what is left of the field, not cut: it has fewer lines and a proportionally larger step
  through the rows, so its first and last lines show the same rows as the VI's (`lang/n64rt/n64rt_vifit.h`).

The runtime works the window, the scales and the words to read for a row out of the VI's registers
(`lang/n64rt/n64rt_io.c`, `vi_width_gamma`) and writes them to `VIDEO_FRAMEBUFFER`.

`VI_H_START` 0 (libultra's `osViBlack`) shows black. A frame buffer so close to the end of the RDRAM that the picture
would run past it shows black too.

## Interlaced pictures

A serrated picture (`VI_STATUS`' SERRATE with a hi-res `VI_WIDTH`: 640 x 480 as two fields of 240 lines) is sent as the
VI sends it, a field each 59.94 Hz frame, and the Pocket weaves the fields into its 640 x 480 scaler mode.

- **The field** is the VI's: `VI_CURRENT`'s bit 0 alternates every field while SERRATE is set, and libultra's retrace
  handler answers it by writing that field's `VI_ORIGIN` (the odd field's one buffer row on) and `VI_V_START`. The
  scan-out reads each field from the origin written last, by the same path as any picture: `VI_Y_SCALE` 2.0 steps two rows
  a line (or `VI_WIDTH` 1280 with 1.0, a pitch of two rows).
- **The Pocket is told** by the words its video interface takes outside DE: after each line's DE the scaler mode of the
  next frame (1 for a field, 0 for a progressive picture), and during VS the next frame's features: interlaced, odd for
  the VI's odd field, and last on every field. Each field then goes on the lines its odd flag names and the other field's
  stay: the weave. (With the last flag on one field of two, the Pocket put both fields on the same lines and left the
  others blank.)
- **A change** between a progressive picture and a serrated one costs one black frame: the first frame of the new kind
  is not the one the frame before announced.

The field's timing is the progressive one: 264 lines, VS at the same place in both fields.

## The core's settings

"Crop Vertical" (none, 8 or 12 lines) blanks that many lines at the top and at the bottom of the picture, and "Crop
Right" (none or 1 column) its last column, a column being a pixel of a 320-wide picture. They are relative to the
picture's window; the frame and its timing do not change.

## Tests

`sim/vi_filter/regress.sh` builds the scan-out alone against a model of it (`tb_vi.cpp`) and checks the pads pixel for
pixel: the filters, pitches and scales other than 320, windows, the crop settings, serrated pictures (each field's rows,
the slot words and the features), PAL and a change of TV system between fields. `sim/vi_filter/vifit_test.c` checks the
fitted window's arithmetic. `lang/n64rt/test/il_test.c` is a game that drives a serrated picture as libultra does: on the
SoC in `sim/mips_soc` (`VGA=1 VGAT=1`: each frame's slot and features, and the fields woven as the Pocket weaves them, in
`w*.ppm`) or on a Pocket, where `ROWCODE` puts each row's number in it as a bar code to read off a capture.
