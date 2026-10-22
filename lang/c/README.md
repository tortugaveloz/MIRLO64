# C

## Examples

* [`helloworld`](./examples/helloworld/) - A simple hello world demonstration in C, with the included `printf()` function.
* [`fungus`](./examples/fungus/) - A demo with visuals, sound and controls.
* [`dhrystone`](./examples/dhrystone/) - The Dhrystone benchmarking program.
* [`keytest`](./examples/keytest/) - Prints the Pocket's buttons as they are pressed.
* [3D: `game`](./game/) - Drawing with the geometry core and RDP: a spinning cube and triangle, and the frame/GDL SDK they use. The geometry core's firmware is in [`geom`](./geom/).

## Overview

Your program runs on the game CPU, a 32-bit VR4300-class MIPS
(big-endian as the N64, single-precision FPU). Any MIPS GCC with its binutils builds
for it: `lang/mips/mips.mk` names them (`MIPS_CC`, `MIPS_BIN`) and sets the
flags (`-march=vr4300 -mabi=32 -EB -msingle-float`; doubles in software).

`lang/mips` provides the system libraries of a bare-metal program:

* `include/generated/` -- `csr.h` (an accessor per register:
  `apf_input_cont1_key_read()`, `timer0_uptime_cycles_read()`, ...), `soc.h`
  (`CONFIG_CLOCK_FREQUENCY`, ...) and `mem.h` (the memory regions), from
  `tools/mirlo_regs.py`;
* `include/system.h` (cache flushes), `uart.h`, `irq.h`, `console.h`;
* `lib.mk` -- picolibc (`printf`, `malloc`, the maths library, ...),
  compiler-rt's builtins and `lib/mirlo_rt.c` (the UART under stdio,
  delays), built once: `make -f lib.mk VARIANT=game` (the game CPU) and
  `VARIANT=lite` (the geometry and audio cores);
* `linker/` -- the program's crt0 (`init_asm.S`, with an exception vector
  that saves registers, so timer interrupts work) and linker script: the
  program starts at `0x4000_0000`;
* `program.mk` -- include it from a Makefile, list `OBJECTS`, and `make`
  gives `build/<PROGRAM>.bin` (see any of the examples).

## Building

```bash
(cd lang/mips && make -f lib.mk VARIANT=game)     # once
cd lang/c/examples/helloworld && make             # -> build/build.bin
```

`build/build.bin` is your program: copy it to `Assets/n64/common/` on the
SD card, or upload it over JTAG (`tools/jtag/jtag_program_and_run.sh`, see the
top-level README).
