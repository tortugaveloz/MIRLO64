/* GameShark codes: a text file's lines parsed, and applied to a memory the caller gives. A pure function of the file's
 * bytes and of that memory (no hardware: n64rt_cheat.c has the runtime's side), which a host test calls too
 * (test/gs_host.c).
 *
 *   # comment                         everything after a "#", ";" or "//" at the start of a line; blank lines are skipped
 *   80123456 0063  Infinite lives     a code (8 hex digits, a space, 4 hex digits); what follows is the description
 *
 * A line that is not a code is skipped. The codes, by the first byte (the rest is an address in the RDRAM's KSEG0: 8000_0000 + the
 * 24 bits; the value is the four digits after the space):
 *   80 / A0 / F0   writes the byte  (the low 8 bits of the value)
 *   81 / A1 / F1   writes the 16-bit value (its address even)
 *   D0 / D1        the next line is run only if the byte / the 16-bit value at the address is the value
 *   D2 / D3        ... only if it is not
 *   50 NN SSSS VVVV   repeats the next line (an 80, 81, A0, A1, F0 or F1) NN times: the address a further SSSS bytes each time, the
 *                  value a further VVVV
 * Anything else (the GS button's 88 and 89 among them: there is no button) is skipped. The codes are run in the file's order, all of them,
 * every time they are applied (a field). */
#ifndef __N64RT_GS_H
#define __N64RT_GS_H

typedef struct { unsigned char type; unsigned char pad; unsigned short val; unsigned addr; } n64rt_gs_code_t;    /* addr: the 24 bits */

/* the caller's memory: the byte / half-word at a RDRAM offset (0 to 0x7FFFFF), and writes (1 when the value changed) */
typedef struct {
    unsigned (*rd8)(unsigned a);
    unsigned (*rd16)(unsigned a);
    int      (*wr8)(unsigned a, unsigned v);
    int      (*wr16)(unsigned a, unsigned v);
} n64rt_gs_mem_t;

/* `txt`: the file's `len` bytes. Returns the codes found (at most `max`, in `out`). Nothing in the bytes can make it read beyond `len`. */
unsigned n64rt_gs_parse(const char *txt, unsigned len, n64rt_gs_code_t *out, unsigned max);
/* runs the `n` codes; returns how many writes changed a value */
unsigned n64rt_gs_apply(const n64rt_gs_code_t *c, unsigned n, const n64rt_gs_mem_t *m);

#endif
