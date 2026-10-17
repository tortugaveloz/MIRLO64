/* mips_core's MON (the SoC built with MIRLO_MON; sim/mips_soc): a game that
 * runs its own exception handler, with the runtime in a monitor level of its
 * own beside it.
 *
 * The "game" is a loop in the N64's RDRAM (0x8000_1000) that sums and
 * xors a counter, with a Count/Compare interrupt every 3000 counts and the
 * handler a console would have at 0x8000_0180. That handler reads a device
 * register (MI_INTR) -- an access the runtime emulates, in the handler's EXL
 * and with its base register live -- and checks that Cause and EPC are still
 * what it found. The monitor (exception_entry, here) takes: the device
 * accesses (the load's result put into the register that asked: $k0, the
 * game's, or $v0), and the field interrupt of its own (irq_mon), which the
 * game cannot mask, once a frame. The loop's sums come out as the C side
 * works them out only if neither level took anything of the other's.
 *
 * Checked: every interrupt of the game reached ITS handler (0x8000_0180, with
 * Cause.IP7, EXL set, EPC inside the loop); the device load in it returned
 * what the monitor said; Cause, EPC and the game's $k0/$k1 survived the
 * monitor's entry; the monitor ran for each field; the loop's result. */
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

#define ITER 400000u
#define RES ((volatile uint32_t *)0xA0000400u)       /* the game's results, RDRAM 0x400 (KSEG1) */

/* ---- the monitor: the vector is the program's own (init_asm.S: exception_entry) ---- */
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl exception_entry\n"
"exception_entry:\n"
"    sw    $k0, -0x8000($zero)\n"                   /* the game's $k0, $k1 are all this has: 0xFFFF8000 is the monitor's RAM */
"    sw    $k1, -0x7FFC($zero)\n"
"    mfc0  $k0, $7\n"                               /* MCTL: [7:3] the cause */
"    srl   $k1, $k0, 3\n"
"    andi  $k1, $k1, 31\n"
"    li    $k0, 31\n"
"    beq   $k1, $k0, 10f\n"                         /* irq_mon */
"    li    $k0, 4\n"
"    beq   $k1, $k0, 20f\n"                         /* a load of a device */
"    li    $k0, 5\n"
"    beq   $k1, $k0, 30f\n"                         /* a store to one */
"    nop\n"
"    lui   $k0, 0x0BAD\n"                            /* nothing else is expected: the game's result says so */
"    sw    $k1, -0x7FF0($zero)\n"
"    b     40f\n"
"    nop\n"
"10: lui   $k1, 0xF000\n"                            /* the field: PEND's bit 1 cleared, a count kept */
"    li    $k0, 2\n"
"    sw    $k0, 0xD08($k1)\n"
"    lw    $k0, -0x7FF8($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF8($zero)\n"
"    b     40f\n"
"    nop\n"
"20: mfc0  $k0, $30\n"                               /* ErrorEPC: the load */
"    lw    $k1, 0($k0)\n"                            /* the instruction (the game's code, read through the window) */
"    srl   $k1, $k1, 16\n"
"    andi  $k1, $k1, 31\n"                           /* rt */
"    li    $k0, 26\n"
"    beq   $k1, $k0, 21f\n"
"    li    $k0, 2\n"
"    beq   $k1, $k0, 22f\n"
"    nop\n"
"    b     40f\n"                                    /* (another register: not in this test) */
"    nop\n"
"21: lui   $k0, 0x1234\n"                            /* $k0: its saved slot is what comes back */
"    ori   $k0, $k0, 0xABCD\n"
"    sw    $k0, -0x8000($zero)\n"
"    b     30f\n"
"    nop\n"
"22: lui   $v0, 0x1234\n"
"    ori   $v0, $v0, 0xABCD\n"
"30: mfc0  $k0, $30\n"                               /* past the access */
"    addiu $k0, $k0, 4\n"
"    mtc0  $k0, $30\n"
"    lw    $k0, -0x7FF4($zero)\n"
"    addiu $k0, $k0, 1\n"
"    sw    $k0, -0x7FF4($zero)\n"                    /* the device accesses */
"40: lw    $k0, -0x8000($zero)\n"
"    lw    $k1, -0x7FFC($zero)\n"
"    eret\n"
"    nop\n"
);

/* ---- the game: copied to the RDRAM ---- */
extern const uint32_t game_start[], game_end[], ghandler_start[], ghandler_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .align 2\n"
"game_start:\n"
"    mfc0  $t0, $12\n"
"    li    $t3, 0x8001\n"                             /* IM7, IE */
"    or    $t0, $t0, $t3\n"
"    mtc0  $t0, $12\n"
"    mfc0  $t1, $9\n"
"    addiu $t1, $t1, 3000\n"
"    mtc0  $t1, $11\n"                                /* Compare */
"    lui   $a1, 0xA000\n"
"    move  $s1, $zero\n"                              /* the sums */
"    move  $s2, $zero\n"
"    move  $t2, $zero\n"                               /* i */
"    lw    $t5, 0x420($a1)\n"                          /* the count */
"    nop\n"
"1:  addu  $s1, $s1, $t2\n"
"    sll   $t6, $t2, 3\n"
"    xor   $s2, $s2, $t6\n"
"    addiu $t2, $t2, 1\n"
"    bne   $t2, $t5, 1b\n"
"    addu  $s1, $s1, $t2\n"                            /* (the delay slot adds i + 1 on, whether or not the loop goes round) */
"    mfc0  $t0, $12\n"                                 /* interrupts off */
"    li    $t3, 0xFFFF7FFE\n"
"    and   $t0, $t0, $t3\n"
"    mtc0  $t0, $12\n"
"    sw    $s1, 0x424($a1)\n"
"    sw    $s2, 0x428($a1)\n"
"    jr    $ra\n"
"    nop\n"
"game_end:\n"
"ghandler_start:\n"                                    /* 0x8000_0180 */
"    mfc0  $k0, $13\n"
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x404($k1)\n"                           /* Cause */
"    mfc0  $k0, $14\n"
"    sw    $k0, 0x408($k1)\n"                           /* EPC */
"    mfc0  $k0, $12\n"
"    sw    $k0, 0x40C($k1)\n"                           /* Status */
"    lui   $k1, 0xA430\n"
"    lw    $k0, 8($k1)\n"                               /* MI_INTR: the monitor's, with $k1 live and EXL set */
"    lui   $k1, 0xA000\n"
"    sw    $k0, 0x410($k1)\n"
"    mfc0  $k0, $13\n"
"    sw    $k0, 0x414($k1)\n"                           /* Cause again */
"    mfc0  $k0, $14\n"
"    sw    $k0, 0x418($k1)\n"                           /* EPC again */
"    lui   $k0, 0xA430\n"
"    lw    $v0, 8($k0)\n"                                /* and into $v0 (the game's loop does not use it) */
"    lw    $k0, 0x400($k1)\n"                           /* a count of this handler's runs */
"    addiu $k0, $k0, 1\n"
"    sw    $k0, 0x400($k1)\n"
"    mfc0  $k0, $9\n"
"    addiu $k0, $k0, 3000\n"
"    mtc0  $k0, $11\n"                                  /* the next one: and IP7 is clear */
"    eret\n"
"    nop\n"
"ghandler_end:\n"
);
/* (the delay slot of the loop makes $t2 + ... : see the model below) */

static void copy_to_rdram(uint32_t phys, const uint32_t *a, const uint32_t *b)
{
    volatile uint32_t *d = (volatile uint32_t *)(uintptr_t)(0xA0000000u + phys);
    while (a < b) *d++ = *a++;
}

int main(void)
{
    puts_("mon_test\n");
    if ((monitor_id_read() & 0xFFFF0001u) != 0x4D4F0001u) { puts_("no monitor in this SoC\nmon_test: FAILED\n"); putc_(4); for (;;) ; }
    copy_to_rdram(0x1000u, game_start, game_end);
    copy_to_rdram(0x0180u, ghandler_start, ghandler_end);
    for (int i = 0; i < 12; i++) RES[i] = 0;
    RES[8] = ITER;                                       /* 0x420 */
    /* the monitor's RAM, and the SoC: the field interrupts (PEND bit 1) are the monitor's */
    volatile uint32_t *mr = (volatile uint32_t *)0xFFFF8000u;
    for (int i = 0; i < 8; i++) mr[i] = 0;
    monitor_pend_write(0xF);
    monitor_mask_write(2u);
    monitor_ctl_write(1u);                                /* OWN: IP2 is the monitor's line (0), the RSP's is not the game's */
    flush_cpu_dcache();
    __asm__ volatile(".set push\n.set mips3\n li $t0, 2\n mtc0 $t0, $7\n.set pop" ::: "t0");   /* MCTL.GAME */
    typedef void (*game_t)(void);
    ((game_t)0x80001000u)();
    __asm__ volatile(".set push\n.set mips3\n mtc0 $zero, $7\n.set pop");
    monitor_ctl_write(0u);
    uint32_t s1 = 0, s2 = 0;
    for (uint32_t i = 0; i < ITER; i++) { s1 += i; s2 ^= i << 3; s1 += i + 1; }
    check("the loop's sum", RES[9], s1);
    check("the loop's xor", RES[10], s2);
    uint32_t runs = RES[0];
    check("the game's handler ran", runs > 100u, 1u);
    check("the monitor ran for the device accesses", mr[3] >= 2u * runs, 1u);
    check("nothing the monitor does not know", mr[4], 0u);
    check("the monitor ran for the fields", mr[2] >= 1u, 1u);
    uint32_t cause = RES[1], epc = RES[2], status = RES[3];
    check("Cause: an interrupt", (cause >> 2) & 31u, 0u);
    check("Cause: IP7", (cause >> 15) & 1u, 1u);
    check("EPC in the game's loop", epc >= 0x80001000u && epc < 0x80001000u + 0x80u, 1u);
    check("Status.EXL in the handler", (status >> 1) & 1u, 1u);
    check("the device load's value", RES[4], 0x1234ABCDu);
    check("Cause after the device access", RES[5], cause);
    check("EPC after the device access", RES[6], epc);
    puts_("handler runs "); puthex(runs); puts_(", device accesses "); puthex(mr[3]); puts_(", fields "); puthex(mr[2]);
    puts_(", unexpected "); puthex(mr[4]); puts_("\n");
    puts_(fails ? "mon_test: FAILED\n" : "mon_test: PASS\n");
    putc_(4);
    for (;;);
}
