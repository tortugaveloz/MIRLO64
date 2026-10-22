# sim/rdp_oracle: the RDP model against an independent software RDP

`lang/c/mrdp` (the RDP's reference model, built with `MRDP_N64`, the texture
level of detail of [docs/lod.md](../../docs/lod.md) included) is written from documentation. This directory runs it, command for command,
next to an independent, pixel-exact, cycle-agnostic software RDP (the oracle)
on the same command words and the same initial RDRAM, and compares the colour
image, its hidden coverage bits and the depth image after every primitive. For
the LOD it also compares, pixel for pixel, the S and T each side took at the
LOD's three points and what each made of them.

The findings are in [RESULTS.md](RESULTS.md). The model built with
`RDP_EXACT` follows the console where the two differ:
[docs/rdp_exact.md](../../docs/rdp_exact.md), each rule with its numbers.

## The oracle

The oracle is the RDP core of angrylion-rdp-plus
(<https://github.com/ata4/angrylion-rdp-plus>, MAME licence), pinned at commit
`9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc`. It is not part of this repository:
`setup.sh` clones it (from the URL above, or one given as its argument) into
`build/oracle/src/` at the repository's root (ignored by git) and applies
`oracle_hooks.patch`, which only adds two macros that report
each 2-cycle LOD and the pixel it is for (they expand to nothing unless
defined; the oracle's behaviour is unchanged). Nothing is built or run from
inside that directory: the Makefile compiles its `n64video.c` from here, as one
translation unit (`oracle_glue.c`), single-threaded, with its messages and
worker threads stubbed.

## Build and run

On macOS (Apple clang, arm64) or in the tools image (`tools/docker/run.sh`,
Debian):

```sh
cd sim/rdp_oracle
./setup.sh            # once: fetch the oracle (needs the network), apply the hooks
make                  # build/rdpo, build/rdpo_exact, build/rdpgen
./run.sh 50           # every suite, 50 seeds: one TOTAL line each, logs in build/logs/
```

`MODEL_DIR=<dir>` (to `make` and `run.sh`) compares another copy of the model;
`ORACLE_DIR=<dir>` (to `make`) takes the oracle from elsewhere;
`TRIAGE_DIR=<dir>` (to `run.sh`) finds the captured task dumps somewhere other
than `build/triage` at the repository's root.

`run.sh` runs every suite through the model without `RDP_EXACT` (`rdpo`) and
with it (`rdpo_exact`), builds four copies of the model (without
the switch) with one LOD choice changed (the mutants: all of DaDy, `|d|`, the
walk reversed, no fraction without TEX_LOD) and shows that each differs from
the oracle, runs `streams/` through both models, and runs the captured task
dumps (`task_gfx_*.bin` under `build/triage`) when there are any, through
both.

In the tools image: `tools/docker/run.sh make -C sim/rdp_oracle B=build/linux`
(a build directory of its own, so the two builds do not mix).

| program | what |
|---|---|
| `build/rdpo` | the model without `RDP_EXACT`, as the RTL is, and the LOD comparison |
| `build/rdpo_exact` | the model with `RDP_EXACT` (docs/rdp_exact.md), and the LOD comparison |
| `build/rdpgen SUITE [SEED [N]]` | writes a stream (below) to stdout |
| `task2rdps.sh DUMP` | a captured graphics task dump (`build/triage/*/dumps`) as a stream, through `sim/rdp/rdp_task` built here |

`rdpo [-q] [-v] [-noresync] [-nodepth] [-min FILE [-at N]] [-ppm PREFIX] stream.rdps ...`

* `-q` one summary a stream; `-v` more examples
* `-min FILE` writes the first differing primitive (or command `N` with `-at`)
  as a stream of its own: the stream's memory lines, every state command before
  it, and it
* `-ppm PREFIX` the last colour image as each side drew it, and a diff image
* `-noresync`: by default, after each primitive the model's images (colour,
  hidden bits, depth) are made the oracle's again, so that each primitive is
  judged alone and a difference names the command that made it; this keeps
  each side's own images instead

## How the commands are fed

A stream (`.rdps`) is text: the initial RDRAM, then one RDP command a line.

```
# comment
rdram zero                      # (or: rdram random SEED; rdram file PATH OFFSET: 8 MiB of a file)
mem 200000 f801f801...          # bytes at an RDRAM address (big-endian, as the N64's)
fill 300000 1000 ff             # LEN bytes of a value
cmd 3f10013f 00100000           # a command, its 32-bit words, high word first
```

Both sides start from the same 8 MiB. The model's memory is big-endian bytes
with its hidden coverage plane at 8 MiB (`mrdp.h`, `MRDP_HID_BASE`); the
oracle's is its own (32-bit words in host order and a byte of hidden bits a
16-bit word). The hidden bits start as the oracle's do: a word never written
has its bit 0 twice. Each command goes whole to the oracle first (through its
DMEM path, so its RDRAM holds only images and textures), then word by word to
the model (`mrdp_push`, as `sim/rdp/tb_rdp.cpp` does). After a primitive
(triangle, texture rectangle, fill rectangle) the current colour image (the
last `SET_COLOR_IMAGE`, its width, as many rows as the scissor has), its hidden
bits and the depth image (when it is not the colour image) are compared word by
word.

Before each stream both sides get a SET_TILE of zeros for each of the eight
tiles: the oracle keeps flags it derives from a tile's fields at SET_TILE
(whether it clamps, among them), so a tile a stream never set would sample
unclamped there, which no console state matches (a zero tile clamps).

The LOD comparison (`rdpo`, `rdpo_exact`): the Makefile writes a copy of
`mrdp.c` under `build/` with two calls added (the inputs `lod_pixel` gives
`mrdp_lod`, and the result `draw_pixel_cvg` takes; the build checks both were
added). The oracle's hook reports each 2-cycle LOD with its pixel. For every
pixel the model draws with an LOD, the two are compared: the same inputs and
result, other inputs (S, T at the pixel, the next pixel, the next line, or the
range flag), or the same inputs and another result (the LOD logic itself).
And every LOD the oracle computes is also put through the model's `mrdp_lod`
with the oracle's own inputs (the "unit" line): the logic alone, on every
pixel of every primitive, drawn or not.

## The suites (`rdpgen`)

| suite | what |
|---|---|
| `cal` (`RDPGEN_CAL=flat, aa, shade, z, aff, persp, rect, t1`) | no LOD: one feature at a time, to calibrate the harness |
| `units` | test_lod.c's worked steps, each an affine triangle, left- and right-major, and two with a DtDy fraction under and at the 17 bits kept |
| `floor` | test_lod.c's perspective floor (seed 2 sharpen, 3 detail; `RDPGEN_PERSP=0` affine) |
| `lod2` | random 2-cycle triangles with TEX_LOD: levels 1..7, detail and sharpen, perspective, magnified and minified, either major edge, min level, primitive tile, now and then no texture coefficients |
| `frac2` | the same without TEX_LOD (the combiner reads LOD_FRACTION) |
| `lod1` | 1-cycle triangles with TEX_LOD (`RDPGEN_LEVEL`, `RDPGEN_DETAIL`, `RDPGEN_TEXLOD` to force) |
| `rect` | texture rectangles and flipped ones with TEX_LOD, 1- and 2-cycle |

`run.sh` adds two suites from these: `lod2_t1` (`lod2` with `RDPGEN_CC=5`: the
second cycle reads TEXEL1, the next pixel's first texel on the console) and
`lod1_t1` (`lod1` with `RDPGEN_CC=3`: a 1-cycle combiner of TEXEL1, also a
pixel ahead).

`RDPGEN_EXACT=1` keeps triangles on whole pixels with steps whose low 13 bits
are 0; `RDPGEN_CC=k` forces a combiner. Dithering is off in every stream (the
noise is each side's own), and the blender passes the pixel.

`streams/` holds the minimal stream of each difference found (RESULTS.md);
each says in its header what it shows.

## Files

| file | what |
|---|---|
| `setup.sh`, `oracle_hooks.patch` | the oracle, fetched and hooked |
| `oracle_glue.c` | the oracle in one translation unit, a command at a time |
| `rdpo.c` | the comparison |
| `rdpgen.c` | the streams |
| `task2rdps.sh` | a task dump as a stream |
| `run.sh` | all of it |
| `streams/*.rdps` | the minimal streams |
