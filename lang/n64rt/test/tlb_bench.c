/* tlb_bench: a game for the runtime's TLB instructions (n64rt_os.c tlb_insn, and n64rt_exc.S's fast path for them) -- a program
 * linked for 0x8000_0400 that runs as a game under the loader (OWN) on the SoC in sim/mips_soc. What it does:
 *   1. checks the instructions' RESULTS: tlbwi of entries with known registers, tlbr back (PageMask, EntryHi without the bits under
 *      the mask, G in both EntryLo), tlbp finding each by VPN2 / ASID (a global one under another ASID too) and giving P when none,
 *      an Index of 32 or more writing nothing, tlbwr, an entry replaced, an entry moved to other frames (a load through it), an
 *      unmap-all after mapped ones. The tlbr reads and tlbp answers (not tlbwr's Random) are also folded into one checksum,
 *      which every runtime must agree on.
 *   2. times them, in the CPU's clocks, from a mark to a mark (sim/mips_soc CPUSTAT=1 prints the cycles between): 32 x ROUNDS tlbr
 *      (the refill handler's walk), tlbp that finds nothing (32 entries looked at), tlbp that finds the last entry, tlbwi that
 *      changes nothing (osUnmapTLBAll's), tlbwi that maps and unmaps a page, tlbwr. The marks are stores to 0x0490_0004, which a
 *      runtime built with TLBPROF=1 turns into the sim's marks (n64rt_io.c); the report is in the RDRAM at 0x8030_0000
 *      (DUMP=0x43B00000,64 in the sim): word 0 the magic 0x600D600D (the checks passed) or 0xBADBAD00 + the number of failed
 *      checks, word 1 the checksum, words 2-4 the first failed check (which, got, want), word 10 0xD0DE when it is done.
 *
 *   make -f tlb_bench.mk; the run is in tlb_bench.mk */
typedef unsigned int u32;
#define K1(pa) ((volatile u32 *)(0xA0000000u | (pa)))
#define MARK(v) (*K1(0x04900004u) = (u32)(v))
#define MFC0(r) ({ u32 v_; __asm__ volatile("mfc0 %0, $" #r : "=r"(v_)); v_; })
#define MTC0(r, v) __asm__ volatile("mtc0 %0, $" #r :: "r"((u32)(v)))
#define TLBWI() __asm__ volatile(".set push\n.set mips3\ntlbwi\n.set pop")
#define TLBWR() __asm__ volatile(".set push\n.set mips3\ntlbwr\n.set pop")
#define TLBR()  __asm__ volatile(".set push\n.set mips3\ntlbr\n.set pop")
#define TLBP()  __asm__ volatile(".set push\n.set mips3\ntlbp\n.set pop")
#define REPORT ((volatile u32 *)0x80300000u)
#ifndef ROUNDS
#define ROUNDS 40
#endif
#define NOTFOUND 0x80000000u

static u32 s_fails, s_sum;
static void check(u32 what, u32 got, u32 want)
{
    if (got == want) return;
    if (!s_fails) { REPORT[2] = what; REPORT[3] = got; REPORT[4] = want; }
    s_fails++;
}
static void fold(u32 v) { s_sum = (s_sum << 5 | s_sum >> 27) ^ v; }

static void write_entry(u32 i, u32 mask, u32 hi, u32 lo0, u32 lo1)
{
    MTC0(0, i); MTC0(5, mask); MTC0(10, hi); MTC0(2, lo0); MTC0(3, lo1);
    TLBWI();
}
static void unmap_all(void)
{
    for (u32 i = 0; i < 32; i++) write_entry(i, 0, 0x80000000u + i * 0x2000u, 0, 0);
}
/* EntryLo for a page at physical address pa: cached (C 3), V, D, and G or not */
#define LO(pa, g) ((((pa) >> 6)) | 0x1Au | (g))

void game_main(void)
{
    /* ---- the results ---- */
    unmap_all();
    /* a pair of 4 KiB pages at 0x2000_0000 (ASID 5, not global): V and D */
    write_entry(3, 0, 0x20000000u | 5u, LO(0x00100000u, 0), LO(0x00101000u, 0));
    /* a global pair of 64 KiB pages at 0x3000_0000 (G in both) */
    write_entry(17, 0x0001E000u, 0x30000000u | 9u, LO(0x00200000u, 1), LO(0x00210000u, 1));
    /* one that is global on its even page only: the AND is 0 */
    write_entry(30, 0, 0x40000000u | 7u, LO(0x00300000u, 1), LO(0x00301000u, 0));
    MTC0(0, 3); TLBR();
    check(1, MFC0(5), 0); check(2, MFC0(10), 0x20000005u); check(3, MFC0(2), LO(0x00100000u, 0)); check(4, MFC0(3), LO(0x00101000u, 0));
    MTC0(0, 17); TLBR();
    check(5, MFC0(5), 0x0001E000u); check(6, MFC0(10), 0x30000009u & ~0x1E000u); check(7, MFC0(2), LO(0x00200000u, 1)); check(8, MFC0(3), LO(0x00210000u, 1));
    MTC0(0, 30); TLBR();
    check(9, MFC0(2) & 1u, 0); check(10, MFC0(3) & 1u, 0);
    MTC0(0, 5); TLBR(); check(11, MFC0(10), 0x80000000u + 5u * 0x2000u); check(12, MFC0(2), 0);
    for (u32 i = 0; i < 32; i++) { MTC0(0, i); TLBR(); fold(MFC0(5)); fold(MFC0(10)); fold(MFC0(2)); fold(MFC0(3)); }
    /* tlbp: by VPN2 and ASID */
    MTC0(10, 0x20000005u); TLBP(); check(13, MFC0(0), 3);
    MTC0(10, 0x20001005u); TLBP(); check(14, MFC0(0), 3);          /* (the odd page of the pair: same VPN2) */
    MTC0(10, 0x20000006u); TLBP(); check(15, MFC0(0), NOTFOUND);   /* (another ASID, not global) */
    MTC0(10, 0x30000042u); TLBP(); check(16, MFC0(0), 17);         /* (global: any ASID) */
    MTC0(10, 0x3001F000u); TLBP(); check(17, MFC0(0), 17);         /* (the pair is 2 x 64 KiB: the odd page, VPN2 under the mask) */
    MTC0(10, 0x30020000u); TLBP(); check(18, MFC0(0), NOTFOUND);   /* (the next pair) */
    MTC0(10, 0x40000007u); TLBP(); check(19, MFC0(0), 30);
    MTC0(10, 0x40000008u); TLBP(); check(20, MFC0(0), NOTFOUND);   /* (its odd page is not global: the ASID matters) */
    MTC0(10, 0x50000000u); TLBP(); check(21, MFC0(0), NOTFOUND);
    for (u32 a = 0; a < 6; a++) { MTC0(10, 0x20000000u + (a << 12) + 5u); TLBP(); fold(MFC0(0)); }
    /* an Index of 32 or more writes nothing */
    write_entry(33, 0, 0x60000001u, 0x5Eu, 0x5Eu);
    MTC0(10, 0x60000001u); TLBP(); check(22, MFC0(0), NOTFOUND);
    /* tlbwr at Random (Wired 0): the entry is somewhere, tlbp finds it, tlbr gives it back */
    MTC0(5, 0); MTC0(10, 0x60000001u); MTC0(2, 0x5E); MTC0(3, 0x5E); TLBWR();
    MTC0(10, 0x60000001u); TLBP();
    { u32 ix = MFC0(0); check(23, ix >> 31, 0); check(24, ix < 32u, 1); MTC0(0, ix & 31u); TLBR(); check(25, MFC0(10), 0x60000001u); }
    /* an entry replaced by tlbwi, and unmap-all after mapped ones */
    write_entry(3, 0, 0x28000000u | 5u, LO(0x00100000u, 0), 0);
    MTC0(10, 0x20000005u); TLBP(); check(26, MFC0(0), NOTFOUND);
    MTC0(10, 0x28000005u); TLBP(); check(27, MFC0(0), 3);
    unmap_all();
    MTC0(10, 0x28000005u); TLBP(); check(28, MFC0(0), NOTFOUND);
    MTC0(0, 17); TLBR(); check(29, MFC0(10), 0x80000000u + 17u * 0x2000u); check(30, MFC0(5), 0);
    /* a TLB instruction in code on a mapped page (GoldenEye's libultra runs from 0x7000_0000): the runtime's fast path must not
     * read the instruction as it does KSEG0's (a load of it that missed the tables took the monitor's own exception for ever) */
    K1(0x00180000u)[0] = 0x42000008u;                       /* tlbp */
    K1(0x00180000u)[1] = 0x03E00008u;                       /* jr ra */
    K1(0x00180000u)[2] = 0;
    write_entry(9, 0, 0x70000000u, LO(0x00180000u, 1), LO(0x00181000u, 1));
    MTC0(10, 0x70000000u); MTC0(0, 0);
    ((void (*)(void))0x70000000u)();
    check(31, MFC0(0), 9);
    MTC0(10, 0x70000000u); MTC0(0, 0);
    ((void (*)(void))0x70000000u)();
    check(32, MFC0(0), 9);
    /* an entry moved to other frames over a mapped one (osMapTLB again, the runtime's fast path for it): a load through each
     * page reads the new frame -- what the CPU's tables had of the old ones is gone */
    K1(0x00190000u)[0] = 0x11111111u; K1(0x00191000u)[0] = 0x22222222u;
    K1(0x00192000u)[0] = 0x33333333u; K1(0x00193000u)[0] = 0x44444444u;
    write_entry(12, 0, 0x02000000u, LO(0x00190000u, 1), LO(0x00191000u, 1));
    check(33, *(volatile u32 *)0x02000000u, 0x11111111u); check(34, *(volatile u32 *)0x02001000u, 0x22222222u);
    write_entry(12, 0, 0x02000000u, LO(0x00192000u, 1), LO(0x00193000u, 1));
    check(35, *(volatile u32 *)0x02000000u, 0x33333333u); check(36, *(volatile u32 *)0x02001000u, 0x44444444u);
    MTC0(0, 12); TLBR(); check(37, MFC0(2), LO(0x00192000u, 1)); check(38, MFC0(3), LO(0x00193000u, 1)); check(39, MFC0(10), 0x02000000u);
    unmap_all();
    REPORT[1] = s_sum;
    REPORT[0] = s_fails ? 0xBADBAD00u + s_fails : 0x600D600Du;

    /* ---- the times ---- */
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS; r++)
        for (u32 i = 0; i < 32; i++) { MTC0(0, i); TLBR(); }
    MARK(0xA6000000u);
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS * 32; r++) { MTC0(10, 0x50000000u); TLBP(); }
    MARK(0xA6000000u);
    write_entry(31, 0, 0x20000005u, LO(0x00100000u, 0), LO(0x00101000u, 0));   /* (the last entry mapped: tlbp finds it at 31) */
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS * 32; r++) { MTC0(10, 0x20000005u); TLBP(); }
    MARK(0xA6000000u);
    REPORT[9] = MFC0(0);
    unmap_all();
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS; r++) unmap_all();
    MARK(0xA6000000u);
    write_entry(20, 0, 0x2C000000u | 5u, LO(0x00100000u, 0), 0);  /* (one more mapped: the runtime logs the first entry mapped, and the loop maps one over and over) */
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS * 4; r++) {                        /* (maps a page, then unmaps it: two writes that change the tables) */
        write_entry(8, 0, 0x28000000u | 5u, LO(0x00100000u, 0), 0);
        write_entry(8, 0, 0x80000000u + 8u * 0x2000u, 0, 0);
    }
    MARK(0xA6000000u);
    MARK(0xA5000000u);
    for (u32 r = 0; r < ROUNDS * 4; r++) { MTC0(5, 0); MTC0(10, 0x60000000u + (r << 13)); MTC0(2, 0x5E); MTC0(3, 0); TLBWR(); }
    MARK(0xA6000000u);
    REPORT[10] = 0xD0DEu;                                          /* (done) */
    for (;;) ;
}

__attribute__((section(".text.start"))) void _start(void)
{
    __asm__ volatile(".set noreorder\n\tj game_main\n\tnop");
}
