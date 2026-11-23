# Settings, saves and cheats

The default of each setting is in bold. For what the controller settings do in detail, see [controllers.md](controllers.md).

## Settings

The core's own menu on the Pocket.

| Setting | Choices | What it does |
|---|---|---|
| Crop Vertical | None, **8 lines**, 12 lines | Blanks that many lines at the top and the bottom of the picture |
| Crop Horizontal | None, 1 to 3 columns, 1 to 3 columns left, **1 column right**, 2 to 3 columns right | Blanks columns at the sides of the picture |
| Expansion Pak/Gameshark | Exp Pak and GameShark, **Exp Pak**, GameShark, None | The Expansion Pak gives the console 8 MiB of RAM, without it 4 MiB; it takes effect when the game is started again. GameShark shows the game's cheats before it starts and runs the ones you choose ([GameShark codes](#gameshark-codes)) |
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

**Known limitation: "Load Game" and the Transfer Pak.** The Transfer Pak only loads Game Boy games and their saves if no other game was loaded after the core started. Once a game has been loaded and then another one is loaded from the core's menu with "Load Game", the Pocket no longer lets the Transfer Pak load Game Boy games (and their saves) for the rest of the session. It looks like a limit of the way the Pocket loads files into the core after the first load; the cause is not known yet and it may not be fixable.

To use a Transfer Pak, start the core with the N64 game that uses it as the first game you load (for example Pokémon Stadium), then choose "Load Transfer Pak 1" (or 2) for the Game Boy games and, if the game only looks for its Transfer Pak when it starts, **Restart Game**. That sequence works. To change to another N64 game, quit the core and start it again.

## Saves

Saves are in `Saves/n64/common/`, written when you quit the core.

| File | What |
|---|---|
| `<game>.sav` | The game's save (EEPROM, SRAM or FlashRAM) |
| `<game>.mpk` | The Controller Pak, when it is on |

A Transfer Pak SD uses the Game Boy game's own `<game>.sav`, in the same Saves folder a Game Boy core uses, so both see the same save. **Keep a copy of your Game Boy saves the first time.**

## GameShark codes

With GameShark chosen in "Expansion Pak/Gameshark", a list of the game's cheats (from `Assets/n64/common/gameshark.gsdz`, which the Game Shark database project builds) appears when the core starts and at "Restart Game". The D-pad
or the stick move, left and right change a cheat's option, A turns it on and off, L/Z and R page, C-down turns all off; **Start** saves your choice and plays with the cheats, **B** saves it and plays without cheats.
The choice is the file `<the game's name>.gs` in `Saves/n64/common/` (for the ROM `Assets/n64/common/Some Game (U).z64`, `Saves/n64/common/Some Game (U).gs`), the same one the Game Shark core writes: text, one code in each line,
`80123456 0063`, with `#` lines for the cheats' names (it can be written by hand too); the types are `80` and `81` (writes), `A0` and `A1`, `D0` to `D3` (conditions on the next line), `50` (the repeater) and `F0` and `F1`.
The codes run in every video field, as a GameShark does, only when you pressed Start in the list ([mips.md](mips.md#the-picker)). The database is the file `Assets/n64/common/gameshark.gsdz` (data slot 11: the Pocket loads it with the core's files at every launch, about 0.7 MB; a missing file shows "The cheat database is not loaded." and the game starts with no cheats); the older `gameshark.gsdb` of the standalone Game Shark core is not used here. Settings that matter: **Expansion Pak/Gameshark** must be "Exp Pak and GameShark" (the Expansion Pak stays in: 8 MiB) or "GameShark" (no Pak: 4 MiB) for the list to appear; with "Exp Pak" (the default) or "None" the start is as it always was. The choice is saved even with B; only Start runs the cheats.