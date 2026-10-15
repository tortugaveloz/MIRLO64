#!/bin/sh
# Run the same pixel reference against delayed memory and the PSRAM controller.
# Requires Verilator, a C++ compiler and make; VERILATOR overrides the executable.
set -eu
HERE="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
"${VERILATOR:-verilator}" --cc --exe --build -j 4 --top-module pocket_video \
    --Mdir "$HERE/obj_dir" "$HERE/../../target/pocket/pocket_video.sv" \
    "$HERE/../../target/pocket/pocket_video_memory.sv" "$HERE/../../rtl/soc/afifo.sv" \
    "$HERE/tb_pocket_video.cpp" -o tb_pocket_video
"$HERE/obj_dir/tb_pocket_video"

"${VERILATOR:-verilator}" --cc --exe --build -j 4 --top-module pocket_video_test \
    -CFLAGS -DREAL_CONTROLLER --Mdir "$HERE/obj_controller" \
    "$HERE/pocket_video_test.sv" "$HERE/../../target/pocket/pocket_video.sv" \
    "$HERE/../../target/pocket/pocket_video_memory.sv" "$HERE/../../rtl/soc/afifo.sv" \
    "$HERE/../../target/pocket/pocket_video_psram.sv" "$HERE/tb_pocket_video.cpp" -o tb_pocket_video
"$HERE/obj_controller/tb_pocket_video"
