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
* **Different settings for each controller.** The same configuration (button mapping, stick, C buttons, Start macro, ...) applies to all the controllers connected. They cannot be configured separately.
* **The joystick of a controller connected by USB.** See [known problems](#known-problems).

### Tested controllers

| Controller | Result |
|---|---|
| Nintendo Switch Pro Controller | Works well. |
| Xbox controller | The two joysticks are swapped. |
| Nintendo Switch Online N64 controller | The joystick does not work. C-Right and C-Up are unassigned or repeat another button. |
| 8BitDo 64 controller | C-Right and C-Down repeat another button. |

### Known problems

* **The joystick of any controller works only over Bluetooth.** Over USB, the Dock does not send the four most significant bits of the joystick's X axis, so the joystick cannot work properly. If you need the joystick, connect the controller by Bluetooth; over USB, use the D-pad.
* All the problems on this page have been reported to Analogue Support.

## Controllers on the Analogizer

With an [Analogizer](https://analogizer-fpga.com) in the Pocket's cartridge slot and an N64 SNAC board connected to it, real Nintendo 64 controllers can be plugged in. The wiring is explained on the Analogizer's own page: <https://analogizer-fpga.com/pages/nintendo-64-snac>.

### The harness

To connect the SNAC board to the Analogizer you need a small harness: two USB 3 boards, six Dupont cables and 18 pins, soldered and wired as the infographic shows.

* [Harness infographic (English)](snac-harness-en.pdf)
* [Infografía del arnés (español)](snac-harness-es.pdf)

### Supported

The original Nintendo 64 controllers and all the peripherals tested so far work.

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
