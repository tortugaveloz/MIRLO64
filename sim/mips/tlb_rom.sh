#!/bin/bash
# tlb_rom.sh OUT.z64 : builds sim/mips/tlb_rom.c as an N64 ROM for the lockstep bench (MIPS_CC / MIPS_BIN as lang/mips/mips.mk)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
CC="${MIPS_CC:-$HOME/local/mipsgcc/usr/bin/mips-linux-gnu-gcc-13}"; BIN="${MIPS_BIN:-$HOME/local/mips-bin/mips-linux-gnu-}"
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
for t in as ld; do ln -sf "$(command -v ${BIN}$t)" "$T/$t"; done
cat > "$T/rom.ld" <<'LD'
ENTRY(_start)
SECTIONS {
    . = 0x80000400;
    .text : { *(.text.start) *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
}
LD
"$CC" -B"$T/" -mabi=32 -mno-abicalls -fno-pic -G0 -ffreestanding -nostdlib -EB -march=vr4300 -msoft-float -O1 -fno-strict-aliasing \
    -Wall -Wno-pointer-to-int-cast -c "$HERE/tlb_rom.c" -o "$T/rom.o"
"${BIN}ld" -EB -T "$T/rom.ld" -o "$T/rom.elf" "$T/rom.o"
"${BIN}objcopy" -I elf32-tradbigmips -O binary "$T/rom.elf" "$T/rom.bin"
"${BIN}nm" "$T/rom.elf" | sort > "${1%.z64}.syms"; cp "$T/rom.elf" "${1%.z64}.elf"
python3 - "$T/rom.bin" "$1" <<'PY'
import struct, sys
code = open(sys.argv[1], "rb").read()
rom = bytearray(0x101000 + 0x100000)
struct.pack_into(">I", rom, 0, 0x80371240)
struct.pack_into(">I", rom, 8, 0x80000400)
rom[0x20:0x34] = b"MIRLO TLB TEST      "
assert len(code) < 0xE0000, len(code)
rom[0x1000:0x1000 + len(code)] = code
open(sys.argv[2], "wb").write(rom)
print(sys.argv[2], len(code), "bytes of code")
PY
