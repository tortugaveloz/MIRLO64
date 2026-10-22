/* See log.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <generated/csr.h>
#include <generated/mem.h>

#include "log.h"
#include "trap_arch.h"

unsigned long log_dropped;

/* Log text waiting to go out. Sized to hold several seconds of reports at the
 * rate the JTAG UART actually drains (~0.3 KB/s observed), so a burst around a
 * test transition does not lose anything. */
#define LOG_RING 4096u
static char     s_ring[LOG_RING];
static uint32_t s_head, s_tail;   /* free-running; index with % LOG_RING */

void log_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;

    uint32_t free = LOG_RING - (s_head - s_tail);
    if ((uint32_t)n > free) {
        /* Drop the whole line rather than half of it: a truncated line is
         * worse than a missing one, because it silently reads as data. */
        log_dropped += (unsigned long)n;
        return;
    }
    for (int i = 0; i < n; i++)
        s_ring[s_head++ % LOG_RING] = buf[i];
}

void log_pump(void)
{
    /* Push only while the UART will take a byte without waiting. Never spin:
     * the whole point is that the caller is inside the frame loop. Whatever
     * does not fit goes out next frame.
     *
     * The ev_pending_tx write matters and is easy to miss: libbase's
     * uart_write() does it after every byte, and without it the FIFO does not
     * advance -- output silently stopped after ~21 characters, which looked
     * exactly like a too-small FIFO rather than a missing acknowledgement. */
    while (s_tail != s_head && !uart_txfull_read()) {
        uart_rxtx_write((uint8_t)s_ring[s_tail++ % LOG_RING]);
        uart_ev_pending_tx_write(1);
    }
}

/* Trap reporter, installed as mtvec by lang/mips/linker/init_asm.S whenever it is
 * linked in. Without it the BIOS's vector stays live after an SFL boot, and
 * its isr() only services PLIC claims: a synchronous trap -- e.g. the bus
 * error the Wishbone interconnect returns when a slave has not acked for 1e6
 * cycles -- found no claim, mret'd straight back to the faulting instruction,
 * which timed out again, forever and without a word on the UART. This prints
 * what trapped where, then halts; the log already queued is flushed first so
 * the report lands after it. Interrupts are not used by this firmware (UART
 * polling), so a stray one is masked and resumed. */
#define rd_mcause trap_cause
#define rd_mepc   trap_epc
#define rd_mtval  trap_tval

/* ---- stall watchdog (see log.h) ---- */
volatile char     wd_marks[64];
volatile unsigned wd_mark_pos;
static unsigned long s_wd_cycles;

void watchdog_kick(void)
{
    if (!s_wd_cycles) return;
    timer_irq_arm(s_wd_cycles);
}

void watchdog_start(unsigned long cycles)
{
    s_wd_cycles = cycles;
    watchdog_kick();
    timer_irq_enable();
}

void watchdog_stop(void)
{
    timer_irq_disable();
    s_wd_cycles = 0;
}

/* ---- sampling PC profiler (see log.h) ---- */
#define PROF_BASE   0x40000000u
#define PROF_SHIFT  4u
#define PROF_BUCKETS (0x100000u >> PROF_SHIFT)  /* 1 MiB of code from PROF_BASE (the full game's .text is ~600 KiB) */
static uint16_t s_prof_hist[PROF_BUCKETS];
static uint32_t s_prof_samples, s_prof_outside;
static unsigned long s_prof_period;

static void prof_arm(void) { timer_irq_arm(s_prof_period); }

void prof_start(unsigned long period)
{
    for (uint32_t i = 0; i < PROF_BUCKETS; i++) s_prof_hist[i] = 0;
    s_prof_samples = s_prof_outside = 0;
    s_prof_period = period;
    prof_arm();
    timer_irq_enable();
}

void prof_stop(void)
{
    timer_irq_disable();
    s_prof_period = 0;
}

void prof_dump(unsigned topn)
{
    printf("PROF samples=%lu outside=%lu period=%lu\n", (unsigned long)s_prof_samples,
           (unsigned long)s_prof_outside, (unsigned long)s_prof_period);
    for (unsigned k = 0; k < topn; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_BUCKETS; i++)
            if (s_prof_hist[i] > best) { best = s_prof_hist[i]; bi = i; }
        if (!best) break;
        printf("PROF %08lx %lu\n", (unsigned long)(PROF_BASE + (bi << PROF_SHIFT)), (unsigned long)best);
        s_prof_hist[bi] = 0;
    }
    printf("PROF end\n");
}

/* Passive sampling: when the machine timer belongs to someone else (the SM64
 * port's 60 Hz audio tick), every one of its interrupts also records where
 * the CPU was. prof_passive(1/0) turns recording on/off (the histogram
 * accumulates across on periods), prof_passive_samples() counts, and
 * prof_dump_log() prints like prof_dump() but through the non-blocking log,
 * so it can run from the frame loop. */
static int s_prof_passive;
void prof_passive(int on) { s_prof_passive = on; }
void prof_passive_reset(void)
{
    for (uint32_t i = 0; i < PROF_BUCKETS; i++) s_prof_hist[i] = 0;
    s_prof_samples = s_prof_outside = 0;
}
unsigned long prof_passive_samples(void) { return s_prof_samples; }
static void prof_sample(uint32_t epc)
{
    uint32_t off = epc - PROF_BASE;
    if (off < (PROF_BUCKETS << PROF_SHIFT)) {
        uint32_t i = off >> PROF_SHIFT;
        if (s_prof_hist[i] != 0xFFFFu) s_prof_hist[i]++;
    } else {
        s_prof_outside++;
    }
    s_prof_samples++;
}
void prof_dump_log(unsigned topn)
{
    log_printf("PROF samples=%lu outside=%lu (passive)\n", (unsigned long)s_prof_samples,
               (unsigned long)s_prof_outside);
    for (unsigned k = 0; k < topn; k++) {
        uint32_t best = 0, bi = 0;
        for (uint32_t i = 0; i < PROF_BUCKETS; i++)
            if (s_prof_hist[i] > best) { best = s_prof_hist[i]; bi = i; }
        if (!best) break;
        log_printf("PROF %08lx %lu\n", (unsigned long)(PROF_BASE + (bi << PROF_SHIFT)), (unsigned long)best);
        s_prof_hist[bi] = 0;
    }
    log_printf("PROF end\n");
}

/* Periodic machine-timer work (e.g. a 60 Hz audio tick): when set, every
 * timer interrupt goes here and
 * the hook re-arms the timer itself. The interrupt attribute saves all
 * caller-saved integer and FP registers (and fcsr) around the call, so the
 * hook may run float code. */
void (*volatile g_mtimer_hook)(void);

TRAP_HANDLER_ATTR
void trap_handler(void)
{
    uint32_t cause = rd_mcause();
    if (trap_is_timer(cause) && g_mtimer_hook) {
        if (s_prof_passive) prof_sample(rd_mepc());
        g_mtimer_hook();
        return;
    }
    if (trap_is_timer(cause) && s_prof_period) {  /* machine timer: a profiler sample */
        uint32_t off = rd_mepc() - PROF_BASE;
        if (off < (PROF_BUCKETS << PROF_SHIFT)) {
            uint32_t i = off >> PROF_SHIFT;
            if (s_prof_hist[i] != 0xFFFFu) s_prof_hist[i]++;
        } else {
            s_prof_outside++;
        }
        s_prof_samples++;
        prof_arm();
        return;
    }
    if (trap_is_timer(cause)) {                 /* machine timer: the watchdog */
        uint32_t epc = rd_mepc();
        timer_irq_disable();
        while (s_tail != s_head) log_pump();
        char m[65];
        unsigned end = wd_mark_pos;
        for (unsigned i = 0; i < 64; i++) {
            char c = wd_marks[(end + i) & 63u];
            m[i] = c ? c : '.';
        }
        m[64] = 0;
        printf("\n!! WATCHDOG mepc=%08lx bus_errors=%lu marks(oldest->newest)=%s\n",
               (unsigned long)epc, (unsigned long)ctrl_bus_errors_read(), m);
        printf("!! rdp status=%08lx cmd_status=%08lx sync=%lu loads=%lu dropped=%lu mailbox_status=%08lx\n",
               (unsigned long)rdp_status_read(), (unsigned long)rdp_cmd_status_read(),
               (unsigned long)rdp_sync_count_read(), (unsigned long)rdp_load_count_read(),
               (unsigned long)rdp_cmd_dropped_read(), (unsigned long)mailbox_status_read());
        for (;;) { }
    }
    if (trap_is_irq(cause)) {
        trap_mask_stray(cause);
        return;
    }
    uint32_t epc = rd_mepc(), tval = rd_mtval();
    while (s_tail != s_head) log_pump();
    printf("\n!! TRAP mcause=%lu mepc=%08lx mtval=%08lx bus_errors=%lu\n",
           (unsigned long)cause, (unsigned long)epc, (unsigned long)tval,
           (unsigned long)ctrl_bus_errors_read());
    for (;;) { }
}
