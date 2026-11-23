#!/usr/bin/env python3
# gspick_check.py <out dir> [base]: the checks of gspick_check.sh on the runs it left in <out dir> (A, B, C, D, and Bbase with `base`); prints PASS or FAIL for each.
import os, re, sys
out = sys.argv[1]; base = len(sys.argv) > 2 and sys.argv[2] == "base"
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    fails += 0 if ok else 1
def log(run):
    p = os.path.join(out, run, "run.log")
    return open(p, errors="replace").read() if os.path.exists(p) else None
def ram(run):
    p = os.path.join(out, run, "ram.bin")
    return open(p, "rb").read() if os.path.exists(p) else None
def word(r, off): return int.from_bytes(r[off:off + 4], "big")
def gsfile(run):
    p = os.path.join(out, run, "pfs/Saves/n64/common/gspick.gs")
    return open(p).read() if os.path.exists(p) else None
def game_ran(r): return r is not None and word(r, 0x300000) == 0x600D600D and word(r, 0x300010) == 0xD0DE
def picker_count(l): return l.count("n64rt: GameShark: the picker")

l = log("A")
if l is not None:
    r = ram("A")
    check(picker_count(l) == 3, "A: the picker ran at the start and at each of the two restarts (%d)" % picker_count(l))
    check("OPENFILE slot 10 /Saves/n64/common/gspick.gs flags 0 -> 3" in l, "A: the first picker found no file for the game")
    check(len(re.findall(r"WRITE slot 10 .*gspick\.gs", l)) == 3, "A: the file was written three times (Start, Start, B: each saves what changed)")
    check(l.count("GameShark: read /Saves/n64/common/gspick.gs") == 2 and l.count("2 cheats on") == 2, "A: the second and the third pickers read the file, with its two cheats on")
    check(len(re.findall(r"read back from the file: 0, \d+ bytes, \d codes \(\d wanted\)", l)) == 3 and len(re.findall(r"read back from the file: 0, \d+ bytes, (\d) codes \(\1 wanted\)", l)) == 3, "A: each file written was read back from the file and its codes counted: as many as wanted, three times")
    check(l.count("2 codes handed to the cheat engine") == 2 and l.count("0 codes handed to the cheat engine") == 1, "A: Start handed 2 codes to the engine twice, B handed none")
    check(l.count("n64rt: GameShark: 2 codes from the picker") == 2 and l.count("n64rt: GameShark: 0 codes from the picker") == 1, "A: the engine took them: 2, 2 and 0 codes")
    check(l.count("the first field: 2 codes run, 2 writes changed a value") == 2, "A: the engine ran them in the first field, each changing the RDRAM (twice: after the two Starts)")
    f = gsfile("A")
    want = "# Game Shark: GSPICK TEST (U)\n# Alpha 16-bit\n81300100 1234\n# Beta 8-bit\n80300104 0055\n# Gamma level = Three\n80300108 0033\n"
    ok = f is not None and f.startswith(want) and all(x == "" or x.startswith("#") for x in f[len(want):].split("\n"))
    check(ok, "A: the file holds B's choice: Alpha, Beta and Gamma at Three, then comment lines only")
    if not ok: print(repr(f))
    check(game_ran(r), "A: the game ran to its report after B")
    check(r is not None and r[0x300100] == 0 and r[0x300104] == 0 and r[0x300108] == 0 and r[0x30010C] == 0, "A: B's game is clean: none of the cheats is in the RDRAM")
l = log("X")
if l is not None:
    from PIL import Image
    import glob
    fl = sorted(glob.glob(os.path.join(out, "X/frames/f*.ppm")))
    black = []
    for f in fl:
        im = Image.open(f).convert("RGB")
        black.append(im.getextrema() == ((0, 0), (0, 0), (0, 0)))
    nonblack = [i for i, v in enumerate(black) if not v]
    last = nonblack[-1] if nonblack else -1
    check(last >= 0 and len(black) - last - 1 >= 3 and all(black[last + 1:]), "X: the picker's pictures are the last that are not black (the %d-th of %d), and every one after Start is black: the scan-out shows nothing else until the game draws" % (last, len(black)))
l = log("S")
if l is not None:
    from PIL import Image
    def fr(n):
        p = os.path.join(out, "S/frames/f%06d.ppm" % n)
        return Image.open(p).convert("RGB") if os.path.exists(p) else None
    fs = {n: fr(n) for n in (70, 110, 150, 190, 230, 260)}
    if all(v is not None for v in fs.values()):
        row = (3, 8 + 13 * 6, 317, 8 + 13 * 7)          # the fifth cheat's row (the list starts at row 2)
        crop = {n: v.crop(row).tobytes() for n, v in fs.items()}
        check(crop[70] == crop[110], "S: the row waits at its start for the first second (0.1 s and 0.7 s: the same)")
        check(crop[150] != crop[110] and crop[190] != crop[150], "S: and moves after it (1.4 s, 2.1 s: not the same)")
        check(crop[230] == crop[260], "S: and waits at the end (2.7 s, 3.1 s: the same)")
        # (the others rows do not move, and nothing is drawn in the margins)
        bg = fs[150].getpixel((0, 0))
        im = fs[150]
        w, h = im.size
        marg = [im.getpixel((x, y)) for y in range(h) for x in list(range(0, 3)) + list(range(w - 3, w))] + [im.getpixel((x, y)) for x in range(w) for y in list(range(0, 8)) + list(range(h - 8, h))]
        check(all(p == bg for p in marg), "S: the margins (3 pixels at the sides, 8 lines above and below) are the background")
        same = lambda n: fs[n].crop((3, 8, 317, 8 + 13 * 6)).tobytes()
        check(same(150) == same(190), "S: the rows above the cursor's do not move")
    else:
        check(False, "S: the frames are missing")
l = log("E")
if l is not None:
    r = ram("E"); f = gsfile("E")
    want = "# Game Shark: GSPICK TEST (U)\n# Gamma level = Three\n80300108 0033\n# Delta two codes\n8030010C 00AA\n8030010D 00BB\n"
    old = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../lang/n64rt/test/gspick_old.gs")).read()
    check("read /Saves/n64/common/gspick.gs (168 bytes, 4 cheats on)" in l, "E: the picker read the old file with its four cheats on")
    ok = f is not None and f.startswith(want) and len(f) == len(old) and all(x == "" or x.startswith("#") for x in f[len(want):].split("\n"))
    check(ok, "E: a Pocket that does not shorten the file: the file keeps its length (%d), the new choice first, then comment lines only" % len(old))
    if not ok: print(repr(f))
    check(game_ran(r), "E: the game ran")
    check(r is not None and r[0x300100] == 0 and r[0x300104] == 0, "E: Alpha and Beta, off now, are not in the RDRAM (the old tail does not run them)")
    check(r is not None and r[0x300108] == 0x33 and r[0x30010C] == 0xAA and r[0x30010D] == 0xBB, "E: Gamma (Three) and Delta, still on, are")
l = log("B")
if l is not None:
    check(picker_count(l) == 0 and "GameShark" not in l, "B: no picker without GameShark in the setting")
    check(not os.path.exists(os.path.join(out, "B/pfs/Saves")), "B: no file was written")
    check(game_ran(ram("B")), "B: the game ran")
    if base and log("Bbase") is not None:
        def lines(run): return [x for x in log(run).splitlines() if not x.startswith("[") and not x.startswith("program:") and not x.startswith("gb_cart_model:") and " cpu pc " not in x]
        check(lines("B") == lines("Bbase"), "B: the boot log is the same text as the runtime without the picker's (%d lines; not the program's size, the pc it ends at and the bench's counts at the end)" % len(lines("B")))
        a, b = ram("B"), ram("Bbase")
        # (what the game reports of its own timing, Count and loops, moves by a few cycles with the runtime's code layout; the top 4 KiB of the RDRAM is the game's stack)
        skip = lambda i: 0x300008 <= i < 0x300010 or 0x30001C <= i < 0x300020 or i >= len(a) - 0x1000
        diff = [i for i in range(len(a)) if a[i] != b[i] and not skip(i)]
        check(len(a) == len(b) and not diff, "B: the RDRAM at the end is the same as the runtime without the picker's, but for the game's timing words (%d other bytes differ)" % len(diff))
l = log("C")
if l is not None:
    check(picker_count(l) == 1 and "[pocket: OPENFILE slot 10" not in l, "C: the picker ran, found no cheats and opened no file")
    check(game_ran(ram("C")), "C: the game started")
l = log("D")
if l is not None:
    check(picker_count(l) == 1, "D: the picker ran with no database")
    check(game_ran(ram("D")), "D: the game started")
l = log("R")
if l is not None:
    r = ram("R")
    check(picker_count(l) == 1 and "WRITE slot 10" in l, "R: the picker ran and wrote the file")
    check(r is not None and r[0x33B21D] == 0x64 and r[0x33B21E] == 0x08, "R: the two plain cheats chosen are in the game's RDRAM (0x64, 0x08)")
    check(r is not None and r[0x33B177] == 0x08, "R: the option chosen for the cheat with options (the fourth, 0x08) is in the game's RDRAM")
# the framework's check of an open file against its slot's size_maximum (the bench prints an ERROR line): no run may have one
for run in ("A", "B", "C", "D", "E", "R", "S", "X"):
    l = log(run)
    if l is not None:
        check("too large" not in l, "%s: no file is larger than its slot's size_maximum (the database is in slot 11, 4 MiB, the .gs in slot 10, 64 KiB)" % run)
sys.exit(1 if fails else 0)
