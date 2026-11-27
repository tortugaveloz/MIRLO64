#!/bin/bash
# lld_check.sh <tb_soc dir> <out dir> [Mcycles]: a game that uses LLD and SCD (lang/n64rt/test/lld_game.S), which the CPU does not have: the runtime does them
# on the game's behalf (n64rt_os.c emulate_lld_scd) as a compare-and-swap. The game leaves its results in the RDRAM (0x8030_0040 on); this reads them back
# (DUMP) and compares: a pair that succeeds (the new value stored, the register 1), a pair with the doubleword written between (scd 0, the doubleword as it
# was written), an scd with no lld before it (0), the pair with the lld and the scd in branch delay slots, libdragon's xorshift update loop three times,
# and the same through KSEG1.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
TB=${1:?tb_soc dir}/tb_soc; OUT=${2:?out dir}; C="${3:-60}"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -s -C "$R/lang/n64rt" BUILD="$OUT/rt" > "$OUT/rt.log" 2>&1 || { tail "$OUT/rt.log"; exit 1; }
make -s -C "$R/lang/n64rt/test" -f lld_game.mk MIRLO="$R" BUILD="$OUT/game" > "$OUT/game.log" 2>&1 || { tail "$OUT/game.log"; exit 1; }
( cd "$HERE"; ROM="$OUT/game/lld_game.z64" DUMP=0x43B00000,64 "$TB" "$OUT/rt/n64rt.bin" -c "$C" -o "$OUT" > "$OUT/run.log" 2>&1 )
python3 - "$OUT/run.log" <<'PY'
import re, sys
w = {}
for l in open(sys.argv[1], errors="replace"):
    m = re.match(r"^43b000([0-9a-f]{2}): ([0-9a-f]{8})$", l)
    if m: w[int(m.group(1), 16) // 4] = int(m.group(2), 16)
def d(off):                      # the doubleword at the report's byte offset
    return w.get(off // 4, -1) << 32 | w.get(off // 4 + 1, -1)
M = (1 << 64) - 1
want = {0x40: 0x0123456789ABCDEF, 0x48: 1, 0x50: 0x1111111122222222,
        0x58: 0x1111111122222222, 0x60: 0, 0x68: 0x1111111100000077,
        0x70: 0, 0x78: 0x1111111100000077,
        0x80: 0xAAAABBBBCCCCDDDD, 0x88: 1, 0x90: 0x0F0F0F0FF0F0F0F0}
s = 0x123456789ABCDEF0
for i in range(3):
    s ^= s >> 12; s ^= (s << 25) & M; s ^= s >> 27
    want[0xA0 + 8 * i] = s
want.update({0xB8: 0xDEADBEEFFEEDFACE, 0xC0: 1, 0xC8: 0x0101010102020202, 0xD0: 0, 0xD8: 0x0101010100000000})
bad = 0
if w.get(0) != 0x600D600D: print("the game did not finish (no magic in word 0)"); bad += 1
for off, v in sorted(want.items()):
    if d(off) != v: print(f"  +{off:#x}: got {d(off):016x} want {v:016x}"); bad += 1
print(f"lld_check: {'PASS' if not bad else 'FAIL'}: {len(want)} checks, {bad} failed")
sys.exit(1 if bad else 0)
PY
