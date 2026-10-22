# 32-bit colour images

A game that renders into a 32-bit colour image (`SET_COLOR_IMAGE` size 3, RGBA8888) shows it with `VI_STATUS` type
3: one such game does it for its title screens, from the "No Controller Pak" screen on. Three parts make that work:

| Part | Where | State |
|---|---|---|
| The model | `lang/c/rdp/rdp.c`, the N64's pixel pipeline (`RDP_N64`) | done; `lang/c/rdp/test_cimg32.c` |
| The scan-out | `rtl/soc/n64_video.sv`, `rtl/soc/rcp_vi.sv`, `rtl/soc/mirlo_mips.sv`, the runtime's `lang/n64rt/n64rt_io.c` | done |
| The RDP | `rtl/rdp/rdp_top.v`, `rdp_pix.v`, `rdp_mem.v` | done ("Implementation" below) |

## The rules

The public description is the N64 programming manual's blender section (15.5). What it says, word for word:

- "For 32-bit RGBA types, the 3 coverage bits are stored as the 3 MSBs of the 8-bit alpha channel and the hidden bits
  are ignored."
- "Note that the 32-bit RGBA mode does not provide increased alpha resolution."
- "The dithering of the RGB from 8 bits to 5 bits by adding 3 LSBs of noise to the original 8 bits is enabled even in
  32 bit mode (8/8/8/8), where there is no truncation to be done." It goes on: opaque surfaces should have the dither
  off in 32-bit mode "so the 3 LSBs do not get stepped on".
- "You cannot copy the 32-bit RGBA color image type."

The model (`RDP_N64`, with the blender) follows them:

| What | Rule |
|---|---|
| A pixel | One 32-bit word at `image + (y * width + x) * 4`: R, G, B, A from its lowest byte. The depth image stays 16-bit at `zimage + (y * width + x) * 2`, whatever the colour image is |
| Memory colour | The blender reads the pixel's R, G, B at 8 bits each (a 16-bit image's five bits with three zeros below) |
| Memory coverage | `IM_RD`: the alpha byte's bits 7:5. Without it: 7, as for a 16-bit image. The blender's "memory alpha" input is that coverage, as for a 16-bit image |
| A pixel written | R, G, B as the blender leaves them, then the RGB dither (below), no truncation to five bits. The alpha byte is the coverage written (`CVG_DST`, the same rules as a 16-bit image) shifted left by five |
| Hidden bits | Not read and not written |
| Depth | Unchanged: the same compare, update and word |
| Fill | `FILL_RECTANGLE` in fill mode, and a triangle in fill mode: the 32-bit fill colour whole at each pixel, a pixel a word; no hidden bits |
| Copy mode | A texture rectangle (or anything else) in copy mode into a 32-bit image is not drawn |
| Texture loads | Unchanged: a 32-bit colour image loaded as an RGBA 32 texture was already read as one |

### Choices where the description is not explicit

- **The RGB dither applies to 32-bit images.** The manual says so (above), so the model runs the same `dither_n64` as
  for a 16-bit image: each channel either keeps its eight bits or goes up to the next multiple of 8 (255 at most),
  and the result is written whole, as the RTL does. One define, `CIMG32_DITHER` in `rdp.c`, turns it off in the model. The captured title
  screens have the dither off (`RGB_DITHER_SEL` 3), so their pixels do not depend on this choice.
- **The alpha byte's low five bits are written 0.** The manual puts the coverage in the top three and says nothing of
  the rest; the pixel's own alpha is not stored, as it is not in a 16-bit image.
- **The hidden bits are not written.** The manual says they are ignored; nothing reads them for a 32-bit image. A game
  that later reads the same memory as a 16-bit image with `IM_RD` would see what the hidden plane held before. No game
  seen does that.
- **Copy mode draws nothing.** The manual says it cannot be done; the console's behaviour is not described, so nothing
  is written (the pixel count does not move either).
- **The fill writes whole pixels.** The console writes 64 bits (two 32-bit pixels) at a time; the model's fill
  rectangle covers the same pixels as for a 16-bit image (whole pixels, the lower right one included), one at a time.

### Verification

`lang/c/rdp/test_cimg32.c` (its header has the build line): fill (the whole word at each pixel, no hidden plane
write, the rectangle's corners, against the 16-bit fill's two halves), a blended triangle over a background at both
sizes, the memory colour's low bits and the memory coverage from the alpha byte (clamp and wrap), the blender's
memory alpha as the coverage through the divide, a textured rectangle of an RGBA 32 texture (the texel's eight bits,
its alpha not stored), the dither in 32-bit mode, depth at the 16-bit offset, copy mode not drawn, and one scene drawn
into a 16-bit and a 32-bit image: the 16-bit image is the 32-bit one's channels truncated to five bits with the same
coverage at every pixel, wherever the memory colours the scene reads are multiples of 8:

```
test_cimg32: 31 checks, 0 failed
```

`sim/rdp/rdp_task` (`make`) draws the four captured title tasks of a game that renders into a 32-bit colour image:
88,895 pixels each, the "No Controller Pak" screen's 101 glyph rectangles.

## The scan-out

`VI_STATUS` type 3 makes the picture 32-bit. What changes:

- **The pitch and the row.** `VI_WIDTH` stays a count of pixels. The runtime (`vi_width_gamma`) gives the scan-out bit
  17 of `VIDEO_FRAMEBUFFER.WIDTH` (32-bit pixels) and the words a row reads, `((pw - 1) * X_SCALE >> 10) + 8 + 1` (a
  word a pixel; a 16-bit picture's is half that, plus one); the picture's bytes, for the end-of-RDRAM blank, are 4 a
  pixel. In the scan-out the pitch is `VI_WIDTH` words, and a woven frame's second field is 640 words on.
- **When.** The runtime takes the type with the width: from the game's `VI_STATUS` writes (software VI), from
  `osViSetMode`'s control word, or (VI in hardware) by reading `VI_STATUS` back in `n64rt_vi_hw_event`; `rcp_vi.sv`
  raises its change event when the type's bit 0 changes (2 to 3 or back; 0 to 2, a 16-bit game's start, does not wake
  the runtime, so a 16-bit game runs cycle for cycle as before), so a switch of pixel size is seen like a switch of width, and the
  scan-out takes the type and the pitch together at its frame boundary.
- **The pixels.** The DMA reads a word a pixel and no hidden plane. The FIFO keeps its 38-bit words. Its read side takes
  a word a step and turns the pixel into what a 16-bit picture's pixel carries: each channel's top five bits, and the
  alpha byte's top three bits as the coverage (bit 7 where a 16-bit pixel's bit 0 is, bits 6:5 where its hidden bits
  are). The line buffers and the filters are the 16-bit picture's, unchanged.

### What is approximated

- **The picture is shown at five bits a channel.** The VI's own 32-bit path works on eight bits; the line buffers
  (1024 x 19 bits, two M10K each) have no room for the three low bits of each channel, and the device has no block to
  spare, so they are dropped where the FIFO is read. The anti-alias, divot, dither filter and gamma then do exactly
  what they do for a 16-bit picture. Keeping the low bits would cost one M10K more (the centre row's buffer at 28 bits)
  and three bits more in the filters' adders; done once, the filters are exact for the centre pixel with the taps' low
  bits taken to be the centre's.
- **A serrated 32-bit hi-res picture** (640 x 480 interlaced at 4 bytes a pixel): the woven frame's rows are read at the
  right pitch, but the hardware VI's test for "the second field's origin is the first's plus a row" (`rcp_vi.sv`
  `row_up`) knows 2-byte rows only, so the base would follow the game's two origins. The runtime's software VI knows
  4-byte rows. No game is known to need it.
- **`VI_STATUS` type 0** (blank) is still not read.

### Bandwidth

A 320-wide 32-bit picture reads 328 words a row, 78,720 a frame: what a 640-wide 16-bit picture reads, 7.5 % of the
SDRAM's cycles at 60 Hz (from [resolution.md](resolution.md): a woven frame's 153,600 words are 14.7 %). A 640-wide
32-bit picture reads twice that, a woven one 307,200 words a frame, 29 %. The FIFO is 16,384 words either way: 50 rows
of a 320-wide 32-bit picture.

### Verification

- `sim/vi_filter/regress.sh`: 55 runs, 0 failed. The 44 runs of 16-bit pictures give the same pad hash (an FNV of every
  pad word and sync of the run) as before the change; the ten 32-bit runs (`TB_PX32=1`: the filters at 320, a scaled
  440, hi-res, woven, crop and PAL, under the SDRAM's load) match the bench's model pixel for pixel, with no read of the
  hidden plane.
- `lang/mips/test/vi32_test.c` on the whole SoC (`sim/mips_soc`): the CPU fills a 320 x 240
  RGBA8888 picture with partial coverage in alpha, the game sets `VI_STATUS` 0x3013 (type 3, anti-alias, divot);
  `sim/vi_filter/vi32_check.py` compares the pads' frame (`VGA=1`) with the model, pure Python:
  `PASS: 0 of 153600 pad pixels differ from the model (11088 partial pixels in the picture, ...)`.
- Area, Quartus 23.1 Analysis & Synthesis of the scan-out alone (`tools/area2/ooc.sh video`): 1,581 ALUTs and 1,104
  registers before, 1,614 and 1,106 after (+33 ALUTs, +2 registers); block memory bits 666,272 both; DSP 4 both.
  `rcp_vi.sv`'s change event gains one term (a few ALUTs).

### The wire

The scan-out's new input takes the CSR bit that `mirlo_regs.sv` already decoded and the SoC already wired (`fb_tall`,
WIDTH's bit 17, which had no use): `rtl/soc/mirlo_mips.sv`'s `n64_video` instance has `.vi_px32(fb_tall)`. A bench
that leaves the input unconnected (0) shows every picture as 16-bit, as before.

## The RDP: design

What `rdp_top.v`, `rdp_pix.v` and `rdp_mem.v` need, so that the RTL writes what the model writes
(the regression compares them word for word). The pixel pipeline itself does not change: it already works on 8-bit
channels and expands a 16-bit memory pixel to 32 bits (`un565`); only the edges, where pixels come from and go to
memory, learn a second format.

### State and decode (`rdp_top.v`)

- `SET_COLOR_IMAGE`: `cimg32 <= size == 3`, and `cimg_ok` true for sizes 1, 2 and 3. A primitive in copy mode while
  `cimg32` is dropped as today's 32-bit primitives are (the model's `cimg_ok`).
- A row's colour base is `cimg + y * width * 4`, its depth base still `zimg + y * width * 2`.
- `rq_h` (the hidden words' read) and the write engine's hidden merge are off with `cimg32`.
- `need_mem`, `z_cmp`, `z_upd` as for a 16-bit image.

### The line buffers: no new block

Today, per set (two sets, a row on each in turn): `cold` and `zold` are 512 words of 32 bits, two 16-bit pixels a word;
`cnew` is 512 entries of 36 bits, `{colour 16, hidden 2}` for each of two pixels; `cmsk` two pass bits an entry. A
32-bit pixel needs 32 bits in `cold` (the memory word as it is) and 27 bits in `cnew` (R, G, B and the coverage), so
each already holds one 32-bit pixel an entry:

| Buffer | 16-bit image | 32-bit image |
|---|---|---|
| `cold` 1024 x 32 | two pixels a word, address `{set, (rwb + x) >> 1}` | one pixel a word, address `{set, rwb + x}` (9 bits) |
| `cnew` 1024 x 36 | `{c16, h2}` x 2 | `{R8, G8, B8, cvg3}` in bits 26:0, one pixel an entry |
| `cmsk` 1024 x 2 | a bit a pixel | bit 0 |
| `zold`, `znew`, `zmsk` | two pixels a word | unchanged: two pixels a word, address `x >> 1` |
| `hraw0`, `hraw1` | the span's hidden words | not used |

The cost is the span's length: a set holds 512 pixels of a 32-bit row against 1,024 of a 16-bit one. A span wider
than 512 pixels in a 32-bit image (only a hi-res 32-bit picture has one) is drawn in two halves by the sequencer
(`S_SETUP` splits `[x0, x1]` at 512 pixels and runs the row twice), or, as a first step, the scissor is taken as
at most 512 pixels wide for a 32-bit image and the case left out. The captured title screens are 320 wide.

M10K: 0 more. (`cold` and `cnew` keep their 4 blocks each; the 32-bit image uses them differently.)

### The pixel side (`rdp_pix.v`)

- **Read addresses.** The colour and depth buffers are read at different words for a 32-bit image: `lb_raddr` (today
  shared by `zold` and `cold`) becomes two, the colour one `{set, (rwb + x)[8:0]}` with `cimg32`.
- **The memory colour.** `mem10 <= cimg32 ? {clb_rdata[31:8], 8'hFF} : un565(...)`: the eight bits as they are, where
  the 16-bit path has five and zeros. The rest of the blender already takes 8-bit channels.
- **The memory coverage.** `cimg32 ? clb_rdata[7:5] : {memb10, hid10}`, the same three bits further on (the overflow,
  the memory alpha, `CVG_DST`).
- **The output.** For a 32-bit image the pixel leaves as `{R8, G8, B8, cvg3}`. The RGB dither applies as today: where a
  channel's threshold is under its low three bits (`rdp_dither_add`'s step), the output is the next multiple of 8, or
  255 from 248 up (`{c5 + 1, 000}`, saturated), else the eight bits unchanged (`{c5, lo3}`); the 16-bit path keeps
  taking `c5 + step`. So the dither's compare is shared and the 32-bit output is a 24-bit multiplexer behind it.
- **Fill.** In fill mode the colour is `fill` whole for every pixel (today: `fill[31:17]` or `fill[15:1]` by the
  pixel's parity). The write engine can do it alone (below), as it does a fill rectangle today.

### The memory side (`rdp_mem.v`)

- **Reads.** A colour span reads `x1 - x0 + 1` words from `cbase + 4 * x0` with `cimg32` (today
  `(x1 >> 1) - (x0 >> 1) + 1` from `cbase + 2 * (x0 & ~1)`), into `cold` at `{set, (rwb + x)[8:0]}`. No hidden read.
  The depth span is unchanged.
- **Writes.** A colour span writes, for each pixel with its pass bit, the word `{R8, G8, B8, cvg3, 00000}` with all four
  byte enables (today two pixels a word with the byte enables of the pixels that passed, and the group's hidden word
  merged). No hidden write.
- **Fill.** A fill rectangle in a 32-bit image writes `fill` to each word of the span, one pixel a word, with no
  hidden quads.
- **Bandwidth.** A 32-bit image's colour span is twice the words: a title screen's glyph rectangle is mostly that (no depth).

### Estimate

| Where | What | ALMs (about) |
|---|---|---|
| `rdp_top.v` | `cimg32`, the copy-mode drop, the row base `* 4`, `rq_h` and the hidden merge off | 15 |
| `rdp_pix.v` | the colour read address, the memory colour and coverage multiplexers (24 + 3 bits), the 32-bit output after the dither (24 bits and the saturation) | 50 |
| `rdp_mem.v` | the span's word count and start, the buffer address in pixels, the write word and its enables, the fill | 50 |
| the 512-pixel split (if done) | `S_SETUP`'s second pass of a row | 30 |

About 115 ALMs without the split, 145 with it; 0 M10K; 0 DSP. The ALM figures are estimates from the multiplexer
widths (Cyclone V: about two 2-to-1 bits an ALM), to be replaced by `tools/area2/ooc.sh rdp`'s count.

### Its verification

- `sim/rdp`'s `tb_rdp` with a `TB_C32=1` mode as `TB_C8=1` is for an 8-bit image
  (the random streams drawn into a 32-bit colour image, at widths 292, 296 and 320, with and without the SoC's memory
  timing); `tools/area2/rdp_regress.sh` gains those runs.
- `tb_rdp` on the fourth captured title task (`task_gfx_003.bin`): "RTL == model" and 88,895 pixels.
- Then the whole SoC: that game for 10 console seconds, its title through the RDP and the scan-out's 32-bit path.

## Implementation

The RTL draws into a 32-bit colour image as the model does (with its `CIMG32_DITHER` at 1), word for word, with no
new block RAM and no DSP block.

### As built, against the design

- **Decode** (`rdp_top`): `SET_COLOR_IMAGE` sets `cimg32` for size 3 and `cimg_ok` for sizes 1, 2 and 3. A triangle,
  texture rectangle or fill rectangle is dropped when `cimg_ok` is clear or, with `cimg32`, in copy mode (the modes
  when the primitive starts, as the model's `execute` tests them). `need_mem`, `z_cmp` and `z_upd` are a 16-bit
  image's.
- **Row bases.** The row offset stays `y * width * 2`, the depth image's; the colour base is `cimg + 2 * row_off` with
  `cimg32` (`crow`, one adder with a select on its operand, for the read and the write requests). No hidden read
  (`rq_h` off), no hidden merge or fill quads in the write engine (`h_any` off).
- **Line buffers**, as the design's table, with two differences. The new colour buffer `cnew` is written at
  `{set, x[8:0]}` with `cimg32` (`cn_wa`, from the depth buffers' `nb_wa` and the pixel's `x[0]`), every pixel's
  entry whole, laid out as `{R8 G8, pass, 0, B8 cvg3, 00000, 00}`: the write engine's 16-bit word,
  `{[35:20], [17:2]}`, is then already `{R G B, cvg << 5}`, and the pixel's pass bit is the entry's bit 19 rather than
  a bit of `cmsk`. So `cmsk` keeps `zmsk`'s write address and byte enables, and the fit goes on packing the two pass
  masks together (with `cmsk` at its own address the unit took two RAM blocks more). `cold` has its own read address
  (`clb_raddr` from `rdp_pix`); `zold` keeps `lb_raddr`.
- **The pixel side** (`rdp_pix`): the memory colour is `{clb_rdata[31:8], 0xFF}`, the memory coverage `clb_rdata[7:5]`
  in the places of the colour's bit 0 and the hidden two, so the overflow, the blender's memory alpha and `CVG_DST`
  see it unchanged. The output `out_c27` is behind the output registers, beside `out_color`: per channel, where the
  dither's compare (the threshold under the low three bits, the compare `rdp_dither_add` makes) takes it up, the next
  multiple of 8, or 255 from 248; else the eight bits as they were; then the coverage's three bits. With the dither
  off (`RGB_DITHER_SEL` 3) the threshold is 7 and the eight bits go through.
- **The memory side** (`rdp_mem`): the word indices are 10 bits. A span's read takes its columns from `rdp_top`'s
  `xs` and `xe` directly (they stay as they are while the read waits: the row's pixels wait for it), keeps the span's
  first and last pixel, and derives each span's words from them: Z's `x >> 1`, the colour span's `x` with `cimg32`;
  the Z span goes first and the colour span starts over from its own first word, and the returning side compares
  with its own span's last word, since the asking side may already be on the colour span. The write engine's colour
  pass takes a word a pixel with `wr_c32` (`w_wide`): all four byte enables by the pixel's pass bit, ready when that
  pixel's result is in; its Z pass is a 16-bit image's. In fill mode (`wr_fmode`) the word is the fill colour whole
  (`w_fc`): a fill rectangle (straight from the sequencer, one word a pixel, up to 1,024 a row, not through the line
  buffers) and a triangle drawn in fill mode (through the pipeline, its pass bits as usual, the colour replaced at the
  write).
- **Spans longer than 512 pixels: the split** (`rdp_top`, `spl`, `S_SPL`). Where a row's span is set up (`S_ROW` for a
  rectangle, `S_AASPAN` for a triangle) a 32-bit image's span of more than 512 pixels ends at `xs + 512`; a triangle's
  test is two compares side by side, `aa_max >= aa_min + 512` and `scissor > aa_min + 512`, so that it does not
  lengthen the span's path. The first half runs as a row. At its last pixel `S_SPL` makes the rest, up to the span's
  end (`xe2`: a rectangle's `xb`, a triangle's `aa_xe`, which still hold it), a row of its own on the other set of line
  buffers, with its own read and write requests, and the walk goes on: `cur` holds the first half's last pixel and
  steps at the next issue, so the attributes are the values the unsplit walk reaches, bit for bit, with no second
  setup (the setup's product could differ in its top bits where the edge's 16.16 distance wraps). With the LOD walking
  left, the second half starts with a ghost of the first half's last pixel, as a span starts with a ghost of the pixel
  before it; walking right, the first half already ends with its neighbour's ghost. The design note's fallback (a
  scissor of at most 512 pixels for a 32-bit image) is not needed.

Nothing of the rules above is left out. The hidden plane is neither read nor written for a 32-bit image; a primitive
in copy mode into one draws nothing and counts no pixel, as the model.

### Resources and timing

`tools/area2/ooc.sh rdp` (from a copy whose project directory is under `build/c32`), then the unit fitted with every
pin virtual and a 16 ns clock, three seeds, as sim/area_study.md ("32-bit colour images") has it. Before: the tree as the LOD left
it.

| RDP | ALUTs | registers | block memory bits | RAM blocks | DSP | fitted ALMs, seeds 1 / 2 / 3 | worst setup slack, seeds 1 / 2 / 3 |
|---|---|---|---|---|---|---|---|
| before | 7,332 | 6,313 | 231,341 | 48 | 39 | 4,992 / 4,982 / 4,976 | +1.119 / +0.595 / +0.828 ns |
| with 32-bit images | 7,535 | 6,367 | 231,381 | 48 | 39 | 5,124 / 5,119 / 5,114 | +1.149 / +0.909 / +1.005 ns |

So +132 to +138 fitted ALMs (the estimate was 145 with the split), no RAM block, no DSP block. The worst path is the
same in every fit, before and after: the YUV conversion's input select into `texA9` (docs/rdp.md, "A timing option
kept in reserve"); the new paths (the split's compares into `xe` and `spl`, the colour base, the read and write
engines' indices, the buffers' addresses) have 2.9 ns or more, within a few hundred picoseconds of what the same
endpoints had before.

### How it was checked

- `tools/area2/rdp_regress.sh`: `rdp_regress: 63 runs, 0 failed`: its 53 runs and ten with `TB_C32=1` (eight seeds
  over the four widths, two with `TB_SDRAM=1 TB_CPU=30`). Its other random streams switch the colour image to a
  random size now and then, so they draw into 32-bit images too.
- `tb_rdp -r SEED 300`, seeds 100 to 130: all `RTL == model`, and the output is the pre-change bench's to the clock
  but for the three seeds that switch to a 32-bit image (120, 122, 129), which now draw more pixels.
- `TB_C32=1` (new in `tb_rdp`): random streams into a 32-bit colour image, at widths 320, 296, 640 and 1,000 by the
  seed (`TB_CW` to choose one); a wide image's scissor and primitives span its width, so the 640 and 1,000 wide streams
  draw hundreds of spans in two halves each. Seeds 1 to 40: all `RTL == model`; and 28 more with `TB_LFSR=1 TB_STEP=1`,
  `TB_SDRAM=1 TB_CPU=30`, `TB_LOD=1`, `TB_CC=1`, `TB_CW=1000`, and `TB_CW=640 TB_LOD=1`: all `RTL == model`.
- `TB_LOD=1`, seeds 1 to 12: all `RTL == model`.
- The 88 captured task dumps under `build/triage/*/dumps*` and `build/lod_dumps/*/`: all `RTL == model` (and with
  `TB_SDRAM=1 TB_CPU=30`). Every `TB_CMDT` log of those that do not draw into a 32-bit image is byte for byte the
  pre-change RTL's. The four 32-bit title tasks now draw 88,895 pixels each (88,895 writes,
  175,130 clocks against 38,133), and the picture from the RTL's memory is `rdp_task`'s, byte for byte: the
  "No Controller Pak" screen.
- The whole SoC (`sim/mips_soc`, a bench built with this RTL, `FB_N64=1 VGA=1`): a 3D platformer for 300M cycles gives
  frames byte for byte those of a bench built with the RTL before the change. The 32-bit game (PAL) for 625M cycles with a
  runtime built from the current `lang/n64rt` sources shows the "No Controller Pak" screen from field 60, the legal
  text from field 255, its fade, and, run on to 720M cycles, the attract scene's first frames fading in (a textured
  wall under a starry sky), all at the right pitch and colour. A runtime without the change to `n64rt_io.c` draws the
  same pictures but shows them as 16-bit pixels at half the pitch.
