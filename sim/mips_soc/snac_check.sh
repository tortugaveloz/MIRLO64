#!/bin/bash
# snac_check.sh <tb_soc dir> <out dir> [Mcycles]: the SNAC port as the console's PIF on the whole SoC (docs/analogizer.md). lang/n64rt/test/snac_game.c is a game
# that drives the PIF with the SI's DMA as libultra does; the bench puts the Joybus device models of sim/analogizer/snac/jb_dev.sv on the four wires
# (+snac<n> options, soc_sim_top.sv), sets the menu's "Analogizer controllers" (a mask of the players that are the SNAC port's; INTERACT) and the runtime (lang/n64rt, built here) is the
# PIF. Runs:
#   A  Dock P1P2P3P4 (0), devices on ports 1, 2, 4   the Joybus is off: the Pocket's pad and the virtual Controller Pak are controller 1, the rest do not answer
#   B  SNAC P1P2P3P4 (15), the same devices          ports 1 (pad + pak), 2 (pad), 4 (pad + pak) are the real devices', raw, with their CRC bytes; port 3 does not answer
#   C  Dock P1, SNAC P2P3P4 (14), a device on port 2 only   controller 1 stays the virtual one (the Pocket's pad and Controller Pak), controller 2 is real
#   D  as B, with a Game Boy cartridge in the slot (GBCART=<rom>, lang/n64rt/test/make_gb.py; needs ULTRALIB): the pins are the cartridge's, no device is
#      seen -- as A -- and the bench reports no bank 0 conflict
#   E  Dock P1P2P3P4, changed to SNAC P1P2P3P4 at field 60 while the game runs: the game sees virtual only, then the real devices as in B
# The EEPROM (channel 4) is the runtime's in all of them. Pass: the game's report says 0 failed checks and the log has what the run needs.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
TB=${1:?tb_soc dir}/tb_soc; OUT=${2:?out dir}; C="${3:-90}"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
make -s -C "$R/lang/n64rt" BUILD="$OUT/rt" > "$OUT/rt.log" 2>&1 || { tail "$OUT/rt.log"; exit 1; }
RT="$OUT/rt/n64rt.bin"
DEV="+snac0=2 +snac0_btn=42435 +snac0_stick=4128 +snac1=1 +snac1_btn=1 +snac1_stick=61448 +snac3=2 +snac3_btn=3855 +snac3_stick=32641"
DEVC="+snac1=1 +snac1_btn=1 +snac1_stick=61448"
RUNS="A B C E"; [ -n "${GBCART:-}" ] && RUNS="A B C D E"
pids=()
for r in $RUNS; do
    make -s -C "$R/lang/n64rt/test" -f snac_game.mk MIRLO="$R" RUN=$r BUILD="$OUT/game_$r" > "$OUT/game_$r.log" 2>&1 || { tail "$OUT/game_$r.log"; exit 1; }
    ia=0x10000108:1; at=""
    case $r in
    A) args="$DEV" ;;
    B) args="$DEV"; ia="$ia,0x10000148:15" ;;
    C) args="$DEVC"; ia="$ia,0x10000148:14" ;;
    D) args="$DEV +gbcart=$GBCART"; ia="$ia,0x10000148:15" ;;
    E) args="$DEV"; at="60:0x10000148:15" ;;
    esac
    mkdir -p "$OUT/run_$r"
    ( cd "$HERE"; ROM="$OUT/game_$r/snac_game.z64" INTERACT="$ia" INTERACT_AT="$at" DUMP=0x43B00000,64 PAD=0-99999:0018 \
        "$TB" "$RT" -c "$C" -o "$OUT/run_$r" $args > "$OUT/run_$r.log" 2>&1 ) &
    pids+=($!)
done
for p in "${pids[@]}"; do wait $p || true; done
python3 - "$OUT" $RUNS <<'PY'
import re, sys
out, runs = sys.argv[1], sys.argv[2:]
bad = 0
for r in runs:
    log = open(f"{out}/run_{r}.log", errors="replace").read().splitlines()
    w = {}
    for l in log:
        m = re.match(r"^43b000([0-9a-f]{2}): ([0-9a-f]{8})$", l)
        if m: w[int(m.group(1), 16) // 4] = int(m.group(2), 16)
    on = any("SNAC: the Joybus is on" in l for l in log)
    cart = any("the pins are a cartridge" in l for l in log)
    own = any("owns the port" in l for l in log)
    ok = w.get(0) == 0x600D600D and w.get(1) == 0 and not any(l.startswith("SNAC: ") and ("drove" in l or "driven" in l) for l in log)
    if r == "A": ok &= not on
    elif r == "D": ok &= on and cart and not own
    else: ok &= on and own and not cart
    on = {"A": "off", "D": "on, the pins a cartridge's"}.get(r, "on") if (on or r == "A") else "OFF?"
    ticks = lambda n: w.get(n, 0) / 46875.0
    print(f"run {r}: {'PASS' if ok else 'FAIL'}: {w.get(2, 0)} checks, {w.get(1, -1)} failed, Joybus {on}; "
          f"game's write+read of a block of four buttons commands {ticks(3):.2f} ms, of four status commands {ticks(4):.2f} ms, of a pak block write {ticks(5):.2f} ms" + (f"; controller 2 answered after {w.get(7)} polls" if r == "E" else ""))
    if not ok:
        bad = 1
        print("   failed check numbers:", [w.get(8 + i) for i in range(8) if w.get(8 + i)], "report words:", {k: hex(v) for k, v in sorted(w.items())})
sys.exit(bad)
PY
