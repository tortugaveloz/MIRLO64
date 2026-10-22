# MIRLO 64: texture level of detail

The N64's RDP picks, for each pixel of a mipmapped primitive, which level of a
texture's mip chain to sample and how far it is toward the next one. This page
is the LOD as the reference model computes it, the design that was weighed for
the RTL, and the RTL that implements it
([rdp.md](rdp.md), "TEXEL1 and LOD").

The model is the contract ([rdp.md](rdp.md), "How it is verified"): the LOD was
put in the model first, and the RTL was made to match it.

| what | where |
|---|---|
| where | the N64's pixel pipeline of the model (`RDP_N64`) |
| the computation | `lang/c/rdp/rdp.c`, the LOD section: `rdp_lod` (declared in `rdp.h`), `lod_pixel`, `lod_dist`, `lod_tri` |
| the console's perspective divide | `rdp_n64_div`, for the pixel and the LOD's points, as the RTL has it |
| the console's rules | with `RDP_EXACT` ([rdp_exact.md](rdp_exact.md)): the console's attributes, and the LOD in 1-cycle and copy mode (`lod1_pixel`); not in the RTL |
| where it is used | `draw_pixel_cvg`: the tiles of TEXEL0 and TEXEL1, and LOD_FRACTION into the combiner |
| a test | `lang/c/rdp/test_lod.c` (its header has the build line) |
| a frame | `make` in `sim/rdp`: `rdp_task` counts the pixels that took the LOD, by level |
| the RTL bench | `./build_rtl.sh` in `sim/rdp`, and `TB_LOD=1` for `tb_rdp`'s LOD streams |

The perspective divide on this page is the RDP's per-pixel divide of the
texture coordinates S and T by W, not the CPU's integer division. "The
console's perspective divide" is that divide as the N64's RDP computes it (its
reciprocal tables, `rdp_n64_div`); "the model's own" was the model's
reciprocal and product (`rdp_rcp`, `rdp_persp`), which the N64's pixel
pipeline used before the LOD and which only RDP's own triangles still take.

## When it applies

* **2-cycle mode only.** In 1-cycle, copy and fill mode nothing changes (see
  "Choices" for what the N64 does in 1-cycle mode). With `RDP_EXACT`, 1-cycle
  and copy mode too, as the console does ([rdp_exact.md](rdp_exact.md), rule 3).
* **TEX_LOD** (bit 16 of SET_OTHER_MODES' high word) moves the tiles.
* The **LOD fraction** is computed for every pixel of a 2-cycle primitive
  with TEX_LOD, or whose combiner reads LOD_FRACTION (C 13 in a colour, C 0 in
  an alpha, either cycle), with or without TEX_LOD.
* **DETAIL** (bit 18) and **SHARPEN** (bit 17) change the fraction and the
  tiles, as below.
* The **level count** is the triangle command's level field (bits 21:19 of
  its first word): the last level, so a primitive with a mip chain of six
  levels has 5. A texture rectangle has 0.
* The **minimum level** is SET_PRIM_COLOR's bits 12:8 (5 bits, u0.5 texels),
  next to the primitive LOD fraction (bits 7:0).

## The algorithm

### Three points through the perspective divide

For each pixel, S and T are taken at three points:

| point | S, T, W |
|---|---|
| the pixel | the pixel's own attributes (what it samples with) |
| the next pixel | plus DaDx, toward where the N64 walks: a triangle's span starts at its major edge, so with `lft` set (major edge on the left) the next pixel is x + 1, and without it x - 1 (DaDx negated). A rectangle is walked left to right. |
| the next line | plus DaDy, of which the top 17 bits are kept (`& ~0x7FFF`) |

The attributes are the model's: S and T s10.5 in the integer half of a
32-bit word (s10.5.16), W s.15 in the integer half, each pixel's the plane at
its top-left corner, stepped by the whole DaDx. DaDy of S, T and W comes from
the triangle's texture block (words 10 and 14, as the other coefficients'
halves). With `RDP_EXACT` they are the console's: each span started at the
major edge's pixel and stepped by DaDx without its low 5 bits, the next pixel
too ([rdp_exact.md](rdp_exact.md), rule 4).

Each point's S and T go through the same perspective divide as the pixel's own:

* **Perspective** (PERSP): the console's perspective divide, `rdp_n64_div`
  (the 16-bit integer halves of S and W, the console's two 64-entry tables;
  [rdp.md](rdp.md), "Coordinates"), but not saturated to 16 bits: the result
  is kept as s11.5 in 17 bits, and a value outside that range, or a W whose
  integer half is zero or negative, makes the pixel **out of range**.
* **Affine**: S >> 16, the s10.5 the pixel samples with, sign-extended to 17
  bits. Never out of range.
* **A texture rectangle**: its own S and T (s10.5) at the column and row,
  from the rectangle's formula. Never out of range. With `RDP_EXACT` a
  rectangle is the console's primitive, walked as a triangle (rule 6 there):
  its next line is its row step kept to `LOD_DY_MASK` as a triangle's DaDy.

A triangle without texture coefficients has S = T = W = 0 at all three
points: affine, a step of 0 (magnified); with perspective, out of range.

### The distance

For each of the two other points, the step from the pixel in S and in T,
each 18 bits signed (`d = S1 - S0`). Its magnitude is `d` when positive and
its ones' complement `~d` (that is `|d| - 1`) when negative. The LOD is the
largest of the four magnitudes, kept to 15 bits: its low 14 bits, and bit 14
set when any of bits 16:14 is (a step of 512 texels or more). It is u10.5
texels a pixel: 32 is one texel.

### Level, fraction, far and magnified

| case | level | far | magnified | fraction |
|---|---|---|---|---|
| out of range, or LOD bit 14 | the last | yes | no | 0xFF |
| LOD < 32 | 0 | when the level count is 0 | yes | plain: 0 (0xFF when far); sharpen or detail: `max(LOD, min level) << 3`, with sharpen `\| 0x100` |
| LOD >= 32 | `log2((LOD >> 5) & 0xFF)` | when the level is at or past the level count, or LOD >= 0x2000, or the level count is 0 | no | plain and far: 0xFF; otherwise `((LOD << 3) >> level) & 0xFF` |

A far pixel's level is the level count (the last level). `log2` is the place
of the highest set bit (0 for 0 and 1): level k is a step of 2^k up to
2^(k+1) texels a pixel, and the fraction is the 8 bits under that bit, how far
the step is toward the next power of two.

The fraction is 9 bits. The combiner reads it as C (colour 13, alpha 0) as a
signed 9-bit number, as it reads K5: sharpen's `0x100 |` makes it negative,
`-256 + 8 * LOD` 256ths, so `(TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0` moves
away from the next level, a sharper texel, the more magnified the pixel is.

### The tiles

With TEX_LOD, the primitive's tile plus the level, masked to 3 bits:

| mode | TEXEL0 | TEXEL1 |
|---|---|---|
| plain or sharpen | tile + level | tile + level + 1; the same as TEXEL0 when far, or magnified without sharpen |
| detail, magnified | tile (the detail texture) | tile + 1 (level 0) |
| detail, not magnified | tile + level + 1 | tile + level + 2; tile + level + 1 when far |

Without TEX_LOD they stay the primitive's tile and the next.

A 2-cycle pixel goes through the texture stages twice when its first
combiner cycle reads TEXEL1 or its second reads TEXEL0, as without the LOD
([rdp.md](rdp.md), "TEXEL1 and LOD"): the first pass samples TEXEL0's tile,
the second TEXEL1's, and the second cycle's TEXEL0 is that second sample.

### Worked steps

`test_lod.c` checks `rdp_lod` on these (primitive tile 2, level count 5
unless said):

| step | LOD | level | tiles | fraction |
|---|---|---|---|---|
| 1 texel | 32 | 0 | 2, 3 | 0x00 |
| 2.5 texels | 80 | 1 | 3, 4 | 0x40 (a quarter of the way from 2 to 4) |
| 1 texel in S, 3.75 in T | 120 | 1 | 3, 4 | 0xE0 |
| half a texel | 16 | 0 | 2, 2 | 0x00 |
| half a texel, sharpen | 16 | 0 | 2, 3 | 0x180 (-0.5) |
| half a texel, sharpen, min level 24 | 16 | 0 | 2, 3 | 0x1C0 |
| half a texel, detail | 16 | 0 | 2, 3 | 0x80 |
| 2.5 texels, detail | 80 | 1 | 4, 5 | 0x40 |
| 16 texels, level count 2 | 512 | 2, far | 4, 4 | 0xFF |
| 2.5 texels, level count 0 | 80 | 0, far | 2, 2 | 0xFF |
| -1 texel | 31 | 0 | 2, 2 | 0x00 (magnified: the ones' complement) |
| 2.5 texels, no TEX_LOD | 80 | 1 | 2, 3 | 0x40 |

## Choices

Where the public descriptions of the RDP leave something open, the model makes
one choice, in one place, so that it can be changed when the RTL is compared
with captured frames.

1. **DaDy's precision** (`LOD_DY_MASK` in `rdp.c`): the top 17 bits of DaDy
   of S, T and W. The N64 keeps no more of them (they serve nothing else), and
   the RTL then needs 17-bit adders for the next line.
2. **A negative step** (`LOD_ONES_ABS`): its ones' complement, one less than
   its magnitude, as an inverter without the increment would give. A step of
   exactly one texel backward counts as magnified. Set to 0 for `|d|`.
3. **LOD_FRACTION without TEX_LOD** (`LOD_FRAC_USED`): computed whenever the
   combiner reads it; TEX_LOD only moves the tiles. Set to 0 to compute it
   only with TEX_LOD (LOD_FRACTION then reads 0 otherwise, as without the LOD).
4. **The perspective divide** (`lod_pixel`): the console's (`rdp_n64_div`),
   for the pixel and the three points alike, as the RTL has it; the range
   check is the 17 bits the console keeps there, and a W with a zero or
   negative integer half is out of range whatever S is, as the console's
   perspective divide reports it. (The model's own reciprocal, which the LOD
   took at first, had the console's range rule but not its values: S, T off
   by a few subtexels, and with them the fraction.) With all of `RDP_EXACT`
   the LOD agrees with the console's on every pixel of the comparison
   ([rdp_exact.md](rdp_exact.md)).
5. **The walk's direction** (`lod_tri`): the next pixel is x - 1 for a
   triangle without `lft`. The model draws every span left to right; only the
   LOD sees the N64's direction.
6. **Texture rectangles** (`draw_texrect`, `lod_pixel`): level count 0, so
   with TEX_LOD a rectangle samples its own tile (the far level, fraction 0xFF;
   detail moves it a tile on), and its other two points come from its own
   column and row formula, without the perspective divide, as its pixel's S
   and T do. The
   console's next line differs: its rectangle is a primitive whose DaDy is the
   row step (DtDy, DsDx flipped), kept to `LOD_DY_MASK` as a triangle's and
   added to the coordinate with its fraction; with `RDP_EXACT` the model does
   that ([rdp_exact.md](rdp_exact.md), rule 2).
7. **1-cycle mode**: unchanged. The console computes the LOD in 1-cycle mode
   too, from the next pixel and the one after it (with a look at the next
   row's span near a span's end), and with TEX_LOD it moves TEXEL0's tile;
   a 1-cycle combiner that reads LOD_FRACTION gets the fraction. Games mipmap
   in 2-cycle mode, and the one set of dumps here that sets TEX_LOD in 1-cycle
   mode does it with a level count of 0 and without DETAIL, where the tile does
   not move (below); with DETAIL it would move a tile on even at a level count
   of 0. With `RDP_EXACT` the model follows the console here, and in copy
   mode with TEX_LOD ([rdp_exact.md](rdp_exact.md), rule 3, where the span-end
   cases are written out).
8. **The second cycle's TEXEL0 and TEXEL1**: as without the LOD. On the console the
   second cycle's TEXEL1 is the next pixel's first texel; the model keeps the
   tile's own sample there, with or without the LOD. With `RDP_EXACT` it takes
   the console's ([rdp_exact.md](rdp_exact.md), rule 7).
9. **The 9th bit of `LOD >> 5`** is dropped before the log2, as the
   console's 8-bit table would: it only matters for a step of 256 texels or
   more, which is at the far level anyway, and then only for the fraction in
   sharpen or detail mode.
10. **A YUV tile with CONVERT_ONE** (`texture_pass`): the second pass still
    checks the tile before the one it samples. With the LOD that may not be
    the first pass's tile; no game is known to mipmap a YUV texture.

## How it was checked

* **The captured dumps**: `rdp_task`, built from the model before the LOD and
  from this one, writes the same PPM, byte for byte, for each of the 65
  frames that the 79 captured task dumps of five games under `build/triage`
  give (16.5 million pixels). None of the dumps draws a 2-cycle primitive
  with TEX_LOD, and no combiner reads LOD_FRACTION. One title's dumps set
  TEX_LOD 32 times each, all in 1-cycle mode, and every triangle there has a
  level count of 0: on the console too the tile would not move.
* **`test_lod`**: the steps above, and a perspective floor in 2-cycle mode
  with a mip chain of six levels (32 x 32 down to 1 x 1, each a checker of its
  own colour), combined as `(TEXEL1 - TEXEL0) * LOD_FRACTION + TEXEL0`. The
  levels follow the distance down the middle of the picture: level 0 from
  the bottom to row 98 (a step under about 1.5 texels), then levels 1 to 5 by
  row 54, blended between; of the floor's 61,220 pixels 40,637 are
  magnified, and by level 47,992, 4,488, 3,200, 2,560, 1,480 and 1,500 (the
  last at the far level). S and T at the three points are the console's: the
  model's own perspective divide, a few subtexels off, gave 47,680, 4,800,
  3,520, 2,240, 1,480 and 1,500, as near a power of two that moves the level.
  Before the LOD the floor was level 0 to the horizon, an aliased moire there.
* **`test_lod` with `RDP_EXACT`** ([rdp_exact.md](rdp_exact.md)): the
  floor's rows 229 to 239 are not drawn (an edge there is past 2048 pixels,
  which the console's 12-bit edges count as crossed: rule 6), 3,520 pixels of
  level 0: 57,700 LOD pixels, by level 44,472, 4,488, 3,200, 2,560, 1,480 and
  1,500, 37,117 magnified; level 1 from row 97, 2 from 81, 3 from 68, 4 from
  60, 5 from 54. The comparison (`sim/rdp_oracle`) finds the console's levels
  and fractions on every one of those pixels.
* **The RTL bench**, before the RTL had the LOD: of 12 random seeds (1 to 8
  and `TB_CC`'s 51 to 54), 5 differed from the model with it (4, 8, 51, 53,
  54): the random streams set TEX_LOD and read LOD_FRACTION, so they cover
  the LOD now that the RTL has it.

## The RTL: a design

### What the pipeline did before

`rdp_top` steps the eight attributes of the pixel, `cur[]`, by `adx[]`
(DaDx) every pixel, and issues pixels left to right with their sample mask
(0 for a pixel of the span with no sample: it goes down the pipeline and is
not drawn). DaDy is not read, except Z's. The tiles are two 8-entry M10K RAMs,
`tl_attr` and `tl_size`, read at `p_tile` and `p_tile + 1` into `ts_*` and
`ts1_*` (the RAMs' output registers), stable for a primitive.

`rdp_pix` does the perspective divide at the front of the pipeline:

| stage | what | resources |
|---|---|---|
| P0 | S, T, W registered; a two-pass pixel is held a second clock (`dup0`) | |
| P1 | W normalised (31-bit priority encoder and shift), the reciprocal table read | `rdp_rcp_rom`, an M10K |
| P2 | `r = P - (D * frac >> 9)`; S, T clamped to 27 bits | an 11 x 9 product (a DSP half) |
| P3 | `S * r`, `T * r` | two 27 x 27 DSP blocks |
| P4 | s10.5, saturated to 16 bits; the first tile fields (shift, origin) by pass | |
| P5 | tile shift and origin, then clamp, mirror and mask | the shift on a DSP |
| P6 to P8 | TMEM addresses, TMEM, TLUT, decode | |
| P9 | the 3-point filter | |
| P10, P11 | the two combiner cycles | |

Each stage that reads tile fields (P4, P5, P6, P8, P8t) takes them from `t_*`
or `t1_*` by its pass bit (`ph<n>`). A two-pass pixel's second pass puts the
same S, T, W through the perspective divide again: in 2-cycle mode with
TEXEL1 the perspective divider does every pixel's work twice.

### What the LOD adds

1. **Two more coordinate pairs a pixel**: the next pixel's and the next
   line's S, T through the perspective divide, as 17-bit values with a range
   flag.
2. **The LOD arithmetic**: four 18-bit differences, their magnitudes, the
   maximum; the level (a priority encoder on 8 bits), the clamp against the
   level count, far and magnified, the fraction, the two tile numbers.
3. **Tile fields by pixel**: before, a primitive's two tiles were fixed; with the
   LOD each pass of each pixel has its own.
4. **LOD_FRACTION into the combiner**: 9 bits with the pixel to P10, and one
   more input on the C selects (colour 13, alpha 0), read as a signed 9-bit
   number like K5.
5. **Latency**: the tile is known only after the perspective divide and the
   LOD logic, and the tile shift is the stage right after the perspective
   divide. Two to five stages go in between P4 and P5, by variant.

Common to every variant:

* **The tile RAMs read by pixel.** Address `tl_attr` and `tl_size` at the
  stage before P4 with the pass's tile (`pass ? tile1 : tile0`), one read a
  clock (the two passes are a clock apart), and carry each field down to the
  stage that uses it. Without the LOD the tiles are `p_tile` and
  `p_tile + 1`, the same as now. This frees the second read copy of each RAM
  and removes the `ph ? t1_* : t_*` selects at P4 to P8t (about 95 two-way
  selects, some 50 ALUTs), against some 80 flip-flops that carry the later
  stages' fields: about even.
* **Delay in RAM.** The stages inserted after P4 lengthen the delay lines of
  what the texture stages do not read (the shade, the depth, x, the line
  buffer set, the coverage, S and T for the second pass, LOD_FRACTION): Quartus
  already maps such lines to RAM shift registers (`altshift_taps`: seven in
  `rdp_pix` then, x, the line buffer set and the depth among them), so they
  cost M10K (two or three) rather than logic.
* **The arithmetic on DSPs where it fits.** The differences `S1 - S0` and
  `T1 - T0` (17-bit operands) on the pre-adders of 18 x 18 DSP halves (`(a -
  b) * 1`), two to a block. The fraction, `((LOD << 3) >> level) & 0xFF`, is
  bits 11:4 of `LOD * 2^(7 - level)`: one 18 x 18 half with a one-hot
  multiplier, instead of an 8-bit window select of 15 bits. What stays logic:
  the magnitudes and the maximum (about 60 ALUTs), the level, far, magnified
  and the tile numbers (about 40).
* **When the LOD pipeline is on.** In 2-cycle mode, with TEX_LOD, or when the
  combiner reads LOD_FRACTION (colour C 13, alpha C 0). It may leave out a
  cycle whose A and B selects are the same: its product is 0 whatever C is,
  so the result does not depend on the fraction. (None of the dumps here reads
  LOD_FRACTION at all.)

### Variant (a): full rate, two more perspective dividers

The next pixel's and the next line's S, T, W at P0 (three adders on 32 bits
for the next pixel, or `cur + adx` from `rdp_top` for `lft` and a subtractor
for its absence; three on 17 bits for the next line), each through its own
copy of P1 to P4. The three results are there in the same clock, so the LOD
logic follows P4 directly: three stages before P5.

| part | ALUTs | DSP blocks | M10K |
|---|---|---|---|
| two perspective divide datapaths (normaliser, interpolation, two products, range check), the reciprocal table shared through the M10K's second port and one copy | 300 to 400 | 5 | 1 |
| the next pixel's and next line's attributes, DaDy loaded from the staging RAM | 150 to 200 | | |
| LOD logic, differences on DSPs | 100 to 130 | 2 | |
| combiner inputs, control | 50 to 80 | | |
| delay lines | | | 2 to 3 |
| **total** | **600 to 800 (400 to 520 ALMs)** | **7** | **3 to 4** |

It keeps every pixel at the rate it has now. But a mipmapped primitive's
combiner reads TEXEL1, so its pixels take two clocks already; the second
perspective divider earns nothing there, and both would only pay for a
1-cycle LOD, which
the model does not have. And 7 more DSP blocks is most of what is left: of the
device's 66, the RDP, the RSP and the CPU use 51 before the scan-out's.

### Variant (b): one perspective divider, a 3-clock pixel in LOD mode

The pixel enters P0 three times (a 2-bit phase in place of `dup0`; `rdp_top`
issues an LOD pixel every third clock): the pixel's S, T, W, then plus
`+-DaDx`, then plus DaDy (a registered delta by phase, so each adder's input
select fits the LUT in front of it). The perspective divider runs as it does;
its P4 keeps 17 bits and a range flag besides the saturated 16. The first two
results are held (70 flip-flops) until the third arrives, then the LOD logic,
with the differences of the next pixel and the next line in turn on one pair of
subtractors. The pixel's two texture passes then go into P5 from the held
S, T of the first phase, with the tiles just found.

| part | ALUTs | DSP blocks | M10K |
|---|---|---|---|
| the three-phase input: delta select and three 32-bit adders, DaDy from the staging RAM | 120 to 190 | | |
| P4's 17-bit range, the held results | 30 | | |
| LOD logic, one pair of subtractors in turn | 100 to 140 (70 to 100 with the DSP pre-adders and fraction) | 0 (2) | |
| combiner inputs | 30 to 50 | | |
| control: the phase, the issue every third clock, the passes' S, T select | 40 to 60 | | |
| delay lines | | | 2 to 3 |
| **total** | **320 to 470 (210 to 300 ALMs)** | **0 to 2** | **2 to 3** |

An LOD pixel takes three clocks, against two for a two-pass pixel today: such
primitives' pixel issue is 1.5 times slower, part of which the memory's own
pace hides on rows that wait for their spans.

### Variant (c), a third option: the neighbour's perspective divide, a 2-clock pixel

The next pixel of the walk is the pixel just before or just after in the
issue order, and its attributes are exactly the pixel's plus DaDx (the model
and `rdp_top` both step `cur` by DaDx): its own first perspective divide is
the LOD's next point. So only the next line needs a perspective divide of its
own, and it fits the second clock a two-pass pixel already has, where the
perspective divider now repeats the first.

* The pixel's second phase puts S, T, W plus DaDy through the perspective
  divide (three 17-bit adders on the top bits; DaDy loaded as in (b)).
* The last few pixels' first results are held: for `lft` the next one's
  arrives two clocks after the pixel's; without `lft` the one before's is
  already there.
* At a span's end (with `lft`) or start (without) the neighbour is outside
  the span: `rdp_top` issues one pixel more there, with a mask of 0, to go
  through the perspective divide and not be drawn: two clocks a row.
* The pipeline never stalls, so the next pixel must follow two clocks after
  the pixel: with `lft`, `rdp_top` issues a pixel only when the next one's
  memory words are there too (its issue test looks one pixel ahead).

| part | ALUTs | DSP blocks | M10K |
|---|---|---|---|
| next line's attributes: three 17-bit adders, DaDy from the staging RAM | 60 to 80 | | |
| P4's 17-bit range, the held results | 30 to 40 | | |
| LOD logic | 100 to 140 (70 to 100 with the DSPs) | 0 (2) | |
| combiner inputs | 30 to 50 | | |
| control: the extra pixel, the look-ahead, the neighbour select | 40 to 60 | | |
| delay lines | | | 2 to 3 |
| **total** | **260 to 370 (170 to 240 ALMs)** | **0 to 2** | **2 to 3** |

Every LOD pixel takes two clocks, as a two-pass pixel does now (a mipmapped
primitive's do), plus two clocks a row.

### Which

With hundreds of ALMs free, not thousands, and DSP blocks counted too, (a) is
out. (b) is the simplest to make match the model: each pixel's LOD depends on
nothing but its own three perspective divides. (c) is the cheapest and the
fastest but couples neighbouring pixels in a pipeline that never stalls.
Either way the LOD logic should take the DSP route for the differences and the
fraction (two blocks, some 30 to 40 ALUTs saved), and the tile RAMs read by
pixel cost about nothing.

Where to check it: the random bench already sets TEX_LOD and reads
LOD_FRACTION (5 of 12 seeds differed from the model with the LOD), so
`rdp_regress.sh`'s seeds are the RTL's test, and `test_lod.c`'s floor, put
through `tb_rdp` as a task, would be a directed one.

## Implementation

The RTL has the LOD as the model has it, with the console's perspective
divide (`rdp_n64_div`: with it the texel coordinates match the console's to
the subtexel) for every pixel, LOD or not. It is variant (c), the neighbour's
perspective divide, with two changes to the design above: a missing neighbour
is a ghost pixel rather than a look-ahead in the issue, and the LOD
primitives' extra clocks come from where two RAMs are read, so that a
primitive without the LOD takes the clocks it took before, to the clock.

### When

`lod_on` (`rdp_top`, registered from the modes): 2-cycle mode, and TEX_LOD
or a combiner C that reads LOD_FRACTION (colour 13, alpha 0, either cycle).
Its pixels take two clocks each, whether they make one texture pass or two.
1-cycle mode, copy mode and fill mode are as before.

### The pixel's three points

* **The pixel**: P0's first load, the pixel's S, T, W, through the
  perspective divide as before.
* **The next line**: P0's second load. `rdp_top` gives S, T, W plus DaDy in
  the clock after the issue, while `pix_valid` is down: a triangle's DaDy
  of S, T and W is read from the staging RAM at S_VSET's steps 6 and 7, which
  were free, and kept to its top 17 bits; a texture rectangle's row step is
  DtDy (DsDx flipped), the exact next row, as the model has it without
  `RDP_EXACT` (the console, and `RDP_EXACT`, keep 17 bits of it as of a
  16.16 DaDy: `dy_s` and `dy_t` in `rdp_top` would drop `t_dxl`'s low 4
  bits).
  The sums are 32-bit, as the model's.
* **The next pixel**: the neighbour's own first perspective divide. `rdp_top`
  issues a primitive's pixels in the walk's order: walking right (`lft`, and
  every rectangle), each pixel's next pixel must go through the perspective
  divide two clocks after it, so the issue two clocks after a pixel is the
  next one, or, when its memory is not in yet or the span has ended, a ghost
  of it (through the perspective divide, mask 0, never drawn; the pixel
  itself follows when its memory is there). Walking left,
  the neighbour is the pixel issued before, and a row starts with a ghost of
  the pixel before the span (the span setup takes `xs - 1`, and the first
  step brings `cur` to the span's first pixel: the same values, bit for bit).

### The perspective divide

The console's (`n64_div`): W's integer half, its leading zeros up to 14
shifted out, a 64-entry point and slope table (logic, 25 LUTs), a 15-bit
reciprocal, the integer half of S (and T) times it, shifted back. The
reciprocal is taken up `(sh + 2) mod 3` places at P2, so that the way back
at P4 is one of six shifts (0 to 15 by 3) instead of fifteen, and the
products stay 18 x 18. The range is the model's: the product's bits from 29
down to the result's top not all one value, or W's integer half zero or
negative. P4 keeps both the texel's 16 bits (saturated) and the LOD's 17 and
its range flag. The pipeline's depth is unchanged (P1 to P4).

### L1 to L3

After P4 (`rdp_pix`, "The LOD"):

| stage | what |
|---|---|
| L1 | each perspective divide's result less the last pass A's (`pa_*`): a pass B (the next line) less its own A is the line's step; a pass A less the A before is a step to the next pixel, the pixel before's walking right, this pixel's own walking left (negated: the ones' complement of `A - prev - 1`, a borrow in). Of each step the larger magnitude of S's and T's, 14 bits, and a far flag (512 texels or more, or a point out of range) |
| L2 | the distance (the larger step), its level (log2 of bits 12:5, chosen by the same compare from each step's own), far, magnified |
| L3 | the level against the level count, the tiles, the fraction (into `lf`) |

The first pass's tile goes from L3 to `rdp_top`'s tile RAMs, which now have
one read port a pass (`tl_ra`): the primitive's tile, the next (TEXEL1's),
or L3's. That freed the second read copy of each, and the `ph ? t1_* : t_*`
selects of P4 to P8t: each stage's fields are carried from the pass's read.

### The latency

A pixel's texture passes cannot start before L3. Its S, T wait in `st_ram`
(16 x 32, M10K) and P4 takes them back D clocks after the perspective divide,
D = 5 walking right (the next pixel's perspective divide is two clocks after
the pixel's, its step a clock later), 4 walking left. What the texture stages do
not read (x, the line buffer set, the shade, the depth, the coverage, the row)
goes from P1 to P8b through `sb_ram` (16 x 71, two M10K, where the stages'
registers and Quartus's shift registers made of them were), read 9 clocks
after its write, or 9 + D with the LOD: the pointers make the delay, and the
only choice between two sources is S, T at P4. The fraction goes from L3 to P9
in `lf_ram` (16 x 9).

What the LOD costs in time: two clocks a pixel (a mipmapped primitive's
pixels took two already for TEXEL1), a ghost a row (two clocks), a ghost for
each pixel whose memory was not in when it was due (walking right), and D
clocks at the primitive's end. A primitive without the LOD takes the clocks
it took.

### 1-cycle mode, later

The console's 1-cycle LOD ([rdp_exact.md](rdp_exact.md), rule 3; the model's
with `RDP_EXACT`) takes the step between the next pixel and the one after it,
and no next line. In this pipeline that is L1's step to the next pixel of
the walk's next pixel:
the pixel's LOD is its neighbour's `mn`, one pixel later (L2 a pixel later,
D two clocks longer), at a pixel a clock (no second load). Walking right the
issue would follow a pixel by its next two (ghosts where they are missing:
x + 1 and x + 2 at the span's end), and the span-end cases that take the next
row's start would need that row's setup done before this row's last pixel:
the part that needs care. TEXEL0's tile from L3 is already a pass's read.

### Resources and timing

The RDP alone (`tools/area2/ooc.sh rdp`, the project's settings, then the
unit fitted with every pin virtual, one seed, a 16 ns clock), before
(round 2) and after, with the two-copy TMEM of the area study's candidate 8:

| | ALUTs | registers | memory ALUTs | DSP | block memory bits | fitted ALMs | RAM blocks | worst setup slack |
|---|---|---|---|---|---|---|---|---|
| before | 7,169 | 6,094 | 64 | 39 | 269,704 | 4,955 | 55 | +1.018 ns |
| after | 7,325 | 6,313 | 64 | 39 | 231,341 | 5,097 | 48 | +1.251 ns |

(+142 fitted ALMs, -7 RAM blocks, no DSP block more; with three TMEM copies
5,092 ALMs, 53 RAM blocks, +0.835 ns: the two copies cost about nothing and
are kept; sim/area_study.md, "LOD", has the breakdown. The console's
perspective divide alone, without the LOD, measured 7,067 ALUTs and 6,013
registers in synthesis: it is smaller than the old one.)

### How it was checked

The RTL against the model (`build_rtl.sh`):

* `tools/area2/rdp_regress.sh`: `rdp_regress: 53 runs, 0 failed`.
* `tb_rdp -r SEED 300`, seeds 100 to 160: all `RTL == model`.
* `TB_LOD=1` (new in `tb_rdp`): random streams made for the LOD (2-cycle
  mostly, DaDy on the scale of DaDx, steps from a sixteenth of a texel to
  hundreds, the LOD blend combiner, the minimum level, YUV tiles): 211 runs
  (seeds 1 to 60 and 100 to 220 plain, 61 to 80 with `TB_SDRAM=1 TB_CPU=30`,
  81 to 90 with `TB_CC=1`), all `RTL == model`. They are what tests the LOD: the plain random streams'
  DaDy is a random word, which puts nearly every pixel at the far level.
  Each of 17 deliberate faults (the borrow, the minimum level, sharpen's sign,
  the detail and sharpen tiles, the CONVERT_ONE tile, a flipped rectangle's
  next line, a rectangle's next line kept to 17 bits, a DaDy fraction bit,
  the first cycle's signed alpha, D, the two ghosts, the far flags, T's
  range, W's range) fails some of these runs (the 17-bit rectangle step one
  run in 150); a fault in the perspective divide's range direction (the
  product's bit 29) fails none: that decomposition of `rdp_n64_div` was
  checked against it in Python instead, on 2 million random S, W.
* The 79 captured task dumps under `build/triage`, each with the fast memory
  and with `TB_SDRAM=1 TB_CPU=30`: all `RTL == model`, and every
  per-command clock log (`TB_CMDT`) byte for byte the RTL's before the LOD,
  and the same against the RTL before the LOD with only its perspective
  divide made the console's.
* The whole SoC (`sim/mips_soc`, `FB_N64=1 VGA=1 -e 60`, a bench built with
  this RTL against `obj_soc` and against one built with the RTL before the LOD
  with only its perspective divide made the console's): a 3D platformer's
  boot, 300M cycles, runs to the end (288 frames, the CPU at 80246ddc as on
  `obj_soc`); its five frame dumps are byte for byte the second bench's, and
  `obj_soc`'s but for frame 240 (the title screen's 3D logo: 1,642 single
  pixels, the perspective divide's subtexels). A second game, 625M cycles
  (600 frames): its ten frame dumps are byte for byte the second bench's (no
  LOD primitive draws in its opening); against `obj_soc` frames 180 to 540
  differ in 3,639 to 8,327 scattered pixels over the textured ground and
  trees, the same picture: the perspective divide again.
