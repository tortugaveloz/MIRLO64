#!/bin/bash
# Verilator bench of rtl/analogizer/an_scandbl.sv (tb_scandbl.cpp). Builds the default module (VGA polarity: both low) and
# builds with other parameters (both polarities high, VS_LINES / VS_DELAY, CB), runs the stream tests; exit status 0 if all pass.
#   ./run_tests.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../../.." && pwd)"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
fail=0
build() {   # build <name> <-G params> -- <-D macros of the bench>
    local dir="$HERE/obj_$1"; shift
    local gp=() cf=""
    while [ "$1" != "--" ]; do gp+=("$1"); shift; done; shift
    cf="$*"
    "$VL" --cc --exe --build -j 8 -O2 -Wall -Wno-fatal -Wno-DECLFILENAME -Wno-UNUSEDSIGNAL -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC \
        -Mdir "$dir" --top-module an_scandbl "${gp[@]}" -CFLAGS "-O2 $cf" \
        "$R/rtl/analogizer/an_scandbl.sv" "$HERE/tb_scandbl.cpp" -o tb > "$dir.log" 2>&1 || { tail -30 "$dir.log"; exit 1; }
}
run() { echo "== $*"; "$@" || fail=1; }
build def -- ""
run "$HERE/obj_def/tb" main
run "$HERE/obj_def/tb" noise 0 0
run "$HERE/obj_def/tb" noise 1 1
run "$HERE/obj_def/tb" noise 2 0
build alt -GHS_ACTIVE_LOW=0 -GVS_ACTIVE_LOW=0 -GVS_LINES=2 -GVS_DELAY=8 -GCB=8 -- "-DHS_LOW=0 -DVS_LOW=0 -DVS_LINES_P=2 -DVS_DELAY_P=8 -DCB_P=8"
run "$HERE/obj_alt/tb" short
build c7 -GCB=7 -GVS_LINES=3 -- "-DCB_P=7 -DVS_LINES_P=3"
run "$HERE/obj_c7/tb" short
# the write/read margin: the read may start 17 clocks earlier (zero slack: it reads each pixel the clock after it is written), not 18
build m17 -GDLY_ADJ=-17 -- "-DDLY_ADJ_P=-17"
run "$HERE/obj_m17/tb" short
build m18 -GDLY_ADJ=-18 -- "-DDLY_ADJ_P=-18"
echo "== obj_m18/tb short (must FAIL: the read overtakes the write)"
if "$HERE/obj_m18/tb" short > /dev/null; then echo "m18 passed: the margin test is not testing"; fail=1; else echo "failed as expected"; fi
[ $fail = 0 ] && echo "ALL PASS" || echo "SOME FAILED"
exit $fail
