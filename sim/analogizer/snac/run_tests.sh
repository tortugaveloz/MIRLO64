#!/bin/bash
# Verilator bench of the SNAC port's Joybus master and port mux (rtl/analogizer/an_joybus_m.sv, an_snac_pif.sv) against behavioural
# Joybus devices (jb_dev.sv): cell timing, replies, the four ports, the scan, misbehaving devices, the pins' rules.
#   ./run_tests.sh            (builds into $OBJ, default /tmp/an_snac_$USER)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../../.." && pwd)"; A="$R/rtl/analogizer"
OBJ="${OBJ:-/tmp/an_snac_${USER:-u}}"; mkdir -p "$OBJ"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
"$VL" --cc --exe --build -j 8 -O2 -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-MULTIDRIVEN \
    -Wno-BLKSEQ -Wno-INITIALDLY -Wno-UNOPTFLAT -Wno-CASEINCOMPLETE -Wno-SYNCASYNCNET \
    --x-assign fast --x-initial fast --top-module snac_top -Mdir "$OBJ" \
    "$A/an_joybus_m.sv" "$A/an_snac_pif.sv" "$HERE/jb_dev.sv" "$HERE/snac_top.sv" "$HERE/tb.cpp" -o tb > "$OBJ.log" 2>&1 \
    || { grep -E "%Error" "$OBJ.log" | head -20; tail -5 "$OBJ.log"; echo "build failed"; exit 1; }
"$OBJ/tb"
