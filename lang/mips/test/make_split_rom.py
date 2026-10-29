#!/usr/bin/env python3
"""Generate the 64 MiB synthetic ROM used by the PSRAM split tests."""
import struct
import sys

if len(sys.argv) != 2:
    sys.exit('usage: make_split_rom.py OUTPUT')
words = {
    0x00000000: 0x11223344,
    0x00dffffc: 0x81a2b3c4,
    0x00e00000: 0x92b3c4d5,
    0x01dffffc: 0x55667788,
    0x01e00000: 0x99aabbcc,
    0x02000000: 0x12345678,
    0x02fffffc: 0x55667788,
    0x03000000: 0x99aabbcc,
    0x036ffffc: 0x01234567,
    0x03700000: 0x89abcdef,
    0x03fffffc: 0xdeadbeef,
}
with open(sys.argv[1], 'wb') as out:
    out.truncate(0x04000000)
    for offset, word in words.items():
        out.seek(offset)
        out.write(struct.pack('>I', word))
