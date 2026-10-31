<p align="center"><img src="docs/mirlo64.png" width="480" alt="MIRLO 64"></p>

# MIRLO 64

MIRLO 64 is an N64 core made specifically to fit in the Analogue Pocket. It intends to replicate as best as possible the N64 architecture.

It is written from scratch in SystemVerilog for the Pocket's Cyclone V FPGA and runs the games from their own ROMs. This is **version 0.7** and it is experimental: read [Status](#status) before expecting a particular game to work.

## What is in it

* **CPU**: a MIPS of the VR4300 class at 93.75 MHz, with its FPU and caches.
* **RSP and RDP**: both in hardware. The RSP runs the game's own microcode and the RDP draws the game's own command stream.
* **Video**: the picture the game's video registers describe, in a 640 x 240 frame, NTSC (59.94 Hz) and PAL (50 Hz), following the game and its region, with the N64's anti-alias, divot and dither filters.
* **Audio**: the game's own audio, at the Pocket's 48 kHz.
* **Memory**: 8 MiB of RAM, as a console with the Expansion Pak, or 4 MiB with the Pak taken out in the core's settings.
* **Controllers**: up to four through the Dock, controls are customizable.
* **Sleep and Memories**: the Pocket's Sleep and its Memories (save states), with every accessory.
* **Accessories and saves**: Controller Pak, Transfer Pak compatible with physical GB cartridges in the Pocket GB slot, a Virtual Transfer Pak that runs a Game Boy ROM and its save from the SD card, and EEPROM, SRAM and FlashRAM saves.

A small runtime, `n64rt.sys`, loads the game and does what is not in hardware. The game runs with its own code, unmodified. No game is included, and games must be in `.z64` format (see [The games' format](#the-games-format)).

## Status

MIRLO 64 is experimental and not every game works. What is known of v0.7:

* ROMs of up to 64 MiB (the first 30 MiB in the Pocket's PSRAM, the rest in its SDRAM).
* Rumble Pak is not supported. This is currently a limitation in the Dock.
* Only controllers 1 and 2 can have a pak, and controller 2's can only be a Virtual Transfer Pak.
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
| Controller 1 pak | **None**, Controller Pak, Transfer Pak, Virtual Transfer Pak | The accessory in controller 1. The Virtual Transfer Pak takes its Game Boy game from "Load Virtual Transfer Pak 1" (one of the core's Load entries, with "Load Game": a `.gb` or `.gbc` in `Assets/n64/common/`), and keeps its save in `Saves/n64/common/<N64 game>.gbsav1`: **first copy the Game Boy game's `.sav` there with that name** (see [Saves](#saves)). Sleep and Memories work with it (its RAM is in the state); the Transfer Pak with a cartridge in the Pocket's slot is unplugged in them, and the game finds it gone when it wakes |
| Controller 2 pak | **None**, Virtual Transfer Pak | A second Virtual Transfer Pak, in controller 2 (a pad that is there, with nothing pressed, when the dock has none). Its Game Boy game is "Load Virtual Transfer Pak 2", and its save `Saves/n64/common/<N64 game>.gbsav2`; with no game loaded there the pak has no cartridge, as pak 1 has none with no game loaded. Set it before the game starts, as a game looks for its controllers then |
| Enable Gameshark codes | **Off**, on | Runs the game's GameShark codes, if it has a file in `Assets/n64/common/gameshark/` ([GameShark codes](#gameshark-codes)) |
| Stick | None, **D-pad**, L stick, R stick | What moves the N64's analog stick |
| D-pad | **None**, D-pad, L stick, R stick | What moves the N64's D-pad |
| Z button | None, **L**, R, ZL, ZR, A, B, X, Y, Select, Start | The Pocket's button for Z |
| L button | **None**, L, R, ZL, ZR, A, B, X, Y, Select, Start | The Pocket's button for the N64's L |
| R button | None, L, **R**, ZL, ZR, A, B, X, Y, Select, Start | The Pocket's button for the N64's R |
| C buttons | **None**, L stick, R stick | A stick as the C buttons, besides the face buttons |
| Start button | None, **Start**, Select+Start | The Pocket's button for Start; the Start key it leaves (alone with Select+Start, or with None) is C-up |
| Modifier | **None**, L, R, ZL, ZR | Held, it turns the face buttons into the four C buttons and it makes the D-pad presses half a joystick press |

### Saves

The Pocket writes them to `Saves/n64/common/` when you quit the core. A file appears once the game has saved.

| File | What |
|---|---|
| `<game>.sav` | The cartridge's save: EEPROM, SRAM or FlashRAM, whichever the game uses |
| `<game>.mpk` | The Controller Pak, when that setting is on |
| `<game>.gbsav1`, `<game>.gbsav2` | The Game Boy game of the Virtual Transfer Pak in controller 1, and in controller 2: the cartridge's RAM, as a Game Boy core keeps it (one file each for each N64 game) |

The Game Boy save is named after the N64 game, not after the Game Boy game. **Before using a Game Boy game in the Virtual Transfer Pak, copy its `.sav` (the Game Boy core's, from `Saves/gbc/common/` or `Saves/gb/common/`) to `Saves/n64/common/<N64 game>.gbsav1`** (`.gbsav2` for controller 2's): without that file the game starts with an empty save and writes a new one, so a save you already have is not used. To play that game again on a Game Boy core, copy the `.gbsav1` (or `.gbsav2`) back to `Saves/gbc/common/` (or `Saves/gb/common/`) with the name that core uses (`<Game Boy game>.sav`). Keep a copy of the save the first time.

### GameShark codes

With "Enable Gameshark codes" on, the core looks in `Assets/n64/common/gameshark/` for a text file named after the game, `<ID>.txt`, where `<ID>`
is the four characters of the ROM's header that say its game and its region (for example `NSME`), and runs every code in it, in every video
field, as a GameShark does. `<ID>.<version>.txt` (the header's version byte, in decimal) is used first, for a revision whose codes differ. There is no
choosing among the codes: put in the file the ones to run. A code is a line of `80123456 0063`, with anything after it as its description; the
types are `80` and `81` (writes), `A0` and `A1`, `D0` to `D3` (conditions on the next line), `50` (the repeater) and `F0` and `F1`. The codes are read when the
game starts. The file `PUT_CODES_HERE.txt` in that folder says it too.

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
python3 tools/make_release.py mirlo.rev n64rt.sys tortuga.Mirlo64_0.7.zip
```

**The fitter's seed matters.** The design fills about 97 % of the FPGA's logic (the ALMs of each recorded fit are in `projects/fits.json`) and its timing is tight, so whether a compile closes timing depends on the seed (`SEED` in `projects/mirlo.qsf`). `tools/area2/fit.sh <tag> <seed>` compiles a copy of the tree with a seed, and `tools/area2/closure.py` gives the worst slack: a bitstream is good only if no analysis is negative. The seeds that close for the hardware's sources as they are in the tree are recorded in `projects/fits.json`, best first (`tools/fits.py seed` prints the first; `tools/fits.py record <projects directory>...` writes the file after a sweep). A change to the hardware asks for a new sweep: `tools/fits.py check` tells whether the record is of these sources.

### Nightly builds

Every night at 00:00 (Europe/Madrid) the default branch is built with the recorded seed and, if that compile closes timing, published as an artifact of the [Nightly workflow](https://github.com/tortugaveloz/MIRLO64/actions/workflows/nightly.yml), kept for two weeks: a zip to unpack onto the SD card's root, like a release. `tools/nightly.sh` is that build, and it can be run by hand. It can also be run by hand on any branch (Actions > Nightly > Run workflow), for instance to check that a pull request's `projects/fits.json` closes. A nightly is the tree of that day: it has not been tried on a Pocket the way a release has.

### Simulation checks

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

## Documentation

* [docs/mips.md](docs/mips.md): the system: memory map, CPU, runtime, saves, controllers, timing and area.
* [docs/rsp.md](docs/rsp.md) and [docs/rdp.md](docs/rdp.md): the RSP and the RDP, and how each is verified.
* [docs/resolution.md](docs/resolution.md): the video output.
* [docs/control.md](docs/control.md): the registers.

## License

MIRLO 64 is released under the [Apache License 2.0](LICENSE). The third-party material it includes keeps its own license: see [NOTICE](NOTICE).

Nintendo 64 is a trademark of Nintendo. This project is not affiliated with or endorsed by Nintendo or Analogue.

This development is AI assisted.
