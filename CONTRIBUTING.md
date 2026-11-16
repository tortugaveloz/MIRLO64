# Contributing

This page is for people who want to build MIRLO 64 or work on it. To use the core, see the [README](README.md).

## Building from source

You need Quartus Prime Lite 25.1std (with Cyclone V support), a GCC for MIPS with its binutils (`MIPS_CC` and `MIPS_BIN` name them; `mips-linux-gnu-` by default), Python 3 with meson and ninja, and Verilator for the simulations.

```bash
git clone --recursive https://github.com/tortugaveloz/MIRLO64.git
cd MIRLO64

make -C lang/mips -f lib.mk VARIANT=game                    # the C library for the CPU
make -C lang/mips/boot                                      # the boot ROM
python3 sim/rsp/make_div_rom.py rtl/rsp/rsp_div_rom.hex     # the RSP's reciprocal ROM
python3 tools/mips_inits.py                                 # the images the bitstream is built with

(cd projects && quartus_sh --flow compile mirlo)            # the bitstream: see the note below
python3 tools/package_bitstream.py projects/output_files/mirlo.rbf mirlo.rev

make -C lang/n64rt && cp lang/n64rt/build/n64rt.bin n64rt.sys      # the runtime
python3 tools/make_release.py mirlo.rev n64rt.sys tortuga.Mirlo64_0.9.zip
```

**The fitter's seed matters.** The design fills about 97 % of the FPGA's logic (the ALMs of each recorded fit are in `projects/fits.json`) and its timing is tight, so whether a compile closes timing depends on the seed (`SEED` in `projects/mirlo.qsf`). `tools/area2/fit.sh <tag> <seed>` compiles a copy of the tree with a seed, and `tools/area2/closure.py` gives the worst slack: a bitstream is good only if no analysis is negative. The seeds that close for the hardware's sources as they are in the tree are recorded in `projects/fits.json`, best first (`tools/fits.py seed` prints the first; `tools/fits.py record <projects directory>...` writes the file after a sweep). A change to the hardware asks for a new sweep: `tools/fits.py check` tells whether the record is of these sources.

## Nightly builds

Every night at 00:00 (Europe/Madrid) the default branch is built with the recorded seed and, if that compile closes timing, published as an artifact of the [Nightly workflow](https://github.com/tortugaveloz/MIRLO64/actions/workflows/nightly.yml), kept for two weeks: a zip to unpack onto the SD card's root, like a release. `tools/nightly.sh` is that build, and it can be run by hand. It can also be run by hand on any branch (Actions > Nightly > Run workflow), for instance to check that a pull request's `projects/fits.json` closes. A nightly is the tree of that day: it has not been tried on a Pocket the way a release has.

## Simulation checks

The [Simulation workflow](https://github.com/tortugaveloz/MIRLO64/actions/workflows/sim.yml) runs on every pull request and every push to `main` (and by hand, Actions > Simulation > Run workflow), on Ubuntu with Verilator 5.052, no Quartus and no hardware. It builds the C library, the boot ROM, the runtime and the SoC's test programs, runs the test programs on the whole SoC (`sim/mips_soc`) and fails if one of them prints `FAIL` or no `PASS` (`vi_test` and `pi2_test` are run but not gated: the first gives no verdict in its 30 Mcycles, the second has a known failure), runs the RSP against its model on a random program with every kind of instruction and the RSP's DMA bench, and runs `tools/area2/rdp_regress.sh`. The outputs are kept as an artifact when it fails. Changes to the docs alone do not run it. It is one job of about 15 runner minutes; about 25 when its caches (Verilator, built from source, and the C library) are cold.

## Repository layout

| Directory | Contents |
|---|---|
| `rtl/` | The hardware: the CPU (`mips/`), the RSP (`rsp/`), the RDP (`rdp/`) and the rest of the system (`soc/`) |
| `target/pocket/`, `platform/pocket/` | The Pocket's side: top level, clocks, the bridge to the Pocket's firmware |
| `projects/` | The Quartus project |
| `lang/n64rt/` | The runtime that becomes `n64rt.sys` |
| `lang/mips/` | The toolchain's settings, the C library, the boot ROM and the CPU's tests |
| `sim/` | Simulations (Verilator) |
| `tools/` | Generators, packaging and the fit scripts |
| `pkg/pocket/` | What goes on the SD card: the core's definition, the platform and the save-type list |
| `docs/` | Design notes |

## Design notes

The design is documented in `docs/`:

* [mips.md](docs/mips.md): the system: memory map, CPU, runtime, saves, controllers, timing and area.
* [rsp.md](docs/rsp.md) and [rdp.md](docs/rdp.md): the RSP and the RDP, and how each is verified.
* [resolution.md](docs/resolution.md): the video output.
* [control.md](docs/control.md): the registers.
