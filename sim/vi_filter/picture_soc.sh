#!/bin/bash
# The core's "Picture" setting (docs/video_options.md) on the whole SoC: lang/mips/test's vi32_test (a 32-bit picture) and viaa_test
# (a 16-bit one), run with the setting written as the Pocket does (INTERACT=0x10000144:<n>), against the checkers of this
# directory. Needs the SoC bench and the two tests built:
#   OBJ=<dir> sim/mips_soc/build.sh soc;  make -C lang/mips/test vi32_test viaa_test
#   picture_soc.sh <dir>/tb_soc <work dir>
# (viaa_check.py's model has the next row's first pixel off the picture, the scan-out reads it: the last column differs in 41
# pixels with the options off as well, so --skip-last-column.)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; T="$HERE/../../lang/mips/test/build"
TB="$1"; W="$2"; mkdir -p "$W"; head -c 65536 /dev/zero > "$W/dummy.rom"
n=0; fail=0
for opt in 0 1 2 3; do
    flags=""; [ $((opt & 1)) = 1 ] && flags="$flags --out32"; [ $((opt & 2)) = 2 ] && flags="$flags --deblur"
    for t in vi32 viaa; do
        d="$W/${t}_$opt"; rm -rf "$d"; mkdir -p "$d"
        ( cd "$d" && ROM="$W/dummy.rom" INTERACT=$([ $opt = 0 ] || echo "0x10000144:$opt") VGA=1 "$TB" "$T/${t}_test.bin" -c 30 -o "$d" -e 1 > log.txt 2>&1 ) &
    done
done
wait
for opt in 0 1 2 3; do
    flags=""; [ $((opt & 1)) = 1 ] && flags="$flags --out32"; [ $((opt & 2)) = 2 ] && flags="$flags --deblur"
    f=$(ls "$W/vi32_$opt"/v*.ppm | tail -1); n=$((n+1))
    out=$(python3 "$HERE/vi32_check.py" "$f" $flags | head -1); case "$out" in PASS*) ;; *) echo "FAIL: vi32_test option $opt:$flags: $out"; fail=$((fail+1));; esac
    f=$(ls "$W/viaa_$opt"/v*.ppm | tail -1); n=$((n+1))
    out=$(python3 "$HERE/viaa_check.py" "$f" --skip-last-column $flags | head -1)
    case "$out" in PASS*) ;; *) echo "FAIL: viaa_test option $opt:$flags: $out"; fail=$((fail+1));; esac
done
echo "picture_soc: $n checks, $fail failed"
exit $fail
