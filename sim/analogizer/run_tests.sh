#!/bin/bash
# Verilator benches for the Analogizer's video modules (rtl/analogizer), each against a floating-point model in Python:
#   an_ypbpr  all 2^24 colours against the BT.601 matrix (max error under 1 LSB)
#   an_csync  pulse widths and the vertical interval (serrations), NTSC and PAL, both polarities, VS_LINES 3 and "as vs"
#   an_out    each mode's pin map, blanking and sync pins, on the scan-out's timing
#   an_yc     colour bars: luma, chroma amplitude and phase against the burst, the burst's position and length, the PAL
#             line alternation, sync, in NTSC and PAL
#   ./run_tests.sh            (builds into $OBJ, default /tmp/an_sim_$USER)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"; A="$R/rtl/analogizer"
OBJ="${OBJ:-/tmp/an_sim_${USER:-u}}"; mkdir -p "$OBJ"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
build() {   # name top sources tb [extra verilator args]
    local name="$1" top="$2" src="$3" tb="$4"; shift 4
    "$VL" --cc --exe --build -j 8 -O2 -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
        --x-assign fast --x-initial fast --top-module "$top" -Mdir "$OBJ/$name" "$@" -CFLAGS "-I$HERE" \
        $src "$tb" -o tb > "$OBJ/$name.log" 2>&1 || { grep -E "%Error" "$OBJ/$name.log" | head -20; tail -5 "$OBJ/$name.log"; echo "build $name failed"; exit 1; }
}
rc=0
# --- an_ypbpr
build ypbpr an_ypbpr "$A/an_ypbpr.sv" "$HERE/an_ypbpr/tb.cpp" --prefix Van_ypbpr
"$OBJ/ypbpr/tb" "$OBJ/ypbpr.bin"
python3 "$HERE/an_ypbpr/check.py" "$OBJ/ypbpr.bin" || rc=1
rm -f "$OBJ/ypbpr.bin"
# --- an_csync: VS_LINES 3 active low, VS_LINES 0 active high
build cs_a an_csync "$A/an_csync.sv" "$HERE/an_csync/tb.cpp" --prefix Van_csync -GVS_LINES=3 -GCSYNC_ACTIVE_LOW=1
build cs_b an_csync "$A/an_csync.sv" "$HERE/an_csync/tb.cpp" --prefix Van_csync -GVS_LINES=0 -GCSYNC_ACTIVE_LOW=0
for sys in ntsc pal; do
    "$OBJ/cs_a/tb" $sys 3 "$OBJ/cs.bin"; python3 "$HERE/an_csync/check.py" $sys 3 1 "$OBJ/cs.bin" || rc=1
    "$OBJ/cs_b/tb" $sys 3 "$OBJ/cs.bin"; python3 "$HERE/an_csync/check.py" $sys 0 0 "$OBJ/cs.bin" || rc=1
done
rm -f "$OBJ/cs.bin"
# --- an_yc
build yc an_yc "$A/an_yc.sv $A/an_ypbpr.sv $A/an_csync.sv" "$HERE/an_yc/tb.cpp" --prefix Van_yc
for sys in ntsc pal; do
    "$OBJ/yc/tb" $sys 3 "$OBJ/yc.bin"; python3 "$HERE/an_yc/check.py" $sys "$OBJ/yc.bin" || rc=1
done
rm -f "$OBJ/yc.bin"
# --- an_out: the pin map of each mode on the scan-out's timing
build out an_out "$A/an_out.sv $A/an_yc.sv $A/an_ypbpr.sv $A/an_csync.sv $A/an_scandbl.sv" "$HERE/an_out/tb.cpp" --prefix Van_out
"$OBJ/out/tb" | tail -1 | grep -q "ALL PASS" || { "$OBJ/out/tb"; rc=1; }
[ $rc = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $rc
