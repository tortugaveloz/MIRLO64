#!/usr/bin/env python3
"""The RDP's time against the console's, on hardware timings of large rectangles.

The timings: a 320 x 240 RGBA16 rectangle (a fill rectangle in 1- or 2-cycle mode), each list one rectangle, a SET
PRIM DEPTH and a SYNC FULL, measured on a console with the DPC's PIPEBUSY counter (Thar0's RDP-Timing-Tests,
github.com/Thar0/RDP-Timing-Tests, MIT: sample_results.txt, the minimum over 1000 runs, VI off; the numbers below are its
"Pipe" minima in milliseconds, RDP clocks = ms x 62,500): depth read (Z_CMP) and write (Z_UPD), passing or failing, IM_RD, the
alpha compare failing every pixel, the depth image in the colour image's 1 MiB bank or not.

  hw_rects.py gen DIR          writes each case's command list (big-endian words) to DIR/<case>.rdpw
  hw_rects.py check DIR TB     runs each through TB (obj_rdp/tb_rdp: TB_WORDS, TB_SYNCGAP, TB_SYNCT, TB_SDRAM), prints
                               the RTL's clocks against the console's and the geometric mean of the ratio's distance;
                               exit status 1 when that is over MAX (env, default 1.08)
"""
import math, os, struct, subprocess, sys

W, H = 320, 240
FB, ZB_SAME, ZB_DIFF = 0x100000, 0x100000 + W * H * 2, 0x400000

def cmd(op, w0, w1):
    return [(op << 24) | (w0 & 0xFFFFFF), w1 & 0xFFFFFFFF]

def stream(two, crd, zr, zw, zpass, zsame, ac):
    zb = ZB_SAME if zsame else ZB_DIFF
    fillrect = cmd(0x36, (W << 14) | (H << 2), 0)
    s = cmd(0x2D, 0, (W * 4) << 12 | (H * 4)) + cmd(0x2F, 3 << 20, 0)
    s += cmd(0x3F, (2 << 19) | (W - 1), FB) + cmd(0x37, 0, 0x00010001) + fillrect + cmd(0x27, 0, 0)
    s += cmd(0x3F, (2 << 19) | (W - 1), zb) + cmd(0x37, 0, 0xFFFCFFFC) + fillrect + cmd(0x27, 0, 0)
    s += cmd(0x3F, (2 << 19) | (W - 1), FB) + cmd(0x3E, 0, zb)
    s += cmd(0x3C, (15 << 20) | (31 << 15) | (7 << 12) | (7 << 9) | (15 << 5) | 31,
             (15 << 28) | (15 << 24) | (7 << 21) | (7 << 18) | (3 << 15) | (7 << 12) | (3 << 9) | (3 << 6) | (7 << 3) | 3)
    om0 = ((1 if two else 0) << 20) | (3 << 4) | (3 << 6) | (6 << 9)
    om1 = (1 if ac else 0) | 4 | 0x200 | (0x40 if crd else 0) | (0x10 if zr else 0) | (0x20 if zw else 0)
    if not zpass:      # (a rectangle at depth 0 in front, so that the timed ones fail)
        s += cmd(0x2F, om0, om1 | 0x20) + cmd(0x3A, 0, 0xFF0000FF) + cmd(0x2E, 0, 0) + fillrect + cmd(0x27, 0, 0)
    s += cmd(0x2F, om0, om1) + cmd(0x39, 0, 128) + cmd(0x3A, 0, 0x00FF0000 | (96 if ac else 255)) + cmd(0x2E, 0, 0x7FFF << 16) + cmd(0x29, 0, 0)
    for i in (1, 2):   # (two timed lists, each drawn at the depth the one before set, a step nearer: the second's time
        # is the one compared -- as the console's runs, one depth step nearer each)
        s += fillrect + cmd(0x2E, 0, (0x7FFF - i) << 16) + cmd(0x29, 0, 0)
    return s

# name: (2-cycle, IM_RD, Z_CMP, Z_UPD, Z passes, Z in the colour's bank, alpha compare fails), console clocks
CASES = {}
def add(name, args, ms):
    CASES[name] = (args, round(ms * 62500))
HW = [  # (base name, Z_CMP, Z_UPD, pass, same bank), [1c, 2c, 1c IM_RD, 2c IM_RD] in ms
    ("nozb", False, False, True, False, [1.2874720, 2.5176480, 2.6154240, 2.5916960]),
    ("zr_fail_same", True, False, False, True, [1.6951680, 2.8206720, 3.0361280, 3.1809280]),
    ("zr_fail_diff", True, False, False, False, [1.6951360, 2.8207680, 2.6590080, 2.9317120]),
    ("zr_pass_same", True, False, True, True, [3.1902880, 3.0173600, 4.2154720, 3.9243680]),
    ("zr_pass_diff", True, False, True, False, [2.8029920, 2.9252640, 3.7245280, 3.4496320]),
    ("zw_same", False, True, True, True, [3.0012000, 3.0717760, 4.3520640, 4.4292320]),
    ("zw_diff", False, True, True, False, [2.2248480, 2.5894080, 3.5492480, 3.4321120]),
    ("zrw_fail_same", True, True, False, True, [1.6980160, 2.8210240, 3.0524160, 3.1970880]),
    ("zrw_fail_diff", True, True, False, False, [1.6979840, 2.8210560, 2.6708800, 2.9371200]),
    ("zrw_pass_same", True, True, True, True, [4.3957760, 4.4617280, 5.7588160, 5.8505600]),
    ("zrw_pass_diff", True, True, True, False, [3.6092160, 3.4322400, 4.4910080, 4.4419200]),
]
for nm, zr, zw, zp, zs, ms in HW:
    for k, (crd, two) in enumerate(((False, False), (False, True), (True, False), (True, True))):
        add(f"{nm}_im{int(crd)}_c{2 if two else 1}", (two, crd, zr, zw, zp, zs, False), ms[k])
for nm, crd, zr, zs, ms in (("ac_im0_z0", False, False, True, [1.2443200, 2.4807840]), ("ac_im0_z1", False, True, True, [1.6951680, 2.8206720]),
                            ("ac_im1_z0", True, False, True, [1.6951360, 2.5296800]), ("ac_im1_z1_same", True, True, True, [3.0334400, 3.1808960]),
                            ("ac_im1_z1_diff", True, True, False, [2.6593280, 2.9316800])):
    for k, two in enumerate((False, True)):
        add(f"{nm}_c{2 if two else 1}", (two, crd, zr, False, False, zs, True), ms[k])

def main():
    if len(sys.argv) >= 3 and sys.argv[1] == "gen":
        os.makedirs(sys.argv[2], exist_ok=True)
        for nm, (args, _) in CASES.items():
            with open(os.path.join(sys.argv[2], nm + ".rdpw"), "wb") as f:
                f.write(b"".join(struct.pack(">I", x) for x in stream(*args)))
        return 0
    if len(sys.argv) >= 4 and sys.argv[1] == "check":
        env = dict(os.environ, TB_WORDS="1", TB_SYNCGAP="1", TB_SYNCT="1", TB_SDRAM="1")
        acc, n, bad = 0.0, 0, 0
        for nm, (_, hw) in sorted(CASES.items()):
            out = subprocess.run([sys.argv[3], os.path.join(sys.argv[2], nm + ".rdpw")], env=env, capture_output=True, text=True).stdout
            syncs = [l for l in out.splitlines() if l.startswith("SYNC")]
            ok = "RTL == model" in out
            ours = int(syncs[-1].split()[3])
            acc += abs(math.log(ours / hw)); n += 1; bad += not ok
            print(f"{nm:24s} console {hw:7d}  RTL {ours:7d}  {ours / hw:5.3f}{'' if ok else '  MISMATCH'}")
        g = math.exp(acc / n)
        print(f"geometric mean distance {g:.3f} over {n} cases" + (f", {bad} mismatches" if bad else ""))
        return 1 if bad or g > float(os.environ.get("MAX", "1.08")) else 0
    print(__doc__); return 2

sys.exit(main())
