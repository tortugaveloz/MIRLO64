#!/usr/bin/env python3
"""The N64 controller mapping (../n64rt_pad.h) against the spec, written here independently as tables, over every combination of
the core's settings and many key words / stick bytes.   python3 pad_test.py   (needs gcc)"""
import itertools, os, random, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# the Pocket's keys (the APF's bit numbers) and the N64 controller's bits
K = dict(up=0, down=1, left=2, right=3, A=4, B=5, X=6, Y=7, L1=8, R1=9, L2=10, R2=11, select=14, start=15)
N = dict(A=0x8000, B=0x4000, Z=0x2000, START=0x1000, DUP=0x0800, DDOWN=0x0400, DLEFT=0x0200, DRIGHT=0x0100,
         L=0x0020, R=0x0010, CUP=0x0008, CDOWN=0x0004, CLEFT=0x0002, CRIGHT=0x0001)
CODE = {1: 'L1', 2: 'R1', 3: 'L2', 4: 'R2', 6: 'A', 7: 'B', 8: 'X', 9: 'Y', 10: 'select', 11: 'start'}   # a button setting's code -> the Pocket key; 5 and others: none


def spec(keys, joy, s0, s1):
    """-> (button, stick_x, stick_y) by the table in the task: written from the spec, not from the C"""
    held = {n for n, bit in K.items() if keys >> bit & 1}
    typ = keys >> 28
    analog = typ == 3 or (typ == 2 and joy not in (0, 0xFFFFFFFF))     # type 2: its stick bytes are believed unless a pad with none would send them
    out = set()
    mod_key = CODE.get((s0 >> 10) & 31)           # "Modifier" is interact0[14:10]: a button code, 0 (or 5, ...) no key
    rmod = mod_key is not None and mod_key in held
    if mod_key:
        held.discard(mod_key)                     # the modifier key never reaches the game
    if rmod:                                      # face buttons become the C buttons where they sit; the pad becomes half a stick
        for face, c in (('X', 'CUP'), ('B', 'CDOWN'), ('Y', 'CLEFT'), ('A', 'CRIGHT')):
            if face in held:
                out.add(c)
        held -= {'A', 'B', 'X', 'Y'}
    start_mode = (s1 >> 20) & 3                   # "Start" is interact1[21:20]: 0 Start, 1 Select+Start, 2 and 3 none
    if start_mode == 1:
        if {'select', 'start'} <= held:
            out.add('START')
            held -= {'select', 'start'}
        elif 'start' in held:
            out.add('CUP')                        # the Start key the chord leaves: C-up
    elif start_mode == 0 and 'start' in held:
        out.add('START')
    elif 'start' in held:
        out.add('CUP')                            # Start none: the Start key is C-up
    if 'A' in held: out.add('A')
    if 'B' in held: out.add('B')
    # Z, L, R: (setting, its own default for code 0)
    for name, code, dflt in (('Z', (s1 >> 5) & 31, 'L1'), ('L', (s0 >> 20) & 31, 'L2'), ('R', (s0 >> 25) & 31, 'R1')):
        key = dflt if code == 0 else CODE.get(code)
        if key and key in held:
            out.add(name)
    # joystick bytes: left X, left Y, right X, right Y (0x80 centre)
    lx, ly, rx, ry = (joy >> 0) & 255, (joy >> 8) & 255, (joy >> 16) & 255, (joy >> 24) & 255
    # the C buttons are on the face buttons always (Y left, X right, Select down; up only through the R modifier) ...
    for key, c in (('Y', 'CLEFT'), ('X', 'CRIGHT'), ('select', 'CDOWN')):
        if key in held: out.add(c)
    # ... and "C buttons" 1 adds the right stick, 2 the left one
    cm = (s1 >> 10) & 31
    if cm in (1, 2) and analog:
        cx, cy = (rx, ry) if cm == 1 else (lx, ly)
        if cx - 128 < -64: out.add('CLEFT')
        if cx - 128 > 64: out.add('CRIGHT')
        if cy - 128 < -64: out.add('CUP')
        if cy - 128 > 64: out.add('CDOWN')
    # the stick: "Stick" is interact1[19:15] (0 D-pad, 1 L stick, 2 R stick, 3 none; other: D-pad)
    def ax(v, flip):
        a = v - 128
        if flip: a = -a
        if -12 < a < 12: return 0
        a = int(a * 80 / 127)                     # (C: truncates toward zero)
        return max(-80, min(80, a))
    def dpad_stick():
        full = 40 if rmod else 80
        dx = (-1 if 'left' in held else 0) + (1 if 'right' in held else 0)
        dy = (1 if 'up' in held else 0) + (-1 if 'down' in held else 0)
        if rmod and dx and dy:
            return 28 * dx, 28 * dy
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
    elif smode == 3:
        sx, sy = 0, 0                             # none: centred
    else:
        sx, sy = dpad_stick()                     # D-pad, or an unknown value
    dmode = (s1 >> 22) & 3                        # the N64's D-pad: 0 none, 1 the D-pad, 2 L stick, 3 R stick
    if dmode == 1:
        for key, c in (('up', 'DUP'), ('down', 'DDOWN'), ('left', 'DLEFT'), ('right', 'DRIGHT')):
            if key in held: out.add(c)
    elif dmode in (2, 3) and analog:
        px, py = (lx, ly) if dmode == 2 else (rx, ry)
        if px - 128 < -64: out.add('DLEFT')
        if px - 128 > 64: out.add('DRIGHT')
        if py - 128 < -64: out.add('DUP')
        if py - 128 > 64: out.add('DDOWN')
    return sum(N[n] for n in out), sx, sy


def settings():
    """every combination of the settings' values (valid ones, and invalid codes)"""
    for mod, z, l, r, c, stick, dpad, start in itertools.product(
            (0, 1, 2, 3, 4, 5, 9), (0, 1, 4, 5, 6, 10, 11, 31), (0, 2, 3, 7, 9, 13), (0, 1, 2, 8, 11),
            (0, 1, 2, 3), (0, 1, 2, 3, 9), (0, 1, 2, 3), (0, 1, 2, 3)):
        s0 = mod << 10 | l << 20 | r << 25 | 3 << 5 | 1 << 15 | 2             # (crop, pak: other fields, ignored here)
        s1 = 1 | z << 5 | c << 10 | stick << 15 | start << 20 | dpad << 22 | 1 << 24 | 1 << 25   # (the free fields: EEPROM size, the old FPS and chord bits)
        yield s0, s1


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
    # all settings with a sample of words; all words with the default and a few others
    cases = []
    for s0, s1 in sets:
        for k, j in rng.sample(ws, 14):
            cases.append((k, j, s0, s1))
    for k, j in ws:
        for s0, s1 in rng.sample(sets, 6) + [(0, 0)]:
            cases.append((k, j, s0, s1))
    inp = ''.join('%x %x %x %x\n' % c for c in cases)
    res = subprocess.run([exe], input=inp, capture_output=True, text=True, check=True).stdout.split('\n')
    bad = 0
    for c, line in zip(cases, res):
        b, x, y = line.split()
        want = spec(*c)
        if (int(b, 16), int(x), int(y)) != want:
            bad += 1
            if bad <= 10:
                print('MISMATCH keys %08x joy %08x set0 %08x set1 %08x: C %s %s %s, spec %04x %d %d' % (*c, b, x, y, *want))
    # the defaults: Z on L, L button on ZL, R on R, face-button C buttons, the D-pad is the stick, Start is Start, no modifier
    d = lambda k: spec(k | 1 << 28, 0x80808080, 0, 0)
    assert d(1 << K['L1']) == (N['Z'], 0, 0)
    assert d(1 << K['R1']) == (N['R'], 0, 0)
    assert d(1 << K['L2']) == (N['L'], 0, 0) and d(1 << K['R2']) == (0, 0, 0)      # the N64's L is on ZL by default
    assert d(1 << K['Y']) == (N['CLEFT'], 0, 0) and d(1 << K['X']) == (N['CRIGHT'], 0, 0) and d(1 << K['select']) == (N['CDOWN'], 0, 0)
    assert d(1 << K['start']) == (N['START'], 0, 0)
    assert d(1 << K['left']) == (0, -80, 0)
    assert spec(3 << 28 | 1 << K['Y'], 0x80808080, 0, 1 << 10) == (N['CLEFT'], 0, 0)      # the face buttons stay with the right stick on
    assert spec(3 << 28 | 1 << K['left'], 0x80808080, 0, 0) == (0, -80, 0)      # the default Stick is the D-pad whatever the pad
    print('pad_test: %d cases, %d settings x words: %s' % (len(cases), len(sets), 'FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
