/* mips_core's translation tables in block RAM (TLB_RAM, the SoC's
 * MIRLO_TLB_RAM; sim/mips_soc): an address below 0x2000_0000 is a mapped one.
 * Its page's entry -- written through COP0 23 for EntryHi's page and ASID
 * (the physical page, which of the pair, the table, global / dirty / valid)
 * -- translates it into the N64's RDRAM, or the access takes the TLB's
 * exception with BadVAddr the address and EntryHi its page, and the handler
 * fills the entry and returns to the instruction: data, a store to a clean
 * page, another page of the same index, the ASID, a fetch from a mapped page
 * (the fetches' table: register 23 reads which the exception was), code and
 * its data on pages of one index, and a delay slot on the page after its
 * branch's. */
#include <stdint.h>
#include <generated/csr.h>
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

#define MFC0(r) ({ uint32_t v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((uint32_t)(v)))
enum { V = 1, D = 2, G = 4, FETCH = 8 };
/* an entry for va's page: EntryHi has the page pair and the ASID */
static void tlb_set(uint32_t va, uint32_t asid, uint32_t pa, uint32_t flags)
{
    uint32_t hi = MFC0(10);
    MTC0(10, (va & ~0x1FFFu) | asid);
    MTC0(23, (pa & 0x1FFFF000u) | (va & 0x1000u ? 16u : 0u) | flags);
    MTC0(10, hi);
}

/* what the handler fills a faulting page with (0 flags: it skips the access instead) */
static volatile uint32_t s_fill_pa, s_fill_flags;
static volatile uint32_t s_traps, s_cause, s_badv, s_epc, s_fetch;
void trap_handler(void)
{
    s_traps++;
    s_cause = MFC0(13); s_badv = MFC0(8); s_epc = MFC0(14);
    s_fetch = MFC0(23) >> 3 & 1;
    /* (EntryHi: the faulting page and the ASID, as the CPU left them) */
    if (s_fill_flags) MTC0(23, s_fill_pa | (s_badv & 0x1000u ? 16u : 0u) | s_fill_flags | (s_fetch ? FETCH : 0));
    else MTC0(14, s_epc + 4);
}
#define CODE (s_cause >> 2 & 31)

#define K0(pa) ((volatile uint32_t *)(0x80000000u | (pa)))
#define K1(pa) ((volatile uint32_t *)(0xA0000000u | (pa)))
typedef uint32_t (*fn_t)(uint32_t a);

int main(void)
{
    puts_("tlbram_test\n");
    if ((MFC0(23) & ~9u) != 0x54420000u) { puts_("tlbram_test: SKIPPED, no tables in this CPU\n"); putc_(4); for (;;); }
    for (uint32_t i = 0; i < 512; i++) { tlb_set(i << 12, 0, 0, 0); tlb_set(i << 12, 0, 0, FETCH); }

    /* data, through a valid entry: no exception */
    K0(0x10000)[0] = 0x600DCAFEu; K0(0x10000)[1] = 0x11112222u;
    tlb_set(0x04000000u, 0, 0x00010000u, V | G);
    volatile uint32_t *m = (volatile uint32_t *)0x04000000u;
    check("load", m[0], 0x600DCAFEu);
    check("load 2", m[1], 0x11112222u);
    check("no trap", s_traps, 0);

    /* a store to a page that is not dirty: Mod; the handler makes it dirty */
    s_fill_pa = 0x00010000u; s_fill_flags = V | D | G;
    m[2] = 0xABCD0123u;
    check("mod trap", s_traps, 1);
    check("mod code", CODE, 1);
    check("mod badv", s_badv, 0x04000008u);
    check("mod: data's", s_fetch, 0);
    check("mod store", K0(0x10000)[2], 0xABCD0123u);
    m[3] = 0x55AA55AAu;
    check("dirty: no trap", s_traps, 1);
    check("dirty store", K1(0x10000)[3], 0x55AA55AAu);
    ((volatile uint8_t *)m)[13] = 0x77;
    check("byte store", K0(0x10000)[3], 0x557755AAu);

    /* no entry: TLBL on a load, TLBS on a store; filled, and the access again */
    K0(0x12000)[0] = 0xFEEDF00Du;
    s_fill_pa = 0x00012000u;
    check("miss load", *(volatile uint32_t *)0x04005000u, 0xFEEDF00Du);
    check("miss trap", s_traps, 2);
    check("miss code", CODE, 2);
    check("miss badv", s_badv, 0x04005000u);
    s_fill_pa = 0x00013000u;
    *(volatile uint32_t *)0x04006004u = 0x13572468u;
    check("miss store trap", s_traps, 3);
    check("miss store code", CODE, 3);
    check("miss store", K0(0x13000)[1], 0x13572468u);

    /* another page of the same index (2 MiB on): its own entry, then the first's again */
    K0(0x14000)[0] = 0x0BADBEEFu;
    s_fill_pa = 0x00014000u;
    check("same index, other page", *(volatile uint32_t *)0x04200000u, 0x0BADBEEFu);
    check("other page trap", s_traps, 4);
    s_fill_pa = 0x00010000u;
    check("the first again", m[0], 0x600DCAFEu);
    check("first again trap", s_traps, 5);

    /* an invalid entry is no entry */
    tlb_set(0x04007000u, 0, 0x00012000u, G);
    s_fill_pa = 0x00012000u; s_fill_flags = V | G;
    check("invalid load", *(volatile uint32_t *)0x04007000u, 0xFEEDF00Du);
    check("invalid trap", s_traps, 6);
    check("invalid code", CODE, 2);

    /* the ASID: an entry that is not global is its ASID's only */
    tlb_set(0x04008000u, 5, 0x00012000u, V);
    MTC0(10, 5);
    check("asid 5", *(volatile uint32_t *)0x04008000u, 0xFEEDF00Du);
    check("asid: no trap", s_traps, 6);
    MTC0(10, 6);
    s_fill_pa = 0x00014000u; s_fill_flags = V;           /* (filled for the ASID in EntryHi: 6) */
    check("asid 6", *(volatile uint32_t *)0x04008000u, 0x0BADBEEFu);
    check("asid trap", s_traps, 7);
    check("asid code", CODE, 2);
    check("asid kept", MFC0(10) & 0xFF, 6);
    MTC0(10, 5);
    s_fill_pa = 0x00012000u;
    check("asid 5 again: its own entry", *(volatile uint32_t *)0x04008000u, 0xFEEDF00Du);
    check("asid 5 trap", s_traps, 8);
    check("global with any asid", m[0], 0x600DCAFEu);
    check("global: no trap", s_traps, 8);
    MTC0(10, 0);

    /* code in a mapped page: li v0 / jr ra / nop, and lw v0, 0(a0) / jr ra / nop */
    volatile uint32_t *c = K1(0x30000);
    c[0] = 0x24020123u; c[1] = 0x03E00008u; c[2] = 0x00000000u;
    c[4] = 0x8C820000u; c[5] = 0x03E00008u; c[6] = 0x00000000u;
    s_fill_pa = 0x00030000u; s_fill_flags = V | G;
    check("fetch", ((fn_t)0x05000000u)(0), 0x123u);
    check("fetch trap", s_traps, 9);
    check("fetch code", CODE, 2);
    check("fetch badv", s_badv, 0x05000000u);
    check("fetch epc", s_epc, 0x05000000u);
    check("fetch: the fetches' table", s_fetch, 1);
    check("fetch again", ((fn_t)0x05000000u)(0), 0x123u);
    check("fetch again: no trap", s_traps, 9);
    /* (0x05000000 and 0x04000000 have one index: each in its own table) */
    check("mapped code, mapped data", ((fn_t)0x05000010u)(0x04000004u), 0x11112222u);
    check("mapped code, RDRAM", ((fn_t)0x05000010u)(0x80012000u), 0xFEEDF00Du);
    check("still no trap", s_traps, 9);
    /* a page's entry for fetches is not its entry for data: the code reads itself */
    s_fill_pa = 0x00030000u;
    check("code read as data", ((fn_t)0x05000010u)(0x05000010u), 0x8C820000u);
    check("its data entry", s_traps, 10);
    check("data's table", s_fetch, 0);
    s_fill_pa = 0x00010000u;
    check("the data page again", ((fn_t)0x05000010u)(0x04000000u), 0x600DCAFEu);
    check("and its entry", s_traps, 11);

    /* a branch in a page's last word, its delay slot on the next page, not there yet:
     * the exception is the branch's (BD), and both run again */
    c = K1(0x30FF8);
    c[0] = 0x24020040u;                             /* li v0, 0x40 */
    c[1] = 0x03E00008u;                             /* jr ra */
    K1(0x40000)[0] = 0x24420002u;                   /* addiu v0, v0, 2 */
    s_fill_pa = 0x00040000u;
    check("slot on the next page", ((fn_t)0x05000FF8u)(0), 0x42u);
    check("slot trap", s_traps, 12);
    check("slot code", CODE, 2);
    check("slot badv", s_badv, 0x05001000u);
    check("slot epc", s_epc, 0x05000FFCu);
    check("slot BD", s_cause >> 31, 1);

    /* the VR4300's own TLB instructions do nothing (and nothing waits on them) */
    __asm__ volatile(".set push\n.set mips3\ntlbwi\ntlbp\ntlbr\n.set pop");
    check("tlbp: none", MFC0(0) >> 31, 1);
    check("the table as it was", m[0], 0x600DCAFEu);
    check("no trap at the end", s_traps, 12);

    puts_(fails ? "tlbram_test: FAILED\n" : "tlbram_test: PASS\n");
    putc_(4);
    for (;;);
}
