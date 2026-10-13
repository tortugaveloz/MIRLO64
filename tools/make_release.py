#!/usr/bin/env python3
"""Pack the SD-card release zip: the core as it sits on the Pocket's SD card.

    tools/make_release.py <mirlo.rev> <n64rt.sys> <out.zip> [<version> [<date>]]

<mirlo.rev> is the bit-reversed bitstream (tools/package_bitstream.py) and
<n64rt.sys> the runtime (lang/n64rt: `make -C lang/n64rt`, then
build/n64rt.bin, which the core wants under that name as data slot 3). The
zip holds, from the SD card's root:

    Cores/tortuga.Mirlo64/   the core definition (pkg/pocket), mirlo.rev,
                             LICENSE.txt, NOTICE.txt
    Platforms/n64.json     the platform (Console > Nintendo 64) and its image
    Platforms/_images/n64.bin
    Assets/n64/common/     n64rt.sys, n64savedb.txt (pkg/pocket) and
                             PUT_ROMS_HERE.txt: where the games go

so a user unzips it onto the card's root and is done. Entries are written in
a fixed order with a fixed date, so the same inputs give the same zip.
<version> and <date> (YYYY-MM-DD), when given, are what the zip's core.json
says of itself instead of the repository's (a nightly build's name and day).
"""
import json
import os
import sys
import zipfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PKG = os.path.join(ROOT, "pkg", "pocket")
CORE = "Cores/tortuga.Mirlo64"
DATE = (2026, 1, 1, 0, 0, 0)


def add(z, name, data):
    info = zipfile.ZipInfo(name, DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def add_dir(z, name):
    info = zipfile.ZipInfo(name.rstrip("/") + "/", DATE)
    info.external_attr = (0o40755 << 16) | 0x10
    z.writestr(info, b"")


def main():
    if len(sys.argv) < 4 or len(sys.argv) > 6:
        sys.exit(__doc__)
    rev, sys_file, out = sys.argv[1:4]
    version = sys.argv[4] if len(sys.argv) > 4 else None
    date = sys.argv[5] if len(sys.argv) > 5 else None
    files = []
    for d, _, names in os.walk(PKG):
        for n in names:
            p = os.path.join(d, n)
            rel = os.path.relpath(p, PKG).replace(os.sep, "/")
            if rel.endswith(".rev"):
                continue                       # the bitstream comes from the argument
            files.append((rel, p))
    files.append((CORE + "/mirlo.rev", rev))
    files.append(("Assets/n64/common/n64rt.sys", sys_file))
    files.append((CORE + "/LICENSE.txt", os.path.join(ROOT, "LICENSE")))
    files.append((CORE + "/NOTICE.txt", os.path.join(ROOT, "NOTICE")))
    files.sort()
    dirs = sorted({"/".join(r.split("/")[:i]) for r, _ in files for i in range(1, r.count("/") + 1)}
                  | {"Assets", "Assets/n64", "Assets/n64/common"})
    with zipfile.ZipFile(out, "w") as z:
        for d in dirs:
            add_dir(z, d)
        for rel, p in files:
            with open(p, "rb") as f:
                data = f.read()
            if rel == CORE + "/core.json" and version:
                core = json.loads(data)
                core["core"]["metadata"]["version"] = version
                if date:
                    core["core"]["metadata"]["date_release"] = date
                data = (json.dumps(core, indent=2) + "\n").encode()
            add(z, rel, data)
    print(f"{out}: {len(files)} files")
    for rel, _ in files:
        print("  " + rel)


if __name__ == "__main__":
    main()
