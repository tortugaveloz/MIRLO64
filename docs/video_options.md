# The picture options: 32-bit and Deblur

One entry of the core's menu, "Picture", with four choices: **Default**, **32-bit**, **Deblur** and **32-bit + Deblur**.
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
* **A 16-bit picture is shown at the full range**: each channel, `v` on eight bits, is taken to `v + (v >> 5)`
  (saturated at 255), which for a plain pixel is the five bits repeated (`{c, c[4:2]}`), so white is 255 and not 248.
  The filters' fractions (anti-alias) ride along. It is done before the gamma table.
* **A 32-bit picture keeps its eight bits.** The scan-out used to drop the three low bits of each channel when it read
  the FIFO ([cimg32.md](cimg32.md), "What is approximated"); with the option on the centre row's line buffer carries
  them (one M10K more) and they are in the output; the anti-alias filter takes them in its centre too (its taps'
  low bits are taken to be the centre's). The anti-alias and divot filters still choose and order their taps on the top
  five bits, so on an edge the result is the console's to within the lowest bit of the taps.

What it cannot do:

* **It does not make a 16-bit game 24-bit.** The image in the SDRAM is 5551, and the three bits the blender had
  are gone when the pixel is written; there is no place to keep them for the scan-out to read (a shadow plane would
  need a second write for every pixel the RDP draws, and would be wrong for everything that writes the buffer
  another way: the CPU, the RSP's DMA, a game that copies a frame, reads a frame back as a texture or blends with it).
  So for a 16-bit game "32-bit" means: no dither pattern, no dedither, the full range of white, and **flat bands where the
  console's dither (and its dither filter) made a smooth ramp**. The default is the closer picture to a smooth
  gradient on such a game; the option is for a picture without the pattern.
* **Only a game that draws into a 32-bit image gets eight real bits**, and only if its blender produces them (a texture
  or a shade of eight bits) and it has the dither off, or this option does.
* A game that draws with the dither on in a 16-bit image and relies on the pattern (a noise dither as an effect) loses it.

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
  for a 32-bit picture): every pixel of a frame compared, scaled, windowed, hi-res, woven, PAL, cropped, under the
  SDRAM's load. With the options off the pad hash (an FNV of every pad word and sync of a run) is the same as the
  scan-out's before the options existed.
* `sim/rdp/tb_rdp` (`TB_DITHEROFF=1`): the RDP's RTL against the C model (`rgb_dither_off`) on random command streams,
  into a 16-bit and a 32-bit (`TB_C32=1`) colour image: `RTL == model`; with the RTL's `dither_off` alone
  (`TB_DITHEROFF=rtl`) the images differ, so the streams do exercise the dither.
* The whole SoC (`sim/mips_soc`, the options written as the Pocket does, `INTERACT=0x10000144:<n>`, `n` 1, 2 or 3):
  `lang/mips/test/vi32_test.c` (a 32-bit picture) against `sim/vi_filter/vi32_check.py [--out32] [--deblur]` and
  `lang/mips/test/viaa_test.c` (a 16-bit one) against `viaa_check.py`: no pad pixel differs from the model. (`viaa_check.py`
  differs from the pads in its last column, 41 pixels, with the options off as well: the model treats the next row's
  first pixel as off the picture, the scan-out reads it; `--skip-last-column`). `sim/vi_filter/picture_soc.sh <tb_soc>
  <work dir>` runs the eight combinations.

## Where it is

| What | Where |
|---|---|
| The entry | `pkg/pocket/Cores/tortuga.Mirlo64/interact.json`, "Picture" (id 254) |
| The register field | `rtl/soc/mirlo_regs.sv` (`ia_out32`, `ia_deblur`), [control.md](control.md) |
| The RDP's dither | `rtl/rdp/rdp_pix.v` (`dither_off`), wired in `rtl/soc/mirlo_mips.sv` |
| The scan-out | `rtl/soc/n64_video.sv` (`vi_out32`, `vi_deblur`) |
