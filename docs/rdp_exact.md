# MIRLO 64: the console's RDP rules (RDP_EXACT)

The RDP's reference model, `lang/c/rdp/rdp.c` (built with `RDP_N64`), was
compared pixel for pixel with an independent, pixel-exact software RDP (the
oracle: `sim/rdp_oracle/`, its README and RESULTS.md). Where the two
disagree, the model follows a rule of its own. This page lists each rule the
console follows there, what the model does without the switch below, and what
the RTL in `rtl/rdp` must change to follow it.

The rules are in the model behind compile switches, so that the RTL lockstep
benches can follow the ones the RTL has and no more:

| what | where |
|---|---|
| the rules, one switch each | `RDP_X_RECTLOD` (rule 2), `RDP_X_COPY` (rule 6's copy mode), `RDP_X_SHADE` (rule 4's pixel side), `RDP_X_SPAN` (rule 4's span start), `RDP_X_MEMDZ` (rule 8; needs `RDP_X_SHADE`). `sim/rdp/build_rtl.sh` builds the model with the ones `rtl/rdp` follows (`XRULES=` to change them); the oracle's `rdpo_x` is that model |
| all the rules | `RDP_EXACT`, with `RDP_N64`: the five above and the rest (the walk's edges and spans, rectangles as primitives, the LOD in 1-cycle mode, TEXEL1 a pixel ahead). Off by default |
| the console's perspective divide | rule 1, the model's whatever the switch, as `rtl/rdp` has it: `rdp_n64_div` in `rdp.c` (declared in `rdp.h`), and `n64_persp` (static) for the texel |
| a frame | `make rdp_task EXACT=1` in `sim/rdp` |
| the comparison | `sim/rdp_oracle`: `make` builds `build/rdpo` (no switch), `build/rdpo_x` (the rules the RTL follows) and `build/rdpo_exact` (`RDP_EXACT`); `./run.sh 50` runs them on every suite, the minimal streams and the captured dumps |
| tests | `test_lod.c` checks `rdp_n64_div`; `test_cvg.c` (two checks take the console's values with the switch) and `test_cimg32.c` pass with and without it |

Without a switch the model is as it was.

## In short

`rtl/rdp` follows rules 1, 2, 5, 4 whole (the pixel side and the span's start) and 8 (the depth image's
own dz), the copy mode of rule 6, and the CVG_X_ALPHA write rule; it does not follow the rest of rule 6 (edges,
spans, rectangles as primitives), rule 3 (the LOD in 1-cycle and copy mode) or rule 7 (TEXEL1 a pixel
ahead). The benches build the model with the rules the RTL follows, so `rdp_regress.sh` passes.

With `RDP_EXACT` the model agrees with the oracle on every suite of `run.sh 50` (26 suites: 528
calibration primitives, 11,131 random ones and 176 in the worked-step and floor streams; 5.48
million pixels drawn with an LOD, each with the oracle's inputs and result) and on all the minimal
streams. On the 138 captured tasks measured for the numbers below (92,498 primitives, 34.8 million
pixels drawn) the model differs from the oracle, by rule:

| the model | primitives that differ | colour words that differ |
|---|---|---|
| no rule (what the RTL was) | 51,889 | 383,878 |
| rules 2 and 6's copy mode (no task draws a rectangle in copy mode that they change) | 51,889 | 383,878 |
| and rule 4's pixel side (the shade, Z, the dz code) | 37,558 | 279,995 |
| and rule 4's span start | 15,745 | 218,324 |
| and rule 8, the depth image's own dz (what the RTL is) | 38 | 153,707 |
| `RDP_EXACT` | 27 | 153,755 |

(The colour words are nearly all 27 primitives drawn with the noise dither, whose random numbers
are each side's own: they differ whatever the rules; the figures are of the RTL's per-pixel noise, `RDP_LFSR_PIXEL`, which the benches build. Its default, a
free-running LFSR that steps every clock ([rdp.md](rdp.md), "Noise"), gives the noise-reading pixels other bits again: they are not reproducible
against the model or the oracle, and stay out of the comparison.) Rule 8 took away 15.7 thousand primitives
and 64.6 thousand colour words (0.19 % of the pixels; 13,060 of them four or more steps of a 5-bit channel off, 33,746 two or three,
171,518 one, before; the noise's included), the depth compare's and the blender's use of the depth image's own dz. What is left
is the noise dither's 27 primitives and 11 fill rectangles of no area (XH = XL = 312, one row), a pixel each that the
RTL draws and the console does not (one colour word each, four or more steps off; rule 6, rectangles as primitives, which `RDP_EXACT` draws as the oracle does).

On the suites of `run.sh 50` (colour words that differ; the model with no rule, with the rules the RTL follows, with all):

| suite | no rule | the RTL's rules | `RDP_EXACT` |
|---|---|---|---|
| cal_shade, cal_aff, cal_persp (cal_z: the depth words of 57 primitives) | 753, 2,899, 7,181 (57) | 0, 0, 0 (0) | 0 |
| lod2, frac2 (the 2-cycle LOD, shade, Z, perspective) | 97,047, 112,872 | 0, 0 | 0 |
| cal_rect | 1,197 | 1,197 | 0 |
| cal_t1, lod2_t1 (TEXEL1 a pixel ahead) | 72,762, 733,470 | 72,497, 731,711 | 0 |
| floor, floor_affine (rows past 2,048 pixels) | 10,856, 3,520 | 10,560, 3,520 | 0 |
| rect (rectangles with the LOD) | 76,359 | 75,975 | 0 |
| lod1 (the LOD in 1-cycle mode) | 849,469 | 842,687 | 0 |
| zdz (the depth image's own dz: 1,300 primitives) | 106,113 | 0 | 0 |

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

**RTL**: done. `rdp_top.v`'s `dy_s` and `dy_t`: a rectangle's row step (DtDy, DsDx flipped) is added to
the row's coordinate with its low 4 bits cleared, which is `LOD_DY_MASK`'s 17 bits of a 16.16 DaDy. The
model's `RDP_X_RECTLOD` is the same; `TB_LOD=1 TB_RECT=1` streams in `rdp_regress.sh`.

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

**RTL** (not done): `rdp_pix.v`'s LOD block (L1 to L3) in 1-cycle mode: the two points
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
keeps serving its depth compare and blender shifts until rule 8.

**RTL**: done, in two parts, each with a model switch.

* The pixel side (`RDP_X_SHADE`; `rdp_pix.v` P0, `rdp_top.v` S_VSET): a pixel without full coverage
  takes its shade and Z at its first covered sample by `(A >> 14) * 4 + ox * DaDx' + oy * DaDy'`
  (shade, steps of 13 bits: `adx`'s bits 26:14 and `sdy`, the DaDy of the four shade
  attributes read in S_VSET with the others), a 9-bit result kept 0..255 / 255 / 0, and Z in 22 bits
  (`z_dx`, `z_dy` >> 10) with the 19-bit wrap; the two products of a 2-bit number and their sum are one DSP
  block a shade channel and two for Z (shifts and adds in logic first, after a version with a block for each product
  that missed timing by up to 0.6 ns: sim/area_study.md, round 9). The depth word's dz code is the
  console's: the place of the top set bit of |DzDx| + |DzDy| of the integer halves, plus 1
  (`dzc_p`); the compare and the blender's shifts use it from rule 8 on.
* The span's start (`RDP_X_SPAN`; `rdp_top.v` W_SUB, S_VSET, S_SETUP): the walker's major edge
  `ax_h` is captured on the subscanline the attributes are taken on (`unscrx`, `xfrac`); `dif_ram` has an
  attribute's 3/4 of DaDe - DaDy (0 when the edge does not lean away from the walk), made as DaDy is
  read; a row's setup computes `((e & ~0x1FF) + dif - xfrac * ((DaDx >> 8) & ~1)) & ~0x3FF` in a
  stage of its own (the product on a DSP block of its own) and the shared multiplier's product of the
  distance from the edge's pixel and DaDx' comes in a clock later: a row's setup takes one clock more
  than it did. `exh`, the major edge's running x, and its adder are gone; `adx` and the DaDx copy in `ade_ram` lose
  their low 5 bits (Z whole). The 2-cycle LOD's neighbour is the pixel's own, so it follows.
  (3/4 of DaDe - DaDy is `de - de >> 2 - dy + dy >> 2` with the wrap of 32 bits, not `(de - dy)`
  shifted: with large DaDe and DaDy they differ.)

Measured with `tools/area2/ooc.sh rdp` (3 seeds, ALMs fitted, RDP alone): the pixel side +131, the
span's start +109 (before the sharing of the copy mode's mask and the single write port of `dif_ram`,
which took it from +113 to the figure in docs/rdp.md "Resources"). Cycles, on 138 captured tasks: the
setup state takes 8.9 to 9.7 million clocks (a row's setup 7.6 to 8.3), the whole 95,115,501 to 95,126,383 (+0.01 %: the
row's step of `e[]` now starts with the setup's last clock).

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

**RTL**: done. `rdp_pix.v` P4 saturates S and T when W is out of range
whether the primitive has texture coefficients or not, and the two-pass test
does not look at `p_tex`; the model does the same without the switch (a
triangle's: a fill rectangle outside fill mode keeps S = T = 0 there until
rule 6 makes rectangles primitives).

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

* (not done) `rdp_top.v`, the AA walker (`aa_a8`, `aa_b8`, `aa_clamp`, `aa_cross`): the
  edges' bit tests and the 14-bit crossing compare in place of 19-bit signed
  compares and clamps, 28-bit X: narrower, about 20 ALUTs fewer; the rows'
  limits and the all-over / all-under flags: about 20 ALUTs; the field mode is done
  (the model's and the RTL's own, with the dither's row: `streams/`
  `scissor_field.rdps`).
* (not done) `rdp_top.v` W_SPAN and S_PIX: the span to `lx` included and, in fill and
  copy mode, every pixel with a full mask (no cut at the scissor's last
  column): about 10 ALUTs. A rectangle in fill or copy mode already reaches
  the scissor's last column (the model's and the RTL's own: `streams/`
  `fill_scissor_edge.rdps`, `copy_scissor_edge.rdps`).
* (not done) `rdp_top.v`, the rectangle path (W_ROW's fixed columns, S_SETUP's
  `rect_setup`, S_FILL, `kind_fillmode`): rectangles go through the AA walker
  as triangles with vertical edges and their S, T as two attributes through
  rule 4's span setup; copy mode's group step (a 2- or 3-bit counter gating
  the `adx` add). The rectangle path's own column and row arithmetic and its
  S, T accumulators go: about even, perhaps 50 ALUTs fewer. Fill mode
  rectangles then leave the fast path straight to memory unless it is kept
  for them with the console's span ends.
* done, with `RDP_X_COPY`: `rdp_pix.v` P5 (P5b): in copy mode (a texture rectangle's) the texel is the group's coordinate plus
  0 to 3 (or 7), clamp off, mask at most 10 bits (`tc_axis` shares the other modes' mask logic); `rdp_top.v`'s `cp_m` counts a pixel's place in its
  group, steps S (T) once a group, from the first pixel drawn (the scissor shifts the texture), and `m` goes down the pipe with S and T.
  `TB_COPY=1` streams (also `TB_C8=1`, with the SDRAM's timing) in `rdp_regress.sh`.
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

**RTL** (not done): `rdp_pix.v`: the texture stages already give every pixel its first
texel; the second combiner cycle of a pixel needs its walk neighbour's. With
`lft` the neighbour is issued after the pixel: the second cycle waits a pixel
(the first cycle's result and the pixel's other operands held, about 80
flip-flops, or a pixel-deep delay line in an M10K); without `lft` it was issued
before (its texel held). The span's end needs one texel more (the point after
the span, in the next row's LOD tile): `rdp_top.v` issues a ghost pixel there,
as it does for the LOD. About 100 to 200 ALUTs and 80 to 120 flip-flops.

### 8. The depth image's own dz

**The console** writes with each depth the pixel's dz code (rule 4: four bits, the place of the top bit of
|DzDx| + |DzDy| of the integer halves, plus 1; for the primitive depth, the OR of the places of the set bits of
SET_PRIM_DEPTH's delta). The code's top two bits are the depth word's two low bits; its low two are the depth
image's two hidden bits, in the hidden plane at the depth image's addresses. A depth compare reads the code back:

* the tolerance is the larger power of 2 of the pixel's dz and the memory's (1 << its code; for a word of exponent
  under 3 the memory's is doubled and no less than 16 >> exponent, and a memory dz of 0x8000 is coplanar), times 8:
  `nearer` is the depth less the tolerance at most the memory's, `farther` the depth plus it at least the memory's,
  for opaque, decal, the blend enable and the interpenetrating coverage (its shift is the tolerance's exponent);
* with the memory's coverage as the blender's B, the two weights shift by the difference of the two codes, up to 4: A by the
  pixel's less the memory's, B by the memory's less the pixel's (without Z_CMP, B by the pixel's own: 4 under 11, else 15 less the code).

**Without the switch** the model keeps the top two bits of the code in the word, none in the plane, and reads none back: its
tolerance is the pixel's step alone (its own units, 16-bit with the two low bits set) and no less than what a word of exponent
0 to 2 has lost, the shifts 0 with Z_CMP, and its own dz code (3 higher) serves the shift without Z_CMP and the interpenetrating coverage's.

**With it** (`RDP_X_MEMDZ`, which needs `RDP_X_SHADE`): `draw_pixel_cvg` reads the memory's code (`rawdz`, with `hid_get` at the depth
address), takes the tolerance's exponent `texp` and the two shifts from it, and writes the pixel's code with its depth (`ZHID_SET`:
`hid_set` of the code's low two bits); the code is rule 4's (`dzst`), and the model's own serves nothing any more.

**RTL**: done.

* `rdp_pix.v`: the depth image's hidden pair is read at P9 from a raw hidden buffer of its own beside the colour's (`hz_raddr`,
  the group arithmetic from a row base `rzb` of the depth image, as `hw8b` from `rwb`) and registered at P10 (`hidz10`). From it and the
  depth word's two low bits (`rawdz10`), at P10: the tolerance's exponent `texp10` (the memory's, doubled and floored by the word's exponent, or
  as it is, against the pixel's `dzhbp`) and the two shifts `bsa10`, `bsb10`; `texp` is decoded and added to the depth at P11a in place of
  the primitive's step and the 7 flip-flops of the word's floor, and is the interpenetrating shift (it was static); the shifts travel with
  the pixel to the blender's two stages (30 flip-flops). The pixel's code is static: `dzenc` (the four bits `dzc_p` of `rdp_top.v`, or the primitive
  depth's), `dzhbp` the place its tolerance goes by. The old `z_tol` path (a priority encoder over the primitive's step, `zt_m` in `rdp_top.v`) is gone.
* `rdp_mem.v`: the read engine asks for the depth span's hidden groups after the colour span's, in the same phase, before the words (`rd_hz`; when the
  colour image has none, for a 32-bit image, the depth span's start the clock after the request is taken; the depth span's first and last group are made from
  the registered base with two small adders); the write engine's depth pass writes each group's hidden word back as the colour pass does, merged
  from the new depth buffer's lanes, now {depth word 16, hidden 2} (36 bits, the same four block RAMs) with the colour pass's merge logic: the
  pass selects its lanes and hidden bits (`h_sel`, `h_nhid`) and its image's base. The raw hidden buffers: `hrawz`, the pixels' copy of the depth image's,
  and the write engine's copy twice as deep (`hraw1`, the depth image's groups in its upper half): three block RAMs more.
* `rdp_top.v`: `rq_hz` (a span reads the depth image's groups when it has Z_CMP or Z_UPD), `rzb`, and `rq_half`: the second half of a 32-bit image's
  span of more than 512 pixels waits for the first half's write when the two share a hidden group (the colour image has no hidden bits there; the
  depth image's has).
* Benches: `TB_ZDZ=1` streams in `rdp_regress.sh` (also with the SDRAM's timing, a width whose rows share groups, and 32-bit images); with the model built
  without the rule every one of them fails. Cost and cycles: below.

## The numbers, rule by rule

(The suites' columns are from `run.sh 50`; the "captured dumps" rows are from the first comparison, on 79 captured tasks,
and are kept for the order of the effects: the figures for the RTL's own rules, on the 138 tasks measured later, are in "In short".
The model without the switch differs from the first columns' numbers on the random suites by the generator's
host: the figures here are of the first host, those of `run.sh` on this one are in its log.)

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
`cycle2_texel1` (120; rule 7), `memdz_weights` (256 colour and 256 depth words), `memdz_tolerance` (388 colour, 12 coverage and 370 depth),
`memdz_floor` (66 colour, 1,060 depth) and `memdz_coplanar` (8 colour, 3 coverage, 10 depth; rule 8). No rule raises the dumps' count.

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

**The random streams**: with `RDP_EXACT` nothing, every suite of `run.sh 50` agrees word for word
(colour, hidden bits, depth) and every LOD the model draws takes the oracle's inputs and result. With
the rules `rtl/rdp` follows (`rdpo_x`): the suites that use the walk's edges and spans, rectangles as
primitives, the 1-cycle LOD and TEXEL1 a pixel ahead still differ (`run.sh`'s `rdpo_x` lines).

**The captured tasks**: the table above. What the RTL still differs in, 38 primitives of 138 tasks, is the noise dither's
(27 primitives: each side has its own random numbers, so they differ whatever the rules) and 11 fill rectangles of no area
(rule 6, rectangles as the console's primitives: below). The depth compare's and the blender's use of the depth image's own
dz, the one thing left of the 15.7 thousand primitives that differed (every one AA_EN, Z_CMP, IM_RD, opaque), is rule 8, done. One
case of it is not followed: with the memory's coverage as B in the first blender cycle of a 2-cycle pixel, the console shifts the weights by the
pixel before's memory dz (and the code of this pixel), here the pixel's own; no captured task blends so, and the
comparison does not reach it (the `zdz` suite is 1-cycle).

**Not followed, not seen on any captured task** (but for the 11 fill rectangles above): rule 3 (the LOD in 1-cycle and copy mode), rule 7
(TEXEL1 a pixel ahead; no task's second combiner cycle reads TEXEL1, no 1-cycle combiner either), the walk's
edges and spans of rule 6 (a row with an edge past 2,047 pixels; the 12-bit wrap of a scissored
span's count of pixels), rectangles as the console's primitives (partial coverage at a fractional
edge: no captured rectangle differs), and a triangle drawn in copy mode with the console's attribute walk. Rules 3, 6 (edges) and 7 have a minimal stream each in `sim/rdp_oracle/streams/`
that the model with `RDP_EXACT` draws as the oracle does and the RTL does not (`lod1_*`, `copy_detail`, `edge_2048`, `cycle2_texel1`).

Not covered by the comparison, so not known to agree: YUV textures, dithering
and the noise (each side has its own generator), 32-bit colour images,
the blender beyond passing the pixel in most streams (the `zdz` suite blends in 1-cycle mode, with weights the model's divider is the console's for), the alpha compare in
2-cycle mode (the oracle's structure takes it from the next pixel's first cycle, and the blender's
shade alpha likewise; no stream turns the compare on in 2-cycle mode), copy mode of a 4-, 8- or 32-bit
texture without the TLUT (the console copies TMEM's raw bits, two 8-bit texels a pixel; the model and
the RTL decode and re-encode; a 16-bit texel and a TLUT entry are copied raw, as the console's:
`streams/copy_raw16.rdps`), and a texture rectangle drawn with PERSP (with the switch it goes through
the perspective divide with W = 0, as on the console; untested).

## The RTL, in all

| rule | where | status | cost, fitted ALMs (RDP alone, 3 seeds) |
|---|---|---|---|
| 1 perspective divide | `rdp_pix.v` P1 to P4 | done | |
| 2 rectangle next line | `rdp_top.v` `dy_s`, `dy_t` | done | 0 |
| 3 1-cycle and copy LOD | `rdp_pix.v` L1 to L3, `rdp_top.v` next row's start | not done | doc: 100 to 160, 100 FFs |
| 4 attributes: the pixel side, dz code | `rdp_pix.v` P0, `rdp_top.v` S_VSET | done | see docs/rdp.md "Resources" |
| 4 attributes: the span's start | `rdp_top.v` W_SUB, S_VSET, S_SETUP | done | |
| 8 the depth image's own dz | `rdp_pix.v` P10 to P11b, `rdp_mem.v` hidden phase and depth pass, `rdp_top.v` `rq_hz`, `rzb` | done | ALMS |
| 5 untextured PERSP | `rdp_pix.v` P4 | done | |
| 6 edges, rows, spans | `rdp_top.v` AA walker, W_SPAN, S_PIX | not done (field mode, the CVG_X_ALPHA write rule, fill and copy rectangles at the scissor's edge are) | about even |
| 6 rectangles as primitives | `rdp_top.v` the rectangle path | not done | about even |
| 6 copy mode | `rdp_pix.v` P5b, `rdp_top.v` `cp_m` | done (texture rectangles) | |
| 7 TEXEL1 a pixel ahead | `rdp_pix.v` combiner input, `rdp_top.v` ghost pixel | not done | doc: 60 to 110, 80 to 120 FFs |

The order that keeps the lockstep benches useful: each rule leaves the switch as the RTL follows it
(`build_rtl.sh`'s `XR`, the oracle Makefile's `XRULES`).
