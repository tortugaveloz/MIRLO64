#!/bin/bash
# elf_unpack_check.sh OBJ_DIR ROM... -- a ROM whose program is a libdragon ELF (compressed by n64elfcompress at any level, or not) booted
# on the whole SoC (a tb_soc built by build.sh soc) with lang/n64rt/build/n64rt.bin: the RDRAM when the game CPU first retires at the ELF's
# entry (tb_soc's RAMDUMP_AT) is compared byte for byte with what the IPL3 leaves, as the host reference makes it
# (lang/n64rt/test/unpack_ref.py: the library's decoders, bit by bit). The four bytes at 0x1A4, the random word the boot leaves (the
# boot's own timing), are the only ones allowed to differ. Prints the entry's time in SoC cycles and the segments' unpack times
# (CP0 Count, 46.875 MHz) for each. CYC=<Mcycles> (default 400: a Shrinkler program of 600 KB takes some 110 M), OUT=<dir>, RT=<runtime>.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; R="$(cd "$HERE/../.." && pwd)"
OBJ="${1:?usage: elf_unpack_check.sh OBJ_DIR ROM...}"; shift
O="${OUT:-/tmp/elf_unpack_check}"; RT="${RT:-$R/lang/n64rt/build/n64rt.bin}"; C="${CYC:-400}"
mkdir -p "$O"; rc=0
for rom in "$@"; do
    tag=$(basename "$rom" .z64)
    ( cd "$HERE" && ROM="$rom" RAMDUMP="$O/$tag.ram" RAMDUMP_AT=$(python3 "$R/lang/n64rt/test/unpack_ref.py" info "$rom" | sed -n 's/.*entry 0x\([0-9a-f]*\),.*/\1/p' | head -1) \
        "$OBJ/tb_soc" "$RT" -c "$C" -o "$O" > "$O/$tag.log" 2>&1 )
    ent=$(python3 "$R/lang/n64rt/test/unpack_ref.py" image "$rom" "$O/$tag.ref" | sed 's/entry //')
    res=$(python3 - "$O/$tag.ram" "$O/$tag.ref" <<'EOF'
import sys
a = open(sys.argv[1], 'rb').read() if __import__('os').path.exists(sys.argv[1]) else b''
b = open(sys.argv[2], 'rb').read()
if len(a) != len(b): print('NO DUMP: the game CPU did not reach its entry'); sys.exit(1)
d = [i for i in range(len(a)) if a[i] != b[i] and not 0x1A4 <= i < 0x1A8]
print('RDRAM identical (but the random word)' if not d else 'RDRAM DIFFERS at %d bytes, the first %s' % (len(d), [hex(x) for x in d[:6]]))
sys.exit(1 if d else 0)
EOF
) || rc=1
    when=$(sed -n 's/.*RAMDUMP_AT: the game CPU at \([0-9a-f]*\) after \([0-9]*\) sys.*/\2/p' "$O/$tag.log" | head -1)
    segs=$(sed -n 's/.*level \([0-9]\), unpacked to \([0-9a-f]*\): \([0-9]*\) bytes (CP0 Count: \([0-9]*\)).*/L\1 \3 bytes \4 ticks;/p' "$O/$tag.log" | tr '\n' ' ')
    echo "$tag: entry $ent after ${when:-?} SoC cycles: $res  [$segs]"
    rm -f "$O/$tag.ram" "$O/$tag.ref"
done
exit $rc
