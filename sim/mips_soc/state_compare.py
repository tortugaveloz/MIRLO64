#!/usr/bin/env python3
"""state_compare.py <ref dir> <ref from> <run dir> <run from> [count]: a save state's frames against the run it was made in.

The bench (tb_soc.cpp, -o <dir> -e 1) writes every field's picture as fNNNNNN.ppm. <ref dir> is the run that made the state
(SS_SAVE=<field>: the game goes on after it, as after a Memory), <run dir> one that loaded it (SS_LOAD, SS_LOAD_AT); <ref
from> and <run from> are the first field after each one's request. Both runs stop the game while the hardware comes to rest
and the state is made or put back, so the same picture can come a field or two apart: each of the run's frames is looked
for among the reference's within a few fields of where it should be, and the offset kept from one frame to the next.

Prints, for every frame, the offset at which it matched exactly (or the closest one and how many pixels differ), and a
summary: how many matched exactly, and the longest run of exact matches.

state_compare.py --audio <ref AILOG> <run AILOG> [frames]: the same for the sound -- the game's frames the AI read (the
bench's AILOG=<file>: its F lines), from the state's mark on (M: the core's answer to the save, to the load). The run's
first frames are looked for in the reference's (the AI's buffers were played out before the state was made, so the
first buffer after it starts both), and the two followed from there: how many frames are the same, and where they part."""
import os
import sys


def ppm(path):
    with open(path, "rb") as f:
        d = f.read()
    # P6\n<w> <h>\n255\n
    parts = d.split(b"\n", 3)
    return parts[3] if len(parts) == 4 else d


def diff(a, b):
    if a is None or b is None:
        return None
    if len(a) != len(b):
        return max(len(a), len(b)) // 3
    if a == b:
        return 0
    return sum(1 for i in range(0, len(a), 3) if a[i:i + 3] != b[i:i + 3])


def ai_frames(path, mark):
    out, on = [], False
    with open(path) as f:
        for ln in f:
            if ln.startswith("M "):
                if ln.split()[1] == mark:
                    on = True
            elif on and ln.startswith("F "):
                out.append(ln[2:10])
    return out


def audio(ref_log, run_log, count):
    r, x = ai_frames(ref_log, "save"), ai_frames(run_log, "load")
    if not r or not x:
        print("no frames after the mark (%d in the reference, %d in the run)" % (len(r), len(x)))
        return 1
    key = 64
    start = None
    for i in range(0, min(len(r), 48000) - key):
        if r[i:i + key] == x[:key]:
            start = i
            break
    if start is None:
        print("the run's first %d frames are nowhere in the reference's first 48000" % key)
        return 1
    n = 0
    while n < count and start + n < len(r) and n < len(x) and r[start + n] == x[n]:
        n += 1
    total = min(count, len(x), len(r) - start)
    print("audio: the run's frames from the load are the reference's from its frame %d after the save: %d of %d the same%s" %
          (start, n, total, "" if n == total else " (they part at frame %d)" % n))
    # after they part (a buffer of another length: the game's audio is chunked by its timing), the same sound a few
    # frames off: each run of the run's frames found again in the reference near where it should be
    i, j, same = n, start + n, n
    total = min(count, len(x), len(r) - start)
    while i + key <= total:
        lo, hi = max(0, j - 8192), min(len(r) - key, j + 8192)
        at = next((k for k in sorted(range(lo, hi), key=lambda k: abs(k - j)) if r[k:k + key] == x[i:i + key]), None)
        if at is None:
            i += key
            continue
        m = 0
        while i + m < total and at + m < len(r) and r[at + m] == x[i + m]:
            m += 1
        same += m
        i += m
        j = at + m
    print("audio: %d of %d frames found in the reference, in runs of at least %d, within 8192 frames of their place" % (same, total, key))
    return 0 if same >= total - key else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--audio":
        return audio(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 100000)
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    rd, rf, xd, xf = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 60
    cache = {}

    def ref(n):
        if n not in cache:
            p = os.path.join(rd, "f%06d.ppm" % n)
            cache[n] = ppm(p) if os.path.exists(p) else None
        return cache[n]

    off, exact, run, best_run, missing = 0, 0, 0, 0, 0
    for k in range(count):
        p = os.path.join(xd, "f%06d.ppm" % (xf + k))
        if not os.path.exists(p):
            missing += 1
            continue
        x = ppm(p)
        cands = sorted(range(off - 4, off + 5), key=lambda o: abs(o - off))
        best = None
        for o in cands:
            d = diff(ref(rf + k + o), x)
            if d is None:
                continue
            if best is None or d < best[1]:
                best = (o, d)
            if d == 0:
                break
        if best is None:
            print("frame %d: no reference frame near it" % (xf + k))
            continue
        o, d = best
        if d == 0:
            exact += 1
            run += 1
            best_run = max(best_run, run)
            off = o
        else:
            run = 0
        print("frame %d: %s (reference %d, offset %+d)" % (xf + k, "same" if d == 0 else "%d pixels differ" % d, rf + k + o, o))
    print("%d of %d frames exactly as the reference's (the longest run %d)%s" %
          (exact, count - missing, best_run, ", %d missing" % missing if missing else ""))
    return 0 if exact == count - missing else 1


if __name__ == "__main__":
    sys.exit(main())
