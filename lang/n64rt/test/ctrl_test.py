#!/usr/bin/env python3
"""The per-game controller layouts (../n64rt_ctrl.h): the families do what they say through the pad mapping (the C of pad_host.c), the
cartridge table only has real cartridges (the save-type database's), a game's saved record survives and is refused when damaged, and the
menu's layout lists lay over the right source.   python3 ctrl_test.py   (needs gcc)"""
import os, re, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pad_test import K, N

FAM = dict(standard=0, platformer=1, shooter=2, skate=3, racing=4, fighter=5)
bad = 0


def fail(msg):
    global bad
    bad += 1
    print('FAIL', msg)


def build(name):
    exe = os.path.join(tempfile.mkdtemp(), name)
    subprocess.check_call(['gcc', '-O1', '-Wall', '-Wextra', '-o', exe, os.path.join(HERE, name + '.c')])
    return exe


def run(exe, *args, stdin=None):
    return subprocess.run([exe, *args], input=stdin, capture_output=True, text=True, check=True).stdout


def main():
    ctrl, pad = build('ctrl_host'), build('pad_host')
    presets = {}
    for line in run(ctrl, 'presets').splitlines():
        f, s0, s1, m1, *m = line.split()
        presets[int(f)] = dict(s0=int(s0, 16), s1=int(s1, 16), m1=int(m1, 16), map=[int(x, 16) for x in m])

    def pressed(fam, names, joy=0x80808080, typ=3):
        """the N64 buttons and stick family `fam` gives for the Pocket keys `names` (a settings word as the menu leaves it: the defaults)"""
        p = presets[fam]
        s0, s1 = p['s0'], p['s1'] & p['m1']
        k = typ << 28 | sum(1 << K[n] for n in names)
        out = run(pad, stdin='%x %x %x %x %s\n' % (k, joy, s0, s1, ' '.join('%x' % w for w in p['map']))).split()
        b = int(out[0], 16)
        return {n for n, bit in N.items() if b & bit}, int(out[1]), int(out[2])

    def expect(fam, names, want, stick=(0, 0), **kw):
        got = pressed(FAM[fam], names, **kw)
        if got != (set(want), *stick):
            fail('%s %s: %s, expected %s %s' % (fam, names, got, want, stick))

    # standard: no Modifier, the settings' defaults (Select is C-down)
    expect('standard', ['A'], ['A']); expect('standard', ['B'], ['B']); expect('standard', ['X'], ['CRIGHT']); expect('standard', ['Y'], ['CLEFT'])
    expect('standard', ['select'], ['CDOWN']); expect('standard', ['L1'], ['Z']); expect('standard', ['R1'], ['R']); expect('standard', ['start'], ['START'])
    expect('standard', ['left'], [], (-80, 0))
    # platformer: A, B, C-left, C-right unheld; Select (the Modifier) + X / Y / L / D-pad
    for fam in ('platformer', 'shooter'):
        expect(fam, ['A'], ['A']); expect(fam, ['B'], ['B']); expect(fam, ['X'], ['CRIGHT']); expect(fam, ['Y'], ['CLEFT'])
        expect(fam, ['select'], []); expect(fam, ['select', 'X'], ['CUP']); expect(fam, ['select', 'Y'], ['CDOWN'])
        expect(fam, ['select', 'A'], ['A']); expect(fam, ['select', 'B'], ['B']); expect(fam, ['select', 'L1'], ['L'])
        expect(fam, ['select', 'up'], ['CUP']); expect(fam, ['select', 'left'], ['CLEFT']); expect(fam, ['up'], [], (0, 80))
        expect(fam, ['start'], ['START'])
        expect(fam, [], ['CUP'], joy=0x20808080)                    # a dock pad's right stick up: C-up (the family turns "C buttons" on)
    expect('platformer', ['L1'], ['Z']); expect('platformer', ['R1'], ['R'])
    expect('shooter', ['R1'], ['Z']); expect('shooter', ['L1'], ['R'])
    # skate: the C buttons on the face keys, A / B with Select
    for key, c in (('A', 'CRIGHT'), ('B', 'CDOWN'), ('X', 'CUP'), ('Y', 'CLEFT')):
        expect('skate', [key], [c])
    expect('skate', ['select', 'A'], ['A']); expect('skate', ['select', 'B'], ['B']); expect('skate', ['select', 'X'], ['CUP'])
    expect('skate', ['select', 'A', 'B'], ['A', 'B']); expect('skate', ['L1'], ['Z']); expect('skate', ['left'], [], (-80, 0))
    # racing: the standard keys, Select + D-pad steers gently, Select + X / Y
    expect('racing', ['select', 'A'], ['A']); expect('racing', ['select', 'B'], ['B'])
    expect('racing', ['A'], ['A']); expect('racing', ['L1'], ['Z']); expect('racing', ['R1'], ['R'])
    expect('racing', ['left'], [], (-80, 0)); expect('racing', ['select', 'left'], [], (-40, 0))
    expect('racing', ['select', 'X'], ['CUP']); expect('racing', ['select', 'Y'], ['CDOWN'])
    # fighter: C-up (jump) on X, Z on Y and L, L on a dock pad's ZL
    expect('fighter', ['X'], ['CUP']); expect('fighter', ['Y'], ['Z']); expect('fighter', ['L1'], ['Z']); expect('fighter', ['L2'], ['L'])
    expect('fighter', ['select'], ['CDOWN']); expect('fighter', ['A'], ['A'])
    # a family sets the Modifier and (platformer, shooter) "C buttons", never "Stick" / "D-pad"
    for f, p in presets.items():
        if p['m1'] & ~(31 << 10):
            fail('family %d owns more of set1 than "C buttons": %x' % (f, p['m1']))
        if f not in (1, 2) and p['m1']:
            fail('family %d owns set1 bits without setting them' % f)
        if p['s0'] & ~(31 << 10):
            fail('family %d sets more of set0 than the Modifier: %x' % (f, p['s0']))

    # the cartridge table: every ID is a cartridge of the save-type database
    root = os.path.join(HERE, '..', '..', '..')
    db = open(os.path.join(root, 'pkg', 'pocket', 'Assets', 'n64', 'common', 'n64savedb.txt')).read()
    known = {m.group(1) for m in re.finditer(r'^([A-Z0-9]{3})[A-Z0-9?]\s', db, re.M)}
    src = open(os.path.join(HERE, '..', 'n64rt_ctrl.h')).read()
    ids = re.findall(r'N64RT_CG\("([A-Z0-9]{3})", N64RT_FAM_(\w+)\)', src)
    if not ids:
        fail('no cartridge table found')
    seen = set()
    for i, fam in ids:
        if i not in known: fail('cartridge %s is not in n64savedb.txt' % i)
        if i in seen: fail('cartridge %s is twice in the table' % i)
        seen.add(i)
    for i, want in (('NSME', 1), ('NSMJ', 1), ('NGEP', 2), ('NTQE', 3), ('NKTE', 4), ('NALE', 5), ('XXXX', -1), ('NSM', -1)):
        got = int(run(ctrl, 'family', i).strip())
        if got != want and not (len(i) == 3):
            fail('family %s: %d, expected %d' % (i, got, want))

    # a game's saved layout: it comes back, and any damage to it, or a short file, refuses it
    s0, s1, m = 6 << 10 | 0x7FFF, 1 << 10 | 2 << 15 | 1 << 22 | 0x3FFF000, [0x12345678 & 0x3FFFFFFF, 1, 2, 3, 0x3FFFFFFF]
    args = ['%x' % s0, '%x' % s1, *('%x' % w for w in m)]
    want = '1 %x %x %s' % (s0 & (31 << 10), s1 & (31 << 10 | 31 << 15 | 3 << 22), ' '.join('%x' % w for w in m))
    got = run(ctrl, 'record', *args).strip()
    if got != want:
        fail('record: %s, expected %s' % (got, want))
    for word in range(9):
        for bit in (0, 7, 31):
            if run(ctrl, 'record', *args, str(word), str(bit)).strip() != '0':
                # (a flip in a bit the mask drops is not damage: words 1 and 2)
                if word in (1, 2): continue
                fail('record with word %d bit %d flipped was accepted' % (word, bit))
    if run(ctrl, 'record', *args, '8', '0', '35').strip() != '0':
        fail('a record of 35 bytes was accepted')
    # the menu file against the Pocket's limits (Analogue's docs: 16 entries shown, names 23 characters, a list 16 options, ids unique 16-bit):
    # a longer file is "Load error in interact" at the core's start
    import json
    menu = json.load(open(os.path.join(root, 'pkg', 'pocket', 'Cores', 'tortuga.Mirlo64', 'interact.json')))['interact']['variables']
    if len(menu) > 16: fail('interact.json has %d entries: the Pocket shows 16' % len(menu))
    if len({v['id'] for v in menu}) != len(menu): fail('interact.json has duplicate ids')
    for v in menu:
        if len(v['name']) > 23: fail('interact.json name %r is over 23 characters' % v['name'])
        if not 0 <= v['id'] <= 65535: fail('interact.json id %r' % v['id'])
        if v['type'] not in ('radio', 'check', 'slider_u32', 'list', 'number_u32', 'action'): fail('interact.json type %r' % v['type'])
        if len(v.get('options', [])) > 16: fail('interact.json list %r has over 16 options' % v['name'])
        for o in v.get('options', []):
            if len(o['name']) > 23: fail('interact.json option %r is over 23 characters' % o['name'])
        if v['type'] == 'list' and not 0 <= v['defaultval'] < len(v['options']): fail('interact.json %r defaultval' % v['name'])
    # the menu's picks over a layout: the game's saved one, else its family's, else Standard
    def session(saved, fam, script):
        out = run(ctrl, 'session', str(saved), str(fam), stdin=script).splitlines()
        return [[int(x, 16) for x in line.split()[1:]] for line in out]
    def eff(saved, fam, pk=(0, 0, 0, 0), menu=(0, 0)):
        return session(saved, fam, 'pick %d %d %d %d\nmenu %x %x\neff\n' % (*pk, *menu))[0]
    def fld(words, f): return (words[f // 6] >> (5 * (f % 6))) & 31
    def keys(e, names, joy=0x80808080, typ=3):
        s0, s1, *m = e
        k = typ << 28 | sum(1 << K[n] for n in names)
        out = run(pad, stdin='%x %x %x %x %s\n' % (k, joy, s0, s1, ' '.join('%x' % w for w in m))).split()
        b = int(out[0], 16)
        return sorted(n for n, bit in N.items() if b & bit), int(out[1]), int(out[2])
    def want(e, names, buttons, stick=(0, 0), **kw):
        got = keys(e, names, **kw)
        if got != (sorted(buttons), *stick): fail('%s: %s, expected %s %s' % (names, got, sorted(buttons), stick))
    P, SKATE = 1, 3                                                 # the families (n64rt_ctrl.h: Standard 0 .. Fighter 5)
    # Auto, no saved layout, no family: Standard, the menu's own Shift key stays
    e = eff(0, -1, menu=(1 << 10, 0x1400))
    if e[0] != 1 << 10 or e[1] != 0x1400 or any(e[2:]): fail('Standard: %s' % (e,))
    # a family's Shift key is Select, the menu's own (any key) wins; a saved layout has its own, which is the game's
    for menu, saved, fam, w in ((0, 0, P, 6 << 10), (1 << 10, 0, P, 1 << 10), (0, 1, P, 3 << 10), (1 << 10, 1, P, 3 << 10)):
        got = eff(saved, fam, menu=(menu, 0))[0]
        if got != w: fail('Shift key: menu %x saved %d family %d: %x, expected %x' % (menu, saved, fam, got, w))
    if eff(1, P)[2] != 0x1234: fail('a saved layout is not used under Auto')
    if fld(eff(0, SKATE)[2:], 0) != 10: fail('the family is not used under Auto')
    # Normal: what the face keys, shoulders and Select send
    for pick, rows in ((1, {'A': ['A'], 'B': ['B'], 'X': ['CRIGHT'], 'Y': ['CLEFT'], 'L1': ['Z'], 'R1': ['R'], 'select': ['CDOWN']}),
                       (2, {'A': ['CRIGHT'], 'B': ['CDOWN'], 'X': ['CUP'], 'Y': ['CLEFT'], 'L1': ['Z'], 'R1': ['R'], 'select': []}),
                       (3, {'A': ['A'], 'B': ['B'], 'X': ['CRIGHT'], 'Y': ['CLEFT'], 'L1': ['R'], 'R1': ['Z'], 'select': ['CDOWN']}),
                       (4, {'A': ['A'], 'B': ['B'], 'X': ['CUP'], 'Y': ['Z'], 'L1': ['Z'], 'R1': ['R'], 'select': ['CDOWN']})):
        e = eff(0, -1, (pick, 0, 0, 0))
        for key, b in rows.items(): want(e, [key], b)
        want(e, ['start'], ['START']); want(e, ['left'], [], (-80, 0))
    # Auto is the family's: Skate has its own, and Normal 2 is the same
    e = eff(0, SKATE)
    for key, b in (('A', ['CRIGHT']), ('B', ['CDOWN']), ('X', ['CUP']), ('Y', ['CLEFT'])): want(e, [key], b)
    # Shift (the key is Select in a family, or the menu's): the lists change only what they name
    e = eff(0, P, (0, 1, 0, 0))                                     # same as normal
    want(e, ['select', 'X'], ['CRIGHT']); want(e, ['select', 'A'], ['A'])
    e = eff(0, P, (0, 2, 0, 0))                                     # cR cD cU cL
    for key, b in (('A', ['CRIGHT']), ('B', ['CDOWN']), ('X', ['CUP']), ('Y', ['CLEFT'])): want(e, ['select', key], b)
    e = eff(0, P, (0, 3, 0, 0))                                     # A B cU cD
    for key, b in (('A', ['A']), ('B', ['B']), ('X', ['CUP']), ('Y', ['CDOWN'])): want(e, ['select', key], b)
    e = eff(0, -1, (0, 4, 0, 0), menu=(1 << 10, 0))                 # / L R: Shift on the Pocket's L, no family
    want(e, ['L1'], []); want(e, ['L1', 'R1'], ['R']); want(e, ['L1', 'A'], ['CRIGHT'])   # (the shoulders keep their unshifted jobs: only the pick changes)
    e = eff(0, -1, (0, 4, 0, 0), menu=(6 << 10, 0))                 # Shift on Select
    want(e, ['select', 'L1'], ['L']); want(e, ['select', 'R1'], ['R'])
    # Shift: D-pad
    for pick, buttons, stick in ((1, [], (-80, 0)), (2, ['CLEFT'], (0, 0)), (3, [], (-40, 0)), (4, ['DLEFT'], (0, 0)), (5, ['DLEFT'], (-80, 0))):
        want(eff(0, P, (0, 0, pick, 0)), ['select', 'left'], buttons, stick)
    want(eff(0, P), ['select', 'left'], ['CLEFT'])                  # (Auto in the Platformer family: the C buttons)
    # D-pad keys
    for pick, buttons, stick in ((0, [], (-80, 0)), (1, ['DLEFT'], (0, 0)), (2, ['DLEFT'], (-80, 0)), (3, ['CLEFT'], (0, 0))):
        want(eff(0, -1, (0, 0, 0, pick)), ['left'], buttons, stick)
    # a pick out of the list's range is Auto
    if eff(0, P, (9, 9, 9, 9)) != eff(0, P): fail('picks out of range changed the layout')
    # Save keeps the layout in use with the picks; a saved layout is what Auto then uses; the picks still win over it; Forget goes back
    r = session(0, SKATE, 'pick 0 2 0 0\nmenu 0 0\ncap\nsave\npick 0 0 0 0\neff\nforget\neff\n')
    if fld(r[0][2:], 15) != 10 or r[0][0] != 6 << 10: fail('Save of a layout with a pick: %s' % (r[0],))
    if fld(r[1][2:], 15) != 10: fail('a saved game layout is not used under Auto: %s' % (r[1],))
    if fld(r[2][2:], 15) != 1 or fld(r[2][2:], 0) != 10: fail('Forget did not return Auto to the family: %s' % (r[2],))
    r = session(1, P, 'pick 1 0 0 0\nmenu 0 0\neff\n')
    if fld(r[0][2:], 0) != 1 or r[0][0] != 3 << 10: fail('a pick over a saved layout: %s' % (r[0],))
    print('ctrl_test: %s' % ('FAIL (%d)' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
