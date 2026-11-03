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
| 4 | TMEM busy | a texture load runs |
| 5 | PIPE busy | as bit 3; also cleared by bit 7 of a STATUS write |
| 6 | command busy | a command waits in the FIFO or is under way |
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

`rdp_top` executes one command at a time, but takes the next one's words while
the one before executes. Its length comes from the first word:
a triangle is 8 words plus 16 for shade, 16 for texture and 4 for Z; a
texture rectangle is 4; everything else is 2. The first two words go to
registers of their own (`nw0`, `nw1`: the executing command keeps its copy),
and every word to a 64-word staging RAM (word k at k), from which a triangle's
edges (words 2 to 7), its attributes (from 8 on) and a texture rectangle's S,
T and steps (2 and 3) are read back when the command starts (`S_TLOAD`,
`S_VSET`). The words are taken once the command executing has read its own out
of that RAM (not in `S_TLOAD`, `S_XL`, `S_VSET`): a triangle's up to 44 words,
a word a clock, come in under the rows of the primitive before it.

State commands start only when the primitive before them has finished and
drained, so the pixel pipeline never sees a state change with pixels in
flight. A triangle starts earlier: while the primitive before it drains (its
last pixels through the pipeline and back in memory) it reads its words, sets
up its attributes and edges and walks its first row, since none of that is
read by the pixels in flight. What the pixel pipeline reads of it (its flags,
its tile, the LOD's walk and level count, the depth's dz code) waits in
registers of their own (`n_*`, `dzc_n`) until its first span is asked for of
memory, which waits for the drain.

| op | command | notes |
|---|---|---|
| 0x00 | no-op | |
| 0x08-0x0F | triangle | edges, then shade, texture and Z coefficients by the opcode's low three bits |
| 0x24, 0x25 | texture rectangle, flipped | |
| 0x26-0x28 | sync load, pipe, tile | nothing to do: commands run in order |
| 0x29 | sync full | completes when every earlier write has been accepted by the memory |
| 0x2D | set scissor | the four coordinates and the field mode: with it only the rows of the field kept (the odd bit) are drawn, and the dither's row is the field's own, y / 2 |
| 0x2E | set primitive depth | Z (15 bits) and its delta |
| 0x2F | set other modes | what is read of them: below |
| 0x30 | load TLUT | also sets the tile's size, as every load does |
| 0x32 | set tile size | |
| 0x33 | load block | also sets the tile's size (its lrt: the dxt) |
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

A primitive is drawn row by row. A row walker (`wk`) and the pixel sequencer
(`st`) work on consecutive rows at once. For each row the walker:

1. finds the row's span (the edge walker for a triangle, fixed columns for a
   rectangle);
2. asks the memory engine for what the pixels need of memory: the depth span
   with Z_CMP, the colour span when the blender or the coverage reads it, and
   the span's hidden coverage words;

and the sequencer, when the pixels of the row before are all issued and the
walker has the next row ready (`S_GO`):

3. sets up the attributes at the span's first pixel;
4. issues the pixels, one a clock as their memory words come in;
5. asks for the write-back of the row's results.

The walker goes on to the row after the next while the pixels of this one
issue: the next row's subscanlines are walked (the registers of the walk that
the pixels read are copied where the row is taken, `pc_*`), the step of the
attributes' row values runs, and the row's reads are asked for, so that its
words are coming into the other set of line buffers while the row before is
drawn. The read engine takes the request when the set's last user is done with
it: the last pixel of the row two before has left the old buffers (`old_hold`)
and, for the hidden words, the write engine is not merging from the set. The
walker holds back while the row being drawn has not asked for its write-back,
since the row's base address in the images moves on then. A 32-bit colour
image's rows are not walked ahead (a span of more than 512 pixels is two rows
of the sets, the second half read when the first is done).

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
  attribute's value, DaDx, DaDe and DaDy are put together there from their integer
  and fraction halves. The row-start values step by DaDe each row. A row's span starts as the
  console starts it: from the major edge's pixel, `((e & ~0x1FF) + dif - xfrac * ((DaDx >> 8) & ~1)) &
  ~0x3FF`, e the row's value on the edge, xfrac the edge's 8-bit x fraction on the subscanline the attributes
  are taken on and dif, when the edge leans away from the walk, 3/4 of DaDe - DaDy (the low 9 bits of each cleared);
  a pixel is that plus its distance from the edge's pixel times DaDx without its low 5 bits (Z whole)
  ([rdp_exact.md](rdp_exact.md), rule 4).
* **Z and the shade.** A pixel without full coverage takes its Z and shade at its first covered
  sample (the topmost covered subscanline's leftmost one, `ox` quarters right and `oy` subscanlines down), by the
  steps `(A >> 14) * 4 + ox * DaDx' + oy * DaDy'` for the shade (13-bit steps, 9 bits kept: 256..383 is 255, 384..511
  is 0) and the same of the attribute >> 10 for Z (22 bits, the 19-bit result kept to 18: 10 is all ones, 11 is 0).
* **S, T, W.** S and T are s10.5 in the integer half and W is s.15; they are
  shifted into the pipeline's units where the pixels start.
* **Depth step.** From DzDx and DzDy the walker derives the primitive's
  depth step: all ones under the top bit of |DzDx| + |DzDy|, in the stored
  depth's units. The depth test and the blender use it (below).

A triangle whose command has no texture block has S = T = W = 0, and the
texture stages run all the same: TEXEL0 is the tile's texel (0, 0), or with
PERSP, where W = 0 is out of range and S and T saturate to 0x7FFF, its last
texel (the far corner, clamped). In 2-cycle mode TEXEL1 is the next tile's at
the same point, as for any primitive.

### Rectangles

A rectangle's pixels are whole: all eight samples. A texture rectangle in
copy mode and a fill rectangle in fill mode take the corners' integer parts,
the lower-right corner inclusive, and reach the scissor's right edge itself:
its column is written too, as the console's. Otherwise the corners' quarter pixels
count, as for a triangle without AA_EN: a pixel is drawn where its top-left
sample is inside, `XH <= 4x < XL` and `YH <= 4y < YL`, so a corner with a
fraction leaves out the pixel it falls in at the upper left and takes it in
at the lower right (a run of rectangles placed at fractions of a pixel then
meets without a gap). Its depth is the primitive depth.

A texture rectangle steps S by DsDx along the row and T by DtDy down the
rows; the flipped one swaps the two. The attribute stepped down the rows
starts at YH's whole row. The one stepped along the row is the plane at XH
itself: at column x, its value plus DsDx (or DtDy) times `(4x - XH) / 4` --
in copy mode the one stepped along the row steps once a group of 4 pixels (8 in an 8-bit
image), from the first pixel drawn (a scissor on the left shifts the texture, it does not cut it), by DsDx (DtDy)
counted as a quarter of its value, and the pixel's place in its group (0 to 3, 0 to 7) is added to its texel,
which is mirrored and masked (a mask of at most 10 bits) and never clamped ([rdp_exact.md](rdp_exact.md), rule 6).
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
  at the end it went past (the model's `rdp_n64_div`). Without
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
the second) converts a texel instead of filtering it: the texel at the
coordinates, with SET CONVERT's K0 to K3 (signed, as `2K + 1` 256ths):
`R = Y + K0 V`, `G = Y + K1 U + K2 V`, `B = Y + K3 U`, and the alpha Y, with U,
V and Y the texel's R, G and B. A YUV tile's U and V are taken less 128; the
texel of any other format goes in as it is (an intensity texel is U = V = Y).
With CONVERT_ONE the second cycle converts the first cycle's texel, whatever
its tile's format: a game that reads TEXEL1 in the first combiner cycle with
CONVERT_ONE set and BI_LERP_1 clear gets that conversion, not the next tile's
texel. Each channel is clamped to a byte.

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
* **Copy.** The pixel is the texel. A 16-bit texel, and a TLUT entry, is
  copied as it is (an IA or I texel too: the console moves TMEM's words, it
  does not decode them); a 4-, 8- or 32-bit texel is decoded and written as
  RGBA 5551. The alpha compare's threshold is 128,
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
* SHADE is a byte, 0..255, when it enters the pipeline: 9 bits of it, 256..383 saturated to 255 and 384..511 (a negative shade) to 0.
* The primitive LOD fraction is an input, and the LOD fraction (a signed
  9-bit number, as K5) in a 2-cycle primitive with the LOD ([lod.md](lod.md));
  in 1-cycle mode all ones for a primitive with a level count of 0 (without
  sharpen and detail: the console's fraction there whatever the steps);
  elsewhere it reads 0. The key inputs read 0.
* **NOISE** (A 7 of a colour) is `(n << 6) | 0x20` for three random bits n, a
  new n each pixel: the alpha dither's noise bits, as the console's one random
  number serves both. Read as COMBINED is there (from 0x180 negative).
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
* **Noise.** A 23-bit LFSR (the console's taps, reset to 0), in two modes. By default
  it is free-running, as the console's: it steps on every clock of the RDP's, and a
  pixel takes the bits it finds when it reaches the stage that uses them (the combiner's NOISE at the
  texture stages' end, the alpha dither's and compare's at the combiner's last stage, the colour dither's at the blender's last). Built with
  `RDP_LFSR_PIXEL` (Verilog; `VDEFS=-DRDP_LFSR_PIXEL` of `sim/rdp/build_rtl.sh`, which sets it by default)
  it steps for every pixel with coverage, outside fill mode, as the C model does, and a pixel's
  bits are the state before its own step, so that the lockstep benches compare RTL and model word for word. In the default
  mode a pixel that reads the noise (the noise colour dither, the alpha dither's noise, DITHER_ALPHA_EN, NOISE in the combiner) is not
  reproducible against the model or the oracle, whose generators are other sequences anyway (the oracle's is its own): such pixels stay out
  of every comparison.
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

The pixel's dz code is the console's four bits: the place of the top bit of
|DzDx| + |DzDy| of the integer halves, plus 1 (15 from 0x4000 up; 0 for a
primitive without Z coefficients; SET_PRIM_DEPTH's delta for the primitive
depth). Its top two bits are the word's two low bits, and its low two are the
depth image's hidden bits, in the hidden plane (below) at the depth image's
addresses, written with the depth.

The compares are of the 18 bits against what the word keeps. The modes, with
dz the larger power of 2 of the pixel's dz and the memory's, times 8 (the
memory's is the code the word and the plane hold, doubled for a word of
exponent under 3 and then no less than 16 >> exponent, so that a surface drawn
over itself is within it at any depth; a dz of 0x8000, the code 15 or 14 doubled,
is within at any depth):

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
* With the memory's coverage as B (and Z_CMP), the two weights are shifted by the
  difference of the two dz codes: the A weight right by the pixel's less the
  memory's, the B weight by the memory's less the pixel's, up to 4. Without
  Z_CMP the B weight shifts by the pixel's own code (4 under 11, else 15 less
  the code).

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
pixels a 32-bit word with the left one first. A 16-bit image may be of any
width: when it is odd, every second row starts on the odd halfword of a memory
word (the pixel's address is the image's plus twice its index in the image,
y times its width plus x). The RTL counts such a row's columns in its words
from that halfword (`par` in `rdp_top.v`: bit 1 of the row's byte address, added
to the span's first and last column and so to every pixel's place in the line
buffers, the reads and the write-back); what depends on the column itself --
the dither -- keeps it (`rdp_pix.v` carries the bit along). The fill
colour's half a pixel takes is the memory halfword's, by the address's bit 1.
The depth image is taken to start its rows on the same halfword (the
two images' addresses are multiples of 8 bytes). An 8-bit or a 32-bit image
(below) is not given this: an 8-bit one's rows must start on a 4-byte boundary,
a 32-bit one's always do, and the depth image of those is not shifted.

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
* **The depth image's** two bits a pixel are in the same plane, at the depth
  image's addresses: the low two bits of the pixel's dz code, which the next
  depth compare reads back (rule 8 of [rdp_exact.md](rdp_exact.md)). A span
  with Z_CMP or Z_UPD reads the groups' words of its depth span after the
  colour span's, and the write engine's second pass, the depth words, writes
  each group back as the colour pass does. (A 32-bit colour image has no
  hidden coverage bits; its depth image has these.) A fill of the depth image
  as the colour image writes {a, a} there, 0 for 0xFFFC, the usual clear.
* **Start.** The runtime starts the plane all ones (`n64rt_boot.c`).

### The memory port

`rdp_mem.v` makes every access, over one port: a command channel (write
flag and 4-aligned byte address), a write data channel with byte enables
that follows its command, and read data returned in order. Reads go before
writes. In the SoC the port is one of the SDRAM arbiter's.

Nothing in the engine orders a read after a write of the same word. The
sequencer starts a primitive's reads, and a load, only with the write engine
idle; a row's reads, one row ahead of its pixels, wait for the write engine to
be idle when the image's rows can share a word (a width that is not a multiple
of 16 pixels).

**Where the hidden plane is in the SDRAM's banks.** The SDRAM keeps one row
open in each of its four banks (a byte address's bank is its bits 12:11), and
the colour image's row, the depth image's and the hidden words of each are four
streams that the RDP reads and writes by turns. The plane is laid out so that
the hidden words of an image's row are never in that row's own bank (`hmap` in
`rtl/soc/sdr_arb.sv`: a bijection of the plane's own words, which the CPU, the
scan-out and the RDP all see linear): they no longer close each other's row at
each change between a group's colour words and its hidden word. It does not
help when the colour and the depth image are in the same bank (their addresses
equal modulo 8 KiB), whose rows still close each other's.

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

The model built with `RDP_EXACT` follows the console on every item below that is a rule of
[rdp_exact.md](rdp_exact.md); `rtl/rdp` follows rules 1, 2, 4, 5, 8 and 6's copy mode of it, the
others not. The RTL, and the model as its benches build it, do not follow:

* **Depth, 2-cycle mode**: with the memory's coverage as B in the first blender cycle, the console takes the shifts
  from the pixel before's dz (and the pixel's own code); here that cycle shifts by the pixel's own. No captured
  task blends so (every captured task agrees with the oracle apart from the noise dither, below).
* **LOD**: in 2-cycle mode only. The console computes it in 1-cycle and
  copy mode too, from the next two pixels of the walk; here a 1-cycle pixel
  samples its primitive's tile and the LOD fraction is the console's only at a
  level count of 0 (all ones), 0 otherwise (rule 3: `RDP_EXACT`'s model does it; no captured task
  draws with it).
* **TEXEL1** is the next tile at the same coordinates. In the second cycle
  of a 2-cycle pixel it is the tile's own sample, not the next pixel's (rule 7; no captured task reads it
  there).
* **Edges, spans, rectangles** (rule 6): an edge's 12 bits of whole pixels and the rows' limits are the model's
  own 32-bit ones (a row with an edge past 2,047 pixels is drawn), a rectangle is walked by its own
  formulas (a fractional corner's partial coverage is not the console's; rectangles in a captured task do not differ).
* **Colour images**: an 8-bit one is written only (no image read, no depth,
  no hidden coverage). A 32-bit one is drawn as the model draws it, from
  the programming manual's rules ([cimg32.md](cimg32.md),
  "Choices"); a 32-bit picture is shown at five bits a channel.
* **YUV** textures are converted only from 16-bit YUV tiles, each channel
  clamped to a byte (the console keeps 9 bits); a pass that converts takes
  the texel at the coordinates, unfiltered. SET_KEY is skipped; the key
  inputs of the combiner read 0. The noise is the RDP's own LFSR, not the
  console's sequence.
* **Other modes**: the key enable is not read.

Done, as the console does them: the shade and Z at a pixel's first covered sample, the dz code the
depth word keeps, a triangle's attributes from the major edge's pixel and fraction (the span's start), a
texture rectangle's LOD next line, and a texture rectangle's copy mode (groups of 4 or 8 pixels,
the texel never clamped), and the depth image's own dz (the depth compare's tolerance and the blender's coverage
weights, with the dz code's low two bits written to the hidden plane). Of 138 captured tasks measured against the independent
pixel-exact software RDP ([rdp_exact.md](rdp_exact.md)), the model of `rtl/rdp` differs in 38 primitives and 153,707
colour words, from 51,889 and 383,878 before these rules (nearly all the words are the noise dither's, in 27 primitives, whose
random numbers each side has of its own).

## How it is verified

| what | where | checks |
|---|---|---|
| a frame in software | `sim/rdp/rdp_task` (`make`) | a captured task dump through the RSP's model (`sim/rsp`) and the reference model, to a PPM; `-n` counts the commands by opcode, `-v` lists them, `rdp_stats.py` summarises what the primitives use |
| RTL against the model, captured tasks | `sim/rdp/build_rtl.sh`, `obj_rdp/tb_rdp task...` | the RSP model's commands go to both, each with its own copy of memory; every byte, hidden plane included, must match |
| RTL against the model, random | `tb_rdp -r SEED [N]` | random state, loads of every kind from any alignment, triangles and rectangles in every cycle type |
| 32-bit colour images | `TB_C32=1 tb_rdp -r SEED [N]` | random streams into a 32-bit colour image at widths 320, 296, 640 and 1,000 (the wide ones' spans in two halves) |
| the texture LOD | `TB_LOD=1 tb_rdp -r SEED [N]` | random streams made for the LOD (2-cycle mostly, DaDy on the scale of DaDx, the LOD blend combiner, the minimum level) |
| the console's rules | `sim/rdp_oracle` (`./setup.sh`, `make`, `./run.sh 50`); `make rdp_task EXACT=1` in `sim/rdp` | the model, command for command, against an independent pixel-exact software RDP fetched by `setup.sh`: the colour image, its hidden bits and the depth image after every primitive, and each pixel's LOD inputs and result, on random suites, minimal streams and captured dumps. Built with `RDP_EXACT` the model agrees on every random suite; `rdpo_x`, the model with the rules `rtl/rdp` follows, differs where [rdp_exact.md](rdp_exact.md) says. `build_rtl.sh` builds the model with those rules (`RDP_X_*`) |
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
* `TB_C16=1` keeps the colour image a 16-bit one (a random stream otherwise changes its size now and then); with an odd
  `TB_CW` the rows start on the odd halfword of a word.
* `TB_CC=1` makes random streams in which the combiner's result reaches
  memory unblended.
* `TB_C8=1` makes the random stream's colour image an 8-bit one, `TB_C32=1`
  a 32-bit one (its width by the seed, or `TB_CW`).
* `TB_LOD=1` makes random streams for the texture LOD (above); without it
  the streams are what they were.
* `TB_SDRAM=1` gives the memory the SDRAM controller's timing (rows, turns
  between reads and writes, refresh); `TB_CPU=<percent>` adds another master.
* `TB_FAST=1` is a memory that takes every request; `TB_PROF=1` prints the
  cycles spent in each sequencer state with the memory commands taken in it,
  and why the pixel state waits (for the row's write-back request, for the row
  before's, for the row's reads to be taken or for their words);
  `TB_CMDT=<file>` logs when each command word is taken; `TB_CLS=<file>` the
  time of every command (its start and the next's) for `sim/rdp/rdp_classes.py`,
  which adds it up by kind of primitive.
* `TB_HREMAP=<k>` (with `TB_SDRAM=1`) places the hidden plane in the SDRAM's
  banks as `rtl/soc/sdr_arb.sv` does (k = 2: the bank two on from the image's
  row's); `TB_MISS=1` counts the SDRAM's row changes by what they are between
  (colour, depth, hidden words of either).

`TB_ZDZ=1` makes streams of shaded, anti-aliased, depth-tested triangles that are points of a few planes (slopes of every dz code, now and then a step within a code's tolerance), the blender's B mostly the memory's coverage: the depth image's own dz.

`TB_FREE=1` checks that the free-running LFSR steps on every clock of the bench (on a build with `VDEFS=` empty: `obj_free`);
`TB_NONOISE=1` makes streams in which nothing reads the noise (RTL == model in either mode), `TB_NOISE=1` streams with the noise colour
dither alone and no read of the colour image, in which the free-running RTL differs from the model in colour words by a step of a
5-bit channel at most (the dither adds one) and in nothing else.

`TB_COPY=1` makes copy mode three modes in four (texture rectangles by groups of pixels, with `TB_C8=1` in groups of 8), `TB_RECT=1` adds texture rectangles (with `TB_LOD=1`, a rectangle's LOD next line).

`tools/area2/rdp_regress.sh` builds the bench and runs the lot: random seeds
at colour image widths 292, 296 and 320 (and, with `TB_C16`, odd ones: 289 and 301, also with `TB_COPY`, `TB_ZDZ` and the SDRAM's timing), seeds with `TB_LFSR` and `TB_STEP`,
seeds of the free-running LFSR (`TB_FREE` with `TB_NONOISE`, with `TB_ZDZ` and the SDRAM's timing, and `TB_NOISE`), seeds with `TB_SDRAM` and `TB_CPU=30`, seeds with an 8-bit colour image
(`TB_C8`, at widths 320 and 292 and with `TB_SDRAM`), seeds with a 32-bit
colour image (`TB_C32`, and with `TB_SDRAM`), seeds with `TB_CC`, seeds with `TB_COPY` (also 8-bit, and with the SDRAM's timing), seeds with `TB_LOD` and `TB_RECT`, seeds with `TB_ZDZ` (also at a width whose rows share depth-hidden groups, with the SDRAM's timing and 32-bit images), and
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

## Speed

The RDP's time on 138 captured graphics tasks (92,498 primitives, 34.8 million pixels), with `tb_rdp` (`sim/rdp/rdp_classes.py`
for the table by kind of primitive), in clock cycles of the RDP alone:

| the memory | before | after | |
|---|---|---|---|
| every request taken at once, a word 6 clocks on (`TB_FAST=1`) | 83,374,871 | 75,127,338 | -9.9 % |
| the SDRAM controller's rules (`TB_SDRAM=1`), the hidden plane in the banks it was | 97,442,135 | 88,630,707 | -9.0 % |
| the same with the hidden plane's banks (`TB_HREMAP=2`; `sdr_arb.sv`) | 93,694,655 | 84,514,421 | -13.3 % |

(the hidden plane's banks alone take 3.8 % off the before). A task takes 0.80 to 1.00 of its time before, 0.86 at the median (the
SDRAM's rules with the banks), and 19 of the 138 tasks need more than a 60 Hz frame of the RDP's time, where 25 did. With another
master asking the SDRAM at random for a fifth of the cycles (`TB_CPU=20`, an access that always changes a row) the time is 227.7
million before and 214.1 million after (-6.0 %): what is taken by the other master the RDP does not get back. On the whole SoC
(`sim/mips_soc`, `RCPSTAT=60`, a game's logos, title and first scenes, 660 fields, the RSP's time the same in both) the time the RDP
is busy in a window of 60 fields is 8 to 16 % shorter, 3,265 ms to 2,896 ms (-11.3 %) from the second window on. With every request
taken at once, by kind of primitive:

| | commands | before | after | clocks a pixel before, after |
|---|---|---|---|---|
| triangles, shade, texture, depth test, 1-cycle | 34,138 | 19.0 M | 16.2 M (-14.8 %) | 6.95, 5.92 |
| triangles, shade, texture, depth test, 2-cycle | 28,472 | 17.2 M | 14.7 M (-14.5 %) | 8.42, 7.19 |
| texture rectangles, 1-cycle | 9,007 | 11.2 M | 10.0 M (-10.5 %) | 1.48, 1.32 |
| texture loads | 24,692 | 9.7 M | 9.6 M (-1.5 %) | |
| fill rectangles, fill mode | 6,312 | 9.0 M | 9.0 M | 0.61, 0.61 |
| triangles, shade and depth test (small ones) | 9,358 | 2.8 M | 2.3 M (-18 %) | 16, 13 |

Where the time went before, with every request taken at once: the pixel state 40 % (of which one clock in five was spent
waiting for the row's words), the triangles' setup 12 % (span setup 11 clocks a row, 5 for the walk, one to start and
end a row), their attributes 6 %, the command words 5 % (a triangle's 44 words, a word a clock, with the RDP doing nothing
else), texture loads 11 % (87 % of their clocks a memory word: they are the memory's), fill 11 % (95 %: a word of two
pixels a clock). With the SDRAM's rules a fifth of the clocks are row changes: 4.7 million of them for 46.7 million words
moved, 40 % of them between a group of pixels and its hidden word, which the banks' placement takes away.

What is not faster, and why:

* **Fills and texture loads** are the memory's own rate, a 32-bit word a clock.
* **A small triangle's rows** (8 to 10 pixels, most triangles of a 3D scene) are bound by the two sets of line buffers: a set
  is read, drawn, drained (14 clocks from the last pixel's issue to its last result) and written back before it is read
  again, about 40 clocks for two rows. Setting a row up in nothing (a test with the span setup at 2 clocks, not kept)
  takes 1.2 % more off, as the stalls for the row's words and for the write-back move to where the setup was. A third
  set of buffers would be about a dozen more block RAMs and a change of the addressing of every buffer in `rdp_pix`.
* **The span setup's 11 clocks** could run under the row before, with the first values of its 8 attributes held in about
  230 more registers: not done for the 1.2 % above.
* **A triangle's attributes** (72 clocks) read each attribute's group of 16 words once for each of its four (three)
  attributes; reading them once would take them to 44 clocks (2.6 % of the time) with registers for four attributes at
  once, or a second read port of the staging RAM and a new step sequence.
* **A bank hash** of the SDRAM (the bank xor-ed with the row's bits) took 0.5 % off the before, and on top of the hidden
  plane's placement it made the time 5 % longer (the placement is the part of the layout that is known).

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
  the coverage and the two blender cycles with their divide, and the step of the
  shade and of Z to a pixel's first covered sample (one block a shade channel, two for Z).
* **Registered stages.** Each combiner and blender cycle is two stages, the
  operand selects then the arithmetic, and the decoded texels are registered
  before the filter.

The console's rules ([rdp_exact.md](rdp_exact.md): the shade and Z at a pixel's first covered sample, the span's start from the
major edge's pixel, a rectangle's LOD next line, copy mode by groups) cost 291 fitted ALMs in the RDP (3 seeds, 4,891 to 5,182)
and 2 DSP blocks fewer (Z's), no block RAM, and a row's setup one clock more (the RDP's cycles on 138 captured tasks: 95,115,501 to 95,126,383, +0.01 %);
sim/area_study.md, round 7.

The depth image's own dz (rule 8: its hidden bits read and written with the depth) costs 109 more ALMs in the RDP (3 seeds: 5,289 / 5,282 / 5,303
against 5,181 / 5,179 / 5,186), 60 more registers and 3 more block RAMs (the pixels' copy of the depth image's raw hidden buffer, and the write
engine's copy twice as deep; the new depth buffer is 36 bits wide in the same four blocks), no DSP block. It also takes a hidden-plane word read and
another written for each 16 pixels of a depth span, and its own row of the SDRAM: the RDP's cycles on 138 captured tasks go from 95,133,674 to
96,770,887 (+1.7 %) with the bench's memory, and from 91,983,477 to 97,401,049 (+5.9 %) with the SDRAM controller's row timing
(`TB_SDRAM=1`: the hidden words' rows of the colour image and of the depth image are two more row changes a span);
sim/area_study.md, round 8.

The commands' words coming in under the rows of the one before, a triangle starting under the drain of the primitive
before and the row walker ([Speed](#speed)) cost the RDP alone 173 ALUTs and 231 registers (7,539 to 7,712 ALUTs, 6,199 to 6,430
registers, `tools/area2/ooc.sh rdpw`; no block RAM, no DSP block): the first two words of the next command (64), what the
pixel pipeline reads of a triangle that starts under a drain (16), the columns of a read request (21), the copies of the
subscanlines' registers the pixels read (116), the read engine's counters by set (21) and the walker's own few. In the whole design the same seeds took 56 to 66 more ALMs (seed 3: 17,169 to 17,225,
seed 4: 17,173 to 17,239, seed 1: 17,204 to 17,280); the fitter's own spread from one seed to the next is a hundred and more.

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
