#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/mirlo-apf-arbiter-readback
verilator=${VERILATOR:-verilator}
"$verilator" --binary --timing -Wno-fatal --top-module apf_arbiter_test -GAPF_PREEMPT=1 -GWORDS=32768 --Mdir "$out" \
    "$root/sim/apf/stream_model.sv" "$root/sim/apf/arbiter_readback_test.sv" \
    "$root/target/pocket/apf_wishbone_master.sv" "$root/rtl/soc/n64_ports.sv" "$root/rtl/soc/sdr_arb.sv"
for gap in 1 2 4 8; do
    "$out/Vapf_arbiter_test" +urgent +gap="$gap"
    "$out/Vapf_arbiter_test" +urgent +gap="$gap" +write
done
