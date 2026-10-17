/* The CPU's side of the RSP's registers (the SoC built with MIRLO_MON, MIRLO_MI; sim/mips_soc), as the runtime's monitor reaches them
 * for a game (0x2000_0000: SP_MEM_ADDR .. SP_PC, the DPC registers at 0x2000_0040): what the CPU can do on the console that the
 * hardware did not take before 2026-10-09.
 *   1  SP_SEMAPHORE: a read returns the bit and takes it (0 1 1), a write clears it
 *   2  the SP DMA queue: a second command written while the first runs is queued (SP_DMA_FULL, STATUS bits 2 and 3), does not
 *      corrupt the first, and runs behind it; both land (read back by DMA out)
 *   3  DPC_START / DPC_END / DPC_STATUS written by the CPU: START is kept while START is valid (a second one is ignored), the
 *      set / clear pairs of XBUS, FREEZE and FLUSH, a list is fetched when unfrozen (CURRENT runs to END), the status is the
 *      console's (0xA8 after a list without SYNC FULL: CBUF_READY | PIPE_BUSY | START_GCLK), SYNC FULL raises the DP interrupt
 *      and clears PIPE_BUSY / START_GCLK
 *   3b a START and END written while a list is under way wait (START_VALID, END_VALID), and a read of START or END is the last
 *      value written -- the waiting pair's -- while CURRENT is the list under way's: a microcode that begins its output at
 *      DPC_END takes the end of what the RDP will run, not of what it runs now */
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
static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }
static void wait_cycles(uint32_t n) { uint32_t t0 = now(); while (now() - t0 < n) { } }

#define SP   ((volatile uint32_t *)0x20000000u)      /* 0 MEM_ADDR 1 DRAM_ADDR 2 RD_LEN 3 WR_LEN 4 STATUS 5 DMA_FULL 6 DMA_BUSY 7 SEMAPHORE */
#define DPC  ((volatile uint32_t *)0x20000040u)      /* 0 START 1 END 2 CURRENT 3 STATUS */
#define MI   ((volatile uint32_t *)0xA4300000u)
#define RAM(phys) ((volatile uint32_t *)(uintptr_t)(0xA0000000u + (phys)))

static void sp_wait(void) { for (uint32_t n = 0; n < 1000000u && SP[6]; n++) { } }

int main(void)
{
    puts_("rcpcpu_test\n");
    monitor_pend_write(0xF);
    monitor_mask_write(0u);
    monitor_ctl_write(0x5u);                             /* OWN, IP2 is the MI's line */
    monitor_miclr_write(0x3Eu);
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x300\n mtc0 $t0, $7\n.set pop" ::: "t0");   /* the monitor's accesses too */

    /* 1: the semaphore */
    SP[7] = 0;
    uint32_t s1 = SP[7], s2 = SP[7], s3 = SP[7];
    check("semaphore reads 0 1 1", s1 * 100 + s2 * 10 + s3, 11u);
    SP[7] = 0;
    uint32_t w1 = SP[7], w2 = SP[7];
    check("... after a write 0 1", w1 * 10 + w2, 1u);
    SP[7] = 0;

    /* 2: the DMA queue */
    for (uint32_t i = 0; i < 64; i++) { RAM(0x310000)[i] = 0xA0000000u + i; RAM(0x320000)[i] = 0xB0000000u + i; RAM(0x330000)[i] = 0; RAM(0x340000)[i] = 0; }
    SP[0] = 0x000; SP[1] = 0x310000; SP[2] = 0xFF;       /* DMEM 0 <- 256 bytes of the RDRAM */
    check("DMA_BUSY right after RD_LEN", SP[6], 1u);
    SP[0] = 0x400; SP[1] = 0x320000; SP[2] = 0xFF;       /* the second, while the first runs */
    uint32_t full = SP[5], st = SP[4];
    sp_wait();
    wait_cycles(2000);
    check("DMA_FULL while one is queued, or both already done", full == 1u || (st & 0xC) == 0, 1u);
    check("DMA_FULL after", SP[5], 0u);
    SP[0] = 0x000; SP[1] = 0x330000; SP[3] = 0xFF; sp_wait();      /* DMEM 0 -> RDRAM (WR_LEN) */
    SP[0] = 0x400; SP[1] = 0x340000; SP[3] = 0xFF; sp_wait();
    uint32_t bad1 = 0, bad2 = 0;
    for (uint32_t i = 0; i < 64; i++) { if (RAM(0x330000)[i] != 0xA0000000u + i) bad1++; if (RAM(0x340000)[i] != 0xB0000000u + i) bad2++; }
    check("the first DMA's words", bad1, 0u);
    check("the second (queued) DMA's words", bad2, 0u);

    /* 3: the DPC from the CPU. A list of two commands in the RDRAM: a NO-OP and a SYNC FULL */
    volatile uint32_t *list = RAM(0x350000);
    list[0] = 0; list[1] = 0;                            /* NO-OP */
    list[2] = 0x29000000u; list[3] = 0;                  /* SYNC FULL */
    const uint32_t A = 0x350000u;
    check("DPC_STATUS at reset: CBUF_READY", DPC[3], 0x80u);
    DPC[3] = 0x2u;  check("set XBUS", DPC[3] & 1u, 1u);
    DPC[3] = 0x1u;  check("clear XBUS", DPC[3] & 1u, 0u);
    DPC[3] = 0x20u; check("set FLUSH", DPC[3] & 4u, 4u);
    DPC[3] = 0x10u; check("clear FLUSH", DPC[3] & 4u, 0u);
    DPC[3] = 0x8u;  check("set FREEZE", DPC[3] & 2u, 2u);
    check("DPC_STATUS frozen", DPC[3], 0x82u);
    DPC[0] = A;
    check("DPC_START written", DPC[0], A);
    check("... START_VALID", DPC[3] & 0x400u, 0x400u);
    DPC[0] = A + 0x100u;
    check("START written while START_VALID is ignored", DPC[0], A);
    DPC[1] = A + 8u;                                     /* the NO-OP only; frozen: not fetched */
    wait_cycles(2000);
    check("END written (frozen): CURRENT = START", DPC[2], A);
    check("... DMA busy (commands between CURRENT and END), no START_VALID, not PIPE_BUSY (frozen)", DPC[3], 0x182u);
    DPC[3] = 0x4u;                                       /* unfreeze */
    for (uint32_t n = 0; n < 200000u && DPC[2] != A + 8u; n++) { }
    check("unfrozen: CURRENT runs to END", DPC[2], A + 8u);
    wait_cycles(2000);
    check("after a list without SYNC FULL: CBUF_READY | PIPE_BUSY | START_GCLK", DPC[3], 0xA8u);
    check("no DP interrupt yet", MI[2] & 0x20u, 0u);
    DPC[1] = A + 16u;                                    /* the SYNC FULL */
    for (uint32_t n = 0; n < 400000u && !(MI[2] & 0x20u); n++) { }
    check("SYNC FULL: the DP interrupt", MI[2] & 0x20u, 0x20u);
    wait_cycles(2000);
    check("... CURRENT at END, PIPE_BUSY and START_GCLK cleared", DPC[3], 0x80u);
    check("... CURRENT", DPC[2], A + 16u);
    MI[0] = 0x800u;
    check("DP interrupt cleared", MI[2] & 0x20u, 0u);

    /* 3b: a pair written while a list is under way waits; the reads are the last values written */
    volatile uint32_t *l2 = RAM(0x351000);
    for (int i = 0; i < 16; i++) l2[i] = 0;              /* eight NO-OPs */
    const uint32_t B = 0x351000u;
    DPC[3] = 0x8u;                                       /* frozen: the first list stays under way */
    DPC[0] = B; DPC[1] = B + 16u;
    wait_cycles(2000);
    check("3b: a list under way (frozen): CURRENT is its START", DPC[2], B);
    DPC[0] = B + 32u; DPC[1] = B + 48u;                  /* a second pair: it waits */
    wait_cycles(2000);
    check("3b: a pair waiting: START_VALID and END_VALID", DPC[3] & 0x600u, 0x600u);
    check("... DPC_START is its START", DPC[0], B + 32u);
    check("... DPC_END is its END, the last written (not the list under way's)", DPC[1], B + 48u);
    check("... DPC_CURRENT is the list under way's", DPC[2], B);
    DPC[1] = B + 56u;                                    /* END again: the pair's END moves */
    wait_cycles(2000);
    check("... another END: DPC_END reads it", DPC[1], B + 56u);
    check("... and the list under way is where it was", DPC[2], B);
    DPC[3] = 0x4u;                                       /* unfreeze: the first list, then the pair */
    for (uint32_t n = 0; n < 400000u && DPC[2] != B + 56u; n++) { }
    check("unfrozen: the pair runs after the list, CURRENT runs to its END", DPC[2], B + 56u);
    check("... DPC_END, no pair waiting", DPC[1], B + 56u);
    check("... START_VALID and END_VALID clear", DPC[3] & 0x600u, 0u);

    puts_(fails ? "rcpcpu_test: FAILED\n" : "rcpcpu_test: PASS\n");
    putc_(4);
    for (;;);
}
