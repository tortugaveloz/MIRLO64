/* A TLB exception delivered to the game (mips_core's real TLB with MON; sim/mips_soc): a page that no entry maps -- or one
 * that is invalid, or a store to one that is clean -- is the exception the VR4300 takes, in the CPU, and the game's own
 * handler runs, as a game's does for the pages it maps on demand (0x7F000000): BadVAddr, EntryHi (VPN2 and ASID),
 * Context's BadVPN2, Cause.ExcCode (TLBL 2, TLBS 3, Mod 1) and BD, EPC (the branch's, in a delay slot; not touched when EXL
 * was set), Status.EXL, then the vector -- 0x8000_0000 for a refill with EXL clear, 0x8000_0180 for the rest. The handler
 * does mfc0 / mtc0 / tlbwr / eret and the access runs again.
 *
 * Nothing is the runtime's: the "game" is the code of this program that is in the RDRAM's KSEG0 window (tlbtest.h's gc_*,
 * and the handlers below, copied to 0x8000_0000 and 0x8000_0180) and the functions it calls on mapped pages; the program's
 * own code (the SDRAM) is not, and its exceptions would be its trap_handler's. The refill handler reads its page pairs
 * from a table by Context (PTEBase 0x8000_0000: 16 bytes a pair, EntryLo0, EntryLo1), and a general one logs what it is
 * given and, in mode 0, repairs the entry (tlbp, tlbwi) from two words the test gives it.
 *
 * Checked: refill of a load, of a store and of a fetch, with every register the handler sees; the odd page of a pair the
 * handler wrote (no second exception); a refill in a delay slot (EPC the branch, BD); the ASID (an entry of another one
 * does not map the page: a refill); an invalid page (the general vector, TLBL); a store to a clean page (Mod); a refill with
 * EXL set (the general vector, EPC and BD left alone); a pair replaced by the handler's tlbwr (no stale page). */
#include "tlbtest.h"

static volatile uint32_t t_own;
void trap_handler(void) { t_own++; MTC0(14, MFC0(14) + 4u); }      /* (the program's own: none is expected; the access is skipped) */

/* ---- the game's handlers (copied to the RDRAM at 0x8000_0000 and 0x8000_0180) --------------- */
/* the log, at 0x8001_0000: +0 refills, +4.. Cause, BadVAddr, EPC, Context, EntryHi, Status of the last;
 * +0x40 general exceptions, +0x44.. the same; +0x5C mode (0: repair, 1: just return), +0x60 / +0x64 the repair's EntryLo0 / 1 */
extern char h_refill[], h_refill_end[], h_gen[], h_gen_end[];
__asm__(
"    .set noreorder\n"
"    .set noat\n"
"    .set mips3\n"
"    .text\n"
"    .globl h_refill\n"
"h_refill:\n"
"    mfc0  $k0, $4\n"
"    lw    $k1, 0($k0)\n"
"    lw    $k0, 4($k0)\n"
"    mtc0  $k1, $2\n"
"    mtc0  $k0, $3\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0($k0)\n"
"    mfc0  $k1, $13\n    sw    $k1, 4($k0)\n"
"    mfc0  $k1, $8\n     sw    $k1, 8($k0)\n"
"    mfc0  $k1, $14\n    sw    $k1, 12($k0)\n"
"    mfc0  $k1, $4\n     sw    $k1, 16($k0)\n"
"    mfc0  $k1, $10\n    sw    $k1, 20($k0)\n"
"    mfc0  $k1, $12\n    sw    $k1, 24($k0)\n"
"    nop\n"
"    nop\n"
"    tlbwr\n"
"    nop\n"
"    nop\n"
"    eret\n"
"    nop\n"
"    .globl h_refill_end\n"
"h_refill_end:\n"
"    .globl h_gen\n"
"h_gen:\n"
"    lui   $k0, 0x8001\n"
"    lw    $k1, 0x40($k0)\n"
"    addiu $k1, $k1, 1\n"
"    sw    $k1, 0x40($k0)\n"
"    mfc0  $k1, $13\n    sw    $k1, 0x44($k0)\n"
"    mfc0  $k1, $8\n     sw    $k1, 0x48($k0)\n"
"    mfc0  $k1, $14\n    sw    $k1, 0x4C($k0)\n"
"    mfc0  $k1, $4\n     sw    $k1, 0x50($k0)\n"
"    mfc0  $k1, $10\n    sw    $k1, 0x54($k0)\n"
"    mfc0  $k1, $12\n    sw    $k1, 0x58($k0)\n"
"    lw    $k1, 0x5C($k0)\n"
"    bnez  $k1, 9f\n"
"    nop\n"
"    lw    $k1, 0x60($k0)\n"
"    mtc0  $k1, $2\n"
"    lw    $k1, 0x64($k0)\n"
"    mtc0  $k1, $3\n"
"    nop\n"
"    nop\n"
"    tlbp\n"
"    nop\n"
"    nop\n"
"    tlbwi\n"
"    nop\n"
"    nop\n"
"9:  eret\n"
"    nop\n"
"    .globl h_gen_end\n"
"h_gen_end:\n"
);

/* ---- the game ------------------------------------------------------------------------- */
#define LOG(o) (*K0(0x10000 + (o)))
/* the game's page table: the pair of VA's EntryLo words, where Context (PTEBase 0x8000_0000) points */
static void pte(uint32_t va, uint32_t lo0, uint32_t lo1)
{
    volatile uint32_t *e = K0((va >> 13) << 4);
    e[0] = lo0; e[1] = lo1;
}
static void clear_log(void) { for (uint32_t o = 0; o < 0x80; o += 4) LOG(o) = 0; LOG(0x60) = 0; }

int main(void)
{
    puts_("tlbgame_real_test\n");
    if (!has_real_tlb() || !(MFC0(23) & 1u)) { puts_("tlbgame_real_test: SKIPPED, no real TLB with the monitor in this CPU\n"); putc_(4); for (;;); }
    tlb_unmap_all();
    /* data pages: physical 0x100000 + n * 0x1000 hold n in every word's top, and their offset */
    for (uint32_t n = 0; n < 32; n++) for (uint32_t w = 0; w < 1024; w += 64) K1(0x100000 + n * 0x1000)[w] = (n << 24) | w;
    /* code in a page: addiu v0, zero, 0x1234 / jr ra / nop */
    K1(0x120000)[0] = 0x24021234u; K1(0x120000)[1] = 0x03E00008u; K1(0x120000)[2] = 0;
    copy_k1(0x0, h_refill, h_refill_end);
    copy_k1(0x180, h_gen, h_gen_end);
    install_game_code();
    clear_log();
    MTC0(4, 0x80000000u);                         /* Context: PTEBase */
    MTC0(7, 2);                                   /* MCTL.GAME: a TLB exception of the game's code is the game's */
    volatile uint32_t *m = (volatile uint32_t *)0x04000000u;
    uint32_t pre;

    /* 1. a load from a page no entry maps: the refill vector, every register as the VR4300 leaves it */
    pte(0x04000000u, LO(0x100000, LV | LD | LG), LO(0x101000, LV | LD | LG));
    {
        uint32_t v1 = gload(0x04000000u);
        check("refill: the load's value", v1, 0x00000000u);
        check("refill: handler ran once", LOG(0), 1);
        check("refill: Cause.ExcCode TLBL", (LOG(4) >> 2) & 31u, 2);
        check("refill: Cause.BD clear", LOG(4) >> 31, 0);
        check("refill: BadVAddr", LOG(8), 0x04000000u);
        check("refill: EPC the load", LOG(12), GC(gc_load));
        check("refill: Context BadVPN2", LOG(16), 0x80000000u | (0x2000u << 4));
        check("refill: EntryHi VPN2", LOG(20) & 0xFFFFE000u, 0x04000000u);
        check("refill: Status.EXL, not ERL", LOG(24) & 6u, 2);
        check("refill: not a general exception", LOG(0x40), 0);
        check("after the eret: EXL clear", MFC0(12) & 6u, 0);
    }
    check("refill: even page, second word", m[64], 0x00000040u);
    check("refill: odd page", m[0x400], 0x01000000u);
    check("odd page: no second exception", LOG(0), 1);

    /* 2. a store to a new pair: TLBS */
    pte(0x04010000u, LO(0x102000, LV | LD | LG), LO(0x103000, LV | LD | LG));
    gstore(0x04010004u, 0xC0FFEE01u);
    check("store refill: handler ran", LOG(0), 2);
    check("store refill: Cause.ExcCode TLBS", (LOG(4) >> 2) & 31u, 3);
    check("store refill: BadVAddr", LOG(8), 0x04010004u);
    check("store refill: the store landed", *K1(0x102004), 0xC0FFEE01u);

    /* 3. a refill in a branch's delay slot: EPC the branch, BD, and the branch is taken after the slot ran again */
    pte(0x04020000u, LO(0x104000, LV | LD | LG), LO(0x105000, LV | LD | LG));
    {
        uint32_t v0, x = gslot(0x04020000u, &v0);
        check("delay slot: the branch taken", x, 1);
        check("delay slot: the value", v0, 0x04000000u);
        check("delay slot: handler ran", LOG(0), 3);
        check("delay slot: Cause.BD", LOG(4) >> 31, 1);
        check("delay slot: EPC the branch", LOG(12), GC(gc_slot) + 8);
        check("delay slot: BadVAddr", LOG(8), 0x04020000u);
    }

    /* 4. a fetch: a function in a page no entry maps */
    pte(0x04100000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
    {
        uint32_t r = ((uint32_t (*)(void))0x04100000u)();
        check("fetch refill: the function ran", r, 0x1234);
        check("fetch refill: handler ran", LOG(0), 4);
        check("fetch refill: Cause.ExcCode TLBL", (LOG(4) >> 2) & 31u, 2);
        check("fetch refill: BadVAddr the pc", LOG(8), 0x04100000u);
        check("fetch refill: EPC the pc", LOG(12), 0x04100000u);
        check("fetch refill: Context BadVPN2", LOG(16), 0x80000000u | (0x2080u << 4));
    }

    /* 5. ASID: a non-global entry of ASID 7 maps the page for ASID 7; ASID 8 misses and refills (a second entry) */
    pte(0x04200000u, LO(0x108000, LV | LD), LO(0x109000, LV | LD));
    MTC0(10, 7);
    check("asid 7: refill, the page", gload(0x04200000u), 0x08000000u);
    pre = LOG(0);
    check("asid 7: EntryHi ASID in the handler", LOG(20) & 0xFF, 7);
    check("asid 7: again, no exception", gload(0x04200000u), 0x08000000u);
    check("asid 7: no new refill", LOG(0), pre);
    MTC0(10, 8);
    check("asid 8: the page, after a refill", gload(0x04200000u), 0x08000000u);
    check("asid 8: a refill", LOG(0), pre + 1);
    check("asid 8: EntryHi ASID in the handler", LOG(20) & 0xFF, 8);
    MTC0(10, 0);

    /* 6. an invalid page: the entry names it, V clear -- the general vector, TLBL; the handler repairs it (tlbp, tlbwi) */
    pte(0x04300000u, LO(0x10A000, LD | LG), LO(0x10B000, LV | LD | LG));      /* the even page invalid */
    LOG(0x60) = LO(0x10A000, LV | LD | LG); LOG(0x64) = LO(0x10B000, LV | LD | LG);
    pre = LOG(0);
    check("invalid: after the repair the page", gload(0x04300000u), 0x0A000000u);
    check("invalid: a refill first", LOG(0), pre + 1);
    check("invalid: then the general vector", LOG(0x40), 1);
    check("invalid: Cause.ExcCode TLBL", (LOG(0x44) >> 2) & 31u, 2);
    check("invalid: BadVAddr", LOG(0x48), 0x04300000u);
    check("invalid: the odd page was valid", gload(0x04301000u), 0x0B000000u);

    /* 7. Mod: a store to a clean page (D clear); the load first is fine */
    pte(0x04400000u, LO(0x10C000, LV | LG), LO(0x10D000, LV | LG));
    LOG(0x60) = LO(0x10C000, LV | LD | LG); LOG(0x64) = LO(0x10D000, LV | LD | LG);
    check("clean page: a load", gload(0x04400000u), 0x0C000000u);
    uint32_t g0 = LOG(0x40);
    gstore(0x04400008u, 0xFEED0002u);
    check("clean page: Mod, the general vector", LOG(0x40), g0 + 1);
    check("clean page: Cause.ExcCode Mod", (LOG(0x44) >> 2) & 31u, 1);
    check("clean page: BadVAddr", LOG(0x48), 0x04400008u);
    check("clean page: the store landed after the repair", *K1(0x10C008), 0xFEED0002u);
    gstore(0x0440000Cu, 0xFEED0003u);
    check("clean page: no second Mod", LOG(0x40), g0 + 1);

    /* 8. a refill with EXL set: the general vector, EPC and BD as they were (mode 1: the handler returns to EPC) */
    {
        uint32_t x = 0;
        pte(0x04500000u, LO(0x10E000, LV | LD | LG), LO(0x10F000, LV | LD | LG));
        LOG(0x5C) = 1;
        g0 = LOG(0x40); pre = LOG(0);
        MTC0(14, GC(gc_exl_land));
        MTC0(13, 0x00000000u);
        {
            register uint32_t a0 __asm__("a0") = 0x04500000u, v1 __asm__("v1") = 0;
            __asm__ volatile("jalr %2" : "+r"(v1), "+r"(a0) : "r"(GC(gc_exl)) : "ra", "v0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "t8", "t9", "at", "memory");
            x = v1;
        }
        check("EXL: went to EPC", x, 1);
        check("EXL: the general vector", LOG(0x40), g0 + 1);
        check("EXL: no refill handler", LOG(0), pre);
        check("EXL: Cause.ExcCode TLBL", (LOG(0x44) >> 2) & 31u, 2);
        check("EXL: EPC as it was", LOG(0x4C), GC(gc_exl_land));
        check("EXL: BadVAddr", LOG(0x48), 0x04500000u);
        LOG(0x5C) = 0;
    }

    /* 9. a pair replaced by the handler's tlbwr: the old pages are gone (Wired 31: tlbwr writes entry 31) */
    MTC0(6, 31);
    pte(0x04600000u, LO(0x110000, LV | LD | LG), LO(0x111000, LV | LD | LG));
    check("tlbwr 1", gload(0x04600000u), 0x10000000u);
    check("tlbwr 1 odd", gload(0x04601000u), 0x11000000u);
    pte(0x04700000u, LO(0x112000, LV | LD | LG), LO(0x113000, LV | LD | LG));
    check("tlbwr 2", gload(0x04700000u), 0x12000000u);
    pre = LOG(0);
    check("tlbwr: the first pair's pages are gone, it refills", gload(0x04600000u), 0x10000000u);
    check("tlbwr: refilled", LOG(0), pre + 1);
    MTC0(6, 0);

    /* 10. 0x6000_0000 - 0x7FFF_FFFF are mapped too (the core with MAP_HI: a game maps and runs code from 0x7000_0000 and takes
     * 0x7F000000 from its refill handler): a load and a fetch, refill and all */
    pte(0x70000000u, LO(0x11C000, LV | LD | LG), LO(0x11D000, LV | LD | LG));
    pre = LOG(0);
    check("0x7000_0000: a load", gload(0x70000000u), 0x1C000000u);
    check("0x7000_0000: after a refill", LOG(0), pre + 1);
    check("0x7000_0000: BadVAddr", LOG(8), 0x70000000u);
    check("0x7000_0000: the odd page", gload(0x70001000u), 0x1D000000u);
    pte(0x7F000000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
    {
        uint32_t r = ((uint32_t (*)(void))0x7F000000u)();
        check("0x7F00_0000: a function fetched there ran", r, 0x1234);
        check("0x7F00_0000: the refill's BadVAddr", LOG(8), 0x7F000000u);
    }

    /* 11. code from cacheable mapped pages (the fetch micro-TLB), with the game's own refill handler: a function run
     * twice, then its pair replaced by a tlbwi (the new frame's code, none of the old), an ASID switch (the other ASID's entry
     * or a refill, never the first one's page), an entry made invalid and repaired by the general handler, and the speed */
    {
        typedef uint32_t (*fn_t)(void);
        const uint32_t addiu = 0x24020000u;
        K1(0x122000)[0] = addiu | 0x2222; K1(0x122000)[1] = 0x03E00008u; K1(0x122000)[2] = 0;
        K1(0x123000)[0] = addiu | 0x3333; K1(0x123000)[1] = 0x03E00008u; K1(0x123000)[2] = 0;
        pte(0x04800000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        pre = LOG(0);
        check("code 1", ((fn_t)0x04800000u)(), 0x1234);
        check("code 1: a refill", LOG(0), pre + 1);
        check("code 1 again", ((fn_t)0x04800000u)(), 0x1234);
        check("code 1 again: no refill", LOG(0), pre + 1);
        /* the game replaces the entry by a tlbwi (tlbp finds its index) */
        uint32_t ix = tlb_probe(0x04800000u);
        check("code: the entry is there", ix >> 31, 0);
        tlb_wi(ix, 0, 0x04800000u, LO(0x122000, LV | LD | LG), LO(0x123000, LV | LD | LG));
        check("code, replaced by a tlbwi", ((fn_t)0x04800000u)(), 0x2222);
        tlb_wi(ix, 0, 0x04800000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        check("code, replaced back", ((fn_t)0x04800000u)(), 0x1234);
        check("code, replaced: no game exception", LOG(0), pre + 1);
        /* ASIDs: two non-global entries for one page pair */
        tlb_wi(21, 0, 0x04900000u | 3u, LO(0x120000, LV | LD), LO(0x121000, LV | LD));
        tlb_wi(22, 0, 0x04900000u | 4u, LO(0x122000, LV | LD), LO(0x123000, LV | LD));
        MTC0(10, 3); check("ASID 3's code", ((fn_t)0x04900000u)(), 0x1234);
        MTC0(10, 4); check("ASID 4's code, not ASID 3's", ((fn_t)0x04900000u)(), 0x2222);
        MTC0(10, 3); check("ASID 3's code again", ((fn_t)0x04900000u)(), 0x1234);
        MTC0(10, 0);
        /* an entry made invalid and repaired (by the general handler, tlbp / tlbwi): the repaired frame's code */
        pte(0x04A00000u, LO(0x120000, LV | LD | LG), LO(0x121000, LV | LD | LG));
        check("code, valid", ((fn_t)0x04A00000u)(), 0x1234);
        ix = tlb_probe(0x04A00000u);
        tlb_wi(ix, 0, 0x04A00000u, LO(0x120000, LD | LG), LO(0x121000, LV | LD | LG));
        LOG(0x60) = LO(0x122000, LV | LD | LG); LOG(0x64) = LO(0x123000, LV | LD | LG);
        g0 = LOG(0x40);
        check("code, made invalid and repaired", ((fn_t)0x04A00000u)(), 0x2222);
        check("code, invalid: the general vector", LOG(0x40), g0 + 1);
        check("code, invalid: TLBL", (LOG(0x44) >> 2) & 31u, 2);
        /* the speed: a loop from a mapped page against KSEG0's */
        volatile uint32_t *l = K1(0x124000);
        l[0] = 0x2484FFFFu; l[1] = 0x1480FFFEu; l[2] = 0; l[3] = 0x03E00008u; l[4] = 0x00801021u;
        volatile uint32_t *l2 = K1(0x126000);
        l2[0] = 0x2484FFFFu; l2[1] = 0x1480FFFEu; l2[2] = 0; l2[3] = 0x03E00008u; l2[4] = 0x00801021u;
        pte(0x04B00000u, LO(0x126000, LV | LD | LG), LO(0x127000, LV | LD | LG));
        typedef uint32_t (*fn1_t)(uint32_t);
        ((fn1_t)0x80124000u)(100); ((fn1_t)0x04B00000u)(100);
        uint32_t t = MFC0(9); ((fn1_t)0x80124000u)(3000); uint32_t t_k0 = MFC0(9) - t;
        t = MFC0(9); ((fn1_t)0x04B00000u)(3000); uint32_t t_map = MFC0(9) - t;
        puts_("loop of 3000: KSEG0 "); puthex(t_k0); puts_(", mapped "); puthex(t_map); puts_("\n");
        if (MFC0(23) & 4u) check("code speed: the mapped loop within 1.3x of KSEG0's", t_map * 10 <= t_k0 * 13, 1);       /* (with the page registers) */
    }

    check("no exception of the program's own", t_own, 0);
    puts_("refills "); puthex(LOG(0)); puts_(", general "); puthex(LOG(0x40)); puts_("\n");
    done("tlbgame_real_test");
    return 0;
}
