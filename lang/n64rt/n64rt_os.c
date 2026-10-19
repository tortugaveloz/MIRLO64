/* n64rt: libultra's threads, message queues, events and timers, on Mirlo.
 *
 * The same model as libultra's own: priority-preemptive threads (the
 * highest-priority runnable thread runs; equal priorities take turns only
 * on osYieldThread), message queues that block and wake, events delivered
 * as messages. A switch always goes through the exception path
 * (n64rt_exc.S): a thread that blocks makes a syscall, an interrupt that
 * wakes a higher-priority thread switches on its way out. Interrupts are the
 * game CPU's timer (COP0 Count/Compare): the VI's fields and osSetTimer's
 * timers both come from it (n64rt_tick).
 *
 * Time is the N64's: Count runs at 46.875 MHz there (osClockRate 62.5 MHz *
 * 3 / 4); n64rt_time() converts Mirlo's uptime counter to it. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include "n64rt.h"

/* A thread's saved state (n64rt_exc.S). The registers are the VR4300's 64
 * bits, upper word first; register 0's place holds EPC and Status, k0's and
 * k1's HI and LO. On the CPU without 64-bit integers (n64rt_cpu64 0) only the
 * lower words are saved and restored. */
typedef struct { u32 h, l; } n64rt_reg_t;
typedef struct {
#ifdef N64RT_OWN
    /* (the game's own $k0 and $k1 are live across the monitor: g[26], g[27]; HI and LO are
     * beside them) */
    union {
        n64rt_reg_t g[32];
        struct { u32 epc, status; };
    };
#else
    union {
        n64rt_reg_t g[32];
        struct { u32 epc, status; n64rt_reg_t r1_25[25]; n64rt_reg_t hi, lo; };
    };
#endif
    u32 fcsr, fpr[32];
    u32 wait_ra, wait_q;            /* (debug) where in the game it last blocked, and on what */
    u32 soft;                       /* hi and lo hold a 64-bit product or quotient of the runtime's
                                     * making: the CPU's own HI and LO are 32 bits (its COP0 7) */
#ifdef N64RT_OWN
    n64rt_reg_t hi, lo;             /* (0x190, 0x198) */
    u32 count_in;                   /* (0x1A0) Count on the way into the monitor: put back on the way out */
    /* the monitor's fast path (n64rt_exc.S): its scratch words -- ErrorEPC, the instruction, 8 for a load /
     * 0 for a store, a temporary -- whether it is on (the MI in hardware: n64rt_mon_fast_enable), how many
     * entries it has taken, and where the game goes on (n64rt_mon_ctx only: a thread's context area ends at 0x190) */
    u32 fast_epc, fast_insn, fast_dir, fast_tmp;   /* (0x1A4 .. 0x1B0) */
    u32 fast_on, fast_n, fast_next;                /* (0x1B4, 0x1B8, 0x1BC) */
#endif
} n64rt_ctx_t;
#ifdef N64RT_OWN
_Static_assert(__builtin_offsetof(n64rt_ctx_t, count_in) == 0x1A0 && __builtin_offsetof(n64rt_ctx_t, fast_epc) == 0x1A4 &&
               __builtin_offsetof(n64rt_ctx_t, fast_on) == 0x1B4 && __builtin_offsetof(n64rt_ctx_t, fast_n) == 0x1B8 &&
               __builtin_offsetof(n64rt_ctx_t, fast_next) == 0x1BC, "n64rt_exc.S's offsets");
#endif
/* a 32-bit value into a register: its sign extension, as a 32-bit instruction leaves it */
static inline void gset(n64rt_ctx_t *x, u32 r, u32 v) { if (r) { x->g[r].l = v; x->g[r].h = (u32)((s32)v >> 31); } }
/* a register's lower word (register 0's place is EPC and Status: it reads as 0) */
static inline u32 g32(const n64rt_ctx_t *x, u32 r) { return r ? x->g[r].l : 0u; }
u32 n64rt_cpu64;                    /* the CPU has the VR4300's 64-bit integers (probed at the start) */
u32 n64rt_cpu_alu, n64rt_cpu_sh, n64rt_cpu_md;   /* ... and runs dadd, dsub and the 64-bit logic; the d-shifts; dmult(u) with a
                                     * HI and LO of 64 bits (mips_core's W64_ALU, W64_SH, W64_MD: COP0 7's bits 31-29, so one build
                                     * runs on every bitstream). ddiv(u) is the runtime's on all of them (W64_DIV is
                                     * not in the SoC) */
static volatile u32 s_probe;
#ifndef N64RT_OWN
_Static_assert(sizeof(n64rt_ctx_t) <= sizeof(((OSThread *)0)->context), "context area");
#endif
#define CTX(t) ((n64rt_ctx_t *)(t)->context)

OSThread *n64rt_cur;
static OSThread s_boot;                 /* what runs before the game's first thread */
static OSThread *s_runq;                /* runnable, by priority (highest first), FIFO within one */
static OSThread *s_active;              /* every thread (tlnext) */

void n64rt_switch(void);
void n64rt_resume(void) __attribute__((noreturn));

/* ---- log ----------------------------------------------------------------- */
/* printf on a stack of its own (n64rt_call_on_stack): a log line can come
 * from code running on a game thread's small stack */
void n64rt_call_on_stack(void (*fn)(void *), void *arg, void *stack_top);
static u8 s_log_stack[8192] __attribute__((aligned(16)));
struct log_args { const char *fmt; va_list *ap; };
static void log_body(void *p) { struct log_args *a = p; vprintf(a->fmt, *a->ap); }
void n64rt_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    struct log_args a = { fmt, &ap };
    u32 s; __asm__ volatile("mfc0 %0, $12" : "=r"(s));
    __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s & ~1u));
    n64rt_call_on_stack(log_body, &a, s_log_stack + sizeof s_log_stack);
    __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(s));
    va_end(ap);
}
void n64rt_fatal(const char *fmt, ...)
{
    __asm__ volatile("mtc0 %0, $12" :: "r"(0u));
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("n64rt: halted (thread %ld)\n", n64rt_cur ? (long)n64rt_cur->id : -1L);
    for (;;) ;
}

/* ---- interrupts ------------------------------------------------------------ */
/* The SP interrupt (the RSP's BREAK with its interrupt on: a task ended or
 * yielded) is the game CPU's IP2 on a SoC that has the RSP; one without it
 * leaves the line low and the timer's poll (arm) finds the task's end. */
#ifdef N64RT_RSP
#define N64RT_IM_SP (1u << 10)
void n64rt_sp_irq(void);
#else
#define N64RT_IM_SP 0u
#endif

static inline u32 status_get(void) { u32 v; __asm__ volatile("mfc0 %0, $12" : "=r"(v)); return v; }
static inline void status_set(u32 v) { __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop" :: "r"(v)); }
static inline u32 ints_off(void) { u32 s = status_get(); status_set(s & ~1u); return s & 1u; }
static inline void ints_on(u32 m) { if (m) status_set(status_get() | 1u); }

/* libultra's __osDisableInt / __osRestoreInt: the IE bit */
u32 n64rt___osDisableInt(void) { return ints_off(); }
void n64rt___osRestoreInt(u32 m) { ints_on(m); }
/* osSetIntMask: the RCP's interrupt sources do not exist; the CPU's stay as
 * the runtime needs them. Returns "all enabled". */
u32 n64rt_osSetIntMask(u32 mask) { (void)mask; return 0x003FFF01u; }
u32 n64rt___osSetFpcCsr(u32 v) { u32 o; __asm__ volatile("cfc1 %0, $31\n\tctc1 %1, $31" : "=&r"(o) : "r"(v)); return o; }

/* ---- the run queue --------------------------------------------------------- */
static void runq_insert(OSThread *t)
{
    OSThread **p = &s_runq;
    while (*p && (*p)->priority >= t->priority) p = &(*p)->next;
    t->next = *p; *p = t;
    t->state = OS_STATE_RUNNABLE;
    t->queue = &s_runq;
}
static void q_insert(OSThread **head, OSThread *t)  /* a wait queue: by priority too */
{
    OSThread **q = head;
    while (*q && (*q)->priority >= t->priority) q = &(*q)->next;
    t->next = *q; *q = t;
    t->queue = head;
}
static OSThread *q_pop(OSThread **q)
{
    OSThread *t = *q;
    if (t) { *q = t->next; t->next = 0; t->queue = 0; }
    return t;
}
static void q_remove(OSThread *t)
{
    for (OSThread **p = t->queue; p && *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; break; }
    t->next = 0; t->queue = 0;
}

/* Pick who runs next (from the exception handler: interrupts off). */
static void schedule(void)
{
    OSThread *c = n64rt_cur;
    if (c->state == OS_STATE_RUNNING) {
        if (!s_runq || s_runq->priority <= c->priority) return;
        runq_insert(c);
    }
    OSThread *n = q_pop(&s_runq);
    if (!n) n64rt_fatal("n64rt: no thread to run (all blocked, no idle thread)\n");
    n->state = OS_STATE_RUNNING;
#ifdef N64RT_STATS
    { extern u32 n64rt_stat_switch; if (n != c) n64rt_stat_switch++; }
#endif
    n64rt_cur = n;
}

/* After waking someone: let a higher-priority thread run now. */
void n64rt_resched(void)
{
    if (s_runq && n64rt_cur->state == OS_STATE_RUNNING && s_runq->priority > n64rt_cur->priority) {
        u32 m = ints_off();
        n64rt_switch();
        ints_on(m);
    }
}

/* ---- threads ---------------------------------------------------------------- */
static void thread_exit(void);

void n64rt_osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri)
{
    /* a game may create a thread again in the same OSThread: off every list first */
    u32 m0 = ints_off();
    if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) q_remove(t);
    for (OSThread **p = &s_active; *p; p = &(*p)->tlnext)
        if (*p == t) { *p = t->tlnext; break; }
    ints_on(m0);
    memset(t, 0, sizeof *t);
    t->id = id;
    t->priority = pri;
    t->state = OS_STATE_STOPPED;
    n64rt_ctx_t *x = CTX(t);
    x->epc = (u32)(uintptr_t)entry;
    gset(x, 4, (u32)(uintptr_t)arg);                   /* a0 */
    gset(x, 29, (u32)(uintptr_t)sp - 16);              /* sp (o32: the callee's argument area) */
    gset(x, 31, (u32)(uintptr_t)thread_exit);          /* ra: returning ends the thread */
    /* CU1 CU0, FR (32 FPU registers), timer interrupt IM7, EXL (the
     * exception path's eret drops it), IE. IM3 to IM6 too, as libultra's
     * threads have them: the lines do not exist here, but a game looks at
     * its Status (Banjo-Kazooie stops for good without the pre-NMI's IM4). */
    x->status = 0x34000000u | (1u << 15) | 0x7800u | N64RT_IM_SP | 0x2u | 0x1u;
    u32 m = ints_off();
    t->tlnext = s_active; s_active = t;
    ints_on(m);
}

void n64rt_osStartThread(OSThread *t)
{
    u32 m = ints_off();
    if (t->state == OS_STATE_STOPPED || t->state == 0) {
        runq_insert(t);
        if (n64rt_cur->state == OS_STATE_RUNNING && t->priority > n64rt_cur->priority) n64rt_switch();
    }
    ints_on(m);
}

void n64rt_osStopThread(OSThread *t)
{
    u32 m = ints_off();
    if (!t) t = n64rt_cur;
    if (t == n64rt_cur) { t->state = OS_STATE_STOPPED; n64rt_switch(); }
    else if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) { q_remove(t); t->state = OS_STATE_STOPPED; }
    ints_on(m);
}

void n64rt_osDestroyThread(OSThread *t)
{
    u32 m = ints_off();
    if (!t) t = n64rt_cur;
    if (t->state == OS_STATE_RUNNABLE || t->state == OS_STATE_WAITING) q_remove(t);
    for (OSThread **p = &s_active; *p; p = &(*p)->tlnext)
        if (*p == t) { *p = t->tlnext; break; }
    t->state = 0;
    if (t == n64rt_cur) n64rt_switch();                 /* never comes back */
    ints_on(m);
}

static void thread_exit(void) { n64rt_osDestroyThread(0); for (;;) ; }

void n64rt_osSetThreadPri(OSThread *t, OSPri pri)
{
    u32 m = ints_off();
    if (!t) t = n64rt_cur;
    if (t->priority != pri) {
        t->priority = pri;
        if (t->state == OS_STATE_RUNNABLE) { q_remove(t); runq_insert(t); }
        if (s_runq && s_runq->priority > n64rt_cur->priority) n64rt_switch();
    }
    ints_on(m);
}

OSPri n64rt_osGetThreadPri(OSThread *t) { return (t ? t : n64rt_cur)->priority; }
/* (libultra's own reads its __osRunningThread, which is not kept) */
s32 n64rt_osGetThreadId(OSThread *t) { return (s32)(t ? t : n64rt_cur)->id; }

void n64rt_osYieldThread(void)
{
    u32 m = ints_off();
    if (s_runq && s_runq->priority >= n64rt_cur->priority) {
        n64rt_cur->state = OS_STATE_RUNNABLE;
        /* behind the others of its priority */
        OSThread **p = &s_runq, *t = n64rt_cur;
        while (*p && (*p)->priority >= t->priority) p = &(*p)->next;
        t->next = *p; *p = t; t->queue = &s_runq;
        n64rt_switch();
    }
    ints_on(m);
}

/* ---- message queues --------------------------------------------------------- */
void n64rt_osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msg, s32 count)
{
    mq->mtqueue = 0; mq->fullqueue = 0;
    mq->validCount = 0; mq->first = 0; mq->msgCount = count; mq->msg = msg;
}

static void block_on(OSThread **q)
{
    OSThread *c = n64rt_cur;
    CTX(c)->wait_q = (u32)(uintptr_t)q;
    c->state = OS_STATE_WAITING;
    q_insert(q, c);
    n64rt_switch();
}
static int wake_one(OSThread **q)            /* 1: the woken thread outranks the current one */
{
    OSThread *t = q_pop(q);
    if (!t) return 0;
    runq_insert(t);
    return t->priority > n64rt_cur->priority;
}

/* the delivery itself, interrupts off; 1 = a higher-priority thread woke */
static int send(OSMesgQueue *mq, OSMesg msg, int jam)
{
    if (mq->validCount >= mq->msgCount) return -1;
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    return wake_one(&mq->mtqueue);
}

/* (N64RT_DEBUG) a game port's trace of chosen queues: op 'S' send, 'R' receive, 'B' blocks */
void n64rt_trace_mq(int op, OSMesgQueue *mq, OSMesg msg, u32 ra) __attribute__((weak));
void n64rt_trace_mq(int op, OSMesgQueue *mq, OSMesg msg, u32 ra) { (void)op; (void)mq; (void)msg; (void)ra; }
#ifdef N64RT_DEBUG
#define TRACE_MQ(op, mq, msg) n64rt_trace_mq(op, mq, (OSMesg)(msg), (u32)(uintptr_t)__builtin_return_address(0))
#else
#define TRACE_MQ(op, mq, msg) ((void)0)
#endif

static s32 send_mesg(OSMesgQueue *mq, OSMesg msg, s32 flag, int jam)
{
    u32 m = ints_off();
    while (mq->validCount >= mq->msgCount) {
        if (flag == OS_MESG_NOBLOCK) { ints_on(m); return -1; }
        block_on(&mq->fullqueue);
    }
    if (send(mq, msg, jam) > 0 && n64rt_cur->state == OS_STATE_RUNNING) n64rt_switch();
    ints_on(m);
    return 0;
}
s32 n64rt_osSendMesg(OSMesgQueue *mq, OSMesg msg, s32 flag) { TRACE_MQ('S', mq, msg); return send_mesg(mq, msg, flag, 0); }
s32 n64rt_osJamMesg(OSMesgQueue *mq, OSMesg msg, s32 flag)  { return send_mesg(mq, msg, flag, 1); }

s32 n64rt_osRecvMesg(OSMesgQueue *mq, OSMesg *msg, s32 flag)
{
    CTX(n64rt_cur)->wait_ra = (u32)(uintptr_t)__builtin_return_address(0);
    u32 m = ints_off();
    while (mq->validCount == 0) {
        if (flag == OS_MESG_NOBLOCK) { ints_on(m); return -1; }
        TRACE_MQ('B', mq, 0);
        block_on(&mq->mtqueue);
    }
    TRACE_MQ('R', mq, mq->msg[mq->first]);
    if (msg) *msg = mq->msg[mq->first];
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    if (wake_one(&mq->fullqueue) && n64rt_cur->state == OS_STATE_RUNNING) n64rt_switch();
    ints_on(m);
    return 0;
}

/* A message from the runtime itself (an interrupt, a finished task):
 * never blocks, never switches -- the exception path, or the caller's
 * n64rt_resched(), lets a woken thread run. Interrupts off. */
void n64rt_send_isr(OSMesgQueue *mq, OSMesg msg) { (void)send(mq, msg, 0); }

/* ---- events -------------------------------------------------------------------- */
static struct { OSMesgQueue *mq; OSMesg msg; } s_event[OS_NUM_EVENTS];

void n64rt_osSetEventMesg(OSEvent e, OSMesgQueue *mq, OSMesg msg)
{
    u32 m = ints_off();
    if (e < OS_NUM_EVENTS) { s_event[e].mq = mq; s_event[e].msg = msg; }
    ints_on(m);
}

/* Deliver an event's message, if a queue is set and has room (the RCP's
 * interrupts never block either). Interrupts off. 0: the game set no queue
 * for it. */
int n64rt_post_event(int e)
{
    if (!s_event[e].mq) return 0;
    if (send(s_event[e].mq, s_event[e].msg, 0) < 0) {
#ifdef N64RT_DEBUG
        static u32 s_drops;
        if (++s_drops <= 8u) n64rt_log("n64rt: event %d dropped: its queue is full\n", e);
#endif
    }
    return 1;
}

/* ---- time and timers -------------------------------------------------------------- */
static inline u64 uptime(void) { timer0_uptime_latch_write(1); return timer0_uptime_cycles_read(); }
#define N64_COUNT_HZ 46875000u
/* uptime cycles -> N64 Count ticks, without a 64-bit divide on the hot path:
 * a 32.32 fixed-point ratio */
static u64 s_ratio_q32;
OSTime n64rt_time(void)
{
    u64 c = uptime();
    u64 hi = (c >> 32) * s_ratio_q32, lo = ((c & 0xFFFFFFFFu) * s_ratio_q32) >> 32;
    return hi + lo;
}
static u64 to_uptime(OSTime n64)        /* the other way, for the timer's deadline */
{
    return n64 * CONFIG_CLOCK_FREQUENCY / N64_COUNT_HZ;
}
static OSTime s_time_base;               /* osSetTime: osGetTime's offset (timers and Count go on) */
OSTime n64rt_osGetTime(void) { return n64rt_time() + s_time_base; }
void n64rt_osSetTime(OSTime t) { s_time_base = t - n64rt_time(); }
u32 n64rt_osGetCount(void) { return (u32)n64rt_time(); }

static OSTimer *s_timers;               /* by expiry time */
static void timer_insert(OSTimer *t)
{
    OSTimer **p = &s_timers;
    while (*p && (*p)->value <= t->value) p = &(*p)->next;
    t->next = *p; t->prev = 0; *p = t;
}
/* osSetTimer: `countdown` ticks from now, then every `interval` (0: once).
 * libultra keeps the expiry relative in t->value; here it is absolute. */
s32 n64rt_osSetTimer(OSTimer *t, OSTime countdown, OSTime interval, OSMesgQueue *mq, OSMesg msg)
{
    u32 m = ints_off();
#ifdef N64RT_DEBUG
    {   /* (the first few, and the first to each queue) */
        static u32 s_n, s_nq; static OSMesgQueue *s_q[12];
        u32 k = 0, fresh = 0;
        while (k < s_nq && s_q[k] != mq) k++;
        if (k == s_nq && s_nq < 12u) { s_q[s_nq++] = mq; fresh = 1; }
        if (s_n++ < 6u || fresh)
            n64rt_log("n64rt: osSetTimer(%08lx, countdown %08lx%08lx, interval %08lx%08lx, queue %08lx)\n", (unsigned long)(uintptr_t)t,
                      (unsigned long)(countdown >> 32), (unsigned long)countdown, (unsigned long)(interval >> 32), (unsigned long)interval,
                      (unsigned long)(uintptr_t)mq);
    }
#endif
    /* A queue nobody created: libultra's osContInit creates the EEPROM
     * routines' timer queue, a variable of its own that the runtime's
     * osContInit knows nothing of. The game's osEepromLongWrite then waits
     * on it for good (Banjo-Kazooie). It gets its one slot here. */
#ifndef N64RT_SI_GAME                    /* (the game's own osContInit runs there, and creates it) */
    if (mq && mq->msgCount == 0 && mq->msg == 0) {
        extern void n64rt_osCreateMesgQueue(OSMesgQueue *, OSMesg *, s32);
        n64rt_osCreateMesgQueue(mq, (OSMesg *)n64rt_rdram_alloc(sizeof(OSMesg)), 1);
    }
#endif
    t->interval = interval;
    t->value = n64rt_time() + (countdown ? countdown : interval);
    t->mq = mq; t->msg = msg;
    timer_insert(t);
    ints_on(m);
    return 0;
}

/* (libultra's own would follow its list's links, which are not these) */
s32 n64rt_osStopTimer(OSTimer *t)
{
    u32 m = ints_off();
    s32 r = -1;
    for (OSTimer **p = &s_timers; *p; p = &(*p)->next)
        if (*p == t) { *p = t->next; t->next = 0; r = 0; break; }
    ints_on(m);
    return r;
}

/* The timer interrupt: the VI's fields at 60 Hz, and the timers. A field of
 * the scan-out's is 1,042,710 SoC cycles (the console's 59.940 Hz: rtl/soc/n64_video.sv, 790 clocks of its clock, a fifth
 * of the SoC's, for each of 264 lines but nine a pixel shorter); it was 360 x 264 x 11 = 1,045,440 (59.783 Hz). */
#define VI_PERIOD_NTSC N64RT_FIELD_NTSC
#define VI_PERIOD_PAL  N64RT_FIELD_PAL                  /* 1,250,000: 50 Hz, 313 lines (the scan-out's, in a PAL mode) */
static u32 s_vb_nom = VI_PERIOD_NTSC;                   /* the field the scan-out is making now: n64rt_vi_pal() */
#define VI_PERIOD s_vb_nom
static u64 s_next_field;
#define VB_LEAD (CONFIG_CLOCK_FREQUENCY / 4000u)        /* 250 us: before the scan-out's frame boundary */
#define VB_SPIN (CONFIG_CLOCK_FREQUENCY / 1250u)        /* 800 us: how long the boundary is waited for */
#define FEED_GUARD (CONFIG_CLOCK_FREQUENCY / 800u)      /* 1.25 ms */
static u32 s_vb_period = VI_PERIOD_NTSC;
static int s_vb_meas;
static u64 s_vb_last;
#ifdef N64RT_DEBUG
static u32 s_vb_late_max, s_vb_late_pc[8], s_vb_late_n[8], s_irq_epc;
void n64rt_field_late_log(void)
{
    n64rt_log("n64rt:   a field at most %lu us late; taken late at", (unsigned long)(s_vb_late_max / (CONFIG_CLOCK_FREQUENCY / 1000000u)));
    for (int k = 0; k < 8 && s_vb_late_pc[k]; k++) n64rt_log(" %08lx x%lu", (unsigned long)s_vb_late_pc[k], (unsigned long)s_vb_late_n[k]);
    n64rt_log("\n");
    s_vb_late_max = 0;
    for (int k = 0; k < 8; k++) { s_vb_late_pc[k] = 0; s_vb_late_n[k] = 0; }
}
#endif
static u32 s_vb_n[3];                   /* fields: on the boundary, late (vblank on already), no boundary in reach */
int n64rt_tasks_busy(void);
void n64rt_tasks_poll(void);
#define TASK_POLL (CONFIG_CLOCK_FREQUENCY / 2000u)      /* 0.5 ms while an RSP task is under way */
/* The CPU's clock as 256ths of the SoC's, which uptime counts (a core built
 * with MIRLO_CPU_CLK has the CPU on a faster one): measured at the start */
static u32 s_count_q8 = 256u;
static void arm(void)
{
    u64 next = s_next_field;
    if (s_timers) { u64 t = to_uptime(s_timers->value); if (t < next) next = t; }
    u64 now = uptime();
    if (n64rt_tasks_busy() && now + TASK_POLL < next) next = now + TASK_POLL;
    u32 d = next > now + 64u ? (u32)(next - now) : 64u;
    u32 c; __asm__ volatile("mfc0 %0, $9" : "=r"(c));
    __asm__ volatile("mtc0 %0, $11" :: "r"(c + (u32)(((u64)d * s_count_q8) >> 9)));      /* Count runs at half the CPU's clock */
}
void n64rt_rearm(void) { arm(); }
/* (for the debug log) the scan-out's period as measured, 0 while not locked to it */
u32 n64rt_field_lock(void) { return s_vb_last ? s_vb_period : 0u; }
const u32 *n64rt_field_stats(void) { return s_vb_n; }
void n64rt_tick(void)
{
    n64rt_late_poll();
    /* (the tasks' events, then the field -- it is timed to the scan-out's frame
     * boundary -- and only then the audio FIFO's feed, up to a millisecond of it) */
    { extern void n64rt_tasks_events(void); n64rt_tasks_events(); }
    u64 now = uptime();
    if (now >= s_next_field) {
        {   /* (the scan-out's TV system can change at a field: the period measured is of the one before) */
            const u32 nom = n64rt_vi_pal() ? VI_PERIOD_PAL : VI_PERIOD_NTSC;
            if (nom != s_vb_nom) { s_vb_nom = nom; s_vb_period = nom; s_vb_meas = 0; s_vb_last = 0; }
        }
#ifdef N64RT_DEBUG
        {   /* how late the field is taken, and what was running when it was at last */
            u32 late = (u32)(now - s_next_field);
            if (late > s_vb_late_max) s_vb_late_max = late;
            if (late > VB_LEAD) {
                int k;
                for (k = 0; k < 8 && s_vb_late_pc[k] && s_vb_late_pc[k] != s_irq_epc; k++) ;
                if (k < 8) { s_vb_late_pc[k] = s_irq_epc; s_vb_late_n[k]++; }
            }
        }
#endif
        /* A field is the scan-out's, not a count of the CPU's clock. The
         * scan-out takes a new framebuffer at its frame boundary (vblank's
         * first line), and the game, told of the field, starts to draw in the
         * one it had on show: told at any other time, it draws in a picture
         * that is still being shown -- a frame's last things missing on the
         * screen whenever the two clocks' phases met (Pokemon Stadium's menu:
         * its text box cut short for a second every nine).
         * So: the field comes VB_LEAD before the boundary (the new framebuffer
         * goes to the scan-out in time), the boundary is waited for here, and
         * the next field is timed from it. Late (vblank is on already): a step
         * earlier. No boundary in reach (not locked yet): a step later. */
        int in_vb = (int)apf_video_video_vblank_status_read();
        n64rt_vi_field();
        if (!in_vb) {
            u64 t0 = now;
            while (!apf_video_video_vblank_status_read() && (now = uptime()) - t0 < VB_SPIN) ;
            if (apf_video_video_vblank_status_read()) {
                now = uptime();
                if (s_vb_last) {
                    /* the scan-out's own period, where two boundaries in a row were
                     * seen: within half a percent of a field (one seen late would time
                     * every field after it late), and smoothed */
                    u64 p = now - s_vb_last;
                    if (p > VI_PERIOD - VI_PERIOD / 200u && p < VI_PERIOD + VI_PERIOD / 200u)
                        s_vb_period = s_vb_meas ? (u32)((7u * (u64)s_vb_period + p + 4u) >> 3) : (u32)p;
                    s_vb_meas = 1;
                }
                s_vb_last = now;
                s_next_field = now + s_vb_period - VB_LEAD;
                s_vb_n[0]++;
            } else {
                s_vb_last = 0;
                s_next_field += s_vb_period + VB_SPIN / 2u;
                s_vb_n[2]++;
            }
        } else {
            s_vb_last = 0;
            s_next_field += s_vb_period - VB_LEAD;
            s_vb_n[1]++;
        }
        { extern void n64rt_vi_after_boundary(void); n64rt_vi_after_boundary(); }
        now = uptime();
        if (now >= s_next_field) s_next_field = now + s_vb_period;   /* fell behind: no burst */
    }
    /* (not when the next field is near: it would be taken late by as long as the feed is) */
    if (s_next_field > now + FEED_GUARD) { extern void n64rt_tasks_feed(void); n64rt_tasks_feed(); }
    OSTime t64 = n64rt_time();
    while (s_timers && s_timers->value <= t64) {
        OSTimer *t = s_timers;
        s_timers = t->next;
        if (t->mq) (void)send(t->mq, t->msg, 0);
        if (t->interval) { t->value += t->interval; timer_insert(t); }
        else t->next = 0;
    }
    arm();
}

#ifdef N64RT_STATS
/* ---- what the game asks of the runtime, counted (make STATS=1) ------------------------
 * Since the start, a few lines every 60 fields: each replaced function's
 * calls (n64rt_exc.S counts them on their way in), the exceptions by cause
 * with the time they took from the vector to eret (Count's ticks, two clocks
 * each; on the CPU with 64-bit integers), the switches from a thread to
 * another, and the registers the game reached itself -- each one's reads and
 * writes, and the time for them all. */
u32 n64rt_stats[512] __attribute__((aligned(4096)));
u32 n64rt_stat_in, n64rt_stat_out;
static u32 s_st_exc[32], s_st_ticks[32], s_st_hw_n, s_st_hw_ticks, s_st_last_in, s_st_last = 32u, s_st_last_hw;
u32 n64rt_stat_switch;
static struct { u32 pa, n[2]; } s_st_hw[40];
static void stat_hw(u32 addr, int write)
{
    u32 pa = addr & 0x1FFFFFFCu;
    if (pa >= 0x10000000u && pa < 0x1FC00000u) pa = 0x10000000u;            /* the cartridge: as one */
    else if (pa >= 0x04000000u && pa < 0x04002000u) pa &= ~0xFFFu;          /* the RSP's memories */
    else if (pa >= 0x1FC007C0u && pa < 0x1FC00800u) pa = 0x1FC007C0u;       /* PIF RAM */
    u32 k = 0;
    while (k < 39u && s_st_hw[k].pa && s_st_hw[k].pa != pa) k++;            /* (the last one: all the rest) */
    if (!s_st_hw[k].pa) s_st_hw[k].pa = k < 39u ? pa : 0xFFFFFFFFu;
    s_st_hw[k].n[write]++;
}
void n64rt_stats_log(u32 fields)
{
    extern const char *n64rt_func_name(u32 i);
    n64rt_log("n64rt: stats at field %lu: calls", (unsigned long)fields);
    for (u32 i = 0; i < 256u; i++) if (n64rt_stats[i]) n64rt_log(" %s %lu", n64rt_func_name(i), (unsigned long)n64rt_stats[i]);
    n64rt_log("\nn64rt: stats at field %lu: exceptions (cause:count/ticks)", (unsigned long)fields);
    for (u32 i = 0; i < 32u; i++)
        if (s_st_exc[i]) n64rt_log(" %lu:%lu/%lu", (unsigned long)i, (unsigned long)s_st_exc[i], (unsigned long)s_st_ticks[i]);
    n64rt_log("; switches %lu; registers (reads/writes)", (unsigned long)n64rt_stat_switch);
    for (u32 k = 0; k < 40u && s_st_hw[k].pa; k++)
        n64rt_log(" %08lx %lu/%lu", (unsigned long)s_st_hw[k].pa, (unsigned long)s_st_hw[k].n[0], (unsigned long)s_st_hw[k].n[1]);
    n64rt_log("; %lu of them took %lu ticks\n", (unsigned long)s_st_hw_n, (unsigned long)s_st_hw_ticks);
}
/* (the exception's handler, as it starts) the one before took from its way in to its way out */
static void stat_exc(u32 code)
{
    if (s_st_last < 32u) {
        u32 t = n64rt_stat_out - s_st_last_in;
        s_st_ticks[s_st_last] += t;
        if (s_st_last_hw) { s_st_hw_n++; s_st_hw_ticks += t; }
    }
    s_st_last = code; s_st_last_in = n64rt_stat_in; s_st_last_hw = 0;
    s_st_exc[code]++;
}
#endif

/* ---- the game's own accesses to N64 registers ---------------------------------------
 * A few game functions read or write an RCP register themselves, not through
 * libultra (Pokemon Stadium's audio reads AI_LEN). The trap brings them here:
 * a word load or store to a register n64rt_hw_read/write know is done on
 * its behalf; anything else stays fatal. */
static int branch_next(n64rt_ctx_t *x, u32 pc, u32 *next);
static u64 r64(const n64rt_ctx_t *x, u32 r);
static void w64(n64rt_ctx_t *x, u32 r, u64 v);
/* the instruction at pc: in KSEG0 (the game's) or the runtime's own memory,
 * or in a page the game mapped -- read through what it maps to, not through
 * the CPU's tables: this is the exception handler */
static int insn_at(u32 pc, u32 *insn)
{
    u32 pa;
    if ((pc >> 28) == 0x8u || (pc >> 28) == 0x4u) { *insn = *(volatile u32 *)(uintptr_t)pc; return 1; }
    if (!n64rt_tlb_phys(pc, &pa)) return 0;
    *insn = *(volatile u32 *)(uintptr_t)(0x80000000u | (pa & ~3u));
    return 1;
}
static int emulate_hw_at(n64rt_ctx_t *x, u32 insn, u32 op, u32 rt, u32 badv);
/* a load or store at an address where the console has nothing: a load reads 0, a store goes nowhere (1: done) */
static int emulate_none(n64rt_ctx_t *x, u32 op, u32 rt)
{
    switch (op) {
    case 0x20: case 0x24: case 0x21: case 0x25: case 0x23: case 0x22: case 0x26: case 0x27: if (rt) gset(x, rt, 0); return 1;   /* loads */
    case 0x37: w64(x, rt, 0); return 1;                                                                              /* ld */
    case 0x28: case 0x29: case 0x2B: case 0x2A: case 0x2E: case 0x3F: case 0x39: case 0x3D: return 1;                /* stores */
    case 0x31: x->fpr[rt] = 0; return 1;                                                                             /* lwc1 */
    case 0x35: x->fpr[rt | 1u] = 0; x->fpr[rt & ~1u] = 0; return 1;                                                  /* ldc1 */
    default: return 0;
    }
}
static int emulate_hw(n64rt_ctx_t *x, u32 cause)
{
    u32 bd = cause >> 31, pc = x->epc + (bd ? 4u : 0u), next = pc + 4u;
    u32 insn;
    if (!insn_at(pc, &insn)) return 0;
#ifdef N64RT_OWN
    /* (the monitor level leaves BadVAddr to the game: the address is worked out) */
    u32 badv = g32(x, (insn >> 21) & 31u) + (u32)(s32)(s16)insn;
#else
    u32 badv; __asm__ volatile("mfc0 %0, $8" : "=r"(badv));
#endif
    if ((badv >> 29) != 5u && (badv >> 29) != 4u) return 0;     /* (KSEG1 or KSEG0: a device's address) */
    u32 op = insn >> 26, rt = (insn >> 16) & 31u;
    if ((badv & 0x1FFFFFFFu) < 0x03F00000u) {
#ifdef N64RT_OWN
        /* Past the RDRAM the console has (8 MiB here), below the RCP: nothing. A game that sizes the memory itself goes on a megabyte
         * at a time until a word does not read back: it writes 0x12345678 at 0xA040_0000, 0xA050_0000 ... and its 0xA080_0000
         * must not stop the runtime. On the console the store goes nowhere and the load is 0. */
        if ((badv & 0x1FFFFFFFu) < n64rt_game->mem_size) return 0;      /* (the RDRAM itself is not the monitor's) */
        if (bd && !branch_next(x, x->epc, &next)) return 0;
        if (!emulate_none(x, op, rt)) return 0;
        x->epc = next;
        return 1;
#else
        return 0;                                               /* (RDRAM is not one) */
#endif
    }
    if (bd && !branch_next(x, x->epc, &next)) return 0;         /* (in a delay slot: through the branch) */
    int r = emulate_hw_at(x, insn, op, rt, badv);
    if (r) x->epc = next;
#ifdef N64RT_STATS
    if (r) { stat_hw(badv, op == 0x2B); s_st_last_hw = 1; }
#endif
    return r;
}
/* An access to the RCP's side of the bus is a 32-bit one, whatever the instruction. What the hardware does with the others
 * (n64-systemtest's spmem / pifram / cart_memory groups; the same for every RCP register):
 *   lb lbu lh lhu   the aligned word is read and the byte or half taken out of it (as it always was here)
 *   lwl lwr         the aligned word, merged into the register
 *   sb sh           the WHOLE word is written, the register shifted to the byte's or half's lane and zeros below it -- the other
 *                   lanes are not left alone: sb of 0x12345678 at offsets 0, 1, 2, 3 writes 0x78000000, 0x56780000, 0x34567800,
 *                   0x12345678; sh at 0 writes 0x56780000, at 2 0x12345678
 *   swl swr         the same way (the register shifted to where the instruction puts it, zeros elsewhere); not measured
 *   sd              only the upper 32 bits of the register, at the address (the lower ones are lost)
 *   ld              (the hardware does not survive it) two words, upper first
 *   lwc1 swc1       a word, as lw and sw
 * An unaligned lh, lhu, sh, lw, sw or an sd off its 8 bytes is an address error on the console, and still the game's here. */
static int emulate_hw_at(n64rt_ctx_t *x, u32 insn, u32 op, u32 rt, u32 badv)
{
    (void)insn;
    const u32 k = badv & 3u;                          /* big-endian: byte k is bits 31-8k.. */
    u32 v;
    switch (op) {
    case 0x23: case 0x20: case 0x24: case 0x21: case 0x25: {            /* lw lb lbu lh lhu */
        if (!n64rt_hw_read(badv & ~3u, &v)) return 0;
        if (op == 0x20) v = (u32)(s32)(s8)(v >> (24 - 8 * k));
        else if (op == 0x24) v = (v >> (24 - 8 * k)) & 0xFFu;
        else if (op == 0x21) v = (u32)(s32)(s16)(v >> (16 - 8 * (k & 2u)));
        else if (op == 0x25) v = (v >> (16 - 8 * (k & 2u))) & 0xFFFFu;
        if (rt) gset(x, rt, v);
        return 1;
    }
    case 0x22:                                                          /* lwl: the bytes from the address to the word's end, at the top */
        if (!n64rt_hw_read(badv & ~3u, &v)) return 0;
        if (rt) gset(x, rt, k ? (v << (8u * k)) | (x->g[rt].l & ((1u << (8u * k)) - 1u)) : v);
        return 1;
    case 0x26:                                                          /* lwr: from the word's start to the address, at the bottom */
        if (!n64rt_hw_read(badv & ~3u, &v)) return 0;
        if (rt) gset(x, rt, (x->g[rt].l & ~(0xFFFFFFFFu >> (8u * (3u - k)))) | (v >> (8u * (3u - k))));
        return 1;
    case 0x37: {                                                        /* ld */
        u32 lo;
        if ((badv & 7u) || !n64rt_hw_read(badv, &v) || !n64rt_hw_read(badv + 4u, &lo)) return 0;
        w64(x, rt, (u64)v << 32 | lo);
        return 1;
    }
    case 0x31: if (k || !n64rt_hw_read(badv, &v)) return 0; x->fpr[rt] = v; return 1;                      /* lwc1 */
    case 0x2B: return n64rt_hw_write(badv, g32(x, rt));                                                   /* sw */
    case 0x39: return !k && n64rt_hw_write(badv, x->fpr[rt]);                                             /* swc1 */
    case 0x28: return n64rt_hw_write(badv & ~3u, g32(x, rt) << (8u * (3u - k)));                          /* sb */
    case 0x29: return !(badv & 1u) && n64rt_hw_write(badv & ~3u, g32(x, rt) << (16u - 8u * (k & 2u)));   /* sh */
    case 0x2A: return n64rt_hw_write(badv & ~3u, g32(x, rt) >> (8u * k));                                 /* swl */
    case 0x2E: return n64rt_hw_write(badv & ~3u, g32(x, rt) << (8u * (3u - k)));                          /* swr */
    case 0x3F: return !(badv & 7u) && n64rt_hw_write(badv, (u32)(r64(x, rt) >> 32));                      /* sd */
    default: return 0;
    }
}

/* ---- the TLB --------------------------------------------------------------------
 * A game's osMapTLB is kept in a table here: its 32 entries, pairs of pages
 * of any size.
 *
 * On the CPU with translation tables (mips_core's TLB_RAM: found at the
 * start by its COP0 register 23) an access to, or a fetch from, a page with
 * no entry there takes the TLB's exception, and tlb_fill() writes the entry
 * of that 4 KiB from the game's -- into the data table or the fetch table,
 * as the CPU says the exception was -- and the instruction runs again. A page
 * costs one exception, whatever is done with it afterwards, and code runs
 * from a mapped page. The tables are emptied whenever the game maps or
 * unmaps (the entries come back as they are used).
 *
 * The CPU without them has no TLB: an access to a mapped page traps (an
 * address error: nothing is there) and is done on the game's behalf -- the
 * load or store is decoded, run on the RDRAM the page maps, and the game goes
 * on past it, through the branch when it was in a delay slot. That is
 * thousands of times an access's cost, so it is for what a game maps briefly:
 * Super Mario 64 maps its title screen's head (a segment linked at
 * 0x04000000) while it builds it, and unmaps it. Code cannot run from a
 * mapped page there. */
#define TLB_N 32
struct n64rt_tlbs_e { u32 va, span, pa[2]; s32 asid; };           /* (20 bytes: n64rt_exc.S's tlbwi reads `span`, at +4) */
struct n64rt_tlbs_e n64rt_tlbs[TLB_N];                           /* a pair of pages of `span` bytes; span 0: free */
#define s_tlb n64rt_tlbs
_Static_assert(sizeof(struct n64rt_tlbs_e) == 20 && __builtin_offsetof(struct n64rt_tlbs_e, span) == 4, "n64rt_exc.S's tlbwi");
static u32 s_tlb_hits, s_tlb_fills, s_tlb_delivered;
u32 n64rt_tlb_n;                        /* the entries that map something (n64rt_exc.S's tlbwi reads it) */
#define s_tlb_n n64rt_tlb_n
u32 n64rt_tlbram;                       /* the CPU has the translation tables */
/* The bits of COP0 23's write value (bit 4 is the page of the pair; the physical page is bits 28:12): valid, dirty (writable), global,
 * the fetch table, and TR_C = the page is CACHEABLE (the entry's C field, EntryLo bits 5:3, is not 2 = uncached). mips_core's fetch
 * table keeps the bit (the fetch page registers use the I-cache for a cacheable mapped page); the data tables have no field for it
 * yet and a mapped data access is uncached, a word at a time. */
enum { TR_V = 1, TR_D = 2, TR_G = 4, TR_FETCH = 8, TR_C = 32 };
/* an entry of the CPU's tables: COP0 23 stores it for EntryHi's page pair
 * and ASID -- the physical page, which page of the pair (va's bit 12), the
 * table, global / dirty / valid */
static inline void tr_write(u32 va, u32 pa, u32 flags)
{
    __asm__ volatile("mtc0 %0, $23" : : "r"((pa & 0x1FFFF000u) | ((va >> 8) & 16u) | flags));
}
/* both tables emptied (EntryHi names each page pair in turn: the game's ASID
 * is put back; interrupts off, a thread's TLB exception uses EntryHi too) */
static void tr_flush(void)
{
    if (!n64rt_tlbram) return;
    u32 m = ints_off(), hi;
    __asm__ volatile("mfc0 %0, $10" : "=r"(hi));
    for (u32 va = 0; va < 512u << 12; va += 0x1000u) {
        if (!(va & 0x1000u)) __asm__ volatile("mtc0 %0, $10" : : "r"(va));
        tr_write(va, 0, 0); tr_write(va, 0, TR_FETCH);
    }
    __asm__ volatile("mtc0 %0, $10" : : "r"(hi));
    ints_on(m);
}
/* the pages of [va, va + bytes) emptied in both tables (a big range: all of them) */
static void tr_inval(u32 va, u32 bytes)
{
    if (!n64rt_tlbram) return;
    if (bytes > 0x40000u) { tr_flush(); return; }
    u32 m = ints_off(), hi;
    __asm__ volatile("mfc0 %0, $10" : "=r"(hi));
    for (u32 a = va & ~0x1FFFu; a < va + bytes; a += 0x2000u) {
        __asm__ volatile("mtc0 %0, $10" : : "r"(a));
        tr_write(a, 0, 0); tr_write(a, 0, TR_FETCH);
        tr_write(a + 0x1000u, 0, 0); tr_write(a + 0x1000u, 0, TR_FETCH);
    }
    __asm__ volatile("mtc0 %0, $10" : : "r"(hi));
    ints_on(m);
}
/* the page of the TLB exception's address, from the game's entries: 1 when filled */
static int tlb_fill(void)
{
    u32 badv, sel, hi;                                  /* (EntryHi: the faulting page pair, the game's ASID) */
    __asm__ volatile("mfc0 %0, $8" : "=r"(badv));
    __asm__ volatile("mfc0 %0, $23" : "=r"(sel));
    __asm__ volatile("mfc0 %0, $10" : "=r"(hi));
    for (int i = 0; i < TLB_N; i++) {
        u32 span = s_tlb[i].span, off = badv - s_tlb[i].va;
        if (!span || off >= 2u * span) continue;
        if (s_tlb[i].asid >= 0 && (u32)s_tlb[i].asid != (hi & 0xFFu)) continue;
        u32 p = s_tlb[i].pa[off >= span];
        if (p == 0xFFFFFFFFu) return 0;
        p += off & (span - 1u) & ~0xFFFu;
        if (p >= 0x00800000u) return 0;                     /* (the RDRAM) */
        tr_write(badv, p, TR_V | TR_D | TR_C | (s_tlb[i].asid < 0 ? TR_G : 0) | (sel & TR_FETCH));      /* (osMapTLB maps cached pages) */
        s_tlb_fills++;
        return 1;
    }
    return 0;
}
void n64rt_osMapTLB(s32 index, u32 pm, u32 vaddr, u32 even, u32 odd, s32 asid)
{
    if ((u32)index >= TLB_N) return;
    u32 span = (pm + 0x2000u) >> 1;                       /* OS_PM_4K 0 .. OS_PM_16M 0x1FFE000 */
    if (!s_tlb[index].span) s_tlb_n++;
    tr_flush();
    s_tlb[index].asid = asid;
    s_tlb[index].va = vaddr & ~(2u * span - 1u);
    s_tlb[index].span = span;
    s_tlb[index].pa[0] = even == 0xFFFFFFFFu ? even : even & ~(span - 1u);
    s_tlb[index].pa[1] = odd == 0xFFFFFFFFu ? odd : odd & ~(span - 1u);
    if (s_tlb_n == 1u && !s_tlb_hits)
        n64rt_log("n64rt: osMapTLB(%ld): %08lx, two pages of %lu KiB at %08lx and %08lx (%s)\n", (long)index,
                  (unsigned long)s_tlb[index].va, (unsigned long)(span >> 10), (unsigned long)even, (unsigned long)odd,
                  n64rt_tlbram ? "the CPU's tables" : "done in software");
}
void n64rt_osUnmapTLB(s32 index)
{
    if ((u32)index < TLB_N && s_tlb[index].span) { s_tlb[index].span = 0; s_tlb_n--; tr_flush(); }
}
void n64rt_osUnmapTLBAll(void)
{
    if (s_tlb_n && (s_tlb_hits || s_tlb_fills)) {
        n64rt_log("n64rt: osUnmapTLBAll: %lu accesses to mapped pages were done in software, %lu pages entered in the CPU's tables\n",
                  (unsigned long)s_tlb_hits, (unsigned long)s_tlb_fills);
        s_tlb_hits = s_tlb_fills = 0;
    }
    if (s_tlb_n) tr_flush();
    for (int i = 0; i < TLB_N; i++) s_tlb[i].span = 0;
    s_tlb_n = 0;
}
/* ---- the game's own TLB instructions (OWN) ---------------------------------------
 * A core with the translation tables and the monitor (mips_core TLB_RAM + MON; COP0 register 23's bit 0) takes the game's
 * tlbwi, tlbwr, tlbr and tlbp as a Reserved Instruction for the monitor level (code 10, EPC the instruction): the game's
 * own osMapTLB, osUnmapTLB, osUnmapTLBAll, __osProbeTLB and osVirtualToPhysical then run as they are, wherever they are in
 * the game's memory (a segment it unpacks or loads later too), and nothing of the game has to be found or patched.
 * The 32 entries are kept here as the registers gave them (s_tlbr, which tlbr and tlbp work on) and as the table the
 * fill reads (s_tlb, from osMapTLB's days: a pair of pages of any size by the physical addresses).
 * Done as the VR4300 does, on its registers (the monitor level reads and writes the game's COP0 ones):
 *   tlbwi  entry Index & 31 := PageMask, EntryHi (VPN2 and ASID), EntryLo0, EntryLo1
 *   tlbwr  the same at Random (which runs from 31 down to Wired)
 *   tlbr   PageMask, EntryHi, EntryLo0, EntryLo1 := entry Index & 31 (G in both EntryLo: the two entries' AND)
 *   tlbp   Index := the entry EntryHi names (VPN2 under the entry's mask, ASID unless global), or P (bit 31) if none
 * The D bit (a store to a clean page is a Mod exception for the game's handler), V, G and the ASID are honoured (tlb_service); C is
 * passed on to the tables as TR_C (they do not keep it yet). A tlbr, tlbp or tlbwi that changes no translation is done in
 * n64rt_exc.S without the full path's register save (n64rt_mon_tlbq). */
u32 n64rt_tlbi;                         /* the core takes the game's TLB instructions (COP0 23 bit 0) */
#ifdef N64RT_OWN
struct n64rt_tlbr_e { u32 mask, hi, lo0, lo1; };                 /* (16 bytes: n64rt_exc.S's tlbr, tlbp and tlbwi work on them) */
struct n64rt_tlbr_e n64rt_tlbr[TLB_N];                           /* (hi ^ 0x80000000: a free entry names KSEG0, which no TLB access has) */
#define s_tlbr n64rt_tlbr
_Static_assert(sizeof(struct n64rt_tlbr_e) == 16, "n64rt_exc.S's tlbr, tlbp and tlbwi");
#define CP0_GET(reg, v) __asm__ volatile("mfc0 %0, $" #reg : "=r"(v))
#define CP0_SET(reg, v) __asm__ volatile("mtc0 %0, $" #reg : : "r"(v))
/* ---- the entries a tlbwr put out ---------------------------------------------------------------------------------
 * A game that fills its TLB from a page table of its own (a refill handler: tlbwr) has more pages in use than the TLB's 32
 * entries hold, and each tlbwr puts a random one out: on the console the page it mapped is a refill away, a dozen
 * instructions. Here a refill is two trips through the monitor and the game's handler between them (the fault delivered,
 * its tlbwr, the tables' fill): some 3,000 clocks, and a game can take 180 of them a field, with as many tlbwr --
 * a third of the CPU's time. So the entry a tlbwr replaces is KEPT (s_vic, by its page pair: 4 KiB pages only) and its pages
 * stay in the CPU's tables: an access to them goes on as before, and one that misses the tables is filled from here
 * (tlb_service), neither seen by the game. The game sees its 32 entries as the VR4300 has them (tlbr, tlbp, Index, Random).
 *
 * What the console does that this does not: there, the page of an entry that went takes a refill, from the game's page table
 * as it is THEN. The game does not choose which entry a tlbwr replaces, so it cannot count on that refill -- but a page table
 * can change under an entry that is kept here. So a kept entry goes (with its pages in the tables) as soon as the game
 *   - looks for the page with a tlbp (it finds nothing, as on the console, and there is nothing from then on),
 *   - writes an entry of the same page pair, or an entry that maps one of its frames (the frame is another page's now),
 *   - reads into one of its frames from the cartridge (n64rt_tlb_frames_loaded: a game that looks for a page's entry by reading
 *     all 32 with tlbr finds none for a page whose entry a tlbwr replaced, and loads another page of its code over the frame),
 *   - writes three entries that map nothing in a row (a flush of the TLB): then all of them go.
 * What is left is a game that reuses a frame by copying into it with the CPU while another thread runs code from the page
 * it was: no game known here does. N64RT_NO_TLBKEEP builds the runtime without any of it (every tlbwr as the console's). */
#define VIC_N 128
static struct n64rt_tlbr_e s_vic[VIC_N];                        /* (as n64rt_tlbr: hi ^ 0x80000000; 0: free) */
u32 n64rt_vic_n;                                                /* how many (n64rt_exc.S: with any, a tlbp and a tlbwi are done here) */
u32 n64rt_tlb_unmaps;                                           /* the game's last TLB instructions were this many tlbwi that map nothing */
#define s_tlb_unmaps n64rt_tlb_unmaps                             /* (n64rt_exc.S's tlbwi clears it) */
static inline u32 vic_slot(u32 hi) { return (hi >> 13) & (VIC_N - 1u); }
/* the kept entry of EntryHi `hi`'s page pair (its ASID unless the entry is global), or -1 */
static int vic_find(u32 hi)
{
    if (!n64rt_vic_n) return -1;
    const u32 k = vic_slot(hi), ehi = s_vic[k].hi ^ 0x80000000u;
    if (!s_vic[k].hi || ((hi ^ ehi) & 0xFFFFE000u)) return -1;
    if (!(s_vic[k].lo0 & s_vic[k].lo1 & 1u) && ((hi ^ ehi) & 0xFFu)) return -1;
    return (int)k;
}
/* it goes, and what the tables have of its pages unless `keep` (the game's new entry of the page pair is the same) */
static void vic_drop(u32 k, int keep)
{
    if (!s_vic[k].hi) return;
    if (!keep) tr_inval((s_vic[k].hi ^ 0x80000000u) & 0xFFFFE000u, 0x2000u);
    s_vic[k].hi = 0; n64rt_vic_n--;
}
static void vic_flush(void)
{
    for (u32 k = 0; k < VIC_N && n64rt_vic_n; k++) vic_drop(k, 0);
}
/* the kept entries with a page in the RDRAM's [pa, pa + bytes) go */
static void vic_drop_frames(u32 pa, u32 bytes)
{
    for (u32 k = 0; k < VIC_N && n64rt_vic_n; k++) {
        if (!s_vic[k].hi) continue;
        const u32 f0 = (s_vic[k].lo0 >> 6) << 12, f1 = (s_vic[k].lo1 >> 6) << 12;
        if (((s_vic[k].lo0 & 2u) && f0 + 0x1000u > pa && f0 < pa + bytes) || ((s_vic[k].lo1 & 2u) && f1 + 0x1000u > pa && f1 < pa + bytes))
            vic_drop(k, 0);
    }
}
void n64rt_tlb_frames_loaded(u32 pa, u32 bytes) { if (n64rt_vic_n) vic_drop_frames(pa, bytes); }
static void tlb_write(u32 i, int random)
{
    u32 mask, hi, lo0, lo1;
    CP0_GET(5, mask); CP0_GET(10, hi); CP0_GET(2, lo0); CP0_GET(3, lo1);
    mask &= 0x01FFE000u; hi &= ~(mask | 0x1F00u); lo0 &= 0x3FFFFFFFu; lo1 &= 0x3FFFFFFFu;
    const struct n64rt_tlbr_e old = s_tlbr[i];
    s_tlbr[i].mask = mask; s_tlbr[i].hi = hi ^ 0x80000000u; s_tlbr[i].lo0 = lo0; s_tlbr[i].lo1 = lo1;
    const u32 span = (mask + 0x2000u) >> 1;                            /* a page of the pair (4 KiB with no mask) */
    const u32 p0 = lo0 & 2u ? ((lo0 >> 6) << 12) & ~(span - 1u) : 0xFFFFFFFFu;     /* (V clear: nothing is mapped) */
    const u32 p1 = lo1 & 2u ? ((lo1 >> 6) << 12) & ~(span - 1u) : 0xFFFFFFFFu;
    const int was = s_tlb[i].span != 0, on = p0 != 0xFFFFFFFFu || p1 != 0xFFFFFFFFu;
    if (n64rt_vic_n) {
        /* a kept entry of the page pair written: the page is this entry's now (its tables' pages stay if it is the same); and
         * the kept entries of the frames this one maps: those frames are this page's now */
        const int k = vic_find(hi);
        if (k >= 0) vic_drop((u32)k, mask == 0u && s_vic[k].lo0 == lo0 && s_vic[k].lo1 == lo1);
        if (p0 != 0xFFFFFFFFu && p1 == p0 + span) vic_drop_frames(p0, 2u * span);
        else {
            if (p0 != 0xFFFFFFFFu) vic_drop_frames(p0, span);
            if (p1 != 0xFFFFFFFFu) vic_drop_frames(p1, span);
        }
    }
    s_tlb_unmaps = (!random && !on) ? s_tlb_unmaps + 1u : 0u;
    if (s_tlb_unmaps >= 3u) vic_flush();                                  /* (entries that map nothing, one after another: a flush) */
#ifdef N64RT_NO_TLBKEEP
    random = 0;
#endif
    if (was) {
        const u32 ohi = old.hi ^ 0x80000000u;
        if (random && old.mask == 0u && ((ohi ^ hi) & 0xFFFFE000u)) {      /* put out by a tlbwr: kept, its pages left in the tables */
            const u32 k = vic_slot(ohi);
            vic_drop(k, 0);                                               /* (the slot's other page pair, if any) */
            s_vic[k] = old; n64rt_vic_n++;
        } else
            tr_inval(s_tlb[i].va, 2u * s_tlb[i].span);                   /* (what the CPU's tables took from this entry goes) */
    }
    s_tlb_n += (u32)on - (u32)was;
    s_tlb[i].span = on ? span : 0;
    s_tlb[i].asid = (lo0 & lo1 & 1u) ? -1 : (s32)(hi & 0xFFu);
    s_tlb[i].va = hi & 0xFFFFE000u & ~(2u * span - 1u);
    s_tlb[i].pa[0] = p0; s_tlb[i].pa[1] = p1;
    if (on && s_tlb_n == 1u && !s_tlb_hits && !s_tlb_fills)
        n64rt_log("n64rt: the game maps %08lx, two pages of %lu KiB at %08lx and %08lx (its own TLB instruction; entry %lu)\n",
                  (unsigned long)s_tlb[i].va, (unsigned long)(span >> 10), (unsigned long)p0, (unsigned long)p1, (unsigned long)i);
}
static void tlb_read(u32 i)
{
    const u32 g = s_tlbr[i].lo0 & s_tlbr[i].lo1 & 1u;
    CP0_SET(5, s_tlbr[i].mask);
    CP0_SET(10, s_tlbr[i].hi ^ 0x80000000u);
    CP0_SET(2, (s_tlbr[i].lo0 & ~1u) | g);
    CP0_SET(3, (s_tlbr[i].lo1 & ~1u) | g);
}
static void tlb_probe(void)
{
    u32 hi, found = 0x80000000u;
    CP0_GET(10, hi);
    for (u32 i = 0; i < TLB_N; i++) {
        const u32 ehi = s_tlbr[i].hi ^ 0x80000000u, vpn = ~(s_tlbr[i].mask | 0x1FFFu);
        if ((hi & vpn) == (ehi & vpn) && ((s_tlbr[i].lo0 & s_tlbr[i].lo1 & 1u) || (hi & 0xFFu) == (ehi & 0xFFu))) { found = i; break; }
    }
    if (found >> 31) { const int k = vic_find(hi); if (k >= 0) vic_drop((u32)k, 0); }     /* (the game is told "no entry": there is none from now) */
    CP0_SET(0, found);                  /* (the core lets the monitor level write Index's P bit) */
}
/* the game's TLB instruction at x->epc (or in the delay slot behind it): 1 when it was done */
static int tlb_insn(n64rt_ctx_t *x, int bd)
{
    u32 pc = x->epc + (bd ? 4u : 0u), next = pc + 4u, insn, v;
    if (!insn_at(pc, &insn)) return 0;                                 /* (KSEG0, or a page the game mapped) */
    if ((insn & 0xFFFFFFC0u) != 0x42000000u) return 0;
#ifdef N64RT_DEBUG
    {   /* (the first of each kind, and every 4096th) */
        static u32 s_n[64];
        u32 k = insn & 63u, e0, e1, hi, pm, ix;
        if (s_n[k]++ < 4u || (s_n[k] & 4095u) == 0u) {
            CP0_GET(0, ix); CP0_GET(2, e0); CP0_GET(3, e1); CP0_GET(5, pm); CP0_GET(10, hi);
            n64rt_log("n64rt: the game's %s at %08lx (#%lu): Index %08lx EntryHi %08lx EntryLo %08lx %08lx PageMask %08lx\n",
                      k == 1 ? "tlbr" : k == 2 ? "tlbwi" : k == 6 ? "tlbwr" : "tlbp", (unsigned long)pc, (unsigned long)s_n[k],
                      (unsigned long)ix, (unsigned long)hi, (unsigned long)e0, (unsigned long)e1, (unsigned long)pm);
        }
    }
#endif
    switch (insn & 63u) {
    case 0x01: s_tlb_unmaps = 0; CP0_GET(0, v); tlb_read(v & 31u); break;                 /* tlbr */
    case 0x02: CP0_GET(0, v); if ((v & 63u) < TLB_N) tlb_write(v & 63u, 0); break;        /* tlbwi */
    case 0x06: CP0_GET(1, v); tlb_write(v & 31u, 1); break;                               /* tlbwr */
    case 0x08: s_tlb_unmaps = 0; tlb_probe(); break;                                      /* tlbp */
    default: return 0;
    }
    if (bd && !branch_next(x, x->epc, &next)) return 0;
    x->epc = next;
    return 1;
}
/* ---- a TLB exception the game's own handler takes ----------------------------------------------------------
 * The CPU's tables hold what the game's entries map, one 4 KiB page at a time, filled when an access misses
 * (tlb_service). What the entries do not map is the game's to handle, as on the VR4300 -- GoldenEye's code and data
 * at 0x7F000000 are pages its own refill handler maps on demand (tlbwr from a table of its own):
 *   no entry names the page           TLBL (load, fetch) or TLBS (store), the refill vector
 *   an entry names it, V clear        the same code, the general vector
 *   a store to a page with D clear    Mod (code 1), the general vector
 * The refill vector is 0x8000_0000 when the game's EXL is clear and 0x8000_0180 when it is set; the general vector is
 * 0x8000_0180. The monitor does what the CPU does for an exception it takes itself -- EPC (the branch's, with BD, in
 * a delay slot; not changed when EXL was set), Cause.ExcCode, BadVAddr and EntryHi (set by the CPU when the
 * exception came to the monitor), Context's BadVPN2, Status.EXL -- and returns to the vector. The game's handler does its
 * tlbwr (tlb_insn), eret, and the instruction runs again: the access misses the tables, and tlb_service fills the page
 * from the entry the handler wrote. Cause.ExcCode / BD and Context's BadVPN2 are written by mtc0 in the monitor
 * level only (mips_core, MON).
 * The fault's address is COP0 register 22's: a fault that comes to the monitor leaves the game's BadVAddr and EntryHi as the
 * game has them, and the monitor writes both for the exception it delivers. A page that an entry of the game's maps can miss
 * the tables at any time (512 entries by the page number's low 9 bits; a wired entry of the VR4300's TLB never misses) --
 * a libultra that runs from 0x7000_0000 has its exception handler's own page (0x70010) put out of the fetch table by
 * 0x7F010's, and a fill of it that replaced the BadVAddr the handler is about to read would hand the fault thread a TLBL
 * that is not where the game's pages are: it stops the game for ever (black, silent). */
enum { TLBX_FILLED, TLBX_REFILL, TLBX_INVALID, TLBX_MOD, TLBX_NONE };
static int tlb_service(u32 code, u32 badv)
{
    u32 sel, hi;
    CP0_GET(23, sel); CP0_GET(10, hi);
    for (u32 i = 0; i < TLB_N; i++) {
        const u32 ehi = s_tlbr[i].hi ^ 0x80000000u, vpn = ~(s_tlbr[i].mask | 0x1FFFu);
        const u32 g = s_tlbr[i].lo0 & s_tlbr[i].lo1 & 1u;
        if ((badv & vpn) != (ehi & vpn) || (!g && (hi & 0xFFu) != (ehi & 0xFFu))) continue;
        const u32 span = (s_tlbr[i].mask + 0x2000u) >> 1, off = badv - (ehi & 0xFFFFE000u & ~(2u * span - 1u));
        const u32 lo = off >= span ? s_tlbr[i].lo1 : s_tlbr[i].lo0;
        if (!(lo & 2u)) return TLBX_INVALID;
        if (code == 1u && !(lo & 4u)) return TLBX_MOD;
        const u32 pa = ((((lo >> 6) << 12) & ~(span - 1u)) + (off & (span - 1u) & ~0xFFFu));
        if (pa >= 0x00800000u) return TLBX_NONE;                                   /* (not the RDRAM) */
        CP0_SET(10, (badv & 0xFFFFE000u) | (hi & 0xFFu));                          /* (the tables' write is of EntryHi's page pair and ASID) */
        tr_write(badv, pa, TR_V | ((lo & 4u) ? TR_D : 0u) | (g ? TR_G : 0u) | (((lo >> 3) & 7u) != 2u ? TR_C : 0u) | (sel & TR_FETCH));
        CP0_SET(10, hi);                                                           /* (the game's, as it was) */
        s_tlb_fills++;
#ifdef N64RT_DEBUG
        { static u32 s_n; if (s_n++ < 600u) n64rt_log("n64rt: TLB page %08lx -> %08lx (entry %lu, %s%s)\n", (unsigned long)(badv & ~0xFFFu), (unsigned long)pa, (unsigned long)i,
                                                       (sel & TR_FETCH) ? "fetch" : "data", (lo & 4u) ? "" : ", clean"); }
#endif
        return TLBX_FILLED;
    }
    {   /* an entry a tlbwr put out (above): its valid page is entered as if the entry were there still; an invalid one, or a
         * store to a clean one, is the refill the VR4300 would take */
        const int k = vic_find((badv & 0xFFFFE000u) | (hi & 0xFFu));
        if (k >= 0) {
            const u32 lo = (badv & 0x1000u) ? s_vic[k].lo1 : s_vic[k].lo0, g = s_vic[k].lo0 & s_vic[k].lo1 & 1u;
            const u32 pa = ((lo >> 6) << 12) & 0xFFFFF000u;
            if ((lo & 2u) && !(code == 1u && !(lo & 4u)) && pa < 0x00800000u) {
#ifdef N64RT_TLBKEEP_CHECK
                {   /* (a debugging build, for a game whose refill handler reads its page table by Context from the pointer at
                     * N64RT_TLBKEEP_CHECK: the kept entry must be what a refill would write now) */
                    const u32 base = *(volatile u32 *)(uintptr_t)N64RT_TLBKEEP_CHECK;
                    const u32 pte = *(volatile u32 *)(uintptr_t)(base + ((badv >> 13) << 4)) & 0x3FFFFFFFu;
                    static u32 s_n, s_bad;
                    s_n++;
                    if (pte != s_vic[k].lo0 && s_bad++ < 16u)
                        n64rt_log("n64rt: TLBKEEP: page %08lx kept as %08lx, the game's table has %08lx\n", (unsigned long)(badv & ~0x1FFFu), (unsigned long)s_vic[k].lo0, (unsigned long)pte);
                    if ((s_n & 511u) == 0u) n64rt_log("n64rt: TLBKEEP: %lu fills from kept entries (%lu kept now), %lu not the table's\n", (unsigned long)s_n, (unsigned long)n64rt_vic_n, (unsigned long)s_bad);
                }
#endif
                CP0_SET(10, (badv & 0xFFFFE000u) | (hi & 0xFFu));
                tr_write(badv, pa, TR_V | ((lo & 4u) ? TR_D : 0u) | (g ? TR_G : 0u) | (((lo >> 3) & 7u) != 2u ? TR_C : 0u) | (sel & TR_FETCH));
                CP0_SET(10, hi);
                s_tlb_fills++;
                return TLBX_FILLED;
            }
        }
    }
    return TLBX_REFILL;
}
static void tlb_deliver(n64rt_ctx_t *x, int res, u32 code, u32 bd, u32 badv)
{
    u32 cause, ctxt, hi;
    CP0_GET(13, cause); CP0_GET(4, ctxt); CP0_GET(10, hi);
    CP0_SET(8, badv);                                                             /* (BadVAddr: the monitor level's to write) */
    CP0_SET(10, (badv & 0xFFFFE000u) | (hi & 0xFFu));                             /* EntryHi: the page pair, the ASID as it is */
    const u32 exl = x->status & 2u, exc = res == TLBX_MOD ? 1u : code == 3u ? 3u : 2u;
    cause = (cause & ~0x7Cu) | exc << 2;
    if (!exl) { cause = (cause & 0x7FFFFFFFu) | bd << 31; CP0_SET(14, x->epc); }
    CP0_SET(13, cause);
    CP0_SET(4, (ctxt & 0xFF800000u) | ((badv >> 13) << 4));
    x->status |= 2u;                                                              /* (EXL; the eret to the vector clears ERL) */
    const u32 was_epc = x->epc;
    (void)was_epc;
    x->epc = (res == TLBX_REFILL && !exl) ? 0x80000000u : 0x80000180u;
    s_tlb_delivered++;
#ifdef N64RT_DEBUG
    {   static u32 s_n;
        if (s_n++ < 8u || (s_n & 4095u) == 0u)
            n64rt_log("n64rt: TLB %s for the game's handler (#%lu): %s at %08lx, EPC %08lx%s, vector %08lx, Cause %08lx Context %08lx\n",
                      res == TLBX_REFILL ? "refill" : res == TLBX_INVALID ? "invalid" : "modified", (unsigned long)s_n,
                      exc == 3u ? "a store" : exc == 1u ? "a store (Mod)" : "a load or fetch", (unsigned long)badv, (unsigned long)was_epc,
                      bd ? " (delay slot)" : "", (unsigned long)x->epc, (unsigned long)cause, (unsigned long)((ctxt & 0xFF800000u) | ((badv >> 13) << 4)));
    }
#endif
}
/* a translation-table fault of the game's: the page entered, or the exception taken by its handler (1), or neither (0) */
static int tlb_fault(n64rt_ctx_t *x, u32 code, u32 bd)
{
    if (!n64rt_tlbi) return tlb_fill();
    u32 badv;
    CP0_GET(22, badv);                  /* the fault's address (not BadVAddr: that is the game's) */
    const int res = tlb_service(code, badv);
    if (res == TLBX_FILLED) return 1;
    if (res == TLBX_NONE) return 0;
#ifdef N64RT_TLB_FATAL
    return 0;                           /* (a debugging build: the exception is the runtime's, with the game's registers in the log) */
#endif
    tlb_deliver(x, res, code, bd, badv);
    return 1;
}
void n64rt_tlbi_init(void)
{
    u32 v;
    __asm__ volatile("mfc0 %0, $23" : "=r"(v));
    n64rt_tlbi = (v & ~8u) == 0x54420001u;
    for (u32 i = 0; i < TLB_N; i++) s_tlbr[i].hi = 0;               /* (KSEG0: matches nothing) */
}
#else
void n64rt_tlbi_init(void) { }
#endif
int n64rt_tlb_phys(u32 va, u32 *pa)
{
#ifdef N64RT_OWN
    if (!s_tlb_n && !n64rt_vic_n) return 0;
#else
    if (!s_tlb_n) return 0;
#endif
    for (int i = 0; i < TLB_N; i++) {
        u32 span = s_tlb[i].span, off = va - s_tlb[i].va;
        if (span && off < 2u * span) {
            u32 p = s_tlb[i].pa[off >= span];
            if (p == 0xFFFFFFFFu) return 0;
            p += off & (span - 1u);
            if (p >= 0x00800000u) return 0;                 /* (the RDRAM) */
            *pa = p;
            return 1;
        }
    }
#ifdef N64RT_OWN
    if (n64rt_vic_n) {                                      /* (a page of an entry a tlbwr put out: the game's code may be on one) */
        const u32 k = vic_slot(va), ehi = s_vic[k].hi ^ 0x80000000u;
        if (s_vic[k].hi && !((va ^ ehi) & 0xFFFFE000u)) {
            const u32 lo = (va & 0x1000u) ? s_vic[k].lo1 : s_vic[k].lo0, p = (((lo >> 6) << 12) & 0xFFFFF000u) | (va & 0xFFFu);
            if (!(lo & 2u) || p >= 0x00800000u) return 0;
            *pa = p;
            return 1;
        }
    }
#endif
    return 0;
}
/* va is mapped by a valid page of an entry, onto no memory of the console's: 1 */
static int tlb_maps_nothing(u32 va)
{
    for (int i = 0; i < TLB_N; i++) {
        u32 span = s_tlb[i].span, off = va - s_tlb[i].va;
        if (span && off < 2u * span) {
            u32 p = s_tlb[i].pa[off >= span];
            return p != 0xFFFFFFFFu && p + (off & (span - 1u)) >= 0x00800000u;
        }
    }
    return 0;
}
/* n bytes at a mapped address, big-endian */
static int tlb_rd(u32 va, u32 n, u32 *v)
{
    u32 r = 0, pa;
    for (u32 i = 0; i < n; i++) {
        if (!n64rt_tlb_phys(va + i, &pa)) return 0;
        r = r << 8 | *(volatile u8 *)(uintptr_t)(0x80000000u | pa);
    }
    *v = r;
    return 1;
}
static int tlb_wr(u32 va, u32 n, u32 v)
{
    u32 pa;
    for (u32 i = 0; i < n; i++) {
        if (!n64rt_tlb_phys(va + i, &pa)) return 0;
        *(volatile u8 *)(uintptr_t)(0x80000000u | pa) = (u8)(v >> (8u * (n - 1u - i)));
    }
    return 1;
}
/* where the branch at pc goes (its delay slot is being done here: it was
 * taken up, so a "likely" one is taken); the registers as before the slot */
static int branch_next(n64rt_ctx_t *x, u32 pc, u32 *next)
{
    u32 insn;
    if (!insn_at(pc, &insn)) return 0;
    u32 op = insn >> 26, rs = (insn >> 21) & 31u, rt = (insn >> 16) & 31u;
    s64 a = (s64)r64(x, rs), b = (s64)r64(x, rt);        /* (the VR4300 compares all 64 bits) */
    u32 rel = pc + 4u + ((u32)(s32)(s16)insn << 2);
    int take;
    switch (op) {
    case 0x02: case 0x03:                               /* j, jal */
        *next = ((pc + 4u) & 0xF0000000u) | ((insn & 0x03FFFFFFu) << 2);
        if (op == 0x03) gset(x, 31, pc + 8u);
        return 1;
    case 0x00:                                          /* jr, jalr */
        if ((insn & 0x3Fu) == 0x08u) { *next = (u32)a; return 1; }
        if ((insn & 0x3Fu) == 0x09u) {
            u32 rd = (insn >> 11) & 31u;
            *next = (u32)a;
            if (rd) gset(x, rd, pc + 8u);
            return 1;
        }
        return 0;
    case 0x04: case 0x14: take = a == b; break;         /* beq(l) */
    case 0x05: case 0x15: take = a != b; break;         /* bne(l) */
    case 0x06: case 0x16: take = a <= 0; break;         /* blez(l) */
    case 0x07: case 0x17: take = a > 0; break;          /* bgtz(l) */
    case 0x01:                                          /* bltz, bgez, their link and likely forms */
        if ((rt & 0x0Cu) != 0u) return 0;
        take = (rt & 1u) ? a >= 0 : a < 0;
        if (rt & 0x10u) gset(x, 31, pc + 8u);
        break;
    case 0x11:                                          /* bc1f, bc1t (and likely) */
        if (rs != 8u) return 0;
        take = (int)((x->fcsr >> 23) & 1u) == (int)(rt & 1u);
        break;
    default: return 0;
    }
    *next = take ? rel : pc + 8u;
    return 1;
}
/* the load or store that trapped on a mapped page: done, and epc past it */
static int emulate_mapped(n64rt_ctx_t *x, u32 cause)
{
    if (!s_tlb_n) return 0;
    u32 bd = cause >> 31, pc = x->epc + (bd ? 4u : 0u), next = pc + 4u;
    if ((pc >> 28) != 0x8u && (pc >> 28) != 0x4u) return 0;                /* (not code: a jump into a page) */
    u32 insn = *(volatile u32 *)(uintptr_t)pc;
    u32 op = insn >> 26, rs = (insn >> 21) & 31u, rt = (insn >> 16) & 31u;
    u32 ea = g32(x, rs) + (u32)(s32)(s16)insn, k = ea & 3u, v, r = g32(x, rt), pa;
    if (!n64rt_tlb_phys(ea, &pa)) {
        /* A page mapped onto nothing the console has: libultra's osMapTLBRdb maps 0xC000_0000 onto the development board's debug
         * port, physical 0x8000_0000 (entry 31, at osInitialize), for a game that still has
         * the debugger's code in it to write to. On the console the store goes nowhere and a load reads nothing: so here. */
        if (!tlb_maps_nothing(ea)) return 0;
        if (bd && !branch_next(x, x->epc, &next)) return 0;
        if (!emulate_none(x, op, rt)) return 0;
        x->epc = next;
        return 1;
    }
    if (bd && !branch_next(x, x->epc, &next)) return 0;
    switch (op) {
    case 0x20: if (!tlb_rd(ea, 1, &v)) return 0; r = (u32)(s32)(s8)v; break;          /* lb */
    case 0x24: if (!tlb_rd(ea, 1, &v)) return 0; r = v; break;                        /* lbu */
    case 0x21: if (!tlb_rd(ea, 2, &v)) return 0; r = (u32)(s32)(s16)v; break;         /* lh */
    case 0x25: if (!tlb_rd(ea, 2, &v)) return 0; r = v; break;                        /* lhu */
    case 0x23: if (!tlb_rd(ea, 4, &v)) return 0; r = v; break;                        /* lw */
    case 0x22:                                                                        /* lwl: the bytes from ea to its word's end, at the top */
        if (!tlb_rd(ea, 4u - k, &v)) return 0;
        r = k ? (v << (8u * k)) | (r & ((1u << (8u * k)) - 1u)) : v;
        break;
    case 0x26:                                                                        /* lwr: from its word's start to ea, at the bottom */
        if (!tlb_rd(ea - k, k + 1u, &v)) return 0;
        r = k == 3u ? v : (r & ~((1u << (8u * (k + 1u))) - 1u)) | v;
        break;
    case 0x28: if (!tlb_wr(ea, 1, r)) return 0; goto stored;                          /* sb */
    case 0x29: if (!tlb_wr(ea, 2, r)) return 0; goto stored;                          /* sh */
    case 0x2B: if (!tlb_wr(ea, 4, r)) return 0; goto stored;                          /* sw */
    case 0x2A: if (!tlb_wr(ea, 4u - k, r >> (8u * k))) return 0; goto stored;         /* swl */
    case 0x2E: if (!tlb_wr(ea - k, k + 1u, r)) return 0; goto stored;                 /* swr */
    case 0x31: if (!tlb_rd(ea, 4, &v)) return 0; x->fpr[rt] = v; goto stored;         /* lwc1 */
    case 0x39: if (!tlb_wr(ea, 4, x->fpr[rt])) return 0; goto stored;                 /* swc1 */
    case 0x35:                                                                        /* ldc1: a pair, the high word first */
        if (!tlb_rd(ea, 4, &v)) return 0; x->fpr[rt | 1u] = v;
        if (!tlb_rd(ea + 4u, 4, &v)) return 0; x->fpr[rt & ~1u] = v;
        goto stored;
    case 0x3D:                                                                        /* sdc1 */
        if (!tlb_wr(ea, 4, x->fpr[rt | 1u]) || !tlb_wr(ea + 4u, 4, x->fpr[rt & ~1u])) return 0;
        goto stored;
    default: return 0;
    }
    if (rt) gset(x, rt, r);
stored:
    x->epc = next;
    s_tlb_hits++;
    return 1;
}

/* Every thread's state (N64RT_DEBUG, every 2 s of fields: n64rt_io.c) */
void n64rt_game_dump(void) __attribute__((weak));
void n64rt_game_dump(void) { }
void n64rt_dump_threads(void)
{
    n64rt_game_dump();
    int n = 0;
    for (OSThread *t = s_active; t && n < 32; t = t->tlnext, n++) {
        n64rt_ctx_t *x = CTX(t);
        n64rt_log("  thread %2ld pri %3ld %s epc %08lx game ra %08lx queue %08lx sp %08lx%s\n", (long)t->id, (long)t->priority,
                  t->state == OS_STATE_RUNNING ? "RUN " : t->state == OS_STATE_RUNNABLE ? "rdy " :
                  t->state == OS_STATE_WAITING ? "wait" : t->state == OS_STATE_STOPPED ? "stop" : "?   ",
                  (unsigned long)x->epc, (unsigned long)x->wait_ra, (unsigned long)x->wait_q, (unsigned long)x->g[29].l,
                  t == n64rt_cur ? " <" : "");
    }
}

/* ---- the exception handler (n64rt_exc.S, interrupts off) -------------------------- */
/* ---- the 64-bit instructions ---------------------------------------------------------
 * The CPU is 32 bits wide: an N64 game's 64-bit instruction is a reserved one
 * here, and is done on its behalf. A register's upper half is kept beside it
 * from the 64-bit instruction that wrote it, and holds for as long as the
 * register's lower half is still what that left there; any other register is
 * the sign extension of its 32 bits, which is what a 32-bit instruction makes
 * of it on the N64. That covers what games do with them -- a long long
 * through its helper (ld, ddivu, mflo, dsll32, dsra32), a block moved eight
 * bytes at a time (ld, sd) -- at the price of a trap an instruction: for
 * the odd one, not for a loop a frame runs through. */
static struct {
    OSThread *th;
    u32 hi[32], lo[32]; u8 v[32];
    u32 mlo[2], mhi[2];             /* LO and HI of the last 64-bit multiply or divide: {lower, upper} */
    u8 mv;                          /* they stand for this many instructions more */
} s_w;
static u32 s_w_count;
static u64 r64(const n64rt_ctx_t *x, u32 r)
{
    u32 lo = x->g[r].l;
    if (r == 0) return 0;
    if (n64rt_cpu64) return (u64)x->g[r].h << 32 | lo;
    if (s_w.th == n64rt_cur) {
        if (s_w.v[r] && s_w.lo[r] == lo) return (u64)s_w.hi[r] << 32 | lo;
        if (s_w.mv) {                               /* (an mflo / mfhi since: by the value it took) */
            if (lo == s_w.mlo[0]) return (u64)s_w.mlo[1] << 32 | lo;
            if (lo == s_w.mhi[0]) return (u64)s_w.mhi[1] << 32 | lo;
        }
    }
    return (u64)(s64)(s32)lo;
}
static void w64(n64rt_ctx_t *x, u32 r, u64 v)
{
    if (!r) return;
    x->g[r].l = (u32)v; x->g[r].h = (u32)(v >> 32);
    if (n64rt_cpu64) return;
    if (s_w.th != n64rt_cur) { memset(&s_w, 0, sizeof s_w); s_w.th = n64rt_cur; }
    s_w.hi[r] = (u32)(v >> 32); s_w.lo[r] = (u32)v; s_w.v[r] = 1;
}
static int ram_addr(u32 a) { return (a >> 29) == 4u || (a >> 29) == 5u ? (a & 0x1FFFFFFFu) < 0x00800000u : 0; }
static u64 rd64(u32 a) { const volatile u32 *p = (const volatile u32 *)(uintptr_t)(a & ~7u); return (u64)p[0] << 32 | p[1]; }
static void wr64(u32 a, u64 v) { volatile u32 *p = (volatile u32 *)(uintptr_t)(a & ~7u); p[0] = (u32)(v >> 32); p[1] = (u32)v; }
static u64 mulhu64(u64 a, u64 b)
{
    u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
    u64 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    u64 mid = (ll >> 32) + (u32)lh + (u32)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}
/* The FPU's 64-bit integers: the CPU's FPU is single precision with no .l
 * formats and no dmfc1 / dmtc1 (a double is a pair of registers, computed as
 * a float), so these are done here, as the reference model does them: a
 * 64-bit integer is the pair {odd, even}; a conversion from it rounds to
 * nearest even into a float (widened for a .d result); one to it rounds the
 * float by the instruction or FCR31 and saturates, NaN to the largest. */
static u32 l2f(s64 v)
{
    if (v == 0) return 0;
    u32 s = v < 0;
    u64 m = s ? (u64)0 - (u64)v : (u64)v;
    int e = 63;
    /* (normalised by halves: a bit a step was some 400 instructions of this 32-bit code a conversion) */
    if (!(m >> 32)) { m <<= 32; e -= 32; }
    if (!(m >> 48)) { m <<= 16; e -= 16; }
    if (!(m >> 56)) { m <<= 8; e -= 8; }
    if (!(m >> 60)) { m <<= 4; e -= 4; }
    if (!(m >> 62)) { m <<= 2; e -= 2; }
    if (!(m >> 63)) { m <<= 1; e -= 1; }
    u32 mant = (u32)(m >> 40);
    u64 rest = m & 0xFFFFFFFFFFull;
    if (rest > 0x8000000000ull || (rest == 0x8000000000ull && (mant & 1u))) { mant++; if (mant >> 24) { mant >>= 1; e++; } }
    return s << 31 | (u32)(e + 127) << 23 | (mant & 0x7FFFFFu);
}
static s64 f2l(u32 b, u32 rm)                       /* rm: 0 nearest even, 1 to zero, 2 up, 3 down */
{
    u32 s = b >> 31, m = (b & 0x7FFFFFu) | 0x800000u;
    int e = (int)((b >> 23) & 0xFFu);
    if (e == 0xFF && (b & 0x7FFFFFu)) return 0x7FFFFFFFFFFFFFFFll;
    if (e == 0) return 0;                            /* (below the normals: zero, as the FPU takes it) */
    int sh = e - 150;                                /* the value is m * 2^sh */
    if (sh >= 40) return s ? (s64)0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFll;
    if (sh >= 0) { u64 v = (u64)m << sh; return s ? -(s64)v : (s64)v; }
    u32 r = (u32)-sh, ip = 0, nz = 1, gt = 0, eq = 0;
    if (r < 26u) {
        u32 rem = m & ((1u << r) - 1u), half = 1u << (r - 1u);
        ip = r < 25u ? m >> r : 0u; nz = rem != 0; gt = rem > half; eq = rem == half;
    }
    if (rm == 0u) { if (gt || (eq && (ip & 1u))) ip++; }
    else if (rm == 2u) { if (!s && nz) ip++; }
    else if (rm == 3u) { if (s && nz) ip++; }
    return s ? -(s64)ip : (s64)ip;
}
static u32 d2f(u32 lo, u32 hi)                      /* a double's bits as the FPU takes them: a float's */
{
    u32 s = hi & 0x80000000u;
    s32 e = (s32)((hi >> 20) & 0x7FFu);
    if (e == 0x7FF) return s | 0x7F800000u | ((hi & 0xFFFFFu) << 3) | (lo >> 29) | ((lo & 0x1FFFFFFFu) != 0);
    e += 127 - 1023;
    if (e <= 0) return s;
    if (e >= 255) return s | 0x7F800000u;
    return (s | ((u32)e << 23) | ((hi & 0xFFFFFu) << 3) | (lo >> 29)) + ((lo >> 28) & 1u);
}
static int emulate_fpu64(n64rt_ctx_t *x, u32 insn)
{
    u32 fmt = (insn >> 21) & 31u, rt = (insn >> 16) & 31u, fs = (insn >> 11) & 31u, fd = (insn >> 6) & 31u, fn = insn & 63u;
    u32 *lo = &x->fpr[fs & ~1u], *hi = &x->fpr[fs | 1u];
    if (fmt == 0x01u) { w64(x, rt, (u64)*hi << 32 | *lo); return 1; }                        /* dmfc1 */
    if (fmt == 0x05u) { u64 v = r64(x, rt); *lo = (u32)v; *hi = (u32)(v >> 32); return 1; }  /* dmtc1 */
    if (fmt == 0x15u) {                                                                      /* cvt.s.l, cvt.d.l */
        u32 f = l2f((s64)((u64)*hi << 32 | *lo));
        if (fn == 0x20u) { x->fpr[fd] = f; return 1; }
        if (fn != 0x21u) return 0;
        u32 e = (f >> 23) & 0xFFu, m = f & 0x7FFFFFu;
        x->fpr[fd | 1u] = e == 0 ? f & 0x80000000u : (f & 0x80000000u) | ((e == 0xFFu ? 0x7FFu : e - 127u + 1023u) << 20) | (m >> 3);
        x->fpr[fd & ~1u] = e == 0 ? 0 : m << 29;
        return 1;
    }
    if ((fmt == 0x10u || fmt == 0x11u) && ((fn >= 0x08u && fn <= 0x0Bu) || fn == 0x25u)) {   /* round / trunc / ceil / floor / cvt .l */
        u32 f = fmt == 0x11u ? d2f(*lo, *hi) : x->fpr[fs];
        s64 v = f2l(f, fn == 0x25u ? x->fcsr & 3u : fn & 3u);
        x->fpr[fd & ~1u] = (u32)v; x->fpr[fd | 1u] = (u32)((u64)v >> 32);
        return 1;
    }
    return 0;
}

#ifdef N64RT_DEBUG
#define RI_HIST 64u
static u32 s_ri_pc[RI_HIST], s_ri_n[RI_HIST];
void n64rt_ri_hist_log(void)
{
    u32 tot = 0, shown = 0;
    for (u32 i = 0; i < RI_HIST; i++) tot += s_ri_n[i];
    for (u32 k = 0; k < 12u; k++) {                  /* the twelve most used */
        u32 b = RI_HIST;
        for (u32 i = 0; i < RI_HIST; i++) if (s_ri_n[i] && (b == RI_HIST || s_ri_n[i] > s_ri_n[b])) b = i;
        if (b == RI_HIST) break;
        n64rt_log("n64rt:   done for the game at %08lx: %lu of %lu\n", (unsigned long)s_ri_pc[b], (unsigned long)s_ri_n[b], (unsigned long)tot);
        s_ri_n[b] = 0; shown++;
    }
    for (u32 i = 0; i < RI_HIST; i++) { s_ri_n[i] = 0; s_ri_pc[i] = 0; }
}
#endif
static int emulate_64(n64rt_ctx_t *x, u32 cause)
{
    u32 bd = cause >> 31, pc = x->epc + (bd ? 4u : 0u), next = pc + 4u;
    u32 insn;
    if ((pc >> 28) == 0x4u || !insn_at(pc, &insn)) return 0;    /* (the game's code only) */
    u32 op = insn >> 26, rs = (insn >> 21) & 31u, rt = (insn >> 16) & 31u, rd = (insn >> 11) & 31u, sa = (insn >> 6) & 31u, fn = insn & 63u;
    s32 imm = (s16)insn;
    u32 ea = g32(x, rs) + (u32)imm;
    if (bd && !branch_next(x, x->epc, &next)) return 0;
    if (s_w.th != n64rt_cur) { memset(&s_w, 0, sizeof s_w); s_w.th = n64rt_cur; }
    u8 mv = s_w.mv ? (u8)(s_w.mv - 1u) : 0u;
    switch (op) {
    case 0x37: if (!ram_addr(ea) || (ea & 7u)) return 0; w64(x, rt, rd64(ea)); break;                 /* ld */
    case 0x3F: if (!ram_addr(ea) || (ea & 7u)) return 0; wr64(ea, r64(x, rt)); break;                 /* sd */
    case 0x27: if (!ram_addr(ea) || (ea & 3u)) return 0; w64(x, rt, *(volatile u32 *)(uintptr_t)ea); break;   /* lwu */
    case 0x1A: case 0x1B: case 0x2C: case 0x2D: {                                                      /* ldl ldr sdl sdr */
        if (!ram_addr(ea)) return 0;
        u32 n = ea & 7u;
        u64 m = rd64(ea), r = r64(x, rt);
        if (op == 0x1A) w64(x, rt, n ? ((r & ((1ull << (8u * n)) - 1ull)) | (m << (8u * n))) : m);
        else if (op == 0x1B) w64(x, rt, n == 7u ? m : ((r & (~0ull << (8u * (n + 1u)))) | (m >> (8u * (7u - n)))));
        else if (op == 0x2C) wr64(ea, n ? ((m & (~0ull << (8u * (8u - n)))) | (r >> (8u * n))) : r);
        else wr64(ea, n == 7u ? r : ((m & ((1ull << (8u * (7u - n))) - 1ull)) | (r << (8u * (7u - n)))));
        break;
    }
    case 0x11:
        if (!emulate_fpu64(x, insn)) return 0;
        /* The compiler's conversions between a 64-bit integer and a float (libultra's __ll_to_d, __f_to_ll ...) are two of
         * these in a row -- dmtc1 / jr ra / cvt.d.l in its delay slot, or trunc.l.s / dmfc1 -- and some games call them
         * a hundred times a field: the ones behind this one -- a nop or two between them, as hand-written code has them
         * (dmtc1 / nop / cvt.s.l) -- or the one in the delay slot of the `jr ra` behind it, are done on the same entry. */
        if (!bd) {
            u32 n1, n2;
            const u32 ra = g32(x, 31);
            for (int k = 0; k < 4; k++) {
                u32 p = next;
                if (!insn_at(p, &n1)) break;
                for (int z = 0; z < 2 && n1 == 0u; z++) { p += 4u; if (!insn_at(p, &n1)) { n1 = 0u; break; } }
                if ((n1 >> 26) == 0x11u && emulate_fpu64(x, n1)) { next = p + 4u; continue; }
                if (p == next && n1 == 0x03E00008u && insn_at(next + 4u, &n2) && (n2 >> 26) == 0x11u && emulate_fpu64(x, n2)) next = ra;
                break;
            }
        }
        break;
    case 0x18: case 0x19: w64(x, rt, r64(x, rs) + (u64)(s64)imm); break;                             /* daddi(u) */
    /* (what the CPU hands over when an operand is not a 32-bit value) */
    case 0x0A: w64(x, rt, (s64)r64(x, rs) < (s64)imm); break;                                        /* slti */
    case 0x0B: w64(x, rt, r64(x, rs) < (u64)(s64)imm); break;                                        /* sltiu */
    case 0x0D: w64(x, rt, r64(x, rs) | (u16)insn); break;                                            /* ori */
    case 0x0E: w64(x, rt, r64(x, rs) ^ (u16)insn); break;                                            /* xori */
    case 0x00: {
        u64 a = r64(x, rs), b = r64(x, rt);
        switch (fn) {
        case 0x14: w64(x, rd, b << (g32(x, rs) & 63u)); break;                                       /* dsllv */
        case 0x16: w64(x, rd, b >> (g32(x, rs) & 63u)); break;                                       /* dsrlv */
        case 0x17: w64(x, rd, (u64)((s64)b >> (g32(x, rs) & 63u))); break;                           /* dsrav */
        case 0x24: w64(x, rd, a & b); break;
        case 0x25: w64(x, rd, a | b); break;
        case 0x26: w64(x, rd, a ^ b); break;
        case 0x27: w64(x, rd, ~(a | b)); break;
        case 0x2A: w64(x, rd, (s64)a < (s64)b); break;                                               /* slt */
        case 0x2B: w64(x, rd, a < b); break;                                                         /* sltu */
        /* mthi, mtlo of a 64-bit value: kept here, the CPU's own being 32 bits
         * (the other of the two is in the context as the CPU had it) */
        case 0x11: if (!n64rt_cpu64) return 0; x->hi.l = (u32)a; x->hi.h = (u32)(a >> 32); x->soft = 1; break;
        case 0x13: if (!n64rt_cpu64) return 0; x->lo.l = (u32)a; x->lo.h = (u32)(a >> 32); x->soft = 1; break;
        case 0x10: if (!x->soft) return 0; w64(x, rd, (u64)x->hi.h << 32 | x->hi.l); mv = s_w.mv; break;   /* mfhi, mflo of the */
        case 0x12: if (!x->soft) return 0; w64(x, rd, (u64)x->lo.h << 32 | x->lo.l); mv = s_w.mv; break;   /* runtime's HI and LO */
        case 0x2C: case 0x2D: w64(x, rd, a + b); break;                                              /* dadd(u) */
        case 0x2E: case 0x2F: w64(x, rd, a - b); break;                                              /* dsub(u) */
        case 0x38: w64(x, rd, b << sa); break;                                                       /* dsll */
        case 0x3A: w64(x, rd, b >> sa); break;                                                       /* dsrl */
        case 0x3B: w64(x, rd, (u64)((s64)b >> sa)); break;                                           /* dsra */
        case 0x3C: w64(x, rd, b << (sa + 32u)); break;                                               /* dsll32 */
        case 0x3E: w64(x, rd, b >> (sa + 32u)); break;                                               /* dsrl32 */
        case 0x3F: w64(x, rd, (u64)((s64)b >> (sa + 32u))); break;                                   /* dsra32 */
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: {                                                 /* dmult(u) ddiv(u) */
            u64 lo = 0, hi = 0;
            if (fn == 0x1D) { lo = a * b; hi = mulhu64(a, b); }
            else if (fn == 0x1C) {
                lo = a * b; hi = mulhu64(a, b);
                if ((s64)a < 0) hi -= b;
                if ((s64)b < 0) hi -= a;
            } else if (b == 0) {                      /* (what the VR4300 leaves) */
                lo = fn == 0x1E && (s64)a < 0 ? 1ull : ~0ull; hi = a;
            } else {
                if (fn == 0x1F) { lo = a / b; hi = a % b; }
                else if (!(a == 0x8000000000000000ull && b == ~0ull)) { lo = (u64)((s64)a / (s64)b); hi = (u64)((s64)a % (s64)b); }
                else { lo = a; hi = 0; }
            }
            x->lo.l = (u32)lo; x->lo.h = (u32)(lo >> 32); x->hi.l = (u32)hi; x->hi.h = (u32)(hi >> 32);
            x->soft = n64rt_cpu64 && !n64rt_cpu_md;   /* (the exception path loads them -- with a CPU whose HI and LO are 32 bits,
                                                       * the lower words, and tells it to trap mfhi / mflo) */
            s_w.mlo[0] = (u32)lo; s_w.mlo[1] = (u32)(lo >> 32); s_w.mhi[0] = (u32)hi; s_w.mhi[1] = (u32)(hi >> 32);
            mv = 12;
            break;
        }
        default: return 0;
        }
        break;
    }
    default: return 0;
    }
    s_w.mv = mv;
    x->epc = next;
#ifdef N64RT_DEBUG
    {   /* a histogram of where the instructions done for the game are (n64rt_ri_hist_log) */
        u32 i = 0;
        while (i < RI_HIST && s_ri_pc[i] != pc && s_ri_n[i]) i++;
        if (i < RI_HIST) { s_ri_pc[i] = pc; s_ri_n[i]++; }
    }
    if (++s_w_count <= 6u || (s_w_count & (s_w_count - 1u)) == 0u)
        n64rt_log("n64rt: a 64-bit instruction (%08lx at %08lx), done for the game: %lu so far\n", (unsigned long)insn, (unsigned long)pc, (unsigned long)s_w_count);
#else
    s_w_count++;
#endif
    return 1;
}

static const char *const s_exc_name[32] = {
    "Int", "Mod", "TLBL", "TLBS", "AdEL", "AdES", "IBE", "DBE", "Sys", "Bp", "RI", "CpU", "Ov", "Tr",
};
#ifdef N64RT_DEBUG
/* the last exceptions, for the message of one that stops the game */
static struct { u32 cause, epc, ra, sp; s32 id; } s_xlog[24];
static u32 s_xn;
#endif
void n64rt_exception(void)
{
    n64rt_ctx_t *x = CTX(n64rt_cur);
    u32 cause; __asm__ volatile("mfc0 %0, $13" : "=r"(cause));
    u32 code = (cause >> 2) & 31u;
#ifdef N64RT_STATS
    stat_exc(code);
#endif
#ifdef N64RT_DEBUG
    s_xlog[s_xn % 24u].cause = cause; s_xlog[s_xn % 24u].epc = x->epc; s_xlog[s_xn % 24u].ra = x->g[31].l;
    s_xlog[s_xn % 24u].sp = x->g[29].l; s_xlog[s_xn % 24u].id = n64rt_cur->id; s_xn++;
#endif
    if (code == 0) {
#ifdef N64RT_DEBUG
        s_irq_epc = x->epc;
#endif
        if (cause & (1u << 15)) n64rt_tick();
#ifdef N64RT_RSP
        if (cause & N64RT_IM_SP) { n64rt_sp_irq(); cause &= ~N64RT_IM_SP; }    /* a task's BREAK */
#endif
        if (cause & 0x7F00u & x->status) {           /* nothing else is expected: mask it */
            n64rt_log("n64rt: stray interrupt (cause %08lx), masked\n", (unsigned long)cause);
            x->status &= ~(cause & 0x7F00u);
        }
    } else if (code == 8) {
        x->epc += 4;                                  /* past the syscall (n64rt_switch) */
    } else if (code == 9 && !(cause >> 31) && n64rt_hook_break(x->epc)) {
        /* the boot segment going into the code it loaded: its functions replaced, the instruction again */
    } else if ((code == 4 || code == 5) && emulate_hw(x, cause)) {
        /* an N64 register the game reached directly */
    } else if (n64rt_tlbram && code >= 1 && code <= 3 && tlb_fill()) {
        /* a page the game mapped, entered in the CPU's tables: the instruction again */
    } else if (code >= 2 && code <= 5 && emulate_mapped(x, cause)) {
        /* a page the game mapped (osMapTLB) */
    } else if (code == 10 && s_probe) {
        s_probe = 2; x->epc += 4;                     /* the probe's instruction: this CPU has none */
    } else if (code == 10 && emulate_64(x, cause)) {
        /* a 64-bit instruction */
    } else {
        u32 badv; __asm__ volatile("mfc0 %0, $8" : "=r"(badv));
        u32 epc = x->epc + ((cause >> 31) ? 4u : 0u);
#ifdef N64RT_DEBUG
        for (u32 i = s_xn > 24u ? s_xn - 24u : 0u; i < s_xn; i++)
            n64rt_log("  before it: thread %ld cause %08lx epc %08lx ra %08lx sp %08lx\n", (long)s_xlog[i % 24u].id,
                      (unsigned long)s_xlog[i % 24u].cause, (unsigned long)s_xlog[i % 24u].epc, (unsigned long)s_xlog[i % 24u].ra,
                      (unsigned long)s_xlog[i % 24u].sp);
        for (u32 r = 1; r < 32u; r++)
            n64rt_log("  r%lu %08lx%08lx%s", (unsigned long)r, (unsigned long)x->g[r].h, (unsigned long)x->g[r].l, (r & 3u) == 3u ? "\n" : "");
        n64rt_log("\n");
#endif
        u32 insn = (epc >> 28) == 0x8u || (epc >> 28) == 0x4u ? *(volatile u32 *)(uintptr_t)epc : 0;
        n64rt_fatal("n64rt: %s exception in thread %ld at %08lx (%08lx%s), BadVAddr %08lx, ra %08lx sp %08lx\n",
                    s_exc_name[code] ? s_exc_name[code] : "?", (long)n64rt_cur->id, (unsigned long)epc,
                    (unsigned long)insn, (cause >> 31) ? ", in a delay slot" : "", (unsigned long)badv,
                    (unsigned long)x->g[31].l, (unsigned long)x->g[29].l);
    }
    schedule();
}

#ifdef N64RT_OWN
/* ---- the monitor level (N64RT_OWN: the game's own libultra runs; n64rt_exc.S) ------
 * Entered for what the game's handler must not see: a device register (an
 * address error whose address the registers give), an instruction the CPU
 * lacks, a page the game mapped, and the hardware's wake-ups (the RSP's
 * interrupt, a field, a tick, the RDP's SYNC FULL: n64rt_io.c). ErrorEPC is
 * ctx.epc; the game's registers are the context's. */
/* (4 KiB aligned: n64rt_exc.S's fast path reaches every field of it from %hi(n64rt_mon_ctx) alone) */
n64rt_ctx_t n64rt_mon_ctx __attribute__((aligned(4096)));
/* The fast path (n64rt_exc.S, exception_entry): the game's plain lw / sw of a register that is a copy of another
 * word -- the SP's and the DPC's (the RSP hardware's at 0x2000_0000), PI_DRAM_ADDR, PI_CART_ADDR and the PI's
 * domain registers, the RI's (the runtime's own tables) -- done in the entry itself with $k0 and $k1 only, nothing
 * else saved; everything else comes to n64rt_mon_exception as before. Only with the MI in hardware: without it the
 * line (n64rt_rcp_line) has to be worked out after every register write. */
void n64rt_mon_fast_enable(void)
{
#if !defined(N64RT_NO_FASTMON) && !defined(N64RT_STATS)
    n64rt_mon_ctx.fast_on = 1;
#endif
}
u32 n64rt_mon_fast_count(void) { return n64rt_mon_ctx.fast_n; }
void n64rt_rcp_events(void);
void n64rt_rcp_line(void);
void n64rt_mon_fatal(void)
{
    u32 st, ee, cs, ep, bv, mc;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st)); __asm__ volatile("mfc0 %0, $30" : "=r"(ee));
    __asm__ volatile("mfc0 %0, $13" : "=r"(cs)); __asm__ volatile("mfc0 %0, $14" : "=r"(ep));
    __asm__ volatile("mfc0 %0, $8" : "=r"(bv)); __asm__ volatile("mfc0 %0, $7" : "=r"(mc));
#ifdef N64RT_DEBUG
    { extern void n64rt_mon_dump(void); n64rt_mon_dump(); }
#endif
    n64rt_fatal("n64rt: an exception outside the monitor level: Status %08lx Cause %08lx EPC %08lx BadVAddr %08lx MCTL %08lx ErrorEPC %08lx\n",
                (unsigned long)st, (unsigned long)cs, (unsigned long)ep, (unsigned long)bv, (unsigned long)mc, (unsigned long)ee);
}
#ifdef N64RT_DEBUG
static struct { u32 code, epc, k0, k1, k0_out, epc_out, bd; } s_mlog[16];
static u32 s_mn;
u32 n64rt_mon_codes[32];                /* entries by cause, for the log */
u32 n64rt_mon_epc;                      /* where the game was at the last one */
#endif
/* The game's osMapTLB, osUnmapTLB, osUnmapTLBAll, whose first instruction n64rt_boot.c made a reserved one
 * (SPECIAL 0x35, the function's number in the code field): done here with the game's arguments (a0-a3, the rest
 * on its stack), and the game goes on at its return address. */
static int own_tlb_call(n64rt_ctx_t *x)
{
    u32 epc = x->epc;
    if ((epc >> 28) != 0x8u) return 0;
    u32 insn = *(volatile u32 *)(uintptr_t)epc;
    if ((insn & 0xFC00003Fu) != 0x35u) return 0;
    u32 f = (insn >> 6) & 0xFFFFFu, sp = x->g[29].l;
    if (f == N64RT_osMapTLB) {
        const volatile u32 *st = (const volatile u32 *)(uintptr_t)sp;
        n64rt_osMapTLB((s32)x->g[4].l, x->g[5].l, x->g[6].l, x->g[7].l, st[4], (s32)st[5]);
    } else if (f == N64RT_osUnmapTLB) {
        n64rt_osUnmapTLB((s32)x->g[4].l);
    } else if (f == N64RT_osUnmapTLBAll) {
        n64rt_osUnmapTLBAll();
    } else return 0;
    x->epc = x->g[31].l;
    return 1;
}
void n64rt_mon_exception(void)
{
    n64rt_ctx_t *x = &n64rt_mon_ctx;
    u32 mctl; __asm__ volatile("mfc0 %0, $7" : "=r"(mctl));
    u32 code = (mctl >> 3) & 31u, bd = (mctl >> 2) & 1u, cause = bd << 31 | code << 2;
#ifdef N64RT_DEBUG
    u32 *ml = &s_mlog[s_mn++ & 15u].code;
    ml[0] = code; ml[1] = x->epc; ml[2] = x->g[26].l; ml[3] = x->g[27].l;
    n64rt_mon_codes[code]++; n64rt_mon_epc = x->epc; ml[6] = bd;
#endif
#ifdef N64RT_STATS
    stat_exc(code);
#endif
    if (code == 31) {
        n64rt_rcp_events();
    } else if ((code == 4 || code == 5) && emulate_hw(x, cause)) {
        /* an RCP register */
    } else if (n64rt_tlbram && code >= 1 && code <= 3 && tlb_fault(x, code, bd)) {
        /* a page the game mapped, entered in the CPU's tables (the instruction again), or the exception for the game's handler */
    } else if (code >= 2 && code <= 5 && emulate_mapped(x, cause)) {
        /* a page the game mapped (osMapTLB) */
    } else if (code == 10 && s_probe) {
        s_probe = 2; x->epc += 4;
    } else if (code == 10 && n64rt_tlbi && tlb_insn(x, (int)bd)) {
        /* the game's own tlbwi / tlbwr / tlbr / tlbp */
    } else if (code == 10 && !bd && own_tlb_call(x)) {
        /* osMapTLB / osUnmapTLB / osUnmapTLBAll, the runtime's (n64rt_boot.c: a core that does not take the TLB instructions) */
    } else if (code == 10 && emulate_64(x, cause)) {
        /* a 64-bit instruction */
    } else {
        u32 epc = x->epc + (bd ? 4u : 0u);
        u32 insn = (epc >> 28) == 0x8u || (epc >> 28) == 0x4u ? *(volatile u32 *)(uintptr_t)epc : 0;
#ifdef N64RT_DEBUG
        {   u32 cs, st, ep;
            __asm__ volatile("mfc0 %0, $13" : "=r"(cs)); __asm__ volatile("mfc0 %0, $12" : "=r"(st)); __asm__ volatile("mfc0 %0, $14" : "=r"(ep));
            n64rt_log("  the game's Cause %08lx Status %08lx EPC %08lx; MCTL %08lx; the access: %08lx\n", (unsigned long)cs, (unsigned long)st,
                      (unsigned long)ep, (unsigned long)mctl, (unsigned long)(g32(x, (insn >> 21) & 31u) + (u32)(s32)(s16)insn));
            for (u32 r = 1; r < 32u; r++)
                n64rt_log("  r%lu %08lx%08lx%s", (unsigned long)r, (unsigned long)x->g[r].h, (unsigned long)x->g[r].l, (r & 3u) == 3u ? "\n" : "");
            n64rt_log("\n");
            { extern void n64rt_mon_dump(void); n64rt_mon_dump(); }
        }
#endif
        n64rt_fatal("n64rt: %s exception for the monitor at %08lx (%08lx%s), ra %08lx sp %08lx\n",
                    s_exc_name[code] ? s_exc_name[code] : "?", (unsigned long)epc, (unsigned long)insn, bd ? ", in a delay slot" : "",
                    (unsigned long)x->g[31].l, (unsigned long)x->g[29].l);
    }
    n64rt_rcp_line();
#ifdef N64RT_DEBUG
    ml[4] = x->g[26].l; ml[5] = x->epc;
#endif
}
#ifdef N64RT_DEBUG
void n64rt_mon_dump(void)
{
    for (u32 i = s_mn > 16u ? s_mn - 16u : 0u; i < s_mn; i++) {
        const u32 *m = &s_mlog[i & 15u].code;
        n64rt_log("  monitor %lu: code %lu epc %08lx%s k0 %08lx k1 %08lx -> k0 %08lx epc %08lx\n", (unsigned long)i, (unsigned long)m[0], (unsigned long)m[1],
                  m[6] ? " (delay slot)" : "", (unsigned long)m[2], (unsigned long)m[3], (unsigned long)m[4], (unsigned long)m[5]);
    }
}
#endif
#endif

/* ---- start: the boot "thread" becomes the game's first ----------------------------- */
void n64rt_os_init(void)
{
    s_ratio_q32 = ((u64)N64_COUNT_HZ << 32) / CONFIG_CLOCK_FREQUENCY;
    {   /* Count's ticks, two of the CPU's clocks each, over a stretch of uptime */
        u32 c0, c1;
        u64 u0 = uptime(), u1;
        __asm__ volatile("mfc0 %0, $9" : "=r"(c0));
        do u1 = uptime(); while (u1 - u0 < 16384u);
        __asm__ volatile("mfc0 %0, $9" : "=r"(c1));
        u32 q = (u32)(((u64)(c1 - c0) << 9) / (u1 - u0));
        if (q >= 128u && q <= 1024u) s_count_q8 = q;
    }
    memset(&s_boot, 0, sizeof s_boot);
    s_boot.priority = -1;
    s_boot.state = OS_STATE_RUNNING;
    s_boot.id = -1;
    n64rt_cur = &s_boot;
    /* which CPU: a 64-bit instruction runs, or traps (the exception path
     * saves and restores the registers' upper halves only on the first) */
    static u64 s_probe_mem __attribute__((aligned(8)));
    s_probe = 1;
    __asm__ volatile(".set push\n.set mips3\nld $2, 0(%0)\n.set pop" : : "r"(&s_probe_mem) : "$2");
    n64rt_cpu64 = s_probe == 1;
    s_probe = 0;
    if (n64rt_cpu64) {                              /* (which 64-bit levels: COP0 7's top bits, 0 on a core before them) */
        u32 m; __asm__ volatile("mfc0 %0, $7" : "=r"(m));
        n64rt_cpu_alu = m >> 31 & 1u; n64rt_cpu_sh = m >> 30 & 1u; n64rt_cpu_md = m >> 29 & 1u;
    }
    /* and its translation tables: COP0 23 reads their mark, or 0 */
    {
        u32 v;
        __asm__ volatile("mfc0 %0, $23" : "=r"(v));
        n64rt_tlbram = (v & ~9u) == 0x54420000u;
        n64rt_tlbi_init();
        tr_flush();
    }
#ifdef N64RT_DEBUG
    n64rt_log("n64rt: the CPU %s the 64-bit integers (alu %lu, shifts %lu, dmult %lu), %s translation tables\n", n64rt_cpu64 ? "has" : "is without",
              (unsigned long)n64rt_cpu_alu, (unsigned long)n64rt_cpu_sh, (unsigned long)n64rt_cpu_md, n64rt_tlbram ? "has" : "no");
    if (s_count_q8 < 250u || s_count_q8 > 262u)
        n64rt_log("n64rt: the CPU's clock is %lu.%02lu times the SoC's\n", (unsigned long)(s_count_q8 >> 8), (unsigned long)(((s_count_q8 & 255u) * 100u) >> 8));
#endif
#ifndef N64RT_OWN                       /* (the game's own Compare timer runs there) */
    s_next_field = uptime() + VI_PERIOD;
    arm();
#endif
}

/* Run the game's entry point as a thread of its own (IPL3's jump), at the
 * highest priority until it creates and starts its threads; the boot
 * context never runs again. */
void n64rt_os_start(u32 entry, u32 sp)
{
    static OSThread s_main;
    n64rt_osCreateThread(&s_main, 0, (void (*)(void *))(uintptr_t)entry, 0, (void *)(uintptr_t)(sp + 16), 0x7F);
    s_main.state = OS_STATE_RUNNING;
    n64rt_cur = &s_main;
#ifdef N64RT_OWN
    /* the game starts as IPL3 leaves it: CU0, CU1, FR, interrupts off; the monitor is
     * woken by what the hardware sees (n64rt_resume sets MCTL.GAME on its way) */
    CTX(&s_main)->status = 0x34000002u;
    {   /* (the MI in hardware, if the core has it: then the SP and DP wake-ups the monitor needed only for the MI's line are not) */
        extern void n64rt_mi_init(void); extern u32 n64rt_mi_hw(void), n64rt_vi_hw(void), n64rt_ai_hw(void), n64rt_si_hw(void);
        extern void n64rt_vi_hw_event(void);
        n64rt_mi_init();
        monitor_pend_write(0x1Fu);
        monitor_mask_write((n64rt_mi_hw() ? (n64rt_ai_hw() ? 0xEu : 0x6u) : 0xFu) | (n64rt_si_hw() ? 0x10u : 0u));   /* (the SI in hardware: the PIF's commands wake the monitor) */
        /* (the VI in hardware, if the core has it: MONITOR.CTL bit 3, with the scan-out's window and size from the registers'
         * reset values; the field's wake-up is then the registers' change) */
        monitor_ctl_write((n64rt_mi_hw() ? (n64rt_vi_hw() ? 13u : 5u) : 1u) | (n64rt_ai_hw() ? 16u : 0u));
        if (n64rt_vi_hw()) n64rt_vi_hw_event();
        /* (the AI in hardware: MONITOR.CTL bit 4, the resampler at its default rate; AI_DACRATE's write wakes the monitor) */
        if (n64rt_ai_hw()) { extern void n64rt_ai_hw_reset(void); n64rt_ai_hw_reset(); }
    }
#endif
    n64rt_resume();
}
