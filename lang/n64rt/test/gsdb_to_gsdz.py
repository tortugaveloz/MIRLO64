#!/usr/bin/env python3
# gsdb_to_gsdz.py <in.gsdb> <out.gsdz>: a format-1 Game Shark cheat database (n64rt_gsdb.h) as format 2, one zlib stream per game (RFC 1950, level 9; the Game Shark database project's
# docs/format-v2.md): for the tests' small databases; the real one is built by that project's tool (tools/build_db_libretro.py --format 2).
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read()
magic, ver, n, ib = struct.unpack('>4sIII', d[:16])
assert magic == b'GSDB' and ver == 1 and ib == 16 * n
ents = []
for i in range(n):
    e = d[16 + 16 * i: 32 + 16 * i]
    crc1, crc2, off = struct.unpack('>III', e[:12]); country = e[12]; ln = int.from_bytes(e[13:16], 'big')
    ents.append((crc1, crc2, country, d[off:off + ln]))
ents.sort(key=lambda e: e[:3])
body = bytearray(); idx = bytearray(); base = 24 + 20 * n; maxu = 0
for crc1, crc2, country, blk in ents:
    z = zlib.compress(blk, 9); maxu = max(maxu, len(blk))
    idx += struct.pack('>IIIHHB3x', crc1, crc2, base + len(body), len(z), len(blk), country)
    body += z
out = struct.pack('>4sIIIII', b'GSDB', 2, n, 20 * n, maxu, base + len(body)) + idx + body
open(sys.argv[2], 'wb').write(out)
print('%d games, %d -> %d bytes' % (n, len(d), len(out)))
