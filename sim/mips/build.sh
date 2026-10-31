#!/bin/bash
# The game CPU in lockstep with a reference model of the N64.
#
#   N64_MODEL=<dir with the model: cpu.c bus.c pif.c boot.c n64.h> sim/mips/build.sh
#   obj_dir/Vmips_core rom.z64 [-n instructions] [-l memory latency] [-v]
#
# The core and the model run the same ROM and must agree on every retired
# instruction's pc, word and written register (64 bits), on every exception
# and on the FPU's registers. The model is sim/n64 (N64_MODEL names another
# copy). TBC adds to the bench's compiler flags: TBC=-DTB_RTL_TLB with GEN=--public-flat-rw makes a mismatch print the RTL's TLB too.
# GEN passes Verilator parameters ("-GW64_MD=1": an
# original ROM needs dmult and ddiv, which the default leaves to the
# runtime), OBJ names the build directory.
#
# gen_rand_rom.py SEED out.z64 [N] [nomd] writes a random integer program as
# a ROM ("nomd": without dmult and ddiv, for the default build).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$HERE/../../rtl/mips"
M="${N64_MODEL:-$HERE/../n64}"
export VERILATOR_ROOT="${VERILATOR_ROOT:-$HOME/local/usr/share/verilator}"
VL="${VERILATOR:-$HOME/local/usr/bin/verilator}"
O="$HERE/${OBJ:-obj_dir}"; mkdir -p "$O"
for f in cpu bus pif boot; do gcc -O2 -g -w -std=gnu11 -fno-strict-aliasing -ffp-contract=off -c "$M/$f.c" -o "$O/n64_$f.o"; done
ar rcs "$O/libn64.a" "$O"/n64_*.o
cd "$HERE"
"$VL" --cc --exe --build -j 8 -O3 -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -GSIM_COUNT_RETIRE=1 -GTLB_N=32 ${GEN:-} -Mdir "$O" \
    -CFLAGS "-O2 -I$M ${TBC:-}" -LDFLAGS "$O/libn64.a -lm -lz" --top-module mips_core "$R/fpu_n64.sv" "$R/mips_core.sv" tb.cpp > "$O/build.log" 2>&1 \
    || { grep -A4 "%Error" "$O/build.log" | head -30; exit 1; }
echo "OK -> $O/Vmips_core"
