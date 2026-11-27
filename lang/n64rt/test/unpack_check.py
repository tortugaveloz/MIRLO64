#!/usr/bin/env python3
"""The runtime's decompressors of a libdragon compressed ELF (n64rt_unpack.c) against the library's own, on the host.

  unpack_check.py [--rawcomp PATH] [--tmp DIR] [ROM ...]

  * the host reference (unpack_ref.py: the library's C decoders as Python) is checked against streams the library's COMPRESSORS make
    (rawcomp, a few lines on tools/common/assetcomp.a of a libdragon checkout: `rawcomp LEVEL in out` writes the raw stream
    asset_compress_mem_raw makes for a segment and prints "size margin"; without --rawcomp only the ROMs' own streams are tested);
  * unpack_host.c (the C that the runtime runs, under ASAN and UBSAN) on those streams, out of place and in place, with a short
    buffer and with damaged streams (see it);
  * the streams: synthetic programs -- a byte, three, the lengths around the 15 / 255 / 270 steps of a length's extra bytes, a long
    run of zeros (offset 1: the overlapping copy), a short period, random bytes (not compressible), text, a mix with matches of
    offsets up to 64 KiB - 1, the 4-bit / 7-bit / 8+ bit offsets of aPLib -- at every level; and each compressed segment of each ROM
    given: its own stream (level 1: what the ROM has), and the program of it compressed again at the three levels.
"""
import os, random, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import unpack_ref as ref

HERE = os.path.dirname(os.path.abspath(__file__))
RT = os.path.dirname(HERE)

def text(n, seed):
    r = random.Random(seed)
    words = [bytes(r.choices(b'abcdefghijklmnopqrstuvwxyz', k=r.randint(2, 9))) for _ in range(400)]
    out = bytearray()
    while len(out) < n: out += r.choice(words) + b' '
    return bytes(out[:n])

def synthetic():
    r = random.Random(1)
    yield 'one byte', b'x'
    yield 'three bytes', b'abc'
    yield 'a zero', b'\0'
    yield 'zeros 20', bytes(20)
    for n in (14, 15, 16, 17, 18, 19, 20, 269, 270, 271, 272, 524, 525, 526, 780):          # around the extra length bytes (15 + 255 k)
        yield 'zeros %d' % n, bytes(n)
        yield 'lit %d then zeros' % n, bytes(r.randrange(256) for _ in range(n)) + bytes(40)
        yield 'abcd*%d' % n, (b'abcd' * n)[:n * 2]
    yield 'zeros 1 MiB', bytes(1 << 20)
    yield 'abc period 200000', b'abc' * 200000
    yield 'random 1', bytes(r.randrange(256) for _ in range(1))
    yield 'random 70000', bytes(r.randrange(256) for _ in range(70000))
    yield 'random 100 + same 100', (lambda b: b + b)(bytes(r.randrange(256) for _ in range(100)))
    yield 'text 300000', text(300000, 2)
    yield 'text 40', text(40, 3)
    # offsets: 1..16 (aPLib's 4-bit), up to 127 (7-bit), 128..1279, 1280..31999, 32000..65535 and the LZ4 maximum 65535
    for off in (1, 2, 3, 7, 8, 15, 16, 17, 100, 127, 128, 129, 1279, 1280, 1281, 31999, 32000, 32001, 65534, 65535, 65536, 70000):
        blk = bytes(r.randrange(256) for _ in range(off))
        yield 'match at offset %d' % off, blk + blk[:min(off, 300)] + bytes(r.randrange(256) for _ in range(5)) + blk[:min(off, 300)]
    mix = bytearray()
    for _ in range(3000):
        k = r.random()
        if k < 0.3 and len(mix) > 10: s = r.randrange(len(mix)); mix += mix[s:s + r.randint(2, 400)]
        elif k < 0.6: mix += text(r.randint(1, 60), r.random())
        elif k < 0.8: mix += bytes([r.randrange(256)]) * r.randint(1, 50)
        else: mix += bytes(r.randrange(256) for _ in range(r.randint(1, 30)))
    yield 'mix %d' % len(mix), bytes(mix)

def main():
    a = sys.argv[1:]
    rawcomp = None; tmp = None
    while a and a[0].startswith('--'):
        if a[0] == '--rawcomp': rawcomp = a[1]
        elif a[0] == '--tmp': tmp = a[1]
        a = a[2:]
    tmp = tmp or tempfile.mkdtemp(prefix='unpack_check.')
    os.makedirs(tmp, exist_ok=True)
    exe = os.path.join(tmp, 'unpack_host')
    subprocess.check_call(['gcc', '-O1', '-g', '-Wall', '-Wextra', '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-I' + RT,
                           os.path.join(HERE, 'unpack_host.c'), os.path.join(RT, 'n64rt_unpack.c'), '-o', exe])
    fails = 0; n = 0
    def run(kind, cmp, plain, margin, what):
        nonlocal fails, n
        cf, pf = os.path.join(tmp, 'c.bin'), os.path.join(tmp, 'p.bin')
        open(cf, 'wb').write(cmp); open(pf, 'wb').write(plain)
        r = subprocess.run([exe, str(kind), cf, pf, str(margin)], capture_output=True, text=True)
        n += 1
        if r.returncode:
            fails += 1; print('FAIL level %d, %s:\n%s%s' % (kind, what, r.stdout[-600:], r.stderr[-1500:]))
    def reference(kind, cmp, plain, what):                 # the host reference against the library's own stream
        nonlocal fails
        if bytes(ref.DEC[kind](cmp)) != plain:
            fails += 1; print('FAIL: the host reference, level %d, %s' % (kind, what))
    def compress(kind, plain):
        pf, cf = os.path.join(tmp, 'in.bin'), os.path.join(tmp, 'out.bin')
        open(pf, 'wb').write(plain)
        o = subprocess.check_output([rawcomp, str(kind), pf, cf], text=True).split()
        return open(cf, 'rb').read(), int(o[1])
    for rom in a:
        d = open(rom, 'rb').read()
        r = ref.parse(d)
        if not r: print(rom, ': no ELF'); continue
        o, ent, segs = r
        kind = 0
        for i, s in enumerate(segs):
            if s['type'] == ref.PT_N64_DECOMP: kind = ref.kind_of(d[o + s['off']:o + s['off'] + s['filesz']])
            if s['type'] != ref.PT_LOAD or not s['flags'] & ref.PF_COMPRESSED: continue
            cmp = d[o + s['off']:o + s['off'] + s['filesz']]
            plain = bytes(ref.DEC[kind](cmp))
            what = '%s segment %d' % (os.path.basename(rom), i)
            margin = max(0, s['vaddr'] - s['paddr'] - (len(plain) - len(cmp)) - 8)
            run(kind, cmp, plain, margin, what + ' as the ROM has it')
            if rawcomp:
                for k in (1, 2, 3):
                    c, m = compress(k, plain)
                    reference(k, c, plain, what + ' recompressed')
                    run(k, c, plain, m, what + ' recompressed')
    if rawcomp:
        for name, plain in synthetic():
            for k in (1, 2, 3):
                c, m = compress(k, plain)
                reference(k, c, plain, name)
                run(k, c, plain, m, name)
    print('unpack_check: %d cases, %d failures' % (n, fails))
    return 1 if fails else 0

if __name__ == '__main__':
    sys.exit(main())
