#!/usr/bin/env python3
"""Regenerates pkg/pocket/Assets/n64/common/n64savedb.txt, the save-type database
n64rt reads from the SD card (docs/mips.md, "The save-type database").

The database says which EEPROM a cartridge has (4 Kbit or 16 Kbit): nothing in the
ROM or in the PIF's packets does. It is plain facts about cartridge IDs (the four
characters at ROM 0x3B..0x3E), no ROM data. Sources, all public:

  mupen64plus.ini   (mupen64plus-core, data/mupen64plus.ini): `SaveType=` per ROM, keyed by
                    the MD5 of the .z64 (entries with `RefMD5=` take the referenced one's)
  libretro no-intro (libretro-database, metadat/no-intro/Nintendo - Nintendo 64.dat): the same
                    MD5 -> the cartridge ID ("serial")
  ares              (mia/medium/nintendo-64.cpp, optional, --ares): a list keyed by the first
                    three characters of the ID; it fills the IDs the join above has no ROM for
                    and is a cross-check (disagreements are printed, the join wins)
  micro-64.com      (http://micro-64.com/database/gamesave.shtml, the owner's source, optional,
                    --micro64): games by title under their save method (4/16 Kbit EEPROM, 256 Kbit SRAM
                    (Dezaemon 3D: 768 Kbit), FlashRAM, no save; Controller Pak is not an on-cartridge
                    save and is left out). It has no cartridge IDs: the titles are mapped to IDs through
                    the No-Intro names (exact after normalising, or by ALIASES below, each checked against
                    the dat); what cannot be mapped is listed, commented, at the end of the file. It adds
                    the games the others lack and where it disagrees on the type of a game the others have,
                    the row is kept as the others say and marked with a comment, and printed.

Usage:  tools/build_savedb.py [--fetch] [--dir <cache dir>] [-o <out>] [--ares] [--micro64]
  --fetch downloads the files into the cache dir (default /tmp/n64savedb_src) first (urllib, or curl if that fails).
"""
import argparse, html, os, re, subprocess, sys, time, urllib.request

URLS = {
    'mupen64plus.ini': 'https://raw.githubusercontent.com/mupen64plus/mupen64plus-core/master/data/mupen64plus.ini',
    'nointro.dat': 'https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/Nintendo%20-%20Nintendo%2064.dat',
    'micro64.html': 'http://micro-64.com/database/gamesave.shtml',
    'ares-n64.cpp': 'https://raw.githubusercontent.com/ares-emulator/ares/master/mia/medium/nintendo-64.cpp',
}
# (mupen64plus's `SaveType=None` is explicit -- the games with no battery or EEPROM -- and is kept: without it a 3-character
# row would be taken for every region of the game when only the ones with a chip were known)
TYPES = {'none': 'none', 'eeprom 4kb': 'eeprom4k', 'eeprom 16kb': 'eeprom16k', 'sram': 'sram', 'flash ram': 'flash'}
ARES_KIND = {'eeprom = 512': 'eeprom4k', 'eeprom = 2_KiB': 'eeprom16k', 'sram = 96_KiB': 'sram96k', 'sram': 'sram', 'flash': 'flash'}
MICRO64_SECTIONS = {'Controller Pak': 'cpak', '4Kbit EEPROM': 'eeprom4k', '16Kbit EEPROM': 'eeprom16k', '256Kbit SRAM': 'sram',
                    '1Mbit FlashRAM': 'flash', 'No Save': 'none'}
SRAM = {'sram', 'sram96k'}          # (the same memory, of two sizes: the more specific wins)

# Judged by hand where mupen64plus and ares do not agree and the join below cannot settle it (pattern -> (type, comment));
# each is a row that is put in place of the one the join gives (or added before it). The sources of the verdict are in the comment.
OVERRIDES = {
    # mupen64plus (both Donald Duck ROMs, SaveType=None; their MD5s are not in the No-Intro dat we use, so the join has no row)
    # against ares' first list (`NDQ` eeprom = 512, with a cpak too in another): mupen64plus wins, as it does in the join
    'NDQ?': ('none', "Disney's Donald Duck - Goin' Quackers [Quack Attack] (mupen64plus) [ares says eeprom4k]"),
}

def mupen_types(path):
    """md5 -> save type (RefMD5 followed)"""
    sec, cur = {}, None
    for line in open(path, encoding='latin-1'):
        line = line.strip()
        m = re.match(r'\[([0-9A-Fa-f]{32})\]$', line)
        if m:
            cur = sec.setdefault(m.group(1).upper(), {}); continue
        if cur is not None and '=' in line:
            k, v = line.split('=', 1); cur[k] = v
    out = {}
    for md5, s in sec.items():
        t, seen = s, set()
        while 'SaveType' not in t and 'RefMD5' in t and t['RefMD5'].upper() not in seen:
            seen.add(t['RefMD5'].upper()); t = sec.get(t['RefMD5'].upper(), {})
        if t.get('SaveType', '').lower() in TYPES:
            out[md5] = TYPES[t['SaveType'].lower()]
    return out

def nointro_ids(path):
    """md5 of a .z64 -> (cartridge ID, name)"""
    out = {}
    for m in re.finditer(r'rom \( name "([^"]*\.z64)" size \d+ crc \w+ md5 (\w+) sha1 \w+ serial "(\w{4})"', open(path, encoding='latin-1').read()):
        out[m.group(2).upper()] = (m.group(3), m.group(1)[:-4])
    return out

def revision(name):
    """the ROM header's version byte (0x3F), from No-Intro's "(Rev N)" / "(Rev A)": a guess, used only to
    tell the revisions of one ID apart where their chips differ"""
    m = re.search(r'\(Rev (\w)\)', name)
    return 0 if not m else int(m.group(1)) if m.group(1).isdigit() else ord(m.group(1).upper()) - 64

def ares_types(path):
    out = {}
    for m in re.finditer(r'if\(id == "(\w{3})"\) \{((?:eeprom|sram|flash) = [^;]*);.*?//\s*(.*)$', open(path, encoding='utf-8', errors='replace').read(), re.M):
        k = m.group(2).replace('  ', ' ')
        kind = next((v for p, v in ARES_KIND.items() if k.startswith(p)), None)
        if kind: out.setdefault(m.group(1), (kind, m.group(3).strip()))
    return out

def norm(t):
    """a title as a comparison key: case, punctuation, "the" and "&" do not count"""
    t = html.unescape(t).lower().replace('&', ' and ').replace("'", '')
    return ' '.join(w for w in re.sub(r'[^a-z0-9]+', ' ', t).split() if w != 'the')

def micro64_entries(path):
    """the page's (type, title), in its order (the section headings give the type)"""
    s = open(path, encoding='latin-1').read()
    s = re.sub(r'<script.*?</script>', '', s, flags=re.S | re.I)
    s = re.sub(r'<br\s*/?>', '\n', s, flags=re.I)
    s = re.sub(r'</(td|tr|p|h\d|div|li|th)>', '\n', s, flags=re.I)
    cur, out = None, []
    for l in html.unescape(re.sub(r'<[^>]+>', '', s)).split('\n'):
        l = l.strip()
        if l in MICRO64_SECTIONS: cur = MICRO64_SECTIONS[l]; continue
        if l.startswith('Credit for a lot'): cur = None
        if cur and l and len(l) < 70 and not l.endswith('.') and '(Password)' not in l.replace('(Password)', '') :
            kind = 'sram96k' if cur == 'sram' and '768Kbit' in l else cur
            out.append((kind, l.replace(' (Password)', '')))
    return out

# micro-64's title -> the first three characters of the cartridge IDs, where the No-Intro name differs (each checked
# against the dat's names: the comment is the dat's name)
ALIASES = {
    'aero gauge': 'NAG',                         # AeroGauge
    'bassmaster 2000': 'NB4',                    # Bassmasters 2000
    'battle tanx': 'NBX', 'battle tanx global assault': 'NBQ',          # BattleTanx
    'gt64 championship edition': 'NGC',          # GT 64 - Championship Edition
    'lode runner 3d': 'NLR',                     # Lode Runner 3-D
    'loony toons duck dodgers': 'NDU',           # Duck Dodgers Starring Daffy Duck
    'space station silicon valley': 'NSV',       # SpaceStation Silicon Valley
    'star wars episode 1 battle for naboo': 'NNA', 'star wars episode 1 racer': 'NEP',    # Star Wars Episode I - ...
    'starshot space circus': 'NSC',              # Starshot - Space Circus Fever
    'ridge racer 64': 'NRZ',                     # RR64 - Ridge Racer 64
    'bass hunter 64': 'NFH',                     # In-Fisherman - Bass Hunter 64
    'ken griffey jrs slugfest': 'NKJ',           # Ken Griffey Jr.'s Slugfest
    'megaman 64': 'NM6',                         # Mega Man 64
    'pocket monsters stadium': 'CPS',            # Pokemon Stadium (Japan), CPSJ
    'clayfighter 63 1 3': 'NCF', 'clayfighter sculptors cut': 'NC2',    # Clay Fighter 63 1-3, Clay Fighter - Sculptor's Cut
    'elmos letter adventure': 'NEL', 'elmos number journey': 'NEN',     # Sesame Street - Elmo's ...
    'knife edge nosegunner': 'NKE',              # Knife Edge - Nose Gunner
    'monster truck madness': 'NM3',              # Monster Truck Madness 64
    'rakuga kids': 'NKR',                        # Rakugakids
}

def title_index(path):
    """normalised title -> {the first three characters of an ID: its regions (4th characters)} for the No-Intro entries of that
    name (also by the part before and the part after " - ": "MRC - Multi-Racing Championship")"""
    idx = {}
    for md5, (i, n) in nointro_ids(path).items():
        base = re.sub(r'\s*\(.*', '', n)
        for k in {norm(base)} | {norm(x) for x in base.split(' - ')}:
            if k: idx.setdefault(k, {}).setdefault(i[:3], set()).add(i[3])
    return idx

def sortkey(r): return (r[0].replace('?', '~'), r[3], r[1])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--fetch', action='store_true'); ap.add_argument('--dir', default='/tmp/n64savedb_src')
    ap.add_argument('--ares', action='store_true'); ap.add_argument('--micro64', action='store_true'); ap.add_argument('-o', default=None)
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    for f, u in URLS.items():
        p = os.path.join(a.dir, f)
        if a.fetch or not os.path.exists(p):
            print('fetching', u, file=sys.stderr)
            try: urllib.request.urlretrieve(u, p)
            except OSError:                             # (a sandbox with a proxy that only curl knows)
                subprocess.run(['curl', '-sSL', '-m', '120', '-o', p, u], check=True)
    nidat = os.path.join(a.dir, 'nointro.dat')
    mt, ni = mupen_types(os.path.join(a.dir, 'mupen64plus.ini')), nointro_ids(nidat)
    byid, names, revs = {}, {}, {}       # ID -> {types}; ID -> a name; ID -> {type: {revisions}}
    for md5, t in mt.items():
        if md5 in ni:
            i, n = ni[md5]; byid.setdefault(i, set()).add(t); names.setdefault(i, n)
            revs.setdefault(i, {}).setdefault(t, set()).add(revision(n))
    groups = {}
    for i, t in byid.items(): groups.setdefault(i[:3], {})[i] = t
    ares = ares_types(os.path.join(a.dir, 'ares-n64.cpp')) if a.ares else {}
    for g, (k, n) in ares.items():          # (Dezaemon 3D: the SRAM of 96 KiB, which the join has as "SRAM")
        if g in groups and k == 'sram96k':
            for i in groups[g]: groups[g][i] = {k if x == 'sram' else x for x in groups[g][i]}
    m64, m64_un = {}, []                    # prefix -> {type: [titles]}; the titles that map to no ID
    if a.micro64:
        idx = title_index(nidat)
        for kind, title in micro64_entries(os.path.join(a.dir, 'micro64.html')):
            if kind == 'cpak': continue
            k = norm(re.sub(r'\s*\(.*', '', title))
            # (the page is for the USA releases; the others it names with their region: a title without one is not a game only
            # released in Japan that happens to have the same name -- Pokemon Stadium and Pocket Monsters Stadium)
            regional = re.search(r'\((JPN|PAL|J|E)\b', title) is not None
            gs = {ALIASES[k]} if k in ALIASES else {g for g, r in idx.get(k, {}).items() if regional or r - {'J'}}
            if not gs: m64_un.append((kind, title))
            for g in gs: m64.setdefault(g, {}).setdefault(kind, []).append(title)
    def flag(g, mine):
        """micro-64's note on a game: '' if it agrees (or has nothing), else what it says"""
        theirs = set(m64.get(g, {}))
        if not theirs or theirs == mine or (theirs <= SRAM and mine <= SRAM): return ''
        print('DISAGREES %s: %s %s; micro-64 says %s (%s)' % (g, sorted(mine), names.get(next(iter(sorted(groups.get(g, {}))), ''), ares.get(g, ('', ''))[1]), sorted(theirs),
              '; '.join(sorted(t for ts in m64[g].values() for t in ts))[:80]), file=sys.stderr)
        return ' [micro-64.com says %s]' % '/'.join(sorted(theirs))
    rows = []           # (id pattern, type, comment, rev=)
    for g, d in sorted(groups.items()):
        title = re.sub(r' \((USA|Europe|Japan|World|Australia|Germany|France|Spain|Italy|Brazil|China|Korea)[^)]*\).*', '', names[sorted(d)[0]]).strip()
        allt = {x for t in d.values() for x in t}
        note = flag(g, allt)
        if all(len(t) == 1 for t in d.values()) and len(allt) == 1:
            # the region wildcard: every ID known with these three characters has the one type
            rows.append((g + '?', next(iter(allt)), title + note, ''))
        else:
            # an ID with two types (Hoshi no Kirby 64 (J): SRAM in its first two revisions, EEPROM after): a line per type, with rev=
            for i, ts in sorted(d.items()):
                for t in sorted(ts):
                    r = ','.join(str(x) for x in sorted(revs[i][t])) if len(ts) > 1 else None
                    if r: print('revisions differ:', i, t, 'rev', r, file=sys.stderr)
                    rows.append((i, t, names[i] + note, 'rev=' + r if r else ''))
    for g, (k, n) in sorted(ares.items()):
        if g in groups:
            have = {x for t in groups[g].values() for x in t}
            if have != {k} and not (have <= SRAM and {k} <= SRAM):
                print('ares disagrees:', g, k, sorted(have), n, file=sys.stderr)
        else:
            rows.append((g + '?', k, n + ' (ares)' + flag(g, {k}), ''))
    for g, d in sorted(m64.items()):        # the games only micro-64 has
        if g in groups or g in ares: continue
        if len(d) > 1: print('micro-64 lists %s under %s: skipped' % (g, sorted(d)), file=sys.stderr); continue
        k = next(iter(d)); rows.append((g + '?', k, d[k][0] + ' (micro-64)', ''))
    for pat, (t, c) in OVERRIDES.items():   # (hand-judged; the cases are in OVERRIDES)
        rows = [r for r in rows if not (r[0] == pat and not r[3])]
        rows.append((pat, t, c, ''))
    rows.sort(key=sortkey)
    srcs = ['mupen64plus (SaveType by ROM MD5), libretro no-intro (MD5 -> ID)'] + (['ares (by 3-character ID)'] if a.ares else []) + \
           (['micro-64.com (by title; see below)'] if a.micro64 else [])
    fetched = time.strftime('%Y-%m-%d', time.localtime(os.path.getmtime(os.path.join(a.dir, 'micro64.html')))) if a.micro64 else ''
    out = ['# n64savedb.txt -- the save type of a cartridge, by the ID in its ROM header (offsets 0x3B..0x3E, e.g. NBKE),',
           '# read by n64rt.sys (data slot 4 of the core, the file Assets/n64/common/n64savedb.txt). Plain facts about',
           '# cartridges, no ROM data. Generated by tools/build_savedb.py from: ' + '; '.join(srcs) + ';',
           '# edit by hand if you wish.'] + \
          (['# micro-64.com: http://micro-64.com/database/gamesave.shtml (Game Save Method List, updated 2020-04-13), fetched ' + fetched + '.'] if a.micro64 else []) + \
          ['#',
           '#   <ID> <type>   # anything after a "#" is a comment',
           '#',
           '#   ID    four characters; "?" stands for any character, usually the region (the 4th: NBK? is every region of',
           '#         Banjo-Kazooie). The first line that matches wins, so a specific line goes before a wildcard one.',
           '#         Revisions of a game share the ID (and the chip). An absent game is a 4 Kbit EEPROM.',
           '#   type  eeprom4k (512 bytes, the default), eeprom16k (2,048 bytes), sram (32 KiB), sram96k (96 KiB: Dezaemon 3D),',
           '#         flash (128 KiB), none (no save in the cartridge). Only the EEPROM size is acted on today: the runtime finds',
           '#         an SRAM or a FlashRAM from the game\'s first access, and a line of any other type is skipped (sram96k',
           '#         among them). Optional after the type: rev=0,1 -- the line is for those versions only (the header byte',
           '#         0x3F), for an ID whose revisions have different chips; other fields are ignored.',
           '#   "[micro-64.com says X]": that list has another type for the game; the row is what mupen64plus/ares say.',
           '#   "[mupen64plus says X]", "[ares says X]": a row judged by hand.',
           '#', '']
    for i, t, n, r in rows: out.append('%-5s %-9s %s# %s' % (i, t, '%-8s ' % r if r else '', n))
    if m64_un:
        out += ['', '# micro-64.com titles whose cartridge ID could not be resolved (not guessed); type, title:']
        out += ['#   %-9s %s' % (k, t) for k, t in m64_un]
    text = '\n'.join(out) + '\n'
    if a.o: open(a.o, 'w').write(text)
    else: sys.stdout.write(text)
    c = {}
    for _, t, _, _ in rows: c[t] = c.get(t, 0) + 1
    print('%d lines: %s; %d unresolved micro-64 titles; %d bytes' % (len(rows), c, len(m64_un), len(text)), file=sys.stderr)

if __name__ == '__main__':
    main()
