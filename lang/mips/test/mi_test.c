/* The MI and the PI's engine in hardware (the SoC built with MIRLO_MON, MIRLO_MI, MIRLO_PI; sim/mips_soc; rtl/soc/rcp_mi.sv).
 *
 * The "game" is C code copied to the N64's RDRAM (0x8000_1000, so mips_core takes it for the game's) with its own
 * exception handler at 0x8000_0180, run with MCTL.GAME and the MI pass-through on. It reaches the MI's registers
 * with plain lw / sw at KSEG1 0xA430_0000 -- no address error: the monitor (exception_entry, here) is entered only
 * for what the hardware does not have, here a store to VI_CURRENT (0xA440_0010), which it answers with MICLR.
 *
 * Checked: MI_VERSION, MI_MODE's bits, MI_MASK's set / clear pairs, MI_INTR's pending bits from a forced source (the
 * runtime's MIRAISE) and from the scan-out's field (VI); MI_MODE's clear-DP; PI_STATUS' interrupt flag and its clear;
 * the line, (INTR & MASK) != 0, as Cause.IP2 -- up with the mask, down in the instruction after the store that
 * clears it -- and the interrupt reaching the game's handler (Cause, MI_INTR and MI_MASK read in it); and, from
 * the monitor's level, the PI engine: a copy of words from the SDRAM to the RDRAM, its busy flag, its
 * interrupt, an abort. The monitor's entries are counted: exactly the VI_CURRENT stores. */
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

#define RES ((volatile uint32_t *)0xA0000400u)       /* the game's results, RDRAM 0x400 (KSEG1) */

/* the monitor: its RAM (0xFFFF_8000) has the game's $k0, $k1 and the counts [2] entries, [3] stores to VI_CURRENT,
 * [4] anything else */
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
"    li    $k0, 5\n"
"    bne   $k1, $k0, 50f\n"                          /* only a store is expected */
"    nop\n"
"    mfc0  $k0, $30\n"                               /* ErrorEPC: the store; its address is rs + imm */
"    lw    $k1, 0($k0)\n"
"    lui   $k0, 0xF000\n"
"    li    $k1, 8\n"                                  /* (any store: VI_CURRENT's) MICLR bit 3: VI */
"    sw    $k1, 0xD1C($k0)\n"
"    lw    $k0, -0x7FF4($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF4($zero)\n"
"    b     60f\n"
"    nop\n"
"50: lw    $k0, -0x7FF0($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF0($zero)\n"
"    sw    $k1, -0x7FEC($zero)\n"                    /* the cause of the last unexpected one */
"60: mfc0  $k0, $30\n"
"    addiu $k0, $k0, 4\n"
"    mtc0  $k0, $30\n"
"    lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* the game's handler, at 0x8000_0180: a console's. It reads Cause, MI_INTR and MI_MASK, clears VI's mask and,
 * with a store to VI_CURRENT (the runtime's), VI's pending bit. */
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
"    lui   $k1, 0xA430\n"
"    lw    $k0, 8($k1)\n"
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x484($k1)\n"                           /* RES[33]: MI_INTR */
"    lui   $k1, 0xA430\n"
"    lw    $k0, 12($k1)\n"
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x488($k1)\n"                           /* RES[34]: MI_MASK */
"    lui   $k1, 0xA430\n"
"    li    $k0, 0x40\n"                                  /* clear VI's mask */
"    sw    $k0, 12($k1)\n"
"    lui   $k1, 0xA440\n"
"    sw    $zero, 0x10($k1)\n"                           /* VI_CURRENT: clears VI's pending bit (the monitor) */
"    lui   $k1, 0xA000\n"
"    lw    $k0, 0x48C($k1)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, 0x48C($k1)\n"                           /* RES[35]: runs */
"    eret\n"
"    nop\n"
"ghandler_end:\n"
);

static inline uint32_t getcause(void) { uint32_t v; __asm__ volatile("mfc0 %0, $13" : "=r"(v)); return v; }

/* the game (section "game": __start_migame .. __stop_migame, copied to the RDRAM) */
__attribute__((section("migame"), noinline, used, optimize("O1"))) void mi_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    volatile uint32_t *mi  = (volatile uint32_t *)0xA4300000u;
    volatile uint32_t *pis = (volatile uint32_t *)0xA4600010u;
    volatile uint32_t *vic = (volatile uint32_t *)0xA4400010u;
    uint32_t n;
    res[0] = mi[1];                                 /* MI_VERSION */
    mi[0] = 0x155; res[1] = mi[0];                  /* count 0x55, set init mode (bit 8) */
    mi[0] = 0x80;  res[2] = mi[0];                  /* clear it, count 0 */
    mi[0] = 0x2000; res[24] = mi[0];                /* MI_SET_RDRAM (bit 13): the upper mode, read back as bit 9 */
    mi[0] = 0x1000; res[25] = mi[0];                /* MI_CLR_RDRAM (bit 12) */
    res[3] = mi[3];                                 /* MI_MASK */
    mi[3] = 0x82;  res[4] = mi[3];                  /* set SP (bit 1) and VI (bit 7): SP, VI */
    mi[3] = 0x01;  res[5] = mi[3];                  /* clear SP */
    mi[3] = 0xAAA; res[6] = mi[3];                  /* set all */
    mi[3] = 0x555; res[7] = mi[3];                  /* clear all */
    res[8] = mi[2];                                 /* MI_INTR: SI AI PI DP (raised by the monitor's level), maybe VI */
    mi[0] = 0x800; res[9] = mi[2];                  /* clear DP */
    res[10] = *pis; *pis = 2; res[11] = *pis; res[12] = mi[2];   /* PI_STATUS: the interrupt flag, its clear */
    mi[3] = 0x80;                                   /* VI's mask on: (INTR & MASK) once a field has come */
    for (n = 0; n < 3000000u && !(mi[2] & 8u); n++) { }
    res[13] = mi[2];                                /* VI pending, from the scan-out's field */
    res[14] = getcause();                           /* IP2 up */
    mi[3] = 0x40;                                   /* clear VI's mask: IP2 down by the next instruction */
    res[15] = getcause();
    mi[3] = 0x80;
    res[16] = getcause();                           /* up again */
    mi[3] = 0x40;
    *vic = 0;                                       /* VI_CURRENT: the monitor clears VI pending */
    res[17] = mi[2];
    res[18] = getcause();
    /* an interrupt for the handler: mask VI on, IM2 and IE; a field to come */
    for (n = 0; n < 3000000u && !(mi[2] & 8u); n++) { }     /* the next field */
    uint32_t st;
    __asm__ volatile("mfc0 %0, $12" : "=r"(st));
    __asm__ volatile("mtc0 %0, $12" :: "r"(st | 0x401u));
    mi[3] = 0x80;
    for (n = 0; n < 3000000u && res[35] == 0; n++) { }      /* the handler ran */
    __asm__ volatile("mtc0 %0, $12" :: "r"(st));
    res[19] = mi[2];
    res[20] = getcause();
}

extern const uint32_t __start_migame[], __stop_migame[];
static void copy_to_rdram(uint32_t phys, const uint32_t *a, const uint32_t *b)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + phys);
    while (a < b) *d++ = *a++;
}

#define MI_SI 2u
#define MI_AI 4u
#define MI_PI 0x10u
#define MI_DP 0x20u

int main(void)
{
    puts_("mi_test\n");
    uint32_t id = monitor_id_read();
    if ((id & 6u) != 6u) { puts_("no MI and PI in this SoC\nmi_test: FAILED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, __start_migame, __stop_migame);
    copy_to_rdram(0x0180u, ghandler_start, ghandler_end);
    for (int i = 0; i < 48; i++) RES[i] = 0;
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0xF);
    monitor_mask_write(0u);                              /* nothing wakes the monitor */
    monitor_ctl_write(5u);                               /* OWN, and IP2 is the MI's line */
    monitor_miclr_write(0x3Eu);
    monitor_miraise_write(MI_SI | MI_AI | MI_PI | MI_DP);
    flush_cpu_dcache();
    /* the pass-through: MCTL bit 9 (the write is meant) and 8 (on), and GAME */
    uint32_t m = 0x302u;
    __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(m));
    __asm__ volatile(".set push\n.set mips3\n mfc0 %0, $7\n.set pop" : "=r"(m));
    check("MCTL bit 8 reads back (the core has the pass-through)", m & 0x100u, 0x100u);
    typedef void (*game_t)(void);
    ((game_t)0x80001000u)();
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x200\n mtc0 $t0, $7\n.set pop" ::: "t0");
    monitor_ctl_write(0u);

    check("MI_VERSION", RES[0], 0x02020102u);
    check("MI_MODE: count and set init", RES[1], 0xD5u);
    check("MI_MODE: cleared", RES[2], 0u);
    check("MI_MODE: bit 13 sets the upper mode (libultra's MI_SET_RDRAM)", RES[24], 0x200u);
    check("MI_MODE: bit 12 clears it (MI_CLR_RDRAM)", RES[25], 0u);
    check("MI_MASK at reset", RES[3], 0u);
    check("MI_MASK: set SP, VI", RES[4], 0x09u);
    check("MI_MASK: clear SP", RES[5], 0x08u);
    check("MI_MASK: set all", RES[6], 0x3Fu);
    check("MI_MASK: clear all", RES[7], 0u);
    check("MI_INTR: the raised ones", RES[8] & 0x37u, MI_SI | MI_AI | MI_PI | MI_DP);
    check("MI_INTR: DP cleared by MI_MODE", RES[9] & 0x37u, MI_SI | MI_AI | MI_PI);
    check("PI_STATUS: the interrupt flag", RES[10], 8u);
    check("PI_STATUS: cleared by bit 1", RES[11], 0u);
    check("MI_INTR: PI cleared with it", RES[12] & 0x37u, MI_SI | MI_AI);
    check("MI_INTR: VI from the field", RES[13] & 8u, 8u);
    check("Cause.IP2 up with VI masked in", (RES[14] >> 10) & 1u, 1u);
    check("Cause.IP2 down at once after the mask's clear", (RES[15] >> 10) & 1u, 0u);
    check("Cause.IP2 up again", (RES[16] >> 10) & 1u, 1u);
    check("MI_INTR: VI cleared by the monitor", RES[17] & 8u, 0u);
    check("Cause.IP2 down", (RES[18] >> 10) & 1u, 0u);
    check("the game's handler ran once", RES[35], 1u);
    check("handler: Cause code 0, an interrupt", (RES[32] >> 2) & 31u, 0u);
    check("handler: Cause.IP2", (RES[32] >> 10) & 1u, 1u);
    check("handler: MI_INTR has VI", RES[33] & 8u, 8u);
    check("handler: MI_MASK has VI", RES[34] & 8u, 8u);
    check("after the handler: VI clear", RES[19] & 8u, 0u);
    check("after the handler: IP2 clear", (RES[20] >> 10) & 1u, 0u);
    /* (with the VI in hardware, MONITOR.ID bit 3, a store to VI_CURRENT is a bus access: the monitor is not entered) */
    check("the monitor's entries: the stores to VI_CURRENT", mr[2], mr[3]);
    check("... the two the game made, or none with the VI in hardware", mr[3], (id & 8u) ? 0u : 2u);
    check("no other", mr[4], 0u);

    /* the PI engine, from here: 1024 words of the SDRAM (0x4020_0000) to the RDRAM's 0x1_0000 */
    volatile uint32_t *src = (volatile uint32_t *)0x40200000u;
    volatile uint32_t *dst = (volatile uint32_t *)0xA0010000u;
    for (uint32_t i = 0; i < 1024u; i++) { src[i] = 0xC0DE0000u + i * 0x10001u; dst[i] = 0; }
    writeback_cpu_dcache_range((const void *)src, 1024u * sizeof(*src));
    dst[-1] = 0; dst[1024] = 0;
    monitor_miclr_write(0x3Eu);
    monitor_pidst_write(0x10000u >> 2);
    monitor_pisrc_write(0x00200000u >> 2);
    timer0_uptime_latch_write(1); uint32_t t0 = timer0_uptime_cycles_read();
    monitor_pilen_write(1024u);
    check("PI busy (PILEN)", monitor_pilen_read() >> 31, 1u);
    check("PI_STATUS busy", *(volatile uint32_t *)0xA4600010u, 1u);
    uint32_t t;
    for (t = 0; t < 2000000u && (monitor_pilen_read() >> 31); t++) { }
    timer0_uptime_latch_write(1); uint32_t t1 = timer0_uptime_cycles_read();
    check("PI done", monitor_pilen_read(), 0u);
    puts_("PI engine: "); puthex(t1 - t0); puts_(" sys cycles for 1024 words\n");
    check("PI interrupt: MI_INTR bit", monitor_miintr_read() & 0x10u, 0x10u);
    check("PI_STATUS: interrupt flag only", *(volatile uint32_t *)0xA4600010u, 8u);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 1024u; i++) if (dst[i] != 0xC0DE0000u + i * 0x10001u) bad++;
    check("the copied words", bad, 0u);
    check("the word before", dst[-1], 0u);
    check("the word after", dst[1024], 0u);
    *(volatile uint32_t *)0xA4600010u = 2;
    check("PI_STATUS cleared", *(volatile uint32_t *)0xA4600010u, 0u);
    monitor_pidst_write(0x20000u >> 2);
    monitor_pilen_write(100000u);
    for (t = 0; t < 400u; t++) { }
    *(volatile uint32_t *)0xA4600010u = 1;                  /* abort */
    for (t = 0; t < 2000000u && (monitor_pilen_read() >> 31); t++) { }
    check("PI aborted: idle", monitor_pilen_read() >> 31, 0u);
    check("... with words left", monitor_pilen_read() != 0u, 1u);
    check("... and no interrupt", monitor_miintr_read() & 0x10u, 0u);
    puts_("entries "); puthex(mr[2]); puts_(", VI_CURRENT stores "); puthex(mr[3]); puts_(", unexpected "); puthex(mr[4]); puts_("\n");
    puts_(fails ? "mi_test: FAILED\n" : "mi_test: PASS\n");
    putc_(4);
    for (;;);
}
