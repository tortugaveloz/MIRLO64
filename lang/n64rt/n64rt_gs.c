/* GameShark codes: the parse and the run (n64rt_gs.h). */
#include "n64rt_gs.h"

static int hex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

unsigned n64rt_gs_parse(const char *t, unsigned len, n64rt_gs_code_t *out, unsigned max)
{
    unsigned n = 0, i = 0;
    while (i < len && n < max) {
        unsigned e = i;
        while (e < len && t[e] != '\n' && t[e] != '\r') e++;            /* the line is [i, e) */
        unsigned p = i;
        while (p < e && (t[p] == ' ' || t[p] == '\t')) p++;
        unsigned long hi = 0, lo = 0;
        unsigned d = 0;
        while (p + d < e && d < 8u && hex(t[p + d]) >= 0) hi = hi << 4 | (unsigned)hex(t[p + d++]);
        if (d == 8u && p + d < e && (t[p + d] == ' ' || t[p + d] == '\t' || t[p + d] == ':')) {
            unsigned q = p + d;
            while (q < e && (t[q] == ' ' || t[q] == '\t' || t[q] == ':')) q++;
            unsigned f = 0;
            while (q + f < e && f < 4u && hex(t[q + f]) >= 0) lo = lo << 4 | (unsigned)hex(t[q + f++]);
            if (f == 4u && (q + f == e || t[q + f] == ' ' || t[q + f] == '\t' || t[q + f] == '#' || t[q + f] == ';' || t[q + f] == '/' || t[q + f] == '-')) {
                out[n].type = (unsigned char)(hi >> 24);
                out[n].pad = 0;
                out[n].val = (unsigned short)lo;
                out[n].addr = (unsigned)(hi & 0xFFFFFFu);
                n++;
            }
        }
        i = e;
        while (i < len && (t[i] == '\n' || t[i] == '\r')) i++;
    }
    return n;
}

static int is_write(unsigned char t) { return t == 0x80 || t == 0x81 || t == 0xA0 || t == 0xA1 || t == 0xF0 || t == 0xF1; }
static int wide(unsigned char t) { return t == 0x81 || t == 0xA1 || t == 0xF1; }

static int write_one(unsigned char type, unsigned addr, unsigned val, const n64rt_gs_mem_t *m)
{
    addr &= 0x7FFFFFu;
    if (wide(type)) return (addr & 1u) ? 0 : m->wr16(addr, val & 0xFFFFu);
    return m->wr8(addr, val & 0xFFu);
}

unsigned n64rt_gs_apply(const n64rt_gs_code_t *c, unsigned n, const n64rt_gs_mem_t *m)
{
    unsigned changed = 0;
    for (unsigned i = 0; i < n; i++) {
        const unsigned char t = c[i].type;
        const unsigned a = c[i].addr & 0x7FFFFFu;
        if (is_write(t)) {
            changed += (unsigned)write_one(t, a, c[i].val, m);
        } else if (t >= 0xD0 && t <= 0xD3) {
            unsigned now = (t == 0xD0 || t == 0xD2) ? m->rd8(a) : (a & 1u) ? ~(unsigned)c[i].val : m->rd16(a);
            unsigned want = (t == 0xD0 || t == 0xD2) ? (c[i].val & 0xFFu) : c[i].val;
            int ok = (t == 0xD0 || t == 0xD1) ? now == want : now != want;
            if (!ok && i + 1 < n) i += c[i + 1].type == 0x50 ? 2u : 1u;   /* (the line it governs, or the repeater's two) */
        } else if (t == 0x50 && i + 1 < n && is_write(c[i + 1].type)) {
            const unsigned count = (c[i].addr >> 16) & 0xFFu, step = c[i].addr & 0xFFFFu, vstep = c[i].val;
            const n64rt_gs_code_t *w = &c[i + 1];
            for (unsigned k = 0; k < count; k++)
                changed += (unsigned)write_one(w->type, w->addr + k * step, w->val + k * vstep, m);
            i++;
        }
    }
    return changed;
}
