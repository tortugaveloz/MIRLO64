# The model's LOD against the oracle: results

The model: `lang/c/rdp/rdp.c` without `RDP_EXACT`, built with `RDP_N64`,
as it was when these were measured: with its own perspective divide, which
the console's (disagreement 2 below) has since replaced. `run.sh`'s `rdpo`
now has the console's, and its figures are column 1 of docs/rdp_exact.md's
table. The oracle and how both are driven: [README.md](README.md). Every number below is from `./run.sh 50`; the
same streams give the same numbers built with gcc in the tools image.

Built with `RDP_EXACT` (`rdpo_exact`) the model has every change below and
the pre-existing ones listed at the end, and agrees with the oracle on every
suite (0 colour, coverage and depth words differ; every LOD pixel takes the
oracle's inputs and result), on every stream of `streams/`, and on 78 of the
79 captured dumps (64 colour words in 6 primitives of one captured frame):
[docs/rdp_exact.md](../../docs/rdp_exact.md), each rule, its numbers and what
the RTL must change.

## In short

* **The LOD logic agrees everywhere.** `rdp_lod` (the distance, its ones'
  complement, the 15-bit LOD and its sticky bit, level, far, magnified, the
  fraction with detail, sharpen and the minimum level, the tiles with and
  without TEX_LOD) gives the oracle's result for each of the 4.77 million
  2-cycle LODs the oracle computed in these streams, from the oracle's own
  inputs; and of 4.35 million pixels the model drew with an LOD, none took the
  same inputs as the oracle and came out otherwise. test_lod.c's worked steps,
  as command streams, agree pixel for pixel, image and LOD.
* **Disagreements**, each with a minimal stream in `streams/`:
  1. A texture rectangle's next line (choice 6): the oracle steps it by DtDy
     kept to the same 17 bits as a triangle's DaDy; the model takes the exact
     next row. Fix: `RDP_EXACT` rule 2 (one block in `draw_texrect`);
     with it every texture rectangle LOD agrees (173,939 pixels).
  2. The perspective divide at the three points (choice 4): the oracle divides
     the 16-bit integer halves of S, T and W with the console's reciprocal
     table; the model divides more bits with its own reciprocal. S and T
     differ by a few subtexels and with them the fraction, and near a power of
     two the level. Fix: the console's perspective divide, for the pixel and
     the three points alike (now the model's own, `RDP_EXACT` rule 1); with
     it test_lod's floor goes from 21 to 169,020 of 183,660 LOD pixels agreeing. It also
     removes most of the pre-existing perspective texel differences: on the 79
     captured task dumps the differing colour words go from 222,778 to 10,506.
  3. 1-cycle mode (choice 7), and copy mode: the oracle computes an LOD in
     both (from the next pixel and the one after it), and with TEX_LOD moves
     TEXEL0's tile; the model computes none. At a level count of 0 without
     DETAIL it changes nothing (random streams; and one title's dumps, which set
     TEX_LOD in 1-cycle mode at level count 0, are identical), but with DETAIL
     the tile moves a tile on even at level count 0, and a 1-cycle combiner
     that reads LOD_FRACTION gets a value. The rule is below; `RDP_EXACT`
     rule 3.
  4. The second cycle's TEXEL1 (choice 8, known): the oracle's is the next
     pixel's first texel. `RDP_EXACT` rule 7.
* **Pre-existing, not the LOD** (briefly, at the end): the perspective divide
  (above), the attribute (S, T, W, shade, Z) interpolation's rounding, an
  untextured triangle with PERSP, edges past 2048 pixels, a fill rectangle at
  the scissor's right edge, texture rectangles' partial-pixel coverage, copy
  mode's clamp. Flat and anti-aliased triangles (edges, coverage, hidden bits)
  are identical.

## Each choice of docs/lod.md

| # | choice | the oracle | agrees |
|---|---|---|---|
| 1 | `LOD_DY_MASK`: DaDy of S, T, W kept to its top 17 bits | the same: `dsdy & ~0x7fff` (and `dtdy`, `dwdy`), added to the pixel's 32-bit S, T, W, then `>> 16` | yes (the `units` steps with a DtDy fraction of 0x7C00 and 0x8000; the model built with all of DaDy differs there) |
| 2 | `LOD_ONES_ABS`: a negative step counts its ones' complement | the same: an 18-bit difference of the two 17-bit values, `~d & 0x1ffff` when negative; a step of -1 texel is magnified | yes (units "-1 texel", "-2.5", "-1 texel line"; the model built with `\|d\|` fails 11,296 of the units' LODs) |
| 3 | `LOD_FRAC_USED`: the fraction is computed whenever a combiner reads it, TEX_LOD or not | in 2-cycle mode the same: LOD when TEX_LOD, or either cycle's RGB C is LOD_FRACTION (13) or its alpha C is (0, LOD_FRACTION). In 1-cycle mode also when the one cycle reads it (item 3 below) | yes in 2-cycle (`frac2`: 1.95 million pixels, no logic difference); 1-cycle: see choice 7 |
| 4 | the perspective divide: the model's own reciprocal, the 17-bit range check, W's integer half <= 0 out of range | the console's perspective divide on the 16-bit integer halves of S, T, W (W normalised by its leading zeros, up to 14; a 64-entry point and slope table, a 15-bit reciprocal; the product shifted back). Out of range when the product's bits above the 17 kept are not all its sign, or W's integer half is <= 0 | **no**: the range rule agrees (6 of 4.35 million pixels differed in the range flag, each because the values differed at the boundary: 4 from the perspective divide, 2 from W's interpolation, its integer half 0 on one side and 1 on the other), the values do not. Disagreement 2 |
| 5 | the walk: the next pixel is x + 1 with `lft`, x - 1 without | the same (`flip`: DaDx toward the walk, negated without `lft`) | yes (units, right-major, ±1 texel; the model built with the direction swapped fails 38 of the units' 164 primitives) |
| 6 | texture rectangles: level count 0, walked left to right, the next pixel and line from the rectangle's own formula, no perspective divide | level count 0 and left to right, no perspective divide: the same. The next line: the oracle's rectangle is a primitive whose DaDy is DtDy (DsDx when flipped) and is kept to the 17 bits as a triangle's: `(T32 + ((DtDy << 11) & ~0x7FFF)) >> 16`, T32 the row's T with its fraction | **no** for the next line (9.5 % of rectangle LOD pixels took another T there, a quarter of them another result). Disagreement 1 |
| 7 | 1-cycle mode: unchanged | computes an LOD per pixel in 1-cycle mode (and copy mode, with TEX_LOD), moves TEXEL0's tile with TEX_LOD, and gives a 1-cycle combiner its fraction | **no** (as the choice says), and the reason given ("a level count of 0, where the tile does not move") holds only without DETAIL. Disagreement 3 |
| 8 | the second cycle's TEXEL0 and TEXEL1 | TEXEL0: the first cycle's TEXEL1 (the same); TEXEL1: the next pixel's first texel | no for TEXEL1 (known; `cal_t1`, `streams/cycle2_texel1.rdps`) |
| 9 | the 9th bit of `LOD >> 5` dropped before the log2 | the same (`log2table[(lod >> 5) & 0xff]`) | yes (units "300 texels", detail, level count 7) |
| 10 | a YUV tile with CONVERT_ONE | not tested | |

The rest of "When it applies" and "The algorithm" agrees too: the level count
is the triangle's bits 21:19, the minimum level SET_PRIM_COLOR's bits 12:8; the
magnified case (plain: fraction 0, or 0xFF at level count 0; sharpen and detail:
`max(LOD, min level) << 3`, sharpen `| 0x100`); the far case (out of range or
bit 14: the last level, 0xFF); the tiles in plain, sharpen and detail mode; the
combiner reads the fraction as C sign-extended from 9 bits (sharpen's negative
fraction included: the `units` stream with `(ENV - PRIM) * LOD_FRACTION + PRIM`
is identical); a triangle without texture coefficients has S = T = W =
0 at the three points (perspective: out of range, the far level).

## The disagreements

### 1. A texture rectangle's next line

`streams/texrect_next_line.rdps`: 2-cycle, TEX_LOD and DETAIL, the fraction
as the colour, a rectangle with DsDx 1.0 and DtDy 0x044F (1.077 texels a line).

```
cmd 2f150cf0 00000000
cmd 3c66e1ff 8ffffe38
cmd 240a0060 00020020 00000000 0400044f
```

At (8,10) the oracle's T is 68 at the pixel and 102 on the next line (a step
of 34: DtDy & ~0xF), fraction 0x10; the model's next line is 103 (35),
fraction 0x18. 224 of the 512 pixels differ (every row where the exact step
is 35).

**Fix** (`draw_texrect`'s LOD block, docs/rdp_exact.md rule 2): the next
line is the row's coordinate stepped by the rectangle's DaDy kept to
`LOD_DY_MASK`, as a triangle's. With it: `texrect_next_line` identical, and
all 173,939 LOD pixels of the `rect` suite agree (16,612 took another next line before).
docs/lod.md choice 6 would say that the next line is the rectangle's row step
kept to `LOD_DY_MASK`, as a triangle's DaDy.

### 2. The perspective divide

`streams/persp_divide.rdps`: the near half of test_lod.c's floor, one
triangle, 2-cycle, PERSP, TEX_LOD, six levels, the fraction as the colour.
16,705 colour words differ; at the floor's far end S and T at the pixel differ
by up to 69 subtexels (`-v` lists them), most by 1 to 15. The LOD logic agrees
on every pixel: only its inputs differ.

**Fix**: the console's perspective divide (`rdp_n64_div` in `rdp.c`, the
model's own since; docs/rdp_exact.md rule 1), for the texel and the LOD's
three points. With it: test_lod's floor 169,020 of 183,660 LOD pixels agree (21 before; what is left are the
attribute interpolation's few subtexels, pre-existing), `persp_divide` goes
from 16,705 differing colour words to 3, `cal_persp` from 23,717 to 6,935, and
the 79 captured task dumps from 222,778 to 10,506. test_lod.c still passes
(`all ok`), its level counts become 47,992, 4,488, 3,200, 2,560, 1,480, 1,500
(docs/lod.md quotes 47,680, 4,800, 3,520, 2,240, 1,480, 1,500). The RTL's
perspective divider (`rdp_pix` P1 to P3) would become the console's: a 15-bit
normaliser, the two 64-entry tables, a 16 x 15 product, which is smaller than
today's 27 x 27 products.

### 3. 1-cycle and copy mode

`streams/lod1_levels.rdps` (1-cycle, TEX_LOD, level count 5, an affine
triangle 2.5 texels a pixel, TEXEL0; tile 0 red, tile 1 green): the oracle
draws green (level 1), the model red. `streams/lod1_detail_level0.rdps`: the
same at level count 0 with DETAIL: the oracle draws tile 1 (level 0 plus the
detail tile), the model tile 0. `streams/lod1_fraction.rdps`: a 1-cycle
combiner reading LOD_FRACTION: the oracle's fraction 0x40, the model's 0.
`streams/copy_detail.rdps`: a copy-mode texture rectangle with TEX_LOD and
DETAIL samples tile 1 on the oracle (without DETAIL both sample tile 0).

The random `lod1` suite (TEX_LOD, levels 1 to 7) differs in 859,459 colour
words against 196,540 for the same streams without TEX_LOD (`lod1_control`,
the pre-existing coordinate differences); at level count 0 without DETAIL
(`lod1_level0`) exactly the control's 196,540, so that title's case is as the doc
says; with DETAIL (`lod1_level0_detail`) 688,421.

What the oracle does in 1-cycle mode, per pixel `j` of a span (walked from
the major edge, `j` = 0 to `length`, `length` the span's pixels less one;
`lodlength` = `length` plus the pixels the scissor took off the span's start;
`d` the walk's step of S, T, W; `P` the pixel's S, T, W):

* if the next row has a span: unless `j == length && lodlength > 7`,
  `N = P + d` and `F = P + 2d`, except `F = P - d` when `(j == length - 1 &&
  lodlength > 7) || (j == length && lodlength == 7)`; when `j == length &&
  lodlength > 7`, `N` is the next row's span start and `F = N + d`;
* without a next row: `N = P + d`, `F = P + 2d`;
* both through the perspective divide (out of range: the far level); the LOD
  is the larger of `|dS|`, `|dT|` between `N` and `F` alone (no next-line
  point, ones' complement and sticky bit as in 2-cycle mode);
* level, far, magnified and the fraction as `rdp_lod`; with TEX_LOD TEXEL0's
  tile is the tile plus the level (far: the level count), plus 1 with DETAIL
  when not magnified; TEXEL1 is not moved. A combiner that reads
  LOD_FRACTION gets the fraction (and the LOD is computed for it without
  TEX_LOD).

(When the 1-cycle combiner reads TEXEL1, the oracle computes the same a pixel
ahead, with its own span-end cases.) In copy mode with TEX_LOD: `N = P + d`,
`F = P + 2d` always, magnified: level 0, else the level (far: the level count);
the tile plus the level, plus 1 with DETAIL when not magnified. For a copy-mode
texture rectangle (level count 0) that is: the tile plus 1 when DETAIL is set
and its step a pixel is a texel or more.

The model with `RDP_EXACT` follows both rules (`lod1_pixel`, and the
copy-mode LOD in `draw_pixel_cvg`): it needs the span's position and the next
row's start, which its walk (`draw_triangle_x`) gives each pixel. Without the
switch docs/lod.md choice 7 stands, with its note that DETAIL at level count 0
moves the tile on the console and that a 1-cycle combiner's LOD_FRACTION is
not 0 there.

### 4. The second cycle's TEXEL1

`streams/cycle2_texel1.rdps`: the oracle's second-cycle TEXEL1 is the next
pixel's first texel (and at a span's end the next row's first); the model's
is the pixel's own. 120 of 160 pixels differ; `cal_t1` 64,864 colour words.
Known (choice 8). The `lod2_t1` and `lod1_t1` suites read TEXEL1 with the
LOD: 811,218 and 844,124 colour words without the switch, 0 with it.

## The numbers (`./run.sh 50`)

LOD pixels: drawn by the model with an LOD; "agree": the same inputs and
result; "other inputs": S, T or the range flag at one of the three points
differed (and how many still gave the same result); "logic": the same inputs,
another result.

Models: "without", the model without `RDP_EXACT` as it was then; "fixes",
the same with the fixes of disagreements 1 and 2 (their figures, taken
before rdpo set the eight tiles to zero, which moved only the floor's count without them, from 94,294); "exact", the model with
`RDP_EXACT` (`rdpo_exact`).

| suite | model | primitives differ | colour words | LOD unit (oracle inputs) | LOD pixels | agree | other inputs (same result) | logic |
|---|---|---|---|---|---|---|---|---|
| units (4 combiners) | all | 0 of 164 | 0 | 28,160, 0 differ | 25,600 | 25,600 | 0 | 0 |
| floor (plain, sharpen, detail) | without | 6 of 9 | 91,973 | 175,512, 0 | 183,660 | 21 | 173,079 (59,386) | 0 |
| | fixes | 6 of 9 | 13,746 | 175,512, 0 | 183,660 | 169,020 | 4,080 (3,978) | 0 |
| | exact | 0 of 9 | 0 | 175,512, 0 | 173,100 | 173,100 | 0 | 0 |
| floor, affine | without, fixes | 1 of 3 | 3,520 (rows past 2048 px) | 58,504, 0 | 61,220 | 56,921 | 779 (779) | 0 |
| | exact | 0 of 3 | 0 | 58,504, 0 | 57,700 | 57,700 | 0 | 0 |
| lod2 | without | 488 of 900 | 183,103 | 1,214,384, 0 | 1,107,813 | 303,303 | 804,510 (522,367) | 0 |
| | fixes | 477 of 900 | 130,296 | 1,214,384, 0 | 1,107,813 | 392,094 | 715,719 (490,729) | 0 |
| | exact | 0 of 900 | 0 | 1,214,384, 0 | 1,107,813 | 1,107,813 | 0 | 0 |
| lod2, exact affine | without, fixes | 183 of 863 | 43,877 | 942,402, 0 | 846,770 | 608,521 | 238,249 (188,968) | 0 |
| | exact | 0 of 863 | 0 | 942,402, 0 | 846,770 | 846,770 | 0 | 0 |
| frac2 | without | 504 of 900 | 209,196 | 1,214,384, 0 | 1,107,813 | 303,303 | 804,510 (522,416) | 0 |
| | fixes | 496 of 900 | 151,988 | 1,214,384, 0 | 1,107,813 | 392,094 | 715,719 (490,729) | 0 |
| | exact | 0 of 900 | 0 | 1,214,384, 0 | 1,107,813 | 1,107,813 | 0 | 0 |
| frac2, exact affine | without, fixes | 184 of 863 | 38,570 | 942,402, 0 | 846,770 | 608,521 | 238,249 (188,968) | 0 |
| | exact | 0 of 863 | 0 | 942,402, 0 | 846,770 | 846,770 | 0 | 0 |
| rect | without | 773 of 900 | 137,382 | 197,913, 0 | 173,939 | 157,327 | 16,612 (12,615) | 0 |
| | fixes | 772 of 900 | 137,125 | 197,913, 0 | 173,939 | 173,939 | 0 | 0 |
| | exact | 0 of 900 | 0 | 197,913, 0 | 173,939 | 173,939 | 0 | 0 |
| lod2_t1 | without | 684 of 900 | 811,218 | 1,210,580, 0 | 1,137,138 | 290,350 | 846,788 (524,061) | 0 |
| | exact | 0 of 900 | 0 | 1,210,580, 0 | 1,137,138 | 1,137,138 | 0 | 0 |
| lod1 / control / level 0 / level 0 + detail | without | 726 / 647 / 647 / 716 of 900 (899 for lod1) | 859,459 / 196,540 / 196,540 / 688,421 | (1-cycle: no hook) | | | | |
| | exact | 0 | 0 | | | | | |
| lod1_t1 | without | 757 of 900 | 844,124 | (1-cycle: no hook) | | | | |
| | exact | 0 of 900 | 0 | | | | | |

What still differs with the two fixes is not the LOD: in `lod2` and `frac2`
the attribute interpolation's few subtexels at the pixel (the "other inputs"
move S and T at all three points alike) and untextured triangles with PERSP;
in `rect` the 1-cycle rectangles (disagreement 3) and rectangle edge coverage.
`RDP_EXACT` has those rules too (below, and docs/rdp_exact.md); with the
switch the floor draws fewer LOD pixels, as its rows past 2048 pixels are not
drawn.

The comparison is sensitive to each choice: the model built with all of DaDy
(`LOD_DY_MASK ~0`), with `|d|` (`LOD_ONES_ABS 0`), with the walk's direction
swapped, or with `LOD_FRAC_USED 0`, differs from the oracle in the `units`
stream (2, 19, 38 and 8 primitives; `run.sh`'s "sensitivity" lines).

## Pre-existing differences (not the LOD)

The calibration suites, one feature at a time (3 streams of 20 primitives,
`rdpo`): flat triangles and anti-aliased ones are identical (edges, coverage,
hidden bits); shade 647 colour words, depth 56 of 66 primitives, affine
texturing 1,853, perspective 23,717, texture rectangles 1,329, second-cycle
TEXEL1 64,864. Of the 79 captured task dumps under `build/triage` (as streams
through `task2rdps.sh`; none draws with an LOD), 44 that draw are identical
(15 of one game, 318,965 pixels each; 28 of a second; 1 of a third), 17 differ
(16 of a fourth game, and 1 of the third: 222,756 colour words in 6,310
primitives; 222,778 with the model's former CVG_X_ALPHA rule, which
disagreement 2's figures start from) and 18 draw nothing into the image
compared (two more games'). With `RDP_EXACT`, 78
agree; what is left is in docs/rdp_exact.md, "What is left".

* **The perspective divide** (disagreement 2): the console divides S, T and W's
  16-bit integer halves; the model `S >> 4` by all of W with its own table.
* **Attribute interpolation**: the console starts a span from the major edge's
  value `((A & ~0x1FF) + Adiff - xfrac * ((DaDx >> 8) & ~1)) & ~0x3FF`, xfrac
  the edge's 8-bit x fraction and Adiff three quarters of `DaDe - DaDy` (their
  low 9 bits cleared) when DxHDy's sign matches the walk, and steps a pixel by
  DaDx with its low 5 bits cleared (Z whole); the model takes the plane exactly
  at the pixel's top-left corner. S and T differ by 1 to 15 subtexels where
  DaDx is large (`cal_aff`, and the LOD's "other inputs" in `lod2_exact`),
  shade by an LSB (`cal_shade`), depth by a few (`cal_z`, a captured frame).
* **An untextured triangle with PERSP** (`streams/untextured_persp.rdps`): the
  console's perspective divide sees W = 0 and saturates S and T to 0x7FFF, so
  TEXEL0 is the tile's texel there (its last, clamped); the model's S = T = 0
  (texel 0, 0). The model's note on a character's collar (TEXEL0 read with G_TEXTURE
  off) is this case if that triangle is drawn with PERSP (the usual mode): the
  console then reads the tile's far corner, not its first texel.
* **Edges past 2048 pixels** (`streams/edge_2048.rdps`): the console's edges
  are 12 bits of whole pixels; an edge past 2047 counts as crossed and the
  row is not drawn; the model draws it. test_lod.c's floor has such rows (229
  to 239, the 3,520 pixels of `floor_affine`).
* **A fill rectangle at the scissor's right edge**
  (`streams/fill_scissor_edge.rdps`, from one game's first captured task): in fill mode the console
  fills the scissor's last column (XL = 311 with the scissor's XL 311); the
  model stops a column short (its "the pixel at the scissor's end never has a
  sample" holds for sampled modes only).
* **Texture rectangles**: partial-pixel edges get partial coverage on the
  console (bit 0 and the hidden bits), full in the model (`cal_rect`: most of
  its differences are the coverage bit); and in copy mode the console does not
  clamp T to the tile (a 1-row tile read on its second row reads TMEM's next
  row; the model clamps).

## Not covered

YUV textures (choice 10), 32-bit colour images, dithering and the noise (each
side has its own generator; every stream turns them off), the blender beyond
passing the pixel.
