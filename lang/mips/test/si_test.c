/* The SI and the PIF RAM in hardware (the SoC built with MIRLO_MON, MIRLO_MI, MIRLO_SI; sim/mips_soc; rtl/soc/rcp_si.sv).
 *
 * The "game" is C code copied to the N64's RDRAM (0x8000_1000, so mips_core takes it for the game's) with its own
 * exception handler at 0x8000_0180, run with MCTL.GAME and the pass-through on (as mi_test, vi_test, ai_test): the SI's
 * registers (0xA480_0000) and PIF RAM (0xBFC0_07C0) are real bus accesses, no address error, no entry into the
 * monitor. The monitor's level (main) reaches them as plain accesses too, and plays the runtime: it answers PEND's
 * bit 4 (the PIF has commands to run) by writing the replies into the PIF RAM and saying it is done (MONITOR.SIDONE).
 *
 * Checked, phase by phase:
 *   1  PIF RAM is where the boot ROM's last 64 bytes of its first 2 KiB are: with the pass-through off (MCTL bit 8) the boot
 *      ROM is there, with it on the PIF RAM; the rest of the boot ROM is untouched
 *   2  the registers: reset values, DRAM_ADDR's 8-byte alignment and read-back, the others read 0, the registers repeat
 *      every 0x20 bytes; PIF RAM's 16 words by KSEG1 and KSEG0, word 15's low byte (the PIF's status byte) written 0 but for bit 1
 *   3  WR64B (RDRAM -> PIF RAM), no run flag: STATUS busy while it runs, the PIF RAM right, the interrupt (STATUS bit 12,
 *      MI_INTR's SI bit) at its end, cleared by a write to STATUS; no wake-up of the monitor (PEND bit 4)
 *   4  RD64B (PIF RAM -> RDRAM) at an address that is 8 bytes aligned only: the 64 bytes and not a byte more
 *   5  a start while a DMA is under way is ignored and sets the error bit (cleared by a write to STATUS)
 *   6  the PIF: a WR64B with the run flag sets IO busy and PEND bit 4 at its end (the flag is taken: byte 63 reads 0);
 *      an RD64B then is deferred (DMA busy, nothing moved) until SIDONE; the next RD64B runs the commands again (the
 *      run flag stays: PEND bit 4 at the request), except the first after the run a WR64B made (its replies are there);
 *      a WR64B waits for the PIF too; the challenge's bit (bit 1 of byte 63) is kept and has the PIF answer at once, at the
 *      end of a WR64B or at a CPU write of the status byte
 *   7  the CPU's access to the PIF RAM while the DMA runs stalls until it is done and reads the data (the old word before)
 *   8  the game's own loads and stores to the registers and PIF RAM (no entry into the monitor), and the interrupt
 *      reaching its handler (Cause.IP2, MI_INTR, SI_STATUS, cleared by a write to SI_STATUS at once)
 *   9  the PI's engine copying 4096 words (and the AI reading, when there is one) while the SI moves blocks: every word right
 *  10  the console's time: a WR64B ends some 4,065 cycles after its start; SIDONE's value is how long the read of the PIF's
 *      commands takes, counted from SIDONE -- for the read after the run a WR64B made, and for a read that runs them again
 * The blocks hold a pseudo-random pattern (lcg). */
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
#define SI  ((volatile uint32_t *)0xA4800000u)       /* 0 DRAM_ADDR, 1 RD64B, 4 WR64B, 6 STATUS */
#define MI  ((volatile uint32_t *)0xA4300000u)
#define PIF ((volatile uint32_t *)0xBFC007C0u)       /* PIF RAM, KSEG1 */
#define PIF0 ((volatile uint32_t *)0x9FC007C0u)      /* ... KSEG0 */
#define BOOT ((volatile uint32_t *)0xBFC00000u)
#define RD(phys) ((volatile uint32_t *)(uintptr_t)(0xA0000000u + (phys)))
#define ST_BUSY 1u
#define ST_IO   2u
#define ST_ERR  8u
#define ST_IRQ  0x1000u

/* the monitor: its RAM (0xFFFF_8000) counts [2] entries; any entry is unexpected here */
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

/* the game's handler, at 0x8000_0180: Cause (RES[32]), MI_INTR (RES[33]); with n the runs so far (RES[35]): SI_STATUS as the
 * handler found it (RES[64+n]), after a write to it (RES[80+n]), MI_INTR after (RES[96+n]), Cause after (RES[112+n]) */
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
"    lui   $k0, 0xA430\n"
"    lw    $k0, 8($k0)\n"
"    nop\n"
"    sw    $k0, 0x484($k1)\n"                           /* RES[33]: MI_INTR */
"    lw    $k0, 0x48C($k1)\n"                           /* n: runs so far */
"    nop\n"
"    andi  $k0, $k0, 15\n"
"    sll   $k0, $k0, 2\n"
"    addu  $k1, $k1, $k0\n"                             /* the ring's slot */
"    lui   $k0, 0xA480\n"
"    lw    $k0, 0x18($k0)\n"                             /* SI_STATUS */
"    nop\n"
"    sw    $k0, 0x500($k1)\n"                            /* RES[64 + n] */
"    lui   $k0, 0xA480\n"
"    sw    $zero, 0x18($k0)\n"                           /* SI_STATUS: clears the interrupt (the hardware) */
"    lui   $k0, 0xA480\n"
"    lw    $k0, 0x18($k0)\n"
"    nop\n"
"    sw    $k0, 0x540($k1)\n"                            /* RES[80 + n]: SI_STATUS after the clear */
"    lui   $k0, 0xA430\n"
"    lw    $k0, 8($k0)\n"
"    nop\n"
"    sw    $k0, 0x580($k1)\n"                            /* RES[96 + n]: MI_INTR after the clear */
"    mfc0  $k0, $13\n"
"    nop\n"
"    sw    $k0, 0x5C0($k1)\n"                            /* RES[112 + n]: Cause after the clear */
"    lui   $k1, 0xA000\n"
"    lw    $k0, 0x48C($k1)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, 0x48C($k1)\n"                           /* RES[35]: runs */
"    eret\n"
"    nop\n"
"ghandler_end:\n"
);

/* the game's phases (section "migame": __start_migame .. __stop_migame, copied to the RDRAM) */
__attribute__((section("migame"), noinline, used, optimize("O1", "no-jump-tables"))) void si_game(void)
{
    volatile uint32_t *res = (volatile uint32_t *)0xA0000400u;
    volatile uint32_t *si  = (volatile uint32_t *)0xA4800000u;
    volatile uint32_t *mi  = (volatile uint32_t *)0xA4300000u;
    volatile uint32_t *pk1 = (volatile uint32_t *)0xBFC007C0u;
    volatile uint32_t *pk0 = (volatile uint32_t *)0x9FC007C0u;
    uint32_t n, phase = res[0];
    if (phase == 1) {                               /* the game's own accesses: PIF RAM by both aliases, the registers */
        pk1[2] = 0x13572468u;
        res[8]  = pk1[2];
        res[9]  = pk0[2];
        pk0[3] = 0x2468ACE0u;
        res[10] = pk1[3];
        pk1[15] = 0x0A0B0C0Du;                      /* byte 63 written 0: and no run flag in 0x0D? bit 0 is set: the PIF's flag */
        res[11] = pk0[15];
        si[0] = 0x00345678u;
        res[12] = si[0];
        res[13] = si[6];
    } else if (phase == 2) {                        /* WR64B, RES[1] the block's RDRAM address; the interrupt on: wait for one */
        uint32_t st;
        mi[3] = 0x8;                                /* SI's mask on */
        si[0] = res[1];
        si[4] = 0x1FC007C0u;
        __asm__ volatile("mfc0 %0, $12" : "=r"(st));
        __asm__ volatile("mtc0 %0, $12" :: "r"(st | 0x401u));
        for (n = 0; n < 4000000u && res[35] < 1; n++) { }
        __asm__ volatile("mtc0 %0, $12" :: "r"(st));
        mi[3] = 0x2;
    }
}

extern const uint32_t __start_migame[], __stop_migame[];
static void copy_to_rdram(uint32_t phys, const uint32_t *a, const uint32_t *b)
{
    volatile uint32_t *d = RD(phys);
    while (a < b) *d++ = *a++;
}

typedef void (*game_t)(void);
static void game(uint32_t phase, uint32_t a1)
{
    RES[0] = phase; RES[1] = a1;
    flush_cpu_dcache();
    uint32_t m = 0x302u;
    __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(m));
    ((game_t)0x80001000u)();
    __asm__ volatile(".set push\n.set mips3\n li $t0, 0x300\n mtc0 $t0, $7\n.set pop" ::: "t0");
}
static void mctl(uint32_t v) { __asm__ volatile(".set push\n.set mips3\n mtc0 %0, $7\n.set pop" :: "r"(v)); }

static uint32_t now(void) { timer0_uptime_latch_write(1); return (uint32_t)timer0_uptime_cycles_read(); }
static void wait_cycles(uint32_t n) { uint32_t t0 = now(); while (now() - t0 < n) { } }

static uint32_t lcg_s;
static uint32_t lcg(void) { lcg_s = lcg_s * 1664525u + 1013904223u; return lcg_s ^ (lcg_s >> 15); }
/* a block of 16 words at the RDRAM's phys: pseudo-random, word 15 low byte as given (0: no run flag) */
static void fill_blk(uint32_t phys, uint32_t w15_low)
{
    volatile uint32_t *d = RD(phys);
    for (int i = 0; i < 16; i++) d[i] = lcg();
    d[15] = (d[15] & ~0xFFu) | w15_low;
}
static void sentinel(uint32_t phys, uint32_t n, uint32_t v) { volatile uint32_t *d = RD(phys); for (uint32_t i = 0; i < n; i++) d[i] = v + i; }
static uint32_t si_idle(uint32_t max) { uint32_t n; for (n = 0; n < max && (SI[6] & ST_BUSY); n++) { } return n; }
/* an RD64B once a block with the run flag has been written: the PIF runs the commands before the read -- the monitor's part */
static void rd64_run(uint32_t dram)
{
    SI[0] = dram; SI[1] = 0;
    for (uint32_t k = 0; k < 100000u && !(monitor_pend_read() & 0x10u); k++) { }
    monitor_pend_write(0x10u);
    monitor_sidone_write(1);
}
static uint32_t cmp_blk(uint32_t phys, uint32_t pif_expect_w15_clear)
{
    volatile uint32_t *d = RD(phys); uint32_t bad = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t w = d[i];
        if (i == 15 && pif_expect_w15_clear) w &= ~0xFFu;
        if (PIF[i] != w) bad++;
    }
    return bad;
}

#define A1 0x00200000u
#define A2 0x00201000u
#define A3 0x00202048u                               /* 8-byte aligned only */
#define A4 0x00203000u

int main(void)
{
    puts_("si_test\n");
    uint32_t id = monitor_id_read();
    if ((id & 0x22u) != 0x22u) { puts_("no SI in this SoC\nsi_test: SKIPPED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, __start_migame, __stop_migame);
    copy_to_rdram(0x0180u, ghandler_start, ghandler_end);
    for (int i = 0; i < 128; i++) RES[i] = 0;
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0x1F);
    monitor_mask_write(0u);                              /* nothing wakes the monitor */
    monitor_ctl_write(5u);                               /* OWN, IP2 is the MI's line */
    monitor_miclr_write(0x3Eu);

    /* 1: the boot ROM and the PIF RAM share 0x1FC0_07C0..07FF */
    mctl(0x200u);
    uint32_t b0 = BOOT[0x7C0 / 4], b1 = BOOT[0x7FC / 4], bm = BOOT[2], bp = BOOT[0x7BC / 4], bq = BOOT[0x800 / 4];
    check("the boot ROM is there with the pass-through off", b0 != 0u || b1 != 0u || bm != 0u, 1u);
    mctl(0x300u);
    PIF[0] = ~b0; PIF[15] = 0xFFFFFF00u;                 /* (no run flag: bit 0 of byte 63) */
    check("pass-through on: PIF RAM word 0, not the boot ROM's", PIF[0], ~b0);
    check("... word 15", PIF[15], 0xFFFFFF00u);
    check("the boot ROM below it is untouched", BOOT[0x7BC / 4], bp);
    check("the boot ROM above it is untouched", BOOT[0x800 / 4], bq);
    check("the boot ROM's mark is untouched", BOOT[2], bm);
    mctl(0x200u);
    check("pass-through off: the boot ROM again (word 0)", BOOT[0x7C0 / 4], b0);
    check("... (word 15)", BOOT[0x7FC / 4], b1);
    mctl(0x300u);
    check("pass-through on again: PIF RAM kept its word 0", PIF[0], ~b0);

    /* 2: the registers, PIF RAM by hand */
    lcg_s = 1;
    check("reset: STATUS 0", SI[6], 0u);
    check("reset: DRAM_ADDR 0", SI[0], 0u);
    SI[0] = 0x00ABCDEFu;
    check("DRAM_ADDR: 8-byte aligned, read back", SI[0], 0x00ABCDE8u);
    SI[0] = 0xFFFFFFFFu;
    check("DRAM_ADDR: 24 bits", SI[0], 0x00FFFFF8u);
    check("RD64B reads 0", SI[1], 0u);
    check("WR64B reads 0", SI[4], 0u);
    check("0x08 reads 0", SI[2], 0u);
    check("0x0C reads 0", SI[3], 0u);
    check("0x14 reads 0", SI[5], 0u);
    check("the registers repeat: 0x20 is DRAM_ADDR", SI[8], 0x00FFFFF8u);
    check("the registers repeat: 0x38 is STATUS", SI[14], 0u);
    for (int i = 0; i < 16; i++) PIF[i] = 0x01010101u * (uint32_t)(i + 1);
    {
        uint32_t bad = 0;
        for (int i = 0; i < 15; i++) if (PIF[i] != 0x01010101u * (uint32_t)(i + 1) || PIF0[i] != PIF[i]) bad++;
        check("PIF RAM words 0..14 read back, KSEG1 and KSEG0", bad, 0u);
        check("PIF RAM word 15: byte 63 reads 0 (the PIF's status byte)", PIF[15], 0x10101000u);
        check("... by KSEG0", PIF0[15], 0x10101000u);
    }
    check("(no run flag, no wake-up of the monitor)", monitor_pend_read() & 0x10u, 0u);

    /* 3: WR64B */
    fill_blk(A1, 0u);
    SI[0] = A1;
    SI[4] = 0x1FC007C0u;
    check("WR64B: STATUS busy at once", SI[6] & ST_BUSY, ST_BUSY);
    check("... and no interrupt yet", SI[6] & ST_IRQ, 0u);
    check("MI_INTR has no SI yet", MI[2] & 2u, 0u);
    uint32_t n = si_idle(1000000u);
    puts_("WR64B took "); puthex(n); puts_(" polls\n");
    check("WR64B: STATUS idle, the interrupt up (bit 12)", SI[6], ST_IRQ);
    check("WR64B: MI_INTR has SI", MI[2] & 2u, 2u);
    check("WR64B: the PIF RAM is the block", cmp_blk(A1, 1u), 0u);
    check("WR64B: no run flag, no PEND bit 4", monitor_pend_read() & 0x10u, 0u);
    SI[6] = 0;
    check("a write to STATUS clears the interrupt", SI[6], 0u);
    check("... and MI_INTR's SI bit", MI[2] & 2u, 0u);

    /* 4: RD64B at an 8-byte aligned address */
    sentinel(A3 - 8, 2, 0xAA000000u);
    sentinel(A3 + 64, 2, 0xBB000000u);
    SI[0] = A3;
    SI[1] = 0x1FC007C0u;
    check("RD64B: STATUS busy at once", SI[6] & ST_BUSY, ST_BUSY);
    si_idle(1000000u);
    check("RD64B: STATUS idle, the interrupt up", SI[6], ST_IRQ);
    check("RD64B: MI_INTR has SI", MI[2] & 2u, 2u);
    check("RD64B: the RDRAM has the 64 bytes", cmp_blk(A3, 0u), 0u);
    check("RD64B: the words before are untouched", RD(A3 - 8)[0] == 0xAA000000u && RD(A3 - 8)[1] == 0xAA000001u, 1u);
    check("RD64B: the words after are untouched", RD(A3 + 64)[0] == 0xBB000000u && RD(A3 + 64)[1] == 0xBB000001u, 1u);
    check("RD64B: no run flag, no PEND bit 4", monitor_pend_read() & 0x10u, 0u);
    SI[6] = 0;

    /* 5: a start while a DMA is under way */
    fill_blk(A2, 0u);
    sentinel(A4, 16, 0xCC000000u);
    SI[0] = A2; SI[4] = 0;
    SI[0] = A4; SI[1] = 0;                               /* (DRAM_ADDR is the first's while it runs: the write is ignored) */
    check("a start while busy: the error bit", SI[6] & ST_ERR, ST_ERR);
    si_idle(1000000u);
    check("... the first DMA went through (the PIF RAM is the block)", cmp_blk(A2, 1u), 0u);
    check("... DRAM_ADDR was the first's (and is 64 bytes on, the DMA counts it up)", SI[0], A2 + 64u);
    check("... and the second was ignored", RD(A4)[0] == 0xCC000000u && RD(A4)[15] == 0xCC00000Fu, 1u);
    check("STATUS: idle, error and interrupt", SI[6], ST_ERR | ST_IRQ);
    SI[6] = 0;
    check("a write to STATUS clears both", SI[6], 0u);

    /* 6: the PIF: the run flag */
    monitor_pend_write(0x1F);
    fill_blk(A1, 1u);                                    /* the flag */
    sentinel(A4, 16, 0xDD000000u);
    SI[0] = A1; SI[4] = 0;
    si_idle(1000000u);
    check("WR64B with the run flag: DMA idle, IO busy, the interrupt", SI[6], ST_IO | ST_IRQ);
    check("... PEND bit 4", monitor_pend_read() & 0x10u, 0x10u);
    check("... byte 63 reads 0 (the flag is taken)", PIF[15] & 0xFFu, 0u);
    SI[6] = 0;
    check("STATUS write clears the interrupt, not IO busy", SI[6], ST_IO);
    SI[0] = A4; SI[1] = 0;                               /* RD64B: deferred */
    wait_cycles(5000);
    check("RD64B with the PIF busy: DMA busy (deferred), IO busy", SI[6], ST_BUSY | ST_IO);
    check("... nothing moved", RD(A4)[0] == 0xDD000000u && RD(A4)[15] == 0xDD00000Fu, 1u);
    check("... and no interrupt", MI[2] & 2u, 0u);
    PIF[3] = 0xCAFE0003u;                                /* the reply */
    monitor_sidone_write(1);
    si_idle(1000000u);
    check("SIDONE: the read went through, IO busy clear, the interrupt", SI[6], ST_IRQ);
    check("... the RDRAM has the block with the reply", cmp_blk(A4, 0u), 0u);
    check("... the reply is in it", RD(A4)[3], 0xCAFE0003u);
    SI[6] = 0;
    monitor_pend_write(0x1F);
    SI[0] = A2; SI[1] = 0;                               /* another RD64B: the commands run again */
    wait_cycles(5000);
    check("RD64B again: the run flag stayed -- PEND bit 4 at the request", monitor_pend_read() & 0x10u, 0x10u);
    check("... deferred: DMA busy, IO busy", SI[6], ST_BUSY | ST_IO);
    monitor_sidone_write(1);
    si_idle(1000000u);
    check("... done", SI[6], ST_IRQ);
    check("... the block is the PIF RAM", cmp_blk(A2, 0u), 0u);
    SI[6] = 0;
    /* the first read after the run a WR64B made: its replies are there, the PIF does not run again; the next read does */
    monitor_pend_write(0x1F);
    fill_blk(A1, 1u);
    SI[0] = A1; SI[4] = 0;
    si_idle(1000000u);
    SI[6] = 0;
    check("WR64B with the run flag: PEND bit 4 (the run)", monitor_pend_read() & 0x10u, 0x10u);
    monitor_pend_write(0x10u);
    PIF[3] = 0xFACE0003u;                                /* the run's reply */
    monitor_sidone_write(1);
    sentinel(A4, 16, 0x11000000u);
    SI[0] = A4; SI[1] = 0;
    si_idle(1000000u);
    check("the first read after the run: done at once (the interrupt), IO busy clear", SI[6], ST_IRQ);
    check("... no second run: PEND bit 4 stays down", monitor_pend_read() & 0x10u, 0u);
    check("... the block is the PIF RAM, with the reply", cmp_blk(A4, 0u) == 0u && RD(A4)[3] == 0xFACE0003u, 1u);
    SI[6] = 0;
    SI[0] = A2; SI[1] = 0;
    wait_cycles(5000);
    check("the next read: the commands run again (PEND bit 4), deferred", monitor_pend_read() & 0x10u, 0x10u);
    check("... DMA busy, IO busy", SI[6], ST_BUSY | ST_IO);
    monitor_pend_write(0x10u);
    monitor_sidone_write(1);
    si_idle(1000000u);
    SI[6] = 0;
    /* a WR64B while the PIF is busy waits for it: RD64B (joy) makes it busy, the WR64B behind a finished RD cannot be started
     * while the DMA is under way (error), so make the PIF busy with a WR64B that has the flag, and start another behind it */
    monitor_pend_write(0x1F);
    fill_blk(A1, 1u);
    SI[0] = A1; SI[4] = 0;
    si_idle(1000000u);
    SI[6] = 0;
    fill_blk(A2, 0u);
    uint32_t before = PIF[5];
    SI[0] = A2; SI[4] = 0;                               /* waits */
    wait_cycles(5000);
    check("WR64B with the PIF busy: DMA busy, IO busy", SI[6], ST_BUSY | ST_IO);
    check("... the PIF RAM is as it was", PIF[5], before);
    monitor_sidone_write(1);
    si_idle(1000000u);
    check("... after SIDONE it went through", cmp_blk(A2, 1u), 0u);
    check("... IO busy clear, the interrupt", SI[6], ST_IRQ);
    SI[6] = 0;
    /* the challenge's bit (bit 1 of byte 63): the status byte keeps it, and the PIF answers at the end of the WR64B (IO busy,
     * PEND bit 4); the read after the answer does not run the commands; a CPU write of the status byte with it does the same */
    monitor_pend_write(0x1F);
    fill_blk(A1, 2u);
    SI[0] = A1; SI[4] = 0;
    si_idle(1000000u);
    check("WR64B with the challenge's bit: DMA idle, IO busy, the interrupt", SI[6], ST_IO | ST_IRQ);
    check("... PEND bit 4", monitor_pend_read() & 0x10u, 0x10u);
    check("... byte 63 keeps the bit", PIF[15] & 0xFFu, 2u);
    SI[6] = 0;
    monitor_pend_write(0x10u);
    PIF[12] = 0x136AF74Cu; PIF[15] = RD(A1)[15] & ~0xFFu;          /* the answer, the bit cleared */
    monitor_sidone_write(1);
    sentinel(A4, 16, 0x22000000u);
    SI[0] = A4; SI[1] = 0;
    si_idle(1000000u);
    check("the read after the answer: done at once (the interrupt), IO busy clear", SI[6], ST_IRQ);
    check("... no run: PEND bit 4 stays down", monitor_pend_read() & 0x10u, 0u);
    check("... the block has the answer and a status byte of 0", RD(A4)[12] == 0x136AF74Cu && (RD(A4)[15] & 0xFFu) == 0u, 1u);
    SI[6] = 0;
    PIF[15] = 0x12345602u;
    check("a CPU write of the status byte with the challenge's bit: IO busy", SI[6] & ST_IO, ST_IO);
    check("... PEND bit 4", monitor_pend_read() & 0x10u, 0x10u);
    check("... the bit kept", PIF[15], 0x12345602u);
    monitor_pend_write(0x10u);
    PIF[15] = 0x12345600u;
    monitor_sidone_write(1);
    check("... SIDONE: IO busy clear", SI[6] & ST_IO, 0u);
    check("... no interrupt (no DMA)", SI[6] & ST_IRQ, 0u);

    /* 7: the CPU's read of the PIF RAM while the DMA runs */
    fill_blk(A1, 0u);
    for (int i = 0; i < 16; i++) PIF[i] = 0x55555555u;
    SI[0] = A1; SI[4] = 0;
    {
        uint32_t newv = RD(A1)[7], r = 0, bad = 0, k;  /* (the words move some 3,800 cycles in: phase 10) */
        for (k = 0; k < 20000u && (r = PIF[7]) != newv; k++) if (r != 0x55555555u) bad++;
        check("a CPU read of the PIF RAM during the DMA: the old word or the new one", bad, 0u);
        check("... and the DMA's end is waited for: a read gets the new word", r, newv);
        check("... the DMA is done by then", SI[6] & ST_BUSY, 0u);
    }
    SI[6] = 0;

    /* 8: the game's own accesses, no entry into the monitor; the interrupt reaching its handler */
    game(1, 0);
    check("game: PIF RAM word 2 by KSEG1", RES[8], 0x13572468u);
    check("game: ... by KSEG0", RES[9], 0x13572468u);
    check("game: PIF RAM word 3 stored by KSEG0, read by KSEG1", RES[10], 0x2468ACE0u);
    check("game: PIF RAM word 15's status byte reads 0", RES[11], 0x0A0B0C00u);
    check("game: DRAM_ADDR", RES[12], 0x00345678u & ~7u);
    check("game: STATUS", RES[13], 0u);
    fill_blk(A1, 0u);
    RES[35] = 0;
    game(2, A1);
    check("the game's handler ran once", RES[35], 1u);
    check("handler: Cause code 0, an interrupt", (RES[32] >> 2) & 31u, 0u);
    check("handler: Cause.IP2", (RES[32] >> 10) & 1u, 1u);
    check("handler: MI_INTR has SI", RES[33] & 2u, 2u);
    check("handler: STATUS idle, the interrupt up", RES[64], ST_IRQ);
    check("handler: STATUS after the write: 0", RES[80], 0u);
    check("handler: MI_INTR cleared at once by the write to STATUS", RES[96] & 2u, 0u);
    check("handler: Cause.IP2 down at once", (RES[112] >> 10) & 1u, 0u);
    check("the DMA's block is in the PIF RAM", cmp_blk(A1, 1u), 0u);

    /* 9: the PI's engine and the AI while the SI moves blocks */
    {
        const int ai = (id & 16u) != 0u;
        if (ai) { monitor_ctl_write(0x15u); apf_audio_buffer_flush_write(1); wait_cycles(2000); monitor_airate_write(0x10000u); }
        volatile uint32_t *srcb = RD(0x00230000u), *dstb = RD(0x00240000u);
        for (int i = 0; i < 4096; i++) { srcb[i] = lcg(); dstb[i] = 0; }
        if (ai) {
            for (int i = 0; i < 4096; i++) RD(0x00250000u)[i] = lcg();
            volatile uint32_t *aiw = (volatile uint32_t *)0xA4500000u;
            aiw[3] = 0; aiw[2] = 1; aiw[0] = 0x00250000u; aiw[1] = 0x4000u;
            apf_audio_playback_en_write(1);
            wait_cycles(60000);
        }
        monitor_pidst_write(0x00240000u >> 2);
        monitor_pisrc_write((0x03800000u + 0x00230000u) >> 2);
        monitor_pilen_write(4096u);
        uint32_t bad = 0, rounds = 0;
        while ((monitor_pilen_read() & 0x80000000u) && rounds < 200) {
            fill_blk(A1, 0u);
            SI[0] = A1; SI[4] = 0;
            si_idle(1000000u);
            SI[6] = 0;
            sentinel(A2, 16, 0xEE000000u);
            rd64_run(A2);
            si_idle(1000000u);
            SI[6] = 0;
            for (int i = 0; i < 16; i++) {
                uint32_t w = RD(A1)[i];
                if (i == 15) w &= ~0xFFu;
                if (RD(A2)[i] != w) bad++;
            }
            rounds++;
        }
        for (uint32_t n2 = 0; n2 < 4000000u && (monitor_pilen_read() & 0x80000000u); n2++) { }
        uint32_t badpi = 0;
        for (int i = 0; i < 4096; i++) if (dstb[i] != srcb[i]) badpi++;
        puts_("SI rounds during the PI's copy: "); puthex(rounds); puts_("\n");
        check("the SI's blocks during the PI's copy (and the AI): every word", bad, 0u);
        check("... the SI ran at least 10 rounds in it", rounds >= 10u, 1u);
        check("the PI's copy while the SI moves blocks: every word", badpi, 0u);
        if (ai) { volatile uint32_t *aiw = (volatile uint32_t *)0xA4500000u; aiw[2] = 0; check("... and the AI was reading", aiw[1] < 0x4000u, 1u); }
    }

    /* 10: the console's time */
    {
        monitor_pend_write(0x1F);
        fill_blk(A1, 0u);
        SI[0] = A1;
        uint32_t t0 = now();
        SI[4] = 0;
        si_idle(1000000u);
        uint32_t dt = now() - t0;
        puts_("WR64B: "); puthex(dt); puts_(" cycles\n");
        check("WR64B: the console's 4,065 cycles to its end", dt >= 3900u && dt <= 4600u, 1u);
        SI[6] = 0;
        fill_blk(A1, 1u);
        SI[0] = A1; SI[4] = 0;
        si_idle(1000000u);
        SI[6] = 0;
        monitor_pend_write(0x10u);
        t0 = now();
        monitor_sidone_write(30000u);                    /* the commands' time */
        SI[0] = A4; SI[1] = 0;
        wait_cycles(20000);
        check("the read after the run a WR64B made: held while the time runs (DMA busy, no interrupt)", SI[6], ST_BUSY);
        si_idle(1000000u);
        dt = now() - t0;
        puts_("... done "); puthex(dt); puts_(" cycles after SIDONE\n");
        check("... done at the time SIDONE said", dt >= 30000u && dt <= 31500u, 1u);
        check("... the block is the PIF RAM", cmp_blk(A4, 0u), 0u);
        SI[6] = 0;
        SI[0] = A2; SI[1] = 0;                           /* the commands run again */
        for (uint32_t k = 0; k < 100000u && !(monitor_pend_read() & 0x10u); k++) { }
        monitor_pend_write(0x10u);
        t0 = now();
        monitor_sidone_write(20000u);
        wait_cycles(10000);
        check("a read that runs the commands again: held after SIDONE while its time runs", SI[6], ST_BUSY);
        si_idle(1000000u);
        dt = now() - t0;
        puts_("... done "); puthex(dt); puts_(" cycles after SIDONE\n");
        check("... done at the time SIDONE said", dt >= 20000u && dt <= 21500u, 1u);
        SI[6] = 0;
    }

    puts_("entries "); puthex(mr[2]); puts_(" (the monitor was not to be entered by the SI), last cause "); puthex(mr[4]); puts_("\n");
    check("no entry into the monitor for an SI or PIF RAM access", mr[2], 0u);
    monitor_ctl_write(0u);
    mctl(0x200u);
    puts_(fails ? "si_test: FAILED\n" : "si_test: PASS\n");
    putc_(4);
    for (;;);
}
