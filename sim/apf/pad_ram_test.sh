#!/bin/sh
# io_pad_controller (platform/pocket) against a model of the Pocket's pad bus: its poll and heartbeat words go out as the
# protocol has them, and the twelve words a device answers with land in the pad RAM's words (0..3 keys, 4..7 sticks,
# 8..11 triggers). Verilator cannot take the module's tri-state pin: its copy here has the pin split (pad_1wire in, pad_o out).
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/mirlo-apf-pad-ram
verilator=${VERILATOR:-verilator}
mkdir -p "$out"
sed 's/^inout   reg             pad_1wire,/input wire pad_1wire, output reg pad_o,/; s/pad_1wire <= 1.bZ/pad_o <= 1'"'"'b1/; s/^\( *\)pad_1wire <=/\1pad_o <=/' \
    "$root/platform/pocket/io_pad_controller.v" > "$out/io_pad_controller.v"
"$verilator" --binary --timing -Wno-fatal --top-module pad_ram_test --Mdir "$out" \
    "$root/sim/apf/pad_ram_test.sv" "$out/io_pad_controller.v" "$root/platform/pocket/common.v"
"$out/Vpad_ram_test"
