# The video settings: 32-bit and Deblur

One entry of the core's menu, "Video settings", with four choices: **Default**, **32-bit color**, **Deblur** and **32-bit color + Deblur**.
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
* **A 16-bit picture has more than its five bits too, from a shadow plane** (below): seven bits of red and green, six of blue.

### The shadow plane: more bits for a 16-bit image

The image in the RDRAM stays what it is, RGBA5551, written exactly as without the option: the game, the CPU, the RSP's
DMA and a texture load of the frame see the same data. Beside it the RDP keeps a **shadow plane**, **a byte for each pixel**
of the colour image (a plane word is for four pixels, two colour words), at the image's address halved with bit 24 set (the SoC
maps it to 2 MiB at SDRAM `0x0320_0000`, `LAYOUT_SHADOW`; a plane word's number is the colour image's byte address / 8, its top
bit exclusive-ored into its bit 18, so that two images 4 MiB apart share plane space; its bank is the other of its colour words'
and neither is the hidden plane's):

```
shadow byte = { R[2:1], G[2:1], B[2]  (5 bits: the high two of the low three bits of red and green as the blender made them, the high one of blue),
                CRC-3 of the pixel's 5551 word  (3 bits: x^3 + x + 1, the top bit first) }
```

The bit that is missing repeats the one above it (`{r2, r1, r1}`), so that white stays 255 and black 0: red and green have
128 levels, blue 64. (The plane was a halfword a pixel, the three low bits of each channel and a 7-bit check, before: twice
the traffic, see Costs.)

**Writing.** Every pixel the RDP writes into a 16-bit image (a shaded or textured triangle or rectangle, copy mode, and
a fill) writes its shadow byte with its colour word, with the same coverage and pass masks: the blender's eight bits are
there before they are truncated, and the dither is off. A fill and a copy write the colour's own top bits of each channel again as
the low ones (what the plain expansion gives) with its check, so that what was there before cannot show through them.
The write engine does it with the colour words: after the colour word of a pair of words (or of the span's last word), the plane word
of the pair, with the bytes of the pixels written (byte enables; the first word's bytes are held in a register,
`s_pd`), from a new buffer beside the new colour buffer (`snew`, 16 bits a word). **No plane is kept while the
colour image is at the depth image's address** (`cimg == zimg` to 8 bytes): the RDP clears a depth buffer as a fill of a colour image, and
that is never shown.

**Validation.** A shadow byte is used only where its check is the CRC-3 of the 5551 word now in the image. Anything
that changed the 5551 without writing the plane (the CPU, the RSP's DMA, a copy of a frame, a save state, a game that
reads a frame back and draws it as a texture elsewhere) leaves a byte whose check fails, and the pixel falls back to the plain
expansion, the channel's top three bits again (`{c, c[4:2]}`, white 255, what the console's 8-bit output of that pixel would be).
It fails for every change of up to three adjacent bits of the word (a CRC-3 catches them all: a step of one in a channel that
does not carry far) and for seven in eight of any other change. The other one in eight, and a word written with the colour it
already had, use the old low bits: up to 6 of 255 off in red and green, 7 in blue. (The halfword plane had a 7-bit check, one in 128;
the byte plane's three bits are what a byte a pixel leaves.)
`sim/rdp/tb_rdp.cpp` (`TB_FOREIGN`) checks both ways: of the pixels something else wrote random values to, about 13 % (1 in 8) keep
their byte, and the bench fails above 25 %; of those it did not, all of them keep it.

**Readers.**

1. **The scan-out** (`n64_video.sv`) reads a plane word before the first colour word of each pair of words (and before a row's
   first, if that is the second of its pair), checks the pixels, and carries the nine low bits of each pixel through the FIFO
   (18 bits more a word) into the pipeline that already carries a 32-bit picture's: the anti-alias filter's centre, the divot, the
   gamma table work on eight bits. So it reads **three words for four pixels** (1.5 a colour word, two with a pair's two words; a half more
   than without, two and a bit with the hidden plane). Not for a **woven** frame (a serrated 640 x 480 picture): its 307,200 words a frame
   would be more than the scan-out is sure of when the other masters want it; that picture is shown as a 16-bit picture is without a plane (the
   plain expansion).
2. **The blender** (`rdp_pix.v`) takes the memory colour of a pixel it blends with, or reads the memory's coverage for, with
   the low bits of its shadow byte where the check holds (the memory colour is `{c, 000}` otherwise, as on the
   console), so blends and the edges' coverage accumulate at the plane's precision. The colour span's plane words are read with its
   colour words (`rdp_mem.v`: each before the first colour word it serves, into the old shadow buffer, `sold`, a byte a pixel); the
   pixels wait for them.
3. **A texture load** from a colour image reads the 5551 only, as the console does.

**Where it does not apply.** A picture other than 16-bit (8-bit and 32-bit images have no plane); a woven frame (above); a colour image
at the depth image's address; two colour images 4 MiB apart (modulo, with the plane word number's top bit mixed in) share plane space, and one
overwrites the other's bytes, which then fail their check and show as without the option. The plane is not saved in a Pocket save state; after a
state is loaded its pixels fall back until the game draws them again. A part of the colour image that nothing of the RDP has drawn since the
core started (a picture the CPU draws, say) has whatever the SDRAM held there: one pixel in eight takes its low bits from that (the check is three
bits), up to 6 of 255 off in a channel.

**Costs.** The RDP's cycles with the option on, against the option off, in `tb_rdp` on the captured command lists of 20 graphics tasks (a
flight game's 14 and a space shooter's 6) with the SoC's memory (`TB_SDRAM=1 TB_C16=1 TB_HREMAP=2`: the SDRAM's rows and turns, the hidden plane
in the banks as `sdr_arb.sv` places it, the shadow plane as `mirlo_mips.sv` does):

| | halfword plane (a pixel 16 bits, before) | byte plane |
|---|---|---|
| the 10 large scenes (700 k to 1.2 M cycles off) | +34 to +50 % | +16 to +25 % |
| 4 small tasks with large triangles and sprites | +81 to +91 % | +35 to +40 % |
| tasks that are mostly fills (a colour and a depth buffer cleared) | +88 to +90 % | +25 to +45 % |
| random streams (TB_C16, 400 commands, six seeds) | +23 to +48 % | +7 to +22 % |

(The same lists with the bench's first layout, the depth and hidden planes not placed by the SoC's arbiter, the shadow words at the colour words'
address: +26 to +40 % for the large scenes before, +15 to +25 % now.)
Where it went. The memory channel takes a word a clock and a pixel that blends reads and writes a colour word per two pixels, so the plane's
traffic is the cost: a byte a pixel is a word per four pixels read and another written, against two words per four. Of the difference: the
plane words go with the colour words instead of in a pass of their own (the write engine had a pass after the colour pass, with the pipe
drained between them; the read engine read the shadow span after the colour span, so that a row's pixels began after both): 3 to 4 points; the
rest is the halved traffic. A fill or a copy writes a plane word for four pixels. The depth buffer's clear, which the RDP sees as a fill of a
colour image, writes none. What is left is the plane's traffic itself: a blending pixel costs 1.5 words where it cost 1.
The scan-out reads 1.5 words per colour word where it read 2 (a pair's one plane word, the hidden plane's word a group of 16 pixels on top):
a 320-wide picture passes `tb_vi` under the worst loads, a 640 x 240 one to the worst case of the SoC (five other masters), both with more room than
before. The option off: no extra traffic (the RDP's cycles, reads and writes are the same as before the option, on every stream of the benches).

**Area.** The plane's logic is in the write engine (a plane word for a pair of words, the first word's bytes held, the bytes kept with each
word) and the read engine (a plane word before the first word it serves), less in the pixel pipe and the scan-out; the RAM blocks are 282 of 308,
one fewer than with the halfword plane (the new shadow buffer is half the size, the old one holds whole plane words: 4 blocks). The fit is the
design at 99 %: against the halfword plane, the same seeds give 18,209, 18,219, 18,281 and 18,252 ALMs (seeds 12, 5, 3, 14) where it gave 18,172,
18,213, 18,119 and 18,175, and seeds 6 and 7 18,243 and 18,162; the RDP's own ALMs are 80 to 90 more (5,260 against 5,180 on average, the fitter
spreading a design this full by some 60), and the whole design 70 more on average, which is about 0.4 %. Six seeds were fitted, one closes timing (seed 6,
worst slack +0.014 ns; the others fail in the CPU's paths, by up to 0.56 ns, and one in a hold path by 0.03 ns): `projects/fits.json` has it.

What it cannot do:

* **A game that blends over what the CPU drew, or draws into an image another way, shows that part as without the option.**
  That is the validation working, not a defect.
* **A game that draws with the dither on in a 16-bit image and relies on the pattern** (a noise dither as an effect) loses it.
* The extra bits are the blender's: a flat colour or a texel of five bits has whatever low bits the combiner and the blender
  make of it (a fill and a copy have the plain expansion's); a shaded surface, a lit one and a blend are where the extra
  levels show. They are seven bits of red and green and six of blue, not eight: the bit that is not kept repeats the one above it.

## Deblur

What it does: the VI's **anti-alias and divot filters are bypassed**. Every pixel is shown as the game drew it, an
edge pixel (partial coverage) at the colour it holds, not blended with its neighbours. The filters are the only thing in
the scan-out that mixes a pixel with its neighbours across the picture's edges; the scale does not (above).

What it does not do:

* The dither filter stays as the game set it: it moves a channel by a few levels of 255 to undo the dither and does
  not soften an edge (use "32-bit color" to bypass it; the two combine as "32-bit color + Deblur").
* An edge is jagged where the game relied on the anti-alias filter instead of drawing the edge itself: that is the
  point of the option.
* The Pocket's own scaler (the 640 x 240 slot to its screen) is not the core's; its softness is not touched.
* A picture 512 pixels wide or more already has the filters off.

## Verification

* `sim/vi_filter/regress.sh`: the scan-out against the model (`tb_vi.cpp`; `TB_DEBLUR=1`, `TB_OUT32=1`, with `TB_PX32=1`
  for a 32-bit picture, `TB_SHADOW=1` for a shadow plane filled with bytes that are the pixels' for 70 % and noise for
  the rest, laid out in the SDRAM as `mirlo_mips.sv` maps it): every pixel of a frame compared, scaled, windowed, hi-res, woven, PAL, cropped, under the SDRAM's load. With the
  options off the pad hash (an FNV of every pad word and sync of a run) is the same as the scan-out's before the options
  existed, for 16-bit and 32-bit pictures.
* `sim/rdp/shadow_regress.sh` (`tb_rdp` against the C model, `lang/c/rdp`, whose `shadow` and `rgb_dither_off` are the options'): random
  command streams into a 16-bit image (odd widths too, copy, rectangles, depth, SDRAM timing and the other master's accesses),
  `RTL == model` on the colour image, the hidden plane **and the shadow plane**; the same into a 32-bit and an 8-bit image
  (no plane); and `TB_FOREIGN=1`, where something else writes the colour image between draws (random pixels, and a copy of a
  region of the colour plane alone): RTL == model, the shadow bytes of the pixels nothing else wrote all pass their check, those of
  the random pixels others wrote about 13 % (1 in 8; the bench fails above 25 %). Two runs that must differ do (the model not reading the plane; the model's plane alone).
  Without the option the cycles, reads and writes of every stream are those of the RDP before it (compared on the benches' modes).
* The whole SoC (`sim/mips_soc`, the setting written as the Pocket does, `INTERACT=0x10000144:<n>`; `sim/vi_filter/picture_soc.sh
  <tb_soc> <work dir>` runs them): `lang/mips/test/vi32_test.c` (a 32-bit picture) against `vi32_check.py`, `viaa_test.c` (a 16-bit
  one, a plane of zeros) against `viaa_check.py`, and `vishadow_test.c`: the CPU starts the RDP on a list of commands that fills a
  16-bit image and draws four ramps of shaded triangles from 0 to 255 across 256 pixels (`vishadow_gen.c` makes the list and
  the picture from the model). Pad pixels that differ from the model: none, with the setting off (31 distinct levels along a ramp:
  five bits) and on (120 distinct levels along the red ramp: seven bits). (`viaa_check.py` differs from the pads in its last column,
  41 pixels, with the options off as well: the model treats the next row's first pixel as off the picture, the scan-out reads
  it; `--skip-last-column`.)

## Where it is

| What | Where |
|---|---|
| The entry | `pkg/pocket/Cores/tortuga.Mirlo64/interact.json`, "Video settings" (id 254) |
| The register field | `rtl/soc/mirlo_regs.sv` (`ia_out32`, `ia_deblur`), [control.md](control.md) |
| The RDP's dither and shadow plane | `rtl/rdp/rdp_pix.v` (`dither_off`, `out_sh`, `un565sh`), `rdp_mem.v` (the plane words with the colour words, and the shadow span), `rdp_top.v` (`snew`, `sold`); `shadow_en` wired in `rtl/soc/mirlo_mips.sv`, which maps the plane |
| The scan-out | `rtl/soc/n64_video.sv` (`vi_out32`, `vi_deblur`, `sh_f`, `sh_lo`) |
| The model | `lang/c/rdp/rdp.c` (`shadow`, `sh_crc`), `sim/rdp/tb_rdp.cpp`, `sim/vi_filter/tb_vi.cpp` |
