# The RDP

The RDP of MIRLO 64 (`rtl/rdp`) draws the N64's own command stream: what a
program's microcode, running on the RSP ([rsp.md](rsp.md)), leaves in memory
for it. It decodes the N64's commands, walks the N64's triangle coefficients,
keeps the N64's 4 KiB of TMEM and writes a 16-bit, 8-bit or 32-bit colour
image and a 16-bit depth image in the N64's memory.

Its contract is a C reference model (integer arithmetic only): for the same
command stream the RTL writes the same memory, byte for byte. The benches in
`sim/rdp` compile that model next to the RTL (`sim/rdp/build_rtl.sh` and
`sim/rdp/Makefile` name its source).

| file | what it is |
|---|---|
| `rtl/rdp/rdp_dpc.sv` | the DPC registers and the command fetch |
| `rtl/soc/rcp_dpc_view.sv` | DPC_STATUS as a reader sees it |
| `rtl/rdp/rdp_top.v` | command intake and decode, state, the sequencer, the edge walker, the line buffers |
| `rtl/rdp/rdp_pix.v` | the pixel pipeline: texture, combiner, coverage, depth, blender, dither |
| `rtl/rdp/rdp_tmem.v` | TMEM: a copy in true dual-port memories (two read ports) |
| `rtl/rdp/rdp_mem.v` | every access to the N64's memory: span reads, write-back, fills, texture loads |

`rtl/soc/mirlo_mips.sv` instantiates `rdp_top`, `rdp_dpc` and `rcp_dpc_view`.

## The command interface

### Registers and fetch

`rdp_dpc.sv` holds the N64's DPC_START, DPC_END, DPC_CURRENT and DPC_STATUS
and fetches the 64-bit commands between CURRENT and END, handing them on as
32-bit words.

* A write of START is held until the next write of END, which makes
  CURRENT = START and sets END. While commands are still being fetched that
  pair waits for them. A write of END alone extends the run. An END written
  in the very cycle a waiting pair would start starts the pair itself, with
  that END, and nothing is left waiting: a microcode that writes its output
  in a ring in DMEM writes END after each command, so one of its ENDs meets
  the end of the list before the wrap now and then, and a pair left waiting
  then would have run the ring's first commands a second time, CURRENT
  going back behind what the microcode had already written over.
* There is work while CURRENT is below END. An END behind CURRENT is nothing
  to do, not a run round the address space.
* CURRENT moves by 8 as each command's second word comes back. Up to four
  words are asked for or held at a time.
* DPC_STATUS writes set and clear XBUS, FREEZE and FLUSH with the N64's bit
  pairs. FREEZE stops the fetch. FLUSH is only stored.
* Without XBUS the commands are read from RDRAM, through a port of the SDRAM
  arbiter that the fetch has for itself. With XBUS they are in the RSP's
  DMEM, and START, END and CURRENT are addresses in it: the words are read
  from a copy of DMEM that the RSP keeps for this, a word a cycle.

Two writers reach the registers:

* **The RSP.** The microcode writes START, END and STATUS through the RSP's
  COP0.
* **The CPU.** The CPU's device port has them at `0x2000_0040` (START),
  `+44` (END), `+48` (CURRENT, read only) and `+4C` (STATUS); the monitor
  forwards a program's accesses there. A CPU write is taken in a cycle in
  which the microcode is not writing a register itself.

### What a reader sees

Both readers get START, END and CURRENT from `rdp_dpc.sv` and DPC_STATUS
from `rcp_dpc_view.sv`, which gives the console's layout:

| bit | name | here |
|---|---|---|
| 0, 1, 2 | XBUS, FREEZE, FLUSH | as written |
| 3 | START_GCLK | set by a write of END (not while frozen) and by an unfreeze; cleared when a SYNC FULL completes |
| 4 | TMEM busy | always 0 |
| 5 | PIPE busy | as bit 3; also cleared by bit 7 of a STATUS write |
| 6 | command busy | always 0 |
| 7 | CBUF ready | always 1 |
| 8 | DMA busy | CURRENT below END |
| 9, 10 | END valid, START valid | a pair waiting; a START written and its END not yet |

After a list without a SYNC FULL the register reads `0xA8`. A write of START
while START is valid is ignored.

A read of DPC_START or DPC_END gives the last value written, as the console's:
a pair written while a list is under way waits (bits 9 and 10), and its START
and END are what the reads show, while DPC_CURRENT is the list under way's.
This is the value a microcode takes to begin its output where the RDP's list
ends (a task, or a microcode loaded in the middle of one, that starts while the
RDP is busy): with the END of the list under way instead it would write over
what the waiting pair is still to run, and the RDP would take stale commands
for them.

### The command FIFO

The fetched words go into a 4096-word FIFO in `rtl/soc/mirlo_regs.sv`, and
from there to `rdp_top`. The same module lets the CPU read the FIFO's level,
the count of completed SYNC FULLs and an idle flag. A completed SYNC FULL
also pulses the DP interrupt of the MI (`rcp_mi`).

## Commands

`rdp_top` takes one command at a time. Its length comes from the first word:
a triangle is 8 words plus 16 for shade, 16 for texture and 4 for Z; a
texture rectangle is 4; everything else is 2. State commands wait for the
primitive before them to finish, so the pixel pipeline never sees a state
change with pixels in flight.

| op | command | notes |
|---|---|---|
| 0x00 | no-op | |
| 0x08-0x0F | triangle | edges, then shade, texture and Z coefficients by the opcode's low three bits |
| 0x24, 0x25 | texture rectangle, flipped | |
| 0x26-0x28 | sync load, pipe, tile | nothing to do: commands run in order |
| 0x29 | sync full | completes when every earlier write has been accepted by the memory |
| 0x2D | set scissor | the four coordinates; the interlace field bits are not read |
| 0x2E | set primitive depth | Z (15 bits) and its delta |
| 0x2F | set other modes | what is read of them: below |
| 0x30 | load TLUT | |
| 0x32 | set tile size | |
| 0x33 | load block | |
| 0x34 | load tile | also sets the tile's size |
| 0x35 | set tile | format, size, line, TMEM address, palette, clamp, mirror, mask, shift |
| 0x36 | fill rectangle | |
| 0x37-0x3B | fill, fog, blend, primitive, environment colour | primitive colour also sets the primitive LOD fraction |
| 0x2C | set convert | K0 to K3, a YUV texel's conversion; K4 and K5, two inputs of the combiner |
| 0x3C | set combine | both cycles |
| 0x3D-0x3F | texture, depth, colour image | |

Any other opcode (set key, the undefined ones) is counted in
`unknown_ops` and skipped as a two-word command.

## Drawing a primitive

A primitive is drawn row by row. For each row the sequencer:

1. finds the row's span (the walker for a triangle, fixed columns for a
   rectangle);
2. asks the memory engine for what the pixels need of memory: the depth span
   with Z_CMP, the colour span when the blender or the coverage reads it, and
   the span's hidden coverage words;
3. sets up the attributes at the span's first pixel;
4. issues the pixels, one a clock as their memory words come in;
5. asks for the write-back of the row's results.

What memory holds and what the pixels produce sit in on-chip line buffers of
512 words, two pixels a word, so a span is at most 1024 pixels. There are two
sets of them and rows alternate between the sets, so a row's reads, its
pixels and the write-back of the row before overlap. Only pixels that passed
are written, by byte enables; a word with none is not written. The depth span
is written only with Z_UPD. A primitive ends when its last pixel is back in
memory.

A row's spans are read as runs: all of the depth span's words, then all of the
colour span's. The write-back is a run too: the colour words as their pixels
come out of the pipeline, then the depth words from the line buffers. The
SDRAM keeps one row open in each of its four banks, and a colour image and a
depth image can be in the same bank word for word (their addresses equal
modulo 8 KiB); words of the two in turn would close and open a row at every
word.

The colour span is read only when it is needed: with FORCE_BL, when a blender
cycle in use selects memory; without it, when the last cycle's P is memory
or, in 2-cycle mode, the first cycle's blend of it; whenever the memory's
coverage is read (IM_RD with AA_EN or CVG_X_ALPHA); and with CLR_ON_CVG and
CVG_X_ALPHA without AA_EN, where a pixel with no scaled coverage is written
as M.

### Triangles

Every triangle is walked with the N64's coverage rules, with or without
AA_EN.

* **Subscanlines.** Each row is four subscanlines. The three edges start at
  `X & ~1` at the first row's top and step `(DxDy >> 2) & ~1` a subscanline,
  one subscanline a clock (five clocks a row). Each edge is taken to eighths
  of a pixel with a sticky bit and clamped to the scissor. A subscanline
  counts if it is between YH and YL and inside the scissor's quarter rows,
  and its edges have not crossed.
* **XL.** XH and XM are the edges at YH's scanline, but XL is the low edge at
  YM itself. It is taken back by YM's quarter rows of its slope before the
  walk.
* **Samples.** Each subscanline has two samples, in a checkerboard, so a
  pixel has an 8-sample mask. The span is every pixel a subscanline reaches.
  With AA_EN a pixel is drawn if any sample is covered; without it, only if
  sample 7, the pixel's top-left corner, is covered. The coverage is the
  mask's count in both cases.
* **Attributes.** The attribute words wait in a 64-word staging RAM. Each
  attribute's value, DaDx and DaDe are put together there from their integer
  and fraction halves. The row-start values step by DaDe each row; a pixel's
  value is the plane at its top-left corner. DaDy is not read, except Z's.
* **Z.** Z moves from the corner to the pixel's first covered sample, by
  DzDx and DzDy in quarter pixels. The shade is not moved.
* **S, T, W.** S and T are s10.5 in the integer half and W is s.15; they are
  shifted into the pipeline's units where the pixels start.
* **Depth step.** From DzDx and DzDy the walker derives the primitive's
  depth step: all ones under the top bit of |DzDx| + |DzDy|, in the stored
  depth's units. The depth test and the blender use it (below).

A triangle whose command has no texture block has S = T = W = 0, and the
texture stages run all the same: TEXEL0 is the tile's texel (0, 0).

### Rectangles

A rectangle's pixels are whole: all eight samples. A texture rectangle in
copy mode and a fill rectangle in fill mode take the corners' integer parts,
the lower-right corner inclusive. Otherwise the corners' quarter pixels
count, as for a triangle without AA_EN: a pixel is drawn where its top-left
sample is inside, `XH <= 4x < XL` and `YH <= 4y < YL`, so a corner with a
fraction leaves out the pixel it falls in at the upper left and takes it in
at the lower right (a run of rectangles placed at fractions of a pixel then
meets without a gap). Its depth is the primitive depth.

A texture rectangle steps S by DsDx along the row and T by DtDy down the
rows; the flipped one swaps the two. The attribute stepped down the rows
starts at YH's whole row. The one stepped along the row is the plane at XH
itself: at column x, its value plus DsDx (or DtDy) times `(4x - XH) / 4` --
copy mode has no fraction there, and its DsDx counts a quarter of its value.
A fill rectangle outside fill mode goes through the pixel pipeline like any
untextured primitive.

## Textures

### TMEM and the loads

TMEM is the N64's 4 KiB, as 2048 16-bit words: four memories of 512 words,
the lower and upper halves by the even and odd words. The loads copy the
texture image as it is in memory, from a source that may start at any byte:

* **load block**: a run of texels in whole 64-bit words, at most 4 KiB;
* **load tile**: a rectangle, each row at the tile's `line`;
* **load TLUT**: up to 256 16-bit entries, each written four times over (64
  bits an entry);
* a load into a 32-bit RGBA tile is split: R and G go to the lower half of
  TMEM, B and A to the same word of the upper half;
* a load into a 16-bit YUV tile is split the same way: a pixel pair's U Y0 V
  Y1 as U V to the lower half and Y0 Y1 to the upper.

An odd row's 64-bit words go into TMEM with their halves changed places, and
are sampled so. A row is odd by the load's own count: LOAD TILE's rows, or
LOAD BLOCK's dxt summed over its words. A LOAD BLOCK with dxt 0 loads every
row as an even one.

### Coordinates

* **Perspective** (other modes): the perspective divide, S and T by W, as the
  console's RDP does it. Only the 16-bit integer halves of S, T (s10.5) and W
  (s.15) enter it: W's leading zeros, up to 14, are shifted out of its low 15
  bits, its next 6 bits pick a point and a slope of the console's two 64-entry
  tables and the 8 after them step along the slope, a 15-bit reciprocal; S and
  T times it, shifted back by the normalisation, are S / W and T / W in s10.5,
  17 bits kept. A product whose bits above those are not all its sign is out
  of range, and so is any coordinate when W's integer half is zero or
  negative; the texel takes the 17 bits saturated to 16, an out-of-range one
  at the end it went past (the model's `mrdp_n64_div`). Without
  perspective S and T are taken as they are, their integer halves.
* **The tile**: the shift, then the tile's origin is subtracted, then clamp
  (with the clamp bit, or a mask of 0), mirror and mask. Each axis gives two
  texel coordinates and a 5-bit fraction.

### Texel formats

A texel is decoded when it is sampled, by the tile's size and format:

| | 4-bit | 8-bit | 16-bit | 32-bit |
|---|---|---|---|---|
| RGBA | as I | as I | 5551 | 8888 |
| YUV | as I | as I | U, V, Y, Y (the pair's U V and the pixel's Y) | read as 8888 |
| CI | as I | as I | read as 5551 | read as 8888 |
| IA | 3 + 1 | 4 + 4 | 8 + 8 | read as 8888 |
| I | 4 | 8 | the upper byte | read as 8888 |

An I texel gives its value on all four channels. With the TLUT enabled every
texel is an index instead, whatever its format and size: a 4-bit one with the
tile's palette above it, an 8-bit one as it is, and of a 16- or 32-bit one the
first byte of its word (a game can load its depth image as a texture and take
a table's entry by the depth's top byte). The entry is RGBA5551 or IA 8 + 8 by
the other modes' TLUT type.

### Filter

With the filter select at 0 the texel is the one at the coordinates. Any
other setting takes the N64's three-point filter: three texels of the 2 x 2,
by which triangle of it the fractions fall in. A pixel reads the three at
once: TMEM is kept twice (`rdp_tmem.v`), each copy in true dual-port
memories with a second small memory of the TLUT's entries for the lookup.
The first copy gives two texels, one by each port; the second gives the
third by one port, its other port only written. A load runs only with the
pixel pipeline empty, so a port that reads while drawing is the loads' write
port while one runs. The loads write both copies alike.

A pass whose BI_LERP bit is off (BI_LERP_0 for the first cycle, BI_LERP_1 for
the second) converts a YUV texel instead of filtering it: the texel at the
coordinates, U and V less 128, with SET CONVERT's K0 to K3 (signed, as
`2K + 1` 256ths): `R = Y + K0 V`, `G = Y + K1 U + K2 V`, `B = Y + K3 U`, and
the alpha Y. With CONVERT_ONE the second cycle converts the first cycle's
texel. Only a YUV tile's texels are converted, and each channel is clamped to
a byte.

### TEXEL1 and LOD

TEXEL1 is the next tile's sample (tile + 1, wrapping at 8) at the same S and
T. A 2-cycle textured primitive whose first combiner cycle reads TEXEL1, or
whose second reads TEXEL0, sends each pixel through the texture stages twice,
a clock apart, on the same TMEM copies and filter: first the tile, then the
next tile. `rdp_top` issues such a pixel every second clock. In the second
cycle TEXEL0 is the next tile's sample and TEXEL1 the tile's own. In 1-cycle
mode TEXEL1 is the same sample as TEXEL0.

The texture LOD ([lod.md](lod.md)) applies to a 2-cycle primitive with
TEX_LOD, or whose combiner reads LOD_FRACTION (C 13 in a colour, C 0 in an
alpha). For each pixel S and T go through the perspective divide at three
points: the pixel, the next pixel of the N64's walk (x + 1 for a triangle with
`lft` and for a rectangle, else x - 1) and the next line (plus DaDy, its top
17 bits). The largest step of S or T, in texels, gives the level (the top
bit's place, clamped to the triangle's level count) and the LOD fraction (the
8 bits under it), with the N64's magnified, far, detail, sharpen and
minimum-level rules. With TEX_LOD the passes sample the level's tile and the
next; the fraction reaches both combiner cycles as a signed 9-bit C. The next
pixel's perspective divide is the neighbour's own: `rdp_top` issues such a
primitive's pixels in the walk's order, two clocks each, and a ghost (through
the perspective divide, never drawn) where the walk's neighbour is outside the
span or not issued in time; the next line's perspective divide takes the
pixel's second clock.

## The pixel pipeline

`rdp_pix.v` takes one pixel a clock and never stalls: every operand is on
chip. Its stages, in order: the perspective divide, the LOD (three stages, for
the primitives that take it), the tile's coordinate arithmetic, the TMEM read,
the TLUT read and decode, the filter, two combiner cycles, the alpha, coverage
and depth tests, two blender cycles, the dither.

### Cycle types

* **1-cycle.** The combiner's second-cycle settings with COMBINED at 0, and
  the blender's first-cycle settings.
* **2-cycle.** The two combiner cycles one after the other, then the two
  blender cycles; the first blender cycle always blends.
* **Copy.** The pixel is the texel. The alpha compare's threshold is 128,
  the texel's alpha bit, not the blend colour's alpha. The depth image is
  neither read nor written, whatever the mode word says. No blender, no
  dither. The coverage written is 7 or 0 by the texel's alpha bit.
* **Fill.** The fill colour's two 16-bit halves, the even pixel taking the
  upper one. A fill rectangle goes straight to the memory engine, without the
  pipeline and without a read. A triangle in fill mode goes through the
  pipeline and writes the fill colour where it has coverage.

Copy and fill mode have no depth.

### Colour combiner

Colour and alpha are `((A - B) * C + D * 256 + 0x80) >> 8`, kept to 9 bits.

* "1" is 256, as A and D. C is the plain 0..255.
* A colour result is clamped: bits 8:7 both set give 0, bit 8 alone 255.
* COMBINED reaches the second cycle as the first cycle's 9 raw bits: 0..383,
  or negative when bits 8:7 are both set; as C, a signed 9-bit number.
* An alpha result of bit 8 alone, or exactly 255, counts as 256 where the
  coverage is multiplied by it (CVG_X_ALPHA): a fully covered opaque texel
  keeps all 8 samples.
* SHADE is saturated to a byte, 0..255, when it enters the pipeline.
* The primitive LOD fraction is an input, and the LOD fraction (a signed
  9-bit number, as K5) in a 2-cycle primitive with the LOD ([lod.md](lod.md));
  elsewhere it reads 0. NOISE and the key inputs read 0.
* **K4** (as B) and **K5** (as C) are SET CONVERT's, 9 bits each, read as
  COMBINED is in those places: K4 is negative from 0x180, K5 a signed 9-bit
  number. With them `(TEXEL0 - K4) * K5 + TEXEL0` finishes a YUV texel's
  conversion, and `(TEXEL0 - TEXEL1) * K5 + TEXEL1` with K5 at 255 is
  TEXEL0.

### Alpha and coverage

* **Alpha dither** (ALPHA_DITHER_SEL): the pattern, its inverse, noise or
  none; the pattern is the matrix that RGB_DITHER_SEL's low bit names. It is
  added to the combined alpha and to the shade alpha, which saturate at 255.
  Copy mode has none.
* **Noise.** A 23-bit LFSR that steps for every pixel with coverage, outside
  fill mode.
* **CVG_X_ALPHA**: the coverage becomes `(alpha * coverage + 4) >> 8`.
* **ALPHA_CVG_SEL**: the pixel's alpha becomes the coverage (times the alpha
  with CVG_X_ALPHA).
* **Alpha compare**: against the blend colour's alpha, or with
  DITHER_ALPHA_EN against 8 bits of the LFSR. In 2-cycle mode the alpha
  compared is the first cycle's.
* **Write rule.** With AA_EN a pixel is written when coverage is left after
  CVG_X_ALPHA. Without AA_EN the pixel's own coverage decides: a CVG_X_ALPHA
  pixel whose scaled coverage is 0 is still written. Without IM_RD the
  console's blender takes its memory colour from a register holding the last
  pixel read with IM_RD, so a CLR_ON_CVG pixel written in this mode gets that
  colour on the console and the memory's own colour here.
* **The memory's coverage** is the three stored bits where the colour span
  is read with IM_RD, and 7 otherwise.
* **CVG_DST**: clamp, wrap, zap (7) or save (the memory's).

### Depth

The depth is the N64's 18 bits: the interpolated Z shifted right by 13 and
kept inside 0..0x3FFFF. With the Z source select, or for a primitive without
Z coefficients, the depth is the primitive depth. The depth image's word is
the N64's too: 14 bits of the depth as a floating-point number -- an
exponent of three bits, the count of the depth's leading ones, and the 11
bits under them -- above the top two bits of the pixel's dz code. The far
end of the range keeps all 18 bits; the near end (exponent 0, the first
half) keeps 12. 0xFFFC is the far plane. A game can read the image with the
CPU or as a texture as it would on the console.

The compares are of the 18 bits against what the word keeps. The modes, with
dz the primitive's depth step (and never less than what the memory's word
has lost at its exponent, so that a surface drawn over itself is within it
at any depth):

* **Opaque**: in front, or -- without a coverage overflow -- within dz behind.
* **Interpenetrating**: as opaque; and where a pixel in front, within dz,
  overflows the coverage, its coverage is scaled by how far in front it is.
* **Translucent**: in front.
* **Decal**: within dz either way, and not over a cleared depth.

A stored depth at the far plane is the cleared one and passes the first three.

### Blender

`P * A5 + M * (B5 + 1)` with 5-bit weights, for each blender cycle.

* P and M are the pixel, memory, the blend colour or the fog colour. A is the
  pixel's alpha, the fog alpha, the shade alpha or 0; B is 1 - A, the
  memory's coverage, 1 or 0. Fog is done with these selects.
* The result is the sum `>> 5` (8 bits, wrapping) with FORCE_BL. Without it
  the sum is divided by `A5/4 + B5/4 + 1`, through a reciprocal table.
* The pixel is blended with FORCE_BL, or with AA_EN when the coverage does
  not overflow and, with Z_CMP, the pixel is not more than dz in front of
  memory. Otherwise the result is P. A cycle of P with 1 - its own alpha, at
  an alpha of 255, is P too.
* In 2-cycle mode the first cycle always blends, `>> 5`.
* CLR_ON_CVG without a coverage overflow keeps M.
* The memory's dz is not stored. With Z_CMP the blender's coverage weight is
  not shifted; without it the shift comes from the primitive's own dz.

### RGB dither

The blender's 8 bits go to 5 by truncation, after the three low bits are
compared with a 3-bit threshold: where the threshold is below them the
channel goes up a step (255 stays 255). RGB_DITHER_SEL chooses the threshold:

| value | threshold |
|---|---|
| 0 | the 4 x 4 magic square, at the pixel's place on the screen |
| 1 | the 4 x 4 Bayer matrix |
| 2 | noise: three LFSR bits for each channel |
| 3 | none |

Copy and fill write their colours untouched.

### Other modes that are read

Of the high word: the cycle type, perspective, the TLUT enable and type, the
filter select and the two dither selects. Of the low word: the blender's
selects, FORCE_BL, ALPHA_CVG_SEL, CVG_X_ALPHA, the Z mode, CVG_DST,
CLR_ON_CVG, IM_RD, Z_UPD, Z_CMP, AA_EN, the Z source, DITHER_ALPHA_EN and the
alpha compare enable.

## Images and memory

### The colour image

The colour image is the program's: RGBA5551 in the N64's byte order, two
pixels a 32-bit word with the left one first. A row must start on a 4-byte
boundary (an image 4-aligned, of even width).

An 8-bit colour image (SET_COLOR_IMAGE's size 1) has a byte a pixel, at the
image's address plus y times its width plus x -- what software uses to draw a
colour-indexed texture, which it then draws with its palette. A pixel's byte is
the blender's red, not dithered (in copy mode the texel's red, in fill mode the
fill colour's byte for the address: bits 31:24 at a multiple of 4). The image
is only written: the pixels read neither it nor a depth image, and no hidden
coverage goes with it. Its rows must start on a 4-byte boundary too (an image
4-aligned, its width a multiple of 4). The pixel pipeline's results go through
the line buffers as a 16-bit image's, the byte in a pixel's colour bits 15:8,
and the write engine puts each pair of pixels into its half of the memory word
by byte enables.

A 32-bit colour image (SET_COLOR_IMAGE's size 3, RGBA8888) has a word a
pixel, R at the lowest address, the coverage in the alpha byte's top three
bits and no hidden bits; its depth image stays 16-bit. The blender reads and
writes the pixel's eight bits a channel, the RGB dithered as a 16-bit image's
but not truncated; a fill writes the fill colour whole at each pixel; copy
mode into one draws nothing. A set of line buffers holds 512 pixels of such a
row, so a longer span is drawn in two halves ([cimg32.md](cimg32.md)).

Bit 0 of a pixel is the top bit of its 3-bit coverage. The other two bits,
the N64's hidden bits, are in a plane of their own:

* **Layout.** The 16-bit word at RDRAM byte A has bits [2k+1:2k], k = A[4:1],
  of the 32-bit word A[22:5] of the plane: one word for a group of 16
  pixels. The RDP addresses the plane with bit 23 of its address;
  `mirlo_mips.sv` maps that to the megabyte just below the RDRAM in the
  SDRAM (`LAYOUT_HID` in `lang/mips/include/layout.h`).
* **Spans.** A span reads its groups' words first. The pixels take the
  memory's coverage there, and the write engine writes each group back with
  the span's new bits merged in, when a pixel of the group was written.
* **Fills.** A fill writes each 4-pixel quad it touches by byte enables,
  from bit 0 of the fill colour's halves, with no read.
* **Start.** The runtime starts the plane all ones (`n64rt_boot.c`).

### The memory port

`rdp_mem.v` makes every access, over one port: a command channel (write
flag and 4-aligned byte address), a write data channel with byte enables
that follows its command, and read data returned in order. Reads go before
writes. In the SoC the port is one of the SDRAM arbiter's.

Nothing in the engine orders a read after a write of the same word. The
sequencer starts a primitive's reads, and a load, only with the write engine
idle.

### The scan-out

`rtl/soc/n64_video.sv` reads the same colour image, and each group's hidden
word before its colour words. With the VI registers of `rtl/soc/rcp_vi.sv`
([mips.md](mips.md)) it applies the VI's filters to the frame buffer's own pixels,
before the VI's scale ([resolution.md](resolution.md)):

* **Anti-alias.** A pixel whose coverage is not complete takes six taps:
  (x-1, x+1) above and below, (x-2, x+2) beside. A tap that is not complete
  is the centre itself. Per channel, the second smallest and second largest
  of the six are widened to take in the centre, and the result is
  `c + ((min + max - 2c) * (7 - coverage) + 4) >> 3`.
* **Dither filter.** On a complete pixel (every pixel with the anti-alias
  off), each channel adds +1 for each of its eight ring neighbours above the
  centre and -1 for each below.
* **Divot.** Where any of three neighbours on the line is not complete, each
  channel takes the median of the three.
* **Gamma.** Each channel's square root, from a table.
* **Crop.** The core's "Crop Vertical" and "Crop Right" options blank lines
  at the picture's top and bottom and pixels at its right edge. The frame and
  its timing do not change.

A picture 512 wide or more is shown with the filters off.

A 32-bit picture (`VI_STATUS` type 3: RGBA8888, a pixel a word) goes through
the same filters: each pixel is taken as its channels' top five bits, with
the alpha byte's top three bits as its coverage, so the filters see what a
16-bit pixel would give them and the picture is shown at five bits a channel
([cimg32.md](cimg32.md)).

## Not implemented, or approximated

The model built with `RDP_EXACT` follows the console on the LOD, TEXEL1,
shade and scissor items below, and on edges, spans, rectangles and the
attributes' arithmetic ([rdp_exact.md](rdp_exact.md), with the RTL change each
needs); the RTL, and the model as its benches build it, do not.

* **Colour images**: an 8-bit one is written only (no image read, no depth,
  no hidden coverage). A 32-bit one is drawn as the model draws it, from
  the programming manual's rules ([cimg32.md](cimg32.md),
  "Choices"); a 32-bit picture is shown at five bits a channel.
* **Depth**: the memory's dz is kept to its top two bits, as the word has
  room for, and the blender's coverage weight does not use it.
* **LOD**: in 2-cycle mode only. The console computes it in 1-cycle and
  copy mode too, from the next two pixels of the walk; here a 1-cycle pixel
  samples its primitive's tile and the LOD fraction reads 0 there. A texture
  rectangle's next line is its exact next row, where the console keeps 17
  bits of it ([lod.md](lod.md), "Choices").
* **TEXEL1** is the next tile at the same coordinates. In the second cycle
  of a 2-cycle pixel it is the tile's own sample, not the next pixel's.
* **YUV** textures are converted only from 16-bit YUV tiles, each channel
  clamped to a byte (the console keeps 9 bits); a pass that converts takes
  the texel at the coordinates, unfiltered. SET_KEY is skipped; the key and
  noise inputs of the combiner read 0.
* **Shade** is not corrected to the covered sample; only Z is.
* **Scissor**: the interlace field bits are not read.
* **Other modes**: the key enable is not read.

## How it is verified

| what | where | checks |
|---|---|---|
| a frame in software | `sim/rdp/rdp_task` (`make`) | a captured task dump through the RSP's model (`sim/rsp`) and the reference model, to a PPM; `-n` counts the commands by opcode, `-v` lists them, `rdp_stats.py` summarises what the primitives use |
| RTL against the model, captured tasks | `sim/rdp/build_rtl.sh`, `obj_rdp/tb_rdp task...` | the RSP model's commands go to both, each with its own copy of memory; every byte, hidden plane included, must match |
| RTL against the model, random | `tb_rdp -r SEED [N]` | random state, loads of every kind from any alignment, triangles and rectangles in every cycle type |
| 32-bit colour images | `TB_C32=1 tb_rdp -r SEED [N]` | random streams into a 32-bit colour image at widths 320, 296, 640 and 1,000 (the wide ones' spans in two halves) |
| the texture LOD | `TB_LOD=1 tb_rdp -r SEED [N]` | random streams made for the LOD (2-cycle mostly, DaDy on the scale of DaDx, the LOD blend combiner, the minimum level) |
| the console's rules | `sim/rdp_oracle` (`./setup.sh`, `make`, `./run.sh 50`); `make rdp_task EXACT=1` in `sim/rdp` | the model, command for command, against an independent pixel-exact software RDP fetched by `setup.sh`: the colour image, its hidden bits and the depth image after every primitive, and each pixel's LOD inputs and result, on random suites, minimal streams and captured dumps. Built with `RDP_EXACT` the model agrees on every random suite; without it, the differences [rdp_exact.md](rdp_exact.md) lists. `build_rtl.sh` builds the model without `RDP_EXACT` |
| the combiner's arithmetic | `sim/rdp/cc_unit.sh` | the functions between `cc_begin` and `cc_end` of `rdp_pix.v`, extracted and compared with the model's over a stride of the operand space and random operands |
| the dither | `sim/rdp_dither/tb_dither.sv`, `gradient_test.c` | the threshold table and the adder for every value; a shaded gradient through the model for each RGB_DITHER_SEL |
| the command fetch | `DPC=1 ./build_rtl.sh`, `obj_rdp_dpc/tb_dpc task...` | the RSP's RTL with `rdp_dpc` (`rsp_dpc_wrap.sv`): the words handed on are the RSP model's, in order, on captured tasks, on `gen_xbus_task.py`'s XBUS ones and on `gen_xbus_ring.py`'s (a ring in DMEM, START and END pairs that wait; `TB_CREADY=15`, a slow RDP, keeps lists under way) |
| the scan-out's filters | `sim/vi_filter` | the pads against a model of the filters, pixel for pixel |
| in the SoC | `sim/mips_soc` (`FB_N64=1` for frame dumps) | a program on the whole SoC: RSP, command fetch, RDP, scan-out |

The bench's memory answers with random stalls and read latency. Its
switches:

* `TB_STEP=1` compares after every command and stops at the first that
  differs; with `TB_LFSR=1` it compares the LFSR too.
* `TB_CW=<width>` sets the random stream's colour image width; a width that
  is not a multiple of 16 makes rows share hidden-plane groups.
* `TB_CC=1` makes random streams in which the combiner's result reaches
  memory unblended.
* `TB_C8=1` makes the random stream's colour image an 8-bit one, `TB_C32=1`
  a 32-bit one (its width by the seed, or `TB_CW`).
* `TB_LOD=1` makes random streams for the texture LOD (above); without it
  the streams are what they were.
* `TB_SDRAM=1` gives the memory the SDRAM controller's timing (rows, turns
  between reads and writes, refresh); `TB_CPU=<percent>` adds another master.
* `TB_FAST=1` is a memory that takes every request; `TB_PROF=1` prints the
  cycles spent in each sequencer state; `TB_CMDT=<file>` logs when each
  command word is taken.

`tools/area2/rdp_regress.sh` builds the bench and runs the lot: random seeds
at colour image widths 292, 296 and 320, seeds with `TB_LFSR` and `TB_STEP`,
seeds with `TB_SDRAM` and `TB_CPU=30`, seeds with an 8-bit colour image
(`TB_C8`, at widths 320 and 292 and with `TB_SDRAM`), seeds with a 32-bit
colour image (`TB_C32`, and with `TB_SDRAM`), seeds with `TB_CC`, and
`cc_unit.sh`.
A run passes when the bench prints `RTL == model`.

A frame's time is the RSP's and the RDP's together, on one SDRAM with the
CPU and the scan-out:

* `sim/rdp/rdp_flow.py` puts two benches' logs together (`tb_rsp` with
  `DPCT=<file>`: when each command is handed over; `tb_rdp` with
  `TB_CMDT=<file>`: what each takes to draw) and gives the frame's end for
  the two as they are and for either scaled.
* `lang/mips/test/rsp_replay.c` replays a task dump on the whole SoC in
  `sim/mips_soc` (`PRELOAD=<dump>@0x437FFFF0`; `MEMSTAT=1` for the SDRAM's
  use by port): the RSP's time to its BREAK and the RDP's to its SYNC FULL,
  with the scan-out reading and the CPU idle or busy in memory.

## Resources

Where the area goes, without figures:

* **Block RAM.** Two copies of TMEM in true dual-port memories, each of
  four memories and a TLUT copy. The line buffers: depth and colour as read, depth and colour as
  produced, their pass masks, and the raw hidden words. The attribute
  staging RAM.
* **RAM in place of registers.** The eight tiles are two 8-entry RAMs, read
  into registers once a pass, at the pass's tile (the primitive's, the next,
  or with the LOD the level's). What the texture stages do not read goes
  down the pixel pipeline in a RAM, and so do the LOD's S, T and fraction. The attributes'
  row-start values and their DaDe are LUT RAM, stepped one attribute a clock;
  a copy of their DaDx sits in that RAM's free rows, for the span setup.
* **Multipliers.** One shared 32 x 32 multiplier does all the setup
  arithmetic of the sequencer: edge and attribute start values, row offsets,
  load addresses. The pixel pipeline has its own for the perspective divide
  (the reciprocal's table in logic),
  the tile shift, the TMEM row address, the filter, the two combiner cycles,
  the coverage and the two blender cycles with their divide.
* **Registered stages.** Each combiner and blender cycle is two stages, the
  operand selects then the arithmetic, and the decoded texels are registered
  before the filter.

### A timing option kept in reserve

The YUV conversion's input is selected in front of its multiplies: the
pass's own corner texel, or with CONVERT_ONE the first pass's texel. That
select sits on one of the system clock's longest paths (from the pass bit
through the multiplies to the texel the pass gives). It can be taken off:
a second pass that converts gives no filtered texel, so its corner register
(`ta8b`, stage P8b) is free and can be loaded with the first pass's texel,
and the conversion then reads the corner alone. The result is the same
pixel for pixel. It closes timing on more fitter seeds, but the fitter
spends more logic on it than it saves, so it is not done while timing
closes without it: it is the change to make if a later addition makes the
system clock's timing hard to close.
