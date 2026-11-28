#!/bin/bash
# cartwr_check.sh <tb_soc dir> <out dir> [Mcycles]: a game that stores to the cartridge's bus where the hardware does not answer it (lang/n64rt/test/cartwr_game.S):
# a byte, a half word and a word at a stray pointer in 0xB9DD_C770 and 0x99DD_C770, the first word past the ROM's 64 MiB and the last before the PIF's. The CPU
# raises an address error for each, the runtime takes it (n64rt_io.c n64rt_hw_write) as the console does -- the ROM ignores the write -- and the game goes
# on: it has counted seven stores and leaves the magic 0x600D600D in the RDRAM (0x8030_0000, read back with DUMP) when it is through.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
TB=${1:?tb_soc dir}/tb_soc; OUT=${2:?out dir}; C="${3:-60}"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -s -C "$R/lang/n64rt" BUILD="$OUT/rt" > "$OUT/rt.log" 2>&1 || { tail "$OUT/rt.log"; exit 1; }
make -s -C "$R/lang/n64rt/test" -f cartwr_game.mk MIRLO="$R" BUILD="$OUT/game" > "$OUT/game.log" 2>&1 || { tail "$OUT/game.log"; exit 1; }
( cd "$HERE"; ROM="$OUT/game/cartwr_game.z64" DUMP=0x43B00000,64 "$TB" "$OUT/rt/n64rt.bin" -c "$C" -o "$OUT" > "$OUT/run.log" 2>&1 )
python3 - "$OUT/run.log" <<'PY'
import re, sys
w = {}
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"^43b000([0-9a-f]{2}): ([0-9a-f]{8})$", l)
    if m: w[int(m.group(1), 16) // 4] = int(m.group(2), 16)
bad = 0
if w.get(0) != 0x600D600D: print("the game did not finish (no magic in word 0)"); bad += 1
if w.get(0x40 // 4) != 7: print("  count: got %s want 7" % (("%d" % w[0x40 // 4]) if 0x40 // 4 in w else "none")); bad += 1
print(f"cartwr_check: {'PASS' if not bad else 'FAIL'}")
sys.exit(1 if bad else 0)
PY
