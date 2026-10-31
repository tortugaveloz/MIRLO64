/* The game CPU's own TLB instructions (mips_core's real TLB; sim/mips_soc): tlbwi, tlbwr, tlbr and tlbp are the CPU's, with
 * MCTL.GAME set (the game running under the monitor) as with it clear -- no trap for the monitor. A game's own osMapTLB,
 * osUnmapTLB, osUnmapTLBAll, __osProbeTLB and osVirtualToPhysical run as they are and nothing of libultra is found or patched.
 *
 * Checked: a pair of pages mapped by tlbwi is read and written through (both pages, 4 KiB and 64 KiB, odd and even); tlbr
 * gives the registers back as the VR4300 does (PageMask, EntryHi without the bits under the mask, G in both EntryLos,
 * EntryLo and PageMask as written, 30 and 12 bits); tlbp finds an entry by VPN2 and ASID (not a non-global one of
 * another ASID), or sets Index's P bit; tlbwr writes at Random (Wired and up); an entry replaced or invalidated
 * (osUnmapTLB's EntryHi = 0x80000000) leaves no stale page behind (the micro-TLBs); a TLB instruction in a branch's delay slot
 * is done and the branch taken; and Index is 6 bits and P (31), as the VR4300's. */
#include "tlbtest.h"

/* a fault of this program's own (its code is the SDRAM's: not the game's): counted, and the access skipped */
static volatile uint32_t t_faults, t_cause, t_badv;
void trap_handler(void)
{
    t_faults++; t_cause = MFC0(13); t_badv = MFC0(8);
    MTC0(14, MFC0(14) + 4u);
}

int main(void)
{
    puts_("tlbmon_test\n");
    if (!has_real_tlb()) { puts_("tlbmon_test: SKIPPED, no real TLB in this CPU\n"); putc_(4); for (;;); }
    tlb_unmap_all();
    for (uint32_t i = 0; i < 0x200; i++) { K1(0x10000)[i] = 0x10000000u + i; K1(0x12000)[i] = 0x12000000u + i; }
    K1(0x20000)[0] = 0x20000000u; K1(0x25000)[0] = 0x25000000u; K1(0x21000)[0] = 0x21000000u;
    K1(0x21234)[0] = 0xA1234567u; K1(0x31234)[0] = 0xB1234567u;
    K1(0x40000)[0] = 0x40000000u; K1(0x50000)[0] = 0x50000000u;
    flush_cpu_dcache();
    MTC0(7, 2);                                   /* MCTL.GAME */
    volatile uint32_t *m = (volatile uint32_t *)0x04000000u;

    /* tlbwi: a pair of 4 KiB pages, global, dirty, valid */
    tlb_wi(3, 0, 0x04000000u, LO(0x10000, LV | LD | LG), LO(0x12000, LV | LD | LG));
    check("even page", m[0], 0x10000000u);
    check("even page 2", m[5], 0x10000005u);
    check("odd page", m[0x400], 0x12000000u);
    check("odd page 2", m[0x400 + 7], 0x12000007u);
    m[1] = 0xCAFE0001u; m[0x401] = 0xCAFE0002u;
    check("store through the even page", K1(0x10000)[1], 0xCAFE0001u);
    check("store through the odd page", K1(0x12000)[1], 0xCAFE0002u);
    check("no exception", t_faults, 0);

    /* tlbr: the registers back, as written (the odd EntryLo has the G bit of both) */
    tlb_wi(4, 0, 0x04002000u | 5u, LO(0x40000, LV | LD), LO(0x50000, LV | LD | LG));     /* not global (the G is the AND of the two) */
    MTC0(10, 0x7000); MTC0(2, 0); MTC0(3, 0); MTC0(5, 0x1E000); MTC0(0, 4);
    TLBR();
    check("tlbr: EntryHi", MFC0(10), 0x04002005u);
    check("tlbr: EntryLo0", MFC0(2), LO(0x40000, LV | LD));
    check("tlbr: EntryLo1", MFC0(3), LO(0x50000, LV | LD));
    check("tlbr: PageMask", MFC0(5), 0u);
    MTC0(0, 3);
    TLBR();
    check("tlbr (global): EntryHi", MFC0(10), 0x04000000u);
    check("tlbr (global): EntryLo0", MFC0(2), LO(0x10000, LV | LD | LG));
    check("tlbr (global): EntryLo1", MFC0(3), LO(0x12000, LV | LD | LG));
    /* the registers hold what the VR4300's do: EntryLo 30 bits, PageMask 12 raw bits */
    MTC0(2, 0xFFFFFFFFu); MTC0(3, 0xFFFFFFFFu);
    check("EntryLo0: 30 bits", MFC0(2), 0x3FFFFFFFu);
    MTC0(5, 0x2000u);
    check("PageMask: raw", MFC0(5), 0x2000u);
    MTC0(5, 0x01FFE000u);
    check("PageMask: 16 MiB", MFC0(5), 0x01FFE000u);
    MTC0(5, 0xFFFFFFFFu);
    check("PageMask: 12 bits", MFC0(5), 0x01FFE000u);
    {
        /* (an entry keeps PageMask's twelve bits as written, and tlbr gives them back: the sizes a game uses and the rest alike) */
        const uint32_t w[] = { 0x00000000, 0x00006000, 0x0001E000, 0x0007E000, 0x001FE000, 0x007FE000, 0x01FFE000, 0x00002000, 0x00004000, 0x0001A000 };
        for (unsigned i = 0; i < sizeof w / sizeof w[0]; i++) {
            tlb_wi(20, w[i], 0x80000000u, 0, 0);
            MTC0(5, 0); MTC0(0, 20); TLBR();
            check("tlbwi / tlbr: PageMask", MFC0(5), w[i]);
        }
    }
    MTC0(5, 0);

    /* tlbp: by VPN2 and ASID; a non-global entry is its ASID's only; P when there is none */
    check("tlbp: global entry", tlb_probe(0x04000000u), 3);
    check("tlbp: odd page, other ASID", tlb_probe(0x04001000u | 9u), 3);     /* (same VPN2; ASID does not matter for a global one) */
    check("tlbp: ASID 5", tlb_probe(0x04002000u | 5u), 4);
    check("tlbp: another ASID", tlb_probe(0x04002000u | 6u), 0x80000000u);
    check("tlbp: no entry", tlb_probe(0x04004000u), 0x80000000u);

    /* Index: bits 5:0 are what an mtc0 writes (P, bit 31, is tlbp's alone -- and the monitor level's, which puts a state back) */
    {
        uint32_t p = MFC0(0) & 0x80000000u;                   /* (the last tlbp missed: P stays as it is) */
        MTC0(0, 0xFFFFFFFFu);
        check("mtc0 Index: six bits", MFC0(0), p | 0x3Fu);
        MTC0(0, 6u);
        check("mtc0 Index", MFC0(0), p | 6u);
    }
    tlb_probe(0x04004000u);                           /* (a miss: P is set) */
    check("tlbp miss: P", MFC0(0) >> 31, 1);

    /* the ASID of the non-global entry: a page of it is mapped for ASID 5 only */
    MTC0(10, 5);
    check("ASID 5: the page", *(volatile uint32_t *)0x04002000u, 0x40000000u);
    check("ASID 5: the odd page", *(volatile uint32_t *)0x04003000u, 0x50000000u);
    MTC0(10, 6);
    uint32_t f0 = t_faults;
    volatile uint32_t sink = *(volatile uint32_t *)0x04002000u;
    (void)sink;
    check("ASID 6: not mapped", t_faults, f0 + 1);
    check("ASID 6: TLBL", (t_cause >> 2) & 31u, 2);
    MTC0(10, 0);

    /* tlbwr: at Random, Wired and up */
    MTC0(6, 8);                                      /* Wired */
    MTC0(5, 0); MTC0(10, 0x04100000u); MTC0(2, LO(0x40000, LV | LD | LG)); MTC0(3, LO(0x50000, LV | LD | LG));
    uint32_t rnd = MFC0(1);
    check("Random from Wired up", rnd >= 8u && rnd <= 31u, 1);
    TLBWR();
    uint32_t ix = tlb_probe(0x04100000u);
    check("tlbwr: Wired and up", ix >= 8u && ix <= 31u, 1);
    check("tlbwr: mapped", *(volatile uint32_t *)0x04100000u, 0x40000000u);
    check("tlbwr: odd", *(volatile uint32_t *)0x04101000u, 0x50000000u);
    MTC0(6, 31);                                     /* Wired 31: tlbwr writes entry 31 */
    MTC0(5, 0); MTC0(10, 0x04110000u); MTC0(2, LO(0x40000, LV | LD | LG)); MTC0(3, LO(0x50000, LV | LD | LG));
    TLBWR();
    check("tlbwr: entry 31", tlb_probe(0x04110000u), 31);
    check("Wired", MFC0(6), 31u);
    MTC0(6, 0);

    /* 64 KiB pages (PageMask 0x1E000): a pair of two, any 4 KiB page of them, VPN2 under the mask */
    tlb_wi(6, 0x1E000u, 0x08000000u | 0x7000u, LO(0x20000, LV | LD | LG), LO(0x30000, LV | LD | LG));
    check("64 KiB: first page, offset 0x5000", *(volatile uint32_t *)0x08005000u, 0x25000000u);
    check("64 KiB: first page, offset 0x1000", *(volatile uint32_t *)0x08001000u, 0x21000000u);
    check("64 KiB: first page, offset 0x1234", *(volatile uint32_t *)0x08001234u, 0xA1234567u);
    check("64 KiB: second page, offset 0x1234", *(volatile uint32_t *)0x08011234u, 0xB1234567u);
    MTC0(0, 6); TLBR();
    check("64 KiB tlbr: PageMask", MFC0(5), 0x1E000u);
    check("64 KiB tlbr: EntryHi (VPN2 under the mask)", MFC0(10), 0x08000000u);
    check("64 KiB tlbp: an address in the second page", tlb_probe(0x08010000u), 6);

    /* an entry replaced: the page the micro-TLB took is gone */
    check("before: even page", m[0], 0x10000000u);
    tlb_wi(3, 0, 0x04000000u, LO(0x20000, LV | LD | LG), LO(0x12000, LV | LD | LG));
    check("replaced: the new even page", m[0], 0x20000000u);
    check("replaced: the odd page", m[0x400], 0x12000000u);

    /* osUnmapTLB's: EntryHi 0x80000000, EntryLo 0 -- the pages are gone, tlbp finds none */
    tlb_wi(3, 0, 0x80000000u, 0, 0);
    check("unmapped: tlbp", tlb_probe(0x04000000u), 0x80000000u);
    f0 = t_faults;
    volatile uint32_t r = 0x13572468u;
    r = m[0];                                         /* (no entry: the program's trap_handler skips the load) */
    check("unmapped: the access took the TLB exception", t_faults, f0 + 1);
    check("unmapped: at the page", t_badv, 0x04000000u);
    check("unmapped: TLBL", (t_cause >> 2) & 31u, 2);
    (void)r;

    /* an entry with the valid bit clear: the page is not mapped, the other of the pair is */
    tlb_wi(7, 0, 0x04200000u, LO(0x40000, LD | LG), LO(0x50000, LV | LD | LG));
    f0 = t_faults;
    r = *(volatile uint32_t *)0x04200000u;
    check("invalid even page", t_faults, f0 + 1);
    check("valid odd page", *(volatile uint32_t *)0x04201000u, 0x50000000u);

    /* a TLB instruction in a branch's delay slot: done, and the branch taken (the instruction behind the slot not run) */
    {
        uint32_t x = 0;
        MTC0(0, 9); MTC0(5, 0); MTC0(10, 0x04300000u); MTC0(2, LO(0x40000, LV | LD | LG)); MTC0(3, LO(0x50000, LV | LD | LG));
        __asm__ volatile(".set push\n.set noreorder\n.set mips3\n"
                         "    b 1f\n"
                         "    tlbwi\n"
                         "    addiu %0, %0, 100\n"                 /* (skipped by the branch) */
                         "1:  addiu %0, %0, 1\n"
                         ".set pop" : "+r"(x));
        check("delay slot: the branch taken", x, 1);
        check("delay slot: the entry written", *(volatile uint32_t *)0x04300000u, 0x40000000u);
    }

    /* code from cacheable mapped pages (the fetch micro-TLB): an entry replaced by the game's tlbwi, a 64 KiB page */
    {
        const uint32_t addiu = 0x24020000u;
        K1(0x60000)[0] = addiu | 0x6001; K1(0x60000)[1] = 0x03E00008u; K1(0x60000)[2] = 0;
        K1(0x62000)[0] = addiu | 0x6002; K1(0x62000)[1] = 0x03E00008u; K1(0x62000)[2] = 0;
        K1(0x75000)[0] = addiu | 0x7005; K1(0x75000)[1] = 0x03E00008u; K1(0x75000)[2] = 0;
        K1(0x7F000)[0] = addiu | 0x700F; K1(0x7F000)[1] = 0x03E00008u; K1(0x7F000)[2] = 0;
        typedef uint32_t (*fn_t)(void);
        tlb_wi(10, 0, 0x04400000u, LO(0x60000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: first run", ((fn_t)0x04400000u)(), 0x6001);
        check("code: again", ((fn_t)0x04400000u)(), 0x6001);
        tlb_wi(10, 0, 0x04400000u, LO(0x62000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: the entry replaced by a tlbwi: the new frame", ((fn_t)0x04400000u)(), 0x6002);
        check("code: and again", ((fn_t)0x04400000u)(), 0x6002);
        tlb_wi(10, 0, 0x04400000u, LO(0x60000, LV | LD | LG), LO(0x61000, LV | LD | LG));
        check("code: back to the first", ((fn_t)0x04400000u)(), 0x6001);
        tlb_wi(11, 0x1E000u, 0x08100000u, LO(0x70000, LV | LD | LG), LO(0x80000, LV | LD | LG));
        check("code in a 64 KiB page", ((fn_t)0x08105000u)(), 0x7005);
        check("code in a 64 KiB page, again", ((fn_t)0x08105000u)(), 0x7005);
        check("code in the page's last 4 KiB", ((fn_t)0x0810F000u)(), 0x700F);
    }

    /* with GAME off the instructions are the same: tlbp finds the entry, tlbwi writes */
    MTC0(7, 0);
    check("GAME off: tlbp", tlb_probe(0x04400000u), 10);
    MTC0(7, 2);

    check("only the expected exceptions", t_faults, 3);
    puts_("exceptions of the program's own "); puthex(t_faults); puts_("\n");
    done("tlbmon_test");
    return 0;
}
