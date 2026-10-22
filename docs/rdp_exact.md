# MIRLO 64: the console's RDP rules (RDP_EXACT)

The RDP's reference model, `lang/c/rdp/rdp.c` (built with `RDP_N64`), was
compared pixel for pixel with an independent, pixel-exact software RDP (the
oracle: `sim/rdp_oracle/`, its README and RESULTS.md). Where the two
disagree, the model follows a rule of its own. This page lists each rule the
console follows there, what the model does without the switch below, and what
the RTL in `rtl/rdp` must change to follow it.

The rules are in the model behind a compile switch, so that the RTL lockstep
benches keep passing until the RTL follows them:

| what | where |
|---|---|
| the switch | `RDP_EXACT`, with `RDP_N64`. Off by default |
| the console's perspective divide | rule 1, the model's whatever the switch, as `rtl/rdp` has it: `rdp_n64_div` in `rdp.c` (declared in `rdp.h`), and `n64_persp` (static) for the texel |
| a frame | `make rdp_task EXACT=1` in `sim/rdp` |
| the comparison | `sim/rdp_oracle`: `make` builds `build/rdpo_exact` (`RDP_EXACT`), `./run.sh 50` runs it on every suite, the minimal streams and the captured dumps |
| tests | `test_lod.c` checks `rdp_n64_div`; `test_cvg.c` (two checks take the console's values with the switch) and `test_cimg32.c` pass with and without it |

Without the switch the model is unchanged, byte for byte: `rdp.o` (`-O2`,
`RDP_N64`, and with no define) is the same object code with and without the
switch's code.

## In short

With `RDP_EXACT` the model agrees with the oracle on every suite of
`run.sh 50` (25 suites: 528 calibration primitives, 9,825 random ones and 176
in the worked-step and floor streams; 5.48 million pixels drawn with an LOD,
each with the oracle's inputs and result), on all 10 minimal streams, and on
78 of the 79 captured dumps. What is left is 64 colour words in 6
primitives of one captured frame: the depth compare's use of the depth image's
own dz (below, "What is left"). Without the switch the same dumps differ in
10,484 colour words in 1,696 primitives (222,756 in 6,310 before rule 1 was
the model's own).

`rtl/rdp` does the perspective divide as the console does (rule 1), and so
does the model, with or without the switch. The RTL follows none of the other
rules, and the benches build the model without `RDP_EXACT`, so
`rdp_regress.sh` passes.

## The rules

Each rule: what the console does, what the model does without the switch,
where the model does it with the switch, and the RTL change, with an estimate of its logic (ALUTs; a range, as no
RTL has been written for it).

### 1. The perspective divide

**The console** divides the 16-bit integer halves of S (or T, s10.5) and W
(s.15). W's leading zeros among its low 15 bits, up to 14, normalise it; its
next 6 bits pick a point of a 64-entry table and its next 8 step along a
64-entry slope table: a 15-bit reciprocal. S times it, shifted back by the
normalisation, is S / W, of which 17 bits (s11.5) are kept. The product's
bits above those, unless all copies of its sign, put the result out of range
(over, or under when its bit 29 is set), and a W whose integer half is zero or
negative is over whatever S is. The texel takes the 17 bits saturated to 16
(an out-of-range one to the end it went past); the LOD takes the 17 bits and
the range flag (out of range: the far level).

**Before** this rule was the model's own, it divided S >> 4 by all of W with
its own reciprocal (`rdp_rcp`, `rdp_persp`, and a `lod_persp` for the LOD's
points). S and T differed by up to 69 subtexels at a floor's far end.

**Now**, with or without the switch: `rdp_n64_div` and `n64_persp`, for the
pixel's S, T (`draw_pixel_cvg`) and every point the LOD takes (`lod_pixel`,
`x_div`).

**RTL**: `rdp_pix.v` P1 to P4. Done (its P1 to P4 cite `n64_div` and
`n64_persp`: they are `rdp_n64_div` and `n64_persp` in `rdp.c`). A 15-bit normaliser, the two tables in one ROM, a 16 x 15 product:
smaller than the former 27 x 27 products.

### 2. A texture rectangle's next line for the LOD

**The console** draws a texture rectangle as a primitive of its own whose
DaDy is the row step (DtDy, DsDx when flipped). Its LOD's next line is the
pixel's coordinate with its fraction (s10.5.11) plus that step kept to the
top 17 bits (`LOD_DY_MASK`), as a triangle's DaDy is.

**Without the switch** the model takes the exact next row (`texrect_st` at y
+ 1): 9.5 % of the rectangle LOD pixels take another T there.

**With it**: a rectangle is walked as the console's primitive (rule 6), whose
DaDy of the row-stepped coordinate is the row step; `lod_pixel` keeps it to
`LOD_DY_MASK` as for a triangle.

**RTL**: the rectangle path of the LOD's next line (`rdp_top.v` S_SETUP's
`rect_setup`, and the next-line pass of `rdp_pix.v`'s LOD block, L1): the
row-stepped coordinate kept with an 11-bit fraction and the step masked
before the add; with rule 6's rectangles as primitives this is the triangle
path and costs nothing more. Alone: about 20 to 30 ALUTs.

### 3. The LOD in 1-cycle and copy mode

**The console** computes an LOD in 1-cycle mode too, from two points ahead of
the pixel P (d the walk's step of S, T, W): the next pixel N and the one after
F, no next line. Near a span's end it looks at the next row's span start (S,
T, W at the major edge's pixel, before the scissor), by `lodlen`, the span's
pixels after the first plus those the scissor took off at its start:

* next row with a span, pixel q of 0 to `len`: N = P + d, F = P + 2d, but F =
  P - d at the last but one pixel of a span with `lodlen` > 7 and at the last
  of one with `lodlen` = 7; at the last pixel of one with `lodlen` > 7, N = the
  next row's start, F = N + d;
* the pixel after the span (q = `len` + 1; its tile is the last pixel's TEXEL1
  tile, rule 7): `lodlen` > 7: N = start + d, F = start + 2d; 7: N = start, F =
  start + d; 6: N = P + d, F = P - d; else N = P + d, F = P + 2d;
* no next row: N = P + d, F = P + 2d.

The LOD is the larger step of S and T from N to F (ones' complement, sticky
bit as in 2-cycle mode); level, far, magnified and the fraction as
`rdp_lod`. With TEX_LOD, TEXEL0's tile is the tile plus the level (far: the
level count), plus 1 with DETAIL when not magnified (so DETAIL moves the tile
at a level count of 0); TEXEL1 is not moved. The one cycle reads
LOD_FRACTION (and the LOD is computed for it without TEX_LOD).

In copy mode with TEX_LOD the console takes an LOD for each group of 4 pixels
(8 in an 8-bit image) from the group's next step and the one after; the tile
plus the level, plus 1 with DETAIL when not magnified. For a rectangle (level
count 0): the tile, plus 1 with DETAIL when its step is a texel a pixel or
more.

**Without the switch** the model computes no LOD in 1-cycle or copy mode.

**With it**: `lod1_wanted`, `lod1_pts`, `lod1_pixel` (the LOD section of
`rdp.c`), used by `draw_pixel_cvg` for a 1-cycle pixel; the copy-mode LOD
there too. The walk (`draw_triangle_x`) gives each pixel its place in the span
and the next row's start (`prim_t` `xj`, `xlen`, `xlodlen`, `xnvalid`,
`xnrow`).

**RTL**: `rdp_pix.v`'s LOD block (L1 to L3) in 1-cycle mode: the two points
ahead are the next two pixels' own perspective divides in the walk's order, so
it is the neighbour scheme of docs/lod.md's variant (c) two pixels deep (three
perspective divides held), with the span-end cases choosing the next row's
start, which `rdp_top.v` has once it has stepped `e[]` by DaDe for the next
row (three 32-bit registers handed down with the row, and `lodlen`); the
LOD_FRACTION input of the combiner's second cycle. About 150 to 250 ALUTs and
100 flip-flops, no DSP. Copy mode: the tile select on the group's step, about
20 ALUTs.

### 4. Attribute interpolation

**The console** starts each row's span at the major edge's pixel (`unscrx`:
the edge's 12 bits of whole pixels on subscanline 0, or 3 when the edge leans
away from the walk). Each attribute there is the value on the edge, A + k
DaDe with its low 9 bits cleared; when the edge leans away from the walk, plus
three quarters of (DaDe - DaDy), each with its low 9 bits cleared (to
subscanline 3 and back up to the row's top); less the edge's 8-bit x
fraction times `(DaDx >> 8) & ~1` (nothing in copy mode); then its low 10
bits cleared. The walk starts at the span's major end and steps DaDx with its
low 5 bits cleared (Z's whole) a pixel; the pixels the scissor took off at the
start are stepped first, a count of 12 bits. A pixel without full coverage
moves its shade and Z to its first covered sample (the topmost covered
subscanline's leftmost sample: `offx` quarters right, `offy` subscanlines
down), with the steps' top bits (shade: DaDx and DaDy >> 14, 13 bits; Z: >> 10,
22 bits). The shade then keeps 9 bits: 256 to 383 is 255, 384 to 511 (a
negative shade) 0. Z is 22 bits of the attribute >> 10, the depth its 19-bit
result kept to 18 (its top two bits 10: all ones; 11: 0). A primitive without
Z coefficients has Z 0, not the primitive depth (unless Z_SRC_PRIM). The depth
word keeps the console's dz code: the log2 of |DzDx| + |DzDy| of the integer
halves (ones' complement), taken up to the next power of two, in the units of
the integer half.

**Without the switch** the model takes the plane exactly at each pixel's
top-left corner (`e + DaDx * (x - XH)`, a full product), moves Z alone to the
first covered sample and from its own steps, clamps the shade to a byte,
takes a primitive without Z coefficients at the primitive depth, and stores a
dz code 3 higher (in depth units). S and T differ by up to 15 subtexels where
DaDx is large, the shade by an LSB, the depth by a few.

**With it**: `x_row` (the span start), `draw_triangle_x` (the walk), and in
`draw_pixel_cvg` the coverage correction of the shade and Z, the 9-bit shade,
the 19-bit Z and the dz code (`x_dzpix`, `x_dzcomp`). The model's own dz code
keeps serving its depth compare and blender shifts (see "What is left").

**RTL**: `rdp_top.v` S_SETUP: `cur = e + DaDx * (xs - xh)` becomes the
console's start, `((e & ~0x1FF) + diff - xfrac * ((DaDx >> 8) & ~1)) &
~0x3FF`, then `+ (xs - unscrx) * DaDx'` on the shared multiplier (the walk
still issues left to right: the value at `xs` is the start plus its distance
from `unscrx`, with the 12-bit wrap of the scissored count); `diff` needs DaDy
of all eight attributes from the staging RAM (only Z's is read today), two
adders; `adx[]` loses its low 5 bits (free). About 120 to 180 ALUTs, and 8
more clocks of the shared multiplier a row (the x fraction's product besides
the distance's, for each of the eight attributes). `rdp_pix.v` P0: the shade's coverage
correction (four `offx * cd + offy * dy` with 2-bit factors, shift and add) and
its 9-bit wrap in place of `clamp8`, Z's in the 22-bit form (as `zcorr`
today) with the 19-bit wrap: about 120 to 160 ALUTs and 80 flip-flops for the
steps. The dz code: a 16-bit sum and a priority encoder, about 40 ALUTs.

### 5. An untextured triangle with PERSP

**The console**'s S, T, W of a primitive without texture coefficients are 0
and go through the perspective divide all the same: with PERSP, W's integer
half is 0, out of range, so S and T saturate to 0x7FFF and TEXEL0 is the
tile's texel there (its last, clamped), not its first. Its 2-cycle second
texel is the next tile's at the same point, as for any primitive.

**Without the switch** the model takes S = T = 0 (texel 0, 0) and one pass.
A character's collar (drawn with G_TEXTURE off, combiner TEXEL0 * SHADE) reads the
tile's far corner on the console if it is drawn with PERSP.

**With it**: `draw_pixel_cvg` takes a primitive's S, T through the perspective
divide whether it has texture coefficients or not (theirs are 0), and the
second pass too.

**RTL**: `rdp_pix.v` P4: `tx16(dvs, wle3 && p_tex)` becomes `tx16(dvs,
wle3)` (the comment there says it follows the model's 0); the two-pass test
without `p_tex`. A few ALUTs.

### 6. Edges, rows and rectangles

**The console**:

* **edges** are 28-bit X (30-bit slopes), sign-extended; on each subscanline
  an edge in eighths with a sticky bit is under the scissor when its bit 27 is
  set (2048 pixels or more count as left of the scissor) or below the scissor
  without its bit 26, over when its bit 13 of eighths is set or it is at or
  past the scissor's right edge; the crossing test compares bits 27 to 14 with
  bit 27 inverted (12.2 bits). A row is drawn when a subscanline is in and not
  every edge is over, or under, the scissor; with the scissor's field mode
  (SET_SCISSOR bits 25:24) only every other row. The rows run from YH's to
  YL's, both clipped by the scissor (a negative YH: the scissor's; YL at 4096
  quarter rows or more: the scissor's), the last row included when the
  scissor's YL is fractional.
* **spans** run from the major edge's end (`rx`) to the minor edge's (`lx`),
  both included. The sampled modes draw the pixels with a covered sample; fill
  and copy mode write every pixel of the span, the scissor's last column too
  (the fill rectangle at the scissor's right edge).
* **rectangles** (fill and texture) are primitives of their own walked the
  same way: a left-major triangle with vertical edges at XH and XL (their
  quarter pixels kept), YL taken to its row's last subscanline in fill and
  copy mode, level count 0. A texture rectangle's S and T are s10.5 in the
  integer halves, the column-stepped one stepping by DsDx (DtDy flipped) << 11
  a pixel, the row-stepped one by DtDy (DsDx) << 11 a row. So a fractional edge
  gives a partial coverage (bit 0 and the hidden bits), and without AA_EN the
  top-left sample decides, as for a triangle.
* **copy mode** steps its coordinate once a group of 4 pixels (8 in an 8-bit
  image) from the span's start as it is (the scissor's left edge shifts the
  texture rather than cutting it), and the texel is the group's whole
  coordinate (tile shift and origin) plus the pixel's place in the group, each
  mirrored and masked by the tile's mask (at most 10 bits) and never clamped: a
  1-row tile read on its second row reads TMEM's next row.
* **CVG_X_ALPHA without AA_EN**: a pixel is written when its top-left sample
  is covered, whatever the coverage after the alpha (with AA_EN that coverage
  decides): a fill rectangle in each of one game's 16 captured frames, drawn in 1-cycle
  mode with CVG_X_ALPHA, depends on it. The model and the RTL follow this one
  without the switch ([rdp.md](rdp.md), "Write rule").

**Without the switch** the model compares the edges as 32-bit values (an edge
past 2047 draws), cuts every span a column before the scissor's right edge,
draws rectangles by their own formula (a pixel whose top-left sample is
inside, full coverage; copy mode by pixel, clamped like any tile), and has no
field mode.

**With it**: `x_edge`, `x_row` and `draw_triangle_x` (the walk), `draw_rect_x`
(rectangles; `draw_texrect` and `fill_rect` call it), `x_copy_texel` (copy
mode), `sc_field` and `sc_odd` in `rdp_t`.

**RTL**:

* `rdp_top.v`, the AA walker (`aa_a8`, `aa_b8`, `aa_clamp`, `aa_cross`): the
  edges' bit tests and the 14-bit crossing compare in place of 19-bit signed
  compares and clamps, 28-bit X: narrower, about 20 ALUTs fewer; the rows'
  limits and the all-over / all-under flags: about 20 ALUTs; the field mode:
  about 5.
* `rdp_top.v` S_AASPAN and S_PIX: the span to `lx` included and, in fill and
  copy mode, every pixel with a full mask (no cut at the scissor's last
  column): about 10 ALUTs.
* `rdp_top.v`, the rectangle path (S_ROW's fixed columns, S_SETUP's
  `rect_setup`, S_FILL, `kind_fillmode`): rectangles go through the AA walker
  as triangles with vertical edges and their S, T as two attributes through
  rule 4's span setup; copy mode's group step (a 2- or 3-bit counter gating
  the `adx` add). The rectangle path's own column and row arithmetic and its
  S, T accumulators go: about even, perhaps 50 ALUTs fewer. Fill mode
  rectangles then leave the fast path straight to memory unless it is kept
  for them with the console's span ends.
* `rdp_pix.v` P5 (P5b): in copy mode the texel is the group's coordinate plus
  0 to 3 (or 7), clamp off, mask at most 10 bits: about 20 ALUTs.
* The CVG_X_ALPHA write rule: done (`rdp_pix.v` `kill11`).

### 7. TEXEL1 a pixel ahead

**The console**'s second-cycle TEXEL1 in 2-cycle mode is the next pixel's
first texel: at the walk's next point, in the tile of that pixel's own LOD.
At a span's last pixel, when the next row has a span and the span is 3 or
more long (`lodlen`), the tile is the next row's start's (an LOD from that
start and the line after it alone) and the coordinates still the next point's.
In 1-cycle mode TEXEL1 is likewise the next pixel's texel: at the next point
of the walk (at the last pixel of a span with `lodlen` > 7 and a next row, at
that row's start), in the next pixel's 1-cycle LOD tile (rule 3).

**Without the switch** the model takes the tile's own sample (2-cycle) and
TEXEL0 again (1-cycle).

**With it**: `draw_pixel_cvg` samples that texel (`tex_ahead`, and the 1-cycle
TEXEL1 block) when the second cycle reads TEXEL1 (`cc1_uses_texel1`).

**RTL**: `rdp_pix.v`: the texture stages already give every pixel its first
texel; the second combiner cycle of a pixel needs its walk neighbour's. With
`lft` the neighbour is issued after the pixel: the second cycle waits a pixel
(the first cycle's result and the pixel's other operands held, about 80
flip-flops, or a pixel-deep delay line in an M10K); without `lft` it was issued
before (its texel held). The span's end needs one texel more (the point after
the span, in the next row's LOD tile): `rdp_top.v` issues a ghost pixel there,
as it does for the LOD. About 100 to 200 ALUTs and 80 to 120 flip-flops.

## The numbers, rule by rule

The colour words that differ in `./run.sh 50`, and for `lod2` and `rect` the
LOD pixels (of those the model drew with an LOD) that take the oracle's inputs
and result. "Without": the model without the switch. Columns 1 to 7: the model
with that rule and the ones before it (rules 1 and 2 are the two fixes of
sim/rdp_oracle/RESULTS.md's disagreements 2 and 1). "With": the model with the switch. Columns "without" to 7
were counted with the model's former CVG_X_ALPHA rule (rule 6's last item, now
the model's own: without the switch the dumps differ in 222,756 colour words
in 6,310 primitives), and up to column 3 with each tile left as the streams
set it, which `rdpo` now zeroes on both sides before a stream (the floor
without the switch: 91,973 colour words). The model without the switch now
has rule 1: `run.sh 50`'s `rdpo` lines are column 1, the captured dumps
10,484 colour words.

| suite | without | 1 | 2 | 3 | 4 | 5 | 6 | 7 | with |
|---|---|---|---|---|---|---|---|---|---|
| cal_shade | 647 | 647 | 647 | 647 | 0 | 0 | 0 | 0 | 0 |
| cal_z (depth words) | 56 primitives | 56 | 56 | 56 | 0 | 0 | 0 | 0 | 0 |
| cal_aff | 1,853 | 1,853 | 1,853 | 1,853 | 0 | 0 | 0 | 0 | 0 |
| cal_persp | 23,717 | 6,935 | 6,935 | 6,935 | 0 | 0 | 0 | 0 | 0 |
| cal_rect | 1,329 | 1,329 | 1,329 | 1,329 | 1,329 | 1,329 | 0 | 0 | 0 |
| cal_t1 | 64,864 | 64,864 | 64,864 | 64,864 | 65,134 | 65,134 | 65,134 | 0 | 0 |
| floor | 94,294 | 13,746 | 13,746 | 13,746 | 13,452 | 13,452 | 0 | 0 | 0 |
| floor_affine | 3,520 | 3,520 | 3,520 | 3,520 | 3,520 | 3,520 | 0 | 0 | 0 |
| lod2 | 183,103 | 130,296 | 130,296 | 130,296 | 25,566 | 0 | 0 | 0 | 0 |
| lod2 LOD pixels agreeing | 303,303 | 392,094 | 392,094 | 392,094 | 1,106,184 | 1,107,813 | 1,107,813 | 1,107,813 | 1,107,813 of 1,107,813 |
| lod2_exact | 43,877 | 43,877 | 43,877 | 43,877 | 5,016 | 0 | 0 | 0 | 0 |
| frac2 | 209,196 | 151,988 | 151,988 | 151,988 | 31,230 | 0 | 0 | 0 | 0 |
| frac2_exact | 38,570 | 38,570 | 38,570 | 38,570 | 5,016 | 0 | 0 | 0 | 0 |
| rect | 137,382 | 137,382 | 137,125 | 137,125 | 137,125 | 137,125 | 0 | 0 | 0 |
| rect LOD pixels agreeing | 157,327 | 157,327 | 173,939 | 173,939 | 173,939 | 173,939 | 173,939 | 173,939 | 173,939 of 173,939 |
| lod1 | 859,459 | 839,013 | 839,013 | 156,443 | 0 | 0 | 0 | 0 | 0 |
| lod1_control | 196,540 | 114,642 | 114,642 | 114,642 | 0 | 0 | 0 | 0 | 0 |
| lod1_level0_detail | 688,421 | 652,008 | 652,008 | 106,356 | 0 | 0 | 0 | 0 | 0 |
| lod2_t1 | 811,218 | | | | | | | 0 | 0 |
| lod1_t1 | 844,124 | | | | | | | 0 | 0 |
| captured dumps: colour words | 222,778 | 10,506 | 10,506 | 10,506 | 4,027 | 4,027 | 86 | 86 | 64 |
| captured dumps: primitives differing | 6,323 | 1,709 | 1,709 | 1,709 | 38 | 38 | 19 | 19 | 6 |

The minimal streams of `sim/rdp_oracle/streams/` go to 0 at their rule:
`persp_divide` (16,705 colour words without the switch; 3 with rule 1, the
attribute rounding, 0 with rule 4), `texrect_next_line` (224; rule 2),
`lod1_levels`, `lod1_detail_level0`, `lod1_fraction` (160 each) and
`copy_detail` (4; rule 3), `untextured_persp` (800; rule 5), `edge_2048`
(3,100 colour and 3,100 coverage words) and `fill_scissor_edge` (199; rule 6),
`cycle2_texel1` (120; rule 7). No rule raises the dumps' count.

Notes:

* Rule 4 includes the depth word's dz code (cal_z's depth words), and the
  2-cycle LOD's next pixel stepped by DaDx without its low 5 bits (lod2's last
  1,629 LOD pixels with other inputs: 25,566 to 25,407 colour words, then rule
  5).
* In rule 4's column cal_t1 rises by 270 words: the second-cycle TEXEL1 of the
  pixels whose attributes move, all of it gone with rule 7.
* `rdpo` sets the eight tiles to zero on both sides before each stream: the
  oracle keeps flags it derives at SET_TILE, so a tile never set would sample
  unclamped there, which no console state matches.
* 2-cycle LOD pixels with the switch: 173,100 on `floor` against 183,660
  without, as the rows past 2048 pixels are not drawn (rule 6).

## What is left

**The random streams**: nothing. Every suite of `run.sh 50` agrees with the
switch, word for word (colour, hidden bits, depth), and every LOD the model
draws takes the oracle's inputs and result.

**The captured dumps**: one stream, captured task 002 of one game, 6 primitives, 64
colour words, no coverage or depth words. They are 1-cycle,
anti-aliased, depth-compared triangles (AA_EN, Z_CMP, Z_UPD, IM_RD, opaque).
The console's depth compare reads the depth image's own dz: its code is the
depth word's two low bits and the word's two hidden bits (which the console
writes with the depth); the tolerance is the larger power of two of the
pixel's dz and that memory dz (the memory's doubled and no less than 16 >>
exponent for exponents under 3, coplanar when it is 0x8000), times 8; and when
the blender's B is the memory coverage, its two weights are shifted by the
difference of the two dz codes (up to 4). The model keeps no memory dz (its
own documented simplification, `rdp.h`): its tolerance is the pixel's step
alone and the shifts 0 with Z_CMP. On those pixels the two disagree on the
blend. The model does not follow it under the switch either: following it, the RDP would write the depth image's hidden bits and read them back
(`rtl/rdp/rdp_mem.v` and the hidden plane), a change of the memory traffic
as much as of the logic.

Not covered by the comparison, so not known to agree: YUV textures, dithering
and the noise (each side has its own generator), 32-bit colour images,
the blender beyond passing the pixel in most streams, the alpha compare in
2-cycle mode (the oracle's structure takes it from the next pixel's first
cycle, and the blender's shade alpha likewise; no stream turns the compare on
in 2-cycle mode), copy mode of a texture other than RGBA 16 (the console
copies the texel's raw bits; the model decodes and re-encodes, the same for
RGBA 16 and an RGBA TLUT, not for IA 16, an IA TLUT, or 4- and 8-bit texels),
and a texture rectangle drawn with PERSP (with the switch it goes through the
perspective divide with W = 0, as on the console; untested).

## The RTL, in all

| rule | where | about |
|---|---|---|
| 1 perspective divide | `rdp_pix.v` P1 to P4 | done |
| 2 rectangle next line | `rdp_top.v` `rect_setup`, `rdp_pix.v` L1 | 0 with rule 6, else 20 to 30 ALUTs |
| 3 1-cycle and copy LOD | `rdp_pix.v` L1 to L3, `rdp_top.v` next row's start | 170 to 270 ALUTs, 100 FFs |
| 4 attributes | `rdp_top.v` S_SETUP, `rdp_pix.v` P0, the dz code | 280 to 380 ALUTs, 80 FFs, 8 more multiplier clocks a row |
| 5 untextured PERSP | `rdp_pix.v` P4 | a few |
| 6 edges, rows, rectangles, copy | `rdp_top.v` AA walker, S_AASPAN, S_PIX, the rectangle path; `rdp_pix.v` P5b (the CVG_X_ALPHA write rule: done) | about even |
| 7 TEXEL1 a pixel ahead | `rdp_pix.v` combiner input, `rdp_top.v` ghost pixel | 100 to 200 ALUTs, 80 to 120 FFs |
| **total** | | **about 550 to 900 ALUTs (360 to 600 ALMs), 260 to 300 FFs, no DSP block** |

The order that keeps the lockstep benches useful: rule 1 is in; rules 4 and 6
together (the walk: the rectangles become its primitives, and the span start
is where rule 4 acts), then 5, 2 (free by then), 7 and 3. A model built with
only some rules on does not exist yet: rule 1 left the switch when the RTL
followed it, and the others can leave it the same way, one at a time.
