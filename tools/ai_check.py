#!/usr/bin/env python3
"""Checks the AI in hardware (rtl/soc/rcp_ai.sv) against the algorithm of the runtime's software AI (lang/n64rt/n64rt_rsp.c,
ai_service / fetch_frame), from the bench's AILOG (sim/mips_soc/tb_soc.cpp, a SoC built with MIRLO_AI):

    tools/ai_check.py AILOG [--lcg | --snap] [--lp] [--tv ntsc|pal|mpal]

AILOG's lines: F <word> a frame the reader fetched (L high, R low), S <step> the resampler's step, O <word> <silence> a word
written to the Pocket's FIFO, R the reader started over, I an interrupt, W ... a register write. The software's algorithm
is run over the frames in the order they were fetched, with the step as it was when each word was written, and its words
are compared with the hardware's real (not silence) words, sample for sample: the count of words, the RMS and the largest
error (of the 16-bit samples) are printed. With --snap the frames fetched must also be the buffers as they were when the game
handed them over (the bench's B lines: a buffer changed after it was handed over would be read changed). The silence words must be zero. With --lcg the frames must also be the pattern of
lang/mips/test/ai_test.c (its LCG from seed 1, a frame a word).  --lp: the SoC has the AI's output low-pass (rcp_ai.sv LP, the Pocket's core): the model filters its stream too. Exit status 0 if the stream is the same."""
import math
import sys


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def lcg_words(n, seed=1):
    s = seed
    out = []
    for _ in range(n):
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        out.append(s ^ (s >> 15))
    return out


class Soft:
    """n64rt_rsp.c's resampler: s_pl/s_pr/s_nl/s_nr, s_phase, s_step; fetch_frame shifts the next frame in."""

    def __init__(self, frames):
        self.fr = frames
        self.i = 0
        self.have = False
        self.pl = self.pr = self.nl = self.nr = 0
        self.phase = 0

    def fetch(self):
        w = self.fr[self.i]
        self.i += 1
        self.pl, self.pr = self.nl, self.nr
        self.nl, self.nr = s16(w >> 16), s16(w)

    def sample(self, step):
        if not self.have:
            self.fetch()
            self.have = True
            self.phase = 0x10000
        while self.phase >= 0x10000:
            self.fetch()
            self.phase -= 0x10000
        ph = self.phase >> 1
        # (C: s32 arithmetic shifts; Python's >> floors the same way)
        l = self.pl + (((self.nl - self.pl) * ph) >> 15)
        r = self.pr + (((self.nr - self.pr) * ph) >> 15)
        self.phase += step
        return ((l & 0xFFFF) << 16) | (r & 0xFFFF)


class LowPass:
    """rcp_ai.sv's LP: two stages of y = (x0 + 2 x1 + x2 + 2) >> 2 a channel (the binomial 1-4-6-4-1 / 16): the interpolated frame goes
    through the first, its output through the second; each keeps its last two inputs, from rest when nothing was played."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.s = [[0, 0, 0, 0], [0, 0, 0, 0]]            # per channel: x1, x2 (the first stage's), v1, v2 (the second's)

    def word(self, w):
        out = 0
        for ch, sh in ((0, 16), (1, 0)):
            x0 = s16(w >> sh)
            x1, x2, v1, v2 = self.s[ch]
            u = (x0 + 2 * x1 + x2 + 2) >> 2
            y = (u + 2 * v1 + v2 + 2) >> 2
            self.s[ch] = [x0, x1, u, v1]
            out |= (y & 0xFFFF) << sh
        return out


# libultra's osViClock for each TV system: the AI's DAC divides it (rate = osViClock / (AI_DACRATE + 1))
VI_CLOCK = {"ntsc": 48681812, "pal": 49656530, "mpal": 48628316}


def step_of(clock, dacrate):
    """The runtime's step for an AI_DACRATE write (n64rt_rsp.c: osAiSetFrequency(osViClock / (DACRATE + 1)), then the step)."""
    f = clock // (dacrate + 1)
    dac = (clock + f // 2) // f
    real = clock // dac
    return real, (real << 16) // 48000


def main():
    path = sys.argv[1]
    tv = sys.argv[sys.argv.index("--tv") + 1] if "--tv" in sys.argv else "ntsc"
    lcg = "--lcg" in sys.argv
    snap = "--snap" in sys.argv
    frames, events, snaps = [], [], []
    dacs = []                                   # (DACRATE written, the step that followed it)
    want_dac = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == "W" and len(t) >= 4 and int(t[2], 16) == 0x10:
            want_dac = int(t[3], 16) & 0x3FFF
        elif t[0] == "S" and want_dac is not None:
            dacs.append((want_dac, int(t[1])))
            want_dac = None
        if t[0] == "F":
            frames.append(int(t[1], 16))
        elif t[0] == "S":
            events.append(("S", int(t[1])))
        elif t[0] == "O":
            events.append(("O", int(t[1], 16), int(t[2])))
        elif t[0] == "B":
            snaps += [int(x, 16) for x in t[3:]]
        elif t[0] == "R":
            events.append(("R",))
        elif t[0] == "I":
            events.append(("I",))
    bad = 0
    for d, st in dacs:
        real, want = step_of(VI_CLOCK[tv], d)
        ok = abs(st - want) <= 0     # (the runtime's own arithmetic: exact)
        print("AI_DACRATE %d -> DAC %d Hz, step %d (%s clock %d Hz: want %d)%s" % (d, real, st, tv.upper(), VI_CLOCK[tv], want, "" if ok else "  DIFFERENT"))
        if not ok:
            bad += 1
    if lcg:
        want = lcg_words(len(frames))
        n = next((i for i, (a, b) in enumerate(zip(frames, want)) if a != b), None)
        print("frames fetched: %d; the test's pattern: %s" % (len(frames), "same" if n is None else "DIFFERENT at frame %d" % n))
        if n is not None:
            bad += 1
    if snap:
        n = next((i for i, (a, b) in enumerate(zip(frames, snaps)) if a != b), None)
        print("frames fetched: %d, handed over: %d; what the reader fetched is %s" %
              (len(frames), len(snaps), "what the game handed over" if n is None and len(frames) <= len(snaps)
               else "NOT what was handed over (first difference at frame %s)" % n))
        if n is not None or len(frames) > len(snaps):
            bad += 1
    soft = Soft(frames)
    lp = LowPass() if "--lp" in sys.argv else None
    step = 43690
    real = silent = silent_bad = 0
    se = 0.0
    mx = 0
    first_bad = None
    nints = sum(1 for e in events if e[0] == "I")
    out_words = []
    for e in events:
        if e[0] == "S":
            step = e[1]
        elif e[0] == "R":
            soft.have = False
        elif e[0] == "O":
            if e[2]:
                if lp:
                    lp.reset()
                silent += 1
                if e[1] != 0:
                    silent_bad += 1
                continue
            try:
                want = soft.sample(step)
            except IndexError:
                print("the model wants a frame the hardware never fetched (sample %d)" % real)
                bad += 1
                break
            if lp:
                want = lp.word(want)
            got = e[1]
            real += 1
            for sh in (16, 0):
                d = s16(got >> sh) - s16(want >> sh)
                se += d * d
                mx = max(mx, abs(d))
            if got != want and first_bad is None:
                first_bad = (real - 1, got, want)
    rms = math.sqrt(se / (2 * real)) if real else 0.0
    print("output words: %d real, %d silence (%d not zero); frames fetched %d, used by the model %d; interrupts %d"
          % (real, silent, silent_bad, len(frames), soft.i, nints))
    print("hardware vs the software's algorithm: RMS error %.3f, max error %d (16-bit samples)" % (rms, mx))
    if first_bad:
        print("first difference: sample %d got %08x want %08x" % first_bad)
    if mx != 0 or silent_bad or bad or real == 0:
        print("ai_check: DIFFERENT")
        return 1
    print("ai_check: same stream")
    return 0


if __name__ == "__main__":
    sys.exit(main())
