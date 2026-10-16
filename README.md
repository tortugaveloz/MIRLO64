<p align="center"><img src="docs/mirlo64.png" width="480" alt="MIRLO 64"></p>

# MIRLO 64

MIRLO 64 is an N64 core made specifically to fit in the Analogue Pocket. It intends to replicate as best as possible the N64 architecture.

It is written from scratch in SystemVerilog for the Pocket's Cyclone V FPGA and runs the games from their own ROMs. This is **version 0.1** and it is experimental: read [Status](#status) before expecting a particular game to work.

## What is in it

* **CPU**: a MIPS of the VR4300 class at 93.75 MHz, with its FPU and caches.
* **RSP and RDP**: both in hardware. The RSP runs the game's own microcode and the RDP draws the game's own command stream.
* **Video**: the picture the game's video registers describe, in a 640 x 240 frame, NTSC (59.94 Hz) and PAL (50 Hz), following the game and its region, with the N64's anti-alias, divot and dither filters.
* **Audio**: the game's own audio, at the Pocket's 48 kHz.
* **Memory**: 8 MiB of RAM, as a console with the Expansion Pak, or 4 MiB with the Pak taken out in the core's settings.
* **Controllers**: up to four through the Dock, controls are customizable.
* **Accessories and saves**: Controller Pak, Transfer Pak compatible with physical GB cartridges in the Pocket GB slot, and EEPROM, SRAM and FlashRAM saves.

A small runtime, `n64rt.sys`, loads the game and does what is not in hardware. The game runs with its own code, unmodified. No game is included, and games must be in `.z64` format (see [The games' format](#the-games-format)).

## Status

MIRLO 64 is experimental and not every game works. What is known of v0.1:

* ROMs of up to 48 MiB. A larger one is not started: the core shows "ROM exceeds maximum file size of 48 MiB."
* Rumble Pak is not supported. This is currently a limitation in the Dock.
* Only controller 1 can have a pak.
* The size of a cartridge's EEPROM comes from a list on the SD card (`n64savedb.txt`). A game that is not in it gets a 4 Kbit one.

## Installing on the Pocket

1. Download `tortuga.Mirlo64_<version>.zip` from the [releases](https://github.com/tortugaveloz/MIRLO64/releases).
2. Unzip it onto the **root** of the Pocket's SD card, merging with the folders already there. It adds these files:

   ```
   Assets/n64/common/            n64rt.sys (the runtime) and n64savedb.txt; where the games go
   Cores/tortuga.Mirlo64/        the core: mirlo.rev (the bitstream), core.json, ...
   Platforms/n64.json            the platform: Console > Nintendo 64
   Platforms/_images/n64.bin     its image
   ```

   The platform is `n64`: its file and its image are the same ones another core of that platform would bring.

3. Put the games (games in `.z64` format) in `Assets/n64/common/`.
4. On the Pocket: **openFPGA → Console → Nintendo 64**, then pick a game.

Saves (`.sav`) appear in `Saves/n64/common/` when you quit a game. To update the core, unzip a newer release the same way; it replaces the files in `Cores/tortuga.Mirlo64/`.

**Coming from version 0.1**, whose platform was `mirlo`: move your games from `Assets/mirlo/common/` to `Assets/n64/common/` and your saves from `Saves/mirlo/` to `Saves/n64/`; `Assets/mirlo/`, `Platforms/mirlo.json` and `Platforms/_images/mirlo.bin` can then be deleted.

### The games' format

A game must be a `.z64` file: big-endian, starting with the bytes `80 37 12 40`. A `.v64` or `.n64` file is the same game in another byte order, and has to be converted to `.z64` first.

## Using the core

### Settings

The core's own menu on the Pocket. The default of each is in bold.

| Setting | Choices | What it does |
|---|---|---|
| Crop Vertical | None, **8 lines**, 12 lines | Blanks that many lines at the top and the bottom of the picture |
| Crop Right | None, **1 column** | Blanks the picture's last column |
| Expansion Pak | **On**, off | With it the console has 8 MiB of RAM, without it 4 MiB; takes effect when the game is started again |
| Controller 1 pak | **None**, Controller Pak, Transfer Pak | The accessory in controller 1 |
| Stick | **D-pad**, L stick, R stick | What moves the N64's analog stick |
| D-pad | **None**, D-pad, L stick, R stick | What moves the N64's D-pad |
| Z button | None, **L**, R, ZL, ZR | The Pocket's button for Z |
| L button | **None**, L, R, ZL, ZR | The Pocket's button for the N64's L |
| R button | None, L, **R**, ZL, ZR | The Pocket's button for the N64's R |
| C buttons | **None**, Right stick | The right stick as the C buttons, besides the face buttons |
| Start button | None, **Start**, Select+Start | The Pocket's button for Start |
| Modifier | **None**, L, R, ZL, ZR | Held, it turns the face buttons into the four C buttons and it makes the D-pad presses half a joystick press |

### Saves

The Pocket writes them to `Saves/n64/common/` when you quit the core. A file appears once the game has saved.

| File | What |
|---|---|
| `<game>.sav` | The cartridge's save: EEPROM, SRAM or FlashRAM, whichever the game uses |
| `<game>.mpk` | The Controller Pak, when that setting is on |

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
python3 tools/make_release.py mirlo.rev n64rt.sys tortuga.Mirlo64_0.1.zip
```

**The fitter's seed matters.** The design fills about 90 % of the FPGA and its timing is tight, so whether a compile closes timing depends on the seed (`SEED` in `projects/mirlo.qsf`). `tools/area2/fit.sh <tag> <seed>` compiles a copy of the tree with a seed, and `tools/area2/closure.py` gives the worst slack: a bitstream is good only if no analysis is negative. The seeds that close for the hardware's sources as they are in the tree are recorded in `projects/fits.json`, best first (`tools/fits.py seed` prints the first; `tools/fits.py record <projects directory>...` writes the file after a sweep). A change to the hardware asks for a new sweep: `tools/fits.py check` tells whether the record is of these sources.

### Nightly builds

Every night at 00:00 (Europe/Madrid) the default branch is built with the recorded seed and, if that compile closes timing, published as an artifact of the [Nightly workflow](https://github.com/tortugaveloz/MIRLO64/actions/workflows/nightly.yml), kept for two weeks: a zip to unpack onto the SD card's root, like a release. `tools/nightly.sh` is that build, and it can be run by hand. It can also be run by hand on any branch (Actions > Nightly > Run workflow), for instance to check that a pull request's `projects/fits.json` closes. A nightly is the tree of that day: it has not been tried on a Pocket the way a release has.

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

## Documentation

* [docs/mips.md](docs/mips.md): the system: memory map, CPU, runtime, saves, controllers, timing and area.
* [docs/rsp.md](docs/rsp.md) and [docs/rdp.md](docs/rdp.md): the RSP and the RDP, and how each is verified.
* [docs/resolution.md](docs/resolution.md): the video output.
* [docs/control.md](docs/control.md): the registers.

## License

MIRLO 64 is released under the [Apache License 2.0](LICENSE). The third-party material it includes keeps its own license: see [NOTICE](NOTICE).

Nintendo 64 is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo or Analogue.

This development is AI assisted.
