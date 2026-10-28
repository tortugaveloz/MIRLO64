#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/mirlo-apf-queued-reads
verilator=${VERILATOR:-verilator}
"$verilator" --binary --timing -Wno-fatal --top-module apf_test --Mdir "$out" \
    "$root/sim/apf/queued_reads_test.sv" "$root/target/pocket/apf_wishbone_master.sv"
"$out/Vapf_test"
