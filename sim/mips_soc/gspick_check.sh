#!/bin/bash
# gspick_check.sh <tb_soc dir> <out dir> [baseline runtime n64rt.bin]: the GameShark picker (lang/n64rt/n64rt_gspick.c) on the whole SoC, with a pad script.
# The game is restart_test (lang/n64rt/test) with a header that the cheat database of this check (lang/n64rt/test/gspick_cheats.txt: four cheats, one with
# options) knows; the cheat database is built by the Game Shark core's tools/build_db.py (GAMESHARK=<its checkout>, default ~/projects/GameShark); the bench is the
# Pocket (INTERACT: the core's settings, INTERACT_AT: the "Restart Game" action, PAD: the buttons, POCKET_FS: the card, with the database as the data slot 11's file).
#   run A  "Exp Pak and GameShark" (setting 2): the picker shows the game's four cheats. First picker, with every kind of input: A (Alpha on), the STICK down, A (Beta on),
#          C-down (all off), the D-pad down, right twice (Gamma on at its third option), Z (a page up: the cursor to the first cheat), A (Alpha on), Start; the file is written
#          (Alpha and Gamma at Three: each input counts, or the file differs) and the game starts with the codes. "Restart Game": the picker shows again with the file's choice
#          on; Alpha off, Beta on, Start; the file is written whole (smaller than before). "Restart Game" again: Alpha toggled, then B: the picker is left without
#          saving, the file is as it was, and the game starts with its codes.
#   run B  setting 0 (no GameShark): no picker, no file, the game starts and runs as it does with no picker (the log has none of the picker's lines; with a baseline
#          runtime -- a build of the tree without the picker -- the log is the same text).
#   run C  setting 2 with a game the database does not have (its CRC changed): the picker says so; Start (else it goes on by itself after 3 s) starts the game.
#   run D  setting 2 and no database on the card: the picker says so; Start starts the game.
#   run E  a Pocket that does NOT shorten a file that OPENFILE's set-size flag shrinks (POCKET_NO_TRUNC=1): the file is there already, longer (gspick_old.gs: four cheats
#          on); Alpha and Beta are turned off and Start writes the new choice, which is shorter: the file keeps its length, the tail of old cheats is comment lines, and the
#          game runs only the two cheats left (and not the ones in the old tail).
#   run S  the screen: the picture's margins (3 pixels at each side, 8 lines above and below), and the scroll of a row that does not fit (the fifth cheat's name): the cursor goes to it
#          and stays; the frames at 0.1 s and 0.7 s (the 1 s pause at the start), 1.4 s and 2.1 s (moving), 2.7 s and 3.1 s (at the end, the 2 s pause) are different or equal as the
#          scroll says; the other rows are cut at the window's end and the margins are the background.
#   run X  the way out: Start at once (nothing to save); every second field's picture (FRAME_EVERY=2; not EVERY, which the bench takes for a progress line every that many cycles) from the picker's last one on is black until the game draws -- the game of this check
#          draws nothing --, with no picture of anything else between.
#   run R  (only with REALROM=<a .z64 whose header the database knows, with plain cheats at its positions 3 and 4 and one with options at 6 -- the pad chooses them,
#          and gspick_check.py looks for their codes in the RDRAM -- > and GSDB=<the Game Shark database project's gameshark.gsdz>) the pictures of a real game's picker and of the game after Start.
# GSADDR=10000150 for the main core. Frames (PNG, every 10th field) are in <out dir>/<run>/frames. Slow: each run is some 100 to 250 Mcycles of the whole SoC.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
TB=${1:?tb_soc dir}/tb_soc; OUT=${2:?out dir}; BASE=${3:-}
GAMESHARK=${GAMESHARK:-$HOME/projects/GameShark}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -s -C "$R/lang/n64rt" BUILD="$OUT/rt" > "$OUT/rt.log" 2>&1 || { tail "$OUT/rt.log"; exit 1; }
RT="$OUT/rt/n64rt.bin"
make -s -C "$R/lang/n64rt/test" -f restart_test.mk MIRLO="$R" BUILD="$OUT/game" > "$OUT/game.log" 2>&1 || { tail "$OUT/game.log"; exit 1; }
python3 "$GAMESHARK/tools/build_db.py" "$R/lang/n64rt/test/gspick_cheats.txt" "$OUT/test.gsdb" > /dev/null
python3 "$R/lang/n64rt/test/gsdb_to_gsdz.py" "$OUT/test.gsdb" "$OUT/test.gsdz" > /dev/null
python3 - "$OUT/game/restart_test.z64" "$OUT/gspick.z64" "$OUT/other.z64" <<'P'
import sys
g = bytearray(open(sys.argv[1], 'rb').read())
g[0x10:0x18] = bytes.fromhex('0000C0DE0000FEED'); g[0x20:0x40] = b'GSPICK TEST'.ljust(0x1E) + b'\0\0'; g[0x3E] = 0x45
open(sys.argv[2], 'wb').write(g)
g[0x10:0x18] = bytes.fromhex('12345678')+bytes.fromhex('9ABCDEF0')
open(sys.argv[3], 'wb').write(g)
P
GS=${GSADDR:-1000011C}      # (the address of the "Expansion Pak/Gameshark" list: 1000011C in the Enhanced core, 10000150 in the main core, where the list writes INTERACT1[25:24])
run() {     # <name> <rom> <rt> <setting> <fields> <pad> [more env as VAR=value...]
    local n=$1 rom=$2 rt=$3 set=$4 cyc=$5 pad=$6; shift 6
    rm -rf "$OUT/$n"; mkdir -p "$OUT/$n/frames" "$OUT/$n/pfs/Assets/n64/common"
    cp "$rom" "$OUT/$n/pfs/Assets/n64/common/$(basename "$rom")"
    [ -z "${SEED:-}" ] || { mkdir -p "$OUT/$n/pfs/Saves/n64/common"; cp "$SEED" "$OUT/$n/pfs/Saves/n64/common/gspick.gs"; }
    local dbf=""; [ "$n" = D ] || dbf="${DBFILE:-$OUT/test.gsdz}"          # (the database is a data slot the Pocket loads: the bench loads slot 11 with it; none in run D)
    env ${dbf:+SLOT11="$dbf"} ROM="$rom" INTERACT=0x$GS:$set POCKET_FS="$OUT/$n/pfs" FB_N64=1 PAD="$pad" RAMDUMP="$OUT/$n/ram.bin" "$@" \
        "$TB" "$rt" -c "$cyc" -o "$OUT/$n/frames" -e ${FRAME_EVERY:-10} > "$OUT/$n/run.log" 2>&1 || true
    python3 - "$OUT/$n/frames" <<'P'
import glob, sys
from PIL import Image
for f in sorted(glob.glob(sys.argv[1] + '/f*.ppm')): Image.open(f).save(f[:-4] + '.png')
P
}
which=${WHICH:-ABCD}      # (R is not in the default: it needs a game)
case $which in *A*) run A "$OUT/gspick.z64" "$RT" 2 ${A_CYCLES:-560} "30-34:0010,40-44:30000000/8080FF80,50-54:0010,60-64:4000,70-74:0002,80-84:0008,90-94:0008,100-104:0100,110-114:0010,120-124:8000,260-264:0010,270-274:0002,280-284:0010,290-294:8000,430-434:0010,440-444:0020" \
    INTERACT_AT=${RESTARTS:-215:0x10000130:1,385:0x10000130:1} ;; esac
case $which in *B*) run B "$OUT/gspick.z64" "$RT" 0 ${B_CYCLES:-70} ""
    [ -z "$BASE" ] || run Bbase "$OUT/gspick.z64" "$BASE" 0 ${B_CYCLES:-70} "" ;; esac
case $which in *S*) run S "$OUT/gspick.z64" "$RT" 2 ${S_CYCLES:-270} "30-34:0002,40-44:0002,50-54:0002,60-64:0002" ;; esac
case $which in *X*) FRAME_EVERY=2 run X "$OUT/gspick.z64" "$RT" 2 ${X_CYCLES:-170} "30-34:8000" ;; esac
case $which in *E*) SEED="$R/lang/n64rt/test/gspick_old.gs" run E "$OUT/gspick.z64" "$RT" 2 ${E_CYCLES:-170} "30-34:0010,40-44:0002,50-54:0010,60-64:8000" POCKET_NO_TRUNC=1 ;; esac
case $which in *C*) run C "$OUT/other.z64" "$RT" 2 ${C_CYCLES:-100} "40-41:8000" ;; esac
case $which in *D*) run D "$OUT/gspick.z64" "$RT" 2 ${D_CYCLES:-100} "40-41:8000" ;; esac
case $which in *R*) DBFILE="${GSDB:?GSDB=<gameshark.gsdz>}" run R "${REALROM:?REALROM=<.z64>}" "$RT" 2 ${R_CYCLES:-450} "30-34:0002,40-44:0002,50-54:0002,60-64:0010,70-74:0002,80-84:0010,90-94:0002,100-104:0002,110-114:0008,120-124:0008,130-134:0008,150-154:8000" \
 ;; esac
python3 "$HERE/gspick_check.py" "$OUT" "${BASE:+base}"
