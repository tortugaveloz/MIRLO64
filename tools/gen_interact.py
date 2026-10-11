#!/usr/bin/env python3
"""Writes pkg/pocket/Cores/tortuga.Mirlo64/interact.json: the core's settings menu.

Every entry is `writeonly` (the Pocket stores the value and writes it; it never reads it back): the configuration that loads and works on a
Pocket, and the original author's.

The Pocket shows at most 16 entries of interact.json (Analogue's docs: "Up to 16 UI entries ... can be shown"; a longer file is a load
error), a name is at most 23 characters, a list has at most 16 options, and the names use only letters, digits, space and ( ) - . / :
(the characters the menus that loaded had). The controller layout is four lists -- Normal, Shift, Shift: D-pad, D-pad keys, each with Auto
first -- and Save / Forget for this game (lang/n64rt/n64rt_ctrl.h: the lists lay over the game's saved layout, else its family's, else
Standard); their picks are not remembered between starts (persist false), so Forget for this game means Auto at the next start, and a
kept setup is Save for this game. Bus words (0x1000_0100 + 4 x n): 16 Normal, 17 Shift, 18 Shift: D-pad, 19 D-pad keys, 21 the actions (a write of 2 Saves, 3 Forgets)."""
import json, os, sys

def lst(name, ident, address, options, dflt=0, persist=True):
    e = {"name": name, "id": ident, "type": "list", "enabled": True}
    if persist:
        e["persist"] = True
    e.update({"address": address, "defaultval": dflt, "options": [{"value": v, "name": n} for v, n in options]})
    e["writeonly"] = True
    return e

def action(name, ident, value):
    return {"name": name, "id": ident, "type": "action", "enabled": True, "address": "0x10000154", "value": value, "writeonly": True}

AUTO = (0, "Auto (game)")

def build():
  entries = [
 lst("Crop Vertical", 235, "0x1000010C", [(0, "None"), (8, "8 lines"), (12, "12 lines")], 1),
 lst("Crop Right", 236, "0x10000114", [(0, "None"), (1, "1 column")], 1),
 {"name":"Expansion Pak","id":238,"type":"check","enabled":True,"persist":True,"address":"0x10000138","defaultval":1,"value":0,"value_off":1,"writeonly":True},   # (upstream's: checked writes 0 = the Pak in)
 lst("Controller 1 pak", 214, "0x10000108", [(0, "None"), (1, "Controller Pak"), (2, "Transfer Pak")]),
 lst("Shift key", 228, "0x10000110", [(0, "None"), (1, "L"), (2, "R"), (3, "ZL"), (4, "ZR"), (6, "Select")]),
 lst("Normal: A B X Y / L R", 240, "0x10000140", [AUTO, (1, "A B cR cL / Z R"), (2, "cR cD cU cL / Z R"), (3, "A B cR cL / R Z"),
                                                   (4, "A B cU Z / Z R")], persist=False),
 lst("Shift: A B X Y / L R", 241, "0x10000144", [AUTO, (1, "Same as normal"), (2, "cR cD cU cL"), (3, "A B cU cD"), (4, "/ L R (shoulders)")], persist=False),
 lst("Shift: D-pad", 242, "0x10000148", [AUTO, (1, "Same as normal"), (2, "C buttons"), (3, "Half stick"), (4, "N64 D-pad"),
                                        (5, "Stick and D-pad")], persist=False),
 lst("D-pad keys", 243, "0x1000014C", [(0, "N64 stick (default)"), (1, "N64 D-pad"), (2, "Stick and D-pad"), (3, "C buttons")], persist=False),
 lst("Stick", 225, "0x1000012C", [(0, "D-pad"), (1, "L stick"), (2, "R stick")]),
 lst("C buttons", 223, "0x10000128", [(0, "None"), (1, "Right stick")]),
 action("Save for this game", 271, 2),
 action("Forget this game", 272, 3),
  ]
  return entries

def main():
    args = sys.argv[1:]
    rest = [a for a in args if not a.startswith("--")]
    entries = build()
    assert len(entries) <= 16, "the Pocket shows 16 entries of interact.json at most"
    assert len({e["id"] for e in entries}) == len(entries)
    for e in entries:
        assert len(e["name"]) <= 23, e["name"]
        assert len(e.get("options", [])) <= 16, e["name"]
        assert all(len(o["name"]) <= 23 for o in e.get("options", [])), e["name"]
        assert 0 <= e["id"] <= 65535
        assert all(set(n) <= set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ()-./:") for n in [e["name"]] + [o["name"] for o in e.get("options", [])]), e["name"]
    out = '{"interact":{"magic":"APF_VER_1","variables":[\n' + ",\n".join(json.dumps(e, separators=(",", ":")) for e in entries) + '\n],"messages":[]}}\n'
    shipped = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "pkg", "pocket", "Cores", "tortuga.Mirlo64", "interact.json")
    path = rest[0] if rest else shipped
    open(path, "w").write(out)
    print("%d entries -> %s" % (len(entries), path))

if __name__ == "__main__":
    main()
