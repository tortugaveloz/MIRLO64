# Diagnostic runtime for the SNAC port (not for release)

`n64rt.sys` here is this branch's runtime (`lang/n64rt`, built with `make -C lang/n64rt`), for finding out why a controller on the SNAC port is
now and then reported as "no controller" to the game.

What it does differently from `light-pif`: the SNAC hardware scan is off (`SNAC.CTRL.SCAN` clear), and while the adapter owns the cartridge port
every one of the four ports counts as there, so the game's own commands are the only traffic on the wires. A port with nothing on it gets the
wire's no-response flag, as an empty controller port of a console does. While the menu has the Joybus on, the Pocket's own pad is not controller 1.

Use it with the bitstream of the `light-pif` build (`mirlo.rev`): the hardware is unchanged, only the runtime is. Copy it to
`Assets/n64/common/n64rt.sys` on the card.

If the "no controller" pauses stop with it, the scan (a game command that waits for a scan and then starts 12 us after the pad's reply) is the
cause; if they go on, it is the wire or the pad.

SHA-256: see `n64rt.sys.sha256`.
