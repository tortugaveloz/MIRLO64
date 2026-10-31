/* What a mapped access costs on the real TLB (mips_core's TLB_EN; sim/mips_soc), in cycles of the game CPU, counted with Count
 * (one count every two cycles) over loops of 512 loads:
 *   hit     a load from the page the last load translated (the micro-TLB), over one from KSEG1
 *   search  a load from the other page of the pair, which the scanner finds in entry k of the TLB (it reads one entry a cycle from
 *           entry 0), over a hit
 *   refill  the game's routine (in the RDRAM's KSEG0) loads from a page that no entry holds: the refill exception, a handler of
 *           seven instructions (Context, two loads, two mtc0, tlbwr, eret) and the load again, over the same call with a KSEG0 load
 * Printed, not checked (the checks are of the other tests); the numbers are those of docs/mips.md. */
#include "tlbtest.h"

static __attribute__((unused)) uint32_t counts(void) { return MFC0(9); }
static void putnum(const char *what, uint32_t cycles_x100)
{
    puts_(what); puts_(": "); putdec(cycles_x100 / 100); putc_('.'); putc_((char)('0' + cycles_x100 / 10 % 10)); putc_((char)('0' + cycles_x100 % 10)); puts_(" cycles\n");
}
void trap_handler(void) { MTC0(14, MFC0(14) + 4u); }

/* the refill handler (copied to 0x8000_0000 and 0x8000_0180): the page table is the RDRAM from 0x8000_0000 on, 8 bytes (EntryLo0, EntryLo1)
 * a pair at Context's offset (PTEBase 0x8000_0000, BadVPN2 << 4) */
extern char hr_start[], hr_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl hr_start\n"
"hr_start:\n"
"    mfc0  $k0, $4\n"
"    lw    $k1, 0($k0)\n"
"    lw    $k0, 4($k0)\n"
"    mtc0  $k1, $2\n"
"    mtc0  $k0, $3\n"
"    nop\n"
"    tlbwr\n"
"    eret\n"
"    nop\n"
"    .globl hr_end\n"
"hr_end:\n"
);

int main(void)
{
    puts_("tlbcost_test\n");
    if (!has_real_tlb()) { puts_("tlbcost_test: SKIPPED, no real TLB in this CPU\n"); putc_(4); for (;;); }
    tlb_unmap_all();
    for (uint32_t i = 0; i < 0x400; i++) { K1(0x100000)[i] = i; K1(0x101000)[i] = i; }
    flush_cpu_dcache();
    volatile uint32_t sink = 0;
    const int N = 512;

    /* hit: the same page */
    tlb_wi(5, 0, 0x04000000u, LO(0x100000, LV | LD | LG), LO(0x101000, LV | LD | LG));
    volatile uint32_t *p = (volatile uint32_t *)0x04000000u;
    sink = p[0];
    uint32_t t0 = counts();
    for (int i = 0; i < N; i++) sink += p[(i & 7)];
    uint32_t th = counts() - t0;
    volatile uint32_t *q = K1(0x100000);
    t0 = counts();
    for (int i = 0; i < N; i++) sink += q[(i & 7)];
    uint32_t tk = counts() - t0;
    putnum("a load from KSEG1 (uncached), loop included", tk * 2 * 100 / N);
    putnum("a load from the page the last one translated, loop included", th * 2 * 100 / N);

    static const uint32_t ks[] = { 0, 4, 8, 16, 24, 31 };
    for (unsigned j = 0; j < sizeof ks / sizeof ks[0]; j++) {
        uint32_t k = ks[j];
        tlb_unmap_all();
        tlb_wi(k, 0, 0x04000000u, LO(0x100000, LV | LD | LG), LO(0x101000, LV | LD | LG));
        volatile uint32_t *e = (volatile uint32_t *)0x04000000u, *o = (volatile uint32_t *)0x04001000u;
        sink = e[0];
        t0 = counts();
        for (int i = 0; i < N; i += 2) { sink += e[(i & 7)]; sink += o[(i & 7)]; }
        uint32_t tm = counts() - t0;
        puts_("entry "); putdec(k); puts_(": ");
        putnum("a load that changes page, searched, over a hit", (tm - th) * 2 * 100 / N);
    }

    /* refill: the game's handler at the vectors, a table of pairs by Context */
    tlb_unmap_all();
    {
        const uint32_t n = (uint32_t)(hr_end - hr_start) / 4;
        for (uint32_t i = 0; i < n; i++) { *K1(i * 4) = ((const uint32_t *)hr_start)[i]; *K1(0x180 + i * 4) = ((const uint32_t *)hr_start)[i]; }
        MTC0(4, 0x80000000u);
        for (uint32_t v = 0; v < 64; v++) {              /* the pair v: virtual 0x0400_0000 + v * 0x2000, onto the same two frames */
            uint32_t slot = ((0x04000000u + v * 0x2000u) >> 13) << 4;
            *K1(slot) = LO(0x100000, LV | LD | LG); *K1(slot + 4) = LO(0x101000, LV | LD | LG);
        }
        flush_cpu_dcache(); flush_cpu_icache();
        MTC0(6, 8);                                       /* Wired 8: tlbwr replaces entries 8 .. 31, 64 pairs in turn miss nearly always */
        install_game_code();
        for (int i = 0; i < 64; i++) sink += gload(0x04000000u + (uint32_t)i * 0x2000u);
        t0 = counts();
        for (int i = 0; i < 64; i++) sink += gload(0x04000000u + (uint32_t)i * 0x2000u);
        uint32_t tr = counts() - t0;
        t0 = counts();
        for (int i = 0; i < 64; i++) sink += gload(0x80100000u + (uint32_t)i * 0x2000u);
        uint32_t tg = counts() - t0;
        putnum("a refill (exception, handler, tlbwr, eret, the load again), over a load from KSEG0", (tr - tg) * 2 * 100 / 64);
    }
    done("tlbcost_test");
    return 0;
}
