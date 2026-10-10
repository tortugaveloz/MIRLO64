# Resolution

(The section "Two modes" below describes the earlier cores, with a 268 x 240 frame; the N64 core's frames are the next section's.)

## Two modes, chosen by the game file (Mirlo)

The core runs 268 x 240 (the default: fills the Pocket's screen, the N64's
320-wide frame cropped 26 px a side, 2D moved in) or the N64's own 320 x 240
(4:3, nothing cropped or moved). The game file's header picks: byte 26 of the
MIRLODATA footer, 0 = 268, 1 = 320 (`tools/make_mirlo_game.py --video 320`).
Older files read 0. There is no menu override: a game runs in the mode its
file asks for.

How it switches, at boot (lang/c/game/frame.c `frame_set_video`, called by
the program with the game file header's choice before `frame_init()`):
the scan-out's `vtg_mode` register (rtl/soc/n64_video.sv: two constant
timing sets, 340 x 280 and 360 x 264 -- since replaced by the one 320 x 240 frame, see the section below,
~60 Hz, which also set the frame's length), the Pocket's scaler slot
(sent on the RGB bus during blanking; video.json slot 0 = 268, slot 1 = 320),
the clears' width and the geom core's screen edges (GDL_FBSIZE).

The older, manual procedure below (a new pixel clock and timings) still
applies for any other resolution.

## Frame size (N64 games: the VI's own, 640 x 240)

(2026-10-10, branch `own-ge-vi`; before it the frame was 320 x 240 with the old sections' hi-res reduction and window below, still
in the history of `rtl/soc/n64_video.sv`.) `rtl/soc/n64_video.sv` sends the Pocket ONE frame, the scaler slot 0 of
`video.json`, 640 x 240 (aspect 4:3), as the N64's VI sends its picture: a line of the VI's 640 pixel clocks (H_START 108 to
H_END 748), 240 lines a field, progressive:

| slot | frame       | clocks a pixel | clocks a line | lines a field | rate          |
|------|-------------|----------------|---------------|---------------|---------------|
| 0    | 640 x 240   | 1              | 790 (nine: 788) | 264         | 59.940 Hz     |
| 0    | 640 x 240   | 1              | 800 (fifty: 792) | 313        | 50.000 Hz (PAL) |

On the pixel clock `clk_vid` = sys / 5 (12.5 MHz, `mf_pllbase`'s outputs 3 and 4, unchanged): a pixel a clock (the Pocket
asks for one clock a pixel; `video_skip` is never high now). The line and field lengths in clocks are the old 320-wide frame's
(790 = 395 pixels of two clocks, 264 lines, nine lines two clocks shorter), so a field is the same 1,042,710 system clocks =
59.940 Hz (V_SYNC 0x20D, H_SYNC 0xC15 of every NTSC libultra mode) and the VI's field timing (the runtime's VI_PERIOD) does not
move. DE is 640 clocks and HS 48 clocks after it, a clock of DE gap each side as the APF wants; nothing is sent outside DE.

**What a pixel of it is.** The VI's registers place the picture in that frame, the way the console's do:

- the window in the line: H_START / H_END (`VIDEO_FRAMEBUFFER_HWINDOW`: the first clock `px0` = H_START - 108 -- PAL 128 --
  and the width `pw` = H_END - H_START, in clocks; Mario Tennis' is 20 and 597) and in the field: V_START / V_END
  (`VIDEO_FRAMEBUFFER_WINDOW`: its first line `y0` = (V_START - 37) / 2 -- PAL 95 -- and its `rows` = (V_END - V_START) / 2:
  libultra's modes are 237 lines, the last three of the 240 are black as on a TV), black around it;
- the window's pixel i shows the framebuffer's pixel floor(i X_SCALE / 1024) (`VIDEO_FRAMEBUFFER_SCALE`, 2.10 numbers: 0x200
  doubles a 320-wide picture -- every lo-res game --, 0x2C0 shows GoldenEye's 440 whole, 0x400 is a hi-res 640, 0x20E Mario
  Tennis' 384) and its line j the row floor(j Y_SCALE / 1024) (0x400 a row a line, 0x580 GoldenEye's 330 rows in 240
  lines, 0x800 every other row: the even rows of a hi-res buffer without SERRATE; a SERRATED one is the average of each pair of rows,
  see "Serrated pictures" below), the rows `WIDTH` pixels apart (`VIDEO_FRAMEBUFFER_WIDTH`: GoldenEye's 440, Donkey Kong 64's 1280 = two
  rows of 640 with Y_SCALE 1.0). Nearest pixel and row, as the VI without its filters; X_SCALE up to 1.0 and Y_SCALE up to 2.0
  are what the arithmetic takes (the runtime clamps);
- the VI's filters (AA, divot, dither: MIRLO_VI_AA) work on the framebuffer's own pixels, whatever the scale -- the filter
  pipeline's steps are source pixels, not clocks: a step at each clock the X_SCALE accumulator carries (its fraction is 10
  bits, the carry the next pixel), and a line's first nine in the blanking before it (priming: two empty columns for the left
  taps, then seven pixels, so that the first filtered pixel is out when the window opens). The filters' rows above and
  below are the rows fetched, with Y_SCALE's skipped or repeated rows (a GoldenEye line's neighbours are the rows the accumulator
  reached). A hi-res picture (WIDTH 512 or more) has them off, as before: no hidden coverage plane is read for it.

**The reads.** A row is `ROWWORDS` words from its first (the runtime: the window's last pixel's source pixel plus the
priming's seven, two to a word, and one more: 164 for a 320-wide mode, 324 for a hi-res one), and the next row the one
Y_SCALE's accumulator reaches -- 0, 1 or 2 pitches on. Each row's first word has a flag in the FIFO, and the vid side
starts each line at one, dropping what the last line left (a late or a short row costs one line, not the rest of the frame). The
FIFO is the old one (64 KiB, 38 bits a word now, the same M10K blocks); 18.4 MB/s for a hi-res picture or a 320-wide one, as
before (the worst case: 320 words a row, 240 rows; GoldenEye's 220 words and the hidden plane's eighth are fewer). The line
buffers of the filters are 1024 pixels deep (a hi-res line goes through them with the filters off: two more M10K).

`tb_vi` checks every pad against a model (`sim/vi_filter/regress.sh`: 320, 440, 384, 292 and hi-res pictures, windows, steps, crop,
PAL and the TV system changing between fields); a bare program that sets nothing (WIDTH 0, SCALE 0x200 / 0x400) is a 320 x 240
picture doubled.

**PAL (2026-10-10, branch `own-pal`).** The same slot and pixel clock, with a PAL field: 313 lines of 800 clk_vid clocks (64 us:
the console's PAL line is 3178 clocks of 49.6565 MHz) and fifty of them (lines 253..302, in vblank) eight clocks shorter:
263 x 800 + 50 x 792 = 250,000 clk_vid clocks = **1,250,000 system clocks = exactly 50.000 Hz**
(V_SYNC 0x271, H_SYNC 0xC69 of libultra's PAL modes: 312.5 lines of 3178 clocks = 993,125 VI clocks of 49.6565 MHz = 20.000 ms).
libultra's PAL modes show 237 lines like the NTSC ones (V_START 95..569 half-lines against 37..511). Which system is in use:
`VIDEO_FRAMEBUFFER_WIDTH` bit 18 (the runtime writes it from the ROM's region, `n64rt_tv`) until the game writes a V_SYNC that
names one -- 525 lines (512..575) NTSC, 625 (576..703) PAL; anything else (0, noise) changes nothing --, then that. The system is
taken at the scan-out's frame boundary and applied where the field ends, so no field is cut short (tb_vi checks the 8
transitions of a sequence line by line); bit 19 of the same register reads the system in use. MPAL is NTSC here (525 lines, 59.9
Hz). 50 Hz is inside the 47..61 Hz the scaler takes (not yet seen on a Pocket).

**PAL windows (two regressions of the VI's own window, 6599c53, fixed in branch `own-p240`; Mario Golf (Europe) and SM64 (Europe) were cut).** The field has 240
lines; the VI's window is (V_END - V_START) / 2 of them from the line (V_START - 37) / 2 (PAL: - 95; the clocks: H_START - 108, PAL - 128). The 240-line
scan-out before 6599c53 ignored all that for a window of 228 lines or more (the buffer's 240 rows in the 240 lines, 1:1, from the first) and centred a shorter one, so a
PAL mode showed whole. The two causes, read from the registers in the sim:

1. *The origin of the wrong TV system.* The runtime took the window's origins from the system the scan-out was USING (WIDTH bit 19, which follows the VI's request
   -- V_SYNC, else the ROM's region -- at the scan-out's next frame boundary), at the event of the first write of V_START. A PAL game's registers (V_SYNC 0x271 first)
   were read with NTSC's origin 37 when the system in use was still NTSC (in the sim the scan-out stays NTSC, "V_SYNC -", until the game's first V_SYNC, although the ROM's region is PAL), and a mode that is written once (SM64 (Europe): V_SYNC 0x271, V_START 0x005F0239, 237 lines) is never recomputed: **y0 = (95 - 37) / 2 = 29, rows
   237 clipped to 240 - 29 = 211** -- the picture 29 lines low and its last 26 lines gone (sim: "window 0/29/211" for the whole run). Now the origins are those of the
   system the registers ask for (`vi_tv_pal`: V_SYNC 525 / 625, else the region; as `rcp_vi`'s `vi_pal`): 0 / 237, as the console's.
2. *A window taller than the field.* Mario Golf (Europe) writes, past its first screen (H_START 0, black), V_SYNC 0x271, H_START 0x800300, **V_START 0x002F0269 -- 285
   lines from the half-line 47, before the 95 the field starts at --, Y_SCALE 0x35C, WIDTH 320**: a 240-row buffer stretched by 0.84 over the 285 lines of a PAL
   picture. It was clipped to the field's 240 lines (y0 0, rows 240, Y_SCALE 0x35C): its line 239 is the buffer's row 201, **the last 38 rows of the buffer cut**.
   A window with more lines than the field has from its first (`lang/n64rt/n64rt_vifit.h`, used by `vi_width_gamma` and `vi_pic_bytes`) is now FITTED to them: the rows are
   the field's and the Y_SCALE step larger by the ratio of the lines, so that the first and last lines show the rows of the buffer the VI's first and last do (0x35C * 284 /
   239 = 0x3FD, the last line the row 238 as the VI's 285th: the buffer's row 239 is not in the VI's window either), and the picture is squeezed vertically by 285 / 240
   as the old scan-out squeezed it. 237-line modes (SM64, GoldenEye's 330 rows in 237 lines, Banjo-Kazooie's 216) and shorter ones are unchanged.

`sim/vi_filter/vifit_test.c` (the fit's arithmetic, with Mario Golf's numbers) and `tb_vi`'s PAL cases (237 lines, the 240 lines with Y_SCALE 0x3FD, the crop, NTSC <->
PAL) are in `regress.sh`. A scaler slot of 288 lines for PAL would show the 285 lines of Mario Golf's mode whole, but only by stretching its 240-row buffer: no more
information than the 240-line frame, and it is not built (no PAL mode seen has more than 240 rows of picture; the taller windows are stretches).

**Serrated pictures (VI_CTRL SERRATE with a hi-res WIDTH -- Pokemon Stadium's menus, Donkey Kong 64's logos): 640 x 240, progressive, the rows of each
pair averaged.** (2026-10-10, branch `own-p240`.) The console shows such a picture as 480i: two fields of 240 lines, the picture's even rows then its odd
ones (WIDTH 640 with Y_SCALE 2.0, libultra's own interlaced modes; or WIDTH 1280, Y_SCALE 1.0, two rows' pitch, DK64's), the field's own ORIGIN written at each
retrace (the second is the first one 640-pixel row on, 1280 bytes). The core used to send that to the Pocket as 480i (scaler slot 1, 640 x 480, and frame
feature bits in the VS: a field a frame, the second field's ORIGIN kept by `rcp_vi.sv`). That is gone: the Pocket's handling of an interlaced slot is not
documented where the words are stated (the field height of an interlaced slot, which field is "odd", where "last field" goes), a first attempt showed every other
row black, a menu of eight variants found one that showed Stadium right on one trial, and a field's signalling that does not depend on the game's retrace
timing could not be made reliable in hardware -- Stadium looked wrong again after several rewrites. For now (the owner's decision) a serrated picture is shown
as the same frame as every other picture, the one scaler slot 0 (640 x 240, `video.json`), without interlace and without a slot word or feature bits.

What a line of it is: the **average of the picture's rows 2k and 2k + 1** (a box filter of two rows, which is what a CRT's persistence mixes of the two
fields), each channel (a + b + 1) >> 1 on the 5551 pixel's five bits (the coverage bit the AND of the two), the picture's 640 pixels a row as they are (X_SCALE 1.0).
The base is the lower origin of the pair `rcp_vi.sv` keeps (the write of the base + one row, 1280 bytes, leaves it, as for any hi-res picture), so the rows are the
whole 640 x 480 frame and not the game's alternating field. The averaging applies when `rcp_vi`'s `il` (SERRATE and WIDTH 512 or more) is set and the picture is the two rows a line of libultra's and DK64's modes: WIDTH 1280
(a pitch of two 640-pixel rows, Y_SCALE 1.0) or WIDTH 640 with Y_SCALE of 1.5 or more (the runtime's 0x7FF: 2.0). A serrated 240-row buffer (WIDTH 640,
Y_SCALE 1.0) is not a pair of fields, and any other pitch is not a multiple of the 640-pixel rows (320 words) the pair's addresses assume: both are shown as
any hi-res picture without SERRATE is, the nearest row of Y_SCALE (the first field's rows, filters off). A hi-res picture without SERRATE keeps the nearest rows
of its Y_SCALE (the even rows of a 640 x 480 buffer). The VI's filters stay off for every hi-res picture, as before.

The reads: the two rows of a pair are fetched a word each in turn (the A word, then the B word one picture row on, 320 words further), the scan-out's read engine
keeps the A word and writes the average of the pair to the FIFO, so that the FIFO, its words, the vid side and the line buffers are the same as for any other
picture. The pair's two addresses are in different banks of the SDRAM, or in the same page, so both pages stay open (`sdr_ctrl`: open page per bank): the stream
costs a word a cycle as before. A serrated frame reads 640 words a row, 240 rows = 153,600 words a field (36.8 MB/s at 59.94 Hz: 14.7 % of the SDRAM's cycles at a
word a cycle; the picture before this read 76,800 words, 18.4 MB/s, 7.4 %); the arbiter's round robin (6 ports, 16 words a grant, no priority for the scan-out)
gives it 1/6 = 16.7 % at its worst, all the other ports asking, and the 16,384-word FIFO (51 lines of output) rides out the rest -- `tb_vi`'s `TB_LOAD=5` models
exactly that grant and shows no pixel lost; one more master (`TB_LOAD=6`, 14.3 %) does lose pixels. If it were short, the fall-back is the first field alone
(every other row: no `B` reads).

Area (quartus_map, seed 34, the whole core, estimate of ALMs): 16,598 before (`own-tlbm`, 810230b), 16,689 after (+91, +41 registers): the scan-out's
word register and averaging adders and its stage in front of the FIFO against the 480i machinery and `rcp_vi`'s second origin gone; the blocks' bits are the same.
(The ALM figure's own noise between two maps of near-identical trees is of the order of 70: an untouched `rsp_vu` moved by +65 ALUTs.)

The timing is the progressive one (264 lines, 1,042,710 system clocks, 59.94 Hz; PAL 313 lines, 50.000 Hz). `tb_vi` (`sim/vi_filter/regress.sh`) checks the
average against its model for 640-pitch (`640s`) and 1280-pitch (`1280s`) pictures, DK64's 237 lines, a window, the crop options, PAL, the TV system
changing between fields, and the SDRAM loaded; and that the pads outside DE are 0 in every mode.

Outside DE the pads are 0 for every picture: the scaler slot is always 0, there is no scaler-slot word and no frame feature bits (`core_top.sv` passes nothing but the picture).

## Changing the resolution by hand

If you don't like the default resolution selected by the core, you can change it to anything supported by the Analogue Pocket. Remember openFPGA is limited to 800x720, so you can't choose a resolution outside of those bounds.

1. Find your desired vertical and horizontal counts. [See agg23's wiki](https://github.com/agg23/analogue-pocket-utils/wiki/Video) for additional information about how to select counts, but briefly, visit https://tomverbeure.github.io/video_timings_calculator and enter your desired resolution. It will show you the "conforms to protocol" options that you can select between, but you are not required to use one of these for the Pocket; it is just an example. If you choose an unusual resolution (like the default 266x240), there won't be a standard to base the calculations off of, so the tool will estimate what it would be.  
Note that you want all of your system clocks (CPU, and 2x for SDRAM) to be integer multiples of this clock, and to pass timing you probably need the CPU clock to be < 60MHz. For example, you could choose counts that require a 10MHz clock, which would either be a 5x or 6x multiple (50MHz or 60MHz) for the CPU clock.

2. In Quartus, open the `mf_pllbase` megafunction on the left side of the screen. This will open the tool where you can manipulate the clocks for the core. The last two clocks should be the video pixel clock that you chose, the first is your CPU clock (remember to keep it an integer multiple of the video clock), and the second and third clocks are your SDRAM clock (remember to keep it 2x the CPU clock). Enter those values and click finish.

3. Edit the clock constants: `CLOCK_HZ` in `tools/mirlo_regs.py` (then run it) and `SYS_HZ` in `target/pocket/core_top.sv`. Put your counts into `rtl/soc/n64_video.sv` (its two timing sets and the frame's word counts).

4. Update the Pocket `video.json` to your new resolution. Without it, the Pocket won't know what resolution you want to display.

5. Rebuild the firmware that has the clock compiled in (see the top-level README's build steps).

6. Build the Quartus project by pressing "Build".

7. Reverse the produced bitstream (`*.rbf`). See https://www.analogue.co/developer/docs/packaging-a-core#fpga-bitstream

8. Update your clock speed constants in your software code, if any.

9. The core should now be ready to use at the new resolution.
