/* mips_core's real TLB (sim/mips_soc): the VR4300's 32 entries, fully associative, a pair of pages each, tlbwi / tlbwr / tlbr /
 * tlbp executed by the CPU and the TLB exceptions (refill, invalid, modified) raised in it, to the game's own handlers.
 *
 *   1  the capability word (COP0 23)
 *   2  the seven page sizes (4 KiB .. 4 MiB in the RDRAM; 16 MiB at the start of the physical map): the even and the odd page, the
 *      first and last words, loads, stores and instruction fetches, the frames' own data through KSEG1
 *   3  the micro-TLBs: more pairs than they hold, in turn; an entry replaced by tlbwi / tlbwr / tlbr / a new ASID is gone at
 *      once; an ASID switch back and forth
 *   4  exceptions as the VR4300 raises them, with every register the handler sees (Cause.ExcCode and BD, BadVAddr, Context's
 *      BadVPN2, EntryHi's VPN2 and ASID, EPC, Status.EXL), the vector of each: a refill of a load / store / fetch, an invalid
 *      page of each, a clean page (Mod), invalid and clean (TLBS), a delay slot, EXL set, a fetch across a page boundary
 *   5  tlbp (P bit on a miss, the ASID, global), tlbr round trips of every mask, Index / Wired / Random, tlbwi's Index above 31
 *   6  caches: a store through a mapped page is seen through KSEG0 (the D-cache line drops), code written through one alias and
 *      invalidated by a hit-type cache op through a mapped address runs new; uncached (C = 2) code runs; ll / sc through a pair of
 *      aliases
 *   7  the 0x6000_0000 range
 * The exceptions are the game's (mapped pc, or a KSEG0 routine of tlbtest.h's): the game vectors (tlbtest.h) end in this
 * program's trap_handler, which does what the test's variable h_act says. */
#include "tlbtest.h"

/* ---- the handler */
enum { A_SKIP = 0, A_REFILL = 1, A_REPAIR = 2, A_RET = 3 };
static volatile uint32_t h_act, h_ret, h_pm, h_hi, h_lo0, h_lo1;     /* what to do; EPC for A_RET; the entry for A_REFILL / A_REPAIR */
static volatile uint32_t h_n, h_cause, h_badv, h_epc, h_ctx, h_ehi, h_status, h_index;
static volatile uint32_t h_own;                                        /* faults of this program's own pc */
void trap_handler(void)
{
    h_n++;
    h_cause = MFC0(13); h_badv = MFC0(8); h_epc = MFC0(14); h_ctx = MFC0(4); h_ehi = MFC0(10); h_status = MFC0(12);
    h_index = MFC0(0);
    switch (h_act) {
    case A_REFILL:
        MTC0(5, h_pm); MTC0(10, h_hi); MTC0(2, h_lo0); MTC0(3, h_lo1);
        TLBWR();
        break;
    case A_REPAIR: {
        uint32_t ix = tlb_probe(h_hi);
        MTC0(0, ix); MTC0(5, h_pm); MTC0(10, h_hi); MTC0(2, h_lo0); MTC0(3, h_lo1);
        TLBWI();
        break;
    }
    case A_RET:
        MTC0(14, h_ret);
        break;
    default:
        MTC0(14, h_epc + 4u);
        break;
    }
}
static void act(uint32_t a, uint32_t pm, uint32_t hi, uint32_t lo0, uint32_t lo1) { h_act = a; h_pm = pm; h_hi = hi; h_lo0 = lo0; h_lo1 = lo1; }
#define CODE() ((h_cause >> 2) & 31u)

/* ---- helpers */
static const uint32_t pmask[7] = { 0x0, 0x6000, 0x1E000, 0x7E000, 0x1FE000, 0x7FE000, 0x1FFE000 };
typedef uint32_t (*fn_t)(void);
static void mkfn(uint32_t pa, uint32_t imm)             /* addiu v0, zero, imm / jr ra / nop, uncached into the frame */
{
    volatile uint32_t *c = K1(pa);
    c[0] = 0x24020000u | (imm & 0xFFFFu); c[1] = 0x03E00008u; c[2] = 0;
}
static void icache_hit_inval(uint32_t va) { __asm__ volatile(".set push\n.set mips3\ncache 0x10, 0(%0)\n.set pop" :: "r"(va) : "memory"); }

int main(void)
{
    puts_("tlbram_test\n");
    /* ---- 1 */
    check("COP0 23: 'TLB'", MFC0(23) >> 8, 0x544C42u);
    check("COP0 23: a real TLB", (MFC0(23) >> 1) & 1u, 1);
    if (!has_real_tlb()) { puts_("tlbram_test: SKIPPED, no real TLB in this CPU\n"); putc_(4); for (;;); }
    int mon = MFC0(23) & 1, fpg = (MFC0(23) >> 2) & 1;
    tlb_unmap_all();
    install_game_vectors();
    install_game_code();
    MTC0(7, mon ? 2 : 0);                                    /* MCTL.GAME: a TLB exception of a game's pc is the game's */
    MTC0(4, 0x80000000u);                                    /* Context: PTEBase */
    act(A_SKIP, 0, 0, 0, 0);

    puts_("section 2\n");
    /* ---- 2: the sizes. The pair's frames are the RDRAM's at 0x0040_0000 (4 KiB .. 1 MiB) or 0 (4 MiB); a word of each is marked */
    for (uint32_t k = 0; k < 6; k++) {
        uint32_t span = 0x1000u << (2 * k), va = 0x04000000u + (k << 24);
        uint32_t base = k == 5 ? 0u : 0x00400000u;
        uint32_t even = base, odd = base + span;
        uint32_t offs[4] = { 0x40u, 0x44u, span - 8u, span / 2 + 12u };
        for (int j = 0; j < 4; j++) {
            *K1(even + offs[j]) = 0xE0000000u | (k << 20) | (uint32_t)j;
            *K1(odd + offs[j]) = 0x0D000000u | (k << 20) | (uint32_t)j;
        }
        tlb_wi(k, pmask[k], va | 3u, LO(even, LV | LD | LG), LO(odd, LV | LD | LG));
        for (int j = 0; j < 4; j++) {
            check("size: even page", *(volatile uint32_t *)(va + offs[j]), 0xE0000000u | (k << 20) | (uint32_t)j);
            check("size: odd page", *(volatile uint32_t *)(va + span + offs[j]), 0x0D000000u | (k << 20) | (uint32_t)j);
        }
        *(volatile uint32_t *)(va + offs[0] + 4) = 0x5A5A0000u | k;
        *(volatile uint32_t *)(va + span + offs[0] + 4) = 0xA5A50000u | k;
        check("size: store, even", *K1(even + offs[0] + 4), 0x5A5A0000u | k);
        check("size: store, odd", *K1(odd + offs[0] + 4), 0xA5A50000u | k);
        check("size: tlbp, the odd page", tlb_probe((va + span) | 3u), k);
        check("size: tlbp, the last byte of the pair", tlb_probe((va + 2 * span - 1u) | 3u), k);
        check("size: tlbp, the next pair", tlb_probe((va + 2 * span) | 3u) >> 31, 1);
        MTC0(0, k); TLBR();
        check("size: tlbr, PageMask", MFC0(5), pmask[k]);
        check("size: tlbr, EntryHi", MFC0(10), va | 3u);
        check("size: tlbr, EntryLo0", MFC0(2), LO(even, LV | LD | LG));
        check("size: tlbr, EntryLo1", MFC0(3), LO(odd, LV | LD | LG));
    }
    MTC0(10, 0);
    /* code in the pages of every size */
    for (uint32_t k = 0; k < 6; k++) {
        uint32_t span = 0x1000u << (2 * k), va = 0x04000000u + (k << 24);
        uint32_t base = k == 5 ? 0u : 0x00400000u;
        mkfn(base + span - 0x40, 0x100 + k);              /* near the end of the even page */
        mkfn(base + span + 0x80, 0x200 + k);              /* the start of the odd one */
        check("code, even page", ((fn_t)(va + span - 0x40))(), 0x100 + k);
        check("code, odd page", ((fn_t)(va + span + 0x80))(), 0x200 + k);
        check("code, even page again", ((fn_t)(va + span - 0x40))(), 0x100 + k);
    }
    /* 16 MiB: the even page at physical 0 (the RDRAM's first 8 MiB are reachable through it) */
    tlb_wi(7, pmask[6], 0x60000000u | 1u * 0, LO(0, LV | LD | LG), LO(0x01000000u, LV | LD | LG));
    *K1(0x00500000) = 0x16160001u; *K1(0x007FFFF8) = 0x16160002u;
    check("16 MiB: offset 0x500000", *(volatile uint32_t *)0x60500000u, 0x16160001u);
    check("16 MiB: offset 0x7FFFF8", *(volatile uint32_t *)0x607FFFF8u, 0x16160002u);
    MTC0(0, 7); TLBR();
    check("16 MiB: tlbr PageMask", MFC0(5), 0x01FFE000u);
    check("16 MiB: tlbp in the odd page", tlb_probe(0x61FFFFF0u), 7);
    tlb_unmap_all();

    puts_("section 3\n");
    /* ---- 3: the micro-TLBs */
    {
        for (uint32_t n = 0; n < 6; n++) {
            *K1(0x00300000 + n * 0x2000 + 8) = 0xB0000000u | n; *K1(0x00300000 + n * 0x2000 + 0x1008) = 0xB1000000u | n;
            tlb_wi(n, 0, 0x10000000u + n * 0x2000u, LO(0x00300000 + n * 0x2000, LV | LD | LG), LO(0x00301000 + n * 0x2000, LV | LD | LG));
        }
        for (int r = 0; r < 5; r++)
            for (uint32_t n = 0; n < 6; n++) {
                check("six pairs in turn, even", *(volatile uint32_t *)(0x10000008u + n * 0x2000u), 0xB0000000u | n);
                check("six pairs in turn, odd", *(volatile uint32_t *)(0x10001008u + n * 0x2000u), 0xB1000000u | n);
            }
        for (int r = 0; r < 3; r++) {                              /* a copy between two pairs */
            *(volatile uint32_t *)0x10002010u = *(volatile uint32_t *)0x10000008u;
            *(volatile uint32_t *)0x10004010u = *(volatile uint32_t *)0x10002008u;
        }
        check("the copy", *K1(0x00304010), 0xB0000001u);
        /* replaced: tlbwi, tlbwr, and the access right after sees the new frame */
        tlb_wi(0, 0, 0x10000000u, LO(0x00300000 + 0x6000, LV | LD | LG), LO(0x00301000 + 0x6000, LV | LD | LG));
        *K1(0x00306008) = 0xB0000099u;
        check("replaced by tlbwi: the new frame at once", *(volatile uint32_t *)0x10000008u, 0xB0000099u);
        check("replaced by tlbwi: the other pair is untouched", *(volatile uint32_t *)0x10002008u, 0xB0000001u);
        MTC0(6, 31);                                              /* Wired 31: tlbwr writes entry 31 */
        MTC0(5, 0); MTC0(10, 0x10200000u); MTC0(2, LO(0x00300000, LV | LD | LG)); MTC0(3, LO(0x00301000, LV | LD | LG)); TLBWR();
        check("tlbwr: the pair", *(volatile uint32_t *)0x10200008u, 0xB0000000u);
        MTC0(5, 0); MTC0(10, 0x10200000u); MTC0(2, LO(0x00302000, LV | LD | LG)); MTC0(3, LO(0x00303000, LV | LD | LG)); TLBWR();
        check("replaced by tlbwr: the new frame at once", *(volatile uint32_t *)0x10200008u, 0xB0000001u);
        MTC0(6, 0);
        /* an entry made invalid: the access is the exception (the micro-TLB held it) */
        act(A_SKIP, 0, 0, 0, 0);
        uint32_t n0 = h_n;
        check("(valid)", *(volatile uint32_t *)0x10004008u, 0xB0000002u);
        tlb_wi(2, 0, 0x10004000u, LO(0x00304000, LD | LG), LO(0x00305000, LV | LD | LG));
        (void)gload(0x10004008u);
        check("made invalid: the exception", h_n, n0 + 1);
        check("made invalid: TLBL", CODE(), 2);
        check("made invalid: the odd page still reads", *(volatile uint32_t *)0x10005008u, 0xB1000002u);
        /* tlbr (loads EntryHi) and a new ASID empty the micro-TLBs: a non-global entry of ASID 5 */
        tlb_wi(9, 0, 0x10100000u | 5u, LO(0x00300000, LV | LD), LO(0x00301000, LV | LD));
        MTC0(10, 5);
        check("ASID 5: the page", *(volatile uint32_t *)0x10100008u, 0xB0000000u);
        MTC0(10, 6);
        n0 = h_n;
        (void)gload(0x10100008u);
        check("ASID 6: the page is not there any more", h_n, n0 + 1);
        MTC0(10, 5);
        check("ASID 5 again", *(volatile uint32_t *)0x10100008u, 0xB0000000u);
        MTC0(0, 9); TLBR();                                         /* (EntryHi back to ASID 5 by the entry) */
        MTC0(10, 7);
        n0 = h_n;
        (void)gload(0x10100008u);
        check("ASID 7: not there", h_n, n0 + 1);
        MTC0(10, 0);
        tlb_unmap_all();
    }

    puts_("section 4\n");
    /* ---- 4: exceptions, as the game's handler sees them */
    {
        uint32_t va = 0x14000000u, n0, r0;
        *K1(0x00500000) = 0xC0000000u; *K1(0x00501000) = 0xC1000000u; *K1(0x00500008) = 0xC0000008u;
        MTC0(10, 0x23u);                                           /* ASID 0x23 */
        /* a refill of a load: the handler writes the pair, the load runs again */
        REFILLS = 0; GENERALS = 0;
        act(A_REFILL, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        n0 = h_n;
        check("refill: the value", gload(va + 8), 0xC0000008u);
        check("refill: one exception", h_n, n0 + 1);
        check("refill: the refill vector", REFILLS, 1);
        check("refill: not the general vector", GENERALS, 0);
        check("refill: Cause.ExcCode TLBL", CODE(), 2);
        check("refill: Cause.BD", h_cause >> 31, 0);
        check("refill: BadVAddr", h_badv, va + 8);
        check("refill: Context", h_ctx, 0x80000000u | ((va >> 13) << 4));
        check("refill: EntryHi", h_ehi, va | 0x23u);
        check("refill: EPC the load", h_epc, GC(gc_load));
        check("refill: EXL", (h_status >> 1) & 1u, 1);
        check("refill: the odd page, no exception", *(volatile uint32_t *)(va + 0x1000), 0xC1000000u);
        check("refill: no second", h_n, n0 + 1);
        check("after the eret EXL is clear", (MFC0(12) >> 1) & 1u, 0);
        /* a refill of a store */
        tlb_unmap_all(); MTC0(10, 0x23u);
        REFILLS = 0;
        act(A_REFILL, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        gstore(va + 0x1010, 0x57A7E001u);
        check("store refill: TLBS", CODE(), 3);
        check("store refill: the refill vector", REFILLS, 1);
        check("store refill: BadVAddr", h_badv, va + 0x1010);
        check("store refill: landed", *K1(0x00501010), 0x57A7E001u);
        /* an invalid page (the entry is there): the general vector, TLBL for a load */
        tlb_wi(4, 0, va | 0x23u, LO(0x00500000, LD), LO(0x00501000, LV | LD));
        GENERALS = 0; REFILLS = 0;
        act(A_REPAIR, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        check("invalid: the load after the repair", gload(va + 8), 0xC0000008u);
        check("invalid: the general vector", GENERALS, 1);
        check("invalid: not the refill vector", REFILLS, 0);
        check("invalid: TLBL", CODE(), 2);
        check("invalid: Context", h_ctx, 0x80000000u | ((va >> 13) << 4));
        check("invalid: EntryHi", h_ehi, va | 0x23u);
        /* ... and a store to it: TLBS */
        tlb_wi(4, 0, va | 0x23u, LO(0x00500000, LD), LO(0x00501000, LV | LD));
        act(A_REPAIR, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        gstore(va + 0x20, 0x57A7E002u);
        check("invalid store: TLBS", CODE(), 3);
        check("invalid store: the general vector", GENERALS, 2);
        check("invalid store: landed after the repair", *K1(0x00500020), 0x57A7E002u);
        /* a clean page: a load is fine, a store is Mod; invalid and clean is TLBS */
        tlb_wi(4, 0, va | 0x23u, LO(0x00500000, LV), LO(0x00501000, LV));
        act(A_REPAIR, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        n0 = h_n;
        check("clean: a load", gload(va + 8), 0xC0000008u);
        check("clean: no exception for a load", h_n, n0);
        gstore(va + 0x24, 0x57A7E003u);
        check("clean: Mod", CODE(), 1);
        check("clean: the general vector", GENERALS, 3);
        check("clean: BadVAddr", h_badv, va + 0x24);
        check("clean: landed after the repair", *K1(0x00500024), 0x57A7E003u);
        gstore(va + 0x28, 0x57A7E004u);
        check("clean: repaired, no second Mod", h_n, n0 + 1);
        tlb_wi(4, 0, va | 0x23u, LO(0x00500000, 0), LO(0x00501000, LV));
        act(A_REPAIR, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        gstore(va + 0x2C, 0x57A7E005u);
        check("invalid and clean: TLBS wins over Mod", CODE(), 3);
        /* a refill in a delay slot: EPC the branch, BD */
        tlb_unmap_all(); MTC0(10, 0x23u);
        REFILLS = 0;
        act(A_REFILL, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        {
            uint32_t v0, x = gslot(va + 8, &v0);
            check("delay slot: the branch taken, the slot's load", x, 1);
            check("delay slot: the value", v0, 0xC0000008u);
            check("delay slot: Cause.BD", h_cause >> 31, 1);
            check("delay slot: EPC the branch", h_epc, GC(gc_slot) + 8);
            check("delay slot: BadVAddr", h_badv, va + 8);
            check("delay slot: the refill vector", REFILLS, 1);
        }
        (void)gload(va + 0x10);                                    /* (hits) */
        tlb_unmap_all(); MTC0(10, 0x23u);
        act(A_REFILL, 0, va | 0x23u, LO(0x00500000, LV | LD), LO(0x00501000, LV | LD));
        (void)gload(va + 0x10);
        check("after a delay slot's BD: BD clear again", h_cause >> 31, 0);
        /* a refill with EXL set: the general vector, EPC and BD as they were */
        tlb_unmap_all(); MTC0(10, 0x23u);
        REFILLS = 0; GENERALS = 0;
        act(A_RET, 0, 0, 0, 0); h_ret = GC(gc_exl_land);
        MTC0(14, GC(gc_exl_land));
        MTC0(13, 0);
        {
            register uint32_t a0 __asm__("a0") = va + 8, v1 __asm__("v1") = 0;
            __asm__ volatile("jalr %2" : "+r"(v1), "+r"(a0) : "r"(GC(gc_exl)) : "ra", "v0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "at", "memory");
            check("EXL: the handler returned to EPC", v1, 1);
        }
        check("EXL: the general vector", GENERALS, 1);
        check("EXL: not the refill vector", REFILLS, 0);
        check("EXL: TLBL", CODE(), 2);
        check("EXL: EPC as it was", h_epc, GC(gc_exl_land));
        check("EXL: BadVAddr", h_badv, va + 8);
        check("EXL: EntryHi", h_ehi, va | 0x23u);
        /* fetches: a refill (the pc is BadVAddr and EPC), an invalid page, code across a page boundary */
        tlb_unmap_all(); MTC0(10, 0x23u);
        mkfn(0x00600000, 0x1111); mkfn(0x00601000, 0x2222);
        *K1(0x00600FF8) = 0x24020001u;                         /* addiu v0, zero, 1 */
        *K1(0x00600FFC) = 0x24420002u;                         /* addiu v0, v0, 2 (the last word of the even page) */
        *K1(0x00601000) = 0x24420004u; *K1(0x00601004) = 0x03E00008u; *K1(0x00601008) = 0;   /* ... the odd page's first word */
        REFILLS = 0; GENERALS = 0;
        act(A_REFILL, 0, 0x18000000u | 0x23u, LO(0x00600000, LV | LD), LO(0x00601000, LV | LD));
        n0 = h_n;
        check("fetch refill: the function ran", ((fn_t)0x18000000u)(), 0x1111);
        check("fetch refill: TLBL", CODE(), 2);
        check("fetch refill: the refill vector", REFILLS, 1);
        check("fetch refill: BadVAddr the pc", h_badv, 0x18000000u);
        check("fetch refill: EPC the pc", h_epc, 0x18000000u);
        check("fetch refill: Context", h_ctx, 0x80000000u | ((0x18000000u >> 13) << 4));
        check("fetch refill: EntryHi", h_ehi, 0x18000000u | 0x23u);
        check("fetch: across the boundary", ((fn_t)0x18000FF8u)(), 7);
        check("fetch: across the boundary again", ((fn_t)0x18000FF8u)(), 7);
        check("fetch: the odd page's function", ((fn_t)0x18001000u)() & 0xFF, 4);
        check("fetch: no other exception", h_n, n0 + 1);
        /* an invalid page of code: TLBL at the general vector, BadVAddr the target */
        tlb_unmap_all(); MTC0(10, 0x23u);
        tlb_wi(5, 0, 0x18000000u | 0x23u, LO(0x00600000, LD), LO(0x00601000, LV | LD));
        act(A_REPAIR, 0, 0x18000000u | 0x23u, LO(0x00600000, LV | LD), LO(0x00601000, LV | LD));
        GENERALS = 0;
        check("fetch, invalid: after the repair", ((fn_t)0x18000000u)(), 0x1111);
        check("fetch, invalid: the general vector", GENERALS, 1);
        check("fetch, invalid: TLBL", CODE(), 2);
        check("fetch, invalid: BadVAddr", h_badv, 0x18000000u);
        /* and a store to a mapped page from code ON a mapped page (the pc's page and the data page both mapped) */
        r0 = 0;
        (void)r0;
        tlb_unmap_all();
        MTC0(10, 0);
    }

    puts_("section 5\n");
    /* ---- 5: tlbp, tlbr, Index, Random, Wired */
    {
        tlb_wi(3, 0x6000, 0x14000000u | 9u, LO(0x00400000, LV | LD), LO(0x00404000, LV | LD | LG));         /* 16 KiB pages, not global */
        check("tlbp: 16 KiB, the ASID", tlb_probe(0x14000000u | 9u), 3);
        check("tlbp: 16 KiB, the second page of the pair", tlb_probe(0x14004000u | 9u), 3);
        check("tlbp: 16 KiB, VPN2 under the mask", tlb_probe(0x14006000u | 9u), 3);
        check("tlbp: another ASID, P", tlb_probe(0x14000000u | 10u), 0x80000000u);
        check("tlbp: past the pair, P", tlb_probe(0x14008000u | 9u), 0x80000000u);
        tlb_wi(4, 0, 0x1C000000u | 1u, LO(0x00400000, LV | LD | LG), LO(0x00401000, LV | LD | LG));
        check("tlbp: global, any ASID", tlb_probe(0x1C000000u | 200u), 4);
        MTC0(0, 0xFFFFFFFFu);
        check("mtc0 Index: six bits (P is tlbp's)", MFC0(0), 0x3Fu);
        MTC0(0, 0x3Fu);
        check("mtc0 Index: six bits", MFC0(0), 0x3Fu);
        /* tlbwi with Index above 31 writes entry Index & 31 */
        tlb_wi(33, 0, 0x1D000000u, LO(0x00400000, LV | LD | LG), LO(0x00401000, LV | LD | LG));
        check("tlbwi, Index 33: entry 1", tlb_probe(0x1D000000u), 1);
        /* tlbr: every mask, global or not, a clean and an invalid page */
        for (uint32_t k = 0; k < 7; k++) {
            uint32_t hi = (0x0A000000u + (k << 25)) | (k * 3u);
            tlb_wi(10 + k, pmask[k], hi, LOC(0x00400000, k & 1 ? 2 : 3, LV | LD), LOC(0x00401000 << 0, 3, LV));
            MTC0(2, 0); MTC0(3, 0); MTC0(5, 0); MTC0(10, 0); MTC0(0, 10 + k); TLBR();
            check("tlbr: PageMask", MFC0(5), pmask[k]);
            check("tlbr: EntryHi", MFC0(10), hi & ~(pmask[k] | 0x1F00u));
            check("tlbr: EntryLo0 (G: the AND)", MFC0(2), LOC(0x00400000, k & 1 ? 2 : 3, LV | LD));
            check("tlbr: EntryLo1", MFC0(3), LOC(0x00401000, 3, LV));
        }
        /* Random and Wired */
        MTC0(6, 12);
        check("Wired", MFC0(6), 12u);
        uint32_t lo = 31, hi = 0, bad = 0;
        for (int i = 0; i < 200; i++) { uint32_t r = MFC0(1); if (r < lo) lo = r; if (r > hi) hi = r; if (r > 31) bad++; }
        check("Random in Wired .. 31", lo >= 12u && hi <= 31u && bad == 0, 1);
        check("Random moves", hi > lo, 1);
        MTC0(6, 40);                                               /* Wired above 31: Random stays in 8 .. 31 (its low five bits are the bound) */
        check("Wired 40", MFC0(6), 40u);
        lo = 99; hi = 0;
        for (int i = 0; i < 3000; i++) { uint32_t r = MFC0(1); if (r < lo) lo = r; if (r > hi) hi = r; for (volatile int d = (i & 31) + (int)r; d > 0; d--) ; }
        check("Wired 40: Random in 8 .. 31", lo >= 8u && hi <= 31u, 1);
        MTC0(6, 12);
        for (int i = 0; i < 40; i++) {
            MTC0(5, 0); MTC0(10, 0x1A000000u + ((uint32_t)i << 13)); MTC0(2, LO(0x00400000, LV | LD | LG)); MTC0(3, LO(0x00401000, LV | LD | LG));
            TLBWR();
            uint32_t ix = tlb_probe(0x1A000000u + ((uint32_t)i << 13));
            if (ix < 12u || ix > 31u) { fails++; puts_("FAIL tlbwr outside Wired..31: "); puthex(ix); puts_("\n"); }
        }
        MTC0(6, 31);
        MTC0(5, 0); MTC0(10, 0x1B000000u); MTC0(2, LO(0x00400000, LV | LD | LG)); MTC0(3, LO(0x00401000, LV | LD | LG));
        TLBWR();
        check("Wired 31: tlbwr writes entry 31", tlb_probe(0x1B000000u), 31);
        MTC0(6, 0);
        check("Wired 0", MFC0(6), 0u);
        check("Random after a write of Wired", MFC0(1) <= 31u, 1);
        tlb_unmap_all();
    }

    puts_("section 6\n");
    /* ---- 6: caches */
    {
        uint32_t va = 0x14000000u;
        tlb_wi(0, 0, va, LO(0x00500000, LV | LD | LG), LO(0x00501000, LV | LD | LG));
        tlb_wi(1, 0, va + 0x100000, LO(0x00500000, LV | LD | LG), LO(0x00501000, LV | LD | LG));       /* the same frames, another address */
        *K1(0x00500100) = 0x11111111u;
        check("D-cache: KSEG0 sees the frame", *K0(0x00500100), 0x11111111u);     /* (the line is cached now) */
        *(volatile uint32_t *)(va + 0x100) = 0x22222222u;
        check("D-cache: after a store through the mapped page, KSEG0 sees it", *K0(0x00500100), 0x22222222u);
        *(volatile uint32_t *)(va + 0x100100 + 0x100 - 0x100) = 0x33333333u;
        check("D-cache: through the other alias, KSEG0 sees it", *K0(0x00500100), 0x33333333u);
        check("D-cache: the first alias sees it", *(volatile uint32_t *)(va + 0x100), 0x33333333u);
        *K0(0x00500104) = 0x44444444u;
        check("D-cache: a KSEG0 store is seen through the mapped page", *(volatile uint32_t *)(va + 0x104), 0x44444444u);
        /* code: written through KSEG1, run through a mapped address (old lines dropped by a cache op through a mapped address) */
        mkfn(0x00502000, 0x3001);
        tlb_wi(2, 0, va + 0x4000, LO(0x00502000, LV | LD | LG), LO(0x00503000, LV | LD | LG));
        check("code through a mapped address", ((fn_t)(va + 0x4000))(), 0x3001);
        check("code again", ((fn_t)(va + 0x4000))(), 0x3001);
        mkfn(0x00502000, 0x3002);
        icache_hit_inval(va + 0x4000);
        __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop");     /* (an index sweep over the colours takes a few cycles: a fetch right behind the cache op may still hit) */
        check("rewritten and invalidated through the mapped address", ((fn_t)(va + 0x4000))(), 0x3002);
        mkfn(0x00502000, 0x3003);
        icache_hit_inval(0x80502000u);
        __asm__ volatile("nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop; nop");
        check("rewritten and invalidated through KSEG0", ((fn_t)(va + 0x4000))(), 0x3003);
        /* uncached code (C = 2) */
        mkfn(0x00504000, 0x3004);
        tlb_wi(3, 0, va + 0x8000, LOC(0x00504000, 2, LV | LD | LG), LOC(0x00505000, 2, LV | LD | LG));
        check("uncached code", ((fn_t)(va + 0x8000))(), 0x3004);
        mkfn(0x00504000, 0x3005);
        check("uncached code, rewritten: no invalidation needed", ((fn_t)(va + 0x8000))(), 0x3005);
        /* ll / sc through a pair of aliases */
        {
            uint32_t r, ok;
            __asm__ volatile(".set push\n.set mips3\n"
                             "ll %0, 0(%2)\n"
                             "addiu %0, %0, 1\n"
                             "move %1, %0\n"
                             "sc %1, 0(%3)\n"
                             ".set pop" : "=&r"(r), "=&r"(ok) : "r"(va + 0x100), "r"(va + 0x100100) : "memory");
            check("ll / sc through two aliases of one frame", ok, 1);
            check("ll / sc: the value", *K1(0x00500100), r);
        }
        tlb_unmap_all();
    }

    puts_("section 7\n");
    /* ---- 7: 0x6000_0000 - 0x7FFF_FFFF */
    {
        *K1(0x00500200) = 0x70007000u;
        REFILLS = 0;
        act(A_REFILL, 0, 0x7F000000u, LO(0x00500000, LV | LD | LG), LO(0x00501000, LV | LD | LG));
        check("0x7F00_0000: a load", gload(0x7F000200u), 0x70007000u);
        check("0x7F00_0000: the refill vector", REFILLS, 1);
        check("0x7F00_0000: BadVAddr", h_badv, 0x7F000200u);
        mkfn(0x00506000, 0x7001);
        tlb_wi(2, 0x1E000, 0x70000000u, LO(0x00500000 & ~0xFFFFu, LV | LD | LG), LO(0x00510000, LV | LD | LG));
        mkfn(0x00506000, 0x7002);
        check("0x7000_0000, 64 KiB pages: code", ((fn_t)0x70006000u)(), 0x7002);
        tlb_unmap_all();
    }

    check("no fault of this program's own", h_own, 0);
    (void)mon; (void)fpg;
    done("tlbram_test");
    return 0;
}
