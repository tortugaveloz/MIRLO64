#include <stdio.h>
/* The Mirlo-N64 CPU: a MIPS executing the ROM's code as it is.
 *
 * Integer state is 64 bits, exactly as the VR4300's: libultra copies pairs
 * of 32-bit words with one ld/sd (__osException moves a thread's saved SR
 * and PC that way) and its long long helpers (__ll_mul, __ull_div, ...)
 * build 64-bit values in single registers. What stays 32 bits is the fast
 * path: IDO-compiled code is almost only 32-bit operations, whose results
 * are sign-extended; the 64-bit ones are rare (counted by -p) and in the
 * hardware may take a few cycles more on a 32-bit datapath.
 *
 * The FPU keeps the FR=0 layout (32 32-bit registers, a double in an
 * even/odd pair) and computes doubles as floats (owner rule).
 *
 * COP0 is what libultra touches: Status/Cause/EPC/Count/Compare, eret; the
 * TLB answers "no entry"; cache operations do nothing. See n64.h for the
 * memory layout.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "n64.h"

#define RS(i) (((i) >> 21) & 31)
#define RT(i) (((i) >> 16) & 31)
#define RD(i) (((i) >> 11) & 31)
#define SA(i) (((i) >> 6) & 31)
#define SX(x) ((uint64_t)(int64_t)(int32_t)(uint32_t)(x))     /* a 32-bit result, sign-extended */
#define IMM(i) ((uint64_t)(int64_t)(int16_t)(i))
#define IMMU(i) ((uint64_t)((i) & 0xFFFFu))

#define SR_IE   0x00000001u
#define SR_EXL  0x00000002u
#define SR_ERL  0x00000004u
#define SR_BEV  0x00400000u
#define CAUSE_BD 0x80000000u
#define CAUSE_IP2 0x00000400u   /* the RCP (MI) */
#define CAUSE_IP7 0x00008000u   /* Count == Compare */

uint64_t n64_op64_count[8];
/* every executed 64-bit instruction, by address (the patch census: N64_OP64_SITES=1) */
#define N64_SITES 4096
struct n64_site n64_sites[N64_SITES];
int n64_nsites;
int n64_no64 = -1;     /* N64_NO64=1: a 64-bit instruction is fatal (the 32-bit CPU has none) */
void n64_op64_site(uint32_t pc, uint32_t insn)
{
    if (n64_no64 < 0) n64_no64 = getenv("N64_NO64") != NULL;
    if (n64_no64) { fprintf(stderr, "N64_NO64: a 64-bit instruction %08x at %08x\n", insn, pc); exit(3); }
    for (int i = 0; i < n64_nsites; i++) if (n64_sites[i].pc == pc) { n64_sites[i].count++; return; }
    if (n64_nsites < N64_SITES) n64_sites[n64_nsites++] = (struct n64_site){ pc, insn, 1 };
}     /* executed 64-bit instructions, by kind (see n64.h) */
uint64_t n64_tlb_dropped;       /* TLB writes beyond N64_TLB_ENTRIES */
uint64_t n64_tlb_hits;          /* translated (mapped) accesses */
int      n64_tlb_maxi = -1;     /* the highest entry that translated one */
uint64_t n64_noncanon_src;      /* instructions reading a register whose high half is not its low's sign */

/* ---- the FPU's number formats (float only) ----------------------------- */
typedef union { uint32_t u; float f; } fu_t;

static float d2f(uint32_t lo, uint32_t hi)   /* a double's bits -> float (see recomp.h) */
{
    fu_t r;
    uint32_t s = hi & 0x80000000u;
    int32_t e = (int32_t)((hi >> 20) & 0x7FF);
    if (e == 0x7FF) {
        r.u = s | 0x7F800000u | ((hi & 0xFFFFF) << 3) | (lo >> 29) | ((lo & 0x1FFFFFFFu) != 0);
        return r.f;
    }
    e += 127 - 1023;
    if (e <= 0) { r.u = s; return r.f; }
    if (e >= 255) { r.u = s | 0x7F800000u; return r.f; }
    r.u = (s | ((uint32_t)e << 23) | ((hi & 0xFFFFF) << 3) | (lo >> 29)) + ((lo >> 28) & 1);
    return r.f;
}

static void f2d(float v, uint32_t *lo, uint32_t *hi)   /* float -> a double's bits */
{
    fu_t in = { .f = v };
    uint32_t s = in.u & 0x80000000u, e = (in.u >> 23) & 0xFF, m = in.u & 0x7FFFFF;
    if (e == 0) { *hi = s; *lo = 0; return; }
    *hi = s | ((e == 0xFF ? 0x7FFu : e - 127 + 1023) << 20) | (m >> 3);
    *lo = m << 29;
}

static float fs_get(n64_cpu_t *c, int i) { fu_t u = { .u = c->f[i] }; return u.f; }
static void  fs_set(n64_cpu_t *c, int i, float v) { fu_t u = { .f = v }; c->f[i] = u.u; }
static float fpd_get(n64_cpu_t *c, int i) { i &= ~1; return d2f(c->f[i], c->f[i + 1]); }
static void  fpd_set(n64_cpu_t *c, int i, float v) { i &= ~1; f2d(v, &c->f[i], &c->f[i + 1]); }
static int64_t fl_get(n64_cpu_t *c, int i) { i &= ~1; return (int64_t)((uint64_t)c->f[i + 1] << 32 | c->f[i]); }
static void    fl_set(n64_cpu_t *c, int i, int64_t v) { i &= ~1; c->f[i] = (uint32_t)v; c->f[i + 1] = (uint32_t)((uint64_t)v >> 32); }

static float fround(float x, int rm)   /* FCR31 rounding modes */
{
    switch (rm & 3) {
    case 0: return rintf(x);            /* nearest, ties to even (the host's default) */
    case 1: return truncf(x);
    case 2: return ceilf(x);
    default: return floorf(x);
    }
}

static int32_t f2w(float x)   /* saturating */
{
    if (x != x || x >= 2147483648.0f) return INT32_MAX;
    if (x < -2147483648.0f) return INT32_MIN;
    return (int32_t)x;
}

static int64_t f2l(float x)
{
    if (x != x || x >= 9223372036854775808.0f) return INT64_MAX;
    if (x < -9223372036854775808.0f) return INT64_MIN;
    return (int64_t)x;
}

/* ---- exceptions -------------------------------------------------------- */
static void exception_vec(n64_t *n, uint32_t pc, int in_delay, int code, int ce, int refill)
{
    n64_cpu_t *c = &n->cpu;
    uint32_t *sr = &c->cop0[C0_STATUS], *cause = &c->cop0[C0_CAUSE];
    int exl = (*sr & SR_EXL) != 0;
    if (!exl) {
        c->cop0[C0_EPC] = in_delay ? pc - 4 : pc;
        *cause = in_delay ? (*cause | CAUSE_BD) : (*cause & ~CAUSE_BD);
    }
    *cause = (*cause & ~0x3000007Cu) | ((uint32_t)code << 2) | ((uint32_t)ce << 28);
    *sr |= SR_EXL;
    uint32_t vec = (*sr & SR_BEV) ? 0xBFC00200u : 0x80000000u;
    vec += (refill && !exl) ? 0 : 0x180u;
    c->pc = vec;
    c->next_pc = vec + 4;
    c->in_delay = 0;
}

static void exception(n64_t *n, uint32_t pc, int in_delay, int code, int ce)
{
    exception_vec(n, pc, in_delay, code, ce, 0);
}

/* a TLB fault on va: BadVAddr, Context and EntryHi take the page, then the
 * refill vector (a miss) or the general one */
static void tlb_fault(n64_t *n, uint32_t pc, int dly, uint32_t va, int kind, int write)
{
    n64_cpu_t *c = &n->cpu;
    c->cop0[C0_BADVADDR] = va;
    c->cop0[C0_CONTEXT] = (c->cop0[C0_CONTEXT] & 0xFF800000u) | ((va >> 13) << 4);
    c->cop0[C0_ENTRYHI] = (va & 0xFFFFE000u) | (c->cop0[C0_ENTRYHI] & 0xFFu);
    int code = kind == N64_TLB_MOD ? EXC_MOD : write ? EXC_TLBS : EXC_TLBL;
    exception_vec(n, pc, dly, code, 0, kind == N64_TLB_MISS);
}

void n64_cpu_reset(n64_t *n)
{
    n64_cpu_t *c = &n->cpu;
    memset(c, 0, sizeof *c);
    c->cop0[C0_STATUS] = 0x34000000u;        /* CU0 | CU1 | FR, as the IPL leaves it */
    c->cop0[C0_PRID] = 0x00000B22u;          /* VR4300 */
    c->cop0[C0_CONFIG] = 0x7006E463u;
    c->cop0[C0_RANDOM] = 31;
    c->fcr31 = 0x01000800u;
    /* the TLB is undefined at power-up; kseg0 VPNs never match (as the RTL resets it) */
    for (int i = 0; i < N64_TLB_ENTRIES; i++) c->tlb[i].hi = 0x80000000u;
}

/* ---- memory access ----------------------------------------------------- */
static int load(n64_t *n, uint32_t pc, int dly, uint32_t a, int size, uint64_t *v)
{
    if (a & (size - 1)) {
        n->cpu.cop0[C0_BADVADDR] = a;
        exception(n, pc, dly, EXC_ADEL, 0);
        return -1;
    }
    uint32_t w, w2;
    int t = size == 8 ? n64_read(n, a, 4, &w) : n64_read(n, a, size, &w);
    if (!t && size == 8) t = n64_read(n, a + 4, 4, &w2);
    if (t <= N64_TLB_MISS) { tlb_fault(n, pc, dly, a, t, 0); return -1; }
    if (t) { N64_FATAL(n, "load fault at %08x (pc %08x)", a, pc); return -1; }
    *v = size == 8 ? (uint64_t)w << 32 | w2 : w;
    return 0;
}

static int store(n64_t *n, uint32_t pc, int dly, uint32_t a, int size, uint64_t v)
{
    if (a & (size - 1)) {
        n->cpu.cop0[C0_BADVADDR] = a;
        exception(n, pc, dly, EXC_ADES, 0);
        return -1;
    }
    int t = size == 8 ? n64_write(n, a, 4, (uint32_t)(v >> 32)) : n64_write(n, a, size, (uint32_t)v);
    if (!t && size == 8) t = n64_write(n, a + 4, 4, (uint32_t)v);
    if (t <= N64_TLB_MISS) { tlb_fault(n, pc, dly, a, t, 1); return -1; }
    if (t) { N64_FATAL(n, "store fault at %08x (pc %08x)", a, pc); return -1; }
    return 0;
}

/* lwl, lwr, swl, swr, ldl, ldr, sdl, sdr: one aligned access of `size` bytes (the doubleword's two words as ld's), whose TLB fault is the
 * instruction's own -- a store's for the store forms, TLBS or Mod, not the load's TLBL -- and at the effective address, unaligned */
static int partial(n64_t *n, uint32_t pc, int dly, uint32_t a, int size, int write, uint64_t *w)
{
    int t = n64_xlat_check(n, a & ~(uint32_t)(size - 1), write);
    if (t && t <= N64_TLB_MISS) { tlb_fault(n, pc, dly, a, t, write); return -1; }
    return write ? 0 : load(n, pc, dly, a & ~(uint32_t)(size - 1), size, w);
}

/* sc, scd with the link lost: nothing is stored, but the address is checked and translated as a store's (1: the exception was taken) */
static int sc_fault(n64_t *n, uint32_t pc, int dly, uint32_t a, int size)
{
    if (a & (size - 1)) { n->cpu.cop0[C0_BADVADDR] = a; exception(n, pc, dly, EXC_ADES, 0); return 1; }
    int t = n64_xlat_check(n, a, 1);
    if (t && t <= N64_TLB_MISS) { tlb_fault(n, pc, dly, a, t, 1); return 1; }
    return 0;
}

/* ---- COP1 (but its branches): shared with the co-simulation's FPU ------ */
uint64_t n64_fp_count[32][64];   /* COP1 instructions by fmt (rs) and function */
uint64_t n64_fp_denorm_in, n64_fp_denorm_out, n64_fp_nan_out;
static int fp_denorm(float x) { fu_t u = { .f = x }; return (u.u & 0x7F800000u) == 0 && (u.u & 0x7FFFFFu); }

/* Mirlo's FPU (rtl/mips/fpu_n64.sv) and this model agree bit for bit:
 * single precision, a double taken to float (d2f) and back (f2d); inputs
 * and results below the normals flushed to zero (the VR4300 with FCR31.FS,
 * as libultra sets it; a denormal input traps on the VR4300, here it is 0),
 * but for a single's compare, which orders them exactly as the VR4300 does;
 * every NaN result the default one, 0x7FBFFFFF; arithmetic rounds to nearest
 * even whatever FCR31.RM says (games keep the default). */
extern int n64_no64;
static float ftz(float x) { fu_t u = { .f = x }; if (!(u.u & 0x7F800000u)) u.u &= 0x80000000u; return u.f; }
static float canon(float x) { fu_t u = { .f = x }; if (x != x) u.u = 0x7FBFFFFFu; return u.f; }
/* an arithmetic result: rounded to 24 bits with an unbounded exponent, then
 * flushed below the normals (the host rounds a tiny one on the denormal grid:
 * where that reached the smallest normal, check the 24-bit rounding) */
static float fres(float r, double exact)
{
    fu_t u = { .f = r };
    if (r != r) return canon(r);
    if (!(u.u & 0x7F800000u)) { u.u &= 0x80000000u; return u.f; }
    if ((u.u & 0x7FFFFFFFu) == 0x00800000u && fabs(exact) < 0x1p-126 && rint(fabs(exact) * 0x1p150) < 16777216.0)
        u.u &= 0x80000000u;
    return u.f;
}

int n64_cop1_exec(n64_cpu_t *c, uint32_t insn, uint64_t rt, uint64_t *gpr_out)
{
    uint32_t rt32 = (uint32_t)rt;
    n64_fp_count[RS(insn)][insn & 63]++;
    {
        uint32_t fmt = RS(insn);
        int fs = RD(insn), ft = RT(insn), fd = SA(insn);
        switch (fmt) {
        case 0x00: *gpr_out = SX(c->f[fs]); return 1;                               /* mfc1 */
        case 0x01: if (n64_no64 > 0) return -1; *gpr_out = (uint64_t)fl_get(c, fs); return 1;   /* dmfc1: the pair */
        case 0x02: *gpr_out = SX(fs == 31 ? c->fcr31 : fs == 0 ? 0x00000B00u : 0); return 1;   /* cfc1 */
        case 0x04: c->f[fs] = rt32; break;                                          /* mtc1 */
        case 0x05: if (n64_no64 > 0) return -1; fl_set(c, fs, (int64_t)rt); break;  /* dmtc1 */
        case 0x06: if (fs == 31) c->fcr31 = rt32 & 0x0183FFFFu; break;              /* ctc1 */
        case 0x10: case 0x11: {                                                     /* .s / .d */
            int dbl = fmt == 0x11;
            float x = dbl ? fpd_get(c, fs) : fs_get(c, fs);
            float y = dbl ? fpd_get(c, ft) : fs_get(c, ft);
            float res;
            uint32_t f = insn & 63;
            if (fp_denorm(x) || (f < 4 && fp_denorm(y))) n64_fp_denorm_in++;
            const float rx = x, ry = y;                                             /* (a single's compare: unflushed) */
            x = ftz(x); y = ftz(y);
            if (n64_no64 > 0 && ((f >= 0x08 && f <= 0x0B) || f == 0x25)) return -1;   /* .l results */
            if ((f == 0x20 && !dbl) || (f == 0x21 && dbl)) return -1;                   /* cvt to its own format */
            switch (f) {
            case 0x00: res = fres(x + y, (double)x + y); break;
            case 0x01: res = fres(x - y, (double)x - y); break;
            case 0x02: res = fres(x * y, (double)x * y); break;
            case 0x03: res = fres(x / y, (double)x / y); break;
            case 0x04: res = fres(sqrtf(x), sqrt((double)x)); break;
            case 0x05: res = canon(fabsf(x)); break;
            case 0x06:                                                              /* mov: raw (.d: the pair) */
                if (dbl) { c->f[fd & ~1] = c->f[fs & ~1]; c->f[fd | 1] = c->f[fs | 1]; }
                else c->f[fd] = c->f[fs];
                goto fpu_done;
            case 0x07: res = canon(-x); break;
            case 0x08: fl_set(c, fd, f2l(fround(x, 0))); goto fpu_done;             /* round.l */
            case 0x09: fl_set(c, fd, f2l(truncf(x))); goto fpu_done;
            case 0x0A: fl_set(c, fd, f2l(ceilf(x))); goto fpu_done;
            case 0x0B: fl_set(c, fd, f2l(floorf(x))); goto fpu_done;
            case 0x0C: c->f[fd] = (uint32_t)f2w(fround(x, 0)); goto fpu_done;       /* round.w */
            case 0x0D: c->f[fd] = (uint32_t)f2w(truncf(x)); goto fpu_done;
            case 0x0E: c->f[fd] = (uint32_t)f2w(ceilf(x)); goto fpu_done;
            case 0x0F: c->f[fd] = (uint32_t)f2w(floorf(x)); goto fpu_done;
            case 0x20: fs_set(c, fd, canon(x)); goto fpu_done;                      /* cvt.s.d */
            case 0x21: fpd_set(c, fd, canon(x)); goto fpu_done;                     /* cvt.d.s */
            case 0x24: c->f[fd] = (uint32_t)f2w(fround(x, c->fcr31 & 3)); goto fpu_done;   /* cvt.w */
            case 0x25: fl_set(c, fd, f2l(fround(x, c->fcr31 & 3))); goto fpu_done;         /* cvt.l */
            default:
                if (f >= 0x30) {                                                    /* c.cond */
                    if (!dbl) { x = rx; y = ry; }
                    int un = x != x || y != y;
                    int cond = ((f & 1) && un) || ((f & 2) && !un && x == y) || ((f & 4) && !un && x < y);
                    c->fcr31 = cond ? (c->fcr31 | 0x00800000u) : (c->fcr31 & ~0x00800000u);
                    goto fpu_done;
                }
                return -1;
            }
            if (fp_denorm(res)) n64_fp_denorm_out++;
            if (res != res) {
                n64_fp_nan_out++;
                static int sites = -1;   /* N64_NANSITES=n: the first n NaN results, where */
                if (sites < 0) { const char *e = getenv("N64_NANSITES"); sites = e ? atoi(e) : 0; }
                if (sites > 0) { sites--; fprintf(stderr, "NaN: pc %08x fmt %s fn %02x x %g y %g (cycle %llu)\n", c->pc,
                                                  dbl ? "d" : "s", f, (double)x, (double)y, (unsigned long long)c->cycles); }
            }
            if (dbl) fpd_set(c, fd, res); else fs_set(c, fd, res);
        fpu_done:
            break;
        }
        case 0x14: case 0x15: {                                                     /* .w / .l -> s, d */
            if (fmt == 0x15 && n64_no64 > 0) return -1;
            float x = fmt == 0x14 ? (float)(int32_t)c->f[fs] : (float)fl_get(c, fs);
            uint32_t f = insn & 63;
            if (f == 0x20) fs_set(c, fd, x);
            else if (f == 0x21) fpd_set(c, fd, x);
            else return -1;
            break;
        }
        default: return -1;
        }
        return 0;
    }
}

/* ---- interrupts -------------------------------------------------------- */
int n64_cpu_irq_pending(n64_t *n)
{
    n64_cpu_t *c = &n->cpu;
    if (n->rcp.mi_intr & n->rcp.mi_mask) c->cop0[C0_CAUSE] |= CAUSE_IP2;
    else c->cop0[C0_CAUSE] &= ~CAUSE_IP2;
    uint32_t sr = c->cop0[C0_STATUS];
    return (sr & SR_IE) && !(sr & (SR_EXL | SR_ERL)) && (c->cop0[C0_CAUSE] & sr & 0xFF00u);
}

void n64_cpu_interrupt(n64_t *n)
{
    exception(n, n->cpu.pc, n->cpu.in_delay, EXC_INT, 0);
}

/* ---- the step ---------------------------------------------------------- */
void n64_cpu_step(n64_t *n)
{
    n64_cpu_t *c = &n->cpu;
    uint64_t *r = c->r;

    /* Count / Compare: Count advances at half the CPU clock */
    if (++c->count_frac == 2) {
        c->count_frac = 0;
        if (++c->cop0[C0_COUNT] == c->cop0[C0_COMPARE]) c->cop0[C0_CAUSE] |= CAUSE_IP7;
    }
    /* Random: down from 31 to Wired, every step */
    c->cop0[C0_RANDOM] = c->cop0[C0_RANDOM] <= (c->cop0[C0_WIRED] & 31u) ? 31u : c->cop0[C0_RANDOM] - 1u;

    /* interrupts, before the instruction at pc */
    if (!n->irq_external && n64_cpu_irq_pending(n)) {
        n64_cpu_interrupt(n);
        return;
    }

    uint32_t pc = c->pc, insn;
    int dly = c->in_delay;
    if (pc & 3) {
        c->cop0[C0_BADVADDR] = pc;
        exception(n, pc, dly, EXC_ADEL, 0);
        return;
    }
    {
        int t = n64_read(n, pc, 4, &insn);
        if (t <= N64_TLB_MISS) { tlb_fault(n, pc, dly, pc, t, 0); return; }
        if (t) { N64_FATAL(n, "fetch fault at %08x", pc); return; }
    }
    if (n->trace) fprintf(n->log, "%08x: %08x\n", pc, insn);

    c->pc = c->next_pc;
    c->next_pc = c->pc + 4;
    c->in_delay = 0;
    c->cycles++;

    uint64_t rs = r[RS(insn)], rt = r[RT(insn)], v;
    if (rs != SX(rs) || rt != SX(rt)) n64_noncanon_src++;
    uint32_t rs32 = (uint32_t)rs, rt32 = (uint32_t)rt, a;
    int rti = RT(insn), rdi = RD(insn);
    uint32_t op = insn >> 26;

#define WR(i, x) do { uint64_t x_ = (x); if (i) r[i] = x_; } while (0)
#define BRANCH(cond, target) do { c->in_delay = 1; if (cond) c->next_pc = (uint32_t)(target); } while (0)
#define BRANCH_L(cond, target) do { if (cond) { c->in_delay = 1; c->next_pc = (uint32_t)(target); } \
                                    else { c->pc = c->next_pc; c->next_pc = c->pc + 4; } } while (0)
#define OP64(k) (n64_op64_count[k]++, n64_op64_site(pc, insn))
#define TRAP(cond) do { if (cond) exception(n, pc, dly, EXC_TR, 0); } while (0)
    uint32_t btarget = pc + 4 + (uint32_t)(IMM(insn) << 2);
    a = rs32 + (uint32_t)IMM(insn);                    /* load/store address */

    switch (op) {
    case 0x00: {                                       /* SPECIAL */
        uint32_t f = insn & 63, sa = SA(insn);
        switch (f) {
        case 0x00: WR(rdi, SX(rt32 << sa)); break;                                   /* sll */
        case 0x02: WR(rdi, SX(rt32 >> sa)); break;                                   /* srl */
        case 0x03: WR(rdi, SX((uint32_t)((int32_t)rt32 >> sa))); break;              /* sra */
        case 0x04: WR(rdi, SX(rt32 << (rs32 & 31))); break;                          /* sllv */
        case 0x06: WR(rdi, SX(rt32 >> (rs32 & 31))); break;                          /* srlv */
        case 0x07: WR(rdi, SX((uint32_t)((int32_t)rt32 >> (rs32 & 31)))); break;     /* srav */
        case 0x08: BRANCH(1, rs32); break;                                           /* jr */
        case 0x09: WR(rdi, SX(pc + 8)); BRANCH(1, rs32); break;                      /* jalr */
        case 0x0C: exception(n, pc, dly, EXC_SYS, 0); break;
        case 0x0D: exception(n, pc, dly, EXC_BP, 0); break;
        case 0x0F: break;                                                            /* sync */
        case 0x10: WR(rdi, c->hi); break;
        case 0x11: c->hi = rs; break;
        case 0x12: WR(rdi, c->lo); break;
        case 0x13: c->lo = rs; break;
        case 0x14: OP64(N64_OP64_SHIFT); WR(rdi, rt << (rs32 & 63)); break;          /* dsllv */
        case 0x16: OP64(N64_OP64_SHIFT); WR(rdi, rt >> (rs32 & 63)); break;          /* dsrlv */
        case 0x17: OP64(N64_OP64_SHIFT); WR(rdi, (uint64_t)((int64_t)rt >> (rs32 & 63))); break;   /* dsrav */
        case 0x18: { int64_t p = (int64_t)(int32_t)rs32 * (int32_t)rt32;              /* mult */
                     c->lo = SX(p); c->hi = SX(p >> 32); break; }
        case 0x19: { uint64_t p = (uint64_t)rs32 * rt32;                              /* multu */
                     c->lo = SX(p); c->hi = SX(p >> 32); break; }
        case 0x1A: {                                                                 /* div */
            int32_t x = (int32_t)rs32, y = (int32_t)rt32;
            if (y == 0) { c->lo = x < 0 ? 1 : (uint64_t)-1; c->hi = SX(x); }
            else if (y == -1 && x == INT32_MIN) { c->lo = SX(x); c->hi = 0; }
            else { c->lo = SX(x / y); c->hi = SX(x % y); }
            break;
        }
        case 0x1B:                                                                   /* divu */
            if (rt32 == 0) { c->lo = (uint64_t)-1; c->hi = SX(rs32); }
            else { c->lo = SX(rs32 / rt32); c->hi = SX(rs32 % rt32); }
            break;
        case 0x1C: { OP64(N64_OP64_MULDIV); __int128 p = (__int128)(int64_t)rs * (int64_t)rt;   /* dmult */
                     c->lo = (uint64_t)p; c->hi = (uint64_t)(p >> 64); break; }
        case 0x1D: { OP64(N64_OP64_MULDIV); unsigned __int128 p = (unsigned __int128)rs * rt;   /* dmultu */
                     c->lo = (uint64_t)p; c->hi = (uint64_t)(p >> 64); break; }
        case 0x1E: {                                                                 /* ddiv */
            OP64(N64_OP64_MULDIV);
            int64_t x = (int64_t)rs, y = (int64_t)rt;
            if (y == 0) { c->lo = x < 0 ? 1 : (uint64_t)-1; c->hi = (uint64_t)x; }
            else if (y == -1 && x == INT64_MIN) { c->lo = (uint64_t)x; c->hi = 0; }
            else { c->lo = (uint64_t)(x / y); c->hi = (uint64_t)(x % y); }
            break;
        }
        case 0x1F:                                                                   /* ddivu */
            OP64(N64_OP64_MULDIV);
            if (rt == 0) { c->lo = (uint64_t)-1; c->hi = rs; }
            else { c->lo = rs / rt; c->hi = rs % rt; }
            break;
        case 0x20: case 0x21: WR(rdi, SX(rs32 + rt32)); break;                        /* add, addu */
        case 0x22: case 0x23: WR(rdi, SX(rs32 - rt32)); break;                        /* sub, subu */
        case 0x24: WR(rdi, rs & rt); break;
        case 0x25: WR(rdi, rs | rt); break;
        case 0x26: WR(rdi, rs ^ rt); break;
        case 0x27: WR(rdi, ~(rs | rt)); break;
        case 0x2A: WR(rdi, (int64_t)rs < (int64_t)rt); break;
        case 0x2B: WR(rdi, rs < rt); break;
        case 0x2C: case 0x2D: OP64(N64_OP64_ADD); WR(rdi, rs + rt); break;            /* dadd(u) */
        case 0x2E: case 0x2F: OP64(N64_OP64_ADD); WR(rdi, rs - rt); break;            /* dsub(u) */
        case 0x30: TRAP((int64_t)rs >= (int64_t)rt); break;
        case 0x31: TRAP(rs >= rt); break;
        case 0x32: TRAP((int64_t)rs < (int64_t)rt); break;
        case 0x33: TRAP(rs < rt); break;
        case 0x34: TRAP(rs == rt); break;
        case 0x36: TRAP(rs != rt); break;
        case 0x38: OP64(N64_OP64_SHIFT); WR(rdi, rt << sa); break;                    /* dsll */
        case 0x3A: OP64(N64_OP64_SHIFT); WR(rdi, rt >> sa); break;                    /* dsrl */
        case 0x3B: OP64(N64_OP64_SHIFT); WR(rdi, (uint64_t)((int64_t)rt >> sa)); break;   /* dsra */
        case 0x3C: OP64(N64_OP64_SHIFT); WR(rdi, rt << (sa + 32)); break;             /* dsll32 */
        case 0x3E: OP64(N64_OP64_SHIFT); WR(rdi, rt >> (sa + 32)); break;             /* dsrl32 */
        case 0x3F: OP64(N64_OP64_SHIFT); WR(rdi, (uint64_t)((int64_t)rt >> (sa + 32))); break;   /* dsra32 */
        default: N64_FATAL(n, "unimplemented SPECIAL %02x at %08x (%08x)", f, pc, insn);
        }
        break;
    }
    case 0x01: {                                       /* REGIMM */
        int64_t s = (int64_t)rs;
        switch (rti) {
        case 0x00: BRANCH(s < 0, btarget); break;
        case 0x01: BRANCH(s >= 0, btarget); break;
        case 0x02: BRANCH_L(s < 0, btarget); break;
        case 0x03: BRANCH_L(s >= 0, btarget); break;
        case 0x08: TRAP(s >= (int64_t)IMM(insn)); break;
        case 0x09: TRAP(rs >= IMM(insn)); break;
        case 0x0A: TRAP(s < (int64_t)IMM(insn)); break;
        case 0x0B: TRAP(rs < IMM(insn)); break;
        case 0x0C: TRAP(rs == IMM(insn)); break;
        case 0x0E: TRAP(rs != IMM(insn)); break;
        case 0x10: r[31] = SX(pc + 8); BRANCH(s < 0, btarget); break;
        case 0x11: r[31] = SX(pc + 8); BRANCH(s >= 0, btarget); break;
        case 0x12: r[31] = SX(pc + 8); BRANCH_L(s < 0, btarget); break;
        case 0x13: r[31] = SX(pc + 8); BRANCH_L(s >= 0, btarget); break;
        default: N64_FATAL(n, "unimplemented REGIMM %02x at %08x", rti, pc);
        }
        break;
    }
    case 0x02: BRANCH(1, (pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2)); break;
    case 0x03: r[31] = SX(pc + 8); BRANCH(1, (pc & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2)); break;
    case 0x04: BRANCH(rs == rt, btarget); break;
    case 0x05: BRANCH(rs != rt, btarget); break;
    case 0x06: BRANCH((int64_t)rs <= 0, btarget); break;
    case 0x07: BRANCH((int64_t)rs > 0, btarget); break;
    case 0x08: case 0x09: WR(rti, SX(rs32 + (uint32_t)IMM(insn))); break;            /* addi(u) */
    case 0x0A: WR(rti, (int64_t)rs < (int64_t)IMM(insn)); break;
    case 0x0B: WR(rti, rs < IMM(insn)); break;
    case 0x0C: WR(rti, rs & IMMU(insn)); break;
    case 0x0D: WR(rti, rs | IMMU(insn)); break;
    case 0x0E: WR(rti, rs ^ IMMU(insn)); break;
    case 0x0F: WR(rti, SX((uint32_t)IMMU(insn) << 16)); break;                      /* lui */
    case 0x10: {                                       /* COP0 */
        uint32_t fmt = RS(insn);
        if (fmt == 0x01 || fmt == 0x05) n64_op64_site(pc, insn);                    /* dmfc0, dmtc0 */
        if (fmt == 0x00 || fmt == 0x01) {                                           /* mfc0, dmfc0 */
            uint32_t x = c->cop0[rdi];
            WR(rti, SX(x));
        } else if (fmt == 0x04 || fmt == 0x05) {                                    /* mtc0, dmtc0 */
            switch (rdi) {
            case C0_CAUSE: c->cop0[C0_CAUSE] = (c->cop0[C0_CAUSE] & ~0x300u) | (rt32 & 0x300u); break;
            case C0_COMPARE: c->cop0[C0_COMPARE] = rt32; c->cop0[C0_CAUSE] &= ~CAUSE_IP7; break;
            case C0_RANDOM: case C0_PRID: break;
            case C0_WIRED: c->cop0[C0_WIRED] = rt32 & 0x3Fu; c->cop0[C0_RANDOM] = 31; break;
            case C0_COUNT: c->cop0[C0_COUNT] = rt32; c->count_frac = 0; break;          /* (the half-rate phase starts again) */
            default: {
                /* the VR4300's writable bits (the rest keep their value: Index's P,
                 * Context's BadVPN2, Config's fixed fields; BadVAddr, XContext,
                 * TagHi and the reserved registers take no write) */
                static const uint32_t wm[32] = {
                    [C0_INDEX] = 0x3F, [C0_ENTRYLO0] = 0x3FFFFFFF, [C0_ENTRYLO1] = 0x3FFFFFFF,
                    [C0_CONTEXT] = 0xFF800000, [C0_PAGEMASK] = 0x01FFE000, [C0_COUNT] = 0xFFFFFFFF,
                    [C0_ENTRYHI] = 0xFFFFE0FF, [C0_STATUS] = 0xFFFFFFFF, [C0_EPC] = 0xFFFFFFFF,
                    [C0_CONFIG] = 0x0F00800F, [C0_LLADDR] = 0xFFFFFFFF, [C0_WATCHLO] = 0xFFFFFFFB,
                    [C0_WATCHHI] = 0x0000000F, [C0_TAGLO] = 0x0FFFFFC0, [C0_ERROREPC] = 0xFFFFFFFF };
                c->cop0[rdi] = (c->cop0[rdi] & ~wm[rdi]) | (rt32 & wm[rdi]);
            }
            }
        } else if (fmt == 0x10) {                                                   /* CO */
            switch (insn & 63) {
            case 0x01: {                                                            /* tlbr */
                uint32_t i = c->cop0[C0_INDEX] & 31u;
                if (i < N64_TLB_ENTRIES) {
                    const n64_tlb_t *e = &c->tlb[i];
                    uint32_t g = e->lo0 & e->lo1 & 1u;
                    c->cop0[C0_PAGEMASK] = e->mask;
                    c->cop0[C0_ENTRYHI] = e->hi;
                    c->cop0[C0_ENTRYLO0] = (e->lo0 & ~1u) | g;
                    c->cop0[C0_ENTRYLO1] = (e->lo1 & ~1u) | g;
                }
                break;
            }
            case 0x02: case 0x06: {                                                 /* tlbwi, tlbwr */
                uint32_t i = (insn & 63) == 0x02 ? c->cop0[C0_INDEX] & 31u
                           : c->cop0[C0_RANDOM];
                if (i >= N64_TLB_ENTRIES) { n64_tlb_dropped++; break; }
                n64_tlb_t *e = &c->tlb[i];
                e->mask = c->cop0[C0_PAGEMASK] & 0x01FFE000u;
                e->hi = c->cop0[C0_ENTRYHI] & ~(e->mask | 0x1F00u);
                e->lo0 = c->cop0[C0_ENTRYLO0];
                e->lo1 = c->cop0[C0_ENTRYLO1];
                break;
            }
            case 0x08: {                                                            /* tlbp */
                uint32_t hi = c->cop0[C0_ENTRYHI];
                c->cop0[C0_INDEX] = 0x80000000u;
                for (int i = 0; i < N64_TLB_ENTRIES; i++) {
                    const n64_tlb_t *e = &c->tlb[i];
                    uint32_t vpn = ~(e->mask | 0x1FFFu);
                    if ((hi & vpn) == (e->hi & vpn) && ((e->lo0 & e->lo1 & 1u) || (hi & 0xFFu) == (e->hi & 0xFFu))) {
                        c->cop0[C0_INDEX] = (uint32_t)i;
                        break;
                    }
                }
                break;
            }
            case 0x18:                                                              /* eret */
                if (c->cop0[C0_STATUS] & SR_ERL) { c->pc = c->cop0[C0_ERROREPC]; c->cop0[C0_STATUS] &= ~SR_ERL; }
                else { c->pc = c->cop0[C0_EPC]; c->cop0[C0_STATUS] &= ~SR_EXL; }
                c->next_pc = c->pc + 4;
                c->ll_bit = 0;
                break;
            default: N64_FATAL(n, "unimplemented COP0 %02x at %08x", insn & 63, pc);
            }
        } else {
            N64_FATAL(n, "unimplemented COP0 fmt %02x at %08x", fmt, pc);
        }
        break;
    }
    case 0x11: {                                       /* COP1 */
        /* unusable while SR.CU1 is clear (the VR4300's CpU, CE 1): libultra
         * starts every thread without it and, on this trap, gives the thread
         * CU1 and marks it as using the FPU -- only such threads get their
         * FPU registers saved on a switch. Without the trap no thread ever
         * was, and the sound thread's floats overwrote the game's
         * mid-calculation (2026-09-29) */
        if (!(c->cop0[C0_STATUS] & 0x20000000u)) { exception(n, pc, dly, EXC_CPU, 1); break; }
        if (RS(insn) == 0x08) {                                                     /* bc1 */
            int t = (c->fcr31 >> 23) & 1;
            switch (RT(insn) & 3) {
            case 0: BRANCH(!t, btarget); break;
            case 1: BRANCH(t, btarget); break;
            case 2: BRANCH_L(!t, btarget); break;
            default: BRANCH_L(t, btarget); break;
            }
            break;
        }
        uint64_t g;
        int w = n64_cop1_exec(c, insn, rt, &g);
        if (w < 0) N64_FATAL(n, "unimplemented COP1 %08x at %08x", insn, pc);
        else if (w) WR(rti, g);
        break;
    }
    case 0x12: case 0x13: exception(n, pc, dly, EXC_CPU, (int)(op & 3)); break;   /* COP2/3 */
    case 0x14: BRANCH_L(rs == rt, btarget); break;
    case 0x15: BRANCH_L(rs != rt, btarget); break;
    case 0x16: BRANCH_L((int64_t)rs <= 0, btarget); break;
    case 0x17: BRANCH_L((int64_t)rs > 0, btarget); break;
    case 0x18: case 0x19: OP64(N64_OP64_ADD); WR(rti, rs + IMM(insn)); break;      /* daddi(u) */
    case 0x1A: case 0x1B: {                                                        /* ldl, ldr */
        OP64(N64_OP64_MEM);
        uint64_t w;
        if (partial(n, pc, dly, a, 8, 0, &w)) break;
        uint32_t k = a & 7;
        if (op == 0x1A) WR(rti, (w << (8 * k)) | (k ? rt & ((1ull << (8 * k)) - 1) : 0));
        else WR(rti, (w >> (8 * (7 - k))) | (k == 7 ? 0 : rt & ~(~0ull >> (8 * (7 - k)))));
        break;
    }
    case 0x20: if (!load(n, pc, dly, a, 1, &v)) WR(rti, (uint64_t)(int64_t)(int8_t)v); break;    /* lb */
    case 0x21: if (!load(n, pc, dly, a, 2, &v)) WR(rti, (uint64_t)(int64_t)(int16_t)v); break;   /* lh */
    case 0x22: case 0x26: {                                                        /* lwl, lwr */
        uint64_t w64;
        if (partial(n, pc, dly, a, 4, 0, &w64)) break;
        uint32_t w = (uint32_t)w64, k = a & 3, res;
        if (op == 0x22) res = (w << (8 * k)) | (k ? rt32 & ((1u << (8 * k)) - 1) : 0);
        else res = (w >> (8 * (3 - k))) | (k == 3 ? 0 : rt32 & ~(0xFFFFFFFFu >> (8 * (3 - k))));
        WR(rti, SX(res));
        break;
    }
    case 0x23: if (!load(n, pc, dly, a, 4, &v)) WR(rti, SX(v)); break;             /* lw */
    case 0x24: if (!load(n, pc, dly, a, 1, &v)) WR(rti, v & 0xFF); break;          /* lbu */
    case 0x25: if (!load(n, pc, dly, a, 2, &v)) WR(rti, v & 0xFFFF); break;        /* lhu */
    case 0x27: if (!load(n, pc, dly, a, 4, &v)) { n64_op64_site(pc, insn); WR(rti, v & 0xFFFFFFFFu); } break;   /* lwu */
    case 0x28: store(n, pc, dly, a, 1, rt & 0xFF); break;                          /* sb */
    case 0x29: store(n, pc, dly, a, 2, rt & 0xFFFF); break;                        /* sh */
    case 0x2A: case 0x2E: {                                                        /* swl, swr */
        uint64_t w64;
        if (partial(n, pc, dly, a, 4, 1, &w64) || partial(n, pc, dly, a, 4, 0, &w64)) break;
        uint32_t w = (uint32_t)w64, k = a & 3;
        if (op == 0x2A) w = (w & ~(0xFFFFFFFFu >> (8 * k))) | (rt32 >> (8 * k));
        else w = (w & (k == 3 ? 0 : 0xFFFFFFFFu >> (8 * (k + 1)))) | (rt32 << (8 * (3 - k)));
        store(n, pc, dly, a & ~3u, 4, w);
        break;
    }
    case 0x2B: store(n, pc, dly, a, 4, rt32); break;                               /* sw */
    case 0x2C: case 0x2D: {                                                        /* sdl, sdr */
        OP64(N64_OP64_MEM);
        uint64_t w;
        if (partial(n, pc, dly, a, 8, 1, &w) || partial(n, pc, dly, a, 8, 0, &w)) break;
        uint32_t k = a & 7;
        if (op == 0x2C) w = (w & (k ? ~(~0ull >> (8 * k)) : 0)) | (rt >> (8 * k));
        else w = (w & (k == 7 ? 0 : ~0ull >> (8 * (k + 1)))) | (rt << (8 * (7 - k)));
        store(n, pc, dly, a & ~7u, 8, w);
        break;
    }
    case 0x2F: break;                                                              /* cache */
    case 0x30: if (!load(n, pc, dly, a, 4, &v)) { WR(rti, SX(v)); c->ll_bit = 1; c->cop0[C0_LLADDR] = n64_xlat_pa(n, a) >> 4; } break;   /* ll */
    case 0x31: if (!(c->cop0[C0_STATUS] & 0x20000000u)) { exception(n, pc, dly, EXC_CPU, 1); break; }   /* (lwc1: CpU, see COP1) */
        if (!load(n, pc, dly, a, 4, &v)) c->f[rti] = (uint32_t)v; break;              /* lwc1 */
    case 0x34: if (!load(n, pc, dly, a, 8, &v)) { OP64(N64_OP64_MEM); WR(rti, v); c->ll_bit = 1; c->cop0[C0_LLADDR] = n64_xlat_pa(n, a) >> 4; } break;  /* lld */
    case 0x35: if (!(c->cop0[C0_STATUS] & 0x20000000u)) { exception(n, pc, dly, EXC_CPU, 1); break; }   /* (ldc1: CpU, see COP1) */
        if (!load(n, pc, dly, a, 8, &v)) fl_set(c, rti, (int64_t)v); break;          /* ldc1 */
    case 0x37: if (!load(n, pc, dly, a, 8, &v)) { OP64(N64_OP64_MEM); WR(rti, v); } break;  /* ld */
    case 0x38:                                                                     /* sc */
        if (c->ll_bit) { if (!store(n, pc, dly, a, 4, rt32)) { WR(rti, 1); c->ll_bit = 0; } }      /* (the link ends with the sc) */
        else if (!sc_fault(n, pc, dly, a, 4)) WR(rti, 0);
        break;
    case 0x39: if (!(c->cop0[C0_STATUS] & 0x20000000u)) { exception(n, pc, dly, EXC_CPU, 1); break; }   /* (swc1: CpU, see COP1) */
        store(n, pc, dly, a, 4, c->f[rti]); break;                          /* swc1 */
    case 0x3C:                                                                     /* scd */
        OP64(N64_OP64_MEM);
        if (c->ll_bit) { if (!store(n, pc, dly, a, 8, rt)) { WR(rti, 1); c->ll_bit = 0; } }
        else if (!sc_fault(n, pc, dly, a, 8)) WR(rti, 0);
        break;
    case 0x3D: if (!(c->cop0[C0_STATUS] & 0x20000000u)) { exception(n, pc, dly, EXC_CPU, 1); break; }   /* (sdc1: CpU, see COP1) */
        store(n, pc, dly, a, 8, (uint64_t)fl_get(c, rti)); break;           /* sdc1 */
    case 0x3F: OP64(N64_OP64_MEM); store(n, pc, dly, a, 8, rt); break;             /* sd */
    default: N64_FATAL(n, "unimplemented opcode %02x at %08x (%08x)", op, pc, insn);
    }
    r[0] = 0;
#undef WR
#undef BRANCH
#undef BRANCH_L
#undef OP64
#undef TRAP
}
