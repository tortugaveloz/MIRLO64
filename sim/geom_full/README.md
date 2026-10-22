# sim/geom_full -- the real geom core, end to end

Verilator simulation of the **real** `rtl/mips/mips_geom.sv` CPU wired to
the **real** `rtl/vpu/Vpu4DFixed.v` CFU (`geom_cpu_top.v`), executing the
**real** `lang/c/geom/build/geom.bin`. Only memory, the mailbox and the
RDP command port are C++ models (`tb_geom_cpu.cpp`, zero-wait-state, so
cycle counts are a lower bound for hardware -- use them for A/B comparisons).

This is the only test that exercises the CPU -> CFU instruction encoding.
`sim/vpu/tb_vpu4dfixed.cpp` drives `function_id` straight into the CFU and
the host build uses a C model, so neither could see that the firmware packed
`function_id` as `{funct7, funct3}` while VexRiscv's CfuPlugin delivers
`{funct3, funct7}` (2026-09-22; see `lang/c/geom/geom_vpar.h`'s
`GEOM_VPAR_INSN`). Every vertex came out at w = 0 and no triangle was drawn.

```bash
./build.sh                 # Verilator model + host tools (tools/)
cd ../../lang/c/geom && make clean && make && cd -   # Makefile ignores header deps
./check_host_vs_rtl.sh     # host vs RTL: RDP command streams and frames bit-identical
```

`check_host_vs_rtl.sh` generates GDLs with `tools/mkgdl` (lit/unlit, 1-5
lights, fog), runs each through `tools/hostreplay` (host build of
`geom_pipeline.c`) and through the RTL, and compares the RDP command streams
and the frames the RDP C model draws from them (`tools/cmp_rdp.py`). It
also prints cycles per GDL.

`./build.sh` simulates the MIPS geom core (`rtl/mips/mips_geom.sv`) -- run it
with a firmware built CPU=mips.

Profiling and tracing, on any `obj_dir/tb_geom_cpu <geom.bin> <gdl.bin>` run:

- `PCPROF=prof.txt` -- per-PC cycle histogram of the writeback stage;
  `python3 tools/symprof.py ../../lang/c/geom/build/geom.elf prof.txt 20`
  folds it per function.
- `RETPROF=ret.txt` -- retired instructions per PC (fold it like PCPROF:
  cycles / instructions per function), and the number of taken control
  transfers. With `PCPROF_LAST=1` both cover only the last walk.
  `NM=<...-nm>` makes `tools/symprof.py` read a MIPS elf.
- `CFUTRACE=1` -- prints the first 400 CFU transactions with the
  `function_id` the CFU actually receives.
- `REPEAT=<n>` -- walk the same GDL n times as consecutive frames and print
  each walk's kick->done cycles. Anything the geom core keeps across frames
  (the decoded-texture cache) only shows from the second walk on; profile
  the steady state as `REPEAT=2` minus `REPEAT=1`.
- `SDRAM_WAIT=<n>` -- n extra cycles per SDRAM beat (default 0, the
  zero-wait model every number so far was taken with). SDRAM outside the GDL
  is plain memory now; it used to read 0 and drop writes.
- `TEXFILL=1` + `TEXHASH=<file>` -- unmapped reads return address-derived
  data (so a host-dumped GDL's texture pointers decode to distinct texels),
  and every texture page streamed to RDP logs a hash of its 2 KB. Two
  builds that must stream the same textures must log the same hashes.

Any GDL can be run here: `tools/mkgdl` makes synthetic ones, and a program
can dump the GDL it builds. geom.elf is built with `-g` (geom.bin is unchanged
by it), so `PCPROF` folds per source line with `mips-linux-gnu-addr2line`.

`work/` holds generated GDLs, captures and profiles (ignored).
