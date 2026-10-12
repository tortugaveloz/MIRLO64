#!/usr/bin/env python3
"""The fits known to close timing: projects/fits.json.

Whether a compile of this design closes timing depends on the fitter's seed, and a seed is good only for the sources it
was found with. projects/fits.json records, for one state of the hardware's sources, the seeds that closed: what a
build that must not search (the nightly, a release) compiles with.

    tools/fits.py seed                 the best recorded seed (the one with the most slack)
    tools/fits.py check                0 if the recorded fits are of the sources as they are now, 1 (and a line) if not
    tools/fits.py fingerprint          the fingerprint of the sources as they are now
    tools/fits.py verify <dir>         0 if the fit in <dir> (a projects directory, or its output_files) closes timing:
                                       no analysis of mirlo.sta.summary with a negative slack
    tools/fits.py record <dir>...      adds each fit that closes to fits.json (its seed from <dir>/mirlo.qsf), best first;
                                       fits recorded for other sources are dropped

The fingerprint is a SHA-256 over the files git tracks under rtl/, target/, platform/ and projects/ (but fits.json) and
tools/area2/fit.sh, which sets the build's macros: what the fitter's result depends on. The boot ROM's contents and the
runtime are not part of it: they change no placement.
"""
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
FITS = os.path.join(ROOT, "projects", "fits.json")
PATHS = ["rtl", "target", "platform", "projects", "tools/area2/fit.sh"]


def fingerprint():
    files = subprocess.run(["git", "-C", ROOT, "ls-files", "-z", "--"] + PATHS, check=True, capture_output=True).stdout
    h = hashlib.sha256()
    for name in sorted(f for f in files.decode().split("\0") if f and f != "projects/fits.json"):
        path = os.path.join(ROOT, name)
        if not os.path.isfile(path):
            continue
        h.update(name.encode() + b"\0" + hashlib.sha256(open(path, "rb").read()).digest())
    return h.hexdigest()


def load():
    if not os.path.exists(FITS):
        return {"sources": "", "fits": []}
    return json.load(open(FITS))


def outdir(d):
    return d if os.path.exists(os.path.join(d, "mirlo.sta.summary")) else os.path.join(d, "output_files")


def slacks(d):
    text = open(os.path.join(outdir(d), "mirlo.sta.summary")).read()
    return [float(v) for v in re.findall(r"^Slack\s*:\s*(\S+)", text, re.M)]


def verify(d):
    s = slacks(d)
    if not s:
        print(f"{d}: no timing analysis in mirlo.sta.summary")
        return False
    bad = sum(1 for v in s if v < 0)
    print(f"{d}: {len(s)} analyses, worst slack {min(s):+.3f} ns, {bad} negative")
    return bad == 0


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "fingerprint":
        print(fingerprint())
    elif cmd == "seed":
        fits = load()["fits"]
        if not fits:
            sys.exit("projects/fits.json records no fit")
        print(fits[0]["seed"])
    elif cmd == "check":
        if load()["sources"] != fingerprint():
            print("projects/fits.json: the recorded fits are of other sources than these (a seed sweep is due: tools/area2/fit.sh, then tools/fits.py record)")
            sys.exit(1)
    elif cmd == "verify" and len(sys.argv) == 3:
        sys.exit(0 if verify(sys.argv[2]) else 1)
    elif cmd == "record" and len(sys.argv) >= 3:
        fp = fingerprint()
        data = load()
        if data.get("sources") != fp:
            data = {"sources": fp, "fits": []}
        for d in sys.argv[2:]:
            if not verify(d):
                continue
            qsf = open(os.path.join(os.path.dirname(outdir(d).rstrip("/")), "mirlo.qsf")).read()
            seed = int(re.search(r"^set_global_assignment -name SEED (\d+)", qsf, re.M).group(1))
            alms = re.search(r"Logic utilization \(in ALMs\)\s*:\s*([\d,]+)", open(os.path.join(outdir(d), "mirlo.fit.summary")).read())
            version = re.search(r"Quartus Prime Version\s*:\s*(.+)", open(os.path.join(outdir(d), "mirlo.fit.summary")).read())
            data["quartus"] = version.group(1).strip() if version else data.get("quartus", "")
            data["fits"] = [f for f in data["fits"] if f["seed"] != seed]
            data["fits"].append({"seed": seed, "worst_slack_ns": round(min(slacks(d)), 3),
                                 "alms": int(alms.group(1).replace(",", "")) if alms else 0,
                                 "date": datetime.date.today().isoformat()})
        data["fits"].sort(key=lambda f: -f["worst_slack_ns"])
        out = {"quartus": data.get("quartus", ""), "sources": data["sources"], "fits": data["fits"]}
        open(FITS, "w").write(json.dumps(out, indent=2) + "\n")
        print(f"projects/fits.json: {len(out['fits'])} fits, best seed {out['fits'][0]['seed'] if out['fits'] else '-'}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
