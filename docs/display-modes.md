# Pocket display modes

`Screen Size` is a Pocket-only setting at bridge address `0x10000140`
(interact word 16, ID 237). Its menu order and stored values are:

| Mode | Value | Framing |
|---|---:|---|
| Original | 0 | Default; live 640 × 240 output with the existing crop masks |
| Fit (maintain aspect ratio) | 2 | Keep the selected picture visible, preserving proportions |
| Fill screen (stretch) | 1 | Fill the display by scaling each axis independently |
| Hybrid | 4 | Midpoint between Stretch and ideal aspect-correct Overscale |
| Fill screen (overscale) | 3 | Preserve proportions, cropping the centred overflow |

The additional modes use a completed RGB888 frame in PSRAM and add about one
frame of delay (17 ms NTSC / 20 ms PAL). Original keeps its one-clock output
path. No mode changes the game's rendering resolution or game SDRAM interface.

## Picture bounds and native presets

The scan-out exports its clipped picture bounds after Crop Vertical and Crop
Right. Framing removes the core's padding and the selected crop bands. Black
borders drawn inside a game's framebuffer remain: detecting black pixels
would incorrectly zoom dark scenes. Overscale and Hybrid can crop HUD elements.

The APF interface provides eight fixed presets. Source pixels are passed
through where a preset can represent the framing; other sizes and Hybrid use
bilinear interpolation with edge-clamped taps and 8-bit fractional weights.
This avoids uneven pixel repetitions in an intermediate nearest-neighbour
resize, at the cost of some softening on the filtered path.

| Slot | Output pixels | Pocket aspect | Native source coverage / use |
|---|---|---|---|
| 0 | 640 × 240 | 4:3 | Original; Fit at width 638–640 and height 1–240; Dock Overscale at 640 × 240 |
| 1 | 640 × 240 | 10:9 | Stretch at width 638–640 and height 240 |
| 2 | 640 × 224 | 10:9 | Stretch at width 638–640 and height 224 |
| 3 | 640 × 216 | 10:9 | Stretch at width 638–640 and height 216 |
| 4 | 520 × 234 | 10:9 | Pocket Overscale at width ≥520 and height 240 |
| 5 | 480 × 216 | 10:9 | Pocket Overscale at width ≥480 and height 224 or 216 |
| 6 | 460 × 207 | 10:9 | Pocket Overscale at width ≥460 and height 213 |
| 7 | 400 × 360 | 10:9 | Filtered fallback, including all Hybrid sizes |

Source dimensions in this table are measured **after cropping**, in VI
samples. A 640 × 240 field shown at 4:3 has 1:2 pixel aspect: a sample is
half as wide as a line is tall. The filtered coordinate steps account for
that aspect. Native presets retain every selected VI sample; the fallback
resamples to 400 × 360 and can soften fine horizontal detail. Slots 1–7
specify 4:3 on Dock; slot 7 compensates for its resulting 6:5 pixel aspect.
Use the ordinary openFPGA display mode when comparing framing: optional
CRT/LCD filters can impose further scaling restrictions.

Native Fit centres the selected rows inside 640 × 240. Crop Right removes
two VI samples (one pixel of a 320-wide picture): Fit pads those columns;
native Stretch extends the retained edge colour without duplicating interior
pixels. Odd widths are also accepted, with whole-sample centred padding.
Native Overscale uses centred whole-sample rectangles with a displayed 10:9
aspect. It clips 6 extra rows from 240- or 213-row pictures, 8 from 224-row pictures, and none from
216-row pictures, compared with ideal fractional cover geometry.

Hybrid averages Stretch and ideal Overscale source-coordinate steps, then
centres the result. It removes half the source area that ideal Overscale
would crop, with approximately half Stretch's shape distortion. For a
640 × 240 picture on Pocket, it keeps about 586.67 × 240: 26.67 columns are
cropped per side and relative vertical stretch falls from 1.20 to 1.10.
This midpoint excludes the additional whole-pixel crop in native Overscale.
Portrait pictures receive the corresponding compromise on the other axis.

## Implementation

- `rtl/soc/n64_video.sv` exports the same window and masks used for RGB output.
- `target/pocket/pocket_video.sv` selects presets, prepares source coordinates
  and produces the native or bilinear-filtered output.
- `target/pocket/pocket_video_memory.sv` captures frames and fetches row pairs
  through the Pocket-only `pocket_video_psram.sv` controller and physical interface.
  It derives from upstream a92f021's controller with the unused asynchronous
  game-ROM port removed. Only die 0 is initialized and accessed; die 1 stays
  deselected. No SoC PSRAM mapping is restored.
- `core_top.sv` connects the adapter, reserves interact word 16 without
  aliasing the SoC's 16 interact words, and supplies the Dock notification
  handled by `core_bridge_cmd.v` (command `0x00B2`).

Two frame buffers occupy 2 MiB of otherwise unused PSRAM die 0, using a
1024-word pitch and 256 rows per buffer. Capture uses 64-word bursts and a
512-word asynchronous FIFO; each word carries a chunk address and RGB,
with the burst counter supplying the six low address bits. Alternating row
caches contain four even/odd RAM banks, supplying the four interpolation neighbours without duplicating
memory for more read ports. Native transfers fetch one row and fill both
cache rows identically; filtered transfers fetch the neighbouring row too.
Gray-coded completed-chunk counters cross both clock domains to track queue
space; capture reserves two chunks of headroom without a word-level counter.

Coordinates use signed Q12 arithmetic. Ratio ROMs supply /300, /360, /400
and /960 steps; a geometry ROM supplies the eight preset dimensions.
A shared DSP calculation prepares window-relative X/Y coordinate tables
during vertical blanking, using the pixel index directly rather than a wide
running accumulator. Integer clamping comparisons stay narrow.
Cache read, horizontal interpolation and vertical interpolation
form a three-clock colour pipeline. Native coordinates use zero fractions,
retaining exact RGB888 values. The odd cache address is explicitly kept to
preserve Quartus's synchronous block-memory inference.

Bounds are latched with the first captured pixel and associated with the
completed frame at VS. The coordinate tables finish within 1,100 video
clocks; upstream scan-out leaves over 18,000 before the next capture starts,
so the same bounds register serves capture and table generation. Missing captures and mid-frame bounds changes
invalidate that frame. EOL preset announcements take effect on the next
frame, so active settings follow the previously announced preset. If new
bounds no longer match an announced native preset, output is black until a
matching preset is active. Missed row fetches also produce black pixels.

Output uses 790 clocks per native line, or 572 for the filtered fallback,
and starts after four blank lines. Even the
360-row fallback completes inside the NTSC frame; frame starts follow the
source PAL/NTSC cadence. Original bypasses the frame store; SKIP is cleared
outside DE as required by the APF interface.

## Build and validation

Build `projects/mirlo.qpf` with the generated boot and RSP division ROMs.
The QIP uses the Pocket-only PSRAM controller, and the data-table MIF path
matches `platform/pocket/build_id_gen.tcl`. PSRAM pad registers and timing
constraints accompany that interface; the SoC's clocks and SDRAM timing
are unchanged. `projects/fits.json` must record a fit of the current sources:
results from the earlier 320-wide implementation do not validate this port.

Run the display simulations with Verilator, a C++ compiler and make:

```sh
sh sim/pocket_video/build.sh
NATIVE_DROPPED_FRAME=1 sh sim/pocket_video/build.sh
NATIVE_TRANSITION=1 sh sim/pocket_video/build.sh
NATIVE_EARLY_CAPTURE=1 sh sim/pocket_video/build.sh
```

Both delayed-memory and actual PSRAM-controller models compare pixels with
an independent C++ reference. Coverage includes all five modes, every directed
mode transition on Pocket and Dock, crop settings, PAL/NTSC, small/portrait/
empty/changing windows, memory stalls, missing captures and reset. The full
suite exercises unique colours at all 640 horizontal samples, including odd
windows and the last column/row. Upstream's `sim/vi_filter/regress.sh` also
checks exported bounds while retaining its scan-out and runtime-fit tests.

Use `tools/package_bitstream.py` and `tools/make_release.py` with the matching
runtime to package the build. Install `mirlo.rev`, `video.json` and
`interact.json` together, then fully quit and reload. The contributor tested
this 640-wide build on a physical Pocket and reported no issues. Dock
behaviour is covered by simulation; physical Dock testing has not been
confirmed.

API references: [presets](https://www.analogue.co/developer/docs/openfpga/core-definition-files/video-json),
[video bus](https://www.analogue.co/developer/docs/openfpga/bus-communication),
[Dock notification](https://www.analogue.co/developer/docs/openfpga/host-target-commands).
