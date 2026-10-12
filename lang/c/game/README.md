# 3D on Mirlo: the game CPU side

Mirlo draws 3D with two cores and a rasterizer:

- **Game CPU** (VexRiscv-SMP, `rv32imafc`, with an FPU): your program. Every
  frame it writes a **GDL** — Mirlo's display-list format
  (`../geom/geom_gdl.h`) — with the helpers in `gdl_build.h`: matrices,
  viewport, lights, fog, textures, render modes, vertices and triangles. It does
  no vertex maths itself.
- **Geometry core** (`VexRiscvGeom`, `rv32im`, no FPU, firmware in
  `../geom/`): walks the GDL, transforms, lights and clips the vertices with its
  vector unit (a CFU) and sets up each triangle for MRDP.
- **MRDP** (`../../../rtl/mrdp/`): an N64-style
  rasterizer. It draws into RGB565 framebuffers in SDRAM with a 16-bit depth
  buffer.

The hand-off is a mailbox doorbell plus a GDL buffer in SDRAM; `frame.c` does
all of it.

## Files

| file | role |
|---|---|
| `frame.c/.h` | framebuffers (four RGB565 buffers + one depth buffer), GDL ring slots, `frame_begin()` / `frame_submit()` (clears, geometry-core kick, completion, vblank flip), optional pipelined mode (`frame_set_overlap()`), an FPS counter. |
| `gdl_build.h` | cursor-based helpers that write a GDL. |
| `log.c/.h` | a non-blocking log (a RAM ring pumped to the UART) for use inside the frame loop, and a trap handler that reports faults. |
| `cube_main.c` | example: a spinning, depth-tested cube. |
| `demo_main.c` | example: the smallest drawing program, a spinning Gouraud triangle. |
| `videotest_main.c` | diagnostic: cycles solid-colour framebuffers with no rendering at all — the first thing to run for any video problem. |

## The frame loop

```c
frame_init();
for (;;) {
    gdl_cur_t c;
    frame_begin(&c);                 // a GDL slot, cleared framebuffer
    gdl_mtx_load(&c, GDL_MTX_TARGET_PROJECTION, proj);
    gdl_mtx_load(&c, GDL_MTX_TARGET_MODELVIEW, modelview);
    gdl_viewport(&c, scale, translate);
    gdl_vtx(&c, 0, vertices, 3);
    gdl_tri1(&c, 0, 1, 2);
    frame_submit(&c);                // geometry core + MRDP, flip on vblank
}
```

## Building

The geometry core's firmware is baked into the bitstream (see the top-level
README); the program reads one of its variables, so build it against the same
`lang/c/geom/build/geom.elf` (or pass `GEOM_ELF=`):

```
(cd ../../mips && make -f lib.mk VARIANT=game && make -f lib.mk VARIANT=lite)   # once
(cd ../geom && make)               # geometry-core firmware
make                               # -> build/build.bin  (make PROG=demo, PROG=videotest)
```

`build/build.bin` is a game-CPU program: put it on the SD card as a Mirlo game
(`tools/make_mirlo_game.py`) or upload it over the JTAG UART
(`tools/jtag/jtag_program_and_run.sh`).
