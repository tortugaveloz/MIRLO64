#!/bin/bash
# Runs the SAME GDLs through the host build of geom_pipeline.c and through
# the real geom-core RTL (CPU + CFU) executing lang/c/geom/build/geom.bin,
# and requires both RDP command streams, and the frames the RDP C model
# draws from them, to be bit-identical. Also prints
# cycles per GDL. Run ./build.sh once for the Verilator model, and rebuild
# geom.bin (make clean && make in lang/c/geom -- its Makefile does not track
# header dependencies). The host side is rebuilt here on every run: comparing
# against a stale tools/hostreplay once produced a false mismatch.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/work"; mkdir -p "$OUT"
CF="-O2 -w -std=gnu11 -fno-strict-aliasing -m64 -fcommon -ffp-contract=off -fsigned-char \
    -DGEOM_HOST_TEST -DGEOM_FB_HRES=268 -DGEOM_FB_VRES=240"
INC="-I$R/lang/c/geom -I$R/lang/c/game"
CF="$CF -DGEOM_RDP"; MSRC="$R/lang/c/rdp/rdp.c"
# shellcheck disable=SC2086
gcc $CF $INC -o "$HERE/tools/hostreplay" "$HERE/tools/hostreplay.c" \
    "$R/lang/c/geom/geom_pipeline.c" "$R/lang/c/geom/geom_triangle.c" \
    "$R/lang/c/geom/geom_fixed.c" "$R/lang/c/geom/geom_vecmath.c" $MSRC -lm || exit 1
[ "$R/lang/c/geom/build/geom.bin" -nt "$R/lang/c/geom/geom_pipeline.c" ] \
    || echo "WARNING: geom.bin older than geom_pipeline.c -- rebuild it first"
cd "$OUT"
fail=0
#            lit nlights tris fog tex spread seed   (see tools/mkgdl.c)
for cfg in "0 1 1 0 0 1 7" "1 1 1 0 0 1 7" "1 5 1 0 0 1 7" "1 2 1 1 0 1 7" \
           "0 1 1 0 1 1 7" "1 1 1 1 1 5 3" "1 2 1 1 1 30 1" "0 1 1 0 1 30 2"; do
    set -- $cfg; g="l$1_n$2_t$3_f$4_x$5_s$6_r$7"
    "$HERE/tools/mkgdl" "$@" > "gdl_$g.bin"
    HOSTREPLAY_FB="hostfb_$g.bin" "$HERE/tools/hostreplay" "gdl_$g.bin" > "host_$g.txt" 2>/dev/null
    cyc=$(TB_FB="rtlfb_$g.bin" "$HERE/obj_dir/tb_geom_cpu" "$R/lang/c/geom/build/geom.bin" "gdl_$g.bin" 2>/dev/null \
          | sed -n 's/.*signalled done after \([0-9]*\) cycles.*/\1/p')
    cp geom_cmd_stream.bin "rtl_$g.bin"
    printf '%-22s %9s cycles  ' "$g" "${cyc:-TIMEOUT}"
    python3 "$HERE/tools/cmp_rdp.py" "$g" || fail=1
done
exit $fail
