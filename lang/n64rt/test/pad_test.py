#!/usr/bin/env python3
"""The N64 controller mapping (../n64rt_pad.h) against the spec, written here independently as tables, over every combination of
the core's settings and many key words / stick bytes.   python3 pad_test.py   (needs gcc)"""
import itertools, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# the Pocket's keys (the APF's bit numbers) and the N64 controller's bits
K = dict(up=0, down=1, left=2, right=3, A=4, B=5, X=6, Y=7, L1=8, R1=9, L2=10, R2=11, L3=12, R3=13, select=14, start=15)
N = dict(A=0x8000, B=0x4000, Z=0x2000, START=0x1000, DUP=0x0800, DDOWN=0x0400, DLEFT=0x0200, DRIGHT=0x0100,
         L=0x0020, R=0x0010, CUP=0x0008, CDOWN=0x0004, CLEFT=0x0002, CRIGHT=0x0001)
CODE = {1: 'L1', 2: 'R1', 3: 'L2', 4: 'R2', 6: 'select'}      # a button setting's code -> the Pocket key; 5 and others: none
# the remap table: the ten keys (in the order of its fields), the output codes, and the defaults of each layer's fields
TKEYS = ['A', 'B', 'X', 'Y', 'L1', 'R1', 'L2', 'R2', 'select', 'start', 'L3', 'R3']
HELD = 15                                       # the Held layer's first field; groups: 12 D-pad keys, 13 left stick, 14 right stick (+15 held)
OUT = {1: 'A', 2: 'B', 3: 'Z', 4: 'START', 5: 'L', 6: 'R', 7: 'CUP', 8: 'CDOWN', 9: 'CLEFT', 10: 'CRIGHT',
       11: 'DUP', 12: 'DDOWN', 13: 'DLEFT', 14: 'DRIGHT'}
BASE_DEFAULT = [1, 2, 10, 9, 3, 6, 31, 31, 8, 4, 31, 31]       # A B C-right C-left Z R none none C-down Start none none
HELD_DEFAULT = [10, 8, 7, 9, 15, 15, 15, 15, 15, 15, 15, 15]   # C-right C-down C-up C-left, the rest as Base (15)
DEFAULT_MAP = [0] * 30


def pack(fields):
    """30 five-bit fields -> the five table words, six to a word"""
    return [sum((fields[6 * w + i] & 31) << (5 * i) for i in range(6)) for w in range(5)]


def spec(*args):
    return _spec(*args)[:3]


def _spec(keys, joy, s0, s1, fields):
    """-> (button, stick_x, stick_y) by the table in the task: written from the spec, not from the C"""
    dz = {1: 0, 2: 6, 3: 20, 4: 30}.get((s0 >> 20) & 31, 12)          # "Stick dead zone" interact0 [24:20]
    rng = {1: 85, 2: 75, 3: 70, 4: 60}.get((s0 >> 25) & 31, 80)       # "Stick range" interact0 [29:25]
    held = {n for n, bit in K.items() if keys >> bit & 1}
    typ = keys >> 28
    analog = typ == 3 or (typ == 2 and joy not in (0, 0xFFFFFFFF))     # type 2: its stick bytes are believed unless a pad with none would send them
    out = set()
    mod_key = CODE.get((s0 >> 10) & 31)           # "Modifier" is interact0[14:10]: a button code, 0 (or 5, ...) no key
    rmod = mod_key is not None and mod_key in held
    if mod_key:
        held.discard(mod_key)                     # the modifier key never reaches the game

    def target(i, layer_held):
        code = fields[(HELD if layer_held else 0) + i] or (HELD_DEFAULT if layer_held else BASE_DEFAULT)[i]
        if code == 15:                            # same as Base (in the Base layer: none)
            if not layer_held:
                return None
            code = fields[i] or BASE_DEFAULT[i]
        return OUT.get(code)                      # 15 again, 31 and the rest: none
    for i, key in enumerate(TKEYS):
        if key in held:
            t = target(i, rmod)
            if t:
                out.add(t)
    # the groups (0 the D-pad keys, 1 the left stick, 2 the right stick): 0 the settings', 1 stick, 2 half stick, 3 the N64's D-pad,
    # 4 C buttons, 5 none, 6 same as Base (held), 7 the stick and the N64's D-pad (the D-pad keys)
    def group(g):
        sel, lm = fields[12 + g], False
        if rmod:
            if fields[HELD + 12 + g] == 0:
                sel, lm = 0, True
            elif fields[HELD + 12 + g] != 6:
                sel = fields[HELD + 12 + g]
        return (6 if sel == 7 else 5 if sel > 4 else sel), lm
    dsel, lmod = group(0)
    tx = ty = 0
    if dsel:
        dx = ('right' in held) - ('left' in held)
        dy = ('up' in held) - ('down' in held)
        if dsel == 1:
            tx, ty = rng * dx, rng * dy
        elif dsel == 2:
            tx, ty = ((rng * 7 // 20) * dx, (rng * 7 // 20) * dy) if dx and dy else ((rng // 2) * dx, (rng // 2) * dy)
        elif dsel in (3, 4, 6):
            if dsel == 6: tx, ty = 80 * dx, 80 * dy                     # (7 in the field: the stick and the N64's D-pad)
            for key, c3, c4 in (('up', 'DUP', 'CUP'), ('down', 'DDOWN', 'CDOWN'), ('left', 'DLEFT', 'CLEFT'), ('right', 'DRIGHT', 'CRIGHT')):
                if key in held: out.add(c4 if dsel == 4 else c3)
        held -= {'up', 'down', 'left', 'right'}
    # joystick bytes: left X, left Y, right X, right Y (0x80 centre)
    lx, ly, rx, ry = (joy >> 0) & 255, (joy >> 8) & 255, (joy >> 16) & 255, (joy >> 24) & 255
    def axg(v, flip):
        a = v - 128
        if flip: a = -a
        if -dz < a < dz: return 0
        a = int(a * rng / 127)
        return max(-rng, min(rng, a))
    for g, (px, py) in ((1, (lx, ly)), (2, (rx, ry))):
        sel, _ = group(g)
        if not sel:
            sel = 1 if g == 1 else 4              # the default: the left stick is the N64's stick, the right the C buttons
        if analog:
            if sel in (1, 2):
                dv = 2 if sel == 2 else 1
                ax_, ay_ = int(axg(px, False) / dv), int(axg(py, True) / dv)
                tx += ax_; ty += ay_
            elif sel in (3, 4):
                for cond, c3, c4 in ((px - 128 < -64, 'DLEFT', 'CLEFT'), (px - 128 > 64, 'DRIGHT', 'CRIGHT'),
                                     (py - 128 < -64, 'DUP', 'CUP'), (py - 128 > 64, 'DDOWN', 'CDOWN')):
                    if cond: out.add(c3 if sel == 3 else c4)
        if g == 1: lx = ly = 128          # the settings see it centred
        else: rx = ry = 128
    # "C buttons" 1 adds the right stick
    if (s1 >> 10) & 31 == 1 and analog:
        if rx - 128 < -64: out.add('CLEFT')
        if rx - 128 > 64: out.add('CRIGHT')
        if ry - 128 < -64: out.add('CUP')
        if ry - 128 > 64: out.add('CDOWN')
    # the stick: "Stick" is interact1[19:15] (0 D-pad, 1 L stick, 2 R stick; other: D-pad)
    def ax(v, flip):
        a = v - 128
        if flip: a = -a
        if -dz < a < dz: return 0
        a = int(a * rng / 127)                    # (C: truncates toward zero)
        return max(-rng, min(rng, a))
    def dpad_stick():
        full = rng // 2 if lmod else rng
        dx = (-1 if 'left' in held else 0) + (1 if 'right' in held else 0)
        dy = (1 if 'up' in held else 0) + (-1 if 'down' in held else 0)
        if lmod and dx and dy:
            return (rng * 7 // 20) * dx, (rng * 7 // 20) * dy
        return full * dx, full * dy
    smode = (s1 >> 15) & 31
    sx = sy = 0
    if smode in (1, 2):
        use = (ax(lx, False), ax(ly, True)) if smode == 1 else (ax(rx, False), ax(ry, True))
        if not analog: use = (0, 0)
        if typ != 3 and use == (0, 0):
            sx, sy = dpad_stick()                 # no stick to speak of: the D-pad
        else:
            sx, sy = use
    else:
        sx, sy = dpad_stick()                     # D-pad, or an unknown value
    dmode = (s1 >> 22) & 3                        # the N64's D-pad: 0 none, 1 the D-pad, 2 L stick, 3 R stick
    dpad_swap = dmode == 1 and smode not in (1, 2)
    if dpad_swap and lmod:
        sx = sy = 0
    if dmode == 1 and (not dpad_swap or lmod):
        for key, c in (('up', 'DUP'), ('down', 'DDOWN'), ('left', 'DLEFT'), ('right', 'DRIGHT')):
            if key in held: out.add(c)
    elif dmode in (2, 3) and analog:
        px, py = (lx, ly) if dmode == 2 else (rx, ry)
        if px - 128 < -64: out.add('DLEFT')
        if px - 128 > 64: out.add('DRIGHT')
        if py - 128 < -64: out.add('DUP')
        if py - 128 > 64: out.add('DDOWN')
    sx = max(-rng, min(rng, sx + tx))
    sy = max(-rng, min(rng, sy + ty))
    return sum(N[n] for n in out), sx, sy


def settings():
    """every combination of the legacy settings' values (valid ones, and invalid codes)"""
    for mod, c, stick, dpad in itertools.product((0, 1, 2, 3, 4, 5, 6, 9), (0, 1, 2), (0, 1, 2, 9), (0, 1, 2, 3)):
        s0 = mod << 10 | 3 << 5 | 1 << 15 | 2                   # (crop, pak: other fields, ignored here)
        s1 = c << 10 | stick << 15 | dpad << 22 | 1 << 25         # (a free field)
        yield s0, s1
    for dz, rg in itertools.product((0, 1, 2, 3, 4, 9), (0, 1, 2, 3, 4, 9)):      # the stick's dead zone and range with each stick source
        for stick, dpad in itertools.product((0, 1, 2), (0, 1)):
            yield 2 << 10 | dz << 20 | rg << 25, 1 << 10 | stick << 15 | dpad << 22


def maps(rng):
    """remap tables: the defaults, then random ones (mostly valid codes, some invalid ones)"""
    yield DEFAULT_MAP
    codes = list(range(0, 16)) + [31, 20]
    gcodes = list(range(0, 7)) + [9, 31]
    for _ in range(60):
        f = [rng.choice(codes) for _ in range(30)]
        for g in (12, 13, 14, 27, 28, 29):
            f[g] = rng.choice(gcodes)
        yield f


def words(rng):
    """key words / stick bytes: each key alone and every pair, with every pad type, then random ones"""
    bits = list(K.values())
    sticks = [0x80808080, 0x00000000, 0xFFFFFFFF, 0x80808000, 0x80808080 ^ 0x00FF0000, 0xE0208040, 0x40C0A060, 0x80F08000, 0x7F81BF41]
    for typ in (0, 1, 2, 3):
        base = typ << 28
        ks = [0] + [1 << b for b in bits] + [1 << a | 1 << b for a, b in itertools.combinations(bits, 2)]
        for k in ks:
            yield base | k, 0x80808080
    for _ in range(300):
        yield rng.choice((1, 2, 3)) << 28 | rng.getrandbits(16), rng.choice(sticks) if rng.random() < .5 else rng.getrandbits(32)
    for typ in (2, 3):                            # both sticks off the centre, in all directions
        for joy in (0x808050B0, 0x80B05080, 0x8050B080, 0x50B0B050, 0x80805EB0, 0x805080B0, 0x30D0D030, 0xD03030D0):
            yield typ << 28, joy
    for typ in (2, 3):                            # the stick thresholds, edge by edge
        for v in (0x00, 0x3F, 0x40, 0x41, 0x7F, 0x80, 0xBF, 0xC0, 0xC1, 0xC2, 0xFF, 0x74, 0x8B, 0x8C, 0x8D):
            yield typ << 28, v | v << 8 | v << 16 | v << 24
            yield typ << 28 | 0xFFFF, v | v << 8 | v << 16 | v << 24


def main():
    exe = os.path.join(tempfile.mkdtemp(), 'pad_host')
    subprocess.check_call(['gcc', '-O1', '-Wall', '-o', exe, os.path.join(HERE, 'pad_host.c')])
    rng = random.Random(64)
    ws = list(words(rng))
    sets = list(settings())
    tables = list(maps(rng))
    bad = total = 0

    def check(cases):                             # cases: (keys, joy, s0, s1, fields)
        nonlocal bad, total
        inp = ''.join('%x %x %x %x %x %x %x %x %x\n' % (k, j, s0, s1, *pack(f)) for k, j, s0, s1, f in cases)
        res = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.splitlines()
        assert len(res) == len(cases), 'host harness did not return every case'
        total += len(cases)
        for c, line in zip(cases, res):
            b, x, y = line.split()
            want = _spec(*c)
            if (int(b, 16), int(x), int(y)) != want:
                bad += 1
                if bad <= 10:
                    print('MISMATCH keys %08x joy %08x set0 %08x set1 %08x map %s: C %s %s %s, spec %04x %d %d' % (*c[:4], c[4], b, x, y, *want))
    cases = []
    for s0, s1 in sets:
        for f in rng.sample(tables, 4) + [DEFAULT_MAP]:
            cases.extend((k, j, s0, s1, f) for k, j in rng.sample(ws, 6))
        if len(cases) >= 16384:
            check(cases)
            cases = []
    check(cases)
    check([(k, j, s0, s1, f) for k, j in ws for s0, s1 in rng.sample(sets, 3) for f in rng.sample(tables, 3)])
    # every key alone, in each layer, with every code in its field (and the D-pad group with its codes), the Modifier on R
    mod = 2 << 10
    cases = []
    for i, key in enumerate(TKEYS):
        for held in (0, 1):
            for code in range(32):
                f = [0] * 30
                f[(HELD if held else 0) + i] = code
                f[i] = 3 if held and code == 15 else f[i]          # (Held's "same as Base" with a Base field set)
                k = 1 << 28 | 1 << K[key] | (1 << K['R1'] if held else 0)
                cases.append((k, 0x80808080, mod, 1, f))
    for held in (0, 1):
        for code in range(10):
            for dirs in (['up'], ['down'], ['left'], ['right'], ['up', 'left'], ['down', 'right'], ['up', 'down']):
                for base in (0, 1, 3):
                    f = [0] * 30
                    f[HELD + 12 if held else 12] = code
                    f[12] = base if held else code
                    k = 1 << 28 | sum(1 << K[d] for d in dirs) | (1 << K['R1'] if held else 0)
                    cases.append((k, 0x80808080, mod, 1 | 1 << 22 * (base == 3), f))
    check(cases)
    # the defaults: Z on L, R on R, the N64's L on nothing, the face-button C buttons, the D-pad is the stick, Start is Start, no modifier
    d = lambda k: spec(k | 1 << 28, 0x80808080, 0, 0, DEFAULT_MAP)
    assert d(1 << K['L1']) == (N['Z'], 0, 0)
    assert d(1 << K['R1']) == (N['R'], 0, 0)
    assert d(1 << K['L2']) == (0, 0, 0) and d(1 << K['R2']) == (0, 0, 0)
    assert d(1 << K['Y']) == (N['CLEFT'], 0, 0) and d(1 << K['X']) == (N['CRIGHT'], 0, 0) and d(1 << K['select']) == (N['CDOWN'], 0, 0)
    assert d(1 << K['A']) == (N['A'], 0, 0) and d(1 << K['B']) == (N['B'], 0, 0)
    assert d(1 << K['start']) == (N['START'], 0, 0)
    assert d(1 << K['left']) == (0, -80, 0)
    # the Modifier on R: the face buttons are the C buttons (the layout before the remap table), the rest as unheld
    h = lambda names: spec(1 << 28 | sum(1 << K[n] for n in names) | 1 << K['R1'], 0x80808080, 2 << 10, 0, DEFAULT_MAP)
    assert h(['X']) == (N['CUP'], 0, 0) and h(['B']) == (N['CDOWN'], 0, 0) and h(['Y']) == (N['CLEFT'], 0, 0) and h(['A']) == (N['CRIGHT'], 0, 0)
    assert h(['select']) == (N['CDOWN'], 0, 0) and h(['start']) == (N['START'], 0, 0) and h(['L1']) == (N['Z'], 0, 0)
    assert h(['left']) == (0, -40, 0) and h([]) == (0, 0, 0)      # (the Modifier key itself: nothing)
    # swapping Base and Held: A/B on the face buttons unheld is the old "C-button layer"; Held gives A/B
    swap = [0] * 30
    swap[:4] = [10, 8, 7, 9]                                       # Base A, B, X, Y: C-right, C-down, C-up, C-left
    swap[HELD:HELD + 2] = [1, 2]                                           # Held A, B: A, B (Held X, Y: their defaults, the C buttons)
    g = lambda names, held: spec(1 << 28 | sum(1 << K[n] for n in names) | (1 << K['R1'] if held else 0), 0x80808080, 2 << 10, 0, swap)
    assert g(['A'], 0) == (N['CRIGHT'], 0, 0) and g(['A'], 1) == (N['A'], 0, 0) and g(['B'], 1) == (N['B'], 0, 0)
    assert g(['A', 'B'], 1) == (N['A'] | N['B'], 0, 0)
    # the D-pad keys on the C buttons while held, and half a stick unheld
    dd = [0] * 30
    dd[12], dd[HELD + 12] = 2, 4
    dk = lambda held: spec(1 << 28 | 1 << K['up'] | 1 << K['left'] | (1 << K['R1'] if held else 0), 0x80808080, 2 << 10, 0, dd)
    assert dk(0) == (0, -28, 28) and dk(1) == (N['CUP'] | N['CLEFT'], 0, 0)
    # Select as the Modifier; L3 / R3 in the table; the analog sticks' groups
    sel = lambda names, f, joy=0x80808080, typ=3, s1=0: spec(typ << 28 | sum(1 << K[n] for n in names), joy, 6 << 10, s1, f)
    assert sel(['select', 'X'], DEFAULT_MAP) == (N['CUP'], 0, 0) and sel(['select'], DEFAULT_MAP) == (0, 0, 0)
    f = [0] * 30
    f[10] = 1                                       # L3 -> A
    f[11] = 3                                       # R3 -> Z
    f[HELD + 10] = 14                               # held L3 -> D-right
    assert sel(['L3', 'R3'], f) == (N['A'] | N['Z'], 0, 0)
    assert sel(['select', 'L3'], f) == (N['DRIGHT'], 0, 0)
    f = [0] * 30
    f[13], f[14], f[HELD + 13] = 1, 4, 3        # left stick: the stick as it is, held the N64's D-pad; right stick: the C buttons
    assert sel([], f, joy=0x808080E0) == (0, 60, 0)
    assert sel(['select'], f, joy=0x808080E0) == (N['DRIGHT'], 0, 0)
    assert sel([], f, joy=0x20808080) == (N['CUP'], 0, 0) and sel([], f, joy=0x80E08080) == (N['CRIGHT'], 0, 0)
    assert sel([], f, joy=0x80808080, typ=1) == (0, 0, 0)       # a pad without a stick: nothing to give
    f[13] = 2
    assert sel([], f, joy=0x808080E0, s1=1 << 15) == (0, 30, 0)     # half a stick; the Stick setting sees the left stick centred
    # a dock pad out of the box (the default table, no settings): the left stick is the N64's stick, the right stick the C buttons, the D-pad keys
    # the stick too (and they add); the Pocket's own controls (no stick bytes) are unchanged; None in the group turns a stick off
    dock = lambda joy, names=(), typ=3, f=DEFAULT_MAP: spec(typ << 28 | sum(1 << K[n] for n in names), joy, 0, 0, f)
    assert dock(0x808080E0) == (0, 60, 0) and dock(0x80808020) == (0, -60, 0)               # left stick right / left (x byte 0xE0 / 0x20)
    assert dock(0x80E08080) == (N['CRIGHT'], 0, 0) and dock(0x20808080) == (N['CUP'], 0, 0)   # right stick: C-right / C-up
    assert dock(0x80808080, ['left']) == (0, -80, 0) and dock(0x808080E0, ['right']) == (0, 80, 0)   # the D-pad keys and the stick add, clamped
    assert dock(0x808080E0, typ=1) == (0, 0, 0)                                              # a pad without stick bytes gives nothing
    none = [0] * 30; none[13], none[14] = 5, 5
    assert dock(0x808080E0, f=none) == (0, 0, 0) and dock(0x20808080, f=none) == (0, 0, 0)    # both groups None
    swap = [0] * 30; swap[13], swap[HELD + 13] = 1, 3                                         # held: the left stick is the N64's D-pad
    assert spec(3 << 28 | 1 << K['select'], 0x808080E0, 6 << 10, 0, swap) == (N['DRIGHT'], 0, 0)
    print('pad_test: %d cases, %d legacy settings x %d tables: %s' % (total, len(sets), len(tables), 'FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
