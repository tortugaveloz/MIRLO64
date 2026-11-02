#!/bin/bash
# restart_check.sh: the core's "Restart Game" action on the whole SoC (docs/mips.md, "Restart Game"). The runtime is built with GBTRACE=1 and RESTART_FP=1
# (the toolchain of the README on PATH); the bench (tb_soc.cpp) plays the Pocket: INTERACT_AT chooses the action, SLOT_AT loads a Transfer Pak ROM while
# the game runs, POCKET_FS answers the runtime's file commands.
#
#   restart_check.sh pak  <tb_soc dir> <out dir>
#       restart_test's game (lang/n64rt/test) reads its Transfer Pak's status once, a moment after it starts. The pak is the Transfer Pak (SD), with no
#       ROM; the Game Boy ROM is loaded while the game runs, and the action chosen twice. Checks: the status the game reads is "no cartridge", then "a cartridge",
#       then "a cartridge"; the game starts over twice; the cartridge's RAM, which the game wrote before the first restart, is written to the save file by
#       the restart and found again by the game; every start has the same hardware registers (the runtime's `fp:` lines).
#       ULTRALIB=<an ultralib checkout> is needed for the Game Boy ROM's logo (lang/n64rt/test/make_gb.py); FIELDS="load first second end" the frames (45 60 130 215).
#   restart_check.sh preload <tb_soc dir> <out dir>
#       The same game with the Game Boy ROM loaded before it starts, and no restart: the first status it reads has the cartridge in.
#   restart_check.sh game <tb_soc dir> <out dir> <rom.z64> [first [second [end]]]
#       Any game: the action is chosen at the frames `first` and `second` (default 70 and 130; the run is `end` frames long, default 195), the frames the
#       scan-out sends are kept, and the pictures a start shows, from its first one that is not black, are compared with the first start's: the same
#       list of distinct pictures for as long as the shorter one goes, which is what a start from the beginning gives. The runtime's own log says what the
#       RSP, the RDP and the AI were doing when the action came.
# NOSIM=1: the checks only, on the run an earlier call left in <out dir>.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
MODE=${1:?pak, preload or game}; TB=${2:?tb_soc dir}/tb_soc; OUT=${3:?out dir}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -s -C "$R/lang/n64rt" GBTRACE=1 RESTART_FP=1 BUILD="$OUT/rt" > "$OUT/rt.log" 2>&1 || { tail "$OUT/rt.log"; exit 1; }
RT="$OUT/rt/n64rt.bin"
case "$MODE" in
preload)
    : "${ULTRALIB:?ULTRALIB=<an ultralib checkout>}"
    make -s -C "$R/lang/n64rt/test" -f restart_test.mk MIRLO="$R" BUILD="$OUT/game" > "$OUT/game.log" 2>&1 || { tail "$OUT/game.log"; exit 1; }
    python3 "$R/lang/n64rt/test/make_gb.py" "$ULTRALIB" "$OUT/test.gb"
    [ -n "${NOSIM:-}" ] || { rm -rf "$OUT/pfs"; ROM="$OUT/game/restart_test.z64" INTERACT=0x10000108:3 POCKET_FS="$OUT/pfs" SLOT8="$OUT/test.gb" "$TB" "$RT" -c 75 > "$OUT/run.log" 2>&1 || true; }
    python3 - "$OUT/run.log" <<'EOF'
import re, sys
log = open(sys.argv[1], errors="replace").read().splitlines()
status = [re.search(r"status ([0-9a-f]{2}) ", l).group(1) for l in log if "the game reads the status" in l]
made = next((i for i, l in enumerate(log) if "SD cartridge 0" in l), -1)
first = next((i for i, l in enumerate(log) if "the game reads the status" in l), -1)
ok = status == ["80"] and 0 <= made < first
print(("PASS " if ok else "FAIL ") + "the Game Boy ROM loaded before the game starts is a cartridge when it first looks: %s" % status)
sys.exit(0 if ok else 1)
EOF
    ;;
pak)
    : "${ULTRALIB:?ULTRALIB=<an ultralib checkout>}"
    read -r LOAD FIRST SECOND END <<< "${FIELDS:-45 60 130 215}"
    make -s -C "$R/lang/n64rt/test" -f restart_test.mk MIRLO="$R" BUILD="$OUT/game" > "$OUT/game.log" 2>&1 || { tail "$OUT/game.log"; exit 1; }
    python3 "$R/lang/n64rt/test/make_gb.py" "$ULTRALIB" "$OUT/test.gb"
    [ -n "${NOSIM:-}" ] || rm -rf "$OUT/pfs"
    [ -n "${NOSIM:-}" ] || ROM="$OUT/game/restart_test.z64" INTERACT=0x10000108:3 POCKET_FS="$OUT/pfs" SLOT_AT="$LOAD:8:$OUT/test.gb" \
        INTERACT_AT="$FIRST:0x10000130:1,$SECOND:0x10000130:1" DUMP=0x43B00000,16 "$TB" "$RT" -c "$END" > "$OUT/run.log" 2>&1 || true
    python3 - "$OUT/run.log" "$OUT/pfs" <<'EOF'
import re, sys, os
log = open(sys.argv[1], errors="replace").read().splitlines()
bad = []
def need(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: bad.append(what)
status = [re.search(r"status ([0-9a-f]{2}) ", l).group(1) for l in log if "the game reads the status" in l]
need(status == ["00", "80", "80"], "the status the game reads is no cartridge, a cartridge, a cartridge: %s" % status)
starts = [l for l in log if "restart: the game starts again" in l]
need(len(starts) == 2, "the game started over twice (%d)" % len(starts))
i_made = next((i for i, l in enumerate(log) if "SD cartridge 0" in l), -1)
i_rst = next((i for i, l in enumerate(log) if "restart: the game restarts" in l), 1 << 30)
need(0 <= i_made < i_rst, "the Game Boy ROM, loaded while the game runs, is a cartridge before the first restart")
wr = [l for l in log if "written (the game restarts)" in l]
need(len(wr) == 1, "the cartridge's RAM the game wrote is written to the save file by the restart (%d)" % len(wr))
sav = [os.path.join(d, f) for d, _, fs in os.walk(sys.argv[2]) for f in fs if f.endswith(".sav")]
need(len(sav) == 1 and open(sav[0], "rb").read(1) == b"\x00", "the save file has the byte the second start wrote")
words = {}
for l in log:
    m = re.match(r"^43b000([0-9a-f]{2}): ([0-9a-f]{8})$", l)
    if m: words[int(m.group(1), 16) // 4] = int(m.group(2), 16)
if len(words) >= 10:
    need(words[0] == 0x600D600D and words[1] & 0x80 and words[6] == 0 and words[8] == 0 and words[9] == 1,
         "the last start found the cartridge (status %02x), its RAM byte 0 was %02x and is %02x, and the RDRAM was cleared (%d)" % (words[1], words[8], words[9], words[6]))
else:
    need(False, "the game's report (DUMP) is in the log")
fp, cur = [], None
for l in log:
    if l.startswith("fp: SP_STATUS"): cur = []; fp.append(cur)
    if cur is not None and l.startswith("fp:"): cur.append(l)
need(len(fp) == 3 and fp[0] == fp[1] == fp[2], "the hardware's registers are the same at the %d starts" % len(fp))
if len(fp) == 3 and not (fp[0] == fp[1] == fp[2]):
    for a, b in zip(fp[0], fp[1]):
        if a != b: print("   start 1: " + a + "\n   start 2: " + b)
    for a, b in zip(fp[0], fp[2]):
        if a != b: print("   start 1: " + a + "\n   start 3: " + b)
sys.exit(1 if bad else 0)
EOF
    ;;
game)
    ROM_=${4:?rom.z64}; FIRST=${5:-70}; SECOND=${6:-130}; END=${7:-195}
    [ -n "${NOSIM:-}" ] || { rm -rf "$OUT/frames" "$OUT/pfs"; mkdir -p "$OUT/frames"; }
    # (POCKET_FS: the trace the runtime keeps is written to a file by path; with no Pocket to answer, each write waits a second)
    [ -n "${NOSIM:-}" ] || POCKET_FS="$OUT/pfs" ROM="$ROM_" INTERACT_AT="$FIRST:0x10000130:1,$SECOND:0x10000130:1" "$TB" "$RT" -c "$END" -o "$OUT/frames" -e 1 > "$OUT/run.log" 2>&1 || true
    python3 - "$OUT/run.log" "$OUT/frames" "$FIRST" "$SECOND" <<'EOF'
import hashlib, os, re, sys
log = open(sys.argv[1], errors="replace").read().splitlines()
frames = sorted(f for f in os.listdir(sys.argv[2]) if f.startswith("f") and f.endswith(".ppm"))
first, second = int(sys.argv[3]), int(sys.argv[4])
bad = []
def need(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: bad.append(what)
digest = {}
for f in frames:
    digest[int(f[1:7])] = hashlib.md5(open(os.path.join(sys.argv[2], f), "rb").read()).hexdigest()
def is_black(d, n):    # (a picture with next to nothing on it: under 1,000 pixels of 76,800 that are not black)
    b = open(os.path.join(sys.argv[2], "f%06d.ppm" % n), "rb").read()
    px = b[b.index(b"255\n") + 4:]
    return sum(1 for k in range(0, len(px), 3) if px[k] | px[k + 1] | px[k + 2]) < 1000
def seq(lo, hi):       # the distinct pictures from the first one that is not black, in order (a repeated one is the same frame twice)
    out, started = [], False
    for n in range(lo, hi):
        if n not in digest: continue
        if not started and is_black(digest[n], n): continue
        started = True
        if not out or out[-1] != digest[n]: out.append(digest[n])
    return out
s = [seq(1, first), seq(first + 1, second), seq(second + 1, max(digest) + 1)]
starts = [l for l in log if "restart: the game starts again" in l]
need(len(starts) == 2, "the game started over twice (%d)" % len(starts))
for l in log:
    if "restart: the game restarts" in l: print("     " + l.strip())
need("n64rt: halted" not in "\n".join(log), "the runtime did not stop")
print("     pictures after the first non-black one: start 1 %d (to frame %d), start 2 %d, start 3 %d" % (len(s[0]), first, len(s[1]), len(s[2])))
n01 = min(len(s[0]), len(s[1])); n02 = min(len(s[0]), len(s[2]))
need(n01 >= 1 and s[1][:n01] == s[0][:n01], "start 2 shows the first %d pictures of start 1, in order" % n01)
need(n02 >= 1 and s[2][:n02] == s[0][:n02], "start 3 shows the first %d pictures of start 1, in order" % n02)
fp, cur = [], None
for l in log:
    if l.startswith("fp: SP_STATUS"): cur = []; fp.append(cur)
    if cur is not None and l.startswith("fp:"): cur.append(l)
need(len(fp) == 3 and fp[0] == fp[1] == fp[2], "the hardware's registers are the same at the %d starts" % len(fp))
if len(fp) == 3 and not (fp[0] == fp[1] == fp[2]):
    for k in (1, 2):
        for a, b in zip(fp[0], fp[k]):
            if a != b: print("   start 1: " + a + "\n   start %d: %s" % (k + 1, b))
sys.exit(1 if bad else 0)
EOF
    ;;
*) echo "pak, preload or game"; exit 2 ;;
esac
