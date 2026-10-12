#!/bin/bash
# nightly.sh [<out dir>]: the core from the sources as they are, as the zip a user unpacks onto the Pocket's SD card.
#
# No seed is searched for: the bitstream is compiled with the best fit recorded in projects/fits.json (tools/fits.py) and
# the zip is made only if that compile closes timing in every analysis. If it does not -- the hardware's sources have
# changed since the record -- this fails, and a seed sweep is due (tools/area2/fit.sh, then tools/fits.py record).
#
# Needs what README.md's "Building from source" names: Quartus (QUARTUS_BIN: its bin directory), a GCC for MIPS
# (MIPS_CC, MIPS_BIN), Python 3 with meson and ninja, and the submodules.
#   -> <out dir>/tortuga.Mirlo64_nightly_<date>_<commit>.zip  (default: ./nightly)
set -euo pipefail
W=$(cd "$(dirname "$0")/.." && pwd)
cd "$W"
OUT=$(mkdir -p "${1:-$W/nightly}" && cd "${1:-$W/nightly}" && pwd)
seed=$(tools/fits.py seed)
tools/fits.py check || echo "nightly: the recorded seed ($seed) is tried all the same"

echo "nightly: the C library, the boot ROM, the bitstream's images, the runtime"
make -C lang/mips -f lib.mk VARIANT=game
make -C lang/mips/boot
python3 sim/rsp/make_div_rom.py rtl/rsp/rsp_div_rom.hex
python3 tools/mips_inits.py
make -C lang/n64rt

echo "nightly: the bitstream, fitter seed $seed"
if ! WAIT=1 tools/area2/fit.sh nightly "$seed"; then
    grep -a "^Error" projects_a2_nightly.compile.log | head -20
    echo "nightly: the compile failed (projects_a2_nightly.compile.log)"
    exit 1
fi
if ! tools/fits.py verify projects_a2_nightly; then
    echo "nightly: seed $seed does not close timing with these sources: no zip. A seed sweep is due."
    exit 1
fi
grep -h "Logic utilization" projects_a2_nightly/output_files/mirlo.fit.summary

day=$(date +%Y-%m-%d)
commit=$(git rev-parse --short HEAD)
name=tortuga.Mirlo64_nightly_${day//-/}_$commit
python3 tools/package_bitstream.py projects_a2_nightly/output_files/mirlo.rbf "$OUT/mirlo.rev"
python3 tools/make_release.py "$OUT/mirlo.rev" lang/n64rt/build/n64rt.bin "$OUT/$name.zip" "nightly ${day//-/} $commit" "$day" > /dev/null
rm -f "$OUT/mirlo.rev"
echo "nightly: $OUT/$name.zip"
