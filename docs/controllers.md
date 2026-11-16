# Controllers

MIRLO 64 can be played with the Pocket's own buttons, with controllers connected to the Dock, and with real Nintendo 64 controllers through an Analogizer. This page covers the three, and the options of the core's menu that change how the buttons work.

## The Pocket's buttons

The Analogue Pocket has 8 buttons and a D-Pad. A typical Nintendo 64 controller has 9 buttons and a joystick, so some buttons have to share. By default:

| Nintendo 64 | Pocket |
|---|---|
| A | A |
| B | B |
| Z | L |
| L | ZL |
| R | R |
| C-Up | ZR |
| C-Down | Select |
| C-Left | Y |
| C-Right | X |
| Start | Start |
| Joystick | D-Pad |

You can change this in the Pocket's own Controls menu, and with the core's options below.

### Reaching every button

L is on ZL and C-Up is on ZR by default, and the handheld has neither. If you need C-Up, change the configuration as follows:

* In the Pocket's Controls menu, set C-Up to "+".
* In the core's options, set **Start macro** to "C-Down + C-Up".

With this all the buttons are reachable. Start is pressed with the combination C-Down + C-Up, which in this configuration is the "-" and "+" buttons.

## Options in the core's menu

The default of each is in bold.

| Option | Choices | What it does |
|---|---|---|
| Stick | None, **D-pad**, L stick, R stick | What moves the Nintendo 64's joystick. With D-pad, the directions are the joystick at full deflection |
| Stick dead zone | 0 %, **10 %**, 20 % ... 70 % | How far a stick has to move before the game notices it, for both sticks |
| D-pad | **None**, D-pad, L stick, R stick | What moves the Nintendo 64's own D-pad |
| C buttons | **None**, D-pad, L stick, R stick | Lets a stick (or the D-pad) press the C buttons too, besides the face buttons |
| Start macro | **None**, C-Down + C-Up, C-Left + C-Right, C-Down + C-Left, C-Down + C-Right, C-Up + C-Left, C-Up + C-Right | Pressing those two buttons together is Start. The Start button alone then becomes C-Up |
| R as modifier | **None**, Face C buttons, Joystick half press | While R is held: **Face C buttons** turns X, B, Y and A into C-Up, C-Down, C-Left and C-Right; **Joystick half press** makes the D-Pad move the joystick only 60 % of the way. R does not reach the game while it is a modifier |

## Controllers in the Dock

Up to four players can play with the controllers connected to the Dock. Each controller the Dock reports as connected is a player. A controller with analog sticks can use one of them as the joystick: set **Stick** to L stick or R stick.

### Supported

* Up to four controllers, with the same buttons and options as above.
* Controllers can be connected or removed while a game runs.
* Controller Pak in controller 1.
* Transfer Pak in controller 1 and in controller 2. Transfer Pak with a Game Boy cartridge in the Pocket's slot is controller 1's only; Transfer Pak SD can be in either ([settings](usage.md#transfer-paks)).

### Not supported

* **Rumble Pak.** The Dock gives the core no way to make a controller vibrate.
* A Controller Pak in controllers 2, 3 and 4.
* A Transfer Pak in controllers 3 and 4.

<!-- TODO: add the models of controllers that have been tried in the Dock, and which of them work. -->

## Controllers on the Analogizer

With an [Analogizer](https://analogizer-fpga.com) in the Pocket's cartridge slot and an N64 SNAC board connected to it, real Nintendo 64 controllers can be plugged in. The wiring is explained on the Analogizer's own page: <https://analogizer-fpga.com/pages/nintendo-64-snac>.

### Supported

* Up to four original Nintendo 64 controllers, one for each port of the SNAC board.
* Players are chosen one by one with the **Analogizer controllers** option, so the SNAC board can serve all four players or just some of them, and the Dock's controllers the rest:

  | Choice | Players on the SNAC | Players on the Dock |
  |---|---|---|
  | **Dock: P1P2P3P4** | none | 1, 2, 3, 4 |
  | SNAC: P1P2P3P4 | 1, 2, 3, 4 | none |
  | Dock: P1, SNAC: P2P3P4 | 2, 3, 4 | 1 |
  | SNAC: P1, Dock: P2P3P4 | 1 | 2, 3, 4 |
  | SNAC: P1P2, Dock: P3P4 | 1, 2 | 3, 4 |
  | SNAC: P1P2P3, Dock: P4 | 1, 2, 3 | 4 |

* A real controller reports what it has plugged in, and the game talks to it directly, so the accessories in a real controller (Controller Pak, Rumble Pak, Transfer Pak) are the real ones. Their own settings in the core's menu are not used for them.
* The choice can be changed while a game runs, but a game looks for its controllers when it starts: use **Restart Game** after changing it.

### Not supported

* A SNAC player with nothing plugged in has no controller, as on a console. The Pocket's buttons do nothing for it.
* With a Game Boy cartridge in the Pocket's slot, the slot belongs to the cartridge and every player is the Dock's. Take the cartridge out and restart the core to use real controllers.
* SNAC boards for other consoles (NES, SNES, ...). Do not connect one while a player is set to the SNAC.
* GameCube controllers.
