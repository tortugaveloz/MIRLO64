/* The console's RSP timing (rsp_time.h). */
#include "rsp_time.h"
#include <string.h>
#include <stdlib.h>

/* what an instruction reads and writes, for the pipeline's rules */
typedef struct {
    uint32_t gr, gw;                    /* GPRs read; GPRs written late (loads, MFC0, MFC2, CFC2) */
    uint32_t vr, vw, vf;                /* vector registers read, written; vf: a lane field the pairing check takes for a register */
    uint8_t  cr, cw;                    /* control registers read, written (VCO 1, VCC 2, VCE 4) */
    uint8_t  vec, ld, st, br, grp;      /* a computation; a load; a store; a branch, jump or BREAK; MTC2, LTV or VNOP */
} ins_t;

static ins_t decode(uint32_t in)
{
    ins_t d;
    memset(&d, 0, sizeof d);
    unsigned op = in >> 26, rs = (in >> 21) & 31u, rt = (in >> 16) & 31u, rd = (in >> 11) & 31u, fn = in & 63u;
    unsigned vt = rt, vs = rd, vd = (in >> 6) & 31u;
    switch (op) {
    case 0x00:
        if (fn == 0x00 || fn == 0x02 || fn == 0x03) d.gr = 1u << rt;
        else if (fn == 0x04 || fn == 0x06 || fn == 0x07) d.gr = 1u << rt | 1u << rs;
        else if (fn == 0x08 || fn == 0x09) { d.gr = 1u << rs; d.br = 1; }
        else if (fn == 0x0D) d.br = 1;
        else if ((fn >= 0x20 && fn <= 0x27) || fn == 0x2A || fn == 0x2B) d.gr = 1u << rs | 1u << rt;
        break;
    case 0x01: if (rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11) { d.gr = 1u << rs; d.br = 1; } break;
    case 0x06: case 0x07: d.gr = 1u << rs; d.br = 1; break;
    case 0x02: case 0x03: d.br = 1; break;
    case 0x04: case 0x05: d.gr = 1u << rs | 1u << rt; d.br = 1; break;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: d.gr = 1u << rs; break;
    case 0x10:
        if (rs == 0) { d.gw = 1u << rt; d.ld = d.st = 1; }
        else if (rs == 4) { d.gr = 1u << rt; d.ld = d.st = 1; }
        break;
    case 0x12:
        if (!(in & (1u << 25))) {
            uint8_t c = (uint8_t)(1u << ((rd & 3u) == 3u ? 2u : (rd & 3u)));   /* (rd & 3 of 3: VCE, as it moves) */
            if (rs == 0) { d.gw = 1u << rt; d.vr = 1u << vs; d.ld = d.st = 1; }
            else if (rs == 2) { d.gw = 1u << rt; d.cr = c; d.ld = d.st = 1; }
            else if (rs == 4) { d.gr = 1u << rt; d.vw = 1u << vs; d.ld = d.st = 1; d.grp = 1; }
            else if (rs == 6) { d.gr = 1u << rt; d.cw = c; d.ld = d.st = 1; }
            break;
        }
        d.vec = 1;
        if (fn <= 0x0F) {                       /* the multiplies, VRNDP / VRNDN (vt only), VMACQ (nothing read) */
            d.vw = 1u << vd;
            if (fn == 0x02 || fn == 0x0A) d.vr = 1u << vt;
            else if (fn != 0x0B) d.vr = 1u << vs | 1u << vt;
        } else if (fn == 0x10 || fn == 0x11 || (fn >= 0x13 && fn <= 0x15)) {
            d.vw = 1u << vd; d.vr = 1u << vs | 1u << vt; d.cr = d.cw = 1;
        } else if (fn == 0x1D) d.vw = 1u << vd;
        else if ((fn >= 0x20 && fn <= 0x23) || fn == 0x27) { d.vw = 1u << vd; d.vr = 1u << vs | 1u << vt; d.cr = d.cw = 3; }
        else if (fn >= 0x24 && fn <= 0x26) { d.vw = 1u << vd; d.vr = 1u << vs | 1u << vt; d.cr = d.cw = 7; }
        else if (fn >= 0x28 && fn <= 0x2D) { d.vw = 1u << vd; d.vr = 1u << vs | 1u << vt; }
        else if (fn >= 0x30 && fn <= 0x36) { d.vw = 1u << vd; d.vr = 1u << vt; d.vf = 1u << vs; }   /* one lane: vs is its lane */
        else if (fn == 0x37) { d.vf = 1u << vd; d.grp = 1; }                                       /* VNOP */
        break;
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: case 0x27:
        d.gr = 1u << rs; d.gw = 1u << rt; d.ld = 1; break;
    case 0x28: case 0x29: case 0x2B:
        d.gr = 1u << rs | 1u << rt; d.st = 1; break;
    case 0x32:
        if (rd <= 0x09 || rd == 0x0B) {
            d.gr = 1u << rs; d.ld = 1;
            d.vw = rd == 0x0B ? 0xFFu << (vt & ~7u) : 1u << vt;
            d.grp = rd == 0x0B;
        }
        break;
    case 0x3A:
        if (rd <= 0x0B) { d.gr = 1u << rs; d.st = 1; d.vr = rd == 0x0B ? 0xFFu << (vt & ~7u) : 1u << vt; }
        break;
    default: break;
    }
    d.gr &= ~1u; d.gw &= ~1u;
    return d;
}

/* b, next to a, issues with it */
static int pairs(const ins_t *a, const ins_t *b)
{
    if (a->vec == b->vec || a->br) return 0;
    if (a->vw & (b->vr | b->vw)) return 0;
    if (a->cw & (b->cr | b->cw)) return 0;
    /* the false conflict: the lane field of a one-lane op (or VNOP's vd) taken for a register written by the first --
     * for VNOP only behind MTC2 or LTV */
    if ((a->grp || !b->grp) && (a->vw & b->vf)) return 0;
    return 1;
}

static ins_t slot(uint32_t in0, uint32_t in1, int n)
{
    ins_t u = decode(in0);
    if (n == 2) {
        ins_t b = decode(in1);
        u.gr |= b.gr; u.gw |= b.gw; u.vr |= b.vr; u.vw |= b.vw; u.ld |= b.ld; u.st |= b.st; u.br |= b.br;
    }
    return u;
}

void rsp_time_reset(rsp_time_t *t)
{
    memset(t, 0, sizeof *t);
    /* the DMA: about 3.7 bytes a CPU cycle (n64brew), 5.5 a cycle of this clock, and a few cycles to start */
    t->dma_c8 = getenv("CON_DMA_C8") ? atof(getenv("CON_DMA_C8")) : 1.44;
    t->dma_row = getenv("CON_DMA_ROW") ? (uint32_t)atoi(getenv("CON_DMA_ROW")) : 0;
    t->dma_fix = getenv("CON_DMA_FIX") ? (uint32_t)atoi(getenv("CON_DMA_FIX")) : 8;
}

int rsp_time_pair(const rsp_time_t *t, uint32_t in0, uint32_t in1)
{
    if (t->single || t->delay_taken) return 0;
    ins_t a = decode(in0), b = decode(in1);
    return pairs(&a, &b);
}

int rsp_time_ready(const rsp_time_t *t, uint32_t in0, uint32_t in1, int n)
{
    if (t->bubble) return 0;
    ins_t u = slot(in0, in1, n);
    if (u.gr & (t->h_gw[0] | t->h_gw[1])) return 0;
    if (u.vr & (t->h_vw[0] | t->h_vw[1] | t->h_vw[2])) return 0;
    if (u.st && t->h_ld[1]) return 0;
    return 1;
}

static void push(rsp_time_t *t, uint32_t ld, uint32_t gw, uint32_t vw)
{
    t->h_ld[2] = t->h_ld[1]; t->h_ld[1] = t->h_ld[0]; t->h_ld[0] = ld;
    t->h_gw[2] = t->h_gw[1]; t->h_gw[1] = t->h_gw[0]; t->h_gw[0] = gw;
    t->h_vw[2] = t->h_vw[1]; t->h_vw[1] = t->h_vw[0]; t->h_vw[0] = vw;
    t->now++;
}

void rsp_time_idle(rsp_time_t *t)
{
    if (t->bubble) {
        t->bubble = 0; t->bubbles++;
        if (t->odd_next) t->single = 1;                 /* a target at an odd word issues alone */
    } else t->stalls++;
    push(t, 0, 0, 0);
}

void rsp_time_issue(rsp_time_t *t, uint32_t in0, uint32_t in1, int n, int taken, uint32_t next_pc)
{
    ins_t u = slot(in0, in1, n);
    int delay = t->delay_taken;
    push(t, u.ld, u.gw, u.vw);
    t->slots++; t->pairs += (uint64_t)(n == 2);
    t->single = 0; t->delay_taken = 0;
    if (u.br) { t->single = 1; t->delay_taken = taken; }   /* the delay slot is next */
    else if (delay) { t->bubble = 1; t->odd_next = (next_pc & 4u) != 0; }
}

/* ---- rsp_time_slot: a slot on an rsp_t, with the DMA's timeline */
static uint32_t word(const rsp_t *s, uint32_t pc)
{
    pc &= 0xFFCu;
    return (uint32_t)s->imem[pc] << 24 | s->imem[pc + 1] << 16 | s->imem[pc + 2] << 8 | s->imem[pc + 3];
}

static uint64_t dma_cycles(const rsp_time_t *t, uint32_t len)
{
    uint32_t units = ((len & 0xFFFu) >> 3) + 1u, rows = ((len >> 12) & 0xFFu) + 1u;
    return t->dma_fix + (uint64_t)rows * (t->dma_row + (uint64_t)(units * t->dma_c8 + 0.5));
}

static void dma_update(rsp_time_t *t, rsp_t *s)
{
    if (t->dma_q && t->now >= t->dma_end) { t->dma_end += t->dma_qlen; t->dma_q = 0; }
    s->dma_busy = t->now < t->dma_end || t->dma_q;
    s->dma_full = (uint32_t)t->dma_q;
    if (s->dma_busy) t->dma_wait++;
}

/* an instruction of the slot: a write of SP_RD_LEN / SP_WR_LEN starts or queues a transfer */
static int step(rsp_time_t *t, rsp_t *s, uint32_t in)
{
    int len_w = (in >> 26) == 0x10 && ((in >> 21) & 31u) == 4 && (((in >> 11) & 15u) == 2 || ((in >> 11) & 15u) == 3);
    uint32_t v = s->r[(in >> 16) & 31u];
    int r = rsp_step(s);
    if (len_w) {
        uint64_t c = dma_cycles(t, v);
        if (t->now >= t->dma_end && !t->dma_q) t->dma_end = t->now + c;
        else { t->dma_q = 1; t->dma_qlen = c; }
    }
    return r;
}

int rsp_time_slot(rsp_time_t *t, rsp_t *s)
{
    uint32_t in0 = word(s, s->pc), in1 = word(s, s->pc + 4u);
    int n = rsp_time_pair(t, in0, in1) ? 2 : 1;
    for (;;) {
        dma_update(t, s);
        if (rsp_time_ready(t, in0, in1, n)) break;
        rsp_time_idle(t);
    }
    int r = step(t, s, in0);
    if (!r && n == 2) r = step(t, s, in1);
    rsp_time_issue(t, in0, in1, n, s->delay, s->pc);
    if (r) return r;
    /* (the bubble after a taken branch's delay slot) */
    while (t->bubble) { dma_update(t, s); rsp_time_idle(t); }
    return 0;
}
