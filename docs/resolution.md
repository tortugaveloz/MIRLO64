# MIRLO 64: the video output

`rtl/soc/n64_video.sv` reads the frame buffer the game's VI registers name and sends it to the Pocket. This page is the
frame it sends, its timing, and how each kind of picture is shown in it. The VI's registers and interrupt are in
[mips.md](mips.md), the filters in [rdp.md](rdp.md) (The scan-out), the registers the runtime writes in
[control.md](control.md) (`VIDEO_FRAMEBUFFER`).

## The frame

The Pocket is sent one frame size, 320 x 240, progressive: the single scaler mode of
`pkg/pocket/Cores/tortuga.Mirlo64/video.json` (4:3). The pixel clock is `clk_vid`, 12.5 MHz (the system clock divided
by 5); a pixel lasts two clocks.

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

The VI's window gives the lines shown (237 of libultra's modes; the rest of the 240 are black).

| Frame buffer | How it is shown |
|---|---|
| 320 pixels wide | A pixel a pixel, with the VI's filters as the game sets them (anti-alias, divot, dither filter, gamma) |
| Narrower than 320 | A pixel a pixel, centred, with black around it |
| 512 pixels wide or more (a 640-wide picture), 240 rows | At half its width: each pixel shown is the average of a pair, per channel. The VI's filters are off |
| The same, 480 rows (an interlaced mode) | At half its width, and its first field alone: the buffer's even rows. The VI's filters are off |
| Between 320 and 512 pixels wide | Not handled: the rows are read 320 pixels apart and the picture is garbled |

The runtime works these out of the VI's registers (`lang/n64rt/n64rt_io.c`, `vi_width_gamma`) and writes them to
`VIDEO_FRAMEBUFFER.WIDTH` and `WINDOW`: the width, whether the picture has 480 rows (the rows its window shows, by the
VI's vertical scale, come to more than 300), and a smaller picture's place on the screen.

`VI_H_START` 0 (libultra's `osViBlack`) shows black. A frame buffer so close to the end of the RDRAM that the picture
would run past it shows black too.

## The core's settings

"Crop Vertical" (none, 8 or 12 lines) blanks that many lines at the top and at the bottom of the picture, and "Crop
Right" (none or 1 column) its last column. They are relative to the picture, a centred one too; the frame and its
timing do not change.
