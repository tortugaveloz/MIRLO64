#!/bin/sh
# Full controller arbitration/refresh regression. Set ALTERA_MF to Intel's
# altera_mf.v to include the vendor CDC FIFO model; the default model is for CI.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/mirlo-apf-sdr-deadline
verilator=${VERILATOR:-verilator}
fifo=${ALTERA_MF:-$root/sim/apf/stream_model.sv}
"$verilator" --binary --timing -Wno-fatal --top-module sdr_deadline_test -GWORDS=8192 --Mdir "$out" \
    "$fifo" "$root/sim/apf/sdr_deadline_test.sv" "$root/target/pocket/apf_wishbone_master.sv" \
    "$root/rtl/soc/n64_ports.sv" "$root/rtl/soc/sdr_arb.sv" "$root/rtl/soc/sdr_ctrl.sv" "$root/rtl/soc/psram.sv"
"$out/Vsdr_deadline_test" +period=100 +dma +drain_dma +protect_pending
"$out/Vsdr_deadline_test" +period=100 +dma +drain_dma +protect_pending +random_traffic +traffic_seed=13
"$out/Vsdr_deadline_test" +period=100 +dma +psram +drain_dma +protect_pending
"$out/Vsdr_deadline_test" +period=20 +dma +psram +drain_dma +protect_pending +write
# Prove that blocking new DMA is insufficient when one is already in flight.
ulimit -c 0
if "$out/Vsdr_deadline_test" +period=100 +dma +protect_pending > "$out/old-priority.log" 2>&1; then
    echo "The no-drain starvation control unexpectedly passed" >&2
    exit 1
fi
if ! grep -Eq 'missed deadline|FIFO overflow|Overflow occurred' "$out/old-priority.log"; then
    cat "$out/old-priority.log" >&2
    exit 1
fi
echo "Without DMA drain: starvation reproduced"
