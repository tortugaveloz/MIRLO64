#!/bin/sh
# The interact side of mirlo_regs under Icarus Verilog (iverilog, vvp): sim/interact/interact_test.sv.
set -eu
R=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT HUP INT TERM
iverilog -g2012 -I "$R/rtl/soc" -s interact_test -o "$B/sim" \
    "$R/rtl/soc/mirlo_regs.sv" "$R/rtl/soc/fifo_fwft.sv" "$R/sim/interact/rcp_mi_stub.sv" "$R/sim/interact/interact_test.sv"
vvp -N "$B/sim"
