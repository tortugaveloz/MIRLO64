# Settings, saves and cheats

How to use the core's menu on the Pocket, where the saves go, and how to use GameShark codes. For the controller settings (stick, D-pad, C buttons, Start macro, ...) see [controllers.md](controllers.md). The default of each setting is in bold.

## Settings

| Setting | Choices | What it does |
|---|---|---|
| Crop Vertical | None, **8 lines**, 12 lines | Blanks that many lines at the top and the bottom of the picture |
| Crop Horizontal | None, 1 to 3 columns, 1 to 3 columns left, **1 column right**, 2 to 3 columns right | Blanks columns at the sides of the picture |
| Video settings | **Default**, 32-bit color, Deblur, 32-bit color + Deblur | **32-bit color** shows the game's colors without dither, at 8 bits a channel. **Deblur** turns off the N64's anti-alias and divot filters, for a sharp picture with jagged edges. Both take effect at once ([video_options.md](video_options.md)) |
| Expansion Pak/Gameshark | Exp Pak and GameShark, **Exp Pak**, GameShark, None | The Expansion Pak gives the console 8 MiB of RAM, without it 4 MiB; it takes effect when the game is started again (Restart Game does it). GameShark runs the game's codes ([GameShark codes](#gameshark-codes)) |
| Controller paks | **None**, Controller Pak, Transfer Pak cartridge, Transfer Pak SD, Transfer Pak SD P2, Transfer Pak SD P1P2, TPak cart P1, SD P2 | The accessories in controllers 1 and 2 ([Transfer Paks](#transfer-paks)) |
| Analogizer controllers | **Dock: P1P2P3P4**, SNAC: P1P2P3P4, Dock: P1, SNAC: P2P3P4, SNAC: P1, Dock: P2P3P4, SNAC: P1P2, Dock: P3P4, SNAC: P1P2P3, Dock: P4 | Which players use real N64 controllers on an Analogizer ([controllers.md](controllers.md#controllers-on-the-analogizer)) |
| Analogizer video | **Off**, RGBS, RGsB, YPbPr, Y/C NTSC, Y/C PAL, RGBHV, RGBHV with scanlines 25 %, 50 %, 75 % | The analog picture on an Analogizer in the cartridge slot ([analogizer.md](analogizer.md)); the Pocket's own screen stays on |
| Restart Game | (an action) | Starts the game over, as a console switched off and on, with everything the Pocket loaded as it is. It is the last entry. Useful for a game that looks at its Transfer Pak only when it starts: load the Game Boy game with Load Transfer Pak 1 (or 2) while the N64 game runs, then choose Restart Game |

## Transfer Paks

* **Transfer Pak cartridge**: the Transfer Pak with the Game Boy cartridge that is in the Pocket's cartridge slot, in controller 1.
* **Transfer Pak SD**: the Transfer Pak plays a Game Boy game from the SD card. It needs two steps before the N64 game starts: choose it in "Controller paks", and load the Game Boy game (`.gb` or `.gbc`) with "Load Transfer Pak 1", in either order. The core does not choose the pak for you: with None, Controller Pak or Transfer Pak cartridge a loaded Game Boy game is ignored.
* **Transfer Pak SD P2**, **P1P2** and **TPak cart P1, SD P2** put a Transfer Pak SD in controller 2, alone or together with controller 1's. Controller 2's game is loaded with "Load Transfer Pak 2" and has its own save.

Set the pak before the game starts, as a game looks for its controllers then. Sleep and Memories work with a Transfer Pak SD; a Game Boy cartridge in the Pocket's slot is unplugged in them, and the game finds it gone when it wakes.

## Saves

The Pocket writes them to `Saves/n64/common/` when you quit the core. A file appears once the game has saved.

| File | What |
|---|---|
| `<game>.sav` | The cartridge's save: EEPROM, SRAM or FlashRAM, whichever the game uses |
| `<game>.mpk` | The Controller Pak, when that setting is on |

The Transfer Pak SD's save is the Game Boy game's own: the file `<Game Boy game>.sav` (the name of the file you loaded, without its extension) in the Saves folder of the game's platform: `Saves/gbc/common/` for a game in `Assets/gbc/common/` (or a `.gbc` anywhere else), `Saves/gb/common/` for one in `Assets/gb/common/` (or a `.gb` anywhere else, or, if it is not there, in `Saves/gbc/common/`, where a Game Boy core that plays both kinds keeps it), so a Game Boy core and the N64 game see the same save. The core reads it when the Transfer Pak SD is chosen and the game is loaded, and writes it back when the game takes the Transfer Pak out of its power (as a game does when it leaves its Game Boy menu), when another Game Boy game is loaded or the setting is changed, and a couple of seconds after the game last wrote to it, and only if the game wrote to it. With no such file the game starts with an empty save, and the first write makes the file, at the size the Game Boy game's cartridge has; a file shorter than the cartridge's RAM is left alone (the game starts empty and nothing is written). Quitting the core does not write it: the Pocket tells the core nothing then, so a save is on the card a couple of seconds after the game makes it. The core lists the `gbc` and `gb` platforms for this (the Pocket opens files only in the platforms' folders a core lists). **Keep a copy of your Game Boy saves the first time.**

## GameShark codes

With the codes on in "Expansion Pak/Gameshark", the core looks in `Assets/n64/common/gameshark/` for a text file named after the game, `<ID>.txt`, where `<ID>`
is the four characters of the ROM's header that say its game and its region (for example `NSME`), and runs every code in it, in every video
field, as a GameShark does. `<ID>.<version>.txt` (the header's version byte, in decimal) is used first, for a revision whose codes differ. There is no
choosing among the codes: put in the file the ones to run. A code is a line of `80123456 0063`, with anything after it as its description; the
types are `80` and `81` (writes), `A0` and `A1`, `D0` to `D3` (conditions on the next line), `50` (the repeater) and `F0` and `F1`. The codes are read when the
game starts. The file `PUT_CODES_HERE.txt` in that folder says it too.
