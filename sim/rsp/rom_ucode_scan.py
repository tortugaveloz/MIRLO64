#!/usr/bin/env python3
"""Which RSP instructions do a ROM's microcodes use, and does rtl/rsp have them?

    rom_ucode_scan.py [-v] [--check] [--yaml splat.yaml] ROM.z64|.v64|.n64|.zip ...
        -v       each region found
        --check  decode every vector instruction with rabbitizer too
        --yaml   a splat YAML of the ROM: the segment each region falls in

Finds the RSP microcode in each ROM statically -- in the ROM itself and in
its Yaz0 / Yay0 / MIO0 blocks (where some games keep their code) -- and
counts every instruction in it by kind. RSP code is told from the rest by
its vector instructions: the main CPU's code never has COP2, LWC2 or SWC2.
A static scan sees more than what runs (dead code, a word of data that
decodes), never less: an instruction it does not list is not in the ROM.

The lists below are what rtl/rsp/rsp_vu.sv and rsp_su.sv implement; keep them
in step with the RTL.
"""
import sys
import zipfile
import struct

VEC_NAMES = {
    0x00: "VMULF", 0x01: "VMULU", 0x02: "VRNDP", 0x03: "VMULQ", 0x04: "VMUDL", 0x05: "VMUDM", 0x06: "VMUDN", 0x07: "VMUDH",
    0x08: "VMACF", 0x09: "VMACU", 0x0A: "VRNDN", 0x0B: "VMACQ", 0x0C: "VMADL", 0x0D: "VMADM", 0x0E: "VMADN", 0x0F: "VMADH",
    0x10: "VADD", 0x11: "VSUB", 0x13: "VABS", 0x14: "VADDC", 0x15: "VSUBC", 0x1D: "VSAR",
    0x20: "VLT", 0x21: "VEQ", 0x22: "VNE", 0x23: "VGE", 0x24: "VCL", 0x25: "VCH", 0x26: "VCR", 0x27: "VMRG",
    0x28: "VAND", 0x29: "VNAND", 0x2A: "VOR", 0x2B: "VNOR", 0x2C: "VXOR", 0x2D: "VNXOR",
    0x30: "VRCP", 0x31: "VRCPL", 0x32: "VRCPH", 0x33: "VMOV", 0x34: "VRSQ", 0x35: "VRSQL", 0x36: "VRSQH", 0x37: "VNOP", 0x3F: "VNULL",
}
LD = ["LBV", "LSV", "LLV", "LDV", "LQV", "LRV", "LPV", "LUV", "LHV", "LFV", "LWV", "LTV"]
ST = ["SBV", "SSV", "SLV", "SDV", "SQV", "SRV", "SPV", "SUV", "SHV", "SFV", "SWV", "STV"]

# ---- what the RTL implements
VEC_OK = set(VEC_NAMES) - {0x02, 0x03, 0x0A, 0x0B}
LD_OK = set(range(8)) | {11}                # LBV..LUV, LTV
ST_OK = LD_OK | {8, 9}                     # SBV..SUV, SHV, SFV (an element a multiple of 4), STV
SPECIAL_OK = {0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B}
REGIMM_OK = {0x00, 0x01, 0x10, 0x11}
OP_OK = {0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
         0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2B}


def classify(w):
    """(kind, supported, plausible RSP instruction, vector instruction)"""
    op = w >> 26
    if w == 0:
        return "NOP", True, True, False
    if op == 0x00:
        fn = w & 63
        return f"SPECIAL fn {fn:02x}", fn in SPECIAL_OK, fn in SPECIAL_OK, False
    if op == 0x01:
        rt = (w >> 16) & 31
        return f"REGIMM rt {rt:02x}", rt in REGIMM_OK, rt in REGIMM_OK, False
    if op == 0x10:
        rs = (w >> 21) & 31
        ok = rs in (0, 4) and (w & 0x7FF) == 0
        return ("MFC0" if rs == 0 else "MTC0" if rs == 4 else f"COP0 rs {rs:02x}"), ok, ok, False
    if op == 0x12:
        if w & (1 << 25):
            fn = w & 63
            return VEC_NAMES.get(fn, f"COP2 reserved fn {fn:02x}"), fn in VEC_OK, fn in VEC_NAMES, True
        rs = (w >> 21) & 31
        name = {0: "MFC2", 2: "CFC2", 4: "MTC2", 6: "CTC2"}.get(rs)
        ok = name is not None and (w & 0x7F) == 0
        return name or f"COP2 rs {rs:02x}", ok, ok, ok
    if op in (0x32, 0x3A):
        sub = (w >> 11) & 31
        names = LD if op == 0x32 else ST
        if sub < 12:
            ok = sub in (LD_OK if op == 0x32 else ST_OK)
            if op == 0x3A and sub == 9 and (w >> 7) & 3:
                return "SFV (element not a multiple of 4)", False, True, True
            return names[sub], ok, True, True
        return f"{'LWC2' if op == 0x32 else 'SWC2'} {sub}", False, False, False
    if op in OP_OK:
        return f"op {op:02x}", True, True, False
    return f"op {op:02x}", False, False, False


# ---- the compressed blocks
def yaz0(d, o):
    size = struct.unpack(">I", d[o + 4:o + 8])[0]
    if size > 32 << 20:
        return None
    out = bytearray()
    p = o + 16
    try:
        while len(out) < size:
            code = d[p]; p += 1
            for bit in range(7, -1, -1):
                if len(out) >= size:
                    break
                if code >> bit & 1:
                    out.append(d[p]); p += 1
                else:
                    b0, b1 = d[p], d[p + 1]; p += 2
                    dist = ((b0 & 0xF) << 8 | b1) + 1
                    n = b0 >> 4
                    if n == 0:
                        n = d[p] + 0x12; p += 1
                    else:
                        n += 2
                    for _ in range(n):
                        out.append(out[-dist])
    except IndexError:
        return None
    return bytes(out[:size])


def mio0(d, o, yay=False):
    size, a, b = struct.unpack(">III", d[o + 4:o + 16])
    if size > 32 << 20 or a > len(d) or b > len(d):
        return None
    out = bytearray()
    ctl = o + 16
    cp, up = o + a, o + b                   # MIO0: compressed pairs, uncompressed bytes; Yay0: link table, chunks
    try:
        while len(out) < size:
            code = struct.unpack(">I", d[ctl:ctl + 4])[0]; ctl += 4
            for bit in range(31, -1, -1):
                if len(out) >= size:
                    break
                if code >> bit & 1:
                    out.append(d[up]); up += 1
                else:
                    v = d[cp] << 8 | d[cp + 1]; cp += 2
                    dist = (v & 0xFFF) + 1
                    n = v >> 12
                    if yay:
                        if n == 0:
                            n = d[up] + 0x12; up += 1
                        else:
                            n += 2
                    else:
                        n += 3
                    for _ in range(n):
                        out.append(out[-dist])
    except (IndexError, struct.error):
        return None
    return bytes(out[:size])


def blobs(rom):
    """the ROM, and each compressed block in it, as (name, bytes)"""
    yield "rom", rom
    for magic, fn in ((b"Yaz0", yaz0), (b"MIO0", mio0), (b"Yay0", lambda d, o: mio0(d, o, True))):
        o = rom.find(magic)
        while o >= 0:
            if o % 4 == 0:
                out = fn(rom, o)
                if out and len(out) >= 0x400:
                    yield f"{magic.decode()}@{o:07x}", out
            o = rom.find(magic, o + 4)


def looks_like_code(run):
    """Data decodes as instructions too (text whose letters are COP2's opcode,
    tables of floats): code has vector instructions of several kinds among
    scalar loads, stores and branches, jumps inside IMEM, and is not text."""
    n = len(run)
    cls = [classify(w) for w in run]
    vec = [c[0] for c in cls if c[3]]
    if len(vec) < max(12, n // 40):
        return False
    if sum(1 for w in run if w == 0) > n // 2:
        return False
    text = sum(1 for w in run if all(0x20 <= b <= 0x7E for b in w.to_bytes(4, "big")))
    if text > n // 5:
        return False
    hist = {}
    for k in vec:
        hist[k] = hist.get(k, 0) + 1
    if len(hist) < 6 or max(hist.values()) > len(vec) // 2:
        return False
    # its jumps go to IMEM: written as 0x0400_1xxx or as the 12 bits alone (the
    # RSP's PC has no more) -- never a main-CPU address
    jumps = [w for w in run if (w >> 26) in (2, 3)]
    imem = sum(1 for w in jumps if (w & 0x03FFFC00) == 0x01000400 or (w & 0x03FFFFFF) < 0x800)
    if imem < 3 or imem < len(jumps) // 2:           # (a region may run on into the main CPU's code)
        return False
    mem = sum(1 for w in run if (w >> 26) in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B))
    flow = sum(1 for w in run if (w >> 26) in (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07) or (w >> 26 == 0 and (w & 63) in (8, 9)))
    return mem >= n // 50 and flow >= n // 50


def regions(d):
    """runs of plausible RSP instructions with vector instructions in them: (offset, words)"""
    n = len(d) // 4
    words = struct.unpack(f">{n}I", d[:4 * n])
    out = []
    i = 0
    while i < n:
        if not classify(words[i])[2]:
            i += 1
            continue
        j, bad, last_ok = i, 0, i
        while j < n and bad <= 2:           # (a few stray words allowed: a table inside the code)
            if classify(words[j])[2]:
                bad = 0; last_ok = j
            else:
                bad += 1
            j += 1
        # the run may go on into the main CPU's code on either side (its scalar
        # instructions are the RSP's too): keep the stretches around the vector
        # instructions, 64 words to each side, joined when under 512 words apart
        run = words[i:last_ok + 1]
        vi = [k for k, w in enumerate(run) if classify(w)[3]]
        k = 0
        while k < len(vi):
            m = k
            while m + 1 < len(vi) and vi[m + 1] - vi[m] < 512:
                m += 1
            lo, hi = max(0, vi[k] - 64), min(len(run), vi[m] + 65)
            if hi - lo >= 96 and looks_like_code(run[lo:hi]):
                out.append((4 * (i + lo), run[lo:hi]))
            k = m + 1
        i = last_ok + 1
    return out


def load(path):
    """every ROM in a file (a .zip may hold several), big-endian"""
    def norm(name, d):
        if d[:4] == b"\x37\x80\x40\x12":                    # .v64: 16-bit swapped
            d = bytes(b for i in range(0, len(d) - 1, 2) for b in (d[i + 1], d[i]))
        elif d[:4] == b"\x40\x12\x37\x80":                  # .n64: little-endian words
            d = b"".join(d[i:i + 4][::-1] for i in range(0, len(d), 4))
        return (name, d) if d[:4] == b"\x80\x37\x12\x40" else None
    if path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            for m in z.namelist():
                r = norm(m, z.read(m))
                if r:
                    yield r
    else:
        r = norm(path, open(path, "rb").read())
        if r:
            yield r


def names(rom, out_blobs):
    """the microcodes' own names, where they carry one"""
    found = []
    for _, d in out_blobs:
        for key in (b"RSP Gfx ucode ", b"RSP SW Version: ", b"RSP Gfx[Safe] "):
            o = d.find(key)
            while o >= 0:
                s = d[o:o + 64].split(b"\0")[0].split(b"\n")[0].decode("latin1").strip()
                if s not in found:
                    found.append(s)
                o = d.find(key, o + 1)
    return found


def yaml_segments(path):
    """a splat YAML's code entries, [offset, type, name], sorted: where a ROM offset falls"""
    import re
    segs = []
    for line in open(path):
        m = re.match(r"\s*-\s*\[\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*(?:,\s*([^\],]+))?", line)
        if m:
            segs.append((int(m.group(1), 16), m.group(2), (m.group(3) or "").strip()))
    return sorted(segs)


def seg_of(segs, off):
    name = "?"
    for o, t, n in segs:
        if o > off:
            break
        name = f"{n or t} ({t})"
    return name


def main():
    args = sys.argv[1:]
    verbose = check = False
    segs = None
    while args and args[0].startswith("-"):
        if args[0] == "-v":
            verbose = True
        elif args[0] == "--check":           # every instruction decoded by rabbitizer too (pip install rabbitizer)
            check = True
        elif args[0] == "--yaml" and len(args) > 1:   # a splat YAML of this ROM: name the segment of each region
            segs = yaml_segments(args[1]); args = args[1:]
        else:
            sys.exit(__doc__)
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    if check:
        import rabbitizer
    worst = 0
    for path in args:
        for name, rom in load(path):
            title = rom[0x20:0x34].decode("latin1").strip()
            bl = list(blobs(rom))
            seen, total, unsup, kinds = set(), 0, {}, {}
            nreg = nchecked = 0
            disagree = {}
            for bname, d in bl:
                for off, run in regions(d):
                    key = hash(run)
                    if key in seen:
                        continue
                    seen.add(key)
                    nreg += 1
                    if verbose:
                        un = {}
                        for w in run:
                            kind, ok, plausible, _ = classify(w)
                            if plausible and not ok:
                                un[kind] = un.get(kind, 0) + 1
                        where = f"  [{seg_of(segs, off)} .. {seg_of(segs, off + 4 * len(run) - 4)}]" if segs and bname == "rom" else ""
                        print(f"    {bname} +{off:07x}: {len(run)} words{where}" + ("  -- " + ", ".join(f"{k} x{v}" for k, v in un.items()) if un else ""))
                        if segs and bname == "rom":
                            for k, w in enumerate(run):
                                kind, ok, plausible, _ = classify(w)
                                if plausible and not ok:
                                    print(f"        {off + 4 * k:07x}: {kind} in {seg_of(segs, off + 4 * k)}")
                    if check:
                        for w in run:
                            kind, ok, plausible, is_vec = classify(w)
                            if not is_vec:
                                continue
                            mn = rabbitizer.Instruction(w, category=rabbitizer.InstrCategory.RSP).disassemble().split()[0].upper()
                            if mn != kind.upper():
                                disagree[(kind, mn)] = disagree.get((kind, mn), 0) + 1
                            nchecked += 1
                    for w in run:
                        kind, ok, plausible, _ = classify(w)
                        if not plausible:
                            continue
                        total += 1
                        kinds[kind] = kinds.get(kind, 0) + 1
                        if not ok:
                            unsup[kind] = unsup.get(kind, 0) + 1
            print(f"{title!r} ({name.split('/')[-1]}, {len(rom) >> 20} MiB, {len(bl) - 1} compressed blocks): "
                  f"{nreg} microcode regions, {total} instructions")
            for s in names(rom, bl):
                print(f"    {s}")
            vec = {k: v for k, v in kinds.items() if k in VEC_NAMES.values() or k in LD or k in ST or k in ("MFC2", "MTC2", "CFC2", "CTC2")}
            print("    vector instructions: " + " ".join(f"{k}:{v}" for k, v in sorted(vec.items(), key=lambda x: -x[1])))
            if check:
                print(f"    rabbitizer: {nchecked} vector instructions decoded, " + ("all the same" if not disagree else
                      "DIFFERENT: " + ", ".join(f"{a} / {b} x{n}" for (a, b), n in disagree.items())))
            if nreg == 0:
                print("    !! no microcode found (compressed some other way?)")
                worst = max(worst, 2)
            elif unsup:
                print("    NOT IMPLEMENTED in rtl/rsp: " + ", ".join(f"{k} x{v}" for k, v in sorted(unsup.items(), key=lambda x: -x[1])))
                worst = max(worst, 1)
            else:
                print("    every instruction is implemented")
    sys.exit(worst)


if __name__ == "__main__":
    main()
