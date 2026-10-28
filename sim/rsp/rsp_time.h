/* The console's RSP timing, as a model beside rsp.c: in which cycles of the
 * RSP's 62.5 MHz clock the N64 issues a program's instructions, from the
 * pipeline's rules as the n64brew wiki gives them (Reality Signal Processor /
 * CPU Pipeline). rsp_task -t reports a task's cycles by it; tb_rsp CYCLES=1
 * holds the RTL to it cycle by cycle.
 *
 *   issue    a slot a cycle: one instruction, or two when a vector
 *            computation and a scalar-unit instruction stand next to each other
 *            (either first) and the first writes no vector or control register
 *            the second reads or writes; never the delay slot, a branch first,
 *            or a taken branch's target at an odd word (IMEM is read 8 bytes
 *            at a time)
 *   latency  a vector register written is ready 4 cycles on (a computation's,
 *            a load's, MTC2's); a GPR written by a load, MFC0, MFC2 or CFC2 3
 *            cycles on, any other the next. A slot that reads one sooner waits,
 *            both of a pair
 *   stores   a store exactly 2 cycles after a load waits (MFC0, MTC0, MFC2,
 *            MTC2, CFC2 and CTC2 count as both)
 *   branch   a taken branch: a cycle of nothing after its delay slot
 *
 * The rules decide from the instruction words alone, so a bench can run them
 * next to a functional model: rsp_time_pair / _ready / _issue / _idle, a call
 * a cycle. rsp_time_slot does it all for one slot on an rsp_t, with the DMA
 * on a timeline of its own (SP_DMA_BUSY / FULL read as the console's would,
 * so the microcode's waits spin as long as there). */
#ifndef RSP_TIME_H
#define RSP_TIME_H

#include "rsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsp_time {
    uint64_t now;                       /* cycles so far */
    uint64_t slots, pairs, stalls, bubbles;
    uint32_t h_ld[3];                   /* the cycles before (0 the last): a load issued */
    uint32_t h_gw[3];                   /* ... GPRs it writes late (loads, MFC0, MFC2, CFC2) */
    uint32_t h_vw[3];                   /* ... vector registers written */
    int      single;                    /* the next slot is one instruction (a delay slot; an odd branch target) */
    int      delay_taken;               /* the next slot is a taken branch's delay slot */
    int      bubble;                    /* this cycle is the bubble after one */
    int      odd_next;                  /* (with bubble) the target is at an odd word */
    /* (rsp_time_slot) the DMA: the running transfer ends at dma_end, a queued one takes dma_qlen after it */
    uint64_t dma_end, dma_qlen;
    int      dma_q;
    double   dma_c8;                    /* cycles per 8 bytes */
    uint32_t dma_row, dma_fix;          /* cycles a row, a transfer */
    uint64_t dma_wait;                  /* cycles spent while a DMA ran */
} rsp_time_t;

void rsp_time_reset(rsp_time_t *t);
/* the console would issue in1 with in0 (next to it) */
int  rsp_time_pair(const rsp_time_t *t, uint32_t in0, uint32_t in1);
/* a slot of n (1, 2) instructions may issue this cycle */
int  rsp_time_ready(const rsp_time_t *t, uint32_t in0, uint32_t in1, int n);
/* it issues: taken = its branch is taken, next_pc = the instruction after it */
void rsp_time_issue(rsp_time_t *t, uint32_t in0, uint32_t in1, int n, int taken, uint32_t next_pc);
/* a cycle with nothing issued */
void rsp_time_idle(rsp_time_t *t);
/* one slot of s (one or two rsp_step()s, the cycles it waits first); returns 1 at BREAK / halt */
int  rsp_time_slot(rsp_time_t *t, rsp_t *s);

#ifdef __cplusplus
}
#endif

#endif
