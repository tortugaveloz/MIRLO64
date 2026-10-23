#!/usr/bin/env python3
"""Boot sweep: run every N64 ROM in a folder through the whole-SoC Verilator
bench (sim/mips_soc/obj_soc/tb_soc) and judge each run from its evidence.

  tools/sweep/sweep.py roms/ --seconds 8 -j 16
  tools/sweep/sweep.py roms/game.z64 --pad 300-320:8000

Writes <out>/report.json, <out>/report.md, <out>/sheet.png and, per game,
<out>/<game>/ with run.txt, rsp.log, the frames (PPM and PNG) and report.json.
Standard library only. See tools/sweep/README.md for the verdicts.
"""
import argparse
import concurrent.futures as cf
import datetime
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SYS_HZ = 62.5e6                 # the bench's clk_sys
SAVEDB = REPO / "pkg/pocket/Assets/n64/common/n64savedb.txt"
FROZEN_N = 3                    # identical trailing frames that make a picture "frozen"
BLACK_LEVEL = 24                # a channel above this is not black (RGBA5551 steps of 8)
BLACK_FRACTION = 0.001          # a frame with fewer lit channels than this share of its pixels is black

# What the runtime prints on its fatal paths (n64rt_fatal ends with "halted",
# no_rom() for a bad load) and generic words a future path might use.
FATAL_RE = re.compile(r"n64rt: halted|exception in thread|exception outside the monitor|exception for the monitor"
                      r"|nothing was started|no ROM at|not a \.z64 ROM|no game file|bad block table|no ROM0 block"
                      r"|no RDRAM left|never finished a display list|\bpanic|\bunhandled|\bunexpected", re.I)
ENTRY_RE = re.compile(r"^n64rt: (.*?) -- ROM \d+ bytes at .*entry ([0-9a-f]+)")
SUMMARY_RE = re.compile(r"^(\d+) sys cycles, (\d+) frames, (\d+) SDRAM protocol errors; "
                        r"cpu pc ([0-9a-f]+) cause ([0-9a-f]+) epc ([0-9a-f]+)")
EEP_RE = re.compile(r"EEPROM is used \(the save slot: (\d+) bytes, (\d+) of them from the file\)")
# The longest graphics pause still put down to a blank EEPROM. libultra's writes alone are 64 or 256 blocks at ~14 ms,
# but the pauses measured around them are longer (4 Kbit: 0.8 to 3.8 s, 16 Kbit: 4.8 s), so these leave a margin above that.
EEP_LIMIT_S = {512: 4.0, 2048: 6.0}
EXC_RE = re.compile(r"^\[excstat fields (\d+)\.\.(\d+): (\d+) exceptions \([\d.]+ a field\)(.*); (\d+) instructions retired\]")
VGA_RE = re.compile(r"^\[vga frame (\d+): (\d+) rows, (\d+) sys cycles since the last")
FB_RE = re.compile(r"^\[fb frame (\d+) sys \d+ base ([0-9a-f]+)")
RCP_RE = re.compile(r"^\[rcpstat field (\d+): ([\d.]+) ms; RSP running ([\d.]+) ms, RDP busy ([\d.]+) ms, (\d+) full syncs, CPU busy ([\d.]+) ms\]")
WINDOW = 60                     # fields per performance window (EXCSTAT's and RCPSTAT=60's period)
MAGIC = {b"\x80\x37\x12\x40": "z64", b"\x37\x80\x40\x12": "v64", b"\x40\x12\x37\x80": "n64"}
EXC_CODES = ["int", "mod", "tlbl", "tlbs", "adel", "ades", "ibe", "dbe", "sys", "bp", "ri", "cpu", "ov", "tr", "?", "fpe"]
STOP = threading.Event()        # Ctrl-C: the runs it cut short get no report, so a resumed sweep runs them again


def slug(name):
    return re.sub(r"[^A-Za-z0-9]+", "_", name).strip("_") or "rom"


def write_json(path, obj):
    """Written whole or not at all: --status may read it while the sweep writes it, and a kill must not leave half."""
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(json.dumps(obj, indent=1))
    tmp.replace(path)


def to_z64(src, cache):
    """The ROM as big-endian .z64: the input itself, or a converted copy in cache."""
    with open(src, "rb") as f:
        kind = MAGIC.get(f.read(4))
    if kind is None:
        return None, "unknown"
    if kind == "z64":
        return src, kind
    dst = cache / (slug(src.stem) + ".z64")
    if not dst.exists() or dst.stat().st_mtime < src.stat().st_mtime:
        data = bytearray(src.read_bytes())
        if kind == "v64":       # 16-bit units byte-swapped
            data[0::2], data[1::2] = data[1::2], data[0::2]
        else:                   # 32-bit words byte-reversed
            b = bytes(data)
            data[0::4], data[1::4], data[2::4], data[3::4] = b[3::4], b[2::4], b[1::4], b[0::4]
        cache.mkdir(parents=True, exist_ok=True)
        tmp = dst.with_suffix(".tmp")
        tmp.write_bytes(data)
        tmp.replace(dst)
    return dst, kind


# ---- images -----------------------------------------------------------------

def read_ppm(path):
    data = path.read_bytes()
    tokens, pos = [], 0
    while len(tokens) < 4:      # P6 width height maxval, then one whitespace byte
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        if pos == start:        # an empty or cut-off file (a bench killed as it wrote the frame)
            raise ValueError("truncated PPM header in %s" % path)
        tokens.append(data[start:pos])
    w, h = int(tokens[1]), int(tokens[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xffffffff)
    stride = w * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(h))   # filter 0 on every row
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    Path(path).write_bytes(png)


LIT = bytes(1 if v > BLACK_LEVEL else 0 for v in range(256))


def is_black(w, h, rgb):
    return rgb.translate(LIT).count(1) < max(1, w * h * BLACK_FRACTION)


def thumb(w, h, rgb, tw=160, th=120):
    """Downscale by whole steps (2x for the usual 320 x 240, more for a wider frame) by taking every n-th pixel."""
    sx, sy = max(1, -(-w // tw)), max(1, -(-h // th))
    out = bytearray()
    for y in range(0, h - h % sy, sy):
        row = rgb[y * w * 3:(y + 1) * w * 3]
        for x in range(0, w - w % sx, sx):
            out += row[x * 3:x * 3 + 3]
    return w // sx, h // sy, bytes(out)


# 3x5 digits for the contact sheet's labels
DIGITS = ["111101101101111", "010110010010111", "111001111100111", "111001111001111", "101101111001001",
          "111100111001111", "111100111101111", "111001001001001", "111101111101111", "111101111001111"]


def draw_number(img, iw, n, x0, y0, scale=2):
    text = str(n)
    bw, bh = (len(text) * 4 + 1) * scale, 7 * scale
    for y in range(bh):         # a black box behind the digits
        for x in range(bw):
            p = ((y0 + y) * iw + x0 + x) * 3
            img[p:p + 3] = b"\x00\x00\x00"
    for i, ch in enumerate(text):
        glyph = DIGITS[int(ch)]
        for gy in range(5):
            for gx in range(3):
                if glyph[gy * 3 + gx] == "1":
                    for sy in range(scale):
                        for sx in range(scale):
                            x = x0 + (1 + i * 4 + gx) * scale + sx
                            y = y0 + (1 + gy) * scale + sy
                            p = (y * iw + x) * 3
                            img[p:p + 3] = b"\xff\xff\x00"


def contact_sheet(path, tiles, cols=4):
    """tiles: [(number, w, h, rgb or None)], each already downscaled."""
    if not tiles:
        return
    tw, th = 160, 120
    gap, cols = 4, min(cols, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    iw, ih = cols * (tw + gap) + gap, rows * (th + gap) + gap
    img = bytearray(b"\x40\x40\x40" * (iw * ih))
    for k, (num, w, h, rgb) in enumerate(tiles):
        x0, y0 = gap + (k % cols) * (tw + gap), gap + (k // cols) * (th + gap)
        if rgb:
            for y in range(min(h, th)):
                p = ((y0 + y) * iw + x0) * 3
                img[p:p + min(w, tw) * 3] = rgb[y * w * 3:y * w * 3 + min(w, tw) * 3]
        draw_number(img, iw, num, x0, y0)
    write_png(path, iw, ih, bytes(img))


# ---- evidence ---------------------------------------------------------------

def parse_run(text):
    ev = {"stamps": [], "eeprom": None, "vga_periods": [], "fb_swaps": [], "rcpstat": [], "entry": None, "fatal": [],
          "fatal_before_entry": False, "summary": None, "n64rt": [], "n64rt_lines": 0}
    exc = {"fields": 0, "total": 0, "kinds": {}, "instructions": 0}
    for line in text.splitlines():
        m = EEP_RE.search(line)
        if m and ev["eeprom"] is None:      # where it falls among the timed lines says when it was
            ev["eeprom"] = {"bytes": int(m.group(1)), "from_file": int(m.group(2)), "after": len(ev["stamps"])}
        m = ENTRY_RE.match(line)
        if m and ev["entry"] is None:
            ev["entry"] = {"name": m.group(1).strip(), "entry": m.group(2), "line": line}
        if "(n64rt): CIC" in line and "loader" not in ev:
            ev["loader"] = line
            m = re.search(r"CIC (\w+)", line)
            ev["cic"] = m.group(1) if m else None
        if line.startswith("n64rt:"):
            ev["n64rt_lines"] += 1
            if len(ev["n64rt"]) < 40:
                ev["n64rt"].append(line)
        if FATAL_RE.search(line):
            if not ev["fatal"] and ev["entry"] is None:
                ev["fatal_before_entry"] = True
            if len(ev["fatal"]) < 10:
                ev["fatal"].append(line)
        m = VGA_RE.match(line)
        if m:
            ev["vga_periods"].append(int(m.group(3)))
            ev["stamps"].append(("vga", len(ev["vga_periods"])))
            continue
        m = FB_RE.match(line)
        if m:
            ev["fb_swaps"].append((int(m.group(1)), int(m.group(2), 16)))
            ev["stamps"].append(("sys", int(line.split()[4])))
            continue
        m = RCP_RE.match(line)
        if m:
            ev["rcpstat"].append(tuple(float(g) for g in m.groups()))
            ev["stamps"].append(("field", int(m.group(1))))
            continue
        m = EXC_RE.match(line)
        if m:
            ev["stamps"].append(("field", int(m.group(2))))
            exc["fields"] = int(m.group(2))
            exc["total"] += int(m.group(3))
            exc["instructions"] += int(m.group(5))
            parts = m.group(4).split()
            for kind, n in zip(parts[0::2], parts[1::2]):
                exc["kinds"][kind] = exc["kinds"].get(kind, 0) + int(n)
        m = SUMMARY_RE.match(line)
        if m:
            sys_c, frames, errs = int(m.group(1)), int(m.group(2)), int(m.group(3))
            pc, cause, epc = m.group(4), m.group(5), m.group(6)
            ev["summary"] = {"line": line, "sys_cycles": sys_c, "frames": frames, "sdram_errors": errs,
                             "pc": pc, "cause": cause, "epc": epc,
                             "exc_code": EXC_CODES[(int(cause, 16) >> 2) & 15] if (int(cause, 16) >> 2) & 31 < 16
                             else str((int(cause, 16) >> 2) & 31)}
    f = exc["fields"] or 1
    exc["per_field"] = round(exc["total"] / f, 2)
    exc["kinds_per_field"] = {k: round(v / f, 2) for k, v in exc["kinds"].items()}
    ev["excstat"] = exc
    return ev


def parse_rsplog(path):
    """One line per RSP run: start type flags cycles sig instructions dma_cycles rdp_wait yield_cycles."""
    tasks = []
    if path.exists():
        for line in path.read_text(errors="replace").splitlines():
            p = line.split()
            if len(p) >= 4:
                try:
                    if int(p[0]) == 0:      # the RSP leaving halt as the reset ends: not a task
                        continue
                    tasks.append((int(p[0]), int(p[1]), int(p[2], 16), int(p[3])))
                except ValueError:
                    pass
    return tasks


def field_clock(ev, cyc_per_frame):
    """field -> system cycle: RCPSTAT's measured 60-field windows where there are some (a game can switch from NTSC
    to PAL timing as it boots), the run's average beyond them."""
    marks = [(0, 0.0)]
    for r in ev["rcpstat"]:
        marks.append((int(r[0]), marks[-1][1] + r[1] * SYS_HZ / 1000))
    def cycle(field):
        for (f0, c0), (f1, c1) in zip(marks, marks[1:]):
            if field <= f1:
                return c0 + (c1 - c0) * (field - f0) / (f1 - f0)
        return marks[-1][1] + (field - marks[-1][0]) * cyc_per_frame
    return cycle


def performance(ev, tasks, cyc_per_frame, idle):
    """Per WINDOW fields: framebuffer swaps (FBLOG) as frames a console second, RSP / RDP busy (RCPSTAT) and the
    RSP log's tasks. CPU busy needs the game's idle loop (RCPSTAT_IDLE=lo,hi), so it is left out unless that is set."""
    summ = ev["summary"]
    n_fields = summ["frames"] if summ else 0
    rcp = {int(r[0]): r for r in ev["rcpstat"]}
    cpu_ok = bool(idle)         # the run's own RCPSTAT_IDLE, not the environment of a later --rejudge
    # (a run without RCPSTAT has no rcpstat lines: its fps is unknown, not 0)
    # A swap is a new base more than 4 KiB from the last: an interlaced picture moves its base by one row every
    # field, which is the same frame.
    swaps, prev = [], None
    for f, base in ev["fb_swaps"]:
        if prev is None or abs(base - prev) > 0x1000:
            swaps.append(f)
        prev = base
    cycle = field_clock(ev, cyc_per_frame)
    series = []
    for w0 in range(0, n_fields - WINDOW + 1, WINDOW):
        w1 = w0 + WINDOW
        r = rcp.get(w1)
        lo, hi = cycle(w0), cycle(w1)
        secs = (hi - lo) / SYS_HZ
        n_sw = sum(1 for f in swaps if w0 <= f < w1)     # "fb frame f" is logged as field f ends, at cycle(f + 1)
        sel = [t for t in tasks if lo <= t[0] < hi]
        g = [t[3] for t in sel if t[1] == 1]
        win = {"fields": "%d..%d" % (w0 + 1, w1), "console_s": round(secs, 3), "fb_swaps": n_sw,
               "fps": round(n_sw / secs, 1) if ev["rcpstat"] else None, "gfx": len(g), "audio": sum(1 for t in sel if t[1] == 2),
               "gfx_mean_cycles": round(sum(g) / len(g)) if g else None,
               "rsp_pct": None, "rdp_pct": None, "rdp_full_syncs": None, "cpu_pct": None}
        if r and r[1] > 0:
            win.update(rsp_pct=round(100 * r[2] / r[1], 1), rdp_pct=round(100 * r[3] / r[1], 1), rdp_full_syncs=int(r[4]))
            if cpu_ok:
                win["cpu_pct"] = round(100 * r[5] / r[1], 1)
        series.append(win)
    return {"window_fields": WINDOW, "fb_swaps": len(swaps), "cpu_idle_range": idle,
            "series": series, "last": series[-1] if series else None}


def eeprom_pause(ev, gfx_starts, cycle, end):
    """The graphics pause around the runtime's "EEPROM is used (..., 0 of them from the file)" line: a blank EEPROM,
    which libultra writes block by block at boot while the game holds its graphics. None without such a line."""
    e = ev["eeprom"]
    if not e or e["from_file"] != 0 or not gfx_starts:
        return None
    vga_at = [0]
    for c in ev["vga_periods"]:
        vga_at.append(vga_at[-1] + c)
    def at(stamp):
        kind, v = stamp
        return v if kind == "sys" else vga_at[v] if kind == "vga" else cycle(v)
    before = [at(s) for s in ev["stamps"][:e["after"]]]
    after = [at(s) for s in ev["stamps"][e["after"]:]]
    lo, hi = max(before, default=0), min(after, default=end)
    margin = 5 * cycle(1)           # "within a few fields" of the line, which is only known to lie in [lo, hi]
    starts = gfx_starts + [None]
    best = None             # the longest gap between graphics tasks that begins near the line (the others are frames)
    for g0, g1 in zip(starts, starts[1:]):
        if lo - margin <= g0 <= hi + margin and (g1 is None or g1 >= lo):
            stop = g1 if g1 is not None else end
            if best is None or stop - g0 > best[1] - best[0]:
                best = (g0, stop, g1 is None)
    if best is None:
        return None
    return {"bytes": e["bytes"], "pause_start_s": round(best[0] / SYS_HZ, 2), "pause_s": round((best[1] - best[0]) / SYS_HZ, 2),
            "ongoing": best[2], "limit_s": EEP_LIMIT_S.get(e["bytes"], 6.0)}


def judge(d, ev, frames, tasks, exit_code, timed_out):
    """Fill d with the evidence and return the verdict."""
    summ = ev["summary"]
    # frame index -> sys cycle, from the run's own rate (NTSC and PAL fields differ)
    cyc_per_frame = summ["sys_cycles"] / summ["frames"] if summ and summ["frames"] else SYS_HZ / 60
    cycle = field_clock(ev, cyc_per_frame)
    gfx = sum(1 for t in tasks if t[1] == 1)
    aud = sum(1 for t in tasks if t[1] == 2)
    d["rsp"] = {"tasks": len(tasks), "gfx": gfx, "audio": aud, "other": len(tasks) - gfx - aud,
                "resumed_after_yield": sum(1 for t in tasks if t[2] & 1), "by_type": {}}
    for t in tasks:     # OSTask types: 1 graphics, 2 audio, 3 video, 4 JPEG; anything else is not an OSTask
        d["rsp"]["by_type"][str(t[1])] = d["rsp"]["by_type"].get(str(t[1]), 0) + 1
    # tasks between consecutive captured frames: the RSP's pulse over the run
    marks = [(f["index"], f["cycle"]) for f in frames]
    if not marks or marks[0][1] > 0:
        marks = [(None, 0)] + marks
    periods = []
    for (a, lo), (b, hi) in zip(marks, marks[1:] + [(None, float("inf"))]):
        sel = [t for t in tasks if lo <= t[0] < hi]
        periods.append({"from_frame": a, "to_frame": b, "from_s": round(lo / SYS_HZ, 2), "gfx": sum(1 for t in sel if t[1] == 1),
                        "audio": sum(1 for t in sel if t[1] == 2), "all": len(sel)})
    d["rsp"]["per_period"] = periods
    lit = [f["index"] for f in frames if not f["black"]]
    d["non_black_frames"] = lit
    d["first_non_black"] = lit[0] if lit else None
    d["first_non_black_s"] = next((round(f["cycle"] / SYS_HZ, 2) for f in frames if not f["black"]), None)
    tail = frames[-FROZEN_N:]
    same_tail = len(tail) == FROZEN_N and len({f["md5"] for f in tail}) == 1
    tail_tasks = sum(1 for t in tasks if t[0] >= tail[0]["cycle"]) if tail else 0
    d["tail"] = {"frames": [f["index"] for f in tail], "identical": same_tail, "rsp_tasks_in_span": tail_tasks}
    d["picture_static"] = same_tail and tail_tasks > 0
    d["perf"] = performance(ev, tasks, cyc_per_frame, d.get("rcpstat_idle"))
    gfx_starts = [t[0] for t in tasks if t[1] == 1]
    last_field = None
    if gfx_starts:
        last_field = 0
        while cycle(last_field + 1) <= gfx_starts[-1]:
            last_field += 1
    d["rsp"]["last_gfx_field"] = last_field
    end = summ["sys_cycles"] if summ else (tasks[-1][0] if tasks else 0)
    d["eeprom_init"] = eeprom_pause(ev, gfx_starts, cycle, end)
    # graphics tasks that stop while audio goes on: a game waiting on the RDP or the RSP, worth saying in any verdict,
    # unless it is libultra writing a blank EEPROM at boot (the games pause their graphics for it)
    gfx_note = ""
    if gfx_starts and len(frames) >= 2 and gfx_starts[-1] < frames[-2]["cycle"]:
        ei = d["eeprom_init"]
        if ei and ei["ongoing"] and ei["pause_s"] <= ei["limit_s"]:
            gfx_note = "; blank EEPROM being initialised (graphics paused since field %d, %.1f s so far)" % (
                d["rsp"]["last_gfx_field"], ei["pause_s"])
        else:
            gfx_note = "; graphics stopped after %d gfx tasks (last at field %d)" % (gfx, d["rsp"]["last_gfx_field"])
    elif not gfx_starts and frames:
        gfx_note = "; no graphics task"

    verdict, reason = verdict_of(ev, summ, frames, lit, same_tail, tail, tail_tasks, tasks, exit_code, timed_out)
    return verdict, reason + gfx_note


def verdict_of(ev, summ, frames, lit, same_tail, tail, tail_tasks, tasks, exit_code, timed_out):
    if timed_out:
        return "timeout", "the bench ran past --timeout and was killed"
    if ev["fatal"] and ev["fatal_before_entry"]:
        return "no-boot", "fatal runtime line before the game's entry: " + ev["fatal"][0]
    if ev["entry"] is None:
        return "no-boot", "the runtime never printed the game's entry line"
    if ev["fatal"]:
        return "crash", "fatal runtime line: " + ev["fatal"][0]
    if exit_code != 0:
        return "crash", "the bench exited with %d" % exit_code
    if summ is None:
        return "crash", "no final summary line"
    if summ["sdram_errors"] > 0:
        return "crash", "%d SDRAM protocol errors" % summ["sdram_errors"]
    if not frames:
        return "black", "no frames were dumped"
    if not lit:
        return "black", "every dumped frame is black"
    if same_tail and tail_tasks == 0:
        if summ["exc_code"] != "int":
            return "crash", "picture and RSP stopped, final cause %s (%s) at pc %s" % (summ["cause"], summ["exc_code"], summ["pc"])
        return "frozen", "last %d frames identical and no RSP task since %.1f s" % (FROZEN_N, tail[0]["cycle"] / SYS_HZ)
    if same_tail:
        note = "picture static, RSP running"
    elif tail and tail_tasks == 0:          # a CPU-drawn picture can change with the RSP idle
        note = "frames change, RSP idle since %.1f s" % (tail[0]["cycle"] / SYS_HZ)
    else:
        note = "frames change, RSP running"
    return "alive", note


def wait_progress(p, gdir, timeout):
    """Wait for the bench; kill it after `timeout` wall seconds without progress (run.txt, rsp.log or the frames
    not growing), or after 4 x timeout in all. A loaded machine slows a bench a lot, which is not a hang."""
    t0 = last_change = time.time()
    last = None
    while True:
        try:
            p.wait(timeout=10)
            return False
        except subprocess.TimeoutExpired:
            pass
        files = [gdir / "run.txt", gdir / "rsp.log"] + sorted(gdir.glob("f*.ppm"))[-1:]
        now_sig = tuple(f.stat().st_size if f.exists() else -1 for f in files) + (len(files),)
        now = time.time()
        if now_sig != last:
            last, last_change = now_sig, now
        if now - last_change > timeout or now - t0 > 4 * timeout:
            p.kill()
            p.wait()
            return True


def run_game(rom, args, out, procs, lock):
    name = slug(rom.stem)
    gdir = out / name
    rep_path = gdir / "report.json"
    old = json.loads(rep_path.read_text()) if rep_path.exists() and not args.force else None
    if old and (args.rejudge or old.get("verdict") not in ("timeout", "error")):    # a timeout is tried again
        d = old
        d["skipped"] = True
        if args.rejudge and "exit_code" in d:      # new rules over the old run's files, no bench
            d["use_fb"] = args.fb
            analyse(d, gdir)
            write_json(rep_path, d)
        return d
    if gdir.exists():
        if any(gdir.iterdir()) and not ((gdir / "run.txt").exists() or rep_path.exists()):
            raise RuntimeError("%s is not a game folder an earlier sweep left: not deleting it" % gdir)
        shutil.rmtree(gdir)
    gdir.mkdir(parents=True)
    d = {"game": name, "rom": str(rom), "skipped": False}
    z64, kind = to_z64(rom, out / "z64")
    d["rom_format"] = kind
    if z64 is None:
        d.update(verdict="no-boot", reason="not an N64 ROM (unknown first bytes)", wall_s=0)
        write_json(rep_path, d)
        return d
    d["z64"] = str(z64)
    env = dict(os.environ, ROM=str(z64), FB_N64="1", EXCSTAT="1", RSPLOG=str(gdir / "rsp.log"), SLOT4=str(SAVEDB),
               FBLOG="1", RCPSTAT=str(WINDOW), VGA="1", VGAT="1")
    env.pop("VERIFY", None)
    d["rcpstat_idle"] = env.get("RCPSTAT_IDLE")
    if args.dumps:
        (gdir / "dumps").mkdir()
        env["RSPDUMP"] = str(gdir / "dumps")
    if args.pad:
        env["PAD"] = args.pad
    # every run leaves its save behind (SLOTS_OUT); --saves gives a later sweep the save an earlier one left
    env["SLOTS_OUT"] = str(gdir)
    if args.saves and (args.saves / name / "slot1.bin").exists():
        env["SLOT1"] = str(args.saves / name / "slot1.bin")
        d["save_in"] = env["SLOT1"]
    cmd = [args.tb_soc, args.n64rt, "-c", str(args.mcycles), "-o", str(gdir), "-e", str(args.frame_every)]
    d["command"] = " ".join(cmd)
    t0 = time.time()
    timed_out = False
    with open(gdir / "run.txt", "wb") as log:
        p = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT)
        with lock:
            procs.add(p)
        try:
            timed_out = wait_progress(p, gdir, args.timeout)
        finally:
            with lock:
                procs.discard(p)
    if STOP.is_set() or p.returncode == -signal.SIGINT:    # Ctrl-C reaches the bench too: not a crash of the game
        return dict(d, verdict="interrupted", reason="the sweep was interrupted; run it again to resume")
    d["wall_s"] = round(time.time() - t0, 1)
    d["exit_code"] = p.returncode
    d["timed_out"] = timed_out
    d["use_fb"] = args.fb
    analyse(d, gdir)
    write_json(rep_path, d)
    return d


def load_frames(gdir, prefix, cycle_of):
    """The dumped PPMs of one kind (f: framebuffer at fields, v: the scan-out's frames), each converted to PNG."""
    frames = []
    for ppm in sorted(gdir.glob(prefix + "[0-9]*.ppm")):
        try:
            w, h, rgb = read_ppm(ppm)
        except (ValueError, IndexError):
            continue
        if len(rgb) < w * h * 3:        # a frame cut off by a kill
            continue
        if not ppm.with_suffix(".png").exists():
            write_png(ppm.with_suffix(".png"), w, h, rgb)
        n = int(ppm.stem[1:])
        frames.append({"index": n, "cycle": round(cycle_of(n)), "file": ppm.with_suffix(".png").name, "w": w, "h": h,
                       "md5": hashlib.md5(rgb, usedforsecurity=False).hexdigest(), "black": is_black(w, h, rgb)})
    return frames


def analyse(d, gdir):
    """The evidence a finished run left in gdir, and the verdict, into d."""
    text = (gdir / "run.txt").read_text(errors="replace")
    ev = parse_run(text)
    summ = ev["summary"]
    cycle = field_clock(ev, summ["sys_cycles"] / summ["frames"] if summ and summ["frames"] else SYS_HZ / 60)
    vga_at = [0]                        # the scan-out's frame n ends at vga_at[n + 1] (VGAT's periods, summed)
    for c in ev["vga_periods"]:
        vga_at.append(vga_at[-1] + c)
    d["video_frames"] = load_frames(gdir, "v", lambda n: vga_at[n + 1] if n + 1 < len(vga_at) else n * SYS_HZ / 60)
    d["fb_frames"] = load_frames(gdir, "f", lambda n: cycle(n + 1))     # dump n is taken as field n ends
    # the video is what the scan-out sent; the framebuffer dump is memory read at its base, which can be another
    # size than the picture (--fb picks it, and a run without the video falls back to it)
    d["picture"] = "fb" if d.get("use_fb") or not d["video_frames"] else "video"
    frames = d["fb_frames"] if d["picture"] == "fb" else d["video_frames"]
    d["frames"] = frames
    tasks = parse_rsplog(gdir / "rsp.log")
    d["verdict"], d["reason"] = judge(d, ev, frames, tasks, d["exit_code"], d["timed_out"])
    d["entry"] = ev["entry"]
    d["loader"], d["cic"] = ev.get("loader"), ev.get("cic")
    d["fatal"] = ev["fatal"]
    d["summary"] = ev["summary"]
    d["excstat"] = ev["excstat"]
    d["n64rt"] = ev["n64rt"]
    d["n64rt_lines"] = ev["n64rt_lines"]


def last_lit_tile(out, d):
    lit = [f for f in d.get("frames", []) if not f["black"]]
    if not lit:
        return None
    w, h, rgb = read_ppm(out / d["game"] / (Path(lit[-1]["file"]).stem + ".ppm"))
    return thumb(w, h, rgb)


def write_reports(out, results, settings):
    (out / "report.json").write_text(json.dumps({"settings": settings, "games": results}, indent=1))
    lines = ["# Boot sweep", "",
             "%s, %s console seconds (-c %d), a frame every %d fields, tree %s." % (
                 settings["started"], settings["seconds"], settings["mcycles"], settings["frame_every"], settings["git"]),
             "", "| # | Game | Verdict | First picture | RSP tasks (gfx / audio) | Exceptions a field "
             "| FPS | RSP % | RDP % | Cycles a gfx task | Wall | Notes |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    tiles = []
    for i, d in enumerate(results, 1):
        rsp = d.get("rsp", {})
        exc = d.get("excstat", {})
        fnb = d.get("first_non_black")
        fnb_s = d.get("first_non_black_s")
        last = (d.get("perf") or {}).get("last") or {}
        cell = lambda k: "-" if last.get(k) is None else last[k]
        lines.append("| %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %ss | %s |" % (
            i, d["game"], d["verdict"], "-" if fnb is None else "%s %s (%ss)" % (d.get("picture", "fb")[0], fnb, fnb_s),
            "%d (%d / %d)" % (rsp["tasks"], rsp["gfx"], rsp["audio"]) if rsp else "-",
            exc.get("per_field", "-"), cell("fps"), cell("rsp_pct"), cell("rdp_pct"), cell("gfx_mean_cycles"),
            d.get("wall_s", "-"), d.get("reason", "").replace("|", "/")))
        tile = last_lit_tile(out, d)
        tiles.append((i,) + tile if tile else (i, 160, 120, None))
    lines += ["", "FPS, RSP %%, RDP %% and cycles a gfx task are the last %d-field window's (report.json has every window)."
              % WINDOW]
    lines += ["", "First picture: v (the scan-out's video frame) or f (the framebuffer dump at that field), its number and the "
              "console time. The contact sheet (sheet.png) shows the last non-black picture of each game, numbered as above."]
    (out / "report.md").write_text("\n".join(lines) + "\n")
    contact_sheet(out / "sheet.png", tiles)


# ---- watching a sweep ---------------------------------------------------------

def game_status(gdir, settings):
    """One game's progress from its folder alone, so a sweep can be watched from another shell."""
    every = settings["frame_every"]
    expected = round(settings["seconds"] * 60) // every + 1     # fields 0, every, ... up to the last second
    st = {"game": gdir.name, "state": "queued", "frames": 0, "expected": expected, "console_s": 0.0,
          "wall_s": None, "last_n64rt": ""}
    if (gdir / "report.json").exists():
        d = json.loads((gdir / "report.json").read_text())
        st["state"] = "done " + d.get("verdict", "?")
        st["wall_s"] = d.get("wall_s")
    elif (gdir / "run.txt").exists():
        st["state"] = "running"
    else:
        return st
    prefix = "f" if settings.get("picture") == "fb" else "v"
    st["frames"] = len(list(gdir.glob(prefix + "[0-9]*.ppm")))
    run = gdir / "run.txt"
    if run.exists():
        rate, fields = 60.0, 0
        for line in run.read_text(errors="replace").splitlines():
            if line.startswith("n64rt:"):
                st["last_n64rt"] = line
                if "(PAL)" in line:
                    rate = 50.0
            m = EXC_RE.match(line)
            if m:
                fields = int(m.group(2))
        last_frame = max((int(f.stem[1:]) for f in gdir.glob("f[0-9]*.ppm")), default=0)
        st["console_s"] = round(max(fields, last_frame) / rate, 1)
        st["expected"] = round(settings["seconds"] * rate) // every + 1     # (the scan-out follows the TV system too)
        if st["wall_s"] is None:
            born = getattr(run.stat(), "st_birthtime", None)    # (Linux has none, and a growing log's ctime is its last write)
            st["wall_s"] = None if born is None else round(time.time() - born)
    return st


def status_lines(out, settings, names):
    lines = []
    for n in names:
        st = game_status(out / n, settings)
        lines.append("%-45s %-14s %3d/%-3d frames %5.1f/%gs console %6ss wall  %s" % (
            st["game"][:45], st["state"][:14], st["frames"], st["expected"], st["console_s"], settings["seconds"],
            "-" if st["wall_s"] is None else st["wall_s"], st["last_n64rt"][:70]))
    return lines


def show_status(out, args):
    sp = out / "settings.json"
    if sp.exists():
        settings = json.loads(sp.read_text())
        names = settings.get("games", [])
    else:       # a folder without settings.json: what --seconds / --frame-every say, and the folders there are
        settings = {"seconds": args.seconds, "frame_every": args.frame_every}
        names = []
    names += sorted(p.name for p in out.iterdir() if p.is_dir() and p.name != "z64" and p.name not in names)
    for line in status_lines(out, settings, names):
        print(line)


def progress(out, names, stop, t0):
    """A one-line count every minute while the sweep runs."""
    while not stop.wait(60):
        done = sum((out / n / "report.json").exists() for n in names)
        running = sum((out / n / "run.txt").exists() and not (out / n / "report.json").exists() for n in names)
        print("sweep: %d done, %d running, %d queued, %d min" % (done, running, len(names) - done - running,
                                                                 (time.time() - t0) / 60), flush=True)


# ---- comparing two sweeps ------------------------------------------------------

def frame_diff(a, b):
    """(differing pixels, max channel delta) of two PPMs; a size change counts every pixel."""
    wa, ha, ra = read_ppm(a)
    wb, hb, rb = read_ppm(b)
    if (wa, ha) != (wb, hb) or len(ra) != len(rb):     # (or one of them cut off by a kill)
        return max(wa * ha, wb * hb), 255
    n = mx = 0
    for i in range(0, len(ra), 3):
        if ra[i:i + 3] != rb[i:i + 3]:
            n += 1
            mx = max(mx, abs(ra[i] - rb[i]), abs(ra[i + 1] - rb[i + 1]), abs(ra[i + 2] - rb[i + 2]))
    return n, mx


def compare_frames(da, db, prefix):
    fa = {p.name: p for p in da.glob(prefix + "[0-9]*.ppm")}
    fb = {p.name: p for p in db.glob(prefix + "[0-9]*.ppm")}
    common = sorted(set(fa) & set(fb))
    res = {"compared": len(common), "only_baseline": len(set(fa) - set(fb)), "only_candidate": len(set(fb) - set(fa)),
           "differing": 0, "first_diff": None, "first_diff_pixels": None, "first_diff_max_delta": None}
    for name in common:
        if fa[name].read_bytes() == fb[name].read_bytes():
            continue
        res["differing"] += 1
        if res["first_diff"] is None:      # the first difference is the one that says where things went apart
            res["first_diff"] = int(name[1:-4])
            res["first_diff_pixels"], res["first_diff_max_delta"] = frame_diff(fa[name], fb[name])
    res["identical"] = bool(common) and res["differing"] == 0 and not res["only_baseline"] and not res["only_candidate"]
    return res


def compare(base, cand):
    """Per game in both sweeps: the frames byte for byte, and the figures' deltas. Writes cand/compare.{md,json}."""
    def figures(d):
        last = (d.get("perf") or {}).get("last") or {}
        return {"verdict": d.get("verdict"), "fps": last.get("fps"), "rsp_pct": last.get("rsp_pct"),
                "rdp_pct": last.get("rdp_pct"), "exc_per_field": (d.get("excstat") or {}).get("per_field"),
                "gfx_tasks": (d.get("rsp") or {}).get("gfx")}
    games = sorted(p.parent.name for p in base.glob("*/report.json") if (cand / p.parent.name / "report.json").exists())
    rows = []
    for g in games:
        a = figures(json.loads((base / g / "report.json").read_text()))
        b = figures(json.loads((cand / g / "report.json").read_text()))
        delta = {k: (round(b[k] - a[k], 2) if isinstance(a[k], (int, float)) and isinstance(b[k], (int, float)) else None)
                 for k in a if k != "verdict"}
        rows.append({"game": g, "baseline": a, "candidate": b, "delta": delta,
                     "video": compare_frames(base / g, cand / g, "v"), "fb": compare_frames(base / g, cand / g, "f")})
    (cand / "compare.json").write_text(json.dumps({"baseline": str(base), "candidate": str(cand), "games": rows}, indent=1))

    def frames_cell(r):
        if not r["compared"]:
            return "-"
        if r["identical"]:
            return "identical (%d)" % r["compared"]
        if r["first_diff"] is None:
            return "%d common identical; %d only in baseline, %d only in candidate" % (
                r["compared"], r["only_baseline"], r["only_candidate"])
        return "%d of %d differ, first %d: %d px, max delta %d" % (r["differing"], r["compared"], r["first_diff"],
                                                                 r["first_diff_pixels"], r["first_diff_max_delta"])
    fmt = lambda v: "-" if v is None else ("%+g" % v)
    lines = ["# Sweep comparison", "", "Baseline `%s`, candidate `%s`." % (base, cand), "",
             "| Game | Verdicts | Video frames | Framebuffer frames | FPS | RSP % | RDP % | Exceptions a field | Gfx tasks |",
             "|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        dl = r["delta"]
        lines.append("| %s | %s / %s | %s | %s | %s | %s | %s | %s | %s |" % (
            r["game"], r["baseline"]["verdict"], r["candidate"]["verdict"], frames_cell(r["video"]), frames_cell(r["fb"]),
            fmt(dl["fps"]), fmt(dl["rsp_pct"]), fmt(dl["rdp_pct"]), fmt(dl["exc_per_field"]), fmt(dl["gfx_tasks"])))
    lines += ["", "Figures are candidate minus baseline (the last 60-field window for FPS, RSP and RDP)."]
    (cand / "compare.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description="Run N64 ROMs through the whole-SoC Verilator bench and judge each boot.")
    ap.add_argument("roms", nargs="*", type=Path, help="ROM files or folders (.z64, .v64, .n64)")
    ap.add_argument("--out", type=Path, help="output folder (default build/sweep/<timestamp>)")
    ap.add_argument("--seconds", type=float, default=8, help="console seconds to run (default 8)")
    ap.add_argument("--frame-every", type=int, default=60, help="dump every Nth field (default 60)")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 2), help="games run in parallel")
    ap.add_argument("--dumps", action="store_true", help="RSPDUMP the tasks too (8 MiB each)")
    ap.add_argument("--saves", type=Path, metavar="OUT",
                    help="start each game with the save an earlier sweep left in OUT/<game>/slot1.bin (SLOT1)")
    ap.add_argument("--fb", action="store_true",
                    help="judge and show the framebuffer dumps instead of the scan-out's video (both are captured)")
    ap.add_argument("--pad", help="controller 1 keys, passed as PAD= (from-to:keys[/joy],...)")
    ap.add_argument("--timeout", type=float, help="wall seconds a game may go without progress (default seconds*90+120; 4x that in all)")
    ap.add_argument("--force", action="store_true", help="re-run games that already have a report")
    ap.add_argument("--rejudge", action="store_true", help="recompute the verdicts of games already run, without the bench")
    ap.add_argument("--status", type=Path, metavar="OUT", help="print the progress of a running or finished sweep and exit")
    ap.add_argument("--compare", type=Path, nargs=2, metavar=("BASELINE", "CANDIDATE"),
                    help="compare two sweeps' frames and figures (writes CANDIDATE/compare.md and .json) and exit")
    args = ap.parse_args()
    if args.compare:
        for p in args.compare:
            if not p.is_dir():
                sys.exit("no such sweep folder: %s" % p)
        compare(args.compare[0].resolve(), args.compare[1].resolve())
        return
    if args.status:
        if not args.status.is_dir():
            sys.exit("no such sweep folder: %s" % args.status)
        show_status(args.status.resolve(), args)
        return
    if not args.roms:
        ap.error("give ROM files or folders (or --status <out>)")

    args.tb_soc = os.environ.get("TB_SOC", str(REPO / "sim/mips_soc/obj_soc/tb_soc"))
    args.n64rt = os.environ.get("N64RT", str(REPO / "lang/n64rt/build/n64rt.bin"))
    for p in (args.tb_soc, args.n64rt):
        if not Path(p).exists():
            sys.exit("missing %s (build the bench and the runtime first)" % p)
    # two million cycles more (about two fields) so the frame at the last whole second is dumped too
    args.mcycles = max(1, round(args.seconds * SYS_HZ / 1e6) + 2)
    if args.saves:
        if not args.saves.is_dir():
            sys.exit("no such sweep folder: %s" % args.saves)
        args.saves = args.saves.resolve()
    if args.timeout is None:
        args.timeout = args.seconds * 90 + 120
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = (args.out or REPO / "build/sweep" / stamp).resolve()
    out.mkdir(parents=True, exist_ok=True)

    roms = []
    for p in args.roms:
        if p.is_dir():
            roms += sorted(q for q in p.iterdir() if q.suffix.lower() in (".z64", ".v64", ".n64"))
        elif p.is_file():
            roms.append(p)
        else:
            sys.exit("no such ROM or folder: %s" % p)
    roms = [r.resolve() for r in roms]
    names = [slug(r.stem) for r in roms]
    folded = [n.casefold() for n in names]      # a case-insensitive filesystem gives Foo and foo one folder
    if len(set(folded)) != len(folded) or "z64" in folded:
        sys.exit("two ROMs map to the same output folder name (or one to z64, the converted ROMs' folder)")
    try:
        git = subprocess.run(["git", "describe", "--always", "--dirty", "--tags"], cwd=REPO,
                             capture_output=True, text=True).stdout.strip()
    except OSError:
        git = "unknown"
    settings = {"started": stamp, "seconds": args.seconds, "mcycles": args.mcycles, "frame_every": args.frame_every,
                "jobs": args.jobs, "timeout_s": args.timeout, "saves": str(args.saves) if args.saves else None,
                "picture": "fb" if args.fb else "video", "pad": args.pad, "dumps": args.dumps,
                "tb_soc": args.tb_soc, "n64rt": args.n64rt, "savedb": str(SAVEDB), "git": git, "out": str(out)}
    print("sweep: %d ROMs, -c %d, %d at a time, into %s" % (len(roms), args.mcycles, args.jobs, out), flush=True)

    (out / "settings.json").write_text(json.dumps(dict(settings, games=names), indent=1))
    procs, lock = set(), threading.Lock()
    results = {}
    stop = threading.Event()
    threading.Thread(target=progress, args=(out, names, stop, time.time()), daemon=True).start()
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run_game, r, args, out, procs, lock): r for r in roms}
        try:
            for fut in cf.as_completed(futs):
                r = futs[fut]
                try:
                    d = fut.result()
                except Exception as e:      # one broken game must not stop the sweep
                    d = {"game": slug(r.stem), "rom": str(r), "verdict": "error", "reason": repr(e)}
                results[d["game"]] = d
                kept = (" (rejudged)" if args.rejudge else " (kept from an earlier run)") if d.get("skipped") else ""
                print("%-50s %-8s %6ss  %s%s" % (d["game"], d["verdict"], d.get("wall_s", "-"), d.get("reason", ""), kept),
                      flush=True)
        except KeyboardInterrupt:
            # the pool waits for its threads on the way out: stop queued games and kill the running benches
            STOP.set()
            for f in futs:
                f.cancel()
            with lock:
                for p in procs:
                    p.kill()
            raise
    stop.set()
    ordered = [results[n] for n in names if n in results]
    write_reports(out, ordered, settings)
    print("sweep: %s" % (out / "report.md"))


if __name__ == "__main__":
    main()
