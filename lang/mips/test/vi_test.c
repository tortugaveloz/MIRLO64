/* The VI in hardware (the SoC built with MIRLO_MON, MIRLO_MI, MIRLO_VI; sim/mips_soc; rtl/soc/rcp_vi.sv).
 *
 * The "game" is C code copied to the N64's RDRAM (0x8000_1000, so mips_core takes it for the game's) with its own
 * exception handler at 0x8000_0180, run with MCTL.GAME and the pass-through on (as mi_test), and MONITOR.CTL's VI bit:
 * the VI's registers (0xA440_0000) are real bus accesses, no address error, no entry into the monitor.
 *
 * Checked, phase by phase (the game's function takes the phase in RES[0]):
 *   1  every register reads back what was written (the 13 but CURRENT); CURRENT is a counter; a register the window
 *      is computed from (WIDTH, V_START, Y_SCALE) changing raises MONITOR.PEND's bit 1 (and only then: the same value
 *      again, or BURST, do not)
 *   2  VI_CURRENT: a half-line counter, 0..523, never going back inside a field, wrapping once per field; its bit 0 is
 *      0 without STATUS' SERRATE and alternates with it
 *   3  V_INTR: the interrupt (MI_INTR's VI, Cause.IP2) reaches the game's handler once a field, at the half-line
 *      V_INTR names (the handler reads CURRENT: V_INTR/2 plus the latency), 8 fields in as many field periods
 *   4  a write to VI_CURRENT clears the interrupt (MI_INTR's VI bit, at once), and it comes back the next field
 *   13 the console's lines are all as long: libultra's V_INTR 2 (the line 1) comes a line after CURRENT wraps to 0, not three
 *      (osViInit waits for CURRENT to be 10 or less, and its caller has that long to register for the retrace)
 *   6  the TV system (rtl/soc/n64_video.sv, rcp_vi.sv): the scan-out's field is NTSC (59.94 Hz, 264 lines of the scan-out, the
 *      half-line counter's 262 lines) or PAL (50 Hz, 313 lines, 312). It starts as the CSR says (VIDEO_FRAMEBUFFER_WIDTH's bit
 *      18, the runtime's choice from the ROM's region), and once the game writes V_SYNC with a value that names a system (525:
 *      NTSC, 625: PAL) that is the system; a V_SYNC that is neither (0, noise: phase 1's pattern) changes nothing. Read back,
 *      the WIDTH CSR's bit 19 is the system the scan-out is using. In each: CURRENT's last line, the interrupt's period
 * and, from the monitor's level: the monitor was not entered for a VI access (the entries counted), and the
 * interrupt's period matches the scan-out's field (1,045,440 sys cycles). ORIGIN's effect at the next frame start is
 * the bench's: VITRACE=1 prints the scan-out's base at every latch. */
#include <stdint.h>
#include <generated/csr.h>
#include <generated/soc.h>
#include <system.h>

static void putc_(char c) { while (uart_txfull_read()); uart_rxtx_write((uint8_t)c); }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v) { for (int i = 28; i >= 0; i -= 4) putc_("0123456789abcdef"[(v >> i) & 15]); }
static int fails;
static void check(const char *what, uint32_t got, uint32_t want)
{
    if (got == want) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(want); puts_("\n");
}
static void check_range(const char *what, uint32_t got, uint32_t lo, uint32_t hi)
{
    if (got >= lo && got <= hi) return;
    fails++;
    puts_("FAIL "); puts_(what); puts_(": got "); puthex(got); puts_(" want "); puthex(lo); puts_(".."); puthex(hi); puts_("\n");
}

#define RES ((volatile uint32_t *)0xA0000400u)       /* the game's results, RDRAM 0x400 (KSEG1) */

/* the monitor: its RAM (0xFFFF_8000) counts [2] entries, [4] the unexpected ones; any entry is unexpected here */
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"
"    sw    $k1, -0x7FFC($zero)\n"
"    lw    $k0, -0x7FF8($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF8($zero)\n"                    /* entries */
"    mfc0  $k0, $7\n"
"    srl   $k1, $k0, 3\n"
"    andi  $k1, $k1, 31\n"
"    sw    $k1, -0x7FEC($zero)\n"                    /* the cause of the last */
"    mfc0  $k0, $30\n"
"    addiu $k0, $k0, 4\n"
"    mtc0  $k0, $30\n"
"    lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* the game's handler, at 0x8000_0180: reads Cause, VI_CURRENT and MI_INTR, clears VI's pending bit with a store to
 * VI_CURRENT, counts (RES[35]) and keeps VI_CURRENT (RES[64+n]) and Count (RES[80+n]) of the first 16 */
extern const uint32_t ghandler_start[], ghandler_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .align 2\n"
"ghandler_start:\n"
"    mfc0  $k0, $13\n"
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x480($k1)\n"                           /* RES[32]: Cause */
"    lw    $k0, 0x48C($k1)\n"                           /* n: runs so far */
"    nop\n"
"    andi  $k0, $k0, 15\n"
"    sll   $k0, $k0, 2\n"
"    addu  $k1, $k1, $k0\n"                             /* the ring's slot */
"    mfc0  $k0, $9\n"                                    /* Count */
"    sw    $k0, 0x540($k1)\n"                            /* RES[80 + n] */
"    lui   $k0, 0xA440\n"
"    lw    $k0, 0x10($k0)\n"                             /* VI_CURRENT */
"    nop\n"
"    sw    $k0, 0x500($k1)\n"                            /* RES[64 + n] */
"    lui   $k1, 0xA430\n"
"    lw    $k0, 8($k1)\n"
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x484($k1)\n"                           /* RES[33]: MI_INTR */
"    lui   $k1, 0xA440\n"
"    sw    $zero, 0x10($k1)\n"                           /* VI_CURRENT: clears VI's pending bit (the hardware) */
"    lui   $k1, 0xA000\n"
"    lw    $k0, 0x48C($k1)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, 0x48C($k1)\n"                           /* RES[35]: runs */
"    eret\n"
"    nop\n"
"ghandler_end:\n"
);

static inline uint32_t getcause(void) { uint32_t v; __asm__ volatile("mfc0 %0, $13" : "=r"(v)); return v; }

/* the game's phases (section "game": __start_migame .. __stop_migame, copied to the RDRAM) */
__attribute__((section("migame"), noinline, used, optimize("O1", "no-jump-tables"))) void vi_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    volatile uint32_t *vi  = (volatile uint32_t *)0xA4400000u;
    volatile uint32_t *mi  = (volatile uint32_t *)0xA4300000u;
    uint32_t n, phase = res[0];
    if (phase == 1) {                               /* registers: write the patterns (res[1..]), read back (res[17..]) */
        for (n = 0; n < 14; n++) if (n != 4) vi[n] = 0x01010101u * (n + 1) ^ 0x0F0F00F0u;
        for (n = 0; n < 14; n++) res[17 + n] = vi[n];
    } else if (phase == 2) {                        /* the same again: no change */
        for (n = 0; n < 14; n++) if (n != 4) vi[n] = 0x01010101u * (n + 1) ^ 0x0F0F00F0u;
    } else if (phase == 3) {                        /* BURST (index 5) changed, SERRATE off (STATUS bit 6) */
        vi[5] = 0x12345678u;
        vi[0] = 0x000003u;
    } else if (phase == 4) {                        /* Y_SCALE (index 13) changed */
        vi[13] = 0x00000401u;
    } else if (phase == 5) {                        /* V_START (index 10): the start's bits changed */
        vi[10] = 0x00250200u;
    } else if (phase == 6) {                        /* VI_CURRENT's counter: 250,000 reads, with SERRATE off (res[1] = 0) or on (1) */
        uint32_t prev = vi[4], line, pl = prev >> 1, wraps = 0, badback = 0, mx = 0, bit0 = 0, fchg = 0, pb = prev & 1, pbad = 0;
        const uint32_t reads = res[2] ? res[2] : 250000u;
        vi[0] = res[1] ? 0x40u : 0u;
        for (n = 0; n < reads; n++) {
            uint32_t c = vi[4];
            line = c >> 1;
            if (c & ~0x3FFu) pbad++;
            if (line < pl) { wraps++; if ((c & 1) != pb) fchg++; }
            else if (line > pl + 2u) badback++;                       /* (a jump up inside a field: 3 lines at most from one read to the next) */
            if (line > mx) mx = line;
            bit0 |= c & 1;
            pl = line; pb = c & 1;
        }
        res[2] = wraps; res[3] = badback; res[4] = mx; res[5] = bit0; res[6] = fchg; res[7] = pbad;
    } else if (phase == 7) {                        /* the interrupt: V_INTR = res[1], handler's runs (res[35]) up to res[2] */
        vi[3] = res[1];
        vi[4] = 0;                                  /* (the VI bit has been up since the first field: clear it) */
        mi[3] = 0x80;                               /* VI's mask on */
        uint32_t st;
        __asm__ volatile("mfc0 %0, $12" : "=r"(st));
        __asm__ volatile("mtc0 %0, $12" :: "r"(st | 0x401u));
        for (n = 0; n < 40000000u && res[35] < res[2]; n++) { }
        __asm__ volatile("mtc0 %0, $12" :: "r"(st));
        mi[3] = 0x40;
    } else if (phase == 8) {                        /* a write to VI_CURRENT clears it */
        vi[3] = res[1];
        mi[3] = 0x80;
        for (n = 0; n < 4000000u && !(mi[2] & 8u); n++) { }
        res[1] = mi[2];                             /* VI pending */
        res[2] = getcause();                        /* IP2 up */
        vi[4] = 0;
        res[3] = mi[2];                             /* cleared at once */
        res[4] = getcause();
        for (n = 0; n < 4000000u && !(mi[2] & 8u); n++) { }
        res[5] = mi[2];                             /* and back with the next field */
        vi[4] = 0;
        mi[3] = 0x40;
    } else if (phase == 13) {                       /* V_INTR = res[1] (2): Count from the wrap of CURRENT to the handler's run, res[2] times (res[100 + k]) */
        vi[3] = res[1];
        vi[4] = 0;
        mi[3] = 0x80;
        uint32_t st, k;
        __asm__ volatile("mfc0 %0, $12" : "=r"(st));
        __asm__ volatile("mtc0 %0, $12" :: "r"(st | 0x401u));
        for (k = 0; k < res[2]; k++) {
            uint32_t t0, n0 = res[35];
            while ((vi[4] >> 1) <= 100u) { }                                  /* (past the middle of the field) */
            while ((vi[4] >> 1) > 4u) { }                                     /* the wrap: the line 0 */
            __asm__ volatile("mfc0 %0, $9" : "=r"(t0));
            for (n = 0; n < 4000000u && res[35] == n0; n++) { }
            res[100 + k] = res[80 + (n0 & 15u)] - t0;                         /* (the handler's Count, from this run's slot) */
        }
        __asm__ volatile("mtc0 %0, $12" :: "r"(st));
        mi[3] = 0x40;
    } else if (phase == 12) {                       /* V_SYNC = res[1] */
        vi[6] = res[1];
    } else if (phase == 9) {                        /* ORIGIN at known half-lines: the bench's VITRACE shows what the scan-out takes */
        uint32_t org = 0x00100000u;
        vi[8 / 4] = 320u;                           /* WIDTH: lo-res */
        for (uint32_t k = 0; k < 4; k++) {
            while ((vi[4] >> 1) < 50u || (vi[4] >> 1) > 100u) { }         /* line 50: before the latch (line 18 + 237) */
            vi[1] = org + k * 0x10000u;
            while ((vi[4] >> 1) < 120u || (vi[4] >> 1) > 200u) { }
            vi[1] = org + k * 0x10000u + 0x1000u;                         /* (taken by the latch after it: line 255) */
            while ((vi[4] >> 1) < 265u && (vi[4] >> 1) > 200u) { }
            while ((vi[4] >> 1) > 100u) { }
        }
    } else if (phase == 10) {                       /* a hi-res picture's two fields' origins: the base is the lower */
        uint32_t org = 0x00100000u;
        vi[8 / 4] = 640u;
        for (uint32_t k = 0; k < 3; k++) {
            while ((vi[4] >> 1) < 30u || (vi[4] >> 1) > 60u) { }
            vi[1] = org + k * 0x40000u;                                    /* the first field's */
            while ((vi[4] >> 1) < 130u || (vi[4] >> 1) > 160u) { }
            vi[1] = org + k * 0x40000u + 1280u;                            /* the second's: one row on (640 * 2 bytes) */
            while ((vi[4] >> 1) < 200u || (vi[4] >> 1) > 230u) { }
        }
        vi[8 / 4] = 320u;
    } else if (phase == 11) {                       /* Donkey Kong 64's interlaced hi-res: WIDTH 1280 (two rows' pitch), Y_SCALE 1.0, the fields one 640-pixel row (1280 bytes) apart */
        uint32_t org = 0x00100000u;
        vi[8 / 4] = 1280u;
        for (uint32_t k = 0; k < 3; k++) {
            while ((vi[4] >> 1) < 30u || (vi[4] >> 1) > 60u) { }
            vi[1] = org + k * 0x40000u;                                    /* the first field's */
            while ((vi[4] >> 1) < 130u || (vi[4] >> 1) > 160u) { }
            vi[1] = org + k * 0x40000u + 1280u;                            /* the second's: one row on, which is WIDTH bytes */
            while ((vi[4] >> 1) < 200u || (vi[4] >> 1) > 230u) { }
        }
        vi[8 / 4] = 320u;
    }
}

extern const uint32_t __start_migame[], __stop_migame[];
static void copy_to_rdram(uint32_t phys, const uint32_t *a, const uint32_t *b)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + phys);
    while (a < b) *d++ = *a++;
}

typedef void (*game_t)(void);
static void game(uint32_t phase, uint32_t a1, uint32_t a2)
{
    RES[0] = phase; RES[1] = a1; RES[2] = a2;
    flush_cpu_dcache();
    uint32_t m = 0x302u;
    __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(m));
    ((game_t)0x80001000u)();
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x200\n mtc0 $t0, $7\n.set pop" ::: "t0");
}

#define FIELD 1042710u                               /* (n64_video.sv: 59.940 Hz at 62.5 MHz) */
#define FIELD_PAL 1250000u                           /* (50.000 Hz) */

static void wait_cycles(uint32_t n)
{
    timer0_uptime_latch_write(1); uint32_t t0 = timer0_uptime_cycles_read();
    do { timer0_uptime_latch_write(1); } while (timer0_uptime_cycles_read() - t0 < n);
}
/* the system the scan-out is using (the WIDTH CSR's bit 19), after three fields of the longer kind for a change to have come through */
static uint32_t tv_now(void) { wait_cycles(3u * FIELD_PAL); return (video_framebuffer_width_read() >> 19) & 1u; }
/* the counter and the interrupt's period under the system now in use: pal 0/1 */
static void tv_checks(const char *what, uint32_t pal)
{
    const uint32_t lastlo = pal ? 305u : 255u, lasthi = pal ? 311u : 261u, field = pal ? FIELD_PAL : FIELD;
    puts_("TV system: "); puts_(what); puts_("\n");
    check(what, tv_now(), pal);
    game(6, 0, 150000u);
    check_range("  the counter wraps about once a field", RES[2], 1u, pal ? 8u : 6u);
    check("  the counter never goes back inside a field, nor jumps", RES[3], 0u);
    check_range("  the counter reaches the last line of the system's field", RES[4], lastlo, lasthi);
    if (RES[4] > 261u && !pal) check("  (NTSC's counter stays under 262 lines)", RES[4], 261u);
    for (int i = 32; i < 128; i++) RES[i] = 0;
    timer0_uptime_latch_write(1); uint32_t t0 = timer0_uptime_cycles_read();
    game(7, 200u, 5);
    timer0_uptime_latch_write(1); uint32_t t1 = timer0_uptime_cycles_read();
    check("  the handler ran 5 times", RES[35], 5u);
    uint32_t dt = t1 - t0, d0 = RES[81] - RES[80], bad = 0;
    puts_("  5 interrupts in "); puthex(dt); puts_(" sys cycles\n");
    check_range("  the period: the first's wait (less than a field) and 4 fields", dt, 4u * field - field / 20u, 5u * field + field / 20u);
    for (uint32_t n = 1; n < 4; n++) { uint32_t d = RES[81 + n] - RES[80 + n]; if (d > d0 + d0 / 50u || d + d0 / 50u < d0) bad++; }
    check("  the runs are one field apart, every time", bad, 0u);
    /* Count's delta between two runs is one field in the CPU's ticks: PAL's is 1,250,000 / 1,042,710 = 1.1988 times NTSC's */
    static uint32_t cnt_ntsc;
    if (!pal && !cnt_ntsc) cnt_ntsc = d0;
    puts_("  Count between two runs "); puthex(d0); puts_("\n");
    if (cnt_ntsc) check_range("  ... in proportion to the field (per mille of NTSC's)", d0 * 1000u / (cnt_ntsc / 1u), pal ? 1187u : 990u, pal ? 1211u : 1010u);
}

int main(void)
{
    puts_("vi_test\n");
    uint32_t id = monitor_id_read();
    if ((id & 0xAu) != 0xAu) { puts_("no VI in this SoC\nvi_test: SKIPPED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, __start_migame, __stop_migame);
    copy_to_rdram(0x0180u, ghandler_start, ghandler_end);
    for (int i = 0; i < 128; i++) RES[i] = 0;
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0xF);
    monitor_mask_write(0u);                              /* nothing wakes the monitor */
    monitor_ctl_write(0xDu);                             /* OWN, IP2 is the MI's line, and the VI is the hardware's */
    monitor_miclr_write(0x3Eu);
    check("MONITOR.CTL reads back OWN, MI, VI", monitor_ctl_read(), 0xDu);

    /* 1: the registers */
    game(1, 0, 0);
    for (uint32_t n = 0; n < 14; n++) {
        uint32_t want = n == 4 ? 0u : (0x01010101u * (n + 1) ^ 0x0F0F00F0u);
        if (n == 4) continue;
        check("register reads back", RES[17 + n], want);
        if (RES[17 + n] != want) { puthex(n); puts_("\n"); }
    }
    check("PEND bit 1: the window's registers changed", monitor_pend_read() & 2u, 2u);
    monitor_pend_write(0xF);
    game(2, 0, 0);
    check("PEND bit 1: the same values again, no event", monitor_pend_read() & 2u, 0u);
    game(3, 0, 0);
    check("PEND bit 1: BURST and STATUS are not the window's", monitor_pend_read() & 2u, 0u);
    game(4, 0, 0);
    check("PEND bit 1: Y_SCALE changed", monitor_pend_read() & 2u, 2u);
    monitor_pend_write(0xF);
    game(5, 0, 0);
    check("PEND bit 1: V_START changed", monitor_pend_read() & 2u, 2u);
    monitor_pend_write(0xF);

    /* 2: the counter */
    for (uint32_t serr = 0; serr < 2; serr++) {
        game(6, serr, 0);
        puts_("counter, serrate "); puthex(serr); puts_(": wraps "); puthex(RES[2]); puts_(" max line "); puthex(RES[4]); puts_(" field changes "); puthex(RES[6]); puts_("\n");
        check_range("the counter wraps about once a field", RES[2], 1u, 6u);
        check("the counter never goes back inside a field, nor jumps", RES[3], 0u);
        check_range("the counter reaches the last line", RES[4], 255u, 261u);
        check("no bit above bit 9", RES[7], 0u);
        check("the field bit", RES[5], serr);
        if (serr) check("the field bit changes at every wrap", RES[6], RES[2]);
        else check("... and not without SERRATE", RES[6], 0u);
    }

    /* 3: the interrupt at V_INTR */
    for (int i = 32; i < 128; i++) RES[i] = 0;
    {
        const uint32_t vintr = 200u;                      /* the line 100 */
        timer0_uptime_latch_write(1); uint32_t t0 = timer0_uptime_cycles_read();
        game(7, vintr, 8);
        timer0_uptime_latch_write(1); uint32_t t1 = timer0_uptime_cycles_read();
        check("the handler ran 8 times", RES[35], 8u);
        check("handler: Cause code 0, an interrupt", (RES[32] >> 2) & 31u, 0u);
        check("handler: Cause.IP2", (RES[32] >> 10) & 1u, 1u);
        check("handler: MI_INTR has VI", RES[33] & 8u, 8u);
        uint32_t worst = 0, best = ~0u;
        for (uint32_t n = 0; n < 8; n++) {
            uint32_t c = RES[64 + n] >> 1;
            if (c > worst) worst = c;
            if (c < best) best = c;
        }
        puts_("interrupt at lines "); puthex(best); puts_(".."); puthex(worst); puts_(" (V_INTR names "); puthex(vintr >> 1); puts_(")\n");
        check_range("the handler's VI_CURRENT: the line V_INTR names, plus the latency", best, vintr >> 1, (vintr >> 1) + 3u);
        check_range("... for every field", worst, vintr >> 1, (vintr >> 1) + 3u);
        uint32_t dt = t1 - t0;
        puts_("8 interrupts in "); puthex(dt); puts_(" sys cycles\n");
        check_range("the period: the first's wait (less than a field) and 7 fields", dt, 7u * FIELD - FIELD / 20u, 8u * FIELD + FIELD / 20u);
        /* Count (half the CPU's clock) between the handler's runs: the same period every time */
        uint32_t d0 = RES[81] - RES[80], bad = 0;
        for (uint32_t n = 1; n < 7; n++) { uint32_t d = RES[81 + n] - RES[80 + n]; if (d > d0 + d0 / 50u || d + d0 / 50u < d0) bad++; }
        check("the runs are one field apart, every time (Count's deltas within 2 %)", bad, 0u);
    }

    /* 4: a write to VI_CURRENT clears it */
    for (int i = 0; i < 8; i++) RES[i] = 0;
    game(8, 120u, 0);
    check("VI pending (polled)", RES[1] & 8u, 8u);
    check("Cause.IP2 up", (RES[2] >> 10) & 1u, 1u);
    check("VI cleared by the store to VI_CURRENT, at once", RES[3] & 8u, 0u);
    check("Cause.IP2 down", (RES[4] >> 10) & 1u, 0u);
    check("VI back with the next field", RES[5] & 8u, 8u);

    /* 4b: V_INTR 2, the line 1, comes a line (63.6 us, 2979 Count ticks) after the counter wraps to the line 0 */
    for (int i = 32; i < 128; i++) RES[i] = 0;
    game(13, 2u, 6u);
    {
        uint32_t lo = ~0u, hi = 0;
        for (uint32_t k = 0; k < 6; k++) { uint32_t d = RES[100 + k]; if (d < lo) lo = d; if (d > hi) hi = d; }
        puts_("the wrap to the line 1's interrupt: Count ticks "); puthex(lo); puts_(".."); puthex(hi); puts_("\n");
        check_range("V_INTR 2: a line after the wrap (one line is 2979 ticks), not three", lo, 2400u, 3600u);
        check_range("... every time", hi, 2400u, 3600u);
    }

    /* 5: ORIGIN and a hi-res pair, for the bench's VITRACE (no self-check: the scan-out's base is not a register) */
    game(9, 0, 0);
    game(10, 0, 0);
    game(11, 0, 0);

    /* 6: the TV system. Phase 1 wrote noise to V_SYNC (0x080807F7 & 0x3FF = 1015: not a system): the CSR's choice is in use. */
    for (int i = 0; i < 8; i++) RES[i] = 0;
    tv_checks("the CSR says NTSC (reset), V_SYNC never written", 0);
    video_framebuffer_width_write(1u << 18);
    check("WIDTH CSR reads back the choice (bit 18)", (video_framebuffer_width_read() >> 18) & 1u, 1u);
    tv_checks("the CSR says PAL", 1);
    game(12, 0u, 0);                                      /* V_SYNC 0: no system, as it was */
    tv_checks("V_SYNC 0 changes nothing", 1);
    game(12, 0x20Du, 0);                                  /* NTSC's 525 over the CSR's PAL */
    tv_checks("V_SYNC 525 over the CSR's PAL", 0);
    video_framebuffer_width_write(0);
    game(12, 0x271u, 0);                                  /* PAL's 625 over the CSR's NTSC */
    tv_checks("V_SYNC 625 over the CSR's NTSC", 1);
    game(12, 0x3F7u, 0);                                  /* noise */
    tv_checks("V_SYNC noise changes nothing", 1);
    game(12, 0x20Du, 0);
    tv_checks("V_SYNC 525 again", 0);

    puts_("entries "); puthex(mr[2]); puts_(" (the monitor was not to be entered by the VI), last cause "); puthex(mr[4]); puts_("\n");
    check("no entry into the monitor for a VI access", mr[2], 0u);
    monitor_ctl_write(0u);
    puts_(fails ? "vi_test: FAILED\n" : "vi_test: PASS\n");
    putc_(4);
    for (;;);
}
