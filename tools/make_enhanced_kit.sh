#!/bin/bash
# make_enhanced_kit.sh <mirlo_oc.rbf> <n64rt.bin> <out dir>: the files of the "Mirlo64 Enhanced" core (the build with the RSP and the RDP at 93.75 MHz,
# REV=mirlo_oc) to copy over an SD card: Cores/tortuga.Mirlo64 Enhanced/ (pkg/pocket's core definition under its own name, the bitstream) and its runtime,
# Assets/n64/common/n64rt_enh.sys -- not n64rt.sys, which the Mirlo64 core uses from the same folder.
set -e
W=$(cd "$(dirname "$0")/.." && pwd)
rbf=$1; rt=$2; out=$3
[ -f "$rbf" ] && [ -f "$rt" ] && [ -n "$out" ] || { sed -n 2,4p "$0"; exit 1; }
C="$out/Cores/tortuga.Mirlo64 Enhanced"
rm -rf "$C"; mkdir -p "$C" "$out/Assets/n64/common"
for f in core.json data.json interact.json video.json audio.json input.json variants.json icon.bin; do cp "$W/pkg/pocket/Cores/tortuga.Mirlo64/$f" "$C/$f"; done
sed -i 's/"shortname": "Mirlo64"/"shortname": "Mirlo64 Enhanced"/; s/"description": "MIRLO 64: A Nintendo 64 core for the Analogue Pocket"/"description": "MIRLO 64 Enhanced: the Nintendo 64 core with the RSP and the RDP at 93.75 MHz"/; s/"version": "\([^"]*\)"/"version": "\1 Enhanced"/' "$C/core.json"
sed -i 's/"filename": "n64rt.sys"/"filename": "n64rt_enh.sys"/' "$C/data.json"
{ echo "MIRLO 64 Enhanced: A Nintendo 64 core for the Analogue Pocket, with the RSP and the RDP at 93.75 MHz."; tail -n +2 "$W/pkg/pocket/Cores/tortuga.Mirlo64/info.txt"; } > "$C/info.txt"
python3 "$W/tools/package_bitstream.py" "$rbf" "$C/mirlo.rev"
cp "$rt" "$out/Assets/n64/common/n64rt_enh.sys"
