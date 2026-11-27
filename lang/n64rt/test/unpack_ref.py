#!/usr/bin/env python3
"""Host reference of a libdragon compressed ELF (n64elfcompress): the decompressors of its three levels as its own library has
them (src/compress/lz4_dec.c, aplib_dec.c, shrinkler_dec.c, written bit by bit and byte by byte as they are, with no thought of
speed), the ELF found in a ROM as its IPL3 finds it, and what that IPL3 leaves in the RDRAM: the image the runtime's boot must
make (n64rt_boot.c load_elf).

  unpack_ref.py info ROM...              the ELF's place, entry, segments and the codec of its decompression function
  unpack_ref.py image ROM OUT [SIZE]     the RDRAM the IPL3 leaves (SIZE bytes, 8 MiB by default), as a file
  unpack_ref.py segs ROM DIR             each compressed segment: DIR/N.cmp (its stream), N.plain (its program), N.info (kind, margin)
"""
import struct, sys, os

PT_LOAD, PT_N64_DECOMP, PF_COMPRESSED = 1, 0x64E36341, 0x1000

def lz4(src):
    out = bytearray(); i = 0; n = len(src)
    while i < n:
        t = src[i]; i += 1
        l = t >> 4
        if l == 15:
            while True:
                b = src[i]; i += 1; l += b
                if b != 255: break
        out += src[i:i + l]; i += l
        if i + 2 > n: break
        off = src[i] | (src[i + 1] << 8); i += 2
        m = t & 15
        if m == 15:
            while True:
                b = src[i]; i += 1; m += b
                if b != 255: break
        m += 4
        assert 0 < off <= len(out)
        for _ in range(m): out.append(out[-off])
    return out

def aplib(src):
    out = bytearray(); p = 0; sh = -1; cc = 0
    def rb():
        nonlocal p
        v = src[p]; p += 1; return v
    def bit():
        nonlocal sh, cc
        if sh < 0: sh = 7; cc = rb()
        v = (cc >> sh) & 1; sh -= 1; return v
    def g2():
        v = 1
        while True:
            v = (v << 1) | bit()
            if not bit(): break
        return v
    nlit = 3; mo = -1
    out.append(rb())
    while True:
        if not bit():
            out.append(rb()); nlit = 3; continue
        if not bit():
            hi = g2() - nlit
            if hi >= 0:
                mo = (hi << 8) | rb(); ml = g2()
                if mo < 128 or mo >= 32000: ml += 2
                elif mo >= 1280: ml += 1
            else:
                ml = g2()
        elif not bit():
            c = rb()
            if c == 0: break
            mo = c >> 1
            for _ in range(2 + (c & 1)): out.append(out[-mo])
            nlit = 2; continue
        else:
            m = 0
            for k in (3, 2, 1, 0): m |= bit() << k
            nlit = 3
            out.append(out[-m] if m else 0); continue
        for _ in range(ml): out.append(out[-mo])
        nlit = 2
    return out

def shrinkler(src):
    M64 = (1 << 64) - 1
    src = bytes(src) + bytes(8)                  # (the decoder reads whole words: past the end the RDRAM is zero)
    ctxs = [0x8000] * (1 + 4 * 256)
    pos = 0
    def r32():
        nonlocal pos
        v = struct.unpack('>I', bytes(src[pos:pos + 4]).ljust(4, b'\0'))[0]; pos += 4; return v
    iv = 0
    for _ in range(4):
        iv = (iv << 8) | src[pos]; pos += 1
    iv = (iv << 31) & M64
    bl = 1; isz = 0x8000
    def dbit(ci):
        nonlocal iv, bl, isz
        while isz < 0x8000:
            if bl == 0:
                iv |= r32(); bl = 32
            bl -= 1; isz <<= 1; iv = (iv << 1) & M64
        prob = ctxs[ci]; v = iv >> 48; th = (isz * prob) >> 16
        if v >= th:
            iv -= th << 48; isz -= th; ctxs[ci] = prob - (prob >> 4); return 0
        isz = th; ctxs[ci] = prob + (0xffff >> 4) - (prob >> 4); return 1
    def dnum(base):
        i = 0
        while dbit(base + i * 2 + 2): i += 1
        n = 1
        while i >= 0:
            n = (n << 1) | dbit(base + i * 2 + 1); i -= 1
        return n
    out = bytearray(); ref = False; prev = False; off = 0
    while True:
        if ref:
            rep = False
            if not prev: rep = dbit(0)
            if not rep:
                off = dnum(1 + (2 << 8)) - 2
                if off == 0: break
            ln = dnum(1 + (3 << 8)); prev = True
            for _ in range(ln): out.append(out[-off])
        else:
            par = len(out) & 1; c = 1
            for _ in range(8): c = (c << 1) | dbit(1 + ((par << 8) | c))
            out.append(c & 255); prev = False
        ref = bool(dbit(1 + ((len(out) & 1) << 8)))
    return out

DEC = {1: lz4, 2: aplib, 3: shrinkler}

def kind_of(stub):
    """Which level a decompression function is, by the constants of its instructions (n64rt_unpack_kind in the runtime)."""
    f = set()
    for i in range(0, len(stub) - 3, 4):
        w = struct.unpack('>I', stub[i:i + 4])[0]
        if 8 <= (w >> 26) <= 14: f.add(w & 0xFFFF)
    if 32000 in f and 1280 in f: return 2
    if 1026 in f and 1538 in f: return 3
    if 255 in f: return 1
    return 0

def find_elf(d):
    for o in range(0x1000, len(d) - 0x40 + 1, 0x100):
        if d[o:o + 4] == b'\x7fELF': return o
    return None

def parse(d):
    o = find_elf(d)
    if o is None: return None
    e64 = d[o + 4] == 2
    phsz = 0x38 if e64 else 0x20
    phoff = struct.unpack('>I', d[o + (0x24 if e64 else 0x1c):][:4])[0]
    phnum = struct.unpack('>H', d[o + (0x38 if e64 else 0x2c):][:2])[0]
    ent = struct.unpack('>I', d[o + (0x1c if e64 else 0x18):][:4])[0]
    segs = []
    for i in range(phnum):
        q = o + phoff + i * phsz
        if e64:
            t, fl = struct.unpack('>II', d[q:q + 8]); off, va, pa, fs, ms = [struct.unpack('>I', d[q + k:q + k + 4])[0] for k in (0x14, 0x1c, 0x24, 0x2c, 0x34)]
        else:
            t, off, va, pa, fs, ms, fl, al = struct.unpack('>8I', d[q:q + 32])
        segs.append(dict(type=t, off=off, vaddr=va, paddr=pa, filesz=fs, memsz=ms, flags=fl))
    return o, ent, segs

def image(d, memsize=8 << 20):
    """The RDRAM at the entry, as libdragon's loader leaves it (boot/loader.c stage2, below the reserved area it clears)."""
    o, ent, segs = parse(d)
    ram = bytearray(memsize)
    kind = 0
    for s in segs:
        va, off, sz = s['vaddr'], s['off'], s['filesz']
        if s['type'] == PT_N64_DECOMP:
            kind = kind_of(d[o + off:o + off + sz])
            if va == 0: continue                     # (put in the stack, not in the game's memory)
        elif s['type'] != PT_LOAD: continue
        if not sz: continue
        a = va & 0x1FFFFFFF
        dma = sz + (sz & 1)
        ram[a:a + dma] = d[o + off:o + off + dma]
        if s['flags'] & PF_COMPRESSED:
            pa = s['paddr'] & 0x1FFFFFFF
            out = DEC[kind](bytes(ram[a:a + sz]))
            ram[pa:pa + len(out)] = out
            pe, ve = pa + len(out), a + sz
            if a < pa: lo, hi = a, min(ve, pa)
            elif ve > pe: lo, hi = max(a, pe), ve
            else: lo = hi = 0
            ram[lo:hi] = bytes(hi - lo)
    return ent, ram

def main(argv):
    if len(argv) < 3: print(__doc__); return 2
    cmd = argv[1]
    if cmd == 'info':
        for p in argv[2:]:
            d = open(p, 'rb').read()
            r = parse(d)
            if not r: print(p, ': no ELF'); continue
            o, ent, segs = r
            kind = 0
            for s in segs:
                if s['type'] == PT_N64_DECOMP: kind = kind_of(d[o + s['off']:o + s['off'] + s['filesz']])
            print('%s: ELF at %#x, entry %#x, %s' % (p, o, ent, 'level %d' % kind if kind else 'not compressed'))
            for s in segs:
                print('   type %08x off %#x vaddr %08x paddr %08x filesz %#x memsz %#x flags %#x' % (s['type'], s['off'], s['vaddr'], s['paddr'], s['filesz'], s['memsz'], s['flags']))
    elif cmd == 'image':
        d = open(argv[2], 'rb').read()
        ent, ram = image(d, int(argv[4], 0) if len(argv) > 4 else 8 << 20)
        open(argv[3], 'wb').write(ram); print('entry %#x' % ent)
    elif cmd == 'segs':
        d = open(argv[2], 'rb').read(); os.makedirs(argv[3], exist_ok=True)
        o, ent, segs = parse(d); kind = 0
        for i, s in enumerate(segs):
            if s['type'] == PT_N64_DECOMP: kind = kind_of(d[o + s['off']:o + s['off'] + s['filesz']])
            if s['type'] == PT_LOAD and s['flags'] & PF_COMPRESSED:
                cmp = d[o + s['off']:o + s['off'] + s['filesz']]
                open('%s/%d.cmp' % (argv[3], i), 'wb').write(cmp)
                open('%s/%d.plain' % (argv[3], i), 'wb').write(DEC[kind](cmp))
                open('%s/%d.info' % (argv[3], i), 'w').write('%d %d\n' % (kind, s['vaddr'] - s['paddr']))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
