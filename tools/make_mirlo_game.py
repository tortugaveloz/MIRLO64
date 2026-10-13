#!/usr/bin/env python3
"""Pack a Mirlo game file: the program image, data blocks, and a trailer.

    tools/make_mirlo_game.py [--video 268|320] <program.bin> <out.bin> TAG=file [TAG=file ...]
    e.g. tools/make_mirlo_game.py build/build.bin "My Game.bin" SND0=sound.bin

TAG@OFFSET=file puts a block at that offset in the file (zero padding up to
it), i.e. at 0x40000000 + OFFSET in SDRAM once the Pocket has loaded it --
where a game that uses the copy the Pocket loaded wants it. That holds for
a file's first 49 MiB: what it has from there up, to 65 MiB at most, is not
in the SDRAM (docs/mips.md, "The memory of an N64 game").

The Pocket's file browser lists the .bin files in Assets/n64/common. The
Pocket loads the file to 0x40000000 (data slot 0) and the boot ROM
(lang/mips/boot) jumps to it; the game reads its blocks itself, by tag,
through the APF bridge. File layout:

  program | blocks (each on a 512-byte boundary) | block table | footer

Block table: one 16-byte entry per block, big-endian (the game CPU's order):
  [0] tag (4 ASCII chars)  [4] offset in the file  [8] size  [12] 0
Footer, the last 32 bytes of the file, big-endian:
  [0]  program bytes   [4] block count   [8] block-table offset
  [12] version = 1     [16] b"MIRLODATA\\0"
  [26] video mode: 0 = 268 x 240 (the default, and every older file),
       1 = 320 x 240 (lang/c/game/frame.c frame_set_video)
  [27] 5 zero bytes
A file without the footer is loaded whole as the program.
"""
import struct
import sys

ALIGN = 512

def pad(b, a):
    return b + b"\0" * ((-len(b)) % a)

def main():
    args = sys.argv[1:]
    video = 0
    if args[:1] == ["--video"]:
        assert args[1] in ("268", "320"), "--video 268 or 320"
        video = 1 if args[1] == "320" else 0
        args = args[2:]
    if len(args) < 2:
        sys.exit(__doc__)
    prog_path, out_path = args[0], args[1]
    body = pad(open(prog_path, "rb").read(), 4)
    prog_len = len(body)
    table = b""
    for spec in args[2:]:
        tag, path = spec.split("=", 1)
        at = None
        if "@" in tag:
            tag, at = tag.split("@", 1)
            at = int(at, 0)
        assert len(tag) == 4 and tag.isascii(), f"tag must be 4 ASCII chars: {tag}"
        data = open(path, "rb").read()
        body = pad(body, ALIGN)
        if at is not None:
            assert at % ALIGN == 0 and at >= len(body), f"block {tag}: offset {at:#x} is not free"
            body += b"\0" * (at - len(body))
        table += struct.pack(">4sIII", tag.encode(), len(body), len(data), 0)
        print(f"  block {tag}: {len(data)} B at {len(body)}")
        body += data
    body = pad(body, 4)
    table_off = len(body)
    nblocks = len(table) // 16
    footer = struct.pack(">IIII", prog_len, nblocks, table_off, 1) + b"MIRLODATA\0" + bytes([video]) + b"\0" * 5
    assert len(footer) == 32
    open(out_path, "wb").write(body + table + footer)
    print(f"{out_path}: program {prog_len} B, {nblocks} block(s), {len(body) + len(table) + 32} B, "
          f"video {'320' if video else '268'} x 240")

if __name__ == "__main__":
    main()
