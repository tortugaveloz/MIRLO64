# The video settings: 32-bit and Deblur

One entry of the core's menu, "Video settings", with four choices: **Default**, **32-bit**, **Deblur** and **32-bit + Deblur**.
The default is the console's picture, bit for bit what the core showed before the entry existed. The entry is
`interact.json` field 17 (`0x1000_0144`, `INTERACT2[9:5]`): bit 0 is the 32-bit output, bit 1 Deblur. A change takes
effect at once, no restart.

## What the console's picture is made of

A game's colour image is RGBA5551 (or RGBA8888, [cimg32.md](cimg32.md)); the VI turns it into the eight bits a channel
of the video signal:

1. The RDP draws into a 16-bit image by taking the blender's eight bits to five, after adding an ordered (or noise)
   dither of three bits, so a smooth surface does not show five-bit bands (`RGB_DITHER_SEL`, the game's choice).
2. The VI reads the image. A pixel whose coverage is not complete is an edge: the **anti-alias** filter blends it with
   the neighbours. Where a neighbour on the line is an edge, the **divot** filter takes the median of three. A complete
   pixel goes through the **dither filter** (`VI_CTRL` bit 16): +1 or -1 for each of the eight neighbours above or below
   it, which undoes the dither's pattern. Without the filter a five-bit channel is shown as `c << 3` (white is 248,
   as on the console).
3. The picture is scaled by the VI's window and steps, **to the nearest pixel and row**: there is no interpolation in
   this scan-out, so the scale adds no blur of its own. The Pocket's scaler works after it and is not the core's.

## 32-bit

What it does:

* **The RDP's RGB dither is off**, whatever the game asks for (`rdp_pix.v`, `dither_off`: the dither threshold is the
  one that never steps a channel up, and the noise dither is not drawn). A 16-bit image gets the blender's
  channels truncated to five bits; a 32-bit image (`SET_COLOR_IMAGE` size 3) gets them whole, eight bits, where the
  console's dither (`RGB_DITHER_SEL` 0 to 2, which it applies even to a 32-bit image) leaves them at the next multiple
  of 8 or as they are. The alpha dither (the alpha compare's and the coverage's) is not touched.
* **The VI's dither filter is bypassed** (`VI_CTRL` bit 16 is ignored): it would only smooth a dither that is not there.
* **A 32-bit picture keeps its eight bits.** The scan-out used to drop the three low bits of each channel when it read
  the FIFO ([cimg32.md](cimg32.md), "What is approximated"); with the option on the centre row's line buffer carries
  them (one M10K more) and they are in the output; the anti-alias filter takes them in its centre too (its taps'
  low bits are taken to be the centre's). The anti-alias and divot filters still choose and order their taps on the top
  five bits, so on an edge the result is the console's to within the lowest bit of the taps.
* **A 16-bit picture has its eight bits too, from a shadow plane** (below).

### The shadow plane: eight bits for a 16-bit image

The image in the RDRAM stays what it is, RGBA5551, written exactly as without the option: the game, the CPU, the RSP's
DMA and a texture load of the frame see the same data. Beside it the RDP keeps a **shadow plane**, a halfword for each
halfword of the colour image, at the image's address with bit 24 set (the SoC maps it to 2 MiB at SDRAM
`0x0320_0000`, `LAYOUT_SHADOW`; the address is folded into the 2 MiB, `A[22:21]` exclusive-ored into `A[18:17]`, and its
bank is the other of the colour word's, so that a pixel's two words, which the scan-out reads one after the other, are in
two banks):

```
shadow halfword = { R[2:0], G[2:0], B[2:0]  (9 bits: the low three bits of the pixel's channels as the blender made them),
                    CRC-7 of the pixel's 5551 word  (7 bits: x^7 + x^3 + 1, the top bit first) }
```

**Writing.** Every pixel the RDP writes into a 16-bit image (a shaded or textured triangle or rectangle, and copy mode)
writes its shadow halfword with its colour word, with the same coverage and pass masks: the blender's eight bits are
there before they are truncated, and the dither is off. A fill (a fill rectangle, or a triangle in fill mode) writes the fill
colour's own top three bits of each channel again as the low bits (what the plain expansion gives) with its check.
The write engine does it as a pass after the colour pass (`rdp_mem.v`, `w_ps`), from a new buffer beside the new colour
buffer (`snew`).

**Validation.** A shadow halfword is used only where its check is the CRC-7 of the 5551 word now in the image. Anything
that changed the 5551 without writing the plane (the CPU, the RSP's DMA, a copy of a frame, a save state, a game that
reads a frame back and draws it as a texture elsewhere) leaves a halfword whose check fails, and the pixel falls back to
the plain expansion, the channel's top three bits again (`{c, c[4:2]}`, white 255, what the console's 8-bit output of
that pixel would be). A word that is changed to what its CRC-7 is again, 1 in 128 for an arbitrary change, or written
with the colour it already had, uses the old low bits: at most 7 of 255 in a channel, and only for a pixel that is
the same colour to five bits (the check is seven bits and not the fifteen bits and the coverage of a whole copy, to
keep the plane the size of the image and the scan-out's traffic at three words for two pixels).
`sim/rdp/tb_rdp.cpp` (`TB_FOREIGN`) checks both ways: of the pixels something else wrote random values to, under 2 % keep
their halfword (about 1 in 128); of those it did not, all of them do.

**Readers.**

1. **The scan-out** (`n64_video.sv`) reads each colour word after its shadow word, checks the two pixels, and carries the
   nine low bits of each pixel through the FIFO (18 bits more a word) into the pipeline that already carries a 32-bit
   picture's: the anti-alias filter's centre, the divot, the gamma table work on eight bits. Not for a **woven**
   frame (a serrated 640 x 480 picture): its 307,200 words a frame, doubled by the shadow words, are 29 % of the SDRAM's
   cycles, more than the scan-out is sure of when the other masters want it (`tb_vi` fails it at the worst case of a
   round of six ports); that picture is shown as a 16-bit picture is without a plane (the plain expansion). The picture the
   VI scales 640 wide without weaving (a 640 x 240 buffer, or every other row of a 480-row one) passes the worst case.
2. **The blender** (`rdp_pix.v`) takes the memory colour of a pixel it blends with, or reads the memory's coverage for, with
   the low bits of its shadow halfword where the check holds (the memory colour is `{c, 000}` otherwise, as on the
   console), so blends and the edges' coverage accumulate at eight bits. The colour span's shadow words are read after
   the colour words (`rdp_mem.v`, `r_shp`) into the old shadow buffer (`sold`); the pixels wait for them.
3. **A texture load** from a colour image reads the 5551 only, as the console does.

**Where it does not apply.** A picture other than 16-bit (8-bit and 32-bit images have no plane); a woven frame (above);
the image's address folded: two colour images 2 MiB apart (modulo, with `A[22:21]` mixed in) share plane space, and one
overwrites the other's halfwords, which then fail their check and show as without the option. The plane is not
saved in a Pocket save state; after a state is loaded its pixels fall back until the game draws them again.

**Costs.** In `tb_rdp` on random command streams in a 16-bit image, with the SDRAM's timing (`TB_C16=1 TB_SDRAM=1`):
the RDP takes 25 to 45 % more cycles (mean 33 %, six streams), 30 to 60 % more words read and 60 to 80 % more words written. The scan-out
reads three words for two pixels instead of two (two and a bit with the hidden plane): a 320-wide picture passes `tb_vi`
under the worst loads, a 640 x 240 one to the worst case of the SoC (five other masters). The options off: no extra traffic
(the RDP's cycles, reads and writes are the same as before the option, on every stream of the benches).

**Area.** The synthesis estimate grows by 90 ALMs and 78 registers (17,051 to 17,141 ALMs) and the RAM blocks by 44, to 283 of 308 (the FIFO
is 18 bits wider; the new and old shadow line buffers). The fit is the design at 98 %: 18,110 to 18,240 ALMs in the sixteen fits, against
17,920 to 18,170 before the plane (the fitter spreads a design this full). Four of sixteen seeds
close timing (the others fail in the CPU's paths, by up to 0.8 ns); `projects/fits.json` has them.

What it cannot do:

* **A game that blends over what the CPU drew, or draws into an image another way, shows that part as without the option.**
  That is the validation working, not a defect.
* **A game that draws with the dither on in a 16-bit image and relies on the pattern** (a noise dither as an effect) loses it.
* The eight bits are the blender's: a flat colour or a texel of five bits has whatever low bits the combiner and the blender
  make of it (a fill and a copy have the plain expansion's); a shaded surface, a lit one and a blend are where the extra
  levels show.

## Deblur

What it does: the VI's **anti-alias and divot filters are bypassed**. Every pixel is shown as the game drew it, an
edge pixel (partial coverage) at the colour it holds, not blended with its neighbours. The filters are the only thing in
the scan-out that mixes a pixel with its neighbours across the picture's edges; the scale does not (above).

What it does not do:

* The dither filter stays as the game set it: it moves a channel by a few levels of 255 to undo the dither and does
  not soften an edge (use "32-bit" to bypass it; the two combine as "32-bit + Deblur").
* An edge is jagged where the game relied on the anti-alias filter instead of drawing the edge itself: that is the
  point of the option.
* The Pocket's own scaler (the 640 x 240 slot to its screen) is not the core's; its softness is not touched.
* A picture 512 pixels wide or more already has the filters off.

## Verification

* `sim/vi_filter/regress.sh`: the scan-out against the model (`tb_vi.cpp`; `TB_DEBLUR=1`, `TB_OUT32=1`, with `TB_PX32=1`
  for a 32-bit picture, `TB_SHADOW=1` for a shadow plane filled with halfwords that are the pixels' for 70 % and noise for
  the rest): every pixel of a frame compared, scaled, windowed, hi-res, woven, PAL, cropped, under the SDRAM's load. With the
  options off the pad hash (an FNV of every pad word and sync of a run) is the same as the scan-out's before the options
  existed, for 16-bit and 32-bit pictures.
* `sim/rdp/shadow_regress.sh` (`tb_rdp` against the C model, `lang/c/rdp`, whose `shadow` and `rgb_dither_off` are the options'): random
  command streams into a 16-bit image (odd widths too, copy, rectangles, depth, SDRAM timing and the other master's accesses),
  `RTL == model` on the colour image, the hidden plane **and the shadow plane**; the same into a 32-bit and an 8-bit image
  (no plane); and `TB_FOREIGN=1`, where something else writes the colour image between draws (random pixels, and a copy of a
  region of the colour plane alone): RTL == model, the shadow halfwords of the pixels nothing else wrote all pass their check, those of
  the random pixels others wrote under 2 %. Two runs that must differ do (the model not reading the plane; the model's plane alone).
  Without the option the cycles, reads and writes of every stream are those of the RDP before it (compared on the benches' modes).
* The whole SoC (`sim/mips_soc`, the setting written as the Pocket does, `INTERACT=0x10000144:<n>`; `sim/vi_filter/picture_soc.sh
  <tb_soc> <work dir>` runs them): `lang/mips/test/vi32_test.c` (a 32-bit picture) against `vi32_check.py`, `viaa_test.c` (a 16-bit
  one, a plane of zeros) against `viaa_check.py`, and `vishadow_test.c`: the CPU starts the RDP on a list of commands that fills a
  16-bit image and draws four ramps of shaded triangles from 0 to 255 across 256 pixels (`vishadow_gen.c` makes the list and
  the picture from the model). Pad pixels that differ from the model: none, with the setting off (31 distinct levels along a ramp:
  five bits) and on (239 of the 240 pixels of a row distinct: eight bits). (`viaa_check.py` differs from the pads in its last column,
  41 pixels, with the options off as well: the model treats the next row's first pixel as off the picture, the scan-out reads
  it; `--skip-last-column`.)

## Where it is

| What | Where |
|---|---|
| The entry | `pkg/pocket/Cores/tortuga.Mirlo64/interact.json`, "Video settings" (id 254) |
| The register field | `rtl/soc/mirlo_regs.sv` (`ia_out32`, `ia_deblur`), [control.md](control.md) |
| The RDP's dither and shadow plane | `rtl/rdp/rdp_pix.v` (`dither_off`, `out_sh`, `un565sh`), `rdp_mem.v` (the shadow pass and span), `rdp_top.v` (`snew`, `sold`); `shadow_en` wired in `rtl/soc/mirlo_mips.sv`, which maps the plane |
| The scan-out | `rtl/soc/n64_video.sv` (`vi_out32`, `vi_deblur`, `sh_f`, `sh_lo`) |
| The model | `lang/c/rdp/rdp.c` (`shadow`, `sh_crc`), `sim/rdp/tb_rdp.cpp`, `sim/vi_filter/tb_vi.cpp` |
