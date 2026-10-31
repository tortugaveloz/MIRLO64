/* An interrupt of the game's that comes when the instruction in M is a fetch the TLB has no entry for (mips_core's real
 * TLB with MON; sim/mips_soc). The VR4300 takes the interrupt first, at the general vector (0x8000_0180) with Cause.ExcCode 0
 * and EPC the instruction that was to run; the fetch then faults again, when the handler has returned. The refill vector
 * (0x8000_0000) is for a TLB refill exception only: an interrupt that went there, with the refill handler of a game that fills
 * its table from Context, would put in an entry of the last fault's page, return, find the interrupt pending again and the
 * fetch still without an entry, and so on for ever.
 *
 * The game's code sets Compare a few counts ahead, enables the timer interrupt (Status.IM7, IE) and jumps to a page no entry maps:
 * the page is searched for some tens of cycles before the fetch reaches M, and the interrupt rises at a different cycle of those
 * in each run (0 .. 59 counts). Both vectors' handlers put Compare far ahead (the interrupt is taken once) and count what they
 * were given: the refill vector's handler counts an interrupt apart from a refill, and the general vector's logs the EPC.
 * Checked: no interrupt at the refill vector, every run returned the function's value, and (printed) how many interrupts came
 * with the fault's fetch in M -- EPC the function's address. */
#include "tlbtest.h"

static volatile uint32_t t_own;
void trap_handler(void) { t_own++; MTC0(14, MFC0(14) + 4u); }

extern char hr_start[], hr_end[], hg_start[], hg_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl hr_start\n"
"hr_start:\n"
"    mfc0  $k0, $13\n"
"    andi  $k0, $k0, 0x7C\n"
"    bnez  $k0, 1f\n"
"    nop\n"
"    lui   $k0, 0x8001\n"                         /* an interrupt at the refill vector: counted, Compare far ahead, back */
"    lw    $k1, 0x70($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0x70($k0)\n"
"    lui   $k0, 0x4000\n"
"    mfc0  $k1, $9\n"
"    addu  $k1, $k1, $k0\n"
"    mtc0  $k1, $11\n"
"    nop\n"
"    nop\n"
"    eret\n"
"    nop\n"
"1:  mfc0  $k0, $4\n"                              /* a refill: the table by Context, as a game's */
"    lw    $k1, 0($k0)\n"
"    lw    $k0, 4($k0)\n"
"    mtc0  $k1, $2\n"
"    mtc0  $k0, $3\n"
"    nop\n"
"    nop\n"
"    tlbwr\n"
"    nop\n"
"    nop\n"
"    eret\n"
"    nop\n"
"    .globl hr_end\n"
"hr_end:\n"
"    .globl hg_start\n"
"hg_start:\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0x74($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0x74($k0)\n"
"    sll   $k1, $k1, 2\n"
"    addu  $k1, $k1, $k0\n"
"    mfc0  $k0, $14\n"
"    sw    $k0, 0x80($k1)\n"                       /* (the EPC of the n-th interrupt, at 0x80 + 4 n) */
"    lui   $k0, 0x4000\n"
"    mfc0  $k1, $9\n"
"    addu  $k1, $k1, $k0\n"
"    mtc0  $k1, $11\n"
"    nop\n"
"    nop\n"
"    eret\n"
"    nop\n"
"    .globl hg_end\n"
"hg_end:\n"
);

#define LOG(o) (*K0(0x10000 + (o)))
static void pte(uint32_t va, uint32_t lo0, uint32_t lo1)
{
    volatile uint32_t *e = K0((va >> 13) << 4);
    e[0] = lo0; e[1] = lo1;
}

/* the game's code at 0x8003_0000, written here for d counts: v0 = (the function at a0)(); Status.IE off again
 *   t7 = ra; Compare = Count + d; Status |= IM7 | IE; jalr a0; nop; IE off; Compare = Count + 0x4000_0000; ra = t7; jr ra; nop */
static void put_game_code(uint32_t d)
{
    uint32_t w[40], n = 0;
    w[n++] = 0x03E07821u;                         /* addu  t7, ra, zero */
    w[n++] = 0x40084800u | 0;                     /* (mfc0 t0, Count: rt t0 = 8, rd 9) */
    w[n - 1] = 0x40084800u;
    w[n++] = 0x25080000u | (d & 0xFFFFu);         /* addiu t0, t0, d */
    w[n++] = 0x40885800u;                         /* mtc0  t0, Compare */
    w[n++] = 0x40096000u;                         /* mfc0  t1, Status */
    w[n++] = 0x35298001u;                         /* ori   t1, t1, 0x8001 */
    w[n++] = 0x40896000u;                         /* mtc0  t1, Status */
    w[n++] = 0x0080F809u;                         /* jalr  ra, a0 */
    w[n++] = 0;
    w[n++] = 0x40096000u;                         /* mfc0  t1, Status */
    w[n++] = 0x240AFFFEu;                         /* addiu t2, zero, -2 */
    w[n++] = 0x012A4824u;                         /* and   t1, t1, t2 */
    w[n++] = 0x40896000u;                         /* mtc0  t1, Status */
    w[n++] = 0x40084800u;                         /* mfc0  t0, Count */
    w[n++] = 0x3C094000u;                         /* lui   t1, 0x4000 */
    w[n++] = 0x01094021u;                         /* addu  t0, t0, t1 */
    w[n++] = 0x40885800u;                         /* mtc0  t0, Compare */
    w[n++] = 0x01E0F821u;                         /* addu  ra, t7, zero */
    w[n++] = 0x03E00008u;                         /* jr    ra */
    w[n++] = 0;
    for (uint32_t i = 0; i < n; i++) *K1(0x30000 + 4 * i) = w[i];
    flush_cpu_dcache(); flush_cpu_icache();
}

int main(void)
{
    puts_("tlbint_real_test\n");
    if (!has_real_tlb() || !(MFC0(23) & 1u)) { puts_("tlbint_real_test: SKIPPED, no real TLB with the monitor in this CPU\n"); putc_(4); for (;;); }
    tlb_unmap_all();
    /* the function on the mapped pages: addiu v0, zero, 0x1234 / jr ra / nop */
    K1(0x120000)[0] = 0x24021234u; K1(0x120000)[1] = 0x03E00008u; K1(0x120000)[2] = 0;
    copy_k1(0x0, hr_start, hr_end);
    copy_k1(0x180, hg_start, hg_end);
    for (uint32_t o = 0; o < 0x80; o += 4) LOG(o) = 0;
    flush_cpu_dcache(); flush_cpu_icache();
    MTC0(4, 0x80000000u);                         /* Context: PTEBase */
    MTC0(7, 2);                                   /* MCTL.GAME */
    MTC0(13, 0);
    uint32_t bad = 0, at_fault = 0, n_int = 0;
    for (uint32_t d = 0; d < 60; d++) {
        const uint32_t va = 0x04100000u + (d % 16) * 0x10000u;
        pte(va, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        tlb_unmap_all();                          /* (the page's entry from the run before: gone, the fetch faults again) */
        put_game_code(d);
        MTC0(13, 0);
        uint32_t r = ((uint32_t (*)(uint32_t))0x80030000u)(va);
        if (r != 0x1234) { bad++; puts_("the function did not run for d = "); putdec(d); puts_("\n"); }
        while (n_int < LOG(0x74)) { n_int++; if (LOG(0x80 + 4 * n_int) == va) at_fault++; }
        check("no interrupt at the refill vector", LOG(0x70), 0);
        if (LOG(0x70) != 0) break;
    }
    check("every run returned the function's value", bad, 0);
    check("no exception of the program's own", t_own, 0);
    puts_("interrupts at the refill vector "); puthex(LOG(0x70)); puts_(", at the general vector "); puthex(LOG(0x74)); puts_(", with the fault in M "); puthex(at_fault); puts_("\n");
    done("tlbint_real_test");
    return 0;
}
