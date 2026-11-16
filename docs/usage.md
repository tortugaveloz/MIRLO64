# Settings, saves and cheats

The default of each setting is in bold. For what the controller settings do in detail, see [controllers.md](controllers.md).

## Settings

The core's own menu on the Pocket.

| Setting | Choices | What it does |
|---|---|---|
| Crop Vertical | None, **8 lines**, 12 lines | Blanks that many lines at the top and the bottom of the picture |
| Crop Horizontal | None, 1 to 3 columns, 1 to 3 columns left, **1 column right**, 2 to 3 columns right | Blanks columns at the sides of the picture |
| Expansion Pak/Gameshark | Exp Pak and GameShark, **Exp Pak**, GameShark, None | The Expansion Pak gives the console 8 MiB of RAM, without it 4 MiB; it takes effect when the game is started again. GameShark runs the game's codes ([GameShark codes](#gameshark-codes)) |
| Controller paks | **None**, Controller Pak, Transfer Pak cartridge, Transfer Pak SD, Transfer Pak SD P2, Transfer Pak SD P1P2, TPak cart P1, SD P2 | The accessories in controllers 1 and 2 ([Transfer Paks](#transfer-paks)) |
| Stick | None, **D-pad**, L stick, R stick | What moves the joystick |
| D-pad | **None**, D-pad, L stick, R stick | What moves the Nintendo 64's D-pad |
| C buttons | **None**, D-pad, L stick, R stick | Lets a stick or the D-pad press the C buttons |
| Stick dead zone | 0 %, **10 %**, 20 % ... 70 % | How far a stick has to move before the game notices it |
| Start macro | **None**, C-Down + C-Up, C-Left + C-Right, C-Down + C-Left, C-Down + C-Right, C-Up + C-Left, C-Up + C-Right | Pressing those two buttons together is Start |
| R as modifier | **None**, Face C buttons, Joystick half press | While R is held, the face buttons become the C buttons, or the D-pad moves the joystick half way |
| Analogizer controllers | **Dock: P1P2P3P4**, SNAC: P1P2P3P4, Dock: P1, SNAC: P2P3P4, SNAC: P1, Dock: P2P3P4, SNAC: P1P2, Dock: P3P4, SNAC: P1P2P3, Dock: P4 | Which players use real Nintendo 64 controllers on an Analogizer, and which use the Dock |
| Restart Game | (an action) | Starts the game over, as a console switched off and on. It is the last entry |

## Transfer Paks

* **Transfer Pak cartridge**: a Game Boy cartridge in the Pocket's cartridge slot, in controller 1.
* **Transfer Pak SD**: plays a Game Boy game (`.gb` or `.gbc`) from the SD card. Choose it in "Controller paks" and load the game with "Load Transfer Pak 1", before starting the N64 game.
* **Transfer Pak SD P2**, **P1P2** and **TPak cart P1, SD P2**: put a Transfer Pak SD in controller 2, alone or together with controller 1's. Its game is loaded with "Load Transfer Pak 2".

If a game only looks for its Transfer Pak when it starts, load the Game Boy game while it runs and choose **Restart Game**.

## Saves

Saves are in `Saves/n64/common/`, written when you quit the core.

| File | What |
|---|---|
| `<game>.sav` | The game's save (EEPROM, SRAM or FlashRAM) |
| `<game>.mpk` | The Controller Pak, when it is on |

A Transfer Pak SD uses the Game Boy game's own `<game>.sav`, in the same Saves folder a Game Boy core uses, so both see the same save. **Keep a copy of your Game Boy saves the first time.**

## GameShark codes

With the codes on in "Expansion Pak/Gameshark", the core looks in `Assets/n64/common/gameshark/` for a text file named after the game, `<ID>.txt`, where `<ID>`
is the four characters of the ROM's header that say its game and its region (for example `NSME`), and runs every code in it, in every video
field, as a GameShark does. `<ID>.<version>.txt` (the header's version byte, in decimal) is used first, for a revision whose codes differ. There is no
choosing among the codes: put in the file the ones to run. A code is a line of `80123456 0063`, with anything after it as its description; the
types are `80` and `81` (writes), `A0` and `A1`, `D0` to `D3` (conditions on the next line), `50` (the repeater) and `F0` and `F1`. The codes are read when the
game starts. The file `PUT_CODES_HERE.txt` in that folder says it too.
