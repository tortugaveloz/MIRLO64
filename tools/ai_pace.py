#!/usr/bin/env python3
"""The pace at which a game feeds the AI, from the bench's AILOG (sim/mips_soc/tb_soc.cpp, a SoC built with MIRLO_AI):

    tools/ai_pace.py AILOG [--pal] [-v] [from_second [to_second]]

AILOG's W lines are the game's writes to the AI's registers with the sys cycle: a buffer is handed over by its AI_LEN write
(after AI_DRAM_ADDR's). A second a line (the bench's sys clock, 62.5 MHz):

  buffers    the hand-overs, and the frames (stereo sample pairs) in them
  late       when in the field the hand-overs came: the latest, in ms after the usual place (the field is the scan-out's,
             1,042,710 clocks; --pal: 1,250,000 -- the place is the phase most of the log's hand-overs have). A game that
             hands a buffer over once a retrace, when the audio task of the retrace before has ended, is late by what that
             task waited: for the graphics task to yield, for the memory
  missing    retraces with no hand-over (more than a field and a half since the one before)
  skipped    buffers of the game's rotation that were never handed over: the address after one is not the next of the
             ring (the ring is the addresses the log has). libultra's osAiSetNextBuffer returns -1 when the AI is full --
             the buffer being played and one queued -- and an audio manager that does not look at that loses the buffer:
             its 17 ms of sound are never played. It happens two retraces after a hand-over that came some 4 ms late and
             was followed by one in time (the late one found little left and gave a long buffer, the next a short one behind
             it: at the third the first is still being played). (Not counted: the ring starting over after the game set
             the AI's rate again -- its audio started anew)
  silence    words of silence the AI wrote inside the sound (O lines with the silence flag, between two real words): the
             AI was out of buffers

-v: each skipped buffer and each silence. Exit status 1 if a buffer was skipped."""
import collections
import sys

SYS = 62.5e6


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if not args:
        sys.exit(__doc__)
    field = 1250000 if "--pal" in sys.argv else 1042710
    verbose = "-v" in sys.argv
    lo = int(args[1]) if len(args) > 1 else 0
    hi = int(args[2]) if len(args) > 2 else 1 << 30
    addr = None
    bufs = []                               # (sys, address, bytes, the rate was set since the buffer before)
    rate = False
    sil = []                                # (sys of the last write before it, words)
    last = 0
    run = 0
    seen_real = False
    for line in open(args[0], errors="replace"):
        c = line[:1]
        if c == "W":
            t = line.split()
            if len(t) < 4:
                continue
            last = int(t[1])
            reg, v = int(t[2], 16) & 0x1C, int(t[3], 16)
            if reg == 0:
                addr = v & 0xFFFFF8
            elif reg == 4 and (v & 0x3FFF8) and addr is not None:
                bufs.append((last, addr, v & 0x3FFF8, rate))
                rate = False
            elif reg == 0x10:
                rate = True
        elif c == "O":
            t = line.split()
            if len(t) < 3:
                continue
            if t[2] == "1":
                run += 1
            else:
                if run and seen_real:
                    sil.append((last, run))
                run = 0
                seen_real = True
    if not bufs:
        sys.exit("no buffers in the log")
    # the usual place of a hand-over in the field: the fullest millisecond of the phases, taken a millisecond early
    bins = collections.Counter(int((b[0] % field) / 62500.0) for b in bufs)
    base = (bins.most_common(1)[0][0] - 1) * 62500
    ring = sorted(set(b[1] for b in bufs))
    nxt = {a: ring[(i + 1) % len(ring)] for i, a in enumerate(ring)}
    sec = collections.OrderedDict()
    skipped = []
    for i, (t, a, n, anew) in enumerate(bufs):
        d = sec.setdefault(int(t / SYS), [0, 0, 0.0, 0, 0, 0])
        d[0] += 1
        d[1] += n // 4
        d[2] = max(d[2], ((t - base) % field) / 62500.0 - 1.0)
        if i:
            if t - bufs[i - 1][0] > field * 3 // 2:
                d[3] += 1
            if len(ring) > 1 and a != nxt[bufs[i - 1][1]] and not anew:
                d[4] += 1
                skipped.append((t, bufs[i - 1][1], a))
    for t, n in sil:
        if int(t / SYS) in sec:
            sec[int(t / SYS)][5] += n
    print("%d buffers, %d addresses (%s); %d skipped, %d words of silence inside the sound" %
          (len(bufs), len(ring), " ".join("%06x" % a for a in ring), len(skipped), sum(n for _, n in sil)))
    print("second: buffers frames | late (ms) | missing | skipped | silence (words)")
    for k, d in sec.items():
        if lo <= k <= hi:
            print("%5d: %3d %6d | %5.1f | %2d | %2d | %d" % (k, d[0], d[1], d[2], d[3], d[4], d[5]))
    if verbose:
        for t, a, b in skipped:
            print("  skipped at %.3f s: %06x was followed by %06x" % (t / SYS, a, b))
        for t, n in sil:
            print("  silence after %.3f s: %d words (%.1f ms)" % (t / SYS, n, n / 48.0))
    return 1 if skipped else 0


if __name__ == "__main__":
    sys.exit(main())
