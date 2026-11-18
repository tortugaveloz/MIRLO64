/* The console's RDP timing, as a model beside the RDP's reference model (lang/c/rdp): in which cycles of the RDP's
 * 62.5 MHz clock (the RCP's, one DPC_CLOCK tick) the N64 executes each command. Every rule says where it comes from
 * and how far it can be trusted in rdp_time.c; rtl/rdp/rdp_top.v's PACE holds the RTL to the same rules, rdp_task -t
 * reports a captured task by them and tb_rdp (built with -DRDP_CTIME) sets the RTL's time against them.
 *
 * What it decides from: a command's words, the RDP's registers (other modes, the colour and depth image) and, for a
 * primitive, the spans the reference model walks (rdp_span_hook) and the words of memory it writes
 * (rdp_time_write): nothing of the pixels' values but whether a word of memory is written.
 *
 *   rdp_time_begin(t)                      before a command's first word reaches the reference model
 *   rdp_time_span(t, y, x0, x1)            each span a primitive draws (from the reference model's hook)
 *   rdp_time_write(t, byte_address)        each 16-bit word the reference model writes of the RDRAM
 *   rdp_time_cmd(t, r, w, n, with_mem)     the command's cycles, once the reference model has executed it
 *
 * with_mem = 0 gives the pipeline's own time (the compute rate: a pixel a clock, nothing waits for RDRAM), 1 the time
 * with the RDRAM's (the 16-byte transfers of a span's colour and depth words). */
#ifndef RDP_TIME_H
#define RDP_TIME_H

#include <stdint.h>
#include "rdp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rdp_time_span { int y, x0, x1; } rdp_time_span_t;

typedef struct rdp_time {
    rdp_time_span_t *sp;                /* the spans of the primitive being executed */
    unsigned n_sp, cap_sp;
    uint8_t  *wseg;                     /* the 16-byte groups of the RDRAM written since rdp_time_begin (a bit each) */
    uint32_t *wlist;                    /* ... and which, to clear them */
    unsigned n_w, cap_w;
} rdp_time_t;

void     rdp_time_init(rdp_time_t *t);
void     rdp_time_free(rdp_time_t *t);
void     rdp_time_begin(rdp_time_t *t);
void     rdp_time_span(rdp_time_t *t, int y, int x0, int x1);
void     rdp_time_write(rdp_time_t *t, uint32_t byte_address);
uint64_t rdp_time_cmd(rdp_time_t *t, const rdp_t *r, const uint32_t *w, unsigned n, int with_mem);

#ifdef __cplusplus
}
#endif
#endif
